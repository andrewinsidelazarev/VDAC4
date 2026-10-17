/* PLAYER.ELF — медиапроигрыватель VDAC4, приложение ESP (zm_app.h).
 *
 * Команды Z80 AUDIO_* (#30..#3C) — те же, что у плеера, жившего в прошивке
 * (Docs/VDAC4.md, «Команды звука»), поэтому плагин VDAC4 Media Player с ним
 * работает как прежде. Тон и тест линий ЦАП (#34, #37) приложение не берёт:
 * их делает ядро.
 *
 * Байты потока (MP3, WAV, AAC, Ogg Vorbis) приходят кадрами AUDIO_DATA в кольцо в
 * PSRAM; файл модуля (MOD/XM/S3M/IT), MIDI или музыка OPL — целиком в буфер.
 * Декодер — модуль ESP (MP3.ELF, AAC.ELF, OGG.ELF — поток, по расширению из
 * AUDIO_OPEN; TRACKER.ELF, MIDI.ELF, OPL.ELF — файл целиком, по его содержимому,
 * IMF без подписи — по расширению): своя задача плеера зовёт его render и отдаёт
 * PCM ядру (pcm_write). Ядро пересчитывает частоту, применяет громкость и
 * выводит в ЦАП.
 *
 * MIDI.ELF играет по банку звуков MIDI.SF2 — ресурсу ядра (до 6 МБ PSRAM),
 * OPL.ELF у OPL4 — по ПЗУ таблиц волн YRW801.ROM (2 МБ). Файлу, которому с ними
 * не хватает памяти, плеер освобождает ненужный ему ресурс, а по AUDIO_OPEN
 * [#20] — все (плагину не хватило места на ресурс): плагин пришлёт их снова,
 * когда понадобятся. ПЗУ OPL4 нужно не каждому VGM (у VGZ заголовок сжат), поэтому
 * его просит декодер: не нашлось — флаг 16 в AUDIO_STATUS, плагин присылает ПЗУ
 * и открывает файл заново. Набор волн песни MWM («ИМЯ.MWK», до 4 ресурсов у
 * ядра) плагин присылает до открытия; набор другого имени, который попросил
 * декодер, вытесняет прежний, а AUDIO_OPEN [#20] освобождает и его.
 *
 * Обложка и метки MP3 — cover_glue.cpp (AUDIO_COVER, AUDIO_THUMB); название из
 * меток («исполнитель - название») отдаёт AUDIO_INFO, метки по отдельности —
 * AUDIO_TAGS. Обложку, найденную декодером в самом файле (Ogg:
 * METADATA_BLOCK_PICTURE, zm_host_t::cover), сводит тот же код, а флаг 64 в
 * AUDIO_STATUS зовёт плагин за миниатюрой. Окну проигрывателя AUDIO_VIEW отдаёт спектр (view.c) или уровни
 * каналов модуля, позицию и длительность.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "player_cover.h"
#include "radio_list.h"
#include "view.h"
#include "zm_app.h"
#include "zm_source.h"

/* Команды и ответы (include/zifi/protocol.hpp прошивки). */
enum {
  AUDIO_VIEW = 0x2F,                      /* не #3E: там LINK_PROBE ядра */
  AUDIO_OPEN = 0x30, AUDIO_DATA, AUDIO_CLOSE, AUDIO_STATUS, AUDIO_TONE, AUDIO_VOLUME,
  AUDIO_PAUSE, AUDIO_PROBE, AUDIO_SEEK, AUDIO_FLUSH, AUDIO_INFO, AUDIO_COVER, AUDIO_THUMB,
  AUDIO_PICTURE, AUDIO_LINK_PROBE, AUDIO_TAGS,
};
#define RESPONSE(command) ((uint8_t)((command) | 0x80))

/* Состояния и форматы записи AUDIO_STATUS. */
enum { ST_IDLE = 0, ST_PLAYING = 1, ST_DRAINING = 2, ST_OFFLINE = 4, ST_LOADING = 5 };
enum { FMT_NONE = 0, FMT_MP3 = 1, FMT_MOD = 2, FMT_XM = 3, FMT_S3M = 4, FMT_IT = 5, FMT_MIDI = 6,
       FMT_OPL = 7, FMT_SAA = 8, FMT_TSFM = 9 };
#define OPEN_MODULE 0x80                   /* AUDIO_OPEN: файл целиком */
#define OPEN_MIDI 0x40                     /* ... и это MIDI: банк не освобождать */
#define OPEN_RELEASE 0x20                  /* AUDIO_OPEN [#20]: только освободить ресурсы */
#define SKIP_MS 1500                       /* модуль не загрузился: столько «играет» до конца */
#define SKIP_TITLE "Cannot play this file, skipping"   /* название на это время */
#define MIDI_BANK "MIDI.SF2"
#define OPL4_ROM "YRW801.ROM"
#define STATUS_SIZE 32

#define RING_BYTES (128u * 1024u)         /* очередь потока с карты: 8 с при 128 кбит/с, 3 с при 320 */
#define START_BUFFER (96u * 1024u)         /* до начала звука */
#define REFILL_BUFFER (48u * 1024u)        /* после опустошения очереди */
#define MODULE_MAX (6u * 1024u * 1024u)   /* отсчёты XM — в буфере файла: хватает PSRAM */
#define STREAM_WAIT_MS 2000                /* IT потоком: кольцо полно — ждать декодер */
#define REST_EVERY_MS 100                  /* декодер подряд дольше — 1 мс ядра другим (сторож) */
#define ROOM_PIECE (256u * 1024u)          /* make_room: PSRAM кусками (как берёт libxmp) */
#define ROOM_PIECES 48
#define RENDER_FRAMES ZM_RENDER_MAX_FRAMES
/* Клод: начало файла — сначала START_MS звука, потом вывод (1.20). Пока вывод
 * закрыт, прошивка сбрасывает журнал во flash (кэш выключен — оба ядра стоят),
 * и первый render нового трека иногда стоил 2-2,4 бюджета: fm.tfc запинался в
 * начале. С запасом в очереди такая остановка лишь чуть задерживает старт.
 * Только у файлов, лежащих на ESP целиком (модули, TSFM, OPL, MIDI, SAA): поток
 * приходит частями и сам набирает очередь. Буфер — PSRAM на время старта. */
#define START_MS 200
#define TASK_PRIORITY 5                    /* на ступень ниже задачи звука ядра */
#define DECODE_STACK 12288                 /* задача декодера: stb_vorbis открывает поток на стеке */
#define WORKER_PRIORITY 5                  /* помощник декодеров на ядре 1: ниже задачи обмена (20) */
#define CPU_HZ 240000000u                  /* частота ESP32-S3: бюджет кадра — CPU_HZ / частота звука */
/* Интернет-радио (плейлист M3U). */
#define RADIO_TYPE "M3U"                   /* расширение плейлиста в AUDIO_OPEN */
#define RADIO_STATIONS 64
#define RADIO_TIMEOUT_MS 8000              /* соединение и ответ станции */
#define RADIO_READ_MS 300                  /* одно чтение сети: столько ждать стопа и смены */
#define RADIO_SILENCE_MS 15000             /* без байтов дольше — переподключиться */
#define RADIO_RETRY_MS 3000                /* после обрыва — пауза перед новой попыткой */
#define RADIO_REJECT_MS 15000              /* поток не играть (Opus, плейлист, нет декодера): не скоро */
#define FLAC_RING_BYTES (256u * 1024u)
#define RADIO_RING_BYTES (512u * 1024u)    /* очередь радио: 32 с при 128 кбит/с, 13 с при 320;
                                              степень двойки — счётчики head/tail идут по кругу 2^32 */
#define RADIO_START_MS 4000                /* до звука — столько звука в очереди (по битрейту) */
#define RADIO_REFILL_MS 6000               /* после опустошения — столько: пауза одна, а не дробь */
#define RADIO_LOW_MS 100                   /* в очереди ядра меньше — опустошение, дальше тишина */
#define RADIO_GUESS_KBPS 192               /* битрейт, пока его не назвали ни станция, ни декодер */
#define RADIO_CHUNK 2048
#define RADIO_TASK_STACK 6144
#define RADIO_PRIORITY 4                   /* ядро 1: ниже задачи обмена и помощника */
#define ST_F_TAGS 32                       /* AUDIO_STATUS: метки сменились — перечитать (радио,
                                              название от декодера потока) */
#define ST_F_COVER 64                      /* AUDIO_STATUS: декодер нашёл обложку — спросить
                                              миниатюру (AUDIO_THUMB снимает флаг) */
#define ST_F_RATE_HIGH 128                 /* AUDIO_STATUS: частота больше 65535 Гц (88,2 и 96 кГц):
                                              поле LE16 — младшие 16 бит, к нему +65536 */
#define RADIO_LIST_MS 10000                /* список станций вместо столбиков: столько после листания */
#define RADIO_LIST_ROWS 7                  /* строки столбиков и строка чисел окна Z80 */
#define RADIO_LIST_WIDTH 63                /* знаков в строке: столбики — столбцы 2..64 */
#define WORKER_STACK 4096
#define VIEW_PERIOD_MS 50                  /* уровни каналов — 20 раз в секунду */
#define VIEW_CHANNELS 32
enum { VIEW_NONE = 0, VIEW_SPECTRUM = 1, VIEW_CHANNEL_LEVELS = 2, VIEW_STATIONS = 3 };

typedef struct {
  /* Очередь потока: пишет команда AUDIO_DATA (ядро обмена) или задача радио, читает
   * декодер. У радио — своё кольцо, побольше (ring_resize). */
  uint8_t* ring;                          /* ring_small или ring_big */
  uint8_t* ring_small;                    /* RING_BYTES: живёт до выгрузки */
  uint8_t* ring_big;                      /* радио 512 КиБ, FLAC 256 КиБ; до остановки потока */
  volatile uint32_t ring_size;            /* размер ring */
  volatile uint32_t head;                 /* пишет производитель */
  volatile uint32_t tail;                 /* пишет потребитель */

  /* Файл модуля. */
  uint8_t* file;
  uint32_t file_size;
  uint32_t file_written;
  char file_type[4];                      /* расширение из AUDIO_OPEN ("IMF"), host.file_type */

  /* Декодер. */
  void* module;                           /* модуль ESP */
  const zm_source_t* source;
  void* instance;
  char module_name[13];
  void* lock;                             /* декодер: задача против команд */
  void* wake;

  /* Состояние для Z80. */
  volatile uint8_t state;
  volatile uint8_t format;
  volatile int stream_open;               /* поток открыт: AUDIO_DATA пишет в кольцо */
  volatile int module_loading;            /* файл модуля принимается */
  volatile int load_pending;              /* файл модуля принят: открыть декодер */
  volatile int stream_module;             /* файл целиком идёт потоком (IT): декодер читает кольцо */
  volatile int stream_done;               /* декодер файл потока взял или отказал: остаток — прочь */
  volatile int load_abort;                /* стоп посреди приёма: декодеру байтов не ждать */
  volatile int ready_pending;             /* декодер открыт (или отказал), ждём конца файла */
  int load_ok;                            /* декодер файла открыт */
  int room_made;                          /* make_room уже освобождал ресурсы для этого файла */
  volatile int closed;                    /* поток закрыт с доигрыванием */
  volatile int finished;
  volatile int decoded;
  volatile int buffering;
  volatile int paused;
  volatile int stop;
  volatile int task_done;
  volatile int wants_rom;                 /* декодер искал ПЗУ OPL4, а его нет */
  char kit[13];                           /* набор волн MWM, который брал декодер */
  volatile uint32_t generation;           /* +1 на стоп и перемотку: недописанное — прочь */
  uint32_t buffer_target;
  int starving;
  uint16_t underruns;
  uint32_t rate;
  uint8_t channels;
  uint64_t written;                       /* кадров отдано ядру */
  uint64_t rendered;                      /* кадров дал декодер */
  uint32_t dropped;                       /* кадров не взял вывод (не пауза, не перемотка) */
  uint32_t open_rate;                     /* формат, с которым открыт вывод ядра (0 — закрыт) */
  float* start_buf;                       /* начало файла до открытия вывода (START_MS) */
  uint32_t start_cap;                     /* кадров вмещает start_buf */
  uint32_t start_fill;                    /* кадров в start_buf */
  uint32_t start_generation;              /* поток, чьё начало в start_buf */
  uint32_t start_done;                    /* поток, у которого начало уже отдано (+1; 0 — нет) */
  uint8_t open_channels;
  uint32_t base_ms;                       /* время места, с которого шёл звук (перемотка) */
  uint32_t skip_at;                       /* модуль не загрузился: «конец» в эту миллисекунду (0 — нет) */
  uint32_t rest_ms;                       /* когда задача декодера последний раз отдавала ядро */
  uint64_t render_cycles;                 /* такты render за песню (с вытеснением другими задачами) */
  uint32_t render_worst;                  /* худший render: тактов на кадр */
  float* frames;
  char info_track[16];                    /* дополнительные метки декодера */
  zm_info_t info;                         /* снимок для Z80, пишет задача под lock */
  uint32_t stream_size;                   /* длина MP3 (без длинной метки) — для длительности */
  uint32_t view_ms;                       /* когда пересчитаны уровни каналов */
  volatile uint8_t level_count;           /* уровни каналов модуля, 0..VIEW_LEVELS */
  uint8_t levels[VIEW_CHANNELS];

  /* Помощник декодеров на ядре 1 (host.worker_start): одна работа за раз. */
  void* worker_go;                        /* работа есть (или пора выйти) */
  void* worker_done;                      /* работа кончилась */
  void (*worker_fn)(void* arg);
  void* worker_arg;
  int worker_busy;                        /* работа отдана и не дождана; трогает задача декодера */
  volatile int worker_quit;
  volatile int worker_exited;

  /* Интернет-радио (плейлист M3U): см. «интернет-радио» ниже. */
  radio_station_t* stations;              /* PSRAM, RADIO_STATIONS */
  volatile uint16_t station_count;
  volatile uint16_t station;              /* текущая станция */
  volatile int radio;                     /* режим радио */
  volatile uint32_t radio_session;        /* +1 на смену станции, стоп и новый плейлист */
  volatile uint32_t radio_restart;        /* сеанс, для которого задаче декодера сбросить кольцо и декодер */
  volatile uint32_t radio_lost;           /* сеанс, у которого оборвалось соединение: набора
                                             очереди не ждать — играть, что пришло */
  volatile int tags_changed;              /* метки сменились: флаг ST_F_TAGS (радио и поток) */
  volatile int cover_found;               /* декодер отдал обложку: флаг ST_F_COVER */
  volatile int radio_quit;
  volatile int radio_exited;
  int radio_task_started;
  void* radio_lock;                       /* тексты станции ниже */
  void* radio_wake;
  char radio_name[64];                    /* icy-name */
  char radio_about[64];                   /* icy-description, без него — icy-genre */
  char radio_song[128];                   /* StreamTitle (байты как пришли: UTF-8 или CP1251) */
  char radio_state[48];                   /* «Connecting...» и т. п., ASCII */
  uint16_t radio_kbps;                    /* icy-br */
  char radio_decoder[13];                 /* декодер потока станции ("AAC.ELF"): ставит задача
                                             радио перед radio_restart, берёт задача декодера */
  volatile uint32_t radio_failed;         /* сеанс, для которого декодер не загрузился */
  uint32_t info_hash;                     /* название и исполнитель от декодера: сменились — метки */
  volatile uint32_t radio_list_ms;        /* когда листали станции (0 — не листали) */
} player_t;

static const zm_core_t* core;
static zm_app_t app;
static zm_host_t host;
static player_t* P;

/* Called under P->lock, alongside the ordinary info snapshot. Older modules
 * have a smaller source table and need no rebuild for this optional callback. */
static void source_info(zm_info_t* info) {
  P->source->info(P->instance, info);
  const char* track = NULL;
  if (P->source->size >= offsetof(zm_source_t, tag) + sizeof(P->source->tag) && P->source->tag)
    track = P->source->tag(P->instance, "TRACKNUMBER");
  snprintf(P->info_track, sizeof(P->info_track), "%s", track ? track : "");
}

/* --- кольцо ----------------------------------------------------------------------- */

static uint32_t ring_available(void) {
  return __atomic_load_n(&P->head, __ATOMIC_ACQUIRE) - __atomic_load_n(&P->tail, __ATOMIC_ACQUIRE);
}

static uint32_t ring_free(void) { return P->ring_size - ring_available(); }

static void ring_write(const uint8_t* data, uint32_t count) {
  const uint32_t size = P->ring_size;
  uint32_t head = P->head;
  for (uint32_t done = 0; done < count;) {
    const uint32_t at = head % size;
    uint32_t part = size - at;
    if (part > count - done) {
      part = count - done;
    }
    memcpy(P->ring + at, data + done, part);
    done += part;
    head += part;
  }
  __atomic_store_n(&P->head, head, __ATOMIC_RELEASE);
}

/* Стоп посреди приёма (load_abort): декодеру — ничего, пусть откажет сразу. */
static size_t host_input_read(void* destination, size_t count) {
  const uint32_t available = P->load_abort ? 0 : ring_available();
  if (count > available) {
    count = available;
  }
  const uint32_t size = P->ring_size;
  uint32_t tail = P->tail;
  uint8_t* out = (uint8_t*)destination;
  for (size_t done = 0; done < count;) {
    const uint32_t at = tail % size;
    size_t part = size - at;
    if (part > count - done) {
      part = count - done;
    }
    memcpy(out + done, P->ring + at, part);
    done += part;
    tail += (uint32_t)part;
  }
  __atomic_store_n(&P->tail, tail, __ATOMIC_RELEASE);
  return count;
}

static size_t host_input_available(void) { return P->load_abort ? 0 : ring_available(); }

/* Поток MP3 кончился — AUDIO_CLOSE с доигрыванием; файл целиком потоком — пришёл весь. */
static int host_input_ended(void) { return P->stream_module ? !P->module_loading : P->closed; }

/* Файл целиком потоком: ждать count байтов в кольце (или конца файла, стопа,
 * timeout_ms). Возвращает, сколько есть. */
static size_t host_input_wait(size_t count, uint32_t timeout_ms) {
  const uint32_t start = core->millis();
  for (;;) {
    const size_t available = P->load_abort ? 0 : ring_available();
    if (available >= count || P->load_abort || P->stop || host_input_ended() ||
        core->millis() - start >= timeout_ms) {
      return available;
    }
    core->delay(1);
  }
}

/* Можно ли занять bytes байтов PSRAM кусками до ROOM_PIECE (так берёт libxmp:
 * блок на сэмпл и мелочь). Куски сразу отдаются. */
static int psram_fits(size_t bytes) {
  void* pieces[ROOM_PIECES];
  size_t step = ROOM_PIECE;
  if (bytes > step * ROOM_PIECES) {
    step = (bytes + ROOM_PIECES - 1) / ROOM_PIECES;
  }
  int count = 0;
  size_t got = 0;
  while (got < bytes && count < ROOM_PIECES) {
    const size_t want = bytes - got < step ? bytes - got : step;
    pieces[count] = core->alloc(want, ZM_MEM_PSRAM);
    if (pieces[count] == NULL) {
      break;
    }
    got += want;
    ++count;
  }
  while (count > 0) {
    core->free(pieces[--count]);
  }
  return got >= bytes;
}

/* Уступка ядра декодеру (host.rest, ABI источников 7): как в цикле декодера — раз в
 * REST_EVERY_MS 1 мс другим задачам. Долгий разбор файла в open (MIDI с тысячами дорожек,
 * большой банк) или долгая перемотка иначе держали бы ядро 0, и сторож задач перезапустил
 * бы ESP. */
static void host_rest(void) {
  if (core->millis() - P->rest_ms >= REST_EVERY_MS) {
    core->delay(1);
    P->rest_ms = core->millis();
  }
}

/* Место под файл, идущий потоком: не хватает — ресурсы, которые трекеру не
 * нужны (банк MIDI, ПЗУ OPL4, набор MWM), прочь; плагин пришлёт их, когда
 * понадобятся. */
static int host_make_room(size_t bytes) {
  if (psram_fits(bytes)) {
    return 1;
  }
  if (core->resource_release == NULL || P->room_made) {
    return 0;
  }
  P->room_made = 1;
  core->log("player: %u bytes do not fit with the resources: " MIDI_BANK " and " OPL4_ROM
            " released", (unsigned)bytes);
  core->resource_release(MIDI_BANK);
  core->resource_release(OPL4_ROM);
  if (P->kit[0] != 0) {
    core->resource_release(P->kit);
    P->kit[0] = 0;
  }
  return psram_fits(bytes);
}

static void ring_reset(void) {
  __atomic_store_n(&P->tail, __atomic_load_n(&P->head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
}

/* Кольцо радио 512 КиБ, FLAC 256 КиБ (capacity): запас на задержки; нет столько
 * PSRAM — обычное. Остальным — обычное, а кольцо радио отдаётся (память нужна
 * модулям и банку MIDI). Под P->lock (декодер читает кольцо под ним) и radio_lock
 * (задача радио пишет под ним и только в своём сеансе): прежнее кольцо после этого
 * никто не тронет. */
static void ring_resize(uint32_t capacity) {
  /* Callers stop the previous stream (and free its large ring) before opening
   * a different one. Radio and files use separate requested capacities. */
  const int large = capacity > RING_BYTES;
  if (large && P->ring_big == NULL) {
    P->ring_big = (uint8_t*)core->alloc(capacity, ZM_MEM_PSRAM);
    if (P->ring_big == NULL) {
      core->log("player: no PSRAM for the %u KB %s buffer: %u KB",
                (unsigned)(capacity / 1024u),
                strcmp(P->file_type, "FLA") == 0 ? "FLAC" : "radio",
                (unsigned)(RING_BYTES / 1024u));
    }
  }
  uint8_t* const ring = large && P->ring_big != NULL ? P->ring_big : P->ring_small;
  if (P->radio_lock != NULL) {
    core->mutex_lock(P->radio_lock);
  }
  P->ring = ring;
  P->ring_size = ring == P->ring_small ? RING_BYTES : capacity;
  ring_reset();
  if (P->radio_lock != NULL) {
    core->mutex_unlock(P->radio_lock);
  }
  if (!large && P->ring_big != NULL) {
    core->free(P->ring_big);
    P->ring_big = NULL;
  }
}

/* FLAC starts as soon as the first complete frame arrives. A low PCM queue
 * requires refill; its watermark follows the allocated capacity. */
static uint32_t file_buffer_target(int start) {
  if (strcmp(P->file_type, "FLA") == 0) {
    return start ? 0 : P->ring_size * 3u / 8u;
  }
  return start ? START_BUFFER : REFILL_BUFFER;
}

/* --- декодер ----------------------------------------------------------------------- */

/* Обложка из самого файла (Ogg: METADATA_BLOCK_PICTURE) — на миниатюру, как у
 * MP3; у радио окна под обложку нет. */
static void host_cover(const uint8_t* image, size_t size) {
  if (!P->radio && image != NULL && cover_image(image, size)) {
    P->cover_found = 1;
  }
}

/* Набор волн MWM: имя на «.MWK». */
static int kit_name(const char* name) {
  const size_t length = name != NULL ? strnlen(name, 13) : 0;
  return length > 4 && length < 13 && strcmp(name + length - 4, ".MWK") == 0;
}

/* Ресурс для декодера: ПЗУ OPL4, которого нет, — флаг для плагина; набор MWM
 * другого имени — прежний прочь (у ядра всего 4 места). */
static int host_resource(const char* name, const uint8_t** data, size_t* size) {
  if (kit_name(name) && strcmp(name, P->kit) != 0) {
    if (P->kit[0] != 0 && core->resource_release != NULL) {
      core->resource_release(P->kit);
    }
    strncpy(P->kit, name, sizeof(P->kit) - 1);
  }
  const int result = core->resource != NULL ? core->resource(name, data, size) : ZM_NOT_FOUND;
  if (result == ZM_NOT_FOUND && name != NULL && strcmp(name, OPL4_ROM) == 0) {
    P->wants_rom = 1;
  }
  return result;
}

/* --- помощник декодеров на ядре 1 ------------------------------------------------- */

static void worker_task(void* arg) {
  (void)arg;
  while (!P->worker_quit) {
    if (core->signal_wait(P->worker_go, 1000) != ZM_OK) {
      continue;
    }
    void (*fn)(void*) = P->worker_fn;
    if (fn != NULL && !P->worker_quit) {
      fn(P->worker_arg);
    }
    core->signal_give(P->worker_done);
  }
  P->worker_exited = 1;
}

static int host_worker_start(void (*fn)(void* arg), void* arg) {
  if (fn == NULL || P->worker_busy) {
    return ZM_BUSY;
  }
  P->worker_fn = fn;
  P->worker_arg = arg;
  P->worker_busy = 1;
  core->signal_give(P->worker_go);
  return ZM_OK;
}

static void host_worker_wait(void) {
  while (P->worker_busy) {
    if (core->signal_wait(P->worker_done, 1000) == ZM_OK || P->worker_exited) {
      P->worker_busy = 0;
    }
  }
}

static void close_decoder(void) {
  /* Работа помощника — код и память декодера: дождаться, прежде чем их убрать. */
  host_worker_wait();
  if (P->instance != NULL && P->source != NULL) {
    P->source->close(P->instance);
  }
  host_worker_wait();
  P->instance = NULL;
  if (P->module != NULL) {
    core->module_close(P->module);
  }
  P->module = NULL;
  P->source = NULL;
  P->module_name[0] = 0;
}

/* Загрузить модуль-декодер (если другой — выгрузить прежний). */
static int load_decoder(const char* name) {
  if (P->module != NULL && strcmp(P->module_name, name) == 0) {
    return ZM_OK;
  }
  close_decoder();
  const zm_source_t* source = NULL;
  int entry = 0;
  const int loaded = core->module_open(name, &host, &source, &P->module, &entry);
  if (loaded != ZM_OK) {
    core->log("player: decoder %s not loaded (%d)", name, loaded);
    return loaded;
  }
  if (entry != ZM_SOURCE_ABI_MAJOR || source == NULL || source->abi_major != ZM_SOURCE_ABI_MAJOR) {
    core->log("player: decoder %s interface %d, not %d", name, entry, ZM_SOURCE_ABI_MAJOR);
    core->module_close(P->module);
    P->module = NULL;
    return ZM_ERROR;
  }
  P->source = source;
  strncpy(P->module_name, name, sizeof(P->module_name) - 1);
  return ZM_OK;
}

/* Расширение музыки OPL (у IMF подписи нет — только по нему). */
static int opl_type(const char* type) {
  return strcmp(type, "VGM") == 0 || strcmp(type, "VGZ") == 0 || strcmp(type, "DRO") == 0 ||
         strcmp(type, "IMF") == 0 || strcmp(type, "WLF") == 0 || strcmp(type, "MWM") == 0 ||
         strcmp(type, "MFM") == 0;
}

/* Декодер файла целиком — по его началу (IMF — по расширению). */
static const char* whole_decoder(const uint8_t* file, uint32_t size) {
  if (strcmp(P->file_type, "AS0") == 0 ||
      strcmp(P->file_type, "ASC") == 0 ||
      strcmp(P->file_type, "FTC") == 0 ||
      strcmp(P->file_type, "GTR") == 0 ||
      strcmp(P->file_type, "PSC") == 0 ||
      strcmp(P->file_type, "PSM") == 0 ||
      strcmp(P->file_type, "PT1") == 0 ||
      strcmp(P->file_type, "PT2") == 0 ||
      strcmp(P->file_type, "PT3") == 0 ||
      strcmp(P->file_type, "SQT") == 0 ||
      strcmp(P->file_type, "ST3") == 0 ||
      strcmp(P->file_type, "STC") == 0 ||
      strcmp(P->file_type, "STP") == 0 ||
      strcmp(P->file_type, "TS") == 0 ||
      strcmp(P->file_type, "TFC") == 0 ||
      /* С PLAYER 1.17: TurboFM Dump и Multitrack Container ZXTune (TSFM.ELF 1.1). */
      strcmp(P->file_type, "TFD") == 0 ||
      strcmp(P->file_type, "MTC") == 0 ||
      (size >= 4 && (memcmp(file, "TFMD", 4) == 0 || memcmp(file, "MTC1", 4) == 0))) return "TSFM.ELF";
  if ((size >= 18 && memcmp(file + 10, "ETracker", 8) == 0) ||
      strcmp(P->file_type, "SAA") == 0 || strcmp(P->file_type, "COP") == 0) {
    return "SAA.ELF";
  }
  if (size >= 4 && memcmp(file, "MThd", 4) == 0) {
    return "MIDI.ELF";
  }
  if (size >= 12 && memcmp(file, "RIFF", 4) == 0 && memcmp(file + 8, "RMID", 4) == 0) {
    return "MIDI.ELF";
  }
  if ((size >= 4 && memcmp(file, "Vgm ", 4) == 0) || (size >= 2 && file[0] == 0x1F && file[1] == 0x8B) ||
      (size >= 8 && memcmp(file, "DBRAWOPL", 8) == 0) || (size >= 4 && memcmp(file, "MBMS", 4) == 0) ||
      opl_type(P->file_type)) {
    return "OPL.ELF";
  }
  return "TRACKER.ELF";
}

/* Декодер потока: AAC — AAC.ELF, OGG и OGA — OGG.ELF, остальное (MP3, WAV) — MP3.ELF. */
static const char* stream_decoder(const char* type) {
  if (strcmp(type, "AAC") == 0) {
    return "AAC.ELF";
  }
  if (strcmp(type, "OGG") == 0 || strcmp(type, "OGA") == 0) {
    return "OGG.ELF";
  }
  if (strcmp(type, "FLA") == 0) return "FLAC.ELF";
  return "MP3.ELF";
}

static uint8_t format_code(const char* format) {
  if (strncmp(format, "MP3", 3) == 0) return FMT_MP3;
  if (strncmp(format, "WAV", 3) == 0) return FMT_MP3;    /* поток: для Z80 — как MP3 */
  if (strncmp(format, "FLAC", 4) == 0) return FMT_MP3;
  if (strncmp(format, "AAC", 3) == 0) return FMT_MP3;
  if (strncmp(format, "HE-AAC", 6) == 0) return FMT_MP3;
  if (strncmp(format, "Vorbis", 6) == 0) return FMT_MP3;
  if (strncmp(format, "Ogg ", 4) == 0) return FMT_MP3;    /* Ogg с кодеком, которого нет */
  if (strncmp(format, "XM", 2) == 0) return FMT_XM;
  if (strncmp(format, "S3M", 3) == 0) return FMT_S3M;
  if (strncmp(format, "MOD", 3) == 0) return FMT_MOD;
  if (strncmp(format, "IT", 2) == 0) return FMT_IT;
  if (strncmp(format, "MIDI", 4) == 0) return FMT_MIDI;
  if (strncmp(format, "OPL", 3) == 0) return FMT_OPL;
  if (strncmp(format, "SAA", 3) == 0) return FMT_SAA;
  return FMT_NONE;
}

/* Код формата открытого файла. У TSFM.ELF строка формата — только состав
 * микросхем («AY», «TurboSound 2xAY», «TurboSound FM 2xYM2203», «2xAY+2xYM2203»),
 * без имени модуля: код — по загруженному декодеру. */
static uint8_t decoder_format(void) {
  if (strcmp(P->module_name, "TSFM.ELF") == 0) return FMT_TSFM;
  return format_code(P->info.format);
}

/* Итог звука — в журнал ядра (сетевая консоль, журнал наладки). */
/* Такты процессора (ядро, на котором идёт задача декодера). */
static uint32_t cycle_count(void) {
#if defined(__XTENSA__)
  uint32_t count;
  __asm__ __volatile__("rsr %0, ccount" : "=a"(count));
  return count;
#else
  return 0;
#endif
}

static void report(const char* what) {
  if (P->rendered == 0 && P->info.errors == 0) {
    return;
  }
  core->log("player: %s: %s %u ms, rendered %u, written %u, dropped %u, errors %u, underruns %u",
            what, P->info.format, (unsigned)(P->rate ? P->written * 1000 / P->rate : 0),
            (unsigned)P->rendered, (unsigned)P->written, (unsigned)P->dropped,
            (unsigned)P->info.errors, (unsigned)P->underruns);
  /* Нагрузка декодера: такты render на кадр против бюджета кадра (CPU_HZ /
   * частота) — средняя за песню и худший кусок. Больше 100% — не успевает. */
  if (P->rendered != 0 && P->rate != 0 && P->render_cycles != 0) {
    const uint32_t budget = CPU_HZ / P->rate;
    const uint32_t average = (uint32_t)(P->render_cycles / P->rendered);
    core->log("player: load %s %u Hz: %u%% average, %u%% worst (%u and %u cycles per frame of %u)",
              P->info.format, (unsigned)P->rate, (unsigned)(average * 100u / budget),
              (unsigned)(P->render_worst * 100u / budget), (unsigned)average,
              (unsigned)P->render_worst, (unsigned)budget);
  }
}

/* Буфер начала — прочь. */
static void start_release(void) {
  if (P->start_buf != NULL) {
    core->free(P->start_buf);
    P->start_buf = NULL;
  }
  P->start_cap = 0;
  P->start_fill = 0;
}

/* Буфер начала потока generation: есть — 1; нет PSRAM — 0 (играть сразу). */
static int start_take(uint32_t generation) {
  if (P->start_buf != NULL && P->start_generation == generation) {
    return 1;
  }
  start_release();
  const uint32_t cap = P->info.rate * START_MS / 1000 + RENDER_FRAMES;
  P->start_buf = (float*)core->alloc(cap * 2 * sizeof(float), ZM_MEM_PSRAM);
  if (P->start_buf == NULL) {
    P->start_done = generation + 1u;
    return 0;
  }
  P->start_cap = cap;
  P->start_generation = generation;
  return 1;
}

/* Вывод ядра закрыть: в тишине ядро не должно считать, что звук идёт. Под P->lock. */
static void close_output(void) {
  if (P->open_rate != 0) {
    core->pcm_close();
  }
  P->open_rate = 0;
  P->open_channels = 0;
  start_release();
}

static int spectrum_view(void);

/* Вывод — в формате декодера (открыть или переоткрыть, если сменился). */
static void open_output(void) {
  if (P->info.rate != P->open_rate || P->info.channels != P->open_channels) {
    zm_pcm_format_t format = {P->info.rate, P->info.channels};
    core->pcm_open(&format);
    P->open_rate = P->info.rate;
    P->open_channels = P->info.channels;
  }
}

/* Кадры — в вывод. Пока стоит пауза, вывод не берёт кадров (pcm_write ждёт и
 * отдаёт 0): дописать после неё, а не выбросить. Стоп и перемотка меняют
 * поколение — тогда недописанное уже не нужно. */
static void write_frames(const float* data, uint32_t frames, uint32_t generation) {
  uint32_t done = 0;
  while (done < frames && !P->stop && P->generation == generation) {
    const int put = core->pcm_write(data + done * P->channels, frames - done);
    if (put < 0 || (put == 0 && !P->paused)) {
      break;
    }
    core->mutex_lock(P->lock);
    if (P->generation == generation && put > 0) {
      P->written += (uint32_t)put;
      if (spectrum_view()) {
        view_feed(data + done * P->channels, (uint32_t)put, P->channels, P->rate);
      }
    }
    core->mutex_unlock(P->lock);
    done += (uint32_t)put;
  }
  core->mutex_lock(P->lock);
  if (P->generation == generation) {
    P->dropped += frames - done;
  }
  core->mutex_unlock(P->lock);
}

/* Накопленное начало файла — в вывод (открыв его), буфер — прочь. */
static void start_flush(uint32_t generation) {
  const uint32_t fill = P->start_fill;
  P->start_fill = 0;
  P->start_done = generation + 1u;
  if (fill != 0) {
    open_output();
    write_frames(P->start_buf, fill, generation);
  }
  start_release();
}

/* Спектр (MP3, MIDI) или уровни каналов (трекер). */
static int spectrum_view(void) {
  return P->stream_open || P->format == FMT_MP3 || P->format == FMT_MIDI;
}

/* Уровни каналов модуля; спектр публикуется после принятия PCM выводом. */
static void update_levels(void) {
  if (spectrum_view()) return;
  const uint32_t now = core->millis();
  if (now - P->view_ms < VIEW_PERIOD_MS) {
    return;
  }
  P->view_ms = now;
  uint8_t raw[VIEW_CHANNELS];
  int count = 0;
  core->mutex_lock(P->lock);
  if (P->instance != NULL && P->source->levels != NULL) {
    count = P->source->levels(P->instance, raw, VIEW_CHANNELS);
  }
  core->mutex_unlock(P->lock);
  if (count < 0) {
    count = 0;
  }
  for (int c = 0; c < count; ++c) {
    P->levels[c] = (uint8_t)((raw[c] * VIEW_LEVELS + 127) / 255);
  }
  P->level_count = (uint8_t)count;
}

/* --- интернет-радио ------------------------------------------------------------------
 * Файл M3U приходит целиком, как модуль; PLAYER разбирает станции и играет
 * поток станции из того же кольца, что и поток с карты, декодером его кодека:
 * MP3.ELF, AAC.ELF (AAC и HE-AAC в ADTS) или OGG.ELF (Ogg Vorbis). Кодек — по
 * началу потока (OggS, заголовок ADTS или MPEG), не ясно по нему — по
 * Content-Type; Ogg с Opus или FLAC и плейлист в плейлисте — отказ с надписью.
 * Качает поток задача радио на ядре 1 (core->net_*, прошивка 0.1.32): живёт с
 * первого плейлиста до выгрузки, всё её дело привязано к сеансу
 * (radio_session) — смена станции, стоп и новый плейлист его меняют, и
 * прежнее соединение закрывается, даже если задача ждала сеть. Метаданные
 * ICY (название песни) вырезаются из потока; у Ogg их нет — название из
 * комментариев Vorbis каждой песни (декодер); влево-вправо (AUDIO_SEEK ±1) —
 * соседняя станция; обрыв — переподключение.
 *
 * Очередь радио — своё кольцо RADIO_RING_BYTES (ring_resize). Звук начинается,
 * когда в нём RADIO_START_MS звука (байты — по битрейту станции из icy-br, потом
 * декодера); станции сразу шлют 4–10 с звука, и ждать почти не приходится, а
 * запас остаётся на рывки сети. Пустое кольцо при звуке в очереди ядра (до 1,5 с)
 * — ещё не опустошение: поток идёт вровень со звуком, декодер ждёт следующие
 * байты. Опустошение — очередь ядра кончилась (RADIO_LOW_MS): тогда набрать
 * RADIO_REFILL_MS, чтобы пауза была одна. Обрыв соединения при наборе — играть,
 * что пришло. */

static int radio_alive(uint32_t session) {
  return !P->radio_quit && P->radio && P->radio_session == session;
}

/* Состояние и песня станции (NULL — не трогать) — для меток; только в своём сеансе. */
static void radio_set(const char* state, const char* song, uint32_t session) {
  core->mutex_lock(P->radio_lock);
  if (P->radio_session == session) {
    if (state != NULL) {
      snprintf(P->radio_state, sizeof(P->radio_state), "%s", state);
    }
    if (song != NULL) {
      snprintf(P->radio_song, sizeof(P->radio_song), "%s", song);
    }
    P->tags_changed = 1;
  }
  core->mutex_unlock(P->radio_lock);
}

static void radio_wait(uint32_t ms, uint32_t session) {
  for (uint32_t waited = 0; waited < ms && radio_alive(session); waited += 50) {
    core->delay(50);
  }
}

/* Сколько байтов потока — ms миллисекунд звука: битрейт от декодера, нет его — из
 * icy-br станции, нет и его — RADIO_GUESS_KBPS; не больше кольца без двух кусков
 * приёма. В задаче декодера (P->info пишет она). */
static uint32_t radio_bytes(uint32_t ms) {
  uint32_t kbps = P->info.bitrate;
  if (kbps == 0) {
    kbps = P->radio_kbps;
  }
  if (kbps == 0) {
    kbps = RADIO_GUESS_KBPS;
  }
  const uint64_t bytes = (uint64_t)kbps * 125u * ms / 1000u;
  const uint32_t limit = P->ring_size - 2u * RADIO_CHUNK;
  return bytes < limit ? (uint32_t)bytes : limit;
}

/* Кодеки потока станции. */
enum { CODEC_UNKNOWN = 0, CODEC_MP3, CODEC_AAC, CODEC_OGG, CODEC_REFUSED };

static const char* codec_decoder(int codec) {
  return codec == CODEC_AAC ? "AAC.ELF" : codec == CODEC_OGG ? "OGG.ELF" : "MP3.ELF";
}

/* Кодек по Content-Type: CODEC_REFUSED — плейлист в плейлисте (x-mpegurl, x-scpls),
 * CODEC_UNKNOWN — не сказано (octet-stream и т. п.). */
static int radio_type_codec(const char* type) {
  char lower[48];
  size_t i = 0;
  for (; type[i] != 0 && i + 1 < sizeof(lower); ++i) {
    lower[i] = (type[i] >= 'A' && type[i] <= 'Z') ? (char)(type[i] + 32) : type[i];
  }
  lower[i] = 0;
  if (strstr(lower, "mpegurl") != NULL || strstr(lower, "scpls") != NULL) {
    return CODEC_REFUSED;
  }
  if (strstr(lower, "aac") != NULL || strstr(lower, "mp4a") != NULL) {
    return CODEC_AAC;
  }
  if (strstr(lower, "ogg") != NULL || strstr(lower, "vorbis") != NULL) {
    return CODEC_OGG;
  }
  if (strstr(lower, "mpeg") != NULL || strstr(lower, "mp3") != NULL) {
    return CODEC_MP3;
  }
  return CODEC_UNKNOWN;
}

/* Кодек по началу потока: OggS (у Ogg — и что внутри: Opus и FLAC — *refused),
 * ID3 или кадр MPEG Layer I–III — MP3, заголовок ADTS — AAC; CODEC_UNKNOWN — не
 * ясно. Слово синхронизации верно, только если за кадром — ещё одно. */
static int radio_sniff(const uint8_t* p, size_t n, const char** refused) {
  *refused = NULL;
  if (n >= 4 && memcmp(p, "OggS", 4) == 0) {
    const size_t header = n >= 27 ? 27u + p[26] : n;
    if (header + 8 <= n) {
      if (memcmp(p + header, "OpusHead", 8) == 0) {
        *refused = "Opus";
      } else if (memcmp(p + header, "\x7F" "FLAC", 5) == 0) {
        *refused = "FLAC";
      }
    }
    return *refused != NULL ? CODEC_REFUSED : CODEC_OGG;
  }
  if (n >= 3 && memcmp(p, "ID3", 3) == 0) {
    return CODEC_MP3;
  }
  for (size_t at = 0; at + 7 <= n; ++at) {
    if (p[at] != 0xFF || (p[at + 1] & 0xE0) != 0xE0) {
      continue;
    }
    if ((p[at + 1] & 0xF6) == 0xF0) {                    /* ADTS: слой 0 */
      const size_t length = ((size_t)(p[at + 3] & 0x03) << 11) | ((size_t)p[at + 4] << 3) |
                            (p[at + 5] >> 5);
      if (length > 7 && ((p[at + 2] >> 2) & 0x0F) < 13 && at + length + 2 <= n &&
          p[at + length] == 0xFF && (p[at + length + 1] & 0xF6) == 0xF0) {
        return CODEC_AAC;
      }
    } else if ((p[at + 1] & 0x06) != 0 && (p[at + 1] & 0x18) != 0x08 && (p[at + 2] & 0xF0) != 0xF0 &&
               (p[at + 2] & 0x0C) != 0x0C) {
      /* MPEG: слой не 0, версия не зарезервирована, битрейт и частота допустимы. */
      static const uint16_t kbps[2][3][16] = {
          {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},       /* MPEG-2 L1 */
           {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},            /* L2 */
           {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}},           /* L3 */
          {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},    /* MPEG-1 L1 */
           {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},       /* L2 */
           {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}}};      /* L3 */
      static const uint16_t rates[4][3] = {
          {11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};
      const int version = (p[at + 1] >> 3) & 3;
      const int layer = 3 - ((p[at + 1] >> 1) & 3);      /* 0 — Layer I … 2 — Layer III */
      const uint32_t bits = kbps[version == 3][layer][p[at + 2] >> 4] * 1000u;
      const uint32_t rate = rates[version][(p[at + 2] >> 2) & 3];
      if (bits == 0 || rate == 0) {
        continue;
      }
      const int pad = (p[at + 2] >> 1) & 1;
      size_t length;
      if (layer == 0) {
        length = (12u * bits / rate + pad) * 4u;
      } else if (layer == 2 && version != 3) {
        length = 72u * bits / rate + pad;
      } else {
        length = 144u * bits / rate + pad;
      }
      if (at + length + 2 <= n && p[at + length] == 0xFF && (p[at + length + 1] & 0xE0) == 0xE0) {
        return CODEC_MP3;
      }
    }
  }
  return CODEC_UNKNOWN;
}

/* Ровно count байтов (метаданные ICY); 0 — обрыв, тишина дольше RADIO_SILENCE_MS
 * или сеанс кончился. */
static int radio_read_exact(void* stream, uint8_t* out, uint32_t count, uint32_t session) {
  uint32_t done = 0;
  uint32_t quiet = 0;
  while (done < count) {
    if (!radio_alive(session)) {
      return 0;
    }
    const int got = core->net_read(stream, out + done, count - done, RADIO_READ_MS);
    if (got < 0) {
      return 0;
    }
    if (got == 0) {
      quiet += RADIO_READ_MS;
      if (quiet >= RADIO_SILENCE_MS) {
        return 0;
      }
      continue;
    }
    quiet = 0;
    done += (uint32_t)got;
  }
  return 1;
}

static void radio_task(void* arg) {
  (void)arg;
  uint8_t* chunk = (uint8_t*)core->alloc(RADIO_CHUNK, ZM_MEM_INTERNAL);
  char* meta = (char*)core->alloc(16 * 255 + 1, ZM_MEM_PSRAM);
  if (chunk == NULL || meta == NULL) {
    core->log("player: radio: no memory");
  }
  while (!P->radio_quit && chunk != NULL && meta != NULL) {
    const uint32_t session = P->radio_session;
    if (!P->radio || P->station_count == 0) {
      core->signal_wait(P->radio_wake, 200);
      continue;
    }
    char url[RADIO_URL];
    core->mutex_lock(P->radio_lock);           /* новый плейлист пишет список под ним */
    const uint16_t index = P->station < P->station_count ? P->station : 0;
    const uint16_t count = P->station_count;
    memcpy(url, P->stations[index].url, sizeof(url));
    core->mutex_unlock(P->radio_lock);
    url[sizeof(url) - 1] = 0;
    radio_set("Connecting...", "", session);
    int status = 0;
    void* stream = core->net_open(url, "Icy-MetaData: 1\r\n", RADIO_TIMEOUT_MS, &status);
    if (!radio_alive(session)) {
      if (stream != NULL) {
        core->net_close(stream);
      }
      continue;
    }
    if (stream == NULL) {
      char text[48];
      if (status != 0) {
        snprintf(text, sizeof(text), "Station answers %d, retrying", status);
      } else {
        snprintf(text, sizeof(text), "No connection, retrying");
      }
      core->log("player: radio %u/%u: %.70s: %s", (unsigned)index + 1, (unsigned)count, url, text);
      radio_set(text, NULL, session);
      radio_wait(RADIO_RETRY_MS, session);
      continue;
    }
    const char* field = core->net_header(stream, "content-type");
    char type[32];
    snprintf(type, sizeof(type), "%s", field != NULL ? field : "");
    const int type_codec = radio_type_codec(type);
    if (type_codec == CODEC_REFUSED) {
      char text[48];
      snprintf(text, sizeof(text), "%.26s is not supported", type);
      core->log("player: radio %u/%u: %s", (unsigned)index + 1, (unsigned)count, text);
      radio_set(text, NULL, session);
      core->net_close(stream);
      radio_wait(RADIO_REJECT_MS, session);
      continue;
    }
    field = core->net_header(stream, "icy-metaint");
    const uint32_t metaint = field != NULL ? (uint32_t)strtoul(field, NULL, 10) : 0;
    field = core->net_header(stream, "icy-br");
    const uint16_t kbps = field != NULL ? (uint16_t)atoi(field) : 0;
    char name[sizeof(P->radio_name)];
    char about[sizeof(P->radio_about)];
    field = core->net_header(stream, "icy-name");
    snprintf(name, sizeof(name), "%s", field != NULL ? field : "");
    /* Что за станция — пока нет песни (у многих StreamTitle пуст до её смены). */
    field = core->net_header(stream, "icy-description");
    snprintf(about, sizeof(about), "%s", field != NULL ? field : "");
    if (about[0] == 0 || strcmp(about, name) == 0) {
      field = core->net_header(stream, "icy-genre");
      snprintf(about, sizeof(about), "%s", field != NULL ? field : "");
    }
    core->mutex_lock(P->radio_lock);
    if (P->radio_session == session) {
      memcpy(P->radio_name, name, sizeof(name));
      memcpy(P->radio_about, about, sizeof(about));
      P->radio_kbps = kbps;
      P->tags_changed = 1;
    }
    core->mutex_unlock(P->radio_lock);
    /* Начало звука (до метаданных ICY): по нему кодек; не ясно — по Content-Type. */
    uint32_t until_meta = metaint;
    uint32_t want = RADIO_CHUNK;
    if (metaint != 0 && want > until_meta) {
      want = until_meta;
    }
    int pending = 0;                           /* байты начала в chunk — в кольцо первыми */
    uint32_t quiet = 0;
    while (pending < (int)want && radio_alive(session)) {
      const int got = core->net_read(stream, chunk + pending, want - (uint32_t)pending, RADIO_READ_MS);
      if (got < 0) {
        break;
      }
      if (got == 0) {
        quiet += RADIO_READ_MS;
        if (quiet >= RADIO_SILENCE_MS || pending != 0) {
          break;                               /* что пришло — того и хватит */
        }
        continue;
      }
      quiet = 0;
      pending += got;
    }
    if (!radio_alive(session) || pending == 0) {
      core->net_close(stream);
      if (radio_alive(session)) {
        core->log("player: radio %u/%u: no data", (unsigned)index + 1, (unsigned)count);
        radio_set("No data, reconnecting...", NULL, session);
        radio_wait(RADIO_RETRY_MS, session);
      }
      continue;
    }
    const char* refused = NULL;
    int codec = radio_sniff(chunk, (size_t)pending, &refused);
    if (codec == CODEC_REFUSED) {
      char text[48];
      snprintf(text, sizeof(text), "Ogg %s is not supported", refused);
      core->log("player: radio %u/%u: %s", (unsigned)index + 1, (unsigned)count, text);
      radio_set(text, NULL, session);
      core->net_close(stream);
      radio_wait(RADIO_REJECT_MS, session);
      continue;
    }
    if (codec == CODEC_UNKNOWN) {
      codec = type_codec != CODEC_UNKNOWN ? type_codec : CODEC_MP3;
    }
    core->log("player: radio %u/%u: %.70s, %s, %u kbps, metaint %u", (unsigned)index + 1,
              (unsigned)count, url, codec_decoder(codec), (unsigned)kbps, (unsigned)metaint);
    /* Новый поток: кольцо и декодер его кодека — заново (это делает задача декодера). */
    core->mutex_lock(P->radio_lock);
    snprintf(P->radio_decoder, sizeof(P->radio_decoder), "%s", codec_decoder(codec));
    core->mutex_unlock(P->radio_lock);
    P->radio_restart = session;
    core->signal_give(P->wake);
    while (P->radio_restart == session && radio_alive(session)) {
      core->delay(2);
    }
    if (P->radio_failed == session) {
      /* Декодера нет в кэше ESP (плагин его не прислал: нет в /WC/ESP). */
      char text[48];
      snprintf(text, sizeof(text), "No %s in /WC/ESP", codec_decoder(codec));
      core->log("player: radio %u/%u: %s", (unsigned)index + 1, (unsigned)count, text);
      radio_set(text, NULL, session);
      core->net_close(stream);
      radio_wait(RADIO_REJECT_MS, session);
      continue;
    }
    radio_set("Buffering...", NULL, session);
    uint32_t last_data = core->millis();
    int lost = 0;
    while (radio_alive(session)) {
      if (P->paused || P->radio_restart != 0) {
        core->delay(20);                       /* пауза: сеть ждёт (буфер TCP), звук стоит */
        last_data = core->millis();
        continue;
      }
      want = RADIO_CHUNK;
      if (metaint != 0 && want > until_meta) {
        want = until_meta;
      }
      if (pending != 0) {
        want = (uint32_t)pending;              /* начало потока уже в chunk */
      }
      if (ring_free() < want) {
        core->delay(5);                        /* декодер позади: ждать его, это не тишина */
        last_data = core->millis();
        continue;
      }
      const int got = pending != 0 ? pending : core->net_read(stream, chunk, want, RADIO_READ_MS);
      pending = 0;
      if (got < 0) {
        lost = 1;
        break;
      }
      if (got == 0) {
        if (core->millis() - last_data >= RADIO_SILENCE_MS) {
          lost = 1;
          break;
        }
        continue;
      }
      last_data = core->millis();
      /* В кольцо — под radio_lock и только в своём сеансе: стоп (radio_stop) меняет
       * сеанс под ним же, и кольцо, которое после стопа меняют или отдают
       * (ring_resize), прежний сеанс уже не тронет. */
      core->mutex_lock(P->radio_lock);
      const int current = P->radio_session == session;
      if (current) {
        ring_write(chunk, (uint32_t)got);
      }
      core->mutex_unlock(P->radio_lock);
      if (!current) {
        break;
      }
      if (metaint != 0 && (until_meta -= (uint32_t)got) == 0) {
        /* Метаданные ICY: байт длины в шестнадцатках байтов, потом текст. */
        uint8_t blocks = 0;
        if (!radio_read_exact(stream, &blocks, 1, session)) {
          lost = radio_alive(session);
          break;
        }
        const uint32_t length = blocks * 16u;
        if (length != 0) {
          if (!radio_read_exact(stream, (uint8_t*)meta, length, session)) {
            lost = radio_alive(session);
            break;
          }
          meta[length] = 0;
          char song[sizeof(P->radio_song)];
          if (radio_stream_title(meta, song, sizeof(song))) {
            core->mutex_lock(P->radio_lock);
            if (P->radio_session == session && strcmp(song, P->radio_song) != 0) {
              memcpy(P->radio_song, song, sizeof(song));
              P->tags_changed = 1;
            }
            core->mutex_unlock(P->radio_lock);
          }
        }
        until_meta = metaint;
      }
    }
    core->net_close(stream);
    if (lost && radio_alive(session)) {
      P->radio_lost = session;                 /* набор очереди не ждать: играть, что пришло */
      core->log("player: radio %u/%u: connection lost", (unsigned)index + 1, (unsigned)count);
      radio_set("Connection lost, reconnecting...", NULL, session);
      radio_wait(RADIO_RETRY_MS, session);
    }
  }
  core->free(chunk);
  core->free(meta);
  P->radio_exited = 1;
}

/* Звук станции пошёл: состояние в метках — пусто (там песня). */
static void radio_playing(void) {
  if (P->radio) {
    radio_set("", NULL, P->radio_session);
  }
}

/* Файл — плейлист M3U: станции, декодер MP3 для потока, задача радио. 0 — не
 * вышло (ядро без сети, станций нет, нет памяти): файл пропускается с причиной
 * в названии (radio_failure). Под P->lock, в задаче декодера. */
static const char* radio_failure;

static int radio_begin(void) {
  radio_failure = NULL;
  if (core->size < offsetof(zm_core_t, net_close) + sizeof(core->net_close) ||
      core->net_open == NULL || core->net_read == NULL || core->net_header == NULL ||
      core->net_close == NULL) {
    core->log("player: radio needs firmware 0.1.32 or newer");
    radio_failure = "Radio needs ESP firmware 0.1.32";
    return 0;
  }
  if (P->stations == NULL) {
    P->stations = (radio_station_t*)core->alloc(RADIO_STATIONS * sizeof(radio_station_t),
                                               ZM_MEM_PSRAM);
  }
  if (P->stations == NULL) {
    return 0;
  }
  if (!P->radio_task_started) {
    /* Задача радио спит, пока radio = 0. */
    P->radio_lock = core->mutex_create();
    P->radio_wake = core->signal_create();
    if (P->radio_lock == NULL || P->radio_wake == NULL ||
        core->task_start(radio_task, NULL, RADIO_TASK_STACK, RADIO_PRIORITY, 1) != ZM_OK) {
      core->log("player: radio: no task");
      return 0;
    }
    P->radio_task_started = 1;
  }
  /* Прежний сеанс (если был) отпустить и список станций сменить — под замком. */
  core->mutex_lock(P->radio_lock);
  P->radio = 0;
  P->radio_session++;
  const int count = radio_parse_m3u(P->file, P->file_size, P->stations, RADIO_STATIONS);
  core->mutex_unlock(P->radio_lock);
  core->free(P->file);
  P->file = NULL;
  if (count == 0) {
    core->log("player: playlist of %u bytes has no stations", (unsigned)P->file_size);
    radio_failure = "No radio stations in the playlist";
    return 0;
  }
  /* До первого соединения — MP3.ELF (плагин присылает его радио всегда); кодек
   * станции решит задача радио, и декодер сменится при сбросе потока. */
  if (load_decoder("MP3.ELF") != ZM_OK) {
    return 0;
  }
  host.stream_offset = 0;
  P->instance = P->source->open(0, NULL);
  if (P->instance == NULL) {
    return 0;
  }
  ring_resize(RADIO_RING_BYTES);            /* своё кольцо: запас на рывки сети */
  core->log("player: radio buffer %u KB", (unsigned)(P->ring_size / 1024u));
  core->mutex_lock(P->radio_lock);
  P->radio_session++;
  snprintf(P->radio_decoder, sizeof(P->radio_decoder), "MP3.ELF");
  P->radio_failed = 0;
  P->info_hash = 0;
  P->station_count = (uint16_t)count;
  P->station = 0;
  P->radio_name[0] = 0;
  P->radio_about[0] = 0;
  P->radio_song[0] = 0;
  P->radio_kbps = 0;
  snprintf(P->radio_state, sizeof(P->radio_state), "Connecting...");
  P->tags_changed = 1;
  P->radio_restart = 0;
  P->radio_list_ms = 0;
  P->radio = 1;
  core->mutex_unlock(P->radio_lock);
  P->buffering = 1;
  P->buffer_target = radio_bytes(RADIO_START_MS);
  core->signal_give(P->radio_wake);
  core->log("player: radio playlist: %d stations", count);
  return 1;
}

/* Стоп: задаче радио — отпустить станцию (она закроет соединение и уснёт). */
static void radio_stop(void) {
  if (!P->radio_task_started) {
    return;
  }
  core->mutex_lock(P->radio_lock);
  P->radio = 0;
  P->radio_session++;
  P->radio_restart = 0;
  core->mutex_unlock(P->radio_lock);
}

/* Песня от декодера (комментарии Vorbis у Ogg): «исполнитель - название» в out
 * (UTF-8), пусто — нет. Под P->lock: снимок пишет задача декодера. */
static void radio_decoder_song(char* out, size_t size) {
  core->mutex_lock(P->lock);
  if (P->info.artist[0] != 0 && P->info.title[0] != 0) {
    snprintf(out, size, "%s - %s", P->info.artist, P->info.title);
  } else {
    snprintf(out, size, "%s", P->info.title[0] != 0 ? P->info.title : P->info.artist);
  }
  core->mutex_unlock(P->lock);
}

/* Метки радио: [станция][песня, состояние или что за станция][«Station 3 of 11»][][],
 * за ними — строка формата, как у OPL («AAC  128 kbps  44.1 kHz  stereo»). */
static void radio_tags_reply(void) {
  static uint8_t r[49 * 2 + 24 + 2 + 64];
  char title[49];
  char song[49];
  char album[24];
  char format[64];
  char from_decoder[sizeof(P->info.artist) + 3 + sizeof(P->info.title)];
  radio_decoder_song(from_decoder, sizeof(from_decoder));
  core->mutex_lock(P->radio_lock);
  const radio_station_t* s = &P->stations[P->station < P->station_count ? P->station : 0];
  const char* name = s->title[0] != 0 ? s->title : (P->radio_name[0] != 0 ? P->radio_name : s->url);
  text_cp866(name, title, sizeof(title));
  /* Состояние («Buffering...», «Connection lost…») важнее песни из комментариев:
   * она остаётся от прежнего потока, пока новый не начался. */
  text_cp866(P->radio_song[0] != 0    ? P->radio_song
                   : P->radio_state[0] != 0 ? P->radio_state
                   : from_decoder[0] != 0   ? from_decoder
                                            : P->radio_about,
                   song, sizeof(song));
  snprintf(album, sizeof(album), "Station %u of %u", (unsigned)P->station + 1,
           (unsigned)P->station_count);
  P->tags_changed = 0;
  core->mutex_unlock(P->radio_lock);
  /* Кодек — первое слово формата декодера («HE-AAC 64 kbps» → HE-AAC). */
  char codec[12];
  size_t letters = 0;
  while (letters + 1 < sizeof(codec) && P->info.format[letters] > ' ') {
    codec[letters] = P->info.format[letters];
    ++letters;
  }
  codec[letters] = 0;
  /* Частота и каналы — из снимка декодера текущего потока (на сбросе потока
   * снимок обнуляется): у станции, которой отказано, нет и строки формата. */
  const uint32_t rate = P->info.rate;
  if (rate != 0) {
    const unsigned tenths = (unsigned)(rate % 1000) / 100;
    char khz[16];
    if (tenths != 0) {
      snprintf(khz, sizeof(khz), "%u.%u", (unsigned)rate / 1000, tenths);
    } else {
      snprintf(khz, sizeof(khz), "%u", (unsigned)rate / 1000);
    }
    const unsigned kbps = P->info.bitrate != 0 ? P->info.bitrate : P->radio_kbps;
    snprintf(format, sizeof(format), "%s  %u kbps  %s kHz  %s", codec, kbps, khz,
             P->info.channels == 1 ? "mono" : "stereo");
  } else {
    format[0] = 0;
  }
  const char* fields[] = {title, song, album, "", "", format};
  size_t used = 0;
  for (int i = 0; i < 6; ++i) {
    const size_t length = strlen(fields[i]);
    memcpy(r + used, fields[i], length + 1);
    used += length + 1;
  }
  core->reply(RESPONSE(AUDIO_TAGS), r, (uint16_t)used);
}

/* Задача плеера: декодер → PCM ядру. */
static void decode_task(void* arg) {
  (void)arg;
  while (!P->stop) {
    if (P->radio && P->radio_restart != 0) {
      /* Радио: новое соединение (или смена станции) — прежний звук прочь,
       * декодер заново (поток начинается не с начала кадра) и того кодека,
       * что узнала задача радио: модуль сменится, если нужен другой. */
      core->mutex_lock(P->lock);
      const uint32_t session = P->radio_restart;
      if (session != 0) {
        char name[sizeof(P->radio_decoder)];
        core->mutex_lock(P->radio_lock);
        memcpy(name, P->radio_decoder, sizeof(name));
        core->mutex_unlock(P->radio_lock);
        name[sizeof(name) - 1] = 0;
        P->generation++;
        core->pcm_flush();
        view_reset();
        if (P->instance != NULL && P->source != NULL) {
          P->source->close(P->instance);
        }
        P->instance = NULL;
        ring_reset();
        host.stream_offset = 0;
        if (load_decoder(name[0] != 0 ? name : "MP3.ELF") == ZM_OK) {
          P->instance = P->source->open(0, NULL);
        }
        if (P->instance == NULL) {
          P->radio_failed = session;
        }
        memset(&P->info, 0, sizeof(P->info));
  P->info_track[0] = 0;
        P->format = FMT_MP3;
        P->info_hash = 0;
        P->buffering = 1;
        P->buffer_target = radio_bytes(RADIO_START_MS);   /* битрейт — из icy-br новой станции */
        P->radio_lost = 0;
        P->starving = 0;
        P->written = 0;
        P->base_ms = 0;
      }
      /* Сброс сделан для session; за это время могли попросить новый (смена
       * станции) — тот не терять. */
      uint32_t expected = session;
      __atomic_compare_exchange_n(&P->radio_restart, &expected, 0, 0, __ATOMIC_ACQ_REL,
                                  __ATOMIC_ACQUIRE);
      core->mutex_unlock(P->lock);
      continue;
    }
    if (P->load_pending) {
      /* Файл модуля принят (или потоком — начат): декодер разбирает его здесь —
       * это долго (потоком — пока плагин шлёт файл). */
      core->mutex_lock(P->lock);
      P->load_pending = 0;
      int ok;
      if (P->stream_module) {
        ok = P->source != NULL &&
             (P->instance = P->source->open(P->file_size, NULL)) != NULL;
        P->stream_done = 1;
        ring_reset();                           /* остаток потока декодеру не нужен */
        if (!ok) {
          /* Отказ раньше конца файла (не помещается, не IT): приём — прочь, на
           * AUDIO_DATA ответ 2, и плагин 1.1 файл больше не шлёт, а пропуск —
           * сразу (иначе плагин досылал бы мегабайты впустую). */
          P->module_loading = 0;
        }
      } else if (strcmp(P->file_type, RADIO_TYPE) == 0) {
        ok = radio_begin();                     /* плейлист: интернет-радио */
      } else {
        ok = load_decoder(whole_decoder(P->file, P->file_size)) == ZM_OK;
        if (ok) {
          P->instance = P->source->open(P->file_size, P->file);
          ok = P->instance != NULL;
        }
        if (ok && !P->source->keeps_file) {
          core->free(P->file);
          P->file = NULL;
        }
      }
      if (ok) {
        source_info(&P->info);
        P->format = P->radio ? FMT_MP3 : decoder_format();
      } else {
        P->info.errors++;
        snprintf(P->info.title, sizeof(P->info.title), "%s",
                 radio_failure != NULL ? radio_failure : SKIP_TITLE);
        core->log("player: module of %u bytes was not loaded: skipped", (unsigned)P->file_size);
      }
      P->load_ok = ok;
      P->ready_pending = 1;
      core->mutex_unlock(P->lock);
      continue;
    }
    if (P->ready_pending && !P->module_loading) {
      /* Файл пришёл весь: играть. Не взял декодер (не музыка, испорчен) —
       * 1,5 с «играет» с причиной вместо названия и кончается: плагин уходит к
       * следующему файлу сам, альбом на таком файле не встаёт. Формат не
       * распознан (decoded = 0): позиций и каналов плагин не рисует. */
      core->mutex_lock(P->lock);
      if (P->ready_pending) {                   /* не остановлен командой, пока ждали замок */
        P->ready_pending = 0;
        P->state = ST_PLAYING;
        if (P->load_ok) {
          P->decoded = 1;
        } else {
          P->skip_at = core->millis() + SKIP_MS;
          if (P->skip_at == 0) {
            P->skip_at = 1;
          }
        }
      }
      core->mutex_unlock(P->lock);
      continue;
    }
    if (P->skip_at != 0 && P->instance == NULL && (int32_t)(core->millis() - P->skip_at) >= 0) {
      core->mutex_lock(P->lock);
      if (P->skip_at != 0) {                    /* не остановлен командой, пока ждали замок */
        P->skip_at = 0;
        P->finished = 1;
        P->state = ST_IDLE;
      }
      core->mutex_unlock(P->lock);
      continue;
    }
    const uint8_t state = P->state;
    if ((state != ST_PLAYING && state != ST_DRAINING) || P->instance == NULL || P->paused) {
      core->signal_wait(P->wake, 20);
      continue;
    }
    if (P->buffering) {
      const uint32_t available = ring_available();
      /* Соединение станции оборвалось — набора не ждать: играть, что пришло, пока
       * задача радио подключается снова. */
      const int lost = P->radio && available != 0 && P->radio_lost == P->radio_session;
      if ((P->format == FMT_MP3 || strcmp(P->file_type, "FLA") == 0) &&
          available < P->buffer_target && !P->closed && !lost &&
          ring_free() >= 4096) {
        core->delay(5);
        continue;
      }
      P->buffering = 0;
      if (!lost) {
        radio_playing();                        /* при обрыве в метках — «Connection lost…» */
      }
    }
    uint32_t frames = 0;
    core->mutex_lock(P->lock);
    int result = ZM_RENDER_ERROR;
    const uint32_t generation = P->generation;
    if (P->instance != NULL) {
      const uint32_t render_start = cycle_count();
      result = P->source->render(P->instance, P->frames, RENDER_FRAMES, &frames);
      const uint32_t spent = cycle_count() - render_start;
      if (result == ZM_RENDER_OK && frames != 0) {
        P->render_cycles += spent;
        if (spent / frames > P->render_worst) {
          P->render_worst = spent / frames;
        }
      }
      /* Снимок — копией: info декодера сначала обнуляет его, и команды Z80 (они
       * читают P->info без замка) видели бы нули (длительность 0 и т. п.). */
      static zm_info_t snapshot;
      source_info(&snapshot);
      P->info = snapshot;
      if (P->radio || P->stream_open) {
        /* Название и исполнитель от декодера сменились (Ogg: комментарии Vorbis,
         * у радио — каждой песни цепочки; WAV: LIST INFO) — метки заново. У файла
         * — только если они есть: MP3 берёт метки из ID3 (cover_glue). */
        uint32_t hash = 2166136261u;
        for (const char* c = P->info.title; *c != 0; ++c) hash = (hash ^ (uint8_t)*c) * 16777619u;
        for (const char* c = P->info.artist; *c != 0; ++c) hash = (hash ^ (uint8_t)*c) * 16777619u;
        for (const char* c = P->info.album; *c != 0; ++c) hash = (hash ^ (uint8_t)*c) * 16777619u;
        for (const char* c = P->info_track; *c != 0; ++c) hash = (hash ^ (uint8_t)*c) * 16777619u;
        hash = (hash ^ P->info.year) * 16777619u;
        if (hash != P->info_hash) {
          P->info_hash = hash;
          if (P->radio || P->info.title[0] != 0 || P->info.artist[0] != 0 ||
              P->info.album[0] != 0 || P->info.year != 0 || P->info_track[0] != 0) {
            P->tags_changed = 1;
          }
        }
      }
    }
    core->mutex_unlock(P->lock);
    /* Пока render шёл без замка, команда сменила поколение (открыт другой файл, стоп,
     * перемотка): итог прежнего потока не нужен. Прежде его конец (END) закрывал
     * вывод и ставил ST_IDLE уже новому файлу, и тот не начинал играть. */
    if (P->generation != generation) {
      continue;
    }
    if (result == ZM_RENDER_OK && frames != 0) {
      P->rendered += frames;
      update_levels();
      if (P->format == FMT_NONE) {
        P->format = decoder_format();
      }
      P->rate = P->info.rate;
      P->channels = P->info.channels;
      P->decoded = 1;
      P->starving = 0;
      if (P->open_rate == 0 && !P->radio && !P->stream_open && P->info.rate != 0 &&
          P->start_done != generation + 1u && start_take(generation)) {
        /* Вывод ещё закрыт — начало файла: копить до START_MS, потом разом. */
        memcpy(P->start_buf + P->start_fill * P->channels, P->frames,
               frames * P->channels * sizeof(float));
        P->start_fill += frames;
        if (P->start_fill + RENDER_FRAMES <= P->start_cap &&
            P->start_fill < P->info.rate * START_MS / 1000) {
          if (core->millis() - P->rest_ms >= REST_EVERY_MS) {
            core->delay(1);
            P->rest_ms = core->millis();
          }
          continue;
        }
        start_flush(generation);
        continue;
      }
      open_output();
      write_frames(P->frames, frames, generation);
      /* Сторож задач ESP: вывод ждёт только при полной очереди, и декодер, не
       * успевающий за звуком (тяжёлая песня OPL4, многоголосый MIDI), держал
       * бы ядро 0 без пауз — задача IDLE не кормит сторожа, и через 5 с ESP
       * перезапускается. Раз в REST_EVERY_MS — 1 мс другим задачам: при
       * перегрузке звук рвётся, но ESP не падает. */
      if (core->millis() - P->rest_ms >= REST_EVERY_MS) {
        core->delay(1);
        P->rest_ms = core->millis();
      }
    } else if (result == ZM_RENDER_NEED_INPUT) {
      if (P->start_fill != 0 && P->start_generation == generation) {
        start_flush(generation);                /* вход кончился раньше START_MS */
      }
      /* Кольцо пусто. У радио и FLAC это ещё не опустошение, пока есть готовый PCM:
       * поток идёт вровень со звуком, и декодер, обогнав его, ждёт следующие байты.
       * Лишнее повторное накопление при достаточном PCM само создаёт паузы:
       * декодер ждёт заполнения входа, а готовый звук за это время кончается. */
      if (!P->starving && P->decoded && !P->closed &&
          ((!P->radio && strcmp(P->file_type, "FLA") != 0) ||
           core->pcm_queued_ms() < RADIO_LOW_MS)) {
        P->underruns++;
        P->starving = 1;
        P->buffering = 1;
        P->buffer_target = P->radio ? radio_bytes(RADIO_REFILL_MS) : file_buffer_target(0);
        if (P->radio) {
          core->log("player: radio stream ran dry (%u), waiting for %u KB",
                    (unsigned)P->underruns, (unsigned)(P->buffer_target / 1024u));
          radio_set("Buffering...", NULL, P->radio_session);
        }
      }
      core->delay(5);
    } else if (P->radio) {
      core->delay(5);                           /* радио не кончается: ждать поток */
    } else if (result == ZM_RENDER_END || result == ZM_RENDER_ERROR) {
      /* Файл короче START_MS: накопленное начало — в вывод. */
      if (P->start_fill != 0 && P->start_generation == generation) {
        start_flush(generation);
      }
      /* Всё отдано: доиграть очередь ядра и встать. */
      if (core->pcm_queued_ms() == 0) {
        core->mutex_lock(P->lock);
        if (P->generation == generation) {      /* ещё тот же поток — под замком */
          report("played to the end");
          close_output();
          P->finished = 1;
          P->state = ST_IDLE;
        }
        core->mutex_unlock(P->lock);
      } else {
        core->delay(20);
      }
    }
  }
  P->task_done = 1;
}

/* --- команды ----------------------------------------------------------------------- */

static void le16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t* p, uint32_t v) {
  le16(p, v);
  le16(p + 2, v >> 16);
}
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t free_space(void) {
  if (P->module_loading) {
    return P->file_size - P->file_written;
  }
  return ring_free();
}

static uint32_t played_ms(void) {
  if (P->rate == 0) {
    return P->base_ms;
  }
  if (!P->stream_open && P->format != FMT_MP3 && P->info.duration_ms != 0) {
    /* Декодер знает место в песне (MIDI, IT) — и после перемотки: оно за
     * последним отданным кадром, минус то, что ещё в очереди вывода. */
    const uint32_t queued_ms = core->pcm_queued_ms();
    return P->info.position_ms > queued_ms ? P->info.position_ms - queued_ms : 0;
  }
  const uint64_t queued = (uint64_t)core->pcm_queued_ms() * P->rate / 1000;
  const uint64_t played = P->written > queued ? P->written - queued : 0;
  return P->base_ms + (uint32_t)(played * 1000 / P->rate);
}

/* Остановить звук и всё, что было открыто (новый файл, обрыв). */
static void stop_all(void) {
  report("stop");
  radio_stop();
  P->load_abort = 1;                      /* декодер в open потоком — не ждать байтов */
  core->mutex_lock(P->lock);
  P->generation++;
  view_reset();
  close_output();
  P->stream_open = 0;
  P->module_loading = 0;
  P->load_pending = 0;
  P->stream_module = 0;
  P->stream_done = 0;
  P->ready_pending = 0;
  P->load_ok = 0;
  P->room_made = 0;
  close_decoder();
  if (P->file != NULL) {
    core->free(P->file);
    P->file = NULL;
  }
  P->file_size = P->file_written = 0;
  ring_resize(RING_BYTES);                 /* большое кольцо радио/FLAC — прочь */
  core->pcm_flush();
  P->state = ST_IDLE;
  P->format = FMT_NONE;
  P->closed = 0;
  P->decoded = 0;
  P->buffering = 0;
  P->paused = 0;
  P->starving = 0;
  P->written = 0;
  P->rendered = 0;
  P->render_cycles = 0;
  P->render_worst = 0;
  P->dropped = 0;
  P->level_count = 0;
  P->base_ms = 0;
  P->skip_at = 0;
  P->underruns = 0;
  P->wants_rom = 0;
  P->rate = 0;
  memset(&P->info, 0, sizeof(P->info));
  P->info_track[0] = 0;
  P->load_abort = 0;
  P->tags_changed = 0;
  P->info_hash = 0;
  P->cover_found = 0;
  core->mutex_unlock(P->lock);
  core->pcm_pause(0);
}

/* IT идёт потоком: TRACKER разбирает его по мере прихода и держит только то,
 * что libxmp перечитывает, — отсчёты не лежат в памяти дважды (файл и копия
 * libxmp), и IT больше MODULE_MAX тоже играет. Декодер без этого умения
 * (старый TRACKER.ELF) — 0: файл целиком, как прежде. */
static int open_stream_module(uint32_t size) {
  core->mutex_lock(P->lock);
  const int ok = load_decoder("TRACKER.ELF") == ZM_OK &&
                 P->source->size >= offsetof(zm_source_t, streams_whole) + 1 &&
                 P->source->streams_whole;
  core->mutex_unlock(P->lock);
  if (!ok) {
    return 0;
  }
  ring_reset();
  P->file_size = size;
  P->file_written = 0;
  P->stream_module = 1;
  P->stream_done = 0;
  P->module_loading = 1;
  P->buffering = 1;
  P->state = ST_LOADING;
  P->load_pending = 1;
  core->signal_give(P->wake);
  core->log("player: IT of %u bytes comes as a stream", (unsigned)size);
  return 1;
}

static void reply_open(int ok) {
  uint8_t reply[5];
  reply[0] = (uint8_t)ok;
  le32(reply + 1, free_space());
  core->reply(RESPONSE(AUDIO_OPEN), reply, 5);
}

static void command_open(const uint8_t* data, uint16_t length) {
  stop_all();
  cover_reset();
  P->finished = 0;
  P->stream_size = 0;
  memset(P->file_type, 0, sizeof(P->file_type));
  if (length == 1 && data[0] == OPEN_RELEASE) {
    /* Плагину не хватило места на ресурс: освободить все, он пришлёт нужный. */
    if (core->resource_release != NULL) {
      core->log("player: resources released for the plugin");
      core->resource_release(MIDI_BANK);
      core->resource_release(OPL4_ROM);
      if (P->kit[0] != 0) {
        core->resource_release(P->kit);
        P->kit[0] = 0;
      }
    }
    reply_open(1);
    return;
  }
  if (length >= 5 && (data[0] & OPEN_MODULE) != 0) {
    const uint32_t size = rd32(data + 1);
    /* [флаги][длина LE32][расширение, 3 знака] — расширение шлёт плагин 0.7. */
    for (uint16_t i = 0; i < 3 && 5u + i < length && data[5 + i] > ' '; ++i) {
      const char c = (char)data[5 + i];
      P->file_type[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    if (size != 0 && strcmp(P->file_type, "IT") == 0 && open_stream_module(size)) {
      reply_open(1);
      return;
    }
    if (size == 0 || size > MODULE_MAX) {
      reply_open(0);
      return;
    }
    P->file = (uint8_t*)core->alloc(size + ZM_WHOLE_SLACK, ZM_MEM_PSRAM);   /* запас — декодеру */
    if (P->file == NULL && core->resource_release != NULL) {
      /* Не помещается: ресурсы, которые этому файлу не нужны, — прочь. */
      const int midi = (data[0] & OPEN_MIDI) != 0;
      const int opl = opl_type(P->file_type);
      core->log("player: %u bytes do not fit with the resources: %s released", (unsigned)size,
                midi ? OPL4_ROM : opl ? MIDI_BANK : MIDI_BANK " and " OPL4_ROM);
      if (!midi) {
        core->resource_release(MIDI_BANK);
      }
      if (!opl) {
        core->resource_release(OPL4_ROM);
      }
      P->file = (uint8_t*)core->alloc(size + ZM_WHOLE_SLACK, ZM_MEM_PSRAM);
    }
    if (P->file == NULL) {
      reply_open(0);
      return;
    }
    P->file_size = size;
    P->file_written = 0;
    P->module_loading = 1;
    P->buffering = 1;
    P->state = ST_LOADING;
    reply_open(1);
    return;
  }
  /* Поток: [0][длина LE32][флаги][расширение, 3 знака] — длина потока, по ней
   * декодер считает длительность (старый плагин шлёт пустое тело); расширение
   * (плагин 1.4) выбирает декодер: AAC — AAC.ELF, OGG — OGG.ELF, нет его — MP3.ELF. */
  if (length >= 5) {
    P->stream_size = rd32(data + 1);
  }
  for (uint16_t i = 0; i < 3 && 6u + i < length && data[6 + i] > ' '; ++i) {
    const char c = (char)data[6 + i];
    P->file_type[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
  }
  if (strcmp(P->file_type, "FLA") == 0) {
    core->mutex_lock(P->lock);
    ring_resize(FLAC_RING_BYTES);
    if (P->ring_big == NULL && host_make_room(FLAC_RING_BYTES)) ring_resize(FLAC_RING_BYTES);
    core->mutex_unlock(P->lock);
    core->log("player: FLAC buffer %u KB, start %u KB, refill %u KB",
              (unsigned)(P->ring_size / 1024u),
              (unsigned)(file_buffer_target(1) / 1024u),
              (unsigned)(file_buffer_target(0) / 1024u));
  }
  const char* decoder = stream_decoder(P->file_type);
  if (length >= 6) {
    core->log("player: stream %s of %u bytes (%s); plugin: seek %s, own file %s, ID3 tag %s",
              P->file_type[0] != 0 ? P->file_type : "MP3", (unsigned)P->stream_size, decoder,
              (data[5] & 1) ? "yes" : "no", (data[5] & 2) ? "found" : "NOT found",
              (data[5] & 4) ? "yes" : "no");
  }
  core->mutex_lock(P->lock);
  host.stream_offset = 0;                 /* поток — с начала файла */
  const int ok = load_decoder(decoder) == ZM_OK &&
                 (P->instance = P->source->open(P->stream_size, NULL)) != NULL;
  core->mutex_unlock(P->lock);
  if (!ok) {
    reply_open(0);
    return;
  }
  P->format = FMT_NONE;
  P->stream_open = 1;
  P->buffer_target = file_buffer_target(1);
  P->buffering = P->buffer_target != 0;
  P->state = ST_PLAYING;
  core->signal_give(P->wake);
  reply_open(1);
}

static void command_data(const uint8_t* data, uint16_t length) {
  uint8_t reply[5];
  if (P->module_loading && P->stream_module) {
    /* Файл целиком потоком: в кольцо декодеру; оно полно — подождать его (он
     * быстрее SPI; плагин в приёме модуля ждёт только «взято»). Декодер уже
     * взял файл или отказал — остаток прочь. */
    if (length > P->file_size - P->file_written) {
      reply[0] = 2;
    } else {
      const uint32_t start = core->millis();
      while (!P->stream_done && !P->load_abort && ring_free() < length &&
             core->millis() - start < STREAM_WAIT_MS) {
        core->delay(1);
      }
      if (!P->stream_done && ring_free() >= length) {
        ring_write(data, length);
      } else if (!P->stream_done && !P->load_abort) {
        /* Декодер встал: поток без этого куска ему не годится — пусть откажет. */
        core->log("player: stream decoder stalled at %u bytes", (unsigned)P->file_written);
        P->load_abort = 1;
      }
      P->file_written += length;
      reply[0] = 1;
      if (P->file_written == P->file_size) {
        P->module_loading = 0;
        core->signal_give(P->wake);
      }
    }
  } else if (P->module_loading) {
    if (length > P->file_size - P->file_written) {
      reply[0] = 2;
    } else {
      memcpy(P->file + P->file_written, data, length);
      P->file_written += length;
      reply[0] = 1;
      if (P->file_written == P->file_size) {
        P->module_loading = 0;
        P->load_pending = 1;
        core->signal_give(P->wake);
      }
    }
  } else if (!P->stream_open) {
    reply[0] = 2;
  } else if (length > ring_free()) {
    reply[0] = 0;                        /* блок целиком или никак: Z80 повторит */
  } else {
    ring_write(data, length);
    reply[0] = 1;
  }
  le32(reply + 1, free_space());
  core->reply(RESPONSE(AUDIO_DATA), reply, 5);
}

static void command_close(const uint8_t* data, uint16_t length) {
  const int drain = length != 0 && data[0] != 0;
  if (drain && P->instance != NULL && P->stream_open) {
    P->stream_open = 0;
    P->closed = 1;
    P->state = ST_DRAINING;
    core->signal_give(P->wake);
  } else if (!drain) {
    stop_all();
  }
  const uint8_t ok = 1;
  core->reply(RESPONSE(AUDIO_CLOSE), &ok, 1);
}

static void command_status(void) {
  uint8_t r[STATUS_SIZE];
  memset(r, 0, sizeof(r));
  r[0] = P->state;
  r[1] = (uint8_t)((P->finished ? 1 : 0) | (P->decoded ? 2 : 0) | (P->buffering ? 4 : 0) |
                   (P->paused ? 8 : 0) | (P->wants_rom ? 16 : 0) |
                   (P->tags_changed ? ST_F_TAGS : 0) | (P->cover_found ? ST_F_COVER : 0));
  le32(r + 2, free_space());
  le32(r + 6, P->module_loading || P->format != FMT_MP3 ? 0 : ring_available());
  /* Частота — младшие 16 бит; больше 65535 (88,2 и 96 кГц: FLAC 24/96, TSFM на AY) —
   * ещё флаг ST_F_RATE_HIGH (бит 16 частоты; с PLAYER 1.19, плагин 1.18). Прежде
   * 96000 уходило как 30464, и плеер показывал 30.4 kHz. */
  const uint32_t rate = P->decoded ? P->rate : 0;
  le16(r + 10, (uint16_t)rate);
  if (rate > 0xFFFF) r[1] |= ST_F_RATE_HIGH;
  r[12] = P->decoded ? P->channels : 0;
  le16(r + 13, P->info.bitrate);
  const uint32_t seconds = played_ms() / 1000;
  le16(r + 15, seconds > 0xFFFF ? 0xFFFF : seconds);
  le16(r + 17, core->pcm_faults());
  le16(r + 19, P->info.errors > 0xFFFF ? 0xFFFF : P->info.errors);
  le16(r + 21, P->underruns);
  r[23] = core->volume();
  r[24] = P->decoded ? P->format : FMT_NONE;
  r[25] = P->info.row;
  le16(r + 26, P->radio ? P->station : P->info.order);           /* радио: станция из скольких */
  le16(r + 28, P->radio ? P->station_count : P->info.order_length);
  r[30] = P->format != FMT_MP3 ? P->info.voices : 0;
  /* Температура кристалла ESP, °C (0 — неизвестна): у ядра с ABI minor 1. */
  if (core->size >= offsetof(zm_core_t, temperature) + sizeof(core->temperature) &&
      core->temperature != NULL) {
    const int tenths = core->temperature();
    if (tenths != ZM_TEMPERATURE_UNKNOWN) {
      const int celsius = (tenths + 5) / 10;
      r[31] = (uint8_t)(celsius < 1 ? 1 : (celsius > 255 ? 255 : celsius));
    }
  }
  core->reply(RESPONSE(AUDIO_STATUS), r, STATUS_SIZE);
}

static void command_seek(const uint8_t* data, uint16_t length) {
  uint8_t reply[3] = {0, 0, 0};
  if (P->radio) {
    /* Радио: влево-вправо — соседняя станция (по кругу); звук прежней — сразу прочь. */
    if (length >= 1 && P->station_count != 0) {
      core->mutex_lock(P->radio_lock);
      int next = ((int)P->station + (int8_t)data[0]) % (int)P->station_count;
      if (next < 0) {
        next += P->station_count;
      }
      P->station = (uint16_t)next;
      P->radio_session++;
      P->radio_song[0] = 0;
      P->radio_name[0] = 0;
      P->radio_about[0] = 0;
      snprintf(P->radio_state, sizeof(P->radio_state), "Connecting...");
      P->tags_changed = 1;
      P->radio_restart = P->radio_session;
      P->radio_list_ms = core->millis() | 1;     /* окно Z80: список станций RADIO_LIST_MS */
      core->mutex_unlock(P->radio_lock);
      core->signal_give(P->radio_wake);
      core->signal_give(P->wake);
      reply[0] = 1;
      le16(reply + 1, (uint32_t)next);
    }
    core->reply(RESPONSE(AUDIO_SEEK), reply, 3);
    return;
  }
  if (length >= 1 && P->instance != NULL && P->format != FMT_MP3 && P->state == ST_PLAYING) {
    int32_t delta = (int8_t)data[0];
    if (P->source->seek_unit == ZM_SEEK_MS) {
      delta *= 10000;                    /* MIDI: шаг клавиши — 10 с */
    }
    uint32_t offset = 0;
    core->mutex_lock(P->lock);
    const int ok = P->source->seek(P->instance, delta, &offset) == ZM_OK;
    if (ok) {
      P->generation++;
      core->pcm_flush();
      view_reset();
      source_info(&P->info);
    }
    core->mutex_unlock(P->lock);
    reply[0] = (uint8_t)ok;
    le16(reply + 1, P->info.order);
  }
  core->reply(RESPONSE(AUDIO_SEEK), reply, 3);
}

/* Перемотка MP3 по-старому: место посчитал Z80 и подаёт файл оттуда сам. */
/* [время нового места, мс LE32][место в файле LE32 — плагин 0.8]: поток с нового
 * места, декодер заново; место нужно WAV, чтобы встать на отсчёт. */
static void command_flush(const uint8_t* data, uint16_t length) {
  uint8_t reply[5] = {0};
  /* Поток закрыт для доигрывания (AUDIO_CLOSE [1]): плагин всё отдал, а звук ещё
   * идёт, — перемотка открывает поток снова. Прежде FLUSH здесь отвергался, и
   * стрелки во время доигрывания ничего не делали. */
  const int draining = P->state == ST_DRAINING && P->closed && !P->radio;
  if (length >= 4 && (P->stream_open || draining) && P->instance != NULL) {
    core->mutex_lock(P->lock);
    if (draining) {
      P->stream_open = 1;
      P->closed = 0;
      P->state = ST_PLAYING;
    }
    P->generation++;
    P->source->close(P->instance);
    host.stream_offset = length >= 8 ? rd32(data + 4) : UINT32_MAX;
    P->instance = P->source->open(P->stream_size, NULL);
    ring_reset();
    core->pcm_flush();
    view_reset();
    P->written = 0;
    P->base_ms = rd32(data);
    P->buffering = 1;
    P->buffer_target = file_buffer_target(0);
    P->starving = 0;
    core->mutex_unlock(P->lock);
    reply[0] = P->instance != NULL ? 1 : 0;
  }
  le32(reply + 1, free_space());
  core->reply(RESPONSE(AUDIO_FLUSH), reply, 5);
}

/* Метки потока: ID3 (обложка MP3); нет их — что нашёл декодер (LIST INFO у WAV). */
static void stream_tags(player_tags_t* tags) {
  cover_tags(tags);
  if (tags->track[0] == 0) text_cp866(P->info_track, tags->track, sizeof(tags->track));
  if (tags->title[0] == 0 && tags->artist[0] == 0) {
    text_cp866(P->info.title, tags->title, 49);
    text_cp866(P->info.artist, tags->artist, 49);
    text_cp866(P->info.album, tags->album, 49);
    if (P->info.year != 0) {
      snprintf(tags->year, sizeof(tags->year), "%u", (unsigned)P->info.year);
    }
  }
}

/* Название для Z80: у модуля — из файла, у потока — из меток. */
static size_t song_title(char* out, size_t size) {
  out[0] = 0;
  if (P->radio) {                                           /* радио: станция */
    core->mutex_lock(P->radio_lock);
    const radio_station_t* s = &P->stations[P->station < P->station_count ? P->station : 0];
    text_cp866(s->title[0] != 0 ? s->title : P->radio_name, out, size);
    core->mutex_unlock(P->radio_lock);
    return strlen(out);
  }
  if (!P->stream_open && P->format != FMT_MP3) {           /* модуль */
    if (P->state == ST_PLAYING || P->state == ST_DRAINING) {
      strncpy(out, P->info.title, size - 1);
      out[size - 1] = 0;
    }
    return strlen(out);
  }
  player_tags_t tags;
  memset(&tags, 0, sizeof(tags));
  stream_tags(&tags);
  if (tags.artist[0] != 0 && tags.title[0] != 0) {
    snprintf(out, size, "%s - %s", tags.artist, tags.title);
  } else {
    snprintf(out, size, "%s", tags.title[0] != 0 ? tags.title : tags.artist);
  }
  return strlen(out);
}

static void command_info(void) {
  uint8_t r[72];
  memset(r, 0, sizeof(r));
  r[0] = P->format;
  r[1] = P->info.voices;
  le16(r + 2, P->info.order_length);
  const size_t title = song_title((char*)r + 4, sizeof(r) - 4);
  core->reply(RESPONSE(AUDIO_INFO), r, (uint16_t)(5 + title));
}

static void command_cover(const uint8_t* data, uint16_t length) {
  /* [смещение LE32][данные] → [итог][откуда слать дальше LE32][свободно LE32] */
  uint8_t r[9];
  uint32_t want = 0;
  r[0] = length >= 4 ? cover_feed(rd32(data), data + 4, length - 4u, &want) : COVER_NONE;
  le32(r + 1, want);
  le32(r + 5, free_space());
  core->reply(RESPONSE(AUDIO_COVER), r, 9);
}

static void command_picture(const uint8_t* data, uint16_t length) {
  /* [длина файла LE32][смещение LE32][данные] → [итог][принято LE32][свободно LE32] */
  uint8_t r[9];
  uint32_t received = 0;
  r[0] = length >= 8 ? cover_picture(rd32(data), rd32(data + 4), data + 8, length - 8u, &received)
                     : COVER_NONE;
  le32(r + 1, received);
  le32(r + 5, free_space());
  core->reply(RESPONSE(AUDIO_PICTURE), r, 9);
}

/* [вид 0 нет / 1 спектр / 2 уровни каналов][число][длительность с LE16][позиция с LE16]
 * [значения 0..12]. Длительность 0 — неизвестна (модуль, MP3 до первого кадра). */
/* Радио листают: вместо столбиков — список станций, текстом для TXTPR плагина
 * (коды цвета: 7 — белый, 6 6 — ярко-жёлтый; #0D — следующая строка).
 * RADIO_LIST_ROWS строк по RADIO_LIST_WIDTH знаков, текущая станция — посередине
 * окна станций, у краёв списка — к краю. */
static void radio_list_reply(uint32_t position) {
  static uint8_t r[6 + RADIO_LIST_ROWS * (2 + RADIO_LIST_WIDTH + 1) + 1];
  r[0] = VIEW_STATIONS;
  r[1] = 0;
  le16(r + 2, 0);
  le16(r + 4, position > 0xFFFF ? 0xFFFF : position);
  size_t used = 6;
  core->mutex_lock(P->radio_lock);
  const int count = P->station_count;
  const int current = P->station;
  int first = current - RADIO_LIST_ROWS / 2;
  if (first > count - RADIO_LIST_ROWS) {
    first = count - RADIO_LIST_ROWS;
  }
  if (first < 0) {
    first = 0;
  }
  for (int row = 0; row < RADIO_LIST_ROWS; ++row) {
    const int index = first + row;
    char line[RADIO_LIST_WIDTH + 16];       /* с запасом под номер: длиннее — обрезать ниже */
    line[0] = 0;
    if (index < count) {
      char name[RADIO_LIST_WIDTH];
      const radio_station_t* s = &P->stations[index];
      text_cp866(s->title[0] != 0 ? s->title : s->url, name, sizeof(name));
      snprintf(line, sizeof(line), "%c%3d. %s", index == current ? '>' : ' ', index + 1, name);
    }
    size_t length = strlen(line);
    if (length > RADIO_LIST_WIDTH) {
      length = RADIO_LIST_WIDTH;              /* длинное название — по ширину строки */
    }
    while (length < RADIO_LIST_WIDTH) {
      line[length++] = ' ';               /* затереть столбики до конца строки */
    }
    if (index == current) {
      r[used++] = 6;
      r[used++] = 6;
    } else {
      r[used++] = 7;
    }
    memcpy(r + used, line, RADIO_LIST_WIDTH);
    used += RADIO_LIST_WIDTH;
    if (row + 1 < RADIO_LIST_ROWS) {
      r[used++] = 0x0D;
    }
  }
  core->mutex_unlock(P->radio_lock);
  r[used++] = 0;
  core->reply(RESPONSE(AUDIO_VIEW), r, (uint16_t)used);
}

static void command_view(void) {
  /* Метка листания — millis | 1 (не ноль): она бывает на 1 мс впереди — разность со знаком. */
  if (P->radio && P->radio_list_ms != 0 &&
      (int32_t)(core->millis() - P->radio_list_ms) < RADIO_LIST_MS) {
    radio_list_reply(played_ms() / 1000);
    return;
  }
  uint8_t r[6 + VIEW_CHANNELS];
  uint8_t count = 0;
  r[0] = VIEW_NONE;
  if (P->decoded && (P->state == ST_PLAYING || P->state == ST_DRAINING)) {
    if (spectrum_view()) {
      r[0] = VIEW_SPECTRUM;
      count = VIEW_BANDS;
      view_bands(r + 6, P->paused);
    } else {
      r[0] = VIEW_CHANNEL_LEVELS;
      count = P->level_count;
      memcpy(r + 6, P->levels, count);
    }
  }
  r[1] = count;
  const uint32_t duration = P->info.duration_ms / 1000;
  uint32_t position = played_ms() / 1000;
  if (duration != 0 && position > duration) {
    position = duration;                 /* MIDI доигрывает отпускание нот, MP3 VBR — оценка */
  }
  le16(r + 2, duration > 0xFFFF ? 0xFFFF : duration);
  le16(r + 4, position > 0xFFFF ? 0xFFFF : position);
  core->reply(RESPONSE(AUDIO_VIEW), r, (uint16_t)(6 + count));
}

/* [название, 0][исполнитель, 0][альбом, 0][год, 0][номер, 0] в CP866; у модуля
 * за ними [сэмплов LE16][разных эффектов], у MIDI — [инструментов LE16][дорожек],
 * у OPL — [микросхемы, 0] («YMF262 (OPL3)»). */
static void command_tags(void) {
  if (P->radio) {
    radio_tags_reply();
    return;
  }
  P->tags_changed = 0;                      /* поток: метки прочитаны */
  static uint8_t r[64 * 3 + 8 * 2 + sizeof(P->info.format)];
  player_tags_t tags;
  memset(&tags, 0, sizeof(tags));
  const int whole = !P->stream_open && P->format != FMT_MP3 &&
                    (P->state == ST_PLAYING || P->state == ST_DRAINING);
  if (P->stream_open || P->format == FMT_MP3) {
    stream_tags(&tags);
  } else if (whole) {
    /* По 48 знаков: окно шире не покажет, а за метками у OPL — ещё микросхемы
     * (плагин берёт до 200 байт). */
    text_cp866(P->info.title, tags.title, 49);
    text_cp866(P->info.artist, tags.artist, 49);
    text_cp866(P->info.album, tags.album, 49);
    if (P->info.year != 0) {
      snprintf(tags.year, sizeof(tags.year), "%u", (unsigned)P->info.year);
    }
  }
  const char* fields[] = {tags.title, tags.artist, tags.album, tags.year, tags.track};
  size_t used = 0;
  for (int i = 0; i < 5; ++i) {
    const size_t length = strlen(fields[i]);
    memcpy(r + used, fields[i], length + 1);
    used += length + 1;
  }
  if (whole && (P->format == FMT_OPL || P->format == FMT_SAA || P->format == FMT_TSFM)) {
    const char* chips = strncmp(P->info.format, "OPL ", 4) == 0 ? P->info.format + 4 : P->info.format;
    const size_t length = strnlen(chips, sizeof(P->info.format) - 1);
    memcpy(r + used, chips, length);
    r[used + length] = 0;
    used += length + 1;
  } else if (whole) {
    le16(r + used, P->info.samples);
    r[used + 2] = P->info.effects;
    used += 3;
  }
  core->reply(RESPONSE(AUDIO_TAGS), r, (uint16_t)used);
}

static uint8_t thumb_reply[2 + COVER_THUMB_PART];

static void command_thumb(const uint8_t* data, uint16_t length) {
  /* [часть] → [0 сводится / 1 вот / 2 нет][часть][512 байт, если готова] */
  P->cover_found = 0;                     /* плагин пришёл за миниатюрой */
  thumb_reply[1] = (uint8_t)((length != 0 ? data[0] : 0) % COVER_THUMB_PARTS);
  thumb_reply[0] = cover_thumb(thumb_reply[1], thumb_reply + 2);
  core->reply(RESPONSE(AUDIO_THUMB), thumb_reply,
              thumb_reply[0] == THUMB_READY ? (uint16_t)sizeof(thumb_reply) : 2);
}

static int command(uint8_t cmd, const uint8_t* data, uint16_t length) {
  switch (cmd) {
    case AUDIO_OPEN: command_open(data, length); return ZM_OK;
    case AUDIO_DATA: command_data(data, length); return ZM_OK;
    case AUDIO_CLOSE: command_close(data, length); return ZM_OK;
    case AUDIO_STATUS: command_status(); return ZM_OK;
    case AUDIO_VOLUME: {
      if (length != 0) {
        core->set_volume(data[0]);
      }
      const uint8_t ok = length != 0;
      core->reply(RESPONSE(AUDIO_VOLUME), &ok, 1);
      return ZM_OK;
    }
    case AUDIO_PAUSE: {
      P->paused = length != 0 && data[0] != 0;
      core->pcm_pause(P->paused);
      const uint8_t ok = 1;
      core->reply(RESPONSE(AUDIO_PAUSE), &ok, 1);
      return ZM_OK;
    }
    case AUDIO_SEEK: command_seek(data, length); return ZM_OK;
    case AUDIO_FLUSH: command_flush(data, length); return ZM_OK;
    case AUDIO_INFO: command_info(); return ZM_OK;
    case AUDIO_COVER: command_cover(data, length); return ZM_OK;
    case AUDIO_THUMB: command_thumb(data, length); return ZM_OK;
    case AUDIO_PICTURE: command_picture(data, length); return ZM_OK;
    case AUDIO_VIEW: command_view(); return ZM_OK;
    case AUDIO_TAGS: command_tags(); return ZM_OK;
    default:
      return ZM_ERROR;                    /* тон, тест линий — ядро */
  }
}

/* --- жизнь приложения ---------------------------------------------------------------- */

static int start(const zm_core_t* c) {
  core = c;
  P = (player_t*)core->alloc(sizeof(player_t), ZM_MEM_INTERNAL);
  if (P == NULL) {
    return ZM_NO_MEMORY;
  }
  memset(P, 0, sizeof(*P));
  P->ring = P->ring_small = (uint8_t*)core->alloc(RING_BYTES, ZM_MEM_PSRAM);
  P->ring_size = RING_BYTES;
  P->frames = (float*)core->alloc(RENDER_FRAMES * 2 * sizeof(float), ZM_MEM_INTERNAL);
  P->lock = core->mutex_create();
  P->wake = core->signal_create();
  if (P->ring == NULL || P->frames == NULL || P->lock == NULL || P->wake == NULL) {
    return ZM_NO_MEMORY;
  }
  host.size = sizeof(host);
  host.abi_major = ZM_SOURCE_ABI_MAJOR;
  host.abi_minor = ZM_SOURCE_ABI_MINOR;
  host.alloc = core->alloc;
  host.free = core->free;
  host.log = core->log;
  host.input_read = host_input_read;
  host.input_available = host_input_available;
  host.input_ended = host_input_ended;
  host.resource = host_resource;
  host.cover = host_cover;
  host.file_type = P->file_type;
  host.make_room = host_make_room;
  host.input_wait = host_input_wait;
  host.rest = host_rest;                       /* с 1.8: уступка ядра декодеру (ABI 7) */
  /* Помощник на ядре 1: нет задачи или семафоров — декодеры считают всё сами. */
  P->worker_go = core->signal_create();
  P->worker_done = core->signal_create();
  if (P->worker_go != NULL && P->worker_done != NULL &&
      core->task_start(worker_task, NULL, WORKER_STACK, WORKER_PRIORITY, 1) == ZM_OK) {
    host.worker_start = host_worker_start;
    host.worker_wait = host_worker_wait;
  } else {
    core->log("player: no worker on core 1 (decoders do all on core 0)");
  }
  if (cover_begin(core) != ZM_OK) {
    core->log("player: no cover art (no memory)");   /* звук и без неё */
  }
  if (view_begin(core) != ZM_OK) {
    core->log("player: no spectrum (no memory)");
  }
  return core->task_start(decode_task, NULL, DECODE_STACK, TASK_PRIORITY, 0);
}

static void stop(void) {
  if (P == NULL) {
    return;
  }
  /* Задача декодера должна выйти раньше, чем уйдёт декодер: вывод — освободить
   * (запись ждёт места до 500 мс, на паузе очередь не сливается), приём
   * потоком — оборвать. */
  P->stop = 1;
  P->load_abort = 1;
  core->pcm_flush();
  core->pcm_pause(0);
  core->signal_give(P->wake);
  for (int wait = 0; wait < 200 && !P->task_done; ++wait) {
    core->delay(10);
  }
  cover_end();
  if (P->task_done) {
    close_decoder();
  } else {
    /* Не вышла за 2 с: декодер не трогать — ядро снимет задачу и само выгрузит
     * модули приложения (ModuleApp::stopApp); иначе код ушёл бы из-под неё. */
    core->log("player: decode task did not stop in 2 s; the core removes it");
  }
  close_output();
  /* Задача радио: выйти (она могла ждать соединения до RADIO_TIMEOUT_MS). */
  if (P->radio_task_started) {
    P->radio_quit = 1;
    radio_stop();
    core->signal_give(P->radio_wake);
    for (int wait = 0; wait < (RADIO_TIMEOUT_MS + 1000) / 10 && !P->radio_exited; ++wait) {
      core->delay(10);
    }
  }
  if (P->radio_exited && P->stations != NULL) {
    core->free(P->stations);
    P->stations = NULL;
  }
  /* Помощник: выйти (работы у него уже нет — close_decoder её дождался). */
  if (host.worker_start != NULL) {
    P->worker_quit = 1;
    core->signal_give(P->worker_go);
    for (int wait = 0; wait < 200 && !P->worker_exited; ++wait) {
      core->delay(10);
    }
  }
}

int main(int argc, char* argv[]) {
  if (argc < 3) {
    return 0;
  }
  app.size = sizeof(app);
  app.abi_major = ZM_APP_ABI_MAJOR;
  app.abi_minor = ZM_APP_ABI_MINOR;
  app.name = "player";
  app.version = "1.20";
  app.command_first = AUDIO_VIEW;
  app.command_last = AUDIO_TAGS;
  app.start = start;
  app.stop = stop;
  app.command = command;
  app.poll = NULL;
  *(const zm_app_t**)argv[2] = &app;
  return ZM_APP_ABI_MAJOR;
}
