/* TRACKER.ELF — декодер трекерной музыки для плеера VDAC4.
 *
 * MOD, XM, S3M — компонент tracker (libxm и загрузчики TS-Labs,
 * components/tracker); IT и всё, чего tracker не взял, — libxmp
 * (components/libxmp, полная: MED и OctaMED, MultiTracker, Scream Tracker 2,
 * 669, Ultra Tracker, Oktalyzer, Soundtracker на 15 сэмплов, упакованные
 * форматы Amiga…). MOD с изъяном перед разбором чинится (repair_mod): тег
 * сдвинут на 1..3 байта, Soundtracker на 15 сэмплов без тега, файл обрезан —
 * дополняется нулями до длины по заголовку (что уцелело — звучит). Не модуль
 * (текст, ANSI, исполняемый файл) — отказ с записью в журнал. Файл целиком,
 * вид — по содержимому. Контекст
 * tracker держит свои копии нот, а отсчёты XM живут в самом буфере файла
 * (распакованы там на месте), поэтому файл нужен до close (keeps_file = 1);
 * libxmp копирует всё к себе. Песня играет один раз. IT — с усилением libxmp 0
 * (по умолчанию у него ×2: громкие IT упирались в его ограничение ±32767, перегруз)
 * и 32-битным выводом: ослабление не съедает младший бит.
 * IT плеер подаёт потоком (streams_whole, open с file == NULL): libxmp читает
 * его по мере прихода (it_stream.c), в памяти — только её копия отсчётов, и
 * IT больше 6 МиБ тоже играет. Не помещается и она — отсчёты сжимаются при
 * загрузке: стерео в моно, затем 16 бит в 8 (xmp_esp_reduce). Поток не IT —
 * целиком в буфер и как файл целиком.
 * Память tracker — malloc libc (он сам освобождает её через free), libxmp — PSRAM
 * (xmp_malloc, components/libxmp/esp).
 * Громкость (1.6): с усилением libxm 0,25 и libxmp 0 модули звучали на 9–10 дБ
 * тише MIDI и MP3 (EBU R128, 31 файл MOD/XM/S3M/IT: −17…−29 LUFS, середина
 * около −21, у MIDI.ELF −11,7, у MP3 −7,4). Вывод — +9 дБ, а пики выше KNEE
 * мягко сжимаются к 1,0, как в MIDI.ELF: у большинства модулей сжато меньше
 * 2 % отсчётов, у самых громких (ESR-BIT.IT) — до 6 %.
 * Выравнивание по файлу (1.7, на пробу; откат — 1.6): файлы и с +9 дБ
 * расходились до 12 дБ (−8…−20 LUFS). Громкость звука до усиления меряется по
 * ходу песни, как EBU R128 (фильтр K, блоки 400 мс через 100 мс, пороги −70 LUFS
 * и −10 LU), и усиление плавно подводит песню к LEVEL_TARGET: первые
 * LEVEL_WARMUP_STEPS блоков звука песня идёт с прежними +9 дБ, дальше усиление
 * меняется не быстрее LEVEL_SLEW_DB за шаг 100 мс, в пределах
 * LEVEL_MIN_DB…LEVEL_MAX_DB. Пики — тот же мягкий ограничитель (level_*).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "it_stream.h"
#include "tracker.h"
#include "xmp.h"
#include "xmp_esp.h"
#include "zm_app.h"
#include "zm_source.h"
#include "zm_level.h"

#define RATE 44100

typedef struct {
  xm_context_t* context;               /* MOD, XM, S3M */
  xmp_context xmp;                     /* IT */
  struct xmp_frame_info* frame;        /* IT: состояние после последнего render */
  struct xmp_module* module;           /* IT: модуль у libxmp */
  int32_t* pcm;                        /* IT: libxmp пишет 32 бита (XMP_FORMAT_32BIT) */
  uint32_t pcm_frames;
  uint8_t* repaired;                   /* починенная копия MOD (repair_mod) или NULL */
  uint8_t* owned;                      /* файл из потока целиком (не IT): свой буфер */
  uint32_t truncated;                  /* MOD обрезан до первого сэмпла: длина по заголовку */
  char format[4];
  uint64_t frames;
  uint64_t song_frames;                /* MOD, XM, S3M: место в песне, кадров (и после перемотки) */
  uint32_t duration_ms;                /* MOD, XM, S3M: длительность прогоном (0 — неизвестна) */
  uint32_t order_ms[256];              /* начало каждой позиции таблицы порядка, мс */
  uint16_t samples;                    /* сэмплов с данными */
  uint8_t effects;                     /* разных эффектов в узорах */
  zm_level_t level;                     /* выравнивание громкости (1.7) */
} tracker_t;

/* Отметить эффект в наборе (256 бит) и вернуть, сколько отмечено всего. */
static uint32_t count_bits(const uint8_t* used) {
  uint32_t count = 0;
  for (int i = 0; i < 256; ++i) {
    count += (used[i >> 3] >> (i & 7)) & 1u;
  }
  return count;
}

static const zm_host_t* host;
static zm_source_t table;

/* Контекст libxm (MOD, XM, S3M) — одним куском. Не нашлось — ресурсы, которые
 * трекеру не нужны (банк MIDI, ПЗУ OPL4, набор MWM), прочь (make_room: плеер
 * отпускает их, если файл в буфере ещё поместился с ними) и ещё раз: иначе
 * модуль, которому мешал банк, выдавал себя за «не музыку». */
static void* context_alloc(size_t size) {
  void* context = malloc(size);
  if (context == NULL && host->size >= offsetof(zm_host_t, make_room) + sizeof(host->make_room) &&
      host->make_room != NULL && host->make_room(size)) {
    context = malloc(size);
  }
  return context;
}

static int probe(const uint8_t* head, size_t length) {
  if (length >= 17 && memcmp(head, "Extended Module:", 16) == 0) {
    return 100;
  }
  if (length >= 48 && memcmp(head + 44, "SCRM", 4) == 0) {
    return 100;
  }
  if (length >= 4 && memcmp(head, "IMPM", 4) == 0) {
    return 100;
  }
  if (mod_check_sanity_preload(head, length) == 0) {
    return 50;
  }
  return 0;
}

/* Для окна плеера: сэмплы, в которых есть звук, и разные эффекты узоров (столбец
 * эффекта; «0 00» — пусто). */
static void count_contents(tracker_t* t) {
  const xm_module_t* module = &t->context->module;
  uint32_t samples = 0;
  for (uint16_t i = 0; i < module->num_instruments; ++i) {
    const xm_instrument_t* instrument = &module->instruments[i];
    for (uint16_t s = 0; s < instrument->num_samples; ++s) {
      if (instrument->samples[s].length != 0) {
        ++samples;
      }
    }
  }
  uint8_t used[32];
  memset(used, 0, sizeof(used));
  for (uint16_t p = 0; p < module->num_patterns; ++p) {
    const xm_pattern_t* pattern = &module->patterns[p];
    const uint32_t slots = (uint32_t)pattern->num_rows * module->num_channels;
    for (uint32_t i = 0; pattern->slots != NULL && i < slots; ++i) {
      const xm_pattern_slot_t* slot = &pattern->slots[i];
      if (slot->effect_type != 0 || slot->effect_param != 0) {
        used[slot->effect_type >> 3] |= (uint8_t)(1u << (slot->effect_type & 7));
      }
    }
  }
  const uint32_t effects = count_bits(used);
  t->samples = samples > 0xFFFF ? 0xFFFF : (uint16_t)samples;
  t->effects = effects > 0xFF ? 0xFF : (uint8_t)effects;
}

/* Тег MOD по смещению 1080: число каналов; 0 — не тег. */
static int mod_tag_channels(const uint8_t* tag) {
  static const char four[][4] = {{'M', '.', 'K', '.'}, {'M', '!', 'K', '!'}, {'M', '&', 'K', '!'},
                                 {'N', '.', 'T', '.'}, {'F', 'L', 'T', '4'}, {'F', 'E', 'S', 'T'},
                                 {'L', 'A', 'R', 'D'}, {'N', 'S', 'M', 'S'}, {'E', 'X', 'O', '4'}};
  for (size_t i = 0; i < sizeof(four) / sizeof(four[0]); ++i) {
    if (memcmp(tag, four[i], 4) == 0) {
      return 4;
    }
  }
  if (memcmp(tag, "FLT8", 4) == 0 || memcmp(tag, "CD81", 4) == 0 || memcmp(tag, "OKTA", 4) == 0 ||
      memcmp(tag, "OCTA", 4) == 0) {
    return 8;
  }
  if (memcmp(tag + 1, "CHN", 3) == 0 && tag[0] >= '1' && tag[0] <= '9') {
    return tag[0] - '0';
  }
  if (memcmp(tag + 2, "CH", 2) == 0 && tag[0] >= '0' && tag[0] <= '9' && tag[1] >= '0' &&
      tag[1] <= '9') {
    return (tag[0] - '0') * 10 + (tag[1] - '0');
  }
  if (memcmp(tag, "TDZ", 3) == 0 && tag[3] >= '1' && tag[3] <= '9') {
    return tag[3] - '0';
  }
  return 0;
}

/* Длина MOD на 31 сэмпл по его заголовку: 1084 + узоры + отсчёты. */
static uint32_t mod_full_size(const uint8_t* header, int channels) {
  uint32_t samples = 0;
  for (int i = 0; i < 31; ++i) {
    const uint8_t* h = header + 20 + 30 * i;
    samples += (((uint32_t)h[22] << 8) | h[23]) * 2u;
  }
  uint32_t patterns = 0;
  for (int i = 0; i < 128; ++i) {
    patterns = header[952 + i] > patterns ? header[952 + i] : patterns;
  }
  return 1084 + (patterns + 1) * 64u * 4u * (uint32_t)channels + samples;
}

/* Заголовок сэмпла MOD правдоподобен: имя из текста или нулей, громкость до 64. */
static int mod_sample_plausible(const uint8_t* h) {
  int strange = 0;
  for (int c = 0; c < 22; ++c) {
    strange += h[c] != 0 && (h[c] < 32 || h[c] > 126);
  }
  return strange <= 2 && h[24] <= 15 && h[25] <= 64;
}

/* MOD на 31 сэмпл с незнакомым тегом (WN\0& вместо M.K.): все заголовки сэмплов
 * правдоподобны, длина песни 1..128, узоры до 127. */
static int mod31_plausible(const uint8_t* data) {
  for (int i = 0; i < 31; ++i) {
    if (!mod_sample_plausible(data + 20 + 30 * i)) {
      return 0;
    }
  }
  if (data[950] == 0 || data[950] > 128) {
    return 0;
  }
  for (int i = 0; i < 128; ++i) {
    if (data[952 + i] > 127) {
      return 0;
    }
  }
  return 1;
}

/* Soundtracker на 15 сэмплов (тега нет): заголовки сэмплов правдоподобны —
 * имена из текста или нулей, громкость до 64, длина песни 1..128, узоры до 63. */
static int st15_plausible(const uint8_t* data, uint32_t size) {
  if (size < 600 + 1024) {
    return 0;
  }
  for (int i = 0; i < 15; ++i) {
    if (!mod_sample_plausible(data + 20 + 30 * i)) {
      return 0;
    }
  }
  if (data[470] == 0 || data[470] > 128) {
    return 0;
  }
  for (int i = 0; i < 128; ++i) {
    if (data[472 + i] > 63) {
      return 0;
    }
  }
  return 1;
}

/* MOD с изъяном — в новый буфер на 31 сэмпл с тегом на месте и полной длиной по
 * заголовку (недостающее — нули). NULL — чинить нечего или это не MOD. */
static const uint8_t* repair_mod(tracker_t* t, const uint8_t* file, uint32_t* size) {
  const uint32_t length = *size;
  if (length < 1084) {
    return NULL;
  }
  int channels = mod_tag_channels(file + 1080);
  int shift = 0;
  int st15 = 0;
  int retag = 0;
  for (int s = 1; channels == 0 && s <= 3; ++s) {
    channels = mod_tag_channels(file + 1080 + s);
    shift = channels != 0 ? s : 0;
  }
  if (channels == 0 && mod31_plausible(file) && mod_full_size(file, 4) == length) {
    retag = 1;                                        /* тег испорчен, остальное — M.K. */
    channels = 4;
  }
  if (channels == 0) {
    if (!st15_plausible(file, length)) {
      return NULL;
    }
    st15 = 1;
    channels = 4;
  }
  uint8_t header[1084];
  if (st15) {
    memcpy(header, file, 20 + 15 * 30);              /* название и 15 сэмплов */
    memset(header + 470, 0, 16 * 30);                /* ещё 16 — пустые */
    memcpy(header + 950, file + 470, 130);           /* длина, повтор, порядок */
    memcpy(header + 1080, "M.K.", 4);
  } else {
    memcpy(header, file, 1080);
    memcpy(header + 1080, retag ? (const uint8_t*)"M.K." : file + 1080 + shift, 4);
  }
  const uint32_t head = st15 ? 600 : 1084 + (uint32_t)shift;   /* где в файле начались узоры */
  const uint32_t body = length > head ? length - head : 0;
  const uint32_t need = mod_full_size(header, channels);
  const uint32_t total = need > 1084 + body ? need : 1084 + body;
  if (!st15 && !retag && shift == 0 && total <= length) {
    return NULL;                                      /* целый: чинить нечего */
  }
  uint32_t patterns = 0;
  for (int i = 0; i < 128; ++i) {
    patterns = header[952 + i] > patterns ? header[952 + i] : patterns;
  }
  if (need > 1084 + body && body <= (patterns + 1) * 64u * 4u * (uint32_t)channels) {
    t->truncated = need;                              /* обрезан до первого сэмпла: звучать нечему */
    return NULL;
  }
  uint8_t* fixed = (uint8_t*)host->alloc(total + ZM_WHOLE_SLACK, ZM_MEM_PSRAM);
  if (fixed == NULL) {
    return NULL;
  }
  memcpy(fixed, header, 1084);
  memcpy(fixed + 1084, file + head, body);
  memset(fixed + 1084 + body, 0, total + ZM_WHOLE_SLACK - 1084 - body);
  t->repaired = fixed;
  *size = total;
  host->log("tracker: MOD repaired:%s%s%s %u of %u bytes present", st15 ? " 15 samples -> 31," : "",
            shift ? " tag shifted," : "", retag ? " tag -> M.K.," : "", (unsigned)(1084 + body),
            (unsigned)total);
  return fixed;
}

/* libxmp: модуль загружен (result 0) — проигрыватель на 44,1 кГц, линейная
 * интерполяция (до 64 каналов на ESP32-S3), конец песни — конец звука. 0 — не
 * вышло (контекст освобождён). */
static int start_xmp(tracker_t* t, int result) {
  if (result == 0) {
    result = xmp_start_player(t->xmp, RATE, XMP_FORMAT_32BIT);
    if (result != 0) {
      xmp_release_module(t->xmp);
    }
  }
  t->frame = (struct xmp_frame_info*)host->alloc(sizeof(struct xmp_frame_info), ZM_MEM_PSRAM);
  if (result != 0 || t->frame == NULL) {
    if (result == 0) {
      xmp_end_player(t->xmp);
      xmp_release_module(t->xmp);
    }
    host->free(t->frame);
    t->frame = NULL;
    xmp_free_context(t->xmp);
    t->xmp = NULL;
    return 0;
  }
  xmp_set_player(t->xmp, XMP_PLAYER_INTERP, XMP_INTERP_LINEAR);
  xmp_set_player(t->xmp, XMP_PLAYER_AMP, 0);      /* без ×2: запас против перегруза */
  memset(t->frame, 0, sizeof(*t->frame));
  struct xmp_module_info module_info;
  xmp_get_module_info(t->xmp, &module_info);
  t->module = module_info.mod;
  uint32_t samples = 0;
  for (int i = 0; i < t->module->smp; ++i) {
    if (t->module->xxs[i].len > 0) {
      ++samples;
    }
  }
  uint8_t used[32];
  memset(used, 0, sizeof(used));
  for (int i = 0; i < t->module->trk; ++i) {
    const struct xmp_track* track = t->module->xxt[i];
    for (int row = 0; track != NULL && row < track->rows; ++row) {
      const struct xmp_event* event = &track->event[row];
      if (event->fxt != 0 || event->fxp != 0) {
        used[event->fxt >> 3] |= (uint8_t)(1u << (event->fxt & 7));
      }
      if (event->f2t != 0 || event->f2p != 0) {
        used[event->f2t >> 3] |= (uint8_t)(1u << (event->f2t & 7));
      }
    }
  }
  const uint32_t effects = count_bits(used);
  t->samples = samples > 0xFFFF ? 0xFFFF : (uint16_t)samples;
  t->effects = effects > 0xFF ? 0xFF : (uint8_t)effects;
  return 1;
}

/* libxmp: загрузка из памяти. 0 — не загрузился. */
static int open_xmp(tracker_t* t, const uint8_t* file, uint32_t file_size) {
  t->xmp = xmp_create_context();
  if (t->xmp == NULL) {
    return 0;
  }
  return start_xmp(t, xmp_load_module_from_memory(t->xmp, file, (long)file_size));
}

static void* open_whole(uint32_t file_size, const uint8_t* file);

/* Поток не по плану IT (не IT или план невозможен) — целиком в свой буфер,
 * дальше как файл целиком. */
static void* open_stream_whole(it_stream_t* s, uint32_t file_size) {
  uint8_t* file = NULL;
  /* Файл и копия декодера — 2·размер с запасом: у огромного заявленного размера сумма
   * переполнялась, проверялось бы почти ничего, а it_stream_rest копировал весь файл. */
  if (file_size > (UINT32_MAX - ZM_WHOLE_SLACK) / 2) {
    host->log("tracker: %u bytes streamed: too big to keep whole", (unsigned)file_size);
    return NULL;
  }
  if (host->make_room(2u * file_size + ZM_WHOLE_SLACK)) {   /* файл и копия декодера */
    file = (uint8_t*)host->alloc(file_size + ZM_WHOLE_SLACK, ZM_MEM_PSRAM);
  }
  if (file == NULL || it_stream_rest(s, file) != 0) {
    host->log("tracker: %u bytes streamed: no room or stream broken", (unsigned)file_size);
    host->free(file);
    return NULL;
  }
  memset(file + file_size, 0, ZM_WHOLE_SLACK);
  it_stream_end(s);
  tracker_t* t = (tracker_t*)open_whole(file_size, file);
  if (t == NULL) {
    host->free(file);
    return NULL;
  }
  t->owned = file;
  return t;
}

/* IT потоком: план по голове файла, место под копию libxmp — с ресурсами или без
 * них, не хватает — отсчёты сжимаются (моно, затем 8 бит). */
static void* open_stream(uint32_t file_size) {
  if (host->size < offsetof(zm_host_t, input_wait) + sizeof(host->input_wait) ||
      host->make_room == NULL || host->input_wait == NULL) {
    return NULL;
  }
  it_stream_t* s = (it_stream_t*)host->alloc(sizeof(it_stream_t), ZM_MEM_PSRAM);
  if (s == NULL) {
    return NULL;
  }
  if (it_stream_begin(s, host, file_size) != 0) {
    void* whole = NULL;
    if (!s->failed) {
      whole = open_stream_whole(s, file_size);
    }
    it_stream_end(s);
    host->free(s);
    return whole;
  }
  static const char* const level_names[IT_LEVELS] = {"as is", "stereo -> mono", "mono, 16 -> 8 bit"};
  /* Голова уже в памяти: просить место без неё. */
  int level = IT_FULL;
  while (level < IT_LEVELS &&
         !host->make_room(s->memory[level] > s->head_capacity ? s->memory[level] - s->head_capacity : 0)) {
    ++level;
  }
  tracker_t* t = NULL;
  int result = -1;
  if (level < IT_LEVELS) {
    t = (tracker_t*)host->alloc(sizeof(tracker_t), ZM_MEM_INTERNAL);
  }
  if (t != NULL) {
    memset(t, 0, sizeof(*t));
    strcpy(t->format, "IT");
    t->xmp = xmp_create_context();
  }
  if (t != NULL && t->xmp != NULL) {
    struct xmp_callbacks callbacks = {it_stream_read, it_stream_seek, it_stream_tell,
                                      it_stream_close};
    xmp_esp_reduce(level == IT_FULL ? 0 : (level == IT_MONO ? XMP_ESP_REDUCE_MONO
                                                            : XMP_ESP_REDUCE_MONO | XMP_ESP_REDUCE_8BIT));
    result = xmp_load_module_from_callbacks(t->xmp, s, callbacks);
    xmp_esp_reduce(0);
    if (result == 0 && s->failed) {
      xmp_release_module(t->xmp);                     /* поток оборвался: отсчёты неполные */
      result = -1;
    }
  }
  host->log("tracker: IT of %u bytes streamed: %s, libxmp %u bytes (%u / %u / %u by plan), "
            "kept %u, %d channels, %d stereo, %d 16-bit samples, result %d",
            (unsigned)file_size, level < IT_LEVELS ? level_names[level] : "does not fit",
            (unsigned)(level < IT_LEVELS ? s->memory[level] : 0), (unsigned)s->memory[IT_FULL],
            (unsigned)s->memory[IT_MONO], (unsigned)s->memory[IT_MONO_8BIT], (unsigned)s->kept,
            s->channels, s->stereo, s->sixteen, result);
  it_stream_end(s);
  host->free(s);
  if (t == NULL) {
    return NULL;
  }
  if (t->xmp == NULL || !start_xmp(t, result)) {     /* start_xmp освободил контекст */
    host->free(t);
    return NULL;
  }
  return t;
}

static void* open_source(uint32_t file_size, const uint8_t* file) {
  if (file_size == 0) {
    return NULL;
  }
  if (file == NULL) {
    return open_stream(file_size);
  }
  return open_whole(file_size, file);
}

static void* open_whole(uint32_t file_size, const uint8_t* file) {
  tracker_t* t = (tracker_t*)host->alloc(sizeof(tracker_t), ZM_MEM_INTERNAL);
  if (t == NULL) {
    return NULL;
  }
  memset(t, 0, sizeof(*t));
  int result = -1;
  size_t needed = 0;
  void* data = (void*)file;
  if (file_size >= 4 && memcmp(file, "IMPM", 4) == 0) {
    strcpy(t->format, "IT");
    if (!open_xmp(t, file, file_size)) {
      host->log("tracker: IT of %u bytes not loaded", (unsigned)file_size);
      host->free(t);
      return NULL;
    }
    return t;
  }
  /* MOD с изъяном (тег не на месте, 15 сэмплов, обрезан) — сначала починить. */
  const uint8_t* repaired = NULL;
  if (memcmp(file, "Extended Module:", file_size >= 16 ? 16 : file_size) != 0 &&
      (file_size < 48 || memcmp(file + 44, "SCRM", 4) != 0)) {
    uint32_t fixed_size = file_size;
    repaired = repair_mod(t, file, &fixed_size);
    if (t->truncated != 0) {
      host->log("tracker: MOD cut to %u of %u bytes, no sample left", (unsigned)file_size,
                (unsigned)t->truncated);
      host->free(t);
      return NULL;
    }
    if (repaired != NULL) {
      file = repaired;
      file_size = fixed_size;
      data = (void*)file;
    }
  }
  if (file_size >= 17 && memcmp(file, "Extended Module:", 16) == 0) {
    strcpy(t->format, "XM");
    if (host->abi_minor >= 2) {
      /* Отсчёты — в буфере файла: память почти вдвое меньше (keeps_file). */
      result = xm_create_context_in_place(&t->context, data, file_size, file_size + ZM_WHOLE_SLACK,
                                          RATE, context_alloc);
    } else {
      result = xm_create_context_safe(&t->context, data, file_size, RATE, context_alloc);
    }
  } else if (file_size >= 48 && memcmp(file + 44, "SCRM", 4) == 0) {
    strcpy(t->format, "S3M");
    result = s3m_create_context_safe(&t->context, data, file_size, RATE, context_alloc, &needed);
  } else if (mod_check_sanity_preload(file, file_size) == 0) {
    strcpy(t->format, "MOD");
    result = mod_create_context_safe(&t->context, data, file_size, RATE, context_alloc, &needed);
  }
  if (result < 0 || t->context == NULL) {
    if (t->context != NULL) {
      xm_free_context(t->context);
      t->context = NULL;
    }
    /* tracker не взял — libxmp: её форматы и то, что он отверг. */
    if (open_xmp(t, file, file_size)) {
      host->log("tracker: %u bytes played by libxmp: %s", (unsigned)file_size, t->module->type);
      strcpy(t->format, file_size >= 4 && memcmp(file, "IMPM", 4) == 0 ? "IT" : "MOD");
      return t;
    }
    host->log("tracker: %u bytes: not a music module (format %s, result %d)", (unsigned)file_size,
              t->format[0] ? t->format : "?", result);
    host->free(t->repaired);
    host->free(t);
    return NULL;
  }
  xm_set_max_loop_count(t->context, 1);
  count_contents(t);
  /* Длительность и начала позиций — прогоном песни без звука (не дольше часа). */
  t->duration_ms = xm_scan_song(t->context, t->order_ms, 60u * 60u * 1000u);
  return t;
}

static int render_it(tracker_t* t, float* out, uint32_t max_frames, uint32_t* frames) {
  if (t->pcm_frames < max_frames) {
    host->free(t->pcm);
    t->pcm = (int32_t*)host->alloc(max_frames * 2 * sizeof(int32_t), ZM_MEM_INTERNAL);
    t->pcm_frames = t->pcm != NULL ? max_frames : 0;
    if (t->pcm == NULL) {
      return ZM_RENDER_ERROR;
    }
  }
  const int result = xmp_play_buffer(t->xmp, t->pcm, (int)(max_frames * 2 * sizeof(int32_t)), 0);
  if (result == -XMP_END) {
    return ZM_RENDER_END;              /* песня кончилась */
  }
  if (result != 0) {
    return ZM_RENDER_ERROR;
  }
  xmp_get_frame_info(t->xmp, t->frame);
  for (uint32_t i = 0; i < max_frames * 2; ++i) {
    out[i] = (float)t->pcm[i] * (1.0f / 2147483648.0f);
  }
  t->frames += max_frames;
  *frames = max_frames;
  return ZM_RENDER_OK;
}

static void level(tracker_t* t, float* samples, uint32_t frames) {
  if (!t->level.ready) {
    zm_level_init(&t->level, RATE, ZM_LEVEL_BASE_DB, ZM_LEVEL_MIN_DB);
  }
  zm_level_process(&t->level, samples, frames);
}

static int render(void* instance, float* out, uint32_t max_frames, uint32_t* frames) {
  tracker_t* t = (tracker_t*)instance;
  *frames = 0;
  if (t->xmp != NULL) {
    const int result = render_it(t, out, max_frames, frames);
    level(t, out, *frames);
    return result;
  }
  if (xm_get_loop_count(t->context) > 0) {
    return ZM_RENDER_END;              /* песня кончилась: дальше была бы тишина */
  }
  xm_render(t->context, out, max_frames);
  level(t, out, max_frames);
  t->frames += max_frames;
  t->song_frames += max_frames;
  *frames = max_frames;
  return ZM_RENDER_OK;
}

/* Перемотка на delta позиций таблицы порядка. */
static int seek(void* instance, int32_t delta, uint32_t* file_offset) {
  tracker_t* t = (tracker_t*)instance;
  *file_offset = UINT32_MAX;
  if (t->xmp != NULL) {
    int32_t target = t->frame->pos + delta;
    if (target < 0) {
      target = 0;
    }
    if (target >= t->module->len) {
      target = t->module->len > 0 ? t->module->len - 1 : 0;
    }
    xmp_set_position(t->xmp, target);
    t->frame->pos = target;
    t->frame->row = 0;
    return ZM_OK;
  }
  const int32_t length = xm_get_module_length(t->context);
  uint8_t index = 0;
  xm_get_position(t->context, &index, NULL, NULL, NULL);
  int32_t target = (int32_t)index + delta;
  if (target < 0) {
    target = 0;
  }
  if (target >= length) {
    target = length > 0 ? length - 1 : 0;
  }
  xm_seek(t->context, (uint8_t)target, 0, 0);
  if (t->order_ms[target] != UINT32_MAX) {
    t->song_frames = (uint64_t)t->order_ms[target] * RATE / 1000;   /* время — с этой позиции */
  }
  *file_offset = UINT32_MAX;
  return ZM_OK;
}

static void info(const void* instance, zm_info_t* out) {
  const tracker_t* t = (const tracker_t*)instance;
  xm_context_t* context = t->context;
  memset(out, 0, sizeof(*out));
  strcpy(out->format, t->format);
  out->rate = RATE;
  out->channels = 2;
  out->position_ms = (uint32_t)(t->frames * 1000 / RATE);
  out->samples = t->samples;
  out->effects = t->effects;
  if (t->xmp != NULL) {
    out->position_ms = t->frame->time > 0 ? (uint32_t)t->frame->time : 0;   /* и после перемотки */
    strncpy(out->title, t->module->name, sizeof(out->title) - 1);
    for (size_t end = strlen(out->title); end > 0 && out->title[end - 1] == ' '; --end) {
      out->title[end - 1] = 0;
    }
    out->order = (uint16_t)t->frame->pos;
    out->order_length = (uint16_t)t->module->len;
    out->row = (uint8_t)t->frame->row;
    out->voices = (uint8_t)t->module->chn;
    out->duration_ms = t->frame->total_time > 0 ? (uint32_t)t->frame->total_time : 0;
    return;
  }
  strncpy(out->title, xm_get_module_name(context), sizeof(out->title) - 1);
  for (size_t end = strlen(out->title); end > 0 && out->title[end - 1] == ' '; --end) {
    out->title[end - 1] = 0;               /* XM дополняет название пробелами */
  }
  uint8_t index = 0;
  uint8_t row = 0;
  xm_get_position(context, &index, NULL, &row, NULL);
  out->order = index;
  out->order_length = xm_get_module_length(context);
  out->row = row;
  out->voices = (uint8_t)xm_get_number_of_channels(context);
  out->samples = t->samples;
  out->effects = t->effects;
  out->duration_ms = t->duration_ms;
  out->position_ms = (uint32_t)(t->song_frames * 1000 / RATE);
}

/* Уровни каналов: громкость каждого канала сейчас, 0..255. */
static int levels(const void* instance, uint8_t* out, int max) {
  const tracker_t* t = (const tracker_t*)instance;
  if (t->xmp != NULL) {
    int count = t->module->chn;
    if (count > max) {
      count = max;
    }
    for (int channel = 0; channel < count; ++channel) {
      const struct xmp_channel_info* info = &t->frame->channel_info[channel];
      const unsigned volume = info->period != 0 ? info->volume : 0;    /* 0..64; нота кончилась — 0 */
      out[channel] = (uint8_t)(volume >= 64 ? 255 : volume * 4);
    }
    return count;
  }
  int count = xm_get_number_of_channels(t->context);
  if (count > max) {
    count = max;
  }
  for (int channel = 0; channel < count; ++channel) {
    /* Отсчёт кончился (не петля) — канал молчит, хоть громкость и осталась. */
    float volume = xm_is_channel_active(t->context, (uint16_t)(channel + 1))
                       ? xm_get_volume_of_channel(t->context, (uint16_t)(channel + 1))
                       : 0.0f;
    if (volume < 0.0f) {
      volume = 0.0f;
    } else if (volume > 1.0f) {
      volume = 1.0f;
    }
    out[channel] = (uint8_t)(volume * 255.0f);
  }
  return count;
}

static void close_source(void* instance) {
  tracker_t* t = (tracker_t*)instance;
  if (t->level.ready) {
    /* Громкость песни до усиления и усиление к концу — в десятых. */
    const int song = (int)lroundf(t->level.song_lufs * 10.0f);
    const int gain = (int)lroundf(t->level.gain_db * 10.0f);
    host->log("tracker: level %s%d.%d LUFS before gain, gain %s%d.%d dB (target %d LUFS)",
              song < 0 ? "-" : "", abs(song) / 10, abs(song) % 10, gain < 0 ? "-" : "+",
              abs(gain) / 10, abs(gain) % 10, (int)ZM_LEVEL_TARGET);
  }
  if (t->context != NULL) {
    xm_free_context(t->context);
  }
  if (t->xmp != NULL) {
    xmp_end_player(t->xmp);
    xmp_release_module(t->xmp);
    xmp_free_context(t->xmp);
  }
  host->free(t->frame);
  host->free(t->pcm);
  host->free(t->repaired);
  host->free(t->owned);
  host->free(t);
}

int main(int argc, char* argv[]) {
  if (argc < 3) {
    return 0;
  }
  host = (const zm_host_t*)argv[1];
  table.size = sizeof(table);
  table.abi_major = ZM_SOURCE_ABI_MAJOR;
  table.abi_minor = ZM_SOURCE_ABI_MINOR;
  table.name = "tracker";
  table.version = "1.8";
  table.extensions = "MOD MO XM S3M IT MED MTM 669 STM ULT OKT FAR PTM DBM MDL AMF UMX";
  table.input = ZM_INPUT_WHOLE;
  table.keeps_file = 1;
  table.seek_unit = ZM_SEEK_POSITION;
  table.resources = NULL;
  table.probe = probe;
  table.open = open_source;
  table.render = render;
  table.seek = seek;
  table.info = info;
  table.levels = levels;
  table.close = close_source;
  table.streams_whole = 1;
  *(const zm_source_t**)argv[2] = &table;
  return ZM_SOURCE_ABI_MAJOR;
}
