// Стенд VIDEO.ELF на ПК (test_video_host.py): приложение (app.c, audio.c, avi_info.c, hud.cpp) и
// настоящий MP3.ELF (Helix) — те же исходники, что собираются в ELF; ядро ESP — заглушка zm_core_t
// в реальном времени (задачи — потоки), Tang с ядром видео — модель, плагин FTVIEW4 — сценарий.
//
// МОДЕЛЬ ЯДРА (как RTL): разбор идёт по чанкам movi; кадр декодируется DECODE_MS и ждёт слота, у
// записи три буфера (показ + 2) — готовых кадров не больше двух; темп — vp_pace (запас +20 мс на
// выходной кадр 50 Гц, слот — когда запас ≥ периода, первый слот после сброса — сразу и виден со
// следующего кадра, запас не больше двух периодов, пауза — бит 2 REG_CPURESET); звук — байты
// чанков 01wb по 4 в FIFO 1024 слова, пока приём включён (иначе пропуск), FIFO полон или эпоха не
// подтверждена — разбор ждёт; сброс потока — эпоха + 1, указатель записи 0. Регистры LINK — как в
// ts_fb_core (HAS_VIDEO), оверлеи — регистры и регион холстов fb_ovl.
// МОДЕЛЬ ЦАП: очередь 1486 мс (как PSRAM платы), сливается в реальном времени, пауза держит;
// начало каждого «пика» (звук после ≥ 20 мс тишины) записывается временем, когда он зазвучал.
// СЦЕНАРИЙ: START, HDR, поток с начала, STATUS раз в 80 мс; панель (первые 3 с), окно списка,
// пауза 1,5 с, перемотка (сообщение «Building seek index...», SEEKING, сброс потока с кадра), до
// конца ролика, EXIT. Снимки вывода 1280×720 (картинка — серый градиент в окне картинки, поверх —
// оверлеи по формуле fb_ovl) — PPM в каталог --shots.
// Итог — строки ключ=значение (test_video_host.py).
//
//   video_host файл.avi [--seek-at МС --seek-frame N] [--pause-at МС --pause-ms МС] [--decode-ms МС]
//                       [--cfg CTRL] [--shots КАТАЛОГ] [--no-mp3 1]
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "video.h"
#include "zm_source.h"

extern "C" const zm_app_t* video_app_table(void);
extern "C" int mp3_main(int argc, char** argv);

namespace {

using Clock = std::chrono::steady_clock;
const Clock::time_point t0 = Clock::now();
double now_ms() { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }
void sleep_ms(double ms) { std::this_thread::sleep_for(std::chrono::microseconds((long long)(ms * 1000))); }

std::mutex log_lock;
void say(const char* fmt, ...)
{
  std::lock_guard<std::mutex> g(log_lock);
  std::fprintf(stderr, "[%8.1f] ", now_ms());
  va_list a;
  va_start(a, fmt);
  std::vfprintf(stderr, fmt, a);
  va_end(a);
  std::fputc('\n', stderr);
}

// --- модель ядра видео ------------------------------------------------------------------------------
struct Chunk { bool audio; std::vector<uint8_t> data; };

struct Tang {
  std::mutex m;
  bool alive = false;
  double decode_ms = 12;
  // поток
  std::vector<Chunk> chunks;
  size_t pos = 0, au_off = 0;
  bool streaming = false;
  double dec_end = -1;            // декодируется кадр: когда готов
  int waiting = 0;                // готовых кадров ждут слота
  uint32_t per_us = 40000;
  // темп
  bool first = true, skip = false, pause = false;
  int64_t acc = 0;
  uint32_t slots = 0, base = 0;
  int64_t vt_next = 20;           // следующий выходной кадр, мс
  std::map<uint32_t, double> shown;     // кадр ролика → с какого выходного кадра виден (мс)
  uint32_t dup_frames = 0;              // кадр показан дважды (в одной эпохе)
  // звук
  uint32_t au_mem[1024] = {};
  uint32_t au_wp = 0, au_rp = 0, ctl = 0, aclk = 0, acc_w = 0;
  int acc_n = 0;
  uint8_t epoch = 0;
  uint64_t au_bytes_in = 0, au_bytes_skipped = 0;
  // оверлеи и настройки
  uint32_t ovl[2][4] = {};
  std::vector<uint32_t> canvas = std::vector<uint32_t>(131072, 0);
  uint32_t cfg = 0;
  uint64_t link_ops = 0;

  void reset_stream(std::vector<Chunk> c, uint32_t base_frame)
  {
    std::lock_guard<std::mutex> g(m);
    chunks = std::move(c);
    pos = au_off = 0;
    streaming = true;
    dec_end = -1;
    waiting = 0;
    first = true; skip = false; acc = 0; slots = 0;
    pause = false;                // запись REG_CPURESET снимает и паузу (бит 2 = 0)
    base = base_frame;
    epoch++;
    au_wp = 0; acc_n = 0; acc_w = 0;
  }

  void slot(double t, bool at_vtick)
  {
    waiting--;
    slots++;
    // слот по выходному кадру виден с него же, первый после сброса — со следующего
    shown[base + slots - 1] = at_vtick ? t : (double)vt_next;
  }

  // продвинуть модель до времени t (мс)
  void run(double t)
  {
    std::lock_guard<std::mutex> g(m);
    if (!alive) return;
    const int64_t per = per_us / 1000;
    // выходные кадры
    while (vt_next <= t) {
      const double tv = (double)vt_next;
      if (!pause) {
        if (!skip) acc += 20;
        skip = false;
        if (waiting && !first && acc >= per) { acc -= per; slot(tv, true); }
        else if (!first && acc > 2 * per) acc = 2 * per;
      }
      vt_next += 20;
      parse(tv);
    }
    parse(t);
  }

  void parse(double t)
  {
    for (;;) {
      if (!pause && waiting && first) { first = false; skip = true; acc = 0; slot(t, false); }
      if (!pause && waiting && !first && acc >= (int64_t)(per_us / 1000)) { acc -= per_us / 1000; slot(t, false); }
      if (!streaming || pos >= chunks.size()) return;
      Chunk& c = chunks[pos];
      if (c.audio) {
        if (!(ctl & 1)) { au_bytes_skipped += c.data.size() - au_off; pos++; au_off = 0; continue; }
        if (((ctl >> 8) & 255) != epoch) return;            // эпоха не подтверждена
        while (au_off < c.data.size()) {
          if (acc_n == 3 && ((au_wp - au_rp) & 2047) >= 1024) return;   // FIFO полон
          acc_w |= (uint32_t)c.data[au_off++] << (8 * acc_n);
          au_bytes_in++;
          if (++acc_n == 4) { au_mem[au_wp & 1023] = acc_w; au_wp = (au_wp + 1) & 2047; acc_w = 0; acc_n = 0; }
        }
        pos++; au_off = 0;
        continue;
      }
      if (dec_end < 0) {
        if (waiting >= 2) return;
        dec_end = t + decode_ms;
      }
      if (t < dec_end) return;
      dec_end = -1;
      waiting++;
      pos++;
    }
  }

  uint32_t reg(uint32_t a)
  {
    if (a >= W_CANVAS && a < W_CANVAS + 4 * 131072) return canvas[(a - W_CANVAS) / 4];
    if (a >= W_AUDIO && a < W_AUDIO + 4096) return au_mem[(a - W_AUDIO) / 4];
    if (a >= 0xC0 && a < 0xE0) return ovl[(a - 0xC0) / 16][(a & 15) / 4];
    switch (a) {
    case R_VERSION: return 0x05010001;
    case R_SLOTS: return slots;
    case R_ABORTS: return 512u << 16;
    case R_CFG: return cfg;
    case R_AU_WP: return (uint32_t)epoch << 24 | (pause ? 1u << 23 : 0u) | au_wp;
    case R_AU_CTL: return ctl;
    case R_AU_CLK: return aclk;
    default: return 0;
    }
  }
  void set(uint32_t a, uint32_t v)
  {
    if (a >= W_CANVAS && a < W_CANVAS + 4 * 131072) { canvas[(a - W_CANVAS) / 4] = v; return; }
    if (a >= 0xC0 && a < 0xE0) { ovl[(a - 0xC0) / 16][(a & 15) / 4] = v; return; }
    switch (a) {
    case R_AU_WP: au_rp = v & 2047; break;
    case R_AU_CTL: ctl = v; break;
    case R_AU_CLK: aclk = v; break;
    default: break;
    }
  }
} tang;

std::atomic<bool> model_run{true};
void model_thread()
{
  while (model_run) {
    tang.run(now_ms());
    sleep_ms(0.5);
  }
}

// --- модель ЦАП ---------------------------------------------------------------------------------------
struct Sink {
  std::mutex m;
  bool open = false, paused = false;
  uint32_t rate = 0, ch = 0;
  std::deque<float> q;
  double last = 0, frac = 0;
  uint64_t played = 0, writes = 0;
  int quiet = 1 << 30;             // кадров тишины подряд
  std::vector<double> onsets;      // когда зазвучал пик (мс)
  std::vector<double> pause_ticks; // слив во время паузы (не должно быть)
  uint32_t opens = 0, flushes = 0;
  std::vector<double> underruns;   // очередь опустела посреди звука (мс; после pcm_flush — не в счёт)
  bool had_audio = false, starved = false;
  double first_play = -1;          // когда зазвучал первый кадр PCM (мс)
  void drain(double t)
  {
    if (!open || paused || !rate) { last = t; frac = 0; return; }
    double n = (t - last) * rate / 1000.0 + frac;
    const uint64_t k = (uint64_t)n;
    frac = n - (double)k;
    const uint64_t have = q.size() / ch;
    const uint64_t take = k < have ? k : have;
    for (uint64_t i = 0; i < take; i++) {
      float peak = 0;
      for (uint32_t c = 0; c < ch; c++) { peak = std::max(peak, std::fabs(q.front())); q.pop_front(); }
      if (peak > 0.05f) {
        if (quiet >= (int)(rate / 50)) onsets.push_back(last + (double)i * 1000.0 / rate);
        quiet = 0;
      } else if (peak < 0.01f) {
        quiet++;
      }
      played++;
    }
    if (take < k && had_audio && !starved) { underruns.push_back(t); starved = true; }
    if (take && first_play < 0) first_play = last;
    if (take) { had_audio = true; starved = false; }
    last = t;
  }
} sink;

int pcm_open(const zm_pcm_format_t* f)
{
  std::lock_guard<std::mutex> g(sink.m);
  sink.rate = f->rate; sink.ch = f->channels; sink.open = true; sink.paused = false;
  sink.q.clear(); sink.last = now_ms(); sink.frac = 0; sink.opens++; sink.had_audio = false;
  say("pcm_open %u Hz, %u ch", f->rate, f->channels);
  return ZM_OK;
}
int pcm_write(const float* frames, uint32_t count)
{
  uint32_t done = 0;
  double waited = 0;
  while (done < count) {
    std::unique_lock<std::mutex> g(sink.m);
    if (!sink.open) return 0;
    sink.drain(now_ms());
    const uint32_t cap = sink.rate * 1486 / 1000;
    const uint32_t room = cap - (uint32_t)(sink.q.size() / sink.ch);
    if (!room) {
      g.unlock();
      if (waited > 500) break;
      sleep_ms(2);
      waited += 2;
      continue;
    }
    const uint32_t part = count - done < room ? count - done : room;
    for (uint32_t i = 0; i < part * sink.ch; i++) sink.q.push_back(frames[(size_t)done * sink.ch + i]);
    done += part;
    sink.writes++;
  }
  return (int)done;
}
uint32_t pcm_queued_ms()
{
  std::lock_guard<std::mutex> g(sink.m);
  sink.drain(now_ms());
  return sink.rate ? (uint32_t)(sink.q.size() / sink.ch * 1000 / sink.rate) : 0;
}
void pcm_flush()
{
  std::lock_guard<std::mutex> g(sink.m);
  sink.drain(now_ms());
  sink.q.clear();
  sink.flushes++;
  sink.had_audio = false;
}
void pcm_close()
{
  std::lock_guard<std::mutex> g(sink.m);
  sink.open = false;
  sink.q.clear();
}
void pcm_pause(int p)
{
  std::lock_guard<std::mutex> g(sink.m);
  sink.drain(now_ms());
  sink.paused = p != 0;
  sink.last = now_ms();
  say("pcm_pause %d", p);
}

// --- заглушка ядра ESP ---------------------------------------------------------------------------------
size_t psram_max = (size_t)-1;          // --psram-max: больше за раз PSRAM не даёт (кольцо звука меньше)
void* c_alloc(size_t n, uint32_t) { return n > psram_max ? nullptr : std::calloc(1, n ? n : 1); }
void* c_realloc(void* p, size_t n) { return std::realloc(p, n); }
void c_free(void* p) { std::free(p); }
void c_log(const char* fmt, ...)
{
  char b[256];
  va_list a;
  va_start(a, fmt);
  std::vsnprintf(b, sizeof b, fmt, a);
  va_end(a);
  say("ESP: %s", b);
}
uint32_t c_millis() { return (uint32_t)now_ms(); }
void c_delay(uint32_t ms) { sleep_ms(ms ? ms : 1); }
std::atomic<int> tasks{0};
int c_task_start(zm_task_fn fn, void* arg, uint32_t, uint8_t, int8_t)
{
  tasks++;
  std::thread([fn, arg] { fn(arg); tasks--; }).detach();
  return ZM_OK;
}
void* c_mutex_create() { return new std::recursive_mutex; }
void c_mutex_lock(void* p) { static_cast<std::recursive_mutex*>(p)->lock(); }
void c_mutex_unlock(void* p) { static_cast<std::recursive_mutex*>(p)->unlock(); }
struct Signal { std::mutex m; std::condition_variable cv; bool given = false; };
void* c_signal_create() { return new Signal; }
void c_signal_give(void* p)
{
  Signal* s = static_cast<Signal*>(p);
  { std::lock_guard<std::mutex> g(s->m); s->given = true; }
  s->cv.notify_one();
}
int c_signal_wait(void* p, uint32_t ms)
{
  Signal* s = static_cast<Signal*>(p);
  std::unique_lock<std::mutex> g(s->m);
  if (!s->cv.wait_for(g, std::chrono::milliseconds(ms), [s] { return s->given; })) return ZM_TIMEOUT;
  s->given = false;
  return ZM_OK;
}
std::vector<uint8_t> last_reply;
uint8_t last_reply_code = 0;
int c_reply(uint8_t code, const void* d, uint16_t n)
{
  last_reply_code = code;
  last_reply.assign((const uint8_t*)d, (const uint8_t*)d + n);
  return ZM_OK;
}
bool no_mp3 = false;
int modules_open = 0;
int c_module_open(const char* name, void* a1, void* a2, void** handle, int* entry)
{
  if (std::strcmp(name, "MP3.ELF") != 0 || no_mp3) return ZM_NOT_FOUND;
  char* argv[3] = {const_cast<char*>(name), (char*)a1, (char*)a2};
  *entry = mp3_main(3, argv);
  *handle = new int(1);
  modules_open++;
  return ZM_OK;
}
void c_module_close(void* h) { delete static_cast<int*>(h); modules_open--; }
int core_loads = 0, core_backs = 0;
int c_core_load(const char* name, uint32_t* version)
{
  say("core_load %s", name);
  core_loads++;
  sleep_ms(50);
  std::lock_guard<std::mutex> g(tang.m);
  tang.alive = true;
  tang.vt_next = (int64_t)(now_ms() / 20 + 1) * 20;
  *version = 0x05010001;
  return ZM_OK;
}
int c_core_back(uint32_t)
{
  say("core_back");
  core_backs++;
  std::lock_guard<std::mutex> g(tang.m);
  tang.alive = false;
  return ZM_OK;
}
int c_link_read(uint32_t a, uint32_t* w, uint32_t n)
{
  if (n > 16 || (a & 3)) return ZM_ERROR;
  std::lock_guard<std::mutex> g(tang.m);
  if (!tang.alive) return ZM_ERROR;
  tang.link_ops++;
  for (uint32_t i = 0; i < n; i++) w[i] = tang.reg(a + 4 * i);
  return ZM_OK;
}
int c_link_write(uint32_t a, const uint32_t* w, uint32_t n)
{
  if (n > 16 || (a & 3)) return ZM_ERROR;
  std::lock_guard<std::mutex> g(tang.m);
  if (!tang.alive) return ZM_ERROR;
  tang.link_ops++;
  for (uint32_t i = 0; i < n; i++) tang.set(a + 4 * i, w[i]);
  return ZM_OK;
}
int c_ui_available() { return 0; }
void c_app_done() {}

zm_core_t make_core()
{
  zm_core_t c;
  std::memset(&c, 0, sizeof c);
  c.size = sizeof c;
  c.abi_major = ZM_APP_ABI_MAJOR;
  c.abi_minor = ZM_APP_ABI_MINOR;
  c.firmware = "vdac4-host";
  c.alloc = c_alloc; c.realloc = c_realloc; c.free = c_free; c.log = c_log;
  c.millis = c_millis; c.delay = c_delay; c.task_start = c_task_start;
  c.mutex_create = c_mutex_create; c.mutex_lock = c_mutex_lock; c.mutex_unlock = c_mutex_unlock;
  c.signal_create = c_signal_create; c.signal_give = c_signal_give; c.signal_wait = c_signal_wait;
  c.reply = c_reply;
  c.pcm_open = pcm_open; c.pcm_write = pcm_write; c.pcm_queued_ms = pcm_queued_ms;
  c.pcm_flush = pcm_flush; c.pcm_close = pcm_close; c.pcm_pause = pcm_pause;
  c.module_open = c_module_open; c.module_close = c_module_close;
  c.core_load = c_core_load; c.core_back = c_core_back; c.link_read = c_link_read; c.link_write = c_link_write;
  c.ui_available = c_ui_available; c.app_done = c_app_done;
  return c;
}

// --- AVI ------------------------------------------------------------------------------------------------
uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
struct Avi {
  std::vector<uint8_t> file;
  size_t movi = 0;               // начало данных movi (после «movi»)
  std::vector<Chunk> chunks;
  std::vector<size_t> frame_chunk;   // кадр → индекс чанка
  uint32_t us_frame = 40000, frames = 0;
};
bool load_avi(const char* path, Avi& a)
{
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  for (int c; (c = std::fgetc(f)) != EOF;) a.file.push_back((uint8_t)c);
  std::fclose(f);
  const std::vector<uint8_t>& b = a.file;
  for (size_t i = 12; i + 12 <= b.size();) {
    const uint32_t size = le32(&b[i + 4]);
    if (!std::memcmp(&b[i], "LIST", 4) && !std::memcmp(&b[i + 8], "movi", 4)) { a.movi = i + 12; break; }
    if (!std::memcmp(&b[i], "LIST", 4) && !std::memcmp(&b[i + 8], "hdrl", 4)) {
      const size_t avih = i + 12;
      a.us_frame = le32(&b[avih + 8]);
    }
    i += 8 + size + (size & 1);
  }
  if (!a.movi) return false;
  for (size_t i = a.movi; i + 8 <= b.size();) {
    const uint32_t size = le32(&b[i + 4]);
    if (!std::memcmp(&b[i], "idx1", 4)) break;
    Chunk c;
    c.audio = b[i + 2] == 'w' && b[i + 3] == 'b';
    c.data.assign(b.begin() + i + 8, b.begin() + i + 8 + size);
    if (!c.audio) a.frame_chunk.push_back(a.chunks.size());
    a.chunks.push_back(std::move(c));
    i += 8 + size + (size & 1);
  }
  a.frames = (uint32_t)a.frame_chunk.size();
  return true;
}

// --- снимок вывода ----------------------------------------------------------------------------------------
void shot(const std::string& dir, const char* name)
{
  if (dir.empty()) return;
  std::vector<uint8_t> img(1280 * 720 * 3);
  std::lock_guard<std::mutex> g(tang.m);
  static const int W[2][3][4] = {{{160, 1120, 0, 720}, {184, 1096, 18, 702}, {208, 1072, 36, 684}},
                                 {{0, 1280, 0, 720}, {32, 1248, 18, 702}, {64, 1216, 36, 684}}};
  const int asp = (tang.cfg >> 14) & 1, sz = ((tang.cfg >> 15) & 3) == 3 ? 0 : (tang.cfg >> 15) & 3;
  const int* w = W[asp][sz];
  for (int y = 0; y < 720; y++)
    for (int x = 0; x < 1280; x++) {
      uint8_t* p = &img[(y * 1280 + x) * 3];
      if (x >= w[0] && x < w[1] && y >= w[2] && y < w[3]) {
        const int v = 60 + 150 * (x - w[0]) / (w[1] - w[0]);     // картинка: градиент с полосами
        p[0] = (uint8_t)v; p[1] = (uint8_t)(((y / 40) & 1) ? v : 255 - v); p[2] = (uint8_t)(255 - v);
      } else {
        p[0] = p[1] = p[2] = 0;
      }
    }
  for (int k = 0; k < 2; k++) {
    const uint32_t ctrl = tang.ovl[k][0];
    if (!(ctrl & 1)) continue;
    const int s = (ctrl & 2) ? 2 : 1, a = (ctrl >> 4) & 15;
    const int ox = tang.ovl[k][1] & 2047, oy = (tang.ovl[k][1] >> 16) & 1023;
    const int cw = tang.ovl[k][2] & 2047, ch = (tang.ovl[k][2] >> 16) & 1023;
    const uint32_t base = tang.ovl[k][3] & 0x1FFF8, stride = (cw + 15) / 16 * 8;
    for (int y = 0; y < ch * s; y++)
      for (int x = 0; x < cw * s; x++) {
        const int X = ox + x, Y = oy + y;
        if (X >= 1280 || Y >= 720) continue;
        const int cx = x / s, cy = y / s;
        const uint32_t word = tang.canvas[(base + cy * stride + cx / 2) & 131071];
        const uint16_t px = (cx & 1) ? word >> 16 : word & 0xFFFF;
        const int o[3] = {(px >> 11) << 3, ((px >> 5) & 63) << 2, (px & 31) << 3};
        uint8_t* p = &img[(Y * 1280 + X) * 3];
        for (int c = 0; c < 3; c++) p[c] = (uint8_t)(p[c] + (int)std::floor((o[c] - p[c]) * (a + 1) / 16.0));
      }
  }
  const std::string path = dir + "/" + name + ".ppm";
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "P6\n1280 720\n255\n");
  std::fwrite(img.data(), 1, img.size(), f);
  std::fclose(f);
  say("снимок %s", path.c_str());
}

// --- плагин -------------------------------------------------------------------------------------------------
const zm_app_t* app;
std::vector<uint8_t> cmd(uint8_t code, const std::vector<uint8_t>& body)
{
  last_reply.clear();
  app->command(code, body.data(), (uint16_t)body.size());
  return last_reply;
}
void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i))); }
void put16(std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
void put_str(std::vector<uint8_t>& v, const char* s) { while (*s) v.push_back((uint8_t)*s++); v.push_back(0); }

struct Player {
  uint8_t flags = VS_PANEL;
  uint32_t base = 0, audio_base = 0;
  const Avi* avi = nullptr;
  std::vector<uint8_t> status()
  {
    std::vector<uint8_t> b{1, flags};
    put32(b, base); put32(b, avi->frames); put32(b, avi->us_frame); put32(b, audio_base);
    put_str(b, "KINO.AVI");
    return cmd(0x72, b);
  }
};

}  // namespace

int main(int argc, char** argv)
{
  timeBeginPeriod(1);
  if (argc < 2) { std::fprintf(stderr, "video_host файл.avi [ключи]\n"); return 2; }
  double seek_at = -1, pause_at = -1, pause_ms = 1500;
  uint32_t seek_frame = 0;
  std::string shots;
  for (int i = 2; i + 1 < argc; i += 2) {
    const std::string k = argv[i];
    const char* v = argv[i + 1];
    if (k == "--seek-at") seek_at = std::atof(v);
    else if (k == "--seek-frame") seek_frame = (uint32_t)std::atoi(v);
    else if (k == "--pause-at") pause_at = std::atof(v);
    else if (k == "--pause-ms") pause_ms = std::atof(v);
    else if (k == "--decode-ms") tang.decode_ms = std::atof(v);
    else if (k == "--cfg") tang.cfg = (uint32_t)std::strtoul(v, nullptr, 0);
    else if (k == "--shots") shots = v;
    else if (k == "--no-mp3") no_mp3 = std::atoi(v) != 0;
    else if (k == "--psram-max") psram_max = (size_t)std::strtoul(v, nullptr, 0);
  }
  Avi avi;
  if (!load_avi(argv[1], avi)) { std::fprintf(stderr, "нет AVI %s\n", argv[1]); return 2; }
  tang.per_us = avi.us_frame;
  std::printf("frames=%u\nus_frame=%u\n", avi.frames, avi.us_frame);

  static zm_core_t core = make_core();
  app = video_app_table();
  if (app->start(&core) != ZM_OK) { std::printf("start=fail\n"); return 1; }
  std::thread model(model_thread);

  // START и загрузка ядра
  const std::vector<uint8_t> name{'V', 'I', 'D', 'E', 'O', 0};
  std::vector<uint8_t> r = cmd(0x70, name);
  std::printf("start_reply=%02X %u\n", last_reply_code, r.empty() ? 255 : r[0]);
  Player pl;
  pl.avi = &avi;
  int state = 0;
  for (double until = now_ms() + 3000; now_ms() < until; sleep_ms(20)) {
    r = pl.status();
    state = r.empty() ? -1 : r[0];
    if (state == 2) break;
  }
  std::printf("state_loaded=%d\n", state);

  // заголовок AVI (до movi) — частями, как плагин
  const size_t hdr_len = avi.movi - 12;
  for (size_t off = 0; off < hdr_len; off += 200) {
    const size_t n = std::min<size_t>(200, hdr_len - off);
    std::vector<uint8_t> b;
    put32(b, (uint32_t)off);
    b.push_back(off + n >= hdr_len);
    b.insert(b.end(), avi.file.begin() + off, avi.file.begin() + off + n);
    cmd(0x76, b);
  }
  // поток с начала
  tang.reset_stream(avi.chunks, 0);
  const double t_stream = now_ms();
  say("поток с начала");

  bool list_done = false, paused_done = false, seek_done = false, shot1 = false;
  double pause_until = -1;
  uint32_t audio_state_max = 0;
  std::vector<std::pair<double, std::string>> events;
  for (;;) {
    const double t = now_ms() - t_stream;
    // панель: первые 3 с и на паузе
    pl.flags = (uint8_t)((pl.flags & ~VS_PANEL) | ((t < 3000 || (pl.flags & VS_PAUSED)) ? VS_PANEL : 0));
    r = pl.status();
    if (r.size() > 11) audio_state_max = std::max<uint32_t>(audio_state_max, r[11]);
    if (!shot1 && t > 1200) { shot1 = true; shot(shots, "panel"); }
    if (!list_done && t > 3200) {
      list_done = true;
      std::vector<uint8_t> b{1};
      put16(b, 23); put16(b, 0); put16(b, 3); put16(b, 1);
      put_str(b, "");
      for (int i = 0; i < LIST_ROWS; i++) {
        char row[64];
        std::snprintf(row, sizeof row, i == 5 ? "VERY_LONG_NAME_OF_A_FILM_THAT_DOES_NOT_FIT_%02d_ABCDEFGHIJKLMNOPQRSTUVWXYZ.AVI" : "FILM_%02d.AVI", i + 1);
        put_str(b, row);
      }
      cmd(0x77, b);
      sleep_ms(400);
      shot(shots, "list");
      std::vector<uint8_t> off{0};
      put16(off, 0); put16(off, 0); put16(off, 0); put16(off, 0xFFFF);
      put_str(off, "");
      cmd(0x77, off);
      events.push_back({now_ms(), "list"});
    }
    if (!paused_done && pause_at >= 0 && t > pause_at) {
      paused_done = true;
      { std::lock_guard<std::mutex> g(tang.m); tang.pause = true; }
      pl.flags |= VS_PAUSED;
      pl.status();                         // плагин: пауза ядра и сразу STATUS
      pause_until = t + pause_ms;
      events.push_back({now_ms(), "pause"});
      say("пауза");
    }
    if (pause_until >= 0 && t > pause_until - 300 && t < pause_until - 200) shot(shots, "paused");
    if (pause_until >= 0 && t > pause_until) {
      pause_until = -1;
      pl.flags &= (uint8_t)~VS_PAUSED;
      pl.status();
      { std::lock_guard<std::mutex> g(tang.m); tang.pause = false; }
      events.push_back({now_ms(), "resume"});
      say("пауза снята");
    }
    if (!seek_done && seek_at >= 0 && t > seek_at && pause_until < 0) {
      seek_done = true;
      // плагин: пауза ядра, сообщение, индекс; затем сброс потока (снимает и паузу), STATUS с
      // новыми кадром начала и байтами звука до него (запись индекса), подача с этого чанка
      { std::lock_guard<std::mutex> g(tang.m); tang.pause = true; }
      pl.flags |= VS_SEEKING;
      pl.status();
      std::vector<uint8_t> m{1};
      put_str(m, "Building seek index..."); put_str(m, ""); put_str(m, "");
      cmd(0x78, m);
      sleep_ms(250);
      shot(shots, "seek");
      const size_t at = avi.frame_chunk[seek_frame];
      uint32_t audio_base = 0;
      for (size_t i = 0; i < at; i++)
        if (avi.chunks[i].audio) audio_base += (uint32_t)avi.chunks[i].data.size();
      std::vector<Chunk> rest(avi.chunks.begin() + at, avi.chunks.end());
      tang.reset_stream(std::move(rest), seek_frame);
      pl.base = seek_frame;
      pl.audio_base = audio_base;
      pl.flags &= (uint8_t)~VS_SEEKING;
      pl.status();
      std::vector<uint8_t> m0{0};
      cmd(0x78, m0);
      events.push_back({now_ms(), "seek"});
      say("перемотка на кадр %u", seek_frame);
    }
    bool done;
    {
      std::lock_guard<std::mutex> g(tang.m);
      done = tang.pos >= tang.chunks.size() && tang.waiting == 0 && tang.dec_end < 0;
    }
    if (done) { std::printf("stream_end=%.0f\n", now_ms()); sleep_ms(600); break; }
    if (t > 120000) { std::printf("timeout=1\n"); break; }
    sleep_ms(80);
  }
  // выход
  cmd(0x73, {});
  for (double until = now_ms() + 3000; now_ms() < until; sleep_ms(20)) {
    r = cmd(0x72, {});
    state = r.empty() ? -1 : r[0];
    if (state == 3) break;
  }
  std::printf("state_exit=%d\n", state);
  app->stop();
  model_run = false;
  model.join();
  for (int i = 0; i < 100 && tasks; i++) sleep_ms(10);
  std::printf("tasks_left=%d\nmodules_open=%d\ncore_loads=%d\ncore_backs=%d\n", (int)tasks, modules_open,
              core_loads, core_backs);
  std::printf("audio_state_max=%u\n", audio_state_max);

  // итоги ядра: какие кадры ролика не показаны (диапазоны; при перемотке вперёд — пропущенные ею)
  {
    std::lock_guard<std::mutex> g(tang.m);
    std::string miss;
    for (uint32_t f = 0; f < avi.frames;) {
      if (tang.shown.count(f)) { f++; continue; }
      uint32_t e = f;
      while (e + 1 < avi.frames && !tang.shown.count(e + 1)) e++;
      char b[32];
      std::snprintf(b, sizeof b, "%s%u-%u", miss.empty() ? "" : ",", f, e);
      miss += b;
      f = e + 1;
    }
    std::printf("frames_shown=%zu\nframes_missing=%s\n", tang.shown.size(), miss.c_str());
    std::printf("shown_0_1_2=%.1f,%.1f,%.1f\nfirst_play=%.1f\n", tang.shown.count(0) ? tang.shown[0] : -1.0,
                tang.shown.count(1) ? tang.shown[1] : -1.0, tang.shown.count(2) ? tang.shown[2] : -1.0, sink.first_play);
    std::printf("audio_bytes_in=%llu\naudio_bytes_skipped=%llu\nlink_ops=%llu\n",
                (unsigned long long)tang.au_bytes_in, (unsigned long long)tang.au_bytes_skipped,
                (unsigned long long)tang.link_ops);
  }
  {
    std::lock_guard<std::mutex> g(sink.m);
    std::string under;
    for (double u : sink.underruns) under += (under.empty() ? "" : ",") + std::to_string((long)u);
    std::printf("pcm_rate=%u\npcm_ch=%u\npcm_opens=%u\npcm_played=%llu\npcm_underruns=%s\n", sink.rate, sink.ch,
                sink.opens, (unsigned long long)sink.played, under.c_str());
    // пики: начало звука против показа кадра того же времени ролика
    std::string offs;
    for (double on : sink.onsets) {
      // какой кадр виден в момент пика
      uint32_t f = 0;
      double best = -1;
      for (const auto& kv : tang.shown)
        if (kv.second <= on && kv.second > best) { best = kv.second; f = kv.first; }
      const double content = f * (avi.us_frame / 1000.0);
      const long sec = std::lround(content / 1000.0);
      const uint32_t frame = (uint32_t)std::lround(sec * 1000.0 / (avi.us_frame / 1000.0));
      if (!tang.shown.count(frame)) continue;
      char b[64];
      std::snprintf(b, sizeof b, "%s%ld:%.1f", offs.empty() ? "" : ",", sec, on - tang.shown[frame]);
      offs += b;
    }
    std::printf("beeps=%s\n", offs.c_str());
  }
  std::string ev;
  for (const auto& e : events) {
    char b[64];
    std::snprintf(b, sizeof b, "%s%s:%.0f", ev.empty() ? "" : ",", e.second.c_str(), e.first);
    ev += b;
  }
  std::printf("events=%s\n", ev.c_str());
  return 0;
}
