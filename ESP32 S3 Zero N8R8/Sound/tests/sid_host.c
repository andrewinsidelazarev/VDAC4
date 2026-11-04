/* SID.ELF на ПК (clang): модуль целиком, хост — malloc и файлы. Печатает «имя=значение».
 *
 *   sid_host ФАЙЛ.sid [--seconds S] [--base SIDLEN.BIN] [--seek МС] [--before СЕК] [--wav out.wav] [--worker 0|1]
 *                     [--reference 1] [--load N]
 *
 * Звук — до конца основной подмелодии или S секунд; --seek — относительное время в мс
 * после --before секунд обычной игры; --worker 0 — без помощника (всё на одном «ядре»), иначе помощник — поток, как ядро 1
 * у PLAYER; --reference 1 — звук исходным однопоточным cRSID_generateSample (сверка конвейера бит в
 * бит). Время: core0_ms — render без ожидания помощника, core1_ms — работа помощника. --load N —
 * такты «ESP» понарошку: N на каждый замер ccount в потоке (замок нагрузки модуля). */
#include <stdint.h>
static uint32_t test_load;
static _Thread_local uint32_t test_clock;
static uint32_t sid_test_clock(void) { return test_clock += test_load; }
#define SID_TEST_CLOCK sid_test_clock
static int level_enabled = 1;
#define SID_LEVEL_ENABLED level_enabled
#define main sid_main
#include "../modules/sid/main/sid_source.c"
#undef main

#include <math.h>
#include <time.h>
#include <windows.h>

/* Помощник — поток, протокол — как worker_task PLAYER: «работа» и «готово» — двоичные семафоры
 * (события с автосбросом), «занят» снимает ожидающий, получив «готово». */
static HANDLE worker_go, worker_done, worker_thread;
static void (*worker_fn)(void*);
static void* worker_arg;
static int worker_busy, worker_inline;     /* --worker 2: работа помощника сразу, в том же потоке */
static double worker_ms, wait_ms;
static LARGE_INTEGER freq;

static double now_ms(void) {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}
static DWORD WINAPI worker_main(LPVOID unused) {
  (void)unused;
  for (;;) {
    WaitForSingleObject(worker_go, INFINITE);
    const double t = now_ms();
    worker_fn(worker_arg);
    worker_ms += now_ms() - t;
    SetEvent(worker_done);
  }
  return 0;
}
static int host_worker_start(void (*fn)(void*), void* arg) {
  if (worker_inline) {
    fn(arg);
    return ZM_OK;
  }
  if (worker_busy) return ZM_BUSY;
  worker_fn = fn;
  worker_arg = arg;
  worker_busy = 1;
  SetEvent(worker_go);
  return ZM_OK;
}
static void host_worker_wait(void) {
  const double t = now_ms();
  if (worker_busy) {
    WaitForSingleObject(worker_done, INFINITE);
    worker_busy = 0;
  }
  wait_ms += now_ms() - t;
}
static void host_rest(void) {}

static uint8_t* base_data;
static size_t base_size;

static void* host_alloc(size_t size, uint32_t caps) {
  (void)caps;
  return malloc(size);
}
static void host_free(void* p) { free(p); }
static void host_log(const char* format, ...) {
  va_list args;
  va_start(args, format);
  printf("log=");
  vprintf(format, args);
  printf("\n");
  va_end(args);
}
static int host_res(const char* name, const uint8_t** data, size_t* size) {
  if (base_data == NULL || strcmp(name, BASE_NAME) != 0) return ZM_NOT_FOUND;
  *data = base_data;
  *size = base_size;
  return ZM_OK;
}

static uint8_t* read_file(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (f == NULL) return NULL;
  fseek(f, 0, SEEK_END);
  *size = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* data = (uint8_t*)malloc(*size ? *size : 1);
  if (fread(data, 1, *size, f) != *size) *size = 0;
  fclose(f);
  return data;
}

static void wav_header(FILE* f, uint32_t frames) {
  const uint32_t bytes = frames * 4;
  fwrite("RIFF", 1, 4, f);
  uint32_t v = 36 + bytes;
  fwrite(&v, 4, 1, f);
  fwrite("WAVEfmt ", 1, 8, f);
  v = 16;
  fwrite(&v, 4, 1, f);
  const uint16_t fmt[2] = {1, 2};
  fwrite(fmt, 2, 2, f);
  v = RATE;
  fwrite(&v, 4, 1, f);
  v = RATE * 4;
  fwrite(&v, 4, 1, f);
  const uint16_t align[2] = {4, 16};
  fwrite(align, 2, 2, f);
  fwrite("data", 1, 4, f);
  fwrite(&bytes, 4, 1, f);
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  double seconds = 600, before = 0;
  int repeat_seek = 0, close_pending = 0;
  int seek_by = 0, use_worker = 1, reference = 0, cancel_init = 0;
  QueryPerformanceFrequency(&freq);
  const char* wav = NULL;
  for (int i = 2; i + 1 < argc; i += 2) {
    if (strcmp(argv[i], "--seconds") == 0) seconds = atof(argv[i + 1]);
    else if (strcmp(argv[i], "--normalize") == 0) level_enabled = atoi(argv[i + 1]);
    else if (strcmp(argv[i], "--before") == 0) before = atof(argv[i + 1]);
    else if (strcmp(argv[i], "--repeat-seek") == 0) repeat_seek = atoi(argv[i + 1]);
    else if (strcmp(argv[i], "--close-pending") == 0) close_pending = atoi(argv[i + 1]);
    else if (strcmp(argv[i], "--base") == 0) base_data = read_file(argv[i + 1], &base_size);
    else if (strcmp(argv[i], "--seek") == 0) seek_by = atoi(argv[i + 1]);
    else if (strcmp(argv[i], "--wav") == 0) wav = argv[i + 1];
    else if (strcmp(argv[i], "--worker") == 0) use_worker = atoi(argv[i + 1]);
    else if (strcmp(argv[i], "--reference") == 0) reference = atoi(argv[i + 1]);
    else if (strcmp(argv[i], "--cancel-init") == 0) cancel_init = atoi(argv[i + 1]);
    else if (strcmp(argv[i], "--load") == 0) test_load = (uint32_t)atoi(argv[i + 1]);
  }
  size_t size = 0;
  uint8_t* file = read_file(argv[1], &size);
  if (file == NULL) return 3;

  static zm_host_t h;
  h.size = sizeof(h);
  h.abi_major = ZM_SOURCE_ABI_MAJOR;
  h.abi_minor = ZM_SOURCE_ABI_MINOR;
  h.alloc = host_alloc;
  h.free = host_free;
  h.log = host_log;
  h.resource = host_res;
  h.rest = host_rest;
  worker_inline = use_worker == 2;
  if (use_worker) {
    worker_go = CreateEvent(NULL, FALSE, FALSE, NULL);
    worker_done = CreateEvent(NULL, FALSE, FALSE, NULL);
    worker_thread = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);
    h.worker_start = host_worker_start;
    h.worker_wait = host_worker_wait;
  }
  const zm_source_t* src = NULL;
  char* args[3] = {"sid", (char*)&h, (char*)&src};
  printf("entry=%d\n", sid_main(3, args));
  printf("probe=%d\n", src->probe(file, size));
  const double open_started = now_ms();
  void* s = src->open((uint32_t)size, file);
  printf("open_ms=%.3f\n", now_ms() - open_started);
  free(file);                                  /* keeps_file = 0: модуль взял копию */
  printf("open=%d\n", s != NULL);
  if (s == NULL) return 0;
  uint32_t offset = 0;
  float discarded[2 * ZM_RENDER_MAX_FRAMES];
  uint32_t before_frames = 0, before_end = (uint32_t)(before * RATE);
  while (before_frames < before_end) {
    uint32_t n = 0, cap = before_end - before_frames;
    if (cap > ZM_RENDER_MAX_FRAMES) cap = ZM_RENDER_MAX_FRAMES;
    if (src->render(s, discarded, cap, &n) == ZM_RENDER_END) break;
    before_frames += n;
  }
  printf("before_frames=%u\nseek_unit=%u\n", before_frames, (unsigned)src->seek_unit);
  if (cancel_init) {
    float first[2 * ZM_RENDER_MAX_FRAMES];
    uint32_t n = 0;
    int result = src->render(s, first, ZM_RENDER_MAX_FRAMES, &n);
    printf("init_result=%d\ninit_frames=%u\ninit_steps=%u\n", result, n, ((Source*)s)->init_steps);
    seek_by = INT32_MAX;
  }
  printf("gain_before_seek=%.4f\nblocks_before_seek=%u\n", ((Source*)s)->level.gain_db, ((Source*)s)->level.blocks);
  const double seek_started = now_ms();
  if (seek_by != 0) src->seek(s, seek_by, &offset);
  printf("seek_ms=%.3f\n", now_ms() - seek_started);
  printf("gain_after_seek=%.4f\nblocks_after_seek=%u\n", ((Source*)s)->level.gain_db, ((Source*)s)->level.blocks);
  if (repeat_seek || close_pending) {
    uint32_t n = 0;
    const int r = src->render(s, discarded, ZM_RENDER_MAX_FRAMES, &n);
    printf("pending_result=%d\npending_frames=%u\npending_seek=%u\n", r, n, ((Source*)s)->seeking);
    if (repeat_seek) src->seek(s, repeat_seek, &offset);
    if (close_pending) {
      const double closed = now_ms();
      src->close(s);
      printf("close_ms=%.3f\n", now_ms() - closed);
      return 0;
    }
  }
  if (reference) {
    /* Независимый исходный init, процессор и таймеры cRSID. */
    cRSID_startSubtune((unsigned char)((Source*)s)->song);
    ((Source*)s)->initializing = 0;
  }
  zm_info_t info;
  src->info(s, &info);
  printf("format=%s\ntitle=%s\nartist=%s\nalbum=%s\nyear=%u\n", info.format, info.title, info.artist,
         info.album, (unsigned)info.year);
  printf("song=%u\nsongs=%u\nvoices=%u\nduration_ms=%u\n", (unsigned)((Source*)s)->song,
         (unsigned)((Source*)s)->songs, (unsigned)info.voices, (unsigned)info.duration_ms);

  printf("order=%u\norder_length=%u\nseek_position_ms=%u\n", info.order, info.order_length, info.position_ms);
  FILE* out = wav ? fopen(wav, "wb") : NULL;
  if (out) wav_header(out, 0);
  static float buf[2 * ZM_RENDER_MAX_FRAMES];
  static int16_t pcm[2 * ZM_RENDER_MAX_FRAMES];
  uint64_t frames = 0;
  double sum = 0, peak = 0, diff = 0;
  int ended = 0, max_level = 0;
  const uint64_t limit = (uint64_t)(seconds * RATE);
  const double started = now_ms();
  double prep_ms = 0, max_prep_ms = 0;
  uint32_t prep_calls = 0;
  uint64_t seek_sound = 0;
  while (frames < limit) {
    uint32_t got = 0;
    int r = ZM_RENDER_OK;
    if (reference) {
      /* Исходный cRSID одним потоком: cRSID_generateSample, как его cRSID_generateSound. */
      Source* src_s = (Source*)s;
      cRSID_C64.RealSIDmode = cRSID.RealSIDmode;
      cRSID_C64.AudioThread_SIDchipCount = cRSID_C64.SIDchipCount;
      cRSID_C64.Stereo = cRSID_C64.AudioThread_SIDchipCount > 1 ? cRSID.Stereo : CRSID_CHANNELMODE_MONO;
      cRSID_C64.HighQualitySID = cRSID.HighQualitySID;
      cRSID_C64.HighQualityResampler = cRSID.HighQualityResampler;
      uint32_t n = src_s->end > src_s->frame ? src_s->end - src_s->frame : 0;
      if (n > ZM_RENDER_MAX_FRAMES) n = ZM_RENDER_MAX_FRAMES;
      if (n == 0) r = ZM_RENDER_END;
      const uint32_t fade_from = src_s->end > 4 * RATE ? src_s->end - FADE_FRAMES : src_s->end;
      for (uint32_t i = 0; i < n; ++i) {
        const cRSID_Output o = cRSID_generateSample();
        float gain = 1.0f / 32768.0f;
        const uint32_t frame = src_s->frame + i;
        if (frame >= fade_from) gain *= (float)(src_s->end - frame) / (float)FADE_FRAMES;
        buf[2 * i] = (float)o.L * gain;
        buf[2 * i + 1] = (float)o.R * gain;
      }
      src_s->frame += n;
      got = n;
    } else {
      const int preparing = ((Source*)s)->seeking || ((Source*)s)->initializing;
      const uint64_t sound_before = frames_sound;
      const double call_started = now_ms();
      r = src->render(s, buf, ZM_RENDER_MAX_FRAMES, &got);
      const double elapsed = now_ms() - call_started;
      if (preparing && got == 0) {
        prep_ms += elapsed;
        if (elapsed > max_prep_ms) max_prep_ms = elapsed;
        ++prep_calls;
        seek_sound += frames_sound - sound_before;
      }
    }
    if (r == ZM_RENDER_END) {
      ended = 1;
      break;
    }
    for (uint32_t i = 0; i < 2 * got; ++i) {
      const double x = buf[i];
      sum += x * x;
      if (fabs(x) > peak) peak = fabs(x);
      pcm[i] = (int16_t)lrint(x * 32767.0);
    }
    for (uint32_t i = 0; i < got; ++i) diff += fabs(buf[2 * i] - buf[2 * i + 1]);
    if (out) fwrite(pcm, 4, got, out);
    uint8_t lv[16];
    const int n = src->levels(s, lv, 16);
    for (int i = 0; i < n; ++i) if (lv[i] > max_level) max_level = lv[i];
    frames += got;
  }
  const double spent = (now_ms() - started) / 1000.0;
  if (out) {
    fseek(out, 0, SEEK_SET);
    wav_header(out, (uint32_t)frames);
    fclose(out);
  }
  src->info(s, &info);
  printf("frames=%llu\nended=%d\nposition_ms=%u\n", (unsigned long long)frames, ended,
         (unsigned)info.position_ms);
  printf("rms=%.5f\npeak=%.5f\nstereo_diff=%.5f\nmax_level=%d\n", frames ? sqrt(sum / (2.0 * frames)) : 0,
         peak, frames ? diff / frames : 0, max_level);
  printf("speed=%.1f\n", spent > 0 ? (double)frames / RATE / spent : 0);   /* во сколько раз быстрее звука */
  printf("core0_ms=%.1f\ncore1_ms=%.1f\n", spent * 1000.0 - wait_ms, worker_ms);
#ifdef SID_PROFILE
  {
    static const char* names[PROF_N] = {"cpu", "ciavic", "adsr", "snap", "waves", "nyquist", "out", "post", "level"};
    for (int k = 0; k < PROF_N; ++k) {
      printf("prof_%s=%.1f\n", names[k], frames ? (double)prof[k] / (double)frames : 0.0);   /* такты x86 на отсчёт */
    }
  }
#endif
  printf("prep_ms=%.3f\nmax_prep_ms=%.3f\nprep_calls=%u\nseek_frames=%u\nseek_sound_frames=%llu\n",
         prep_ms, max_prep_ms, prep_calls, ((Source*)s)->seek_frames, (unsigned long long)seek_sound);
  printf("level_bytes=%u\nlevel_frames=%llu\nlevel_gain=%.4f\nlevel_lufs=%.4f\nlevel_blocks=%u\n",
         (unsigned)sizeof(zm_level_t), (unsigned long long)((Source*)s)->level_frames,
         ((Source*)s)->level.gain_db, ((Source*)s)->level.song_lufs, ((Source*)s)->level.blocks);
  src->close(s);
  return 0;
}
