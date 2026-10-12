/* MIDI.ELF — декодер MIDI (SMF 0 и 1, RIFF RMID) для плеера VDAC4.
 *
 * Синтез — TinySoundFont (components/tsf: 16-битные отсчёты банка читаются
 * прямо из него, кубическая интерполяция) по банку General MIDI в SoundFont2 —
 * ресурсу MIDI.SF2 из /WC/ESP/: плагин отдаёт его ESP один раз после
 * включения, ядро держит его в PSRAM. Песню разбирает TinyMidiLoader в список
 * событий со временем в мс и держит у себя, поэтому файл после open не нужен
 * (keeps_file = 0).
 *
 * Ноты (нажатие и отпускание) звучат со своего места с точностью EVENT_QUANTUM кадров:
 * кусок кончается на ноте, которая наступает внутри него (с 1.6; прежде все события до
 * конца куска применялись перед ним — до 1,45 мс раньше, а нажатие и отпускание в одном
 * куске — оба до первого отсчёта, и нота с атакой пропадала). Контроллеры, программы и
 * колесо внутри куска — с его начала, как прежде. Перемотка (мс): звучащие голоса сразу
 * замолкают (с 1.6; прежде гасли 10 мс и звучали прежним местом на новом), программы,
 * контроллеры, колесо и SysEx проигрываются от начала песни до нового места без нот, и
 * ноты, которые там звучат (клавиша нажата или её держит педаль), берутся заново (с 1.7;
 * кроме ударных).
 *
 * Режимы (с 1.7) — по сбросам SysEx (tml.h: GM, GM2, GS, XG): ударный канал — по
 * назначению партии GS (SysEx Use for Rhythm Part), банку XG (127, 126) или GM2 (120;
 * 121 — мелодический), иначе канал 9. SysEx общей громкости — плавно, общей настройки и
 * сдвига тональности GS — у мелодических партий. Нет пресета в банке — канал молчит
 * (в журнал — один раз). В конце песни клавиши и педали отпускаются, хвост — не дольше
 * TAIL_MS, затем затухание FADE_FRAMES (прежде звук обрывался через 4 с).
 *
 * Эффекты каналов, как у GS-модулей: каждый голос посылает свой звук (моно,
 * до панорамы) в три шины — реверберация (CC91, по умолчанию 40), хорус (CC93)
 * и эхо (CC94); шины обрабатываются здесь же и прибавляются к сухому звуку.
 * Если плеер дал помощника на ядре 1 (host->worker_start, ABI источников 6),
 * кусок считают два ядра (с 1.3): помощник — эффекты прошлого куска и первые
 * split_voices звучащих голосов этого, ядро 0 — остальные голоса (TSF считает
 * голоса диапазоном мест, у каждого ядра свои буферы). Граница подстраивается по
 * тактам обоих ядер: у кого работа дольше, тому голосом меньше. Эффекты ложатся в
 * звук на кусок позже (BLOCK кадров, 1,45 мс). До 1.3 помощник считал одни
 * эффекты, и на плотном месте (48 голосов) ядру 0 не хватало тактов на синтез.
 * Эффект, в который давно ничего не посылали и чей хвост отзвучал, не считается.
 * Не успевают ядра (такты на кадр в среднем выше LOAD_HIGH) — предел голосов ниже, и в
 * плотном месте гаснут самые тихие, а звук не тормозит (с 1.4).
 * Реверберация — Freeverb Джезара (общественное достояние): 8 гребенчатых
 * фильтров и 4 фазовращателя на канал; хорус — две отводки модулированной
 * задержки в квадратуре; эхо — задержка с затухающей обратной связью, слева
 * и справа разное время. Портаменто (CC5/65/84), модуляторы SF2 (скорость, CC1, CC7,
 * CC10, CC11, CC71..75, CC91, CC93, давление; свои модуляторы банка), педали и NRPN GS/XG
 * — в синтезаторе (components/tsf).
 *
 * Выход (с 1.7): постоянная составляющая прочь (фильтр 5 Гц), общая громкость SysEx,
 * связанный стереоограничитель — оба канала одним усилением по большему из них, атака
 * мгновенная, отпускание ~150 мс, потолок LIMIT_LEVEL. Прежде каждый отсчёт выше 0,75
 * сжимался сам по себе: гармоники на плотных аккордах и сдвиг стереобаланса.
 *
 * Память TSF и TinyMidiLoader — у плеера (host->alloc: в счёт PSRAM стенда), долгий
 * разбор песни и банка уступает ядро (host->rest, ABI источников 7).
 */
#include <stddef.h>
#include <string.h>

#include "tml.h"
#include "tsf.h"
#include "tsf_memory.h"
#include "zm_app.h"
#include "zm_source.h"

#define RATE 44100
#define BANK "MIDI.SF2"
#define MAX_VOICES 48            /* голосов одновременно (предел синтезатора) */
#define RESERVE_VOICES 8         /* запасные места: гаснущие (10 мс) голоса отданных нот */
#define MIN_VOICES 16            /* ниже предел по нагрузке не опускается */
#define CPU_HZ 240000000u
#define FRAME_CYCLES (CPU_HZ / RATE)            /* 5442 такта на кадр — реальное время */
#define LOAD_HIGH (FRAME_CYCLES * 92u / 100u)   /* дольше в среднем — голосов меньше */
#define LOAD_LOW (FRAME_CYCLES * 75u / 100u)    /* короче — можно больше */
#define GAIN_DB (-4.0f)          /* запас: аккорды из многих голосов складываются; с 1.6
                                    −4 вместо −6: затухание зон SF2 по шкале EMU10K1 (tsf.h)
                                    сделало песни тише на 1,2–2,8 дБ (в среднем 1,9) */
#define LIMIT_LEVEL 0.89f        /* потолок ограничителя (−1 дБ) */
#define LIMIT_RELEASE 0.99985f   /* отпускание ограничителя за отсчёт: ~150 мс */
#define DC_POLE 0.99929f         /* фильтр постоянной составляющей: 5 Гц */
#define BLOCK 64                 /* кадров в куске (TSF_RENDER_EFFECTSAMPLEBLOCK) */
#define EVENT_QUANTUM 16         /* место ноты в куске — с точностью до 16 кадров (0,36 мс):
                                    не больше четырёх частей на кусок, такты голосов на часть — те же */
#define TAIL_MS 10000            /* после последнего события — не дольше (затем затухание) */
#define FADE_FRAMES (RATE / 5)   /* затухание в конце: 200 мс */
#define MISSING_LOGGED 8         /* отсутствующих пресетов в журнал — не больше */
#define REST_EVENTS 256          /* событий подряд в render, затем — уступить ядро */
#define QUIET_LEVEL 0.001f       /* -60 дБ: хвост эффектов отзвучал */
#define QUIET_FRAMES (RATE / 4)
#define DRUMS 9                  /* канал ударных General MIDI */
#define CHANNELS 16

/* Реверберация: Freeverb, длины для 44,1 кГц. */
#define COMBS 8
#define ALLPASSES 4
#define SPREAD 23                /* правый канал длиннее: стерео */
#define REVERB_GAIN 0.03f        /* вход шины (у Freeverb — 0,015 на сумму L+R) */
#define REVERB_FEEDBACK 0.84f    /* «комната» 0,5: хвост около 2 с */
#define REVERB_DAMP 0.2f         /* затухание высоких в хвосте */
#define REVERB_WET 1.0f
static const uint16_t comb_lengths[COMBS] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
static const uint16_t allpass_lengths[ALLPASSES] = {556, 441, 341, 225};

/* Хорус: задержка 13 мс, качание ±4 мс с частотой 0,5 Гц, отводки L и R в квадратуре. */
#define CHORUS_LENGTH 2048
#define CHORUS_BASE (0.013f * RATE)
#define CHORUS_DEPTH (0.004f * RATE)
#define CHORUS_RATE (0.5f / RATE)
#define CHORUS_LEVEL 0.8f

/* Эхо: слева 300 мс, справа 200 мс, обратная связь 0,35 с затуханием высоких. */
#define ECHO_LENGTH 13312
#define ECHO_LEFT 13230
#define ECHO_RIGHT 8820
#define ECHO_FEEDBACK 0.35f
#define ECHO_LEVEL 0.7f

/* Эффект без посылов дольше своего хвоста (ниже -60 дБ) — не считать; его задержки
 * один раз обнуляются. Реверберация: обратная связь 0,84 на ~30 мс — 1,3 с; хорус —
 * длина задержки; эхо: обратная связь 0,35 на 300 мс — 2 с. С запасом. */
#define REVERB_IDLE (3 * RATE)
#define CHORUS_IDLE CHORUS_LENGTH
#define ECHO_IDLE (3 * RATE)

typedef struct {
  float* buffer;
  uint16_t length, index;
  float store;
} comb_t;

typedef struct {
  float* buffer;
  uint16_t length, index;
} allpass_t;

typedef struct {
  comb_t comb[2][COMBS];
  allpass_t allpass[2][ALLPASSES];
  float* chorus;               /* моно-задержка хоруса */
  uint32_t chorus_index;
  float chorus_phase;
  float* echo;                 /* моно-задержка эха */
  uint32_t echo_index;
  float echo_low;              /* затухание высоких в обратной связи */
  float* memory;               /* всё одним блоком в PSRAM */
  /* Кадров без посылов подряд: дольше *_IDLE — эффект молчит и не считается. */
  uint32_t reverb_idle, chorus_idle, echo_idle;
} effects_t;

typedef struct {
  tsf* synth;
  tml_message* song;           /* первое событие (и память всего списка) */
  tml_message* next;           /* следующее к применению */
  uint64_t frames;             /* кадров от начала песни */
  uint32_t quiet_frames;       /* песня кончилась, голосов нет: кадров тишины подряд */
  uint32_t length_ms;          /* время последнего события */
  uint16_t instruments;        /* разных программ с нотами (наборы ударных — тоже) */
  uint8_t channels;            /* каналов с нотами */
  uint8_t tracks;              /* дорожек в файле */
  int peak_voices;
  uint32_t cycles_max;         /* такты на кадр: худший кусок render */
  uint64_t cycles;             /* такты всех render */
  uint64_t cycles_effects;     /* эффекты (реверберация, хорус, эхо) — на том ядре, где шли */
  uint64_t cycles_wait;        /* ядро 0 ждало помощника */
  uint64_t cycles_core1;       /* голоса на ядре 1 */
  uint64_t voices_core1;       /* голосов помощника — сумма по кускам */
  uint64_t blocks_split;       /* кусков на двух ядрах */
  uint64_t full_frames;        /* кадров, когда заняты все MAX_VOICES голосов */
  uint32_t full_first_ms;      /* когда впервые (full_frames != 0) */
  /* Предел голосов по нагрузке: такты ядра 0 на кадр (с ожиданием помощника — значит, и
   * ядра 1) в среднем по кускам render; выше LOAD_HIGH — голосов на два меньше, ниже
   * LOAD_LOW — на один больше. Звук не тормозит, а в плотном месте гаснут самые тихие. */
  int voice_cap;               /* предел (MAX_VOICES — нет предела) */
  int voice_cap_min;           /* самый низкий за песню */
  uint32_t load_avg;           /* такты на кадр, скользящее среднее */
  uint64_t capped_frames;      /* кадров с пределом ниже MAX_VOICES */
  uint32_t trimmed;            /* голосов, погашенных пределом */
  char title[64];
  effects_t effects;
  float sends[3 * BLOCK];      /* шины посылов куска: реверберация, хорус, эхо */
  /* Работа помощника плеера на ядре 1: эффекты прошлого куска и часть голосов этого. */
  int pipelined;               /* помощник есть */
  int split_voices;            /* звучащих голосов помощнику (подстраивается) */
  int job_last;                /* голоса помощника: места [0, job_last) массива голосов */
  uint32_t job_frames;         /* кадров куска */
  uint32_t job_effects_frames; /* кадров шин прошлого куска в job_sends (0 — нет) */
  uint32_t job_cycles;         /* такты работы на ядре 1 */
  float job_sends[3 * BLOCK];  /* вход: шины прошлого куска (шаг — job_effects_frames) */
  float job_wet[2 * BLOCK];    /* выход: эффекты прошлого куска, стерео */
  float job_dry[2 * BLOCK];    /* выход: звук голосов помощника, стерео */
  float job_voice_sends[3 * BLOCK]; /* выход: их посылы (шаг — job_frames) */
  uint32_t sends_frames;       /* кадров в sends — шины прошлого куска для эффектов */
  float carry[2 * 2 * BLOCK];  /* эффекты, ещё не легшие в звук */
  uint32_t carry_frames;
  /* Режим (MODE_*, по сбросам SysEx), банк MSB, программа и ударная партия GS каналов;
   * сдвиг тональности GS, общая настройка (полутоны), общая громкость (сейчас и цель). */
  uint8_t mode;
  uint8_t msb[CHANNELS], program[CHANNELS], gs_drum[CHANNELS];
  int8_t key_shift;
  float master_coarse, master_fine;
  float master, master_target;
  uint8_t* keymap;             /* PSRAM: звучащая клавиша каждой клавиши каналов (сдвиг GS) */
  uint16_t missing[MISSING_LOGGED];
  uint8_t missing_count;
  /* Конец песни: клавиши и педали отпущены; затухание (кадров осталось) и его конец. */
  uint8_t released, faded;
  uint32_t fade_left;
  /* Выход: фильтр постоянной составляющей, огибающая ограничителя. */
  float dc_xl, dc_yl, dc_xr, dc_yr, limit_env;
} midi_t;

enum { MODE_GM, MODE_GM2, MODE_GS, MODE_XG };

static const zm_host_t* host;
static zm_source_t table;

/* Память TSF и TinyMidiLoader — у плеера (в счёт PSRAM стенда и платы), только PSRAM. В 1.7
 * блоки меньше 4 КБ просились во внутреннюю память — heap_caps без резерва, который держит
 * malloc ESP-IDF (32 КБ), — и банк TimGM6mb (394 КБ такими блоками) вычерпывал её до нуля
 * при свободных ~96 КБ: ESP сбрасывалась при загрузке банка. realloc — новым блоком: у
 * плеера его нет. Размер — в заголовке блока. */
typedef struct {
  size_t size;
  size_t pad;
} block_t;

static void* lib_malloc(size_t size) {
  block_t* b = (block_t*)host->alloc(sizeof(block_t) + size, ZM_MEM_PSRAM);
  if (b == NULL) {
    return NULL;
  }
  b->size = size;
  return b + 1;
}

static void lib_free(void* pointer) {
  if (pointer != NULL) {
    host->free((block_t*)pointer - 1);
  }
}

static void* lib_realloc(void* pointer, size_t size) {
  if (pointer == NULL) {
    return lib_malloc(size);
  }
  const size_t old = ((block_t*)pointer - 1)->size;
  if (size <= old) {
    return pointer;
  }
  void* grown = lib_malloc(size);
  if (grown != NULL) {
    memcpy(grown, pointer, old);
    lib_free(pointer);
  }
  return grown;
}

/* Уступить ядро (плеер с ABI источников 7): долгий разбор песни и банка, долгая пачка событий. */
static void rest(void) {
  if (host->size >= offsetof(zm_host_t, rest) + sizeof(host->rest) && host->rest != NULL) {
    host->rest();
  }
}

/* Такты процессора: сколько стоит синтез (в журнал при close). */
static uint32_t cycle_count(void) {
#if defined(__XTENSA__)
  uint32_t count;
  __asm__ __volatile__("rsr %0, ccount" : "=a"(count));
  return count;
#else
  return 0;
#endif
}

static uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Начало SMF: сам файл или блок "data" в RIFF RMID. NULL — не MIDI. */
static const uint8_t* smf_start(const uint8_t* file, uint32_t size, uint32_t* smf_size) {
  if (size >= 14 && memcmp(file, "MThd", 4) == 0) {
    *smf_size = size;
    return file;
  }
  if (size < 20 || memcmp(file, "RIFF", 4) != 0 || memcmp(file + 8, "RMID", 4) != 0) {
    return NULL;
  }
  for (uint32_t at = 12; at + 8 <= size;) {
    uint32_t chunk = le32(file + at + 4);
    if (chunk > size - at - 8) {
      chunk = size - at - 8;
    }
    if (memcmp(file + at, "data", 4) == 0) {
      if (chunk >= 14 && memcmp(file + at + 8, "MThd", 4) == 0) {
        *smf_size = chunk;
        return file + at + 8;
      }
      return NULL;
    }
    at += 8 + chunk + (chunk & 1);
  }
  return NULL;
}

static int probe(const uint8_t* head, size_t length) {
  if (length >= 4 && memcmp(head, "MThd", 4) == 0) {
    return 100;
  }
  if (length >= 12 && memcmp(head, "RIFF", 4) == 0 && memcmp(head + 8, "RMID", 4) == 0) {
    return 100;
  }
  return 0;
}

static uint32_t read_length(const uint8_t** p, const uint8_t* end) {
  uint32_t value = 0;
  for (int i = 0; i < 4 && *p < end; ++i) {
    const uint8_t c = *(*p)++;
    value = (value << 7) | (c & 0x7F);
    if ((c & 0x80) == 0) {
      break;
    }
  }
  return value;
}

/* Название песни: мета-событие Track Name (FF 03) первой дорожки до первой
 * ноты, байтами файла (без управляющих): кодировку — UTF-8, CP1251 или CP866 —
 * различает плеер (PLAYER.ELF 1.9). До 1.7.3 всё вне ASCII было '?', и
 * кириллица в названии выходила вопросами. */
static void song_title(const uint8_t* smf, uint32_t size, char* out, size_t out_size) {
  out[0] = 0;
  const uint32_t at = 8 + be32(smf + 4);
  if (at < 8 || at + 8 > size || memcmp(smf + at, "MTrk", 4) != 0) {
    return;
  }
  uint32_t length = be32(smf + at + 4);
  if (length > size - at - 8) {
    length = size - at - 8;
  }
  const uint8_t* p = smf + at + 8;
  const uint8_t* end = p + length;
  uint8_t status = 0;
  while (p < end) {
    read_length(&p, end);                       /* время события */
    if (p >= end) {
      return;
    }
    if (*p == 0xFF) {
      if (end - p < 2) {
        return;
      }
      const uint8_t type = p[1];
      p += 2;
      const uint32_t n = read_length(&p, end);
      if (n > (uint32_t)(end - p)) {
        return;
      }
      if (type == 0x03 && n != 0) {
        size_t used = 0;
        for (uint32_t i = 0; i < n && used + 1 < out_size; ++i) {
          const uint8_t c = p[i];
          if (c >= 0x20 && c != 0x7F) {
            out[used++] = (char)c;
          }
        }
        while (used > 0 && out[used - 1] == ' ') {
          --used;
        }
        out[used] = 0;
        if (used != 0) {
          return;
        }
      }
      p += n;
      status = 0;                               /* мета-событие отменяет running status */
      continue;
    }
    if (*p == 0xF0 || *p == 0xF7) {
      ++p;
      const uint32_t n = read_length(&p, end);
      if (n > (uint32_t)(end - p)) {
        return;
      }
      p += n;
      status = 0;
      continue;
    }
    if (*p & 0x80) {
      status = *p++;
    } else if (status == 0) {
      return;                                   /* испорчено */
    }
    const uint8_t kind = status & 0xF0;
    if (kind == 0x90) {
      return;                                   /* ноты пошли: названия уже не будет */
    }
    p += (kind == 0xC0 || kind == 0xD0) ? 1 : 2;
  }
}

/* --- эффекты ------------------------------------------------------------- */

static int effects_open(effects_t* e) {
  size_t total = CHORUS_LENGTH + ECHO_LENGTH;
  for (int i = 0; i < COMBS; ++i) {
    total += 2u * comb_lengths[i] + SPREAD;
  }
  for (int i = 0; i < ALLPASSES; ++i) {
    total += 2u * allpass_lengths[i] + SPREAD;
  }
  e->memory = (float*)host->alloc(total * sizeof(float), ZM_MEM_PSRAM);
  if (e->memory == NULL) {
    return 0;
  }
  memset(e->memory, 0, total * sizeof(float));
  e->reverb_idle = REVERB_IDLE;               /* задержки пусты: молчат, пока не пошлют */
  e->chorus_idle = CHORUS_IDLE;
  e->echo_idle = ECHO_IDLE;
  float* next = e->memory;
  for (int side = 0; side < 2; ++side) {
    for (int i = 0; i < COMBS; ++i) {
      e->comb[side][i].buffer = next;
      e->comb[side][i].length = (uint16_t)(comb_lengths[i] + side * SPREAD);
      next += e->comb[side][i].length;
    }
    for (int i = 0; i < ALLPASSES; ++i) {
      e->allpass[side][i].buffer = next;
      e->allpass[side][i].length = (uint16_t)(allpass_lengths[i] + side * SPREAD);
      next += e->allpass[side][i].length;
    }
  }
  e->chorus = next;
  next += CHORUS_LENGTH;
  e->echo = next;
  return 1;
}

static float comb_process(comb_t* c, float input) {
  const float output = c->buffer[c->index];
  c->store = output * (1.0f - REVERB_DAMP) + c->store * REVERB_DAMP;
  c->buffer[c->index] = input + c->store * REVERB_FEEDBACK;
  if (++c->index >= c->length) {
    c->index = 0;
  }
  return output;
}

static float allpass_process(allpass_t* a, float input) {
  const float delayed = a->buffer[a->index];
  a->buffer[a->index] = input + delayed * 0.5f;
  if (++a->index >= a->length) {
    a->index = 0;
  }
  return delayed - input;
}

/* Отсчёт из задержки с дробным отставанием delay (линейно между соседями). */
static float delay_tap(const float* buffer, uint32_t length, uint32_t index, float delay) {
  float position = (float)index - delay;
  if (position < 0.0f) {
    position += (float)length;
  }
  const uint32_t a = (uint32_t)position;
  const float t = position - (float)a;
  const uint32_t b = (a + 1 >= length) ? 0 : a + 1;
  return buffer[a] + (buffer[b] - buffer[a]) * t;
}

/* Шина молчит весь кусок. */
static int bus_silent(const float* bus, uint32_t frames) {
  for (uint32_t i = 0; i < frames; ++i) {
    if (bus[i] != 0.0f) {
      return 0;
    }
  }
  return 1;
}

/* Считать ли эффект в этом куске: посылы есть — да; молчит дольше tail — нет, а на
 * переходе его задержки line (length отсчётов) обнуляются (хвост уже тише -60 дБ),
 * и он вернётся с чистого. */
static int effect_needed(uint32_t* idle, const float* bus, uint32_t frames, uint32_t tail,
                         float* line, size_t length) {
  if (!bus_silent(bus, frames)) {
    *idle = 0;
    return 1;
  }
  if (*idle >= tail) {
    return 0;
  }
  *idle += frames;
  if (*idle >= tail) {
    memset(line, 0, length * sizeof(float));
    return 0;
  }
  return 1;
}

/* Шины посылов куска — через эффекты, прибавить к out (стерео). Шины лежат
 * подряд с шагом frames: так их кладёт tsf_render_float_sends. */
static void effects_process(effects_t* e, const float* sends, float* out, uint32_t frames) {
  const float* reverb = sends;
  const float* chorus = sends + frames;
  const float* echo = sends + 2 * frames;
  if (effect_needed(&e->reverb_idle, reverb, frames, REVERB_IDLE, e->memory,
                    (size_t)(e->chorus - e->memory))) {
    for (uint32_t i = 0; i < frames; ++i) {
      const float input = reverb[i] * REVERB_GAIN;
      float l = 0.0f, rr = 0.0f;
      for (int k = 0; k < COMBS; ++k) {
        l += comb_process(&e->comb[0][k], input);
        rr += comb_process(&e->comb[1][k], input);
      }
      for (int k = 0; k < ALLPASSES; ++k) {
        l = allpass_process(&e->allpass[0][k], l);
        rr = allpass_process(&e->allpass[1][k], rr);
      }
      out[2 * i] += l * REVERB_WET;
      out[2 * i + 1] += rr * REVERB_WET;
    }
  } else {
    for (int k = 0; k < COMBS; ++k) {
      e->comb[0][k].store = e->comb[1][k].store = 0.0f;
    }
  }
  if (effect_needed(&e->chorus_idle, chorus, frames, CHORUS_IDLE, e->chorus, CHORUS_LENGTH)) {
    for (uint32_t i = 0; i < frames; ++i) {
      /* Хорус: треугольное качание (дёшево), правая отводка — на четверть периода позже. */
      e->chorus[e->chorus_index] = chorus[i];
      const float p = e->chorus_phase;
      const float lfo_left = 4.0f * (p < 0.5f ? p : 1.0f - p) - 1.0f;
      const float q = p + 0.25f < 1.0f ? p + 0.25f : p - 0.75f;
      const float lfo_right = 4.0f * (q < 0.5f ? q : 1.0f - q) - 1.0f;
      out[2 * i] += delay_tap(e->chorus, CHORUS_LENGTH, e->chorus_index,
                              CHORUS_BASE + CHORUS_DEPTH * lfo_left) * CHORUS_LEVEL;
      out[2 * i + 1] += delay_tap(e->chorus, CHORUS_LENGTH, e->chorus_index,
                                  CHORUS_BASE + CHORUS_DEPTH * lfo_right) * CHORUS_LEVEL;
      e->chorus_phase = p + CHORUS_RATE < 1.0f ? p + CHORUS_RATE : p + CHORUS_RATE - 1.0f;
      if (++e->chorus_index >= CHORUS_LENGTH) {
        e->chorus_index = 0;
      }
    }
  }
  if (effect_needed(&e->echo_idle, echo, frames, ECHO_IDLE, e->echo, ECHO_LENGTH)) {
    for (uint32_t i = 0; i < frames; ++i) {
      const uint32_t at_left = e->echo_index >= ECHO_LEFT ? e->echo_index - ECHO_LEFT : e->echo_index + ECHO_LENGTH - ECHO_LEFT;
      const uint32_t at_right = e->echo_index >= ECHO_RIGHT ? e->echo_index - ECHO_RIGHT : e->echo_index + ECHO_LENGTH - ECHO_RIGHT;
      const float tap_left = e->echo[at_left];
      const float tap_right = e->echo[at_right];
      e->echo_low += 0.4f * (tap_left - e->echo_low);
      e->echo[e->echo_index] = echo[i] + e->echo_low * ECHO_FEEDBACK;
      if (++e->echo_index >= ECHO_LENGTH) {
        e->echo_index = 0;
      }
      out[2 * i] += tap_left * ECHO_LEVEL;
      out[2 * i + 1] += tap_right * ECHO_LEVEL;
    }
  } else {
    e->echo_low = 0.0f;
  }
}

/* Работа помощника (ядро 1): эффекты прошлого куска (job_sends → job_wet) и голоса
 * [0, job_last) этого (→ job_dry и job_voice_sends). */
static void voices_job(void* arg) {
  midi_t* m = (midi_t*)arg;
  const uint32_t start = cycle_count();
  if (m->job_effects_frames != 0) {
    memset(m->job_wet, 0, 2 * m->job_effects_frames * sizeof(float));
    effects_process(&m->effects, m->job_sends, m->job_wet, m->job_effects_frames);
  }
  const uint32_t middle = cycle_count();
  memset(m->job_voice_sends, 0, 3 * m->job_frames * sizeof(float));
  tsf_render_float_sends_range(m->synth, m->job_dry, m->job_voice_sends, (int)m->job_frames, 0,
                               m->job_last);
  const uint32_t end = cycle_count();
  m->cycles_effects += middle - start;
  m->cycles_core1 += end - middle;
  m->job_cycles = end - start;
}

/* Кусок out (frames кадров) на двух ядрах: помощнику — эффекты прошлого куска (его
 * шины в sends) и первые split_voices звучащих голосов, ядру 0 — остальные голоса.
 * События куска уже применены, и пока работают оба ядра, голоса никто не трогает.
 * Эффекты прошлого куска ложатся в этот. */
static void split_block(midi_t* m, float* out, uint32_t frames) {
  const int active = tsf_active_voice_count(m->synth);
  if (m->split_voices > active) {
    m->split_voices = active;
  }
  m->job_last = tsf_voice_split(m->synth, m->split_voices);
  m->job_frames = frames;
  m->job_effects_frames = m->sends_frames;
  memcpy(m->job_sends, m->sends, 3 * m->sends_frames * sizeof(float));
  const int started = host->worker_start(voices_job, m) == ZM_OK;
  const uint32_t start = cycle_count();
  memset(m->sends, 0, 3 * frames * sizeof(float));
  tsf_render_float_sends_range(m->synth, out, m->sends, (int)frames, m->job_last,
                               tsf_voice_slots(m->synth));
  const uint32_t own = cycle_count() - start;
  if (started) {
    const uint32_t wait = cycle_count();
    host->worker_wait();
    m->cycles_wait += cycle_count() - wait;
  } else {
    voices_job(m);                             /* помощник занят (не должен): всё здесь */
  }
  for (uint32_t i = 0; i < 2 * frames; ++i) {
    out[i] += m->job_dry[i];
  }
  for (uint32_t i = 0; i < 3 * frames; ++i) {
    m->sends[i] += m->job_voice_sends[i];
  }
  m->sends_frames = frames;
  memcpy(m->carry + 2 * m->carry_frames, m->job_wet, 2 * m->job_effects_frames * sizeof(float));
  m->carry_frames += m->job_effects_frames;
  const uint32_t take = m->carry_frames < frames ? m->carry_frames : frames;
  for (uint32_t i = 0; i < 2 * take; ++i) {
    out[i] += m->carry[i];
  }
  m->carry_frames -= take;
  memmove(m->carry, m->carry + 2 * take, 2 * m->carry_frames * sizeof(float));
  m->voices_core1 += (uint32_t)m->split_voices;
  m->blocks_split++;
  /* Граница на следующий кусок: у ядра 0 тактов больше — помощнику голосом больше,
   * меньше — голосом меньше. Без счётчика тактов (стенд на ПК) — пополам. */
  if (own == 0 && m->job_cycles == 0) {
    m->split_voices = active / 2;
  } else if (own > m->job_cycles) {
    if (m->split_voices < active) {
      m->split_voices++;
    }
  } else if (m->split_voices > 0) {
    m->split_voices--;
  }
}

/* Хвосты эффектов — в тишину (перемотка): иначе прежнее место звучит дальше. */
static void effects_clear(effects_t* e) {
  size_t total = CHORUS_LENGTH + ECHO_LENGTH;
  for (int i = 0; i < COMBS; ++i) {
    total += 2u * comb_lengths[i] + SPREAD;
    e->comb[0][i].store = e->comb[1][i].store = 0.0f;
  }
  for (int i = 0; i < ALLPASSES; ++i) {
    total += 2u * allpass_lengths[i] + SPREAD;
  }
  memset(e->memory, 0, total * sizeof(float));
  e->echo_low = 0.0f;
  e->reverb_idle = REVERB_IDLE;
  e->chorus_idle = CHORUS_IDLE;
  e->echo_idle = ECHO_IDLE;
}

/* Ударный ли канал: GS — назначение партии (SysEx Use for Rhythm Part, по умолчанию канал
 * 9); XG — банк 127 или 126 (наборы ударных и эффектов), канал 9 без банка; GM2 — банк 120
 * (121 — мелодический), иначе канал 9; GM — канал 9. */
static int is_drum(const midi_t* m, int channel) {
  const uint8_t msb = m->msb[channel];
  switch (m->mode) {
    case MODE_GS:
      return m->gs_drum[channel] != 0;
    case MODE_XG:
      return msb == 127 || msb == 126 || (channel == DRUMS && msb == 0);
    case MODE_GM2:
      return msb == 120 || (channel == DRUMS && msb != 121);
    default:
      return channel == DRUMS;
  }
}

/* Общая настройка — мелодическим партиям (ударные не настраиваются). */
static void apply_tuning(midi_t* m, int channel) {
  tsf_channel_set_master_tuning(m->synth, channel,
                                is_drum(m, channel) ? 0.0f : m->master_coarse + m->master_fine);
}

/* Пресет канала по программе и банку; нет такого — канал молчит (TSF), в журнал — один раз
 * на банк и программу. */
static void set_program(midi_t* m, int channel) {
  const int drum = is_drum(m, channel);
  if (!tsf_channel_set_presetnumber(m->synth, channel, m->program[channel], drum)) {
    const unsigned bank = (unsigned)tsf_channel_get_preset_bank(m->synth, channel);
    const uint16_t key = (uint16_t)((drum ? 0x8000u : 0u) | ((bank & 0xFFu) << 7) | m->program[channel]);
    int known = 0;
    for (int i = 0; i < m->missing_count; ++i) {
      known |= m->missing[i] == key;
    }
    if (!known && m->missing_count < MISSING_LOGGED) {
      m->missing[m->missing_count++] = key;
      host->log("midi: no %s preset bank %u program %u in " BANK ": channel %d is silent",
                drum ? "drum" : "melodic", bank, (unsigned)m->program[channel], channel + 1);
    }
  }
  apply_tuning(m, channel);
}

/* Каналы — как после сброса (начало песни, SysEx GM/GM2/GS/XG): программа 0, контроллеры
 * по умолчанию, ударный — канал 9; общие громкость и настройка — исходные. Голоса гаснут
 * быстро. */
static void reset_channels(midi_t* m, int mode) {
  tsf_reset(m->synth);
  m->mode = (uint8_t)mode;
  m->key_shift = 0;
  m->master_coarse = m->master_fine = 0.0f;
  m->master_target = 1.0f;
  for (int channel = 0; channel < CHANNELS; ++channel) {
    m->msb[channel] = 0;
    m->program[channel] = 0;
    m->gs_drum[channel] = (uint8_t)(channel == DRUMS);
    set_program(m, channel);
  }
}

/* Нажатие: у мелодической партии — со сдвигом тональности GS; какая клавиша звучит —
 * в keymap (отпускание ищет её, даже если сдвиг сменился). */
static void note_on(midi_t* m, int channel, int key, int velocity) {
  int played = key;
  if (velocity == 0) {
    const int sounding = m->keymap != NULL ? m->keymap[channel * 128 + key] : key;
    tsf_channel_note_off(m->synth, channel, sounding);
    return;
  }
  if (m->key_shift != 0 && !is_drum(m, channel)) {
    played = key + m->key_shift;
    if (played < 0 || played > 127) {
      return;
    }
  }
  if (m->keymap != NULL) {
    m->keymap[channel * 128 + key] = (uint8_t)played;
  }
  tsf_channel_note_on(m->synth, channel, played, velocity / 127.0f);
}

static void note_off(midi_t* m, int channel, int key) {
  tsf_channel_note_off(m->synth, channel, m->keymap != NULL ? m->keymap[channel * 128 + key] : key);
}

static void sysex(midi_t* m, const tml_message* message, int notes) {
  switch (message->channel) {
    case TML_SYSEX_GM_ON:
      reset_channels(m, MODE_GM);
      break;
    case TML_SYSEX_GM2_ON:
      reset_channels(m, MODE_GM2);
      break;
    case TML_SYSEX_GS_RESET:
      reset_channels(m, MODE_GS);
      break;
    case TML_SYSEX_XG_ON:
      reset_channels(m, MODE_XG);
      break;
    case TML_SYSEX_MASTER_VOLUME:
      m->master_target = message->pitch_bend / 16383.0f;
      if (!notes) {
        m->master = m->master_target;          /* перемотка: сразу, без перехода */
      }
      break;
    case TML_SYSEX_MASTER_FINE:
    case TML_SYSEX_MASTER_COARSE:
      if (message->channel == TML_SYSEX_MASTER_FINE) {
        m->master_fine = (message->pitch_bend - 8192) / 8192.0f;
      } else {
        m->master_coarse = (float)((message->key & 0x7F) - 64);
      }
      for (int channel = 0; channel < CHANNELS; ++channel) {
        apply_tuning(m, channel);
      }
      break;
    case TML_SYSEX_KEY_SHIFT: {
      const int shift = (message->key & 0x7F) - 64;
      m->key_shift = (int8_t)(shift < -24 ? -24 : (shift > 24 ? 24 : shift));
      break;
    }
    case TML_SYSEX_GS_RHYTHM: {
      const int channel = message->key & 15;
      m->mode = MODE_GS;                          /* назначение партии — признак GS */
      m->gs_drum[channel] = (uint8_t)(message->velocity != 0);
      set_program(m, channel);
      break;
    }
    default:
      break;
  }
}

static void apply(midi_t* m, const tml_message* message, int notes) {
  const int channel = message->channel & 15;
  switch (message->type) {
    case TML_PROGRAM_CHANGE:
      m->program[channel] = (uint8_t)(message->program & 0x7F);
      set_program(m, channel);
      break;
    case TML_NOTE_ON:
      if (notes) {
        note_on(m, channel, message->key & 0x7F, message->velocity & 0x7F);
      }
      break;
    case TML_NOTE_OFF:
      if (notes) {
        note_off(m, channel, message->key & 0x7F);
      }
      break;
    case TML_PITCH_BEND:
      tsf_channel_set_pitchwheel(m->synth, channel, message->pitch_bend);
      break;
    case TML_CONTROL_CHANGE:
      if (message->control == 0) {
        m->msb[channel] = (uint8_t)(message->control_value & 0x7F);
      }
      tsf_channel_midi_control(m->synth, channel, message->control, message->control_value);
      break;
    case TML_CHANNEL_PRESSURE:
      tsf_channel_set_pressure(m->synth, channel, message->channel_pressure & 0x7F);
      break;
    case TML_KEY_PRESSURE:
      if (notes) {
        const int key = message->key & 0x7F;
        tsf_channel_set_key_pressure(m->synth, channel, m->keymap != NULL ? m->keymap[channel * 128 + key] : key,
                                     message->key_pressure & 0x7F);
      }
      break;
    case TML_SYSTEM_EXCLUSIVE:
      sysex(m, message, notes);
      break;
    default:
      break;
  }
}

/* Клавиши без сдвига тональности. */
static void keymap_reset(midi_t* m) {
  if (m->keymap != NULL) {
    for (int i = 0; i < CHANNELS * 128; ++i) {
      m->keymap[i] = (uint8_t)(i & 127);
    }
  }
}

/* Для окна плеера: каналы с нотами и разные инструменты, которые ими звучат. */
static void count_contents(midi_t* m) {
  uint8_t program[CHANNELS] = {0};
  uint8_t melodic[16] = {0};
  uint8_t kits[16] = {0};
  uint16_t channels = 0;
  for (const tml_message* message = m->song; message != NULL; message = message->next) {
    if (message->type == TML_PROGRAM_CHANGE) {
      program[message->channel & 15] = (uint8_t)(message->program & 0x7F);
    } else if (message->type == TML_NOTE_ON && message->velocity != 0) {
      const uint8_t p = program[message->channel & 15];
      uint8_t* set = (message->channel == DRUMS) ? kits : melodic;
      set[p >> 3] |= (uint8_t)(1u << (p & 7));
      channels |= (uint16_t)(1u << (message->channel & 15));
    }
  }
  uint16_t instruments = 0;
  for (int i = 0; i < 128; ++i) {
    instruments += (uint16_t)(((melodic[i >> 3] >> (i & 7)) & 1u) + ((kits[i >> 3] >> (i & 7)) & 1u));
  }
  uint8_t used = 0;
  for (int c = 0; c < CHANNELS; ++c) {
    used += (uint8_t)((channels >> c) & 1u);
  }
  m->instruments = instruments;
  m->channels = used;
}

static void close_source(void* instance);

static void* open_source(uint32_t file_size, const uint8_t* file) {
  if (file == NULL || file_size == 0) {
    return NULL;
  }
  uint32_t smf_size = 0;
  const uint8_t* smf = smf_start(file, file_size, &smf_size);
  if (smf == NULL) {
    host->log("midi: %u bytes are not a MIDI file", (unsigned)file_size);
    return NULL;
  }
  const uint8_t* bank = NULL;
  size_t bank_size = 0;
  if (host->resource == NULL || host->resource(BANK, &bank, &bank_size) != ZM_OK) {
    host->log("midi: no bank " BANK " (the plugin sends it from /WC/ESP/)");
    return NULL;
  }
  midi_t* m = (midi_t*)host->alloc(sizeof(midi_t), ZM_MEM_INTERNAL);
  if (m == NULL) {
    return NULL;
  }
  memset(m, 0, sizeof(*m));
  tsf_vdac4_malloc = lib_malloc;                /* память — у плеера, долгая работа уступает ядро */
  tsf_vdac4_realloc = lib_realloc;
  tsf_vdac4_free = lib_free;
  tsf_vdac4_rest = rest;
  m->keymap = (uint8_t*)host->alloc(CHANNELS * 128, ZM_MEM_PSRAM);
  keymap_reset(m);
  m->master = m->master_target = 1.0f;
  m->synth = tsf_load_memory(bank, (int)bank_size);
  if (m->synth == NULL) {
    host->log("midi: bank " BANK " of %u bytes not loaded", (unsigned)bank_size);
    close_source(m);
    return NULL;
  }
  m->song = tml_load_memory(smf, (int)smf_size);
  if (m->song == NULL) {
    host->log("midi: song of %u bytes not loaded", (unsigned)smf_size);
    close_source(m);
    return NULL;
  }
  tsf_set_output(m->synth, TSF_STEREO_INTERLEAVED, RATE, GAIN_DB);
  if (!tsf_set_max_voices(m->synth, MAX_VOICES + RESERVE_VOICES) || !effects_open(&m->effects)) {
    close_source(m);
    return NULL;
  }
  m->pipelined = host->size >= offsetof(zm_host_t, worker_wait) + sizeof(host->worker_wait) &&
                 host->worker_start != NULL && host->worker_wait != NULL;
  m->voice_cap = m->voice_cap_min = MAX_VOICES;
  tsf_set_voice_cap(m->synth, MAX_VOICES);      /* сверх — замена с гашением в запасных местах */
  reset_channels(m, MODE_GM);
  unsigned int length = 0;
  tml_get_info(m->song, NULL, NULL, NULL, NULL, &length);
  m->length_ms = length;
  const uint32_t tracks = ((uint32_t)smf[10] << 8) | smf[11];
  m->tracks = (uint8_t)(tracks > 255 ? 255 : tracks);
  count_contents(m);
  song_title(smf, smf_size, m->title, sizeof(m->title));
  m->next = m->song;
  host->log("midi: %u ms, %u tracks, %u channels, %u instruments, bank %u presets",
            (unsigned)m->length_ms, (unsigned)tracks, (unsigned)m->channels,
            (unsigned)m->instruments, (unsigned)tsf_get_presetcount(m->synth));
  return m;
}

static uint32_t position_ms(const midi_t* m) { return (uint32_t)(m->frames * 1000 / RATE); }

/* Кадр события: время (мс) в кадрах, вниз до кратного EVENT_QUANTUM. */
static uint64_t event_frame(const tml_message* message) {
  return ((uint64_t)message->time * RATE / 1000) & ~(uint64_t)(EVENT_QUANTUM - 1);
}

/* Выход: постоянная составляющая прочь, общая громкость (к цели линейно за кусок),
 * затухание в конце песни, связанный стереоограничитель (огибающая — пик большего канала,
 * отпускание LIMIT_RELEASE; |отсчёт| не выше LIMIT_LEVEL). */
static void output_stage(midi_t* m, float* out, uint32_t frames) {
  float master = m->master;
  const float step = (m->master_target - master) / (float)frames;
  for (uint32_t i = 0; i < frames; ++i) {
    const float l = out[2 * i], r = out[2 * i + 1];
    float yl = l - m->dc_xl + DC_POLE * m->dc_yl;
    float yr = r - m->dc_xr + DC_POLE * m->dc_yr;
    m->dc_xl = l, m->dc_yl = yl, m->dc_xr = r, m->dc_yr = yr;
    master += step;
    yl *= master, yr *= master;
    if (m->fade_left != 0) {
      const float fade = (float)m->fade_left / (float)FADE_FRAMES;
      yl *= fade, yr *= fade;
      if (--m->fade_left == 0) {
        m->faded = 1;
      }
    } else if (m->faded) {
      yl = yr = 0.0f;
    }
    const float al = yl < 0.0f ? -yl : yl, ar = yr < 0.0f ? -yr : yr;
    const float peak = al > ar ? al : ar;
    const float decayed = m->limit_env * LIMIT_RELEASE;
    m->limit_env = peak > decayed ? peak : decayed;
    if (m->limit_env > LIMIT_LEVEL) {
      const float gain = LIMIT_LEVEL / m->limit_env;
      yl *= gain, yr *= gain;
    }
    out[2 * i] = yl, out[2 * i + 1] = yr;
  }
  m->master = m->master_target;
}

/* Конец событий: клавиши и педали всех каналов отпущены — хвосты отзвучат по отпусканию,
 * а не держатся до предела. */
static void release_all(midi_t* m) {
  for (int channel = 0; channel < CHANNELS; ++channel) {
    tsf_channel_midi_control(m->synth, channel, 64, 0);
    tsf_channel_midi_control(m->synth, channel, 66, 0);
    tsf_channel_note_off_all(m->synth, channel);
  }
  m->released = 1;
}

static int render(void* instance, float* out, uint32_t max_frames, uint32_t* frames) {
  midi_t* m = (midi_t*)instance;
  *frames = 0;
  /* Конец: события кончились, голоса и хвосты эффектов отзвучали — или хвост дольше TAIL_MS
   * отзвучал затуханием (с 1.7; прежде обрывался). */
  if (m->next == NULL && (m->quiet_frames >= QUIET_FRAMES || m->faded)) {
    return ZM_RENDER_END;
  }
  if (m->next == NULL && !m->released) {
    release_all(m);
  }
  if (m->next == NULL && m->fade_left == 0 && !m->faded && position_ms(m) >= m->length_ms + TAIL_MS) {
    m->fade_left = FADE_FRAMES;
  }
  const uint32_t start = cycle_count();
  uint32_t done = 0, events = 0;
  while (done < max_frames) {
    uint32_t block = max_frames - done;
    if (block > BLOCK) {
      block = BLOCK;
    }
    /* События, наступившие к этому кадру; контроллеры, программы и колесо внутри куска —
     * тоже, с его начала. Нота внутри куска его укорачивает: она — начало следующего. */
    const uint64_t now = m->frames + done;
    while (m->next != NULL) {
      const uint64_t at = event_frame(m->next);
      if (at > now) {
        if (at >= now + block) {
          break;
        }
        if (m->next->type == TML_NOTE_ON || m->next->type == TML_NOTE_OFF) {
          block = (uint32_t)(at - now);
          break;
        }
      }
      apply(m, m->next, 1);
      m->next = m->next->next;
      if (++events % REST_EVENTS == 0) {
        rest();                                 /* пачка событий — не держать ядро (с 1.7) */
      }
    }
    if (m->pipelined) {
      split_block(m, out + done * 2, block);
    } else {
      memset(m->sends, 0, sizeof(m->sends));
      tsf_render_float_sends(m->synth, out + done * 2, m->sends, (int)block);
      const uint32_t effects_start = cycle_count();
      effects_process(&m->effects, m->sends, out + done * 2, block);
      m->cycles_effects += cycle_count() - effects_start;
    }
    done += block;
  }
  output_stage(m, out, done);
  const uint32_t spent = cycle_count() - start;
  m->cycles += spent;
  if (spent / done > m->cycles_max) {
    m->cycles_max = spent / done;
  }
  /* Предел голосов по нагрузке (без счётчика тактов — стенд на ПК — не ставится). */
  m->load_avg = (m->load_avg * 3u + spent / done) / 4u;
  const int active = tsf_voices_sounding(m->synth);
  if (m->load_avg > LOAD_HIGH && m->voice_cap > MIN_VOICES) {
    int cap = (active < m->voice_cap ? active : m->voice_cap) - 2;
    m->voice_cap = cap < MIN_VOICES ? MIN_VOICES : cap;
    tsf_set_voice_cap(m->synth, m->voice_cap);
    m->trimmed += (uint32_t)tsf_voices_trim(m->synth);
    if (m->voice_cap < m->voice_cap_min) {
      m->voice_cap_min = m->voice_cap;
    }
  } else if (m->load_avg < LOAD_LOW && m->voice_cap < MAX_VOICES) {
    m->voice_cap++;
    tsf_set_voice_cap(m->synth, m->voice_cap);
  }
  if (m->voice_cap < MAX_VOICES) {
    m->capped_frames += done;
  }
  const int voices = tsf_voices_sounding(m->synth);
  if (voices > m->peak_voices) {
    m->peak_voices = voices;
  }
  if (voices >= MAX_VOICES) {
    if (m->full_frames == 0) {
      m->full_first_ms = position_ms(m);
    }
    m->full_frames += done;
  }
  if (m->next == NULL && tsf_active_voice_count(m->synth) == 0) {
    float peak = 0.0f;
    for (uint32_t i = 0; i < done * 2; ++i) {
      const float magnitude = out[i] < 0.0f ? -out[i] : out[i];
      peak = magnitude > peak ? magnitude : peak;
    }
    m->quiet_frames = peak < QUIET_LEVEL ? m->quiet_frames + done : 0;
  }
  m->frames += done;
  *frames = done;
  return ZM_RENDER_OK;
}

/* Перемотка на delta мс. */
static int seek(void* instance, int32_t delta, uint32_t* file_offset) {
  midi_t* m = (midi_t*)instance;
  *file_offset = UINT32_MAX;
  int64_t target = (int64_t)position_ms(m) + delta;
  if (target < 0) {
    target = 0;
  }
  if (target > m->length_ms) {
    target = m->length_ms;
  }
  tsf_voices_kill(m->synth);                   /* голоса — сразу в тишину (с 1.6) */
  m->carry_frames = 0;                         /* помощник уже свободен: кусок ждёт его */
  m->sends_frames = 0;
  effects_clear(&m->effects);
  m->master = m->master_target = 1.0f;
  reset_channels(m, MODE_GM);                  /* каналы — как в начале песни */
  keymap_reset(m);
  m->released = m->faded = 0;
  m->fade_left = 0;
  /* Какие клавиши к новому месту нажаты (скорость) и какие держит педаль (с 1.7): они
   * берутся заново. Нет памяти — без них, как до 1.7. */
  uint8_t* held = (uint8_t*)host->alloc(CHANNELS * 128 * 2, ZM_MEM_PSRAM);
  uint8_t* pedal = held != NULL ? held + CHANNELS * 128 : NULL;
  uint16_t sustain = 0;
  int last_key[CHANNELS];
  for (int channel = 0; channel < CHANNELS; ++channel) {
    last_key[channel] = -1;
  }
  if (held != NULL) {
    memset(held, 0, CHANNELS * 128 * 2);
  }
  m->next = m->song;
  uint32_t events = 0;
  while (m->next != NULL && m->next->time < (uint32_t)target) {
    const tml_message* e = m->next;
    const int channel = e->channel & 15;
    if (held != NULL && (e->type == TML_NOTE_ON || e->type == TML_NOTE_OFF)) {
      const int at = channel * 128 + (e->key & 0x7F);
      if (e->type == TML_NOTE_ON && e->velocity != 0) {
        held[at] = (uint8_t)(e->velocity & 0x7F);
        pedal[at] = 0;
        last_key[channel] = e->key & 0x7F;
      } else if (held[at] != 0) {
        pedal[at] = (uint8_t)((sustain >> channel) & 1u ? held[at] : 0);
        held[at] = 0;
      }
    } else if (held != NULL && e->type == TML_CONTROL_CHANGE) {
      const int control = e->control, value = e->control_value & 0x7F;
      if (control == 64 && value >= 64) {
        sustain |= (uint16_t)(1u << channel);
      } else if ((control == 64 && value < 64) || control == 121) {
        sustain &= (uint16_t)~(1u << channel);
        memset(pedal + channel * 128, 0, 128);
      } else if (control == 120 || control >= 123) {
        memset(held + channel * 128, 0, 128);
        memset(pedal + channel * 128, 0, 128);
      }
    } else if (held != NULL && e->type == TML_SYSTEM_EXCLUSIVE && e->channel >= TML_SYSEX_GM_ON &&
               e->channel <= TML_SYSEX_XG_ON && e->channel != TML_SYSEX_GM_OFF) {
      memset(held, 0, CHANNELS * 128 * 2);     /* сброс — всё замолкает */
      sustain = 0;
    }
    apply(m, e, 0);
    m->next = e->next;
    if (++events % 4096 == 0) {
      rest();
    }
  }
  if (held != NULL) {
    for (int channel = 0; channel < CHANNELS; ++channel) {
      if (is_drum(m, channel)) {
        continue;                              /* удары — мгновенные: заново не берутся */
      }
      for (int key = 0; key < 128; ++key) {
        const int at = channel * 128 + key;
        if (held[at] != 0) {
          note_on(m, channel, key, held[at]);
        } else if (pedal[at] != 0) {
          note_on(m, channel, key, pedal[at]);  /* педаль нажата — отпущенная нота держится */
          note_off(m, channel, key);
        }
      }
      if (last_key[channel] >= 0) {
        tsf_channel_midi_control(m->synth, channel, 84, last_key[channel]);   /* портаменто — от неё */
      }
    }
    host->free(held);
  }
  m->frames = (uint64_t)target * RATE / 1000;
  m->quiet_frames = 0;
  return ZM_OK;
}

static void info(const void* instance, zm_info_t* out) {
  const midi_t* m = (const midi_t*)instance;
  memset(out, 0, sizeof(*out));
  strcpy(out->format, "MIDI");
  out->rate = RATE;
  out->channels = 2;
  out->duration_ms = m->length_ms;
  out->position_ms = position_ms(m);
  memcpy(out->title, m->title, sizeof(out->title));      /* того же размера, с нулём */
  out->title[sizeof(out->title) - 1] = 0;
  out->voices = m->channels;
  out->samples = m->instruments;
  out->effects = m->tracks;
}

static void close_source(void* instance) {
  midi_t* m = (midi_t*)instance;
  if (m->frames != 0) {
    /* Сколько стоит синтез: в реальном времени кадр — 5442 такта при 240 МГц. С
     * помощником в тактах ядра 0 — его голоса и ожидание помощника, а эффекты и
     * часть голосов идут на ядре 1. */
    if (m->pipelined) {
      host->log("midi: peak %d voices, core 0: cycles per frame %u average (wait %u), %u worst",
                m->peak_voices, (unsigned)(m->cycles / m->frames),
                (unsigned)(m->cycles_wait / m->frames), (unsigned)m->cycles_max);
      const unsigned tenths = m->blocks_split != 0
                                  ? (unsigned)(m->voices_core1 * 10 / m->blocks_split) : 0;
      host->log("midi: core 1: effects %u, voices %u cycles per frame (%u.%u voices on average)",
                (unsigned)(m->cycles_effects / m->frames), (unsigned)(m->cycles_core1 / m->frames),
                tenths / 10, tenths % 10);
    } else {
      host->log("midi: peak %d voices, cycles per frame %u average (effects %u), %u worst",
                m->peak_voices, (unsigned)(m->cycles / m->frames),
                (unsigned)(m->cycles_effects / m->frames), (unsigned)m->cycles_max);
    }
    if (m->capped_frames != 0) {
      host->log("midi: voices limited by load for %u ms (down to %d, %u voices ended)",
                (unsigned)(m->capped_frames * 1000 / RATE), m->voice_cap_min, (unsigned)m->trimmed);
    }
    const unsigned refused = m->synth != NULL ? tsf_voices_refused(m->synth) : 0;
    if (m->full_frames != 0 || refused != 0) {
      host->log("midi: all %d voices busy %u ms (first at %u:%02u), %u notes without a voice",
                MAX_VOICES, (unsigned)(m->full_frames * 1000 / RATE),
                (unsigned)(m->full_first_ms / 60000), (unsigned)(m->full_first_ms / 1000 % 60),
                refused);
    }
  }
  if (m->synth != NULL) {
    tsf_close(m->synth);
  }
  if (m->song != NULL) {
    tml_free(m->song);
  }
  host->free(m->effects.memory);
  if (m->keymap != NULL) {
    host->free(m->keymap);
  }
  host->free(m);
}

int main(int argc, char* argv[]) {
  if (argc < 3) {
    return 0;
  }
  host = (const zm_host_t*)argv[1];
  table.size = sizeof(table);
  table.abi_major = ZM_SOURCE_ABI_MAJOR;
  table.abi_minor = ZM_SOURCE_ABI_MINOR;
  table.name = "midi";
  table.version = "1.7.3";
  table.extensions = "MID RMI";
  table.input = ZM_INPUT_WHOLE;
  table.keeps_file = 0;
  table.seek_unit = ZM_SEEK_MS;
  table.resources = BANK;
  table.probe = probe;
  table.open = open_source;
  table.render = render;
  table.seek = seek;
  table.info = info;
  table.levels = NULL;
  table.close = close_source;
  *(const zm_source_t**)argv[2] = &table;
  return ZM_SOURCE_ABI_MAJOR;
}
