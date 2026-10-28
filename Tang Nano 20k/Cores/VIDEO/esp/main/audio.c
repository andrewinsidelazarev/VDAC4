/* SPDX-License-Identifier: Apache-2.0
 * VIDEO.ELF: звук ролика. Ядро видео кладёт байты звуковых чанков (NNwb) в FIFO звука (1024 слова,
 * окно LINK 0x2000; R_AU_WP — сколько записано, эпоха потока и пауза), отсюда они читаются,
 * декодируются (PCM — здесь, MP3 — декодером MP3.ELF плеера, вкомпилированным в VIDEO.ELF) и идут
 * на ЦАП ESP (pcm_write).
 *
 * ТЕМП И СИНХРОННОСТЬ. Ведущий — темп ядра (кадры по выходным кадрам 50 Гц): он ровный и не
 * зависит от того, где в файле лежит звук. Звук держится у картинки: время картинки — номер
 * показанного кадра (R_SLOTS − 1) · период + сколько прошло с его выхода; время звука — время
 * отсчёта, который звучит сейчас (выдано на ЦАП минус очередь, плюс выброшенное, минус
 * вставленная тишина). Расходятся больше SYNC_MS — звук позади: лишнее входящее выбрасывается
 * (ещё не выброшенное уже в счёте — иначе поправка раскачивается); звук впереди (картинка
 * встала: поток не успел) — вставляется тишина. После начала звука и после паузы — точно
 * (ALIGN_MS). Очередь ЦАП держится короткой (буфер DMA ESP + QUEUE_MS), поэтому поправка
 * действует сразу.
 * Режим часов звука ядра (R_AU_CTL бит 1) не включается: если звук в файле лежит позже своего кадра
 * дальше, чем разбор опережает показ (1–2 кадра), часы встали бы навсегда.
 *
 * ПАУЗА — по биту паузы ядра (R_AU_WP бит 23, его ставит плагин) и по VS_PAUSED/VS_SEEKING
 * плагина: ЦАП встаёт вместе с кадром, не ожидая STATUS.
 *
 * FIFO ЯДРА ЧИТАЕТСЯ ВСЕГДА, пока приём включён: полный FIFO держит разбор ядра, а за ним и
 * картинку. В файле звук может лежать раньше своих кадров (перекос перемежения) — он копится в
 * кольце (2 МБ PSRAM: 12 с PCM 44,1 кГц стерео 16 бит, минуты MP3). Не влезает — выбрасывается
 * самое НОВОЕ (звук далёкого будущего, нужный сейчас остаётся), на его месте в кольце — метка
 * разрыва: когда чтение до неё дойдёт, время звука прыгнет на выброшенное (по nAvgBytesPerSec), и
 * синхронность восстановит тишина. Иначе первый кадр, лежащий в файле за звуком, не вышел бы
 * никогда: звук ждал бы картинку, а картинка — место для звука.
 *
 * ЭПОХА. Сброс потока (перемотка, новый ролик — REG_CPURESET от плагина) увеличивает эпоху ядра и
 * обнуляет его указатель записи; ядро не берёт звук, пока эпоха не подтверждена (R_AU_CTL [15:8]):
 * здесь — свой указатель в 0, очередь ЦАП и декодер — заново, затем подтверждение.
 */
#include <string.h>

#include "video.h"
#include "zm_source.h"

#define QUEUE_MS     60u         /* очередь ЦАП сверх буфера DMA ESP не длиннее */
/* pcm_queued_ms() прошивки ESP считает и буфер DMA I2S: 8 × 480 кадров (audio_player.cpp,
   kDmaDescriptors × kDmaFrames) — 87 мс при 44,1 кГц, 120 при 32, 174 при 22,05. Прежде предел
   очереди был 120 мс на всё: при 22,05 кГц сразу после записи очередь уже 174 мс, звук шёл кусками
   по 23 мс раз в ~55 мс (ЦАП получал ~8,3 тыс. кадров/с вместо 22 050, «fr16677/2000ms udf27»),
   отставал, и подстройка выбрасывала по 250 мс каждые 100 мс — ролик «тормозил» (28.10.2026, Muse
   22 кГц; на пути FT812 звук шёл через GS). */
#define DMA_FRAMES   3840u
#define START_MS     60u         /* начать вывод, когда принято на столько звука */
#define SYNC_MS      30u         /* расхождение с картинкой больше — поправка */
#define ALIGN_MS     8u          /* после начала звука и после паузы — до такого */
#define FIX_MAX_MS   250u        /* поправка за раз не больше */
#define STEP_MS      4u          /* сон между шагами (сторож задач ESP) */
#define FRAMES       512u        /* кадров PCM за раз */
#define GAPS         16u         /* меток разрыва в кольце */
#define AU_PAUSE     (1u << 23)  /* R_AU_WP: пауза ядра */

static const zm_core_t* core;

/* --- кольцо принятых байтов ------------------------------------------------------------------- */
static uint8_t* ring;
static uint32_t ring_size;                   /* степень 2 */
static volatile uint32_t r_head, r_tail;     /* пишет и читает одна задача — без замков */
/* разрывы: на месте pos кольца выброшено frames кадров звука (pos — счёт r_head) */
static struct { uint32_t pos, frames; } gap[GAPS];
static uint32_t gap_n;
static int64_t gap_shift;                    /* разрывы, через которые чтение уже прошло */

static uint32_t ring_used(void) { return r_head - r_tail; }
static uint32_t ring_free(void) { return ring_size - ring_used(); }
static void ring_put(const uint8_t* p, uint32_t n)
{
  const uint32_t at = r_head & (ring_size - 1), first = n < ring_size - at ? n : ring_size - at;
  memcpy(ring + at, p, first);
  memcpy(ring, p + first, n - first);
  r_head += n;
}
/* до n байтов, не дальше ближайшего разрыва; разрыв на месте чтения — в счёт gap_shift */
static uint32_t ring_get(uint8_t* p, uint32_t n)
{
  while (gap_n && gap[0].pos == r_tail) {
    gap_shift += gap[0].frames;
    memmove(gap, gap + 1, (--gap_n) * sizeof gap[0]);
  }
  uint32_t have = ring_used();
  if (gap_n && gap[0].pos - r_tail < have) have = gap[0].pos - r_tail;
  if (n > have) n = have;
  const uint32_t at = r_tail & (ring_size - 1), first = n < ring_size - at ? n : ring_size - at;
  memcpy(p, ring + at, first);
  memcpy(p + first, ring, n - first);
  r_tail += n;
  return n;
}
static void ring_reset(void)
{
  r_head = r_tail = 0;
  gap_n = 0;
  gap_shift = 0;
}

/* --- декодер MP3 ------------------------------------------------------------------------------- */
/* Исходник MP3.ELF плеера (Sound/modules/mp3/main/mp3_source.c, Helix), собранный в VIDEO.ELF: его
   main переименован в mp3_main (main/CMakeLists.txt). Прежде VIDEO.ELF открывал MP3.ELF из кэша
   модулей ESP, а туда его кладёт только плагин плеера — после других модулей (NES, NEXT, VIDEO)
   его в кэше не было, и звук MP3 выключался («MP3.ELF not available»). */
int mp3_main(int argc, char* argv[]);

static zm_host_t host;
static const zm_source_t* mp3;
static void* mp3_inst;
static size_t host_read(void* d, size_t n) { return ring_get((uint8_t*)d, (uint32_t)n); }
static size_t host_available(void) { return ring_used(); }
static int host_ended(void) { return 0; }
/* состояние декодера (~9 КБ) просится во внутреннюю память; её при работе ядра видео мало (Wi-Fi,
   LWIP) — тогда в PSRAM: 44,1 кГц стерео хватает и её */
static void* host_alloc(size_t size, uint32_t caps)
{
  void* p = core->alloc(size, caps);
  if (!p && caps != ZM_MEM_PSRAM) p = core->alloc(size, ZM_MEM_PSRAM);
  return p;
}

static void mp3_close(void)
{
  if (mp3_inst && mp3) mp3->close(mp3_inst);
  mp3_inst = NULL;
}
static void mp3_unload(void)
{
  mp3_close();
  mp3 = NULL;
}
static int mp3_load(void)
{
  if (mp3) return 1;
  /* поля до make_room: поток без файла целиком, помощника и rest у этого хозяина нет */
  host.size = (uint16_t)offsetof(zm_host_t, make_room);
  host.abi_major = ZM_SOURCE_ABI_MAJOR;
  host.abi_minor = ZM_SOURCE_ABI_MINOR;
  host.alloc = host_alloc;
  host.free = core->free;
  host.log = core->log;
  host.input_read = host_read;
  host.input_available = host_available;
  host.input_ended = host_ended;
  host.file_type = "MP3";
  host.stream_offset = UINT32_MAX;
  char* argv[] = {(char*)"MP3", (char*)&host, (char*)&mp3, NULL};
  if (mp3_main(3, argv) != ZM_SOURCE_ABI_MAJOR || !mp3 || mp3->abi_major != ZM_SOURCE_ABI_MAJOR) {
    mp3 = NULL;
    core->log("video: MP3 decoder did not start");
    return 0;
  }
  return 1;
}

/* --- сеанс звука ------------------------------------------------------------------------------- */
typedef struct {
  uint32_t session;
  uint32_t fmt_serial;
  uint16_t tag, channels, bits, block_align;
  uint32_t rate, avg_bytes;
  int enabled;                  /* звук этого формата декодируется (иначе принятое выбрасывается) */
  uint8_t epoch;                /* подтверждённая эпоха */
  uint32_t rp;                  /* слов FIFO прочитано (по модулю 2048) */
  int open;                     /* pcm_open сделан */
  int started;                  /* вывод идёт */
  int paused;
  int align;                    /* следующая сверка — точная (ALIGN_MS) */
  uint64_t written;             /* кадров PCM выдано на ЦАП с начала эпохи (с тишиной) */
  int64_t content_shift;        /* поправки: кадров звука выброшено минус вставлено */
  uint32_t out_rate;            /* частота вывода (у MP3 — из декодера) */
  uint8_t out_ch;
  uint32_t slots_seen, slot_ms; /* темп ядра: последний виденный счёт слотов и когда сменился */
  uint32_t pause_ms;            /* когда началась пауза */
  uint32_t fix_drop;            /* выбросить ещё столько кадров входа */
  uint32_t last_sync_ms;
  uint32_t fix_log_ms, fix_n;   /* подстройки синхронности: сводка в журнал не чаще раза в 5 с */
  int32_t fix_max;
  uint8_t pcm_carry[8];         /* неполный кадр PCM между чтениями */
  uint32_t carry_n;
  uint32_t overflow_bytes;      /* выброшено при полном кольце за эпоху (журнал) */
} asess_t;

static float frames_buf[FRAMES * 2];

/* Приём звука ядром (R_AU_CTL бит 0) включён всегда: при выключенном ядро пропускает чанки звука,
   а формат (заголовок от плагина) мог стать известен здесь позже начала потока — первые чанки
   пропали бы, и звук ушёл бы вперёд картинки. Порядок держит эпоха; звук, который декодировать
   нечем, выбрасывается здесь. */
static void set_ctl(asess_t* a)
{
  vlink_wr(R_AU_CTL, 1u | (uint32_t)a->epoch << 8);
}

/* новый поток (эпоха): всё с начала, кроме указателя чтения FIFO ядра (его обнуляет только новая
   эпоха — ядро обнуляет свой указатель записи вместе с ней) */
static void restart(asess_t* a)
{
  ring_reset();
  a->started = 0;
  a->written = 0;
  a->content_shift = 0;
  a->fix_drop = 0;
  a->carry_n = 0;
  a->slots_seen = 0;
  a->slot_ms = core->millis();
  a->overflow_bytes = 0;
  if (a->open) core->pcm_flush();
  mp3_close();
}

/* новый формат — заголовок ролика (плагин шлёт его при сброшенном потоке, до подачи): декодер и
   вывод — заново; принятое в кольце не трогается — это уже звук нового потока */
static void configure(asess_t* a)
{
  core->mutex_lock(V.lock);
  a->fmt_serial = V.audio_fmt.serial;
  a->tag = V.audio_fmt.tag;
  a->channels = V.audio_fmt.channels;
  a->rate = V.audio_fmt.rate;
  a->avg_bytes = V.audio_fmt.avg_bytes;
  a->bits = V.audio_fmt.bits;
  a->block_align = V.audio_fmt.block_align;
  core->mutex_unlock(V.lock);
  a->started = 0;
  a->carry_n = 0;
  mp3_close();
  if (a->open) { core->pcm_close(); a->open = 0; }
  a->enabled = 0;
  V.audio_state = 0;
  if (a->tag == 1 && (a->bits == 8 || a->bits == 16) && a->channels >= 1 &&
      a->block_align >= a->channels * a->bits / 8 && a->block_align <= sizeof a->pcm_carry) {
    a->enabled = 1;
    a->out_rate = a->rate;
    a->out_ch = a->channels >= 2 ? 2 : 1;
    a->avg_bytes = a->rate * a->block_align;
  } else if (a->tag == 0x55 || a->tag == 0x50) {
    if (mp3_load()) { a->enabled = 1; a->out_rate = 0; a->out_ch = 0; }
    else V.audio_state = 3;
  }
  if (a->enabled) V.audio_state = 1;
}

/* слова из ядра не влезли в кольцо: выброшены — метка разрыва на месте r_head */
static void drop_newest(asess_t* a, uint32_t bytes)
{
  const uint32_t rate = a->out_rate ? a->out_rate : a->rate;
  const uint32_t frames = a->avg_bytes ? (uint32_t)((uint64_t)bytes * rate / a->avg_bytes) : 0;
  if (gap_n && gap[gap_n - 1].pos == r_head) gap[gap_n - 1].frames += frames;
  else if (gap_n < GAPS) { gap[gap_n].pos = r_head; gap[gap_n].frames = frames; gap_n++; }
  else gap[GAPS - 1].frames += frames;     /* меток не хватает: в последнюю (время — грубее) */
  if (!a->overflow_bytes) core->log("video: audio arrives far ahead of the picture, the ring is full");
  a->overflow_bytes += bytes;
}

/* ядро → кольцо: всё, что есть (FIFO ядра не держим — см. начало файла) */
static void pull(asess_t* a, uint32_t wp)
{
  uint32_t words[16];
  uint32_t have = (wp - a->rp) & 2047u;
  while (have) {
    uint32_t n = have > 16 ? 16 : have;
    const uint32_t cell = a->rp & (AUDIO_WORDS - 1);
    if (n > AUDIO_WORDS - cell) n = AUDIO_WORDS - cell;
    if (vlink_read(W_AUDIO + 4 * cell, words, n) != ZM_OK) break;
    if (ring_free() >= 4 * n) {
      for (uint32_t i = 0; i < n; i++) {
        const uint8_t b[4] = {(uint8_t)words[i], (uint8_t)(words[i] >> 8), (uint8_t)(words[i] >> 16),
                              (uint8_t)(words[i] >> 24)};
        ring_put(b, 4);
      }
    } else {
      drop_newest(a, 4 * n);
    }
    a->rp = (a->rp + n) & 2047u;
    have -= n;
  }
  vlink_wr(R_AU_WP, a->rp);
}

/* принято ли на ms звука (PCM — точно; MP3 — несколько кадров по байтам) */
static int have_audio_ms(const asess_t* a, uint32_t ms)
{
  if (a->tag == 1) {
    const uint32_t need = (uint32_t)((uint64_t)a->rate * ms / 1000u) * a->block_align;
    return ring_used() >= need;
  }
  return ring_used() >= 2048;
}

/* кадров звука в кольце сверх уже назначенного выброса (MP3 — по nAvgBytesPerSec) */
static uint32_t buffered_frames(const asess_t* a)
{
  uint64_t f;
  if (a->tag == 1) f = (ring_used() + a->carry_n) / a->block_align;
  else f = a->avg_bytes ? (uint64_t)ring_used() * a->out_rate / a->avg_bytes : 0;
  return f > a->fix_drop ? (uint32_t)(f - a->fix_drop) : 0;
}

/* декодировать до max кадров во frames_buf; вернёт число кадров (0 — нечего) */
static uint32_t decode(asess_t* a, uint32_t max)
{
  if (max > FRAMES) max = FRAMES;
  if (a->tag == 1) {
    const uint32_t ba = a->block_align, bps = a->bits / 8;
    uint32_t got = 0;
    uint8_t blk[8];
    while (got < max) {
      /* кадр: остаток прошлого чтения + кольцо (разрывы — по целым словам, кадр PCM — 1, 2 или 4
         байта, поэтому разрыв кадр не режет) */
      uint32_t n = a->carry_n;
      memcpy(blk, a->pcm_carry, n);
      n += ring_get(blk + n, ba - n);
      if (n < ba) { memcpy(a->pcm_carry, blk, n); a->carry_n = n; break; }
      a->carry_n = 0;
      for (uint32_t c = 0; c < a->out_ch; c++) {
        float s;
        if (bps == 1) s = ((int)blk[c] - 128) * (1.0f / 128.0f);
        else s = (int16_t)(blk[2 * c] | blk[2 * c + 1] << 8) * (1.0f / 32768.0f);
        frames_buf[got * a->out_ch + c] = s;
      }
      got++;
    }
    return got;
  }
  /* MP3 */
  if (!mp3) return 0;
  if (!mp3_inst) {
    if (!have_audio_ms(a, 0)) return 0;
    mp3_inst = mp3->open(0, NULL);
    if (!mp3_inst) return 0;
  }
  uint32_t got = 0;
  const int r = mp3->render(mp3_inst, frames_buf, max, &got);
  if (r == ZM_RENDER_ERROR) { mp3_close(); return 0; }
  if (got && !a->out_rate) {
    zm_info_t info;
    memset(&info, 0, sizeof info);
    mp3->info(mp3_inst, &info);
    a->out_rate = info.rate;
    a->out_ch = info.channels >= 2 ? 2 : 1;
  }
  return got;
}

static void write_frames(asess_t* a, const float* f, uint32_t n)
{
  uint32_t done = 0;
  while (done < n && V.running && V.session_live) {
    const int w = core->pcm_write(f + (size_t)done * a->out_ch, n - done);
    if (w <= 0) break;
    done += (uint32_t)w;
  }
  a->written += done;
}

/* Время — от начала ролика, в мкс там, где копится (период 41 708 мкс при 23,976 к/с: в целых мс
   за тысячу кадров набежало бы 0,7 с).
   Картинка: (кадр начала потока + показанный кадр) · период + сколько прошло с его выхода (не
   больше периода; паузу slot_ms обходит). Слот выходит по началу выходного кадра и виден с него же
   (vp_pace). */
static int64_t video_ms(asess_t* a, uint32_t now)
{
  uint32_t slots = 0;
  if (vlink_read(R_SLOTS, &slots, 1) != ZM_OK) return -1;
  if (slots != a->slots_seen) { a->slots_seen = slots; a->slot_ms = now; }
  if (slots == 0) return -1;
  core->mutex_lock(V.lock);
  const uint32_t per = V.status.us_frame ? V.status.us_frame : 40000u;
  const uint32_t base = V.status.base_frame;
  core->mutex_unlock(V.lock);
  uint64_t since = (uint64_t)(now - a->slot_ms) * 1000u;
  if (since > per) since = per;
  return (int64_t)(((uint64_t)(base + slots - 1) * per + since) / 1000u);
}

/* Звук: начало потока + кадры PCM, отзвучавшие с начала потока (выдано на ЦАП − очередь +
   выброшенное − тишина), с выбросом, который ещё впереди. Начало потока у PCM — байты звука в
   файле до него (индекс перемотки плагина) через nAvgBytesPerSec: точно. У MP3 nAvgBytesPerSec
   заголовка — лишь средний битрейт, у VBR неточный или 0: после перемотки звук считался впереди
   или позади на десятки секунд (28.10.2026 на плате — подстройка «largest 264205 ms», ошибка росла
   с каждой перемоткой вперёд). Поэтому у MP3 начало — время кадра, с которого идёт поток: плагин
   начинает поток с записи перемотки, и звук за ней в файле — того же места ролика. */
static int64_t audio_ms(const asess_t* a)
{
  core->mutex_lock(V.lock);
  const uint32_t base = V.status.audio_base;
  const uint32_t base_frame = V.status.base_frame;
  const uint32_t per = V.status.us_frame ? V.status.us_frame : 40000u;
  core->mutex_unlock(V.lock);
  int64_t base_ms;
  if (a->tag == 1) base_ms = a->avg_bytes ? (int64_t)((uint64_t)base * 1000u / a->avg_bytes) : 0;
  else base_ms = (int64_t)((uint64_t)base_frame * per / 1000u);
  const uint64_t q = (uint64_t)core->pcm_queued_ms() * a->out_rate / 1000u;
  const int64_t played = (int64_t)a->written - (int64_t)q + a->content_shift + gap_shift + a->fix_drop;
  return base_ms + played * 1000 / (int64_t)a->out_rate;
}

static void step(asess_t* a)
{
  const uint32_t now = core->millis();
  uint32_t v = 0;
  if (vlink_read(R_AU_WP, &v, 1) != ZM_OK) return;
  const uint8_t epoch = (uint8_t)(v >> 24);
  if (epoch != a->epoch) {                   /* новый поток */
    restart(a);
    a->epoch = epoch;
    a->rp = 0;
    vlink_wr(R_AU_WP, 0);
    set_ctl(a);
    return;
  }
  pull(a, v & 2047u);
  if (!a->enabled) { ring_reset(); return; }

  core->mutex_lock(V.lock);
  const int paused = (v & AU_PAUSE) || (V.status.flags & (VS_PAUSED | VS_SEEKING)) != 0;
  core->mutex_unlock(V.lock);
  if (paused != a->paused && a->open) core->pcm_pause(paused);
  if (paused && !a->paused) a->pause_ms = now;
  if (a->paused && !paused) { a->align = 1; a->slot_ms += now - a->pause_ms; }   /* кадр стоял */
  a->paused = paused;
  if (paused) return;

  const int64_t tv = video_ms(a, now);
  if (!a->started) {
    /* начать, когда первый кадр показан и звука хватает */
    if (tv < 0 || !have_audio_ms(a, START_MS)) return;
    if (a->tag == 1 && !a->open) {
      zm_pcm_format_t fmt = {a->out_rate, a->out_ch};
      if (core->pcm_open(&fmt) != ZM_OK) { V.audio_state = 3; a->enabled = 0; return; }
      a->open = 1;
    }
    a->started = 1;
    a->align = 1;
    a->last_sync_ms = now - 100;             /* сверка — сразу, как пойдёт звук */
    V.audio_state = 2;
  }

  /* выдать на ЦАП, пока очередь короткая */
  const uint32_t queue_ms = QUEUE_MS + (a->out_rate ? DMA_FRAMES * 1000u / a->out_rate : 0);
  while (V.running && V.session_live && (!a->open || core->pcm_queued_ms() < queue_ms)) {
    uint32_t n = decode(a, FRAMES);
    if (!n) break;
    if (!a->open) {                          /* MP3: частота известна после первых кадров */
      zm_pcm_format_t fmt = {a->out_rate, a->out_ch};
      if (!a->out_rate || core->pcm_open(&fmt) != ZM_OK) { V.audio_state = 3; a->enabled = 0; return; }
      a->open = 1;
    }
    if (a->fix_drop) {                       /* звук позади: входящее — мимо ЦАП */
      const uint32_t d = n < a->fix_drop ? n : a->fix_drop;
      a->fix_drop -= d;
      a->content_shift += d;
      if (d == n) continue;
      memmove(frames_buf, frames_buf + (size_t)d * a->out_ch, (size_t)(n - d) * a->out_ch * sizeof(float));
      n -= d;
    }
    write_frames(a, frames_buf, n);
  }

  /* синхронность с картинкой — раз в 100 мс (после начала и паузы — точнее) */
  if (a->open && a->out_rate && a->written && tv >= 0 && now - a->last_sync_ms >= 100) {
    a->last_sync_ms = now;
    const int64_t d = audio_ms(a) - tv;
    const int64_t lim = a->align ? ALIGN_MS : SYNC_MS;
    a->align = 0;
#ifdef VIDEO_AUDIO_TRACE
    core->log("sync: d %d tv %d written %u q %u shift %d gaps %d drop %u", (int)d, (int)tv, (unsigned)a->written,
              (unsigned)core->pcm_queued_ms(), (int)a->content_shift, (int)gap_shift, (unsigned)a->fix_drop);
#endif
    /* подстройка — обычное дело (звук приходит пачками): строкой на каждую журнал забивался целиком
       и вытеснял всё остальное — теперь сводка раз в 5 с */
    if (d > lim || d < -lim) {
      const int32_t ad = (int32_t)(d > 0 ? d : -d);
      if (ad > a->fix_max) a->fix_max = ad;
      a->fix_n++;
    }
    if (a->fix_n && now - a->fix_log_ms >= 5000) {
      core->log("video: audio sync adjusted %u times in 5 s, largest %d ms", (unsigned)a->fix_n, (int)a->fix_max);
      a->fix_log_ms = now;
      a->fix_n = 0;
      a->fix_max = 0;
    }
    if (d > lim) {                           /* звук впереди: тишина */
      uint32_t ms = (uint32_t)(d > FIX_MAX_MS ? FIX_MAX_MS : d);
      uint32_t n = ms * a->out_rate / 1000u;
      memset(frames_buf, 0, sizeof frames_buf);
      /* в счёт — только тишина, которую ЦАП принял: pcm_write ждёт места не дольше 0,5 с, и
         непринятая прежде навсегда сдвигала время звука назад */
      const uint64_t before = a->written;
      while (n) {
        const uint32_t k = n > FRAMES ? FRAMES : n;
        const uint64_t was = a->written;
        write_frames(a, frames_buf, k);
        if (a->written - was < k) break;
        n -= k;
      }
      a->content_shift -= (int64_t)(a->written - before);
    } else if (d < -lim) {
      /* звук позади: выбросить — но только из принятого сверх START_MS. Звук, лежащий в файле
         позже своих кадров дальше, чем разбор опережает показ (1–2 кадра), догнать нечем: он
         приходит в реальном времени, и выброс оставил бы ЦАП пустым. Такой звук играет с
         опозданием, но без дыр. */
      const uint32_t ms = (uint32_t)(-d > FIX_MAX_MS ? FIX_MAX_MS : -d);
      uint32_t want = ms * a->out_rate / 1000u;
      const uint32_t have = buffered_frames(a), keep = START_MS * a->out_rate / 1000u;
      const uint32_t can = have > keep ? have - keep : 0;
      if (want > can) want = can;
      a->fix_drop += want;
    }
  }
}

static void session(void)
{
  asess_t a;
  memset(&a, 0, sizeof a);
  a.session = V.session_id;
  a.fmt_serial = V.audio_fmt.serial - 1;
  /* эпоха ядра ещё не подтверждена: первый шаг подтвердит её (плагин мог сбросить поток раньше) */
  uint32_t v = 0;
  if (vlink_read(R_AU_WP, &v, 1) == ZM_OK) a.epoch = (uint8_t)~(v >> 24);
  while (V.running && V.session_live && V.session_id == a.session) {
    if (a.fmt_serial != V.audio_fmt.serial) configure(&a);
    step(&a);
    core->delay(STEP_MS);
  }
  if (a.open) core->pcm_close();
  mp3_close();
  V.audio_state = 0;
}

void audio_task(void* arg)
{
  (void)arg;
  core = V.core;
  ring_size = 2u << 20;
  while (!(ring = (uint8_t*)core->alloc(ring_size, ZM_MEM_PSRAM)) && ring_size > (128u << 10)) ring_size >>= 1;
  if (ring) core->log("video: audio ring %u KB", (unsigned)(ring_size >> 10));
  else core->log("video: no memory for the audio ring");
  while (V.running && ring) {
    while (V.running && !V.session_live) core->delay(20);
    if (V.running) session();
  }
  mp3_unload();
  audio_done = 1;
}
