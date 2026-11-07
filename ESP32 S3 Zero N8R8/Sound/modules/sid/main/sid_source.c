/* SID.ELF: музыка Commodore 64 — файлы PSID и RSID (HVSC). Эмуляция C64 (6510, CIA, VIC) и SID
 * 6581/8580, до четырёх микросхем, — cRSID 1.58 (Hermit, Mihaly Horvath; лицензия WTF: код можно
 * брать как угодно, автора — упомянуть), Sound/components/crsid.
 *
 * Два ядра (1.1, 04.11.2026: на одном ядре точная эмуляция cRSID брала 71–127 % ядра 0, RSID не
 * успевали, и сторож задач перезапускал ESP). Звук cRSID считает раз на отсчёт в два шага: такты C64
 * (процессор, CIA, VIC, огибающие ADSR), затем звуковая часть — волны с передискретизацией ×5,
 * фильтр Найквиста, фильтр SID, громкость, сведение. Звуковой части от C64 нужны только регистры SID
 * и огибающие голосов на конце тактов отсчёта (так она их и читает). Поэтому — конвейер, как у
 * MIDI.ELF: ядро 0 (задача декодера) гонит C64 на кусок вперёд и кладёт на каждый отсчёт снимок
 * регистров $00..$18 и огибающих, помощник на ядре 1 (host->worker_start) по снимкам прошлого куска
 * считает звуковую часть в своих копиях SID. До нормализации отсчёты — бит в бит как у cRSID одним потоком.
 * Процессор читает OSC3 ($D41B, волна голоса 3; 6 из 20 проверенных мелодий) и ENV3 ($D41C): их ядро 0
 * считает само — ENV3 из своих огибающих, OSC3 — своими копиями фаз трёх голосов (синхронизация
 * цепочкой) и волной голоса 3 по тому же снимку (osc3_sample), в тот же момент, что cRSID.
 *
 * Играет основная подмелодия из заголовка. Влево/вправо — время с шагом 10 с,
 * как у MIDI. Длину даёт SIDLEN.BIN по MD5 файла; нет записи — 3 минуты.
 * Последняя секунда — затухание, затем плагин берёт следующий
 * файл.
 *
 * Версия 1.6: каскад CIA A -> B (Stereophonik и другие RSID).
 * Версия 1.5: init и перемотка выполняются порциями в render. Догон считает
 * только C64, ADSR и OSC3/ENV3; волны и фильтры звуковой части пропускаются. Предел времени render проверяется между кусками. Перегрузка не
 * завершает песню. Регистровое ядро 6502 и CIA/VIC — в c64_fast.h. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "zm_app.h"
#include "zm_source.h"
/* Reuse the exact gain calculation while its input is unchanged. */
static float sid_level_powf(float base,float exponent) {
  static float last_base,last_exponent,last_value;
  static int valid;
  if(valid&&base==last_base&&exponent==last_exponent)return last_value;
  const float value=powf(base,exponent);
  last_base=base;last_exponent=exponent;last_value=value;valid=1;
  return value;
}
#define powf sid_level_powf
#include "zm_level.h"
#undef powf

/* Стенд может отключить только последующую обработку для сверки самого cRSID. */
#ifndef SID_LEVEL_ENABLED
#define SID_LEVEL_ENABLED 1
#endif
#ifndef SID_DETAIL_CYCLES
#define SID_DETAIL_CYCLES 0                /* retain whole-chunk timing; fine profiling is optional */
#endif

/* cRSID — одним файлом, как его собирает libcRSID.c, без платформы ПК: звук и файлы — у плеера.
 * Текстовую базу длин cRSID (Songlengths.md5, cRSID.SongLengths) не берём: её разбору нужны sscanf,
 * strchr, strspn и strcspn, которых ядро модулям не даёт. cRSID.SongLengths остаётся NULL, разбор
 * не вызывается; эти функции — только чтобы модуль связался. */
static char* sid_strchr(const char* s, int c) {
  for (;; ++s) {
    if (*s == (char)c) return (char*)s;
    if (*s == 0) return NULL;
  }
}
static size_t sid_strspn(const char* s, const char* set) {
  size_t n = 0;
  while (s[n] != 0 && sid_strchr(set, s[n]) != NULL) ++n;
  return n;
}
static size_t sid_strcspn(const char* s, const char* set) {
  size_t n = 0;
  while (s[n] != 0 && sid_strchr(set, s[n]) == NULL) ++n;
  return n;
}
static int sid_sscanf(const char* s, const char* format, ...) {
  (void)s;
  (void)format;
  return 0;
}
#define strchr sid_strchr
#define strspn sid_strspn
#define strcspn sid_strcspn
#define sscanf sid_sscanf
/* Перед изменением control/AD/SR (смещения 4..6, 11..13, 18..20 каждого SID)
 * огибающие догоняют накопленные такты со старыми регистрами. Громкость D418,
 * частота и фильтр не меняют ADSR; цифровой звук не требует догона на каждой записи. */
static void adsr_catch_up(void);
static void adsr_invalidate(void);
#define CRSID_SID_WRITE_HOOK(address)                                                         \
  do {                                                                                        \
    if (RARELY(((address) & 0xFC00) == 0xD400 || ((address) & 0xFE00) == 0xDE00)) {                \
      if ((0x001C3870u >> ((address) & 31)) & 1u) { adsr_catch_up(); adsr_invalidate(); }             \
    }                                                                                         \
  } while (0)
#include "libcRSID.c"
#undef strchr
#undef strspn
#undef strcspn
#undef sscanf

#include "c64_fast.h"
#include "mahoney_dac.h"

#define RATE 44100u
#define DEFAULT_SECONDS 180u               /* нет в базе — 3 минуты */
#define FADE_FRAMES RATE                   /* последняя секунда — затухание */
#define BASE_NAME "SIDLEN.BIN"
#define BASE_BUCKETS 4096u                 /* корзины по старшим 12 битам MD5 */
#define BASE_TABLE 12u                     /* смещения корзин — после заголовка */
#define SONGS_MAX CRSID_SUBTUNE_AMOUNT_MAX
#define CHUNK 128u                         /* fewer worker handoffs; 2.9 ms of PCM */
#define RENDER_MAX 512u                    /* кадров за render — несколько кусков */
#define CPU_HZ 240000000u                  /* такты ESP (ccount) */
#define SLICE_CYCLES (CPU_HZ / 500u)        /* 2 мс между проверками очередной команды */
#define INIT_STEPS 4096u                    /* верхняя граница порции init и без часов */
#define CHIPS CRSID_SIDCOUNT_MAX           /* SID до четырёх */
#define SNAP_REGS 0x19u                    /* регистры SID $00..$18, что читает звуковая часть */
#define SNAP_ENV SNAP_REGS                 /* за ними — огибающие голосов 1..3 */
#define SNAP_CHIP 0x1Cu                    /* байтов снимка на микросхему */

static const zm_host_t* host;
static zm_source_t table;
static int busy;                           /* cRSID один на модуль: экземпляр — тоже один */

/* Снимок отсчёта: то, что звуковой части cRSID нужно от C64. */
typedef struct {
  int16_t digi;                            /* цифровой звук PSID (cRSID_playPSIDdigi), 0 — нет */
  _Alignas(4) uint8_t chip[CHIPS][SNAP_CHIP]; /* aligned register snapshots */
} Snap;

typedef struct {
  uint32_t step;                           /* шаг фазы за шаг передискретизации */
  uint32_t pw;                             /* порог импульса: 16 бит (WV_PULSE) или 12 (комбинированные) */
  const unsigned char* table;              /* комбинированная волна */
  uint32_t mask;                           /* 6581: старший бит индекса таблицы прочь */
  int env;                                 /* огибающая (у 6581 — через его ЦАП) */
  uint8_t kind, test, sync, ring, dest;    /* dest: 0 — никуда, 1 — мимо фильтра, 2 — в фильтр */
  uint16_t digi_out;                       /* WV_DIGI: старший байт частоты << 8 */
} Voice;

typedef struct {
  Voice voice[3];
  _Alignas(4) uint8_t regs[SNAP_CHIP];
  uint16_t model;
  uint8_t real,valid;
  int cutoff,resonance;
} VoiceCache;

/* Звуковая часть (ядро 1): копии SID cRSID со своими волнами и фильтрами и прочее, что cRSID держит
 * в статических переменных своих функций сведения (C64_SIDrouting.c) и cRSID_generateSample. */
typedef struct {
  cRSID_SIDinstance sid[CHIPS + 1];        /* [1..chips], как cRSID_C64.SID */
  uint8_t rd[CHIPS + 1][0x20];             /* их регистры чтения: OSC3 — отсюда в память C64 */
  int nonfilt[CHIPS + 1], prev_nonfilt[CHIPS + 1];   /* фильтр Найквиста ×2 на SID */
  int filt[CHIPS + 1], prev_filt[CHIPS + 1];
  int oversample_cnt;                      /* cRSID_C64.OverSampleCycleCnt */
  int sample_clock_ratio, attenuation;
  int lfsr;                                /* шум дизеринга cRSID_generateSample */
  short volume_mul;                        /* cRSID.MainVolume * FadeLevel */
  uint8_t chips, stereo;
  int32_t dac_dc[CHIPS + 1];               /* output coupling of calibrated digi DAC, Q8 */
  uint8_t dac_active[CHIPS + 1];
} Synth;

/* Кусок конвейера: снимки отсчётов и звук из них. */
typedef struct {
  Snap* snap;
  int16_t* pcm;                            /* стерео, int16 — как cRSID_generateSample */
  uint32_t n;
  uint32_t stride;                         /* байтов на снимок: на столько SID, сколько их есть */
} Job;
/* Снимок отсчёта i куска: снимки — подряд, по stride байтов (у одной SID — 30, а не 114). */
#define SNAP_AT(job, i) ((Snap*)((uint8_t*)(job)->snap + (size_t)(i) * (job)->stride))

typedef struct {
  uint8_t* file;                           /* копия: из неё cRSID кладёт программу в память C64 */
  uint32_t size;
  cRSID_SIDheader* header;
  uint16_t songs, song;                    /* подмелодий, текущая (1..songs) */
  uint16_t seconds[SONGS_MAX + 1];         /* длины из базы, секунды; 0 — неизвестна */
  uint8_t known;                           /* мелодия есть в базе */
  uint8_t chips;                           /* микросхем SID */
  uint8_t pipelined;                       /* помощник на ядре 1 есть */
  uint8_t pending;                         /* кусок jobs[current] у помощника (или посчитан) */
  uint8_t current;
  uint32_t frame, end;                     /* кадров отдано с начала подмелодии; её длина в кадрах */
  uint32_t emulated;                       /* отсчётов C64 с начала подмелодии */
  zm_level_t level;                        /* общий с TRACKER/OPL замер, состояние в PSRAM */
  uint64_t level_cycles, level_frames;
  uint32_t level_max;
  Job jobs[2];
  uint32_t init_left;                      /* оставшиеся команды init PSID */
  uint32_t init_steps, init_max_slice;      /* диагностика: команды и худшая порция */
  uint64_t init_cycles;                    /* время всех порций, без ожиданий PLAYER */
  uint8_t initializing;                   /* звук начнётся после окончания init */
  uint8_t seeking, seek_prepare;           /* новая позиция — frame; догон — emulated */
  uint32_t seek_frames, seek_max_slice, seek_slices;
  uint64_t seek_cycles;                    /* порции догона, отдельно от init */
  uint8_t ready;                           /* посчитанный кусок, что отдаётся сейчас, */
  uint32_t ready_pos, ready_n;             /* ... с какого отсчёта и сколько в нём */
  char title[64], artist[64], album[64];
  uint16_t year;
  char format[24];
} Source;

/* The waveform/filter state is touched for every oversampling step. Prefer
 * internal RAM; the static copy remains available to stand-alone checks and
 * as a fallback. Preserve its dither state across close/open in one module. */
static VoiceCache params_fallback[CHIPS+1];
static VoiceCache* synth_params=params_fallback;
static Synth synth_fallback;
static Synth* synth_state = &synth_fallback;
#define synth (*synth_state)

/* Горячее состояние ядра 0 (1.2): огибающие ADSR и фазы для OSC3 — во внутренней памяти (host->alloc
 * ZM_MEM_INTERNAL, несколько сотен байт), а не в структуре cRSID в PSRAM. На плате 04.11.2026 поток
 * снимков выбивал её из кэша данных: огибающие стоили ~500 тактов на отсчёт, снимок с OSC3 — ~1800. */
typedef struct {
  uint16_t rc[3];                          /* счётчики скорости голосов 1..3 */
  uint8_t state[3], env[3], exp[3];        /* состояние, огибающая, счётчик экспоненты */
  uint32_t until;                         /* next envelope rate event, for all three voices */
  uint8_t cached;
} Adsr;
typedef struct {                           /* фазы трёх голосов и волна голоса 3 — ради OSC3 */
  uint32_t acc[3], prev[3], out3, lfsr3, ring;
  int digi3;
  unsigned rise;
  uint8_t held_test;
} Osc3;
typedef struct {
  Adsr adsr[CHIPS + 1];
  Osc3 osc[CHIPS + 1];
  const uint8_t* regs[CHIPS + 1];          /* регистры SID в памяти C64 (их пишет процессор) */
  uint8_t* rd[CHIPS + 1];                  /* регистры чтения: OSC3, ENV3 */
  uint8_t is6581[CHIPS + 1];
  cRSID_CPUinstance cpu;                    /* регистры 6502 во внутренней памяти */
  cRSID_CIAinstance cia[3];
  FastCiaIo cia_io;
  FastCiaPlan cia_plan;
  cRSID_VICinstance vic;
  int osc_cnt;                             /* свой счётчик шагов передискретизации */
  uint8_t chips;
  uint8_t last_env3[CHIPS + 1];
} Hot;
static Hot* hot;

static void adsr_invalidate(void) {
  if (hot == NULL) return;
  for (unsigned n = 1; n <= hot->chips; ++n) hot->adsr[n].cached = 0;
}

/* Перед init/закрытием вернуть CIA в стандартную память cRSID. */
static void cia_cache_unbind(void) {
  fast_cia_plan = NULL;
  if (fast_cia_io == NULL) return;
  memcpy(cRSID_C64.IObankRD + 0xDC00, fast_cia_io->rd, 512);
  memcpy(cRSID_C64.IObankWR + 0xDC00, fast_cia_io->wr, 512);
  for (unsigned n = 1; n <= 2; ++n) {
    cRSID_C64.CIA[n].BasePtrRD = cRSID_C64.IObankRD + 0xDB00 + 256 * n;
    cRSID_C64.CIA[n].BasePtrWR = cRSID_C64.IObankWR + 0xDB00 + 256 * n;
  }
  fast_cia_io = NULL;
}

static void cia_cache_bind(void) {
  FastCiaIo* io = &hot->cia_io;
  memcpy(io->rd, cRSID_C64.IObankRD + 0xDC00, 512);
  memcpy(io->wr, cRSID_C64.IObankWR + 0xDC00, 512);
  for (unsigned n = 1; n <= 2; ++n) {
    cRSID_C64.CIA[n].BasePtrRD = io->rd + 256 * (n - 1);
    cRSID_C64.CIA[n].BasePtrWR = io->wr + 256 * (n - 1);
    hot->cia[n] = cRSID_C64.CIA[n];
  }
  fast_cia_io = io;
  fast_cia_plan = &hot->cia_plan;
  fast_cia_plan->valid = 0;
  fast_cia_plan->owner = NULL;
}

/* Такты процессора ESP (у каждого ядра свой счётчик): сколько стоит отсчёт — в журнал при закрытии. */
static INLINE uint32_t ccount(void) {
#if defined(__XTENSA__)
  uint32_t c;
  __asm__ volatile("rsr.ccount %0" : "=a"(c));
  return c;
#elif defined(SID_TEST_CLOCK)
  return SID_TEST_CLOCK();                 /* обвязка ПК: такты понарошку (замок нагрузки) */
#else
  return 0;
#endif
}
static uint64_t cycles_c64, cycles_sound, frames_c64, frames_sound;
/* Ядро 0 по частям (ESP): процессор, CIA и VIC, огибающие, снимок с OSC3. */
static uint64_t cycles_cpu, cycles_ciavic, cycles_adsr, cycles_snap;

/* Стенд ПК (sid_host, -DSID_PROFILE): такты по частям — где искать скорость. */
#ifdef SID_PROFILE
#include <x86intrin.h>
enum { PROF_CPU, PROF_CIAVIC, PROF_ADSR, PROF_SNAP, PROF_WAVES, PROF_NYQ, PROF_OUT, PROF_POST, PROF_LEVEL, PROF_N };
static unsigned long long prof[PROF_N];
#define PROF_AT(v) const unsigned long long v = __rdtsc()
#define PROF_ADD(k, from) (prof[k] += __rdtsc() - (from))
#else
#define PROF_AT(v)
#define PROF_ADD(k, from)
#endif

static uint32_t le32(const uint8_t* p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void osc3_sample(const Snap* snap);

/* --- ядро 0: такты C64 одного отсчёта --------------------------------------------------- */

/* Огибающие ADSR пакетом (1.1, 04.11.2026): cRSID зовёт cRSID_emulateADSRs после каждой команды
 * процессора (5–7 раз на отсчёт, на каждую SID три голоса), а на ПК это половина работы ядра 0 у PSID.
 * Пока регистры SID не меняются, счётчик скорости просто растёт: следующее совпадение с периодом —
 * через period - rc тактов (rc уже не меньше периода — через перенос на 0x8000), и шаги огибающей
 * считаются сразу на все накопленные такты. Совпадения не чаще раза в 9 тактов, а команда — до 7, так
 * что это бит в бит то же, что по командам (сверка — tests/test_sid_source.py, эталон cRSID). Такты
 * копятся в adsr_pending; догон — перед записью процессора в регистры SID (крючок в CPU.c, как cRSID:
 * запись команды влияет на её же такты) и в конце отсчёта (снимок для ядра 1, ENV3). */
static const uint8_t adsr_exponent[256] = {
  1, 30, 30, 30, 30, 30, 30, 16, 16, 16, 16, 16, 16, 16, 16, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 2, 2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};
static const uint16_t adsr_periods[16] = {9,    32,   63,   95,   149,   220,   267,   313,
                                         392,  977,  1954, 3126, 3907, 11720, 19532, 31251};
static uint32_t adsr_pending;              /* тактов с прошлого догона */

static INLINE void adsr_run(Adsr* e, const uint8_t* regs, uint32_t cycles) {
  enum { GATE = 0x01, ATTACK = 0x80, DECAYSUSTAIN = 0x40, HOLDZERON = 0x10, RC_RANGE = 0x8000 };
  if (MOSTLY(e->cached && cycles < e->until)) {
    e->rc[0] = (uint16_t)((e->rc[0] + cycles) & 0x7FFF);
    e->rc[1] = (uint16_t)((e->rc[1] + cycles) & 0x7FFF);
    e->rc[2] = (uint16_t)((e->rc[2] + cycles) & 0x7FFF);
    e->until -= cycles;
    return;
  }
  uint32_t next = UINT32_MAX;
  for (unsigned v = 0; v < 3; ++v) {
    const uint8_t* r = regs + 7 * v;
    const uint8_t ctrl = r[4], ad = r[5], sr = r[6];
    uint8_t state = e->state[v];
    uint32_t rc = e->rc[v];
    uint8_t env = e->env[v], exp = e->exp[v];
    if (RARELY((state & GATE) != (ctrl & GATE))) {      /* фронт gate */
      if (state & GATE) state &= (uint8_t)~(GATE | ATTACK | DECAYSUSTAIN);
      else state = GATE | ATTACK | DECAYSUSTAIN | HOLDZERON;
    }
    uint32_t left = cycles;
    for (;;) {
      const uint32_t period = adsr_periods[state & ATTACK ? ad >> 4 : state & DECAYSUSTAIN ? ad & 0x0F : sr & 0x0F];
      const uint32_t until = rc < period ? period - rc : RC_RANGE - rc + period;
      if (MOSTLY(until > left)) {
        rc += left;
        if (rc >= RC_RANGE) rc -= RC_RANGE;
        break;
      }
      left -= until;
      rc = 0;                                            /* совпадение: rc -= period */
      if ((state & ATTACK) || ++exp == adsr_exponent[env]) {
        exp = 0;
        if (state & HOLDZERON) {
          if (state & ATTACK) {
            ++env;
            if (env == 0xFF) state &= (uint8_t)~ATTACK;
          } else if (!(state & DECAYSUSTAIN) || env != (sr & 0xF0) + (sr >> 4)) {
            --env;
            if (env == 0) state &= (uint8_t)~HOLDZERON;
          }
        }
      }
    }
    e->state[v] = state;
    e->rc[v] = (uint16_t)rc;
    e->env[v] = env;
    e->exp[v] = exp;
    const uint32_t period = adsr_periods[state & ATTACK ? ad >> 4 : state & DECAYSUSTAIN ? ad & 0x0F : sr & 0x0F];
    const uint32_t until = rc < period ? period - rc : RC_RANGE - rc + period;
    if (until < next) next = until;
  }
  e->until = next;
  e->cached = 1;
}

static void adsr_catch_up(void) {
  if (adsr_pending == 0 || hot == NULL) return;
  Hot* const h = hot;
  for (unsigned n = 1; n <= h->chips; ++n) adsr_run(&h->adsr[n], h->regs[n], adsr_pending);
  adsr_pending = 0;
}

/* Первая половина cRSID_emulateC64 (C64/C64.c) слово в слово: такты процессора, CIA, VIC и
 * огибающих до конца отсчёта, часы TOD для PSID; затем — ENV3 для процессора и снимок для ядра 1. */
static INLINE void c64_sample(Snap* snap) {
  if (RARELY(fast_cia_io == NULL && cRSID_C64.RealSIDmode)) cia_cache_bind();
  FASTVAR unsigned char InstructionCycles;
  const int real=cRSID_C64.RealSIDmode,ratio=cRSID_C64.SampleClockRatio;
  int sample_cycles=cRSID_C64.SampleCycleCnt,finished=cRSID_C64.Finished;
  unsigned irq=cRSID_C64.IRQ,nmi=cRSID_C64.NMI;

  uint32_t cpu = 0, ciavic = 0;
  while (sample_cycles <= ratio) {
    PROF_AT(t0);
    const uint32_t c0 = SID_DETAIL_CYCLES ? ccount() : 0;
    if (CALMLY(!real)) {
      if (RARELY(cRSID_C64.FrameCycleCnt >= cRSID.FrameCycles)) {
        cRSID_C64.FrameCycleCnt -= cRSID.FrameCycles;
        if (RARELY(finished)) {
          cRSID_initCPU(cRSID.PlayAddress);
          hot->cpu = cRSID_C64.CPU;
          finished = 0;
          if (LIKELY(cRSID.TimerSource == 0)) cRSID_C64.IObankRD[0xD019] = 0x81;
          else cRSID_C64.IObankRD[0xDC0D] = 0x83;
        }
      }
      if (TIGHTLY(finished == 0)) {
        if (RARELY((InstructionCycles = fast_cpu_mode(&hot->cpu,real,nmi)) >= 0xFE)) {
          InstructionCycles = 6;
          finished = 1;
        }
      } else {
        /* Batch seven-cycle idle steps up to the next PCM/play-call boundary.
         * Timer readback and ADSR receive the same total C64 clocks. */
        const unsigned sample_steps=(unsigned)(ratio-sample_cycles)/(7u<<CRSID_CLOCK_FRACTIONAL_BITS)+1;
        const int frame_left=cRSID.FrameCycles-cRSID_C64.FrameCycleCnt;
        unsigned idle_steps=sample_steps;
        if(frame_left>0) {
          const unsigned frame_steps=((unsigned)frame_left+6u)/7u;
          if(idle_steps>frame_steps)idle_steps=frame_steps;
        } else idle_steps=1;
        InstructionCycles=(unsigned char)(7u*idle_steps);
      }
      cRSID_C64.FrameCycleCnt += InstructionCycles;
      cRSID_C64.IObankRD[0xDC04] += InstructionCycles;
      cpu += SID_DETAIL_CYCLES ? ccount() - c0 : 0;
    } else {
      if (RARELY(fast_interrupts_state(&hot->cpu,irq,nmi))) {
        finished = 0;
        InstructionCycles = 7;
      } else if (MOSTLY(finished == 0)) {
        if (RARELY((InstructionCycles = fast_cpu_mode(&hot->cpu,real,nmi)) >= 0xFE)) {
          finished = 1;
          InstructionCycles = 6;
        }
      } else {
        InstructionCycles = 7;
      }
      PROF_ADD(PROF_CPU, t0);
      PROF_AT(t1);
      const uint32_t c1 = SID_DETAIL_CYCLES ? ccount() : 0;
      cpu += c1 - c0;
      irq=fast_cia(&hot->cia[1],InstructionCycles);
      nmi=fast_cia(&hot->cia[2],InstructionCycles);
      irq|=fast_vic(&hot->vic,InstructionCycles);
      PROF_ADD(PROF_CIAVIC, t1);
      ciavic += SID_DETAIL_CYCLES ? ccount() - c1 : 0;
    }
    sample_cycles += (InstructionCycles << CRSID_CLOCK_FRACTIONAL_BITS);
    adsr_pending += InstructionCycles;     /* огибающие — пакетом (adsr_catch_up) */
  }
  cRSID_C64.SampleCycleCnt=sample_cycles-ratio;
  cRSID_C64.IRQ=irq;cRSID_C64.NMI=nmi;cRSID_C64.Finished=finished;
  PROF_AT(t2);
  const uint32_t c2 = SID_DETAIL_CYCLES ? ccount() : 0;
  adsr_catch_up();
  PROF_ADD(PROF_ADSR, t2);
  const uint32_t c3 = SID_DETAIL_CYCLES ? ccount() : 0;
  cycles_cpu += cpu;
  cycles_ciavic += ciavic;
  cycles_adsr += c3 - c2;
  PROF_AT(t3);

  if (CALMLY(!cRSID_C64.RealSIDmode)) {     /* часы TOD CIA (Kawasaki Synthesizer Demo) */
    --cRSID_C64.TenthSecondCnt;
    if (RARELY(cRSID_C64.TenthSecondCnt <= 0)) {
      cRSID_C64.TenthSecondCnt = cRSID_C64.SampleRate / 10;
      ++(cRSID_C64.IObankRD[0xDC08]);
      if (RARELY(cRSID_C64.IObankRD[0xDC08] >= 10)) {
        cRSID_C64.IObankRD[0xDC08] = 0;
        ++(cRSID_C64.IObankRD[0xDC09]);
      }
    }
  }
  if (MOSTLY(cRSID_C64.SecondCnt < cRSID_C64.SampleRate)) ++cRSID_C64.SecondCnt;
  else {
    cRSID_C64.SecondCnt = 0;
    if (MOSTLY(cRSID.PlayTime < 3600)) ++cRSID.PlayTime;
  }

  snap->digi = RARELY(cRSID.PSIDdigiMode) ? cRSID_playPSIDdigi() : 0;
  Hot* const h = hot;
  for (unsigned n = 1; n <= h->chips; ++n) {
    uint8_t* chip = snap->chip[n - 1];
    const Adsr* e = &h->adsr[n];
    _Static_assert(offsetof(cRSID_C64instance,IObankWR)%4==0,"SID source alignment");
    /* Copy seven aligned words, then replace the last three bytes with ENV. */
    __builtin_memcpy(__builtin_assume_aligned(chip,4),__builtin_assume_aligned(h->regs[n],4),SNAP_CHIP);
    chip[SNAP_ENV] = e->env[0];
    chip[SNAP_ENV + 1] = e->env[1];
    chip[SNAP_ENV + 2] = e->env[2];
    if (h->last_env3[n] != e->env[2]) {
      h->last_env3[n] = e->env[2];
      h->rd[n][0x1C] = e->env[2];          /* ENV3 changes are still visible immediately */
    }
  }
  osc3_sample(snap);                       /* OSC3 — тоже здесь: процессор читает его следующим отсчётом */
  PROF_ADD(PROF_SNAP, t3);
  cycles_snap += SID_DETAIL_CYCLES ? ccount() - c3 : 0;
}

/* --- ядро 1: звуковая часть одного отсчёта по снимку ------------------------------------ */

/* Волны одной SID за отсчёт (1.1, 04.11.2026) — то же, что cRSID_emulateHQwaves на каждом шаге
 * передискретизации и фильтр Найквиста cRSID_emulateOversampledSIDwaves, бит в бит, но всё, что за
 * отсчёт не меняется (регистры, огибающая, разводка на фильтр, шаг фазы), считается раз на отсчёт,
 * а состояние голосов — в локальных переменных, а не в структуре cRSID. На шаге — только фаза, волна,
 * умножение на огибающую и два фильтра. Порядок голосов внутри шага — 1, 2, 3: синхронизация и
 * кольцевая модуляция берут старший бит фазы предыдущего (у голоса 1 — голоса 3 прошлого шага).
 * SID друг от друга не зависят: каждая считается на все шаги сразу. В sid->NonFiltedSample и
 * FilterInputSample — среднее за отсчёт, как после cRSID_emulateOversampledSIDwaves. */
enum {
  WV_NONE, WV_NOISE, WV_PULSE, WV_SAW, WV_TRI, WV_COMBINED, WV_COMBINED_PULSE, WV_PULTRI, WV_HOLD,
  WV_DIGI, WV_ZERO
};


enum { CYCLES = CRSID_OVERSAMPLING_CYCLES, ACC_RANGE = 0x1000000, ACC_MSB = 0x800000, NOISE_CLOCK = 0x100000,
       WAVE_MID = 0x8000, ENV_DIV = 256 * CRSID_WAVGEN_PREDIV, DIGI_HOLD = 24, STEPS_MAX = 8 };

#include "synth_params.h"

/* Один голос на все шаги отсчёта — короткий цикл под его волну. ring_in и rise_in — старший бит фазы
 * предыдущего голоса и его подъём на каждом шаге (синхронизация, кольцо), ring_out и rise_out — свои,
 * для следующего голоса; wave — волна голоса на каждом шаге. */
typedef struct {
  uint32_t acc, prev, out, lfsr;
  int digi;
} VoiceState;

static INLINE void voice_pass(const Voice* x, VoiceState* vs, int real, unsigned steps, const uint32_t* ring_in,
                              const uint8_t* rise_in, uint32_t* ring_out, uint8_t* rise_out, uint32_t* wave) {
  uint32_t a = vs->acc, prev = vs->prev, w = vs->out;
  const uint32_t step = x->step;
  const int plain = !x->test && !x->sync;  /* фаза только растёт: самый частый случай */
/* Фаза шага s: тест — 0, синхронизация по подъёму предыдущего голоса — 0, иначе + шаг по модулю 2^24. */
#define PHASE(s)                                                              \
  if (MOSTLY(plain)) {                                                        \
    a = (a + step) & (ACC_RANGE - 1);                                         \
  } else if (x->test || (x->sync && rise_in[s])) {                            \
    a = 0;                                                                    \
  } else {                                                                    \
    a = (a + step) & (ACC_RANGE - 1);                                         \
  }                                                                           \
  const uint32_t msb = a & ACC_MSB;                                           \
  rise_out[s] = msb > (prev & ACC_MSB);                                       \
  ring_out[s] = msb
  switch (x->kind) {
    case WV_SAW:
      for (unsigned s = 0; s < steps; ++s) {
        PHASE(s);
        wave[s] = w = a >> 8;
        prev = a;
      }
      break;
    case WV_PULSE:
      for (unsigned s = 0; s < steps; ++s) {
        PHASE(s);
        wave[s] = w = (a >> 8) >= x->pw || x->test ? 0xFFFF : 0;
        prev = a;
      }
      break;
    case WV_TRI:
      for (unsigned s = 0; s < steps; ++s) {
        PHASE(s);
        if (MOSTLY(!real || vs->digi <= 0)) {
          const uint32_t t = a ^ (x->ring ? ring_in[s] : 0);
          w = ((t ^ (t & ACC_MSB ? ACC_RANGE - 1 : 0)) >> 7) & 0xFFFF;
        } else {
          vs->digi -= CYCLES;                /* SounDemon: треугольник не трогает ЦАП */
        }
        wave[s] = w;
        prev = a;
      }
      break;
    case WV_NOISE: {
      uint32_t t = vs->lfsr;
      for (unsigned s = 0; s < steps; ++s) {
        PHASE(s);
        if (RARELY((a & NOISE_CLOCK) != (prev & NOISE_CLOCK))) {
          const uint32_t fb = ((t & 0x400000) ^ ((t & 0x20000) << 5)) != 0;
          t = ((t << 1) | fb | x->test) & 0x7FFFFF;
        }
        wave[s] = w = ((t & 0x100000) >> 5) | ((t & 0x40000) >> 4) | ((t & 0x4000) >> 1) | ((t & 0x800) << 1) |
                      ((t & 0x200) << 2) | ((t & 0x20) << 5) | ((t & 0x04) << 7) | ((t & 0x01) << 8);
        prev = a;
      }
      vs->lfsr = t;
      break;
    }
    case WV_COMBINED:
      for (unsigned s = 0; s < steps; ++s) {
        PHASE(s);
        wave[s] = w = (uint32_t)x->table[(a >> 12) & x->mask] << 8;
        prev = a;
      }
      break;
    case WV_COMBINED_PULSE:
      for (unsigned s = 0; s < steps; ++s) {
        PHASE(s);
        wave[s] = w = (a >> 12) >= x->pw || x->test ? (uint32_t)x->table[(a >> 12) & x->mask] << 8 : 0;
        prev = a;
      }
      break;
    case WV_PULTRI:
      for (unsigned s = 0; s < steps; ++s) {
        PHASE(s);
        const uint32_t t = a ^ (x->ring ? ring_in[s] : 0);
        wave[s] = w = (a >> 12) >= x->pw || x->test ? (uint32_t)x->table[t >> 12] << 8 : 0;
        prev = a;
      }
      break;
    case WV_DIGI:
      for (unsigned s = 0; s < steps; ++s) {
        PHASE(s);
        wave[s] = w = x->digi_out;
        vs->digi = DIGI_HOLD;
        prev = a;
      }
      break;
    case WV_ZERO:
      for (unsigned s = 0; s < steps; ++s) {
        PHASE(s);
        wave[s] = w = 0;
        prev = a;
      }
      break;
    default:                                 /* WV_HOLD: ЦАП держит прежнее */
      for (unsigned s = 0; s < steps; ++s) {
        PHASE(s);
        wave[s] = w;
        prev = a;
      }
      break;
  }
#undef PHASE
  vs->acc = a;
  vs->prev = prev;
  vs->out = w;
}

/* Independent voices need only their final phase/rise for the next sample.
 * HOLD/ZERO/DIGI and muted simple waves have no intermediate state to retain.
 * Keep every oversampling contribution for the Nyquist filters, but avoid
 * temporary wave/ring arrays and repeated multiplication of pulse levels. */
static INLINE int voice_flat(const Voice* x,VoiceState* vs,int real,unsigned steps,
                             int* nonfilt,int* filt,uint32_t* ring_last,uint8_t* rise_last) {
  if(!steps)return 0;
  if(x->kind!=WV_HOLD&&x->kind!=WV_ZERO&&x->kind!=WV_DIGI&&
     x->kind!=WV_PULSE&&x->kind!=WV_SAW&&x->kind!=WV_TRI)return 0;
  if(x->kind==WV_TRI&&real&&vs->digi>0)return 0;
  const unsigned kind=x->kind;
  const int env=x->dest?x->env:0;
  int* to=x->dest==2?filt:nonfilt;
  uint32_t a=vs->acc,w=vs->out;
  const int constant=kind==WV_HOLD||kind==WV_ZERO||kind==WV_DIGI||x->test;
  if(constant||!env) {
    a=x->test?0:(a+x->step*steps)&(ACC_RANGE-1);
    switch(kind) {
      case WV_ZERO:w=0;break;
      case WV_DIGI:w=x->digi_out;vs->digi=DIGI_HOLD;break;
      case WV_PULSE:w=x->test||(a>>8)>=x->pw?0xFFFF:0;break;
      case WV_SAW:w=a>>8;break;
      case WV_TRI:w=((a^(a&ACC_MSB?ACC_RANGE-1:0))>>7)&0xFFFF;break;
      default:break;
    }
    const int value=((int)w-WAVE_MID)*env/ENV_DIV;
    if(value)for(unsigned s=0;s<steps;++s)to[s]+=value;
  } else if(kind==WV_PULSE) {
    const int low=-(int)WAVE_MID*env/ENV_DIV,high=(0xFFFF-WAVE_MID)*env/ENV_DIV;
    for(unsigned s=0;s<steps;++s) {
      a=(a+x->step)&(ACC_RANGE-1);
      w=(a>>8)>=x->pw?0xFFFF:0;
      to[s]+=w?high:low;
    }
  } else {
    for(unsigned s=0;s<steps;++s) {
      a=(a+x->step)&(ACC_RANGE-1);
      w=kind==WV_SAW?a>>8:((a^(a&ACC_MSB?ACC_RANGE-1:0))>>7)&0xFFFF;
      to[s]+=((int)w-WAVE_MID)*env/ENV_DIV;
    }
  }
  const uint32_t before_last=x->test?0:steps>1?(a-x->step)&(ACC_RANGE-1):vs->prev;
  *ring_last=a&ACC_MSB;
  *rise_last=(a&ACC_MSB)>(before_last&ACC_MSB);
  vs->acc=vs->prev=a;vs->out=w;
  return 1;
}

static INLINE void synth_chip(cRSID_SIDinstance* sid, const uint8_t* regs, const uint8_t* env, unsigned steps,
                              int* nonfilt_io, int* prev_nonfilt_io, int* filt_io, int* prev_filt_io, VoiceCache* params) {
  /* Удерживаемый TEST + pulse: все фазы нулевые, выходы постоянны. Сохраняем
   * точную последовательность фильтра Найквиста и состояние для выхода из TEST. */
  if (RARELY((regs[4] & 0xF8) == 0x48 && (regs[11] & 0xF8) == 0x48 &&
             (regs[18] & 0xF8) == 0x48)) {
    params->valid=0;
    int to_nonfilt = 0, to_filt = 0;
    const unsigned route = regs[0x17], volband = regs[0x18];
    for (unsigned i = 0; i < 3; ++i) {
      const unsigned voice = 7 * i;
      const int e = sid->ChipModel == 8580 ? env[i] : cRSID_ADSR_DAC_6581[env[i]];
      const int value = (0xFFFF - WAVE_MID) * e / ENV_DIV;
      if (route & (1u << i)) to_filt += value;
      else if (i != 2 || !(volband & 0x80)) to_nonfilt += value;
      sid->PhaseAccu[voice] = sid->PrevPhaseAccu[voice] = 0;
      sid->PrevWavGenOut[voice] = 0xFFFF;
    }
    sid->RingSourceMSB = sid->SyncSourceMSBrise = 0;
    sid->BasePtrRD[0x1B] = 0xFF;
    int nonfilt = *nonfilt_io, prev_nonfilt = *prev_nonfilt_io;
    int filt = *filt_io, prev_filt = *prev_filt_io;
    int sum_nonfilt = 0, sum_filt = 0;
    for (unsigned i = 0; i < steps; ++i) {
      int t = prev_nonfilt; prev_nonfilt = nonfilt;
      nonfilt += (to_nonfilt + nonfilt * 3 - (t << 2)) >> 3;
      t = prev_filt; prev_filt = filt;
      filt += (to_filt + filt * 3 - (t << 2)) >> 3;
      sum_nonfilt += nonfilt; sum_filt += filt;
    }
    *nonfilt_io = nonfilt; *prev_nonfilt_io = prev_nonfilt;
    *filt_io = filt; *prev_filt_io = prev_filt;
    sid->NonFiltedSample = sum_nonfilt / (int)steps;
    sid->FilterInputSample = sum_filt / (int)steps;
    return;
  }

  const int count=(int)steps;
  const int real=cRSID_C64.RealSIDmode;
  synth_prepare_params(params,sid,regs,env,real);
  Voice* v=params->voice;
  /* Состояние голосов — в регистры. */
  uint32_t acc0 = (uint32_t)sid->PhaseAccu[0], acc1 = (uint32_t)sid->PhaseAccu[7], acc2 = (uint32_t)sid->PhaseAccu[14];
  uint32_t prev0 = (uint32_t)sid->PrevPhaseAccu[0], prev1 = (uint32_t)sid->PrevPhaseAccu[7],
           prev2 = (uint32_t)sid->PrevPhaseAccu[14];
  uint32_t out[3] = {sid->PrevWavGenOut[0], sid->PrevWavGenOut[7], sid->PrevWavGenOut[14]};
  int digi[3] = {sid->PrevSounDemonDigiWF[0], sid->PrevSounDemonDigiWF[7], sid->PrevSounDemonDigiWF[14]};
  uint32_t lfsr[3] = {sid->NoiseLFSR[0], sid->NoiseLFSR[7], sid->NoiseLFSR[14]};
  uint32_t ring = sid->RingSourceMSB;
  unsigned rise = sid->SyncSourceMSBrise;
  int nonfilt = *nonfilt_io, prev_nonfilt = *prev_nonfilt_io, filt = *filt_io, prev_filt = *prev_filt_io;
  int sum_nonfilt = 0, sum_filt = 0;

  if (MOSTLY(!v[0].sync && !v[0].ring)) {
    /* Голос 1 не берёт ничего у голоса 3 прошлого шага — голоса по очереди, каждый на все шаги:
     * голос 2 получает старший бит и подъём голоса 1 по шагам из массивов, голос 3 — голоса 2. */
    VoiceState vs[3];
    const uint32_t accs[3] = {acc0, acc1, acc2}, prevs[3] = {prev0, prev1, prev2};
    uint32_t ring_s[4][STEPS_MAX], wave[STEPS_MAX];
    uint8_t rise_s[4][STEPS_MAX];
    int to_nonfilt[STEPS_MAX] = {0}, to_filt[STEPS_MAX] = {0};
    const int independent=!(v[1].sync||v[1].ring||v[2].sync||v[2].ring);
    for (unsigned i = 0; i < 3; ++i) {
      vs[i].acc = accs[i];
      vs[i].prev = prevs[i];
      vs[i].out = out[i];
      vs[i].lfsr = lfsr[i];
      vs[i].digi = digi[i];
      if(independent&&voice_flat(&v[i],&vs[i],real,steps,to_nonfilt,to_filt,
                                  &ring_s[i+1][steps-1],&rise_s[i+1][steps-1]))continue;
      voice_pass(&v[i], &vs[i], real, steps, ring_s[i], rise_s[i], ring_s[i + 1], rise_s[i + 1], wave);
      const int e = v[i].env;
      if (v[i].dest != 0 && e != 0) {      /* огибающая 0 или голос никуда — вклада нет */
        int* to = v[i].dest == 2 ? to_filt : to_nonfilt;
        for (unsigned s = 0; s < steps; ++s) to[s] += ((int)wave[s] - WAVE_MID) * e / ENV_DIV;
      }
    }
    for (unsigned s = 0; s < steps; ++s) {
      int t = prev_nonfilt;
      prev_nonfilt = nonfilt;
      nonfilt += (to_nonfilt[s] + nonfilt * 3 - (t << 2)) >> 3;
      t = prev_filt;
      prev_filt = filt;
      filt += (to_filt[s] + filt * 3 - (t << 2)) >> 3;
      sum_nonfilt += nonfilt;
      sum_filt += filt;
    }
    acc0 = vs[0].acc, acc1 = vs[1].acc, acc2 = vs[2].acc;
    prev0 = vs[0].prev, prev1 = vs[1].prev, prev2 = vs[2].prev;
    for (unsigned i = 0; i < 3; ++i) {
      out[i] = vs[i].out;
      lfsr[i] = vs[i].lfsr;
      digi[i] = vs[i].digi;
    }
    ring = ring_s[3][steps - 1];
    rise = rise_s[3][steps - 1];
    steps = 0;                               /* общий цикл ниже — не нужен */
  }

  for (unsigned s = 0; s < steps; ++s) {
    int to_nonfilt = 0, to_filt = 0;
    for (unsigned i = 0; i < 3; ++i) {
      const Voice* x = &v[i];
      uint32_t* accp = i == 0 ? &acc0 : i == 1 ? &acc1 : &acc2;
      uint32_t* prevp = i == 0 ? &prev0 : i == 1 ? &prev1 : &prev2;
      uint32_t a = *accp;
      if (RARELY(x->test || (x->sync && rise))) a = 0;           /* тест или синхронизация */
      else {
        a += x->step;
        if (RARELY(a >= ACC_RANGE)) a -= ACC_RANGE;
      }
      a &= ACC_RANGE - 1;
      const uint32_t msb = a & ACC_MSB;
      rise = msb > (*prevp & ACC_MSB);
      uint32_t w = out[i];
      switch (x->kind) {
        case WV_SAW: w = a >> 8; break;
        case WV_PULSE: w = (a >> 8) >= x->pw || x->test ? 0xFFFF : 0; break;
        case WV_TRI:
          if (MOSTLY(!real || digi[i] <= 0)) {
            const uint32_t t = a ^ (x->ring ? ring : 0);
            w = ((t ^ (t & ACC_MSB ? ACC_RANGE - 1 : 0)) >> 7) & 0xFFFF;
          } else {
            digi[i] -= CYCLES;
          }
          break;
        case WV_NOISE: {
          uint32_t t = lfsr[i];
          if (RARELY((a & NOISE_CLOCK) != (*prevp & NOISE_CLOCK))) {
            const uint32_t fb = ((t & 0x400000) ^ ((t & 0x20000) << 5)) != 0;
            t = ((t << 1) | fb | x->test) & 0x7FFFFF;
            lfsr[i] = t;
          }
          w = ((t & 0x100000) >> 5) | ((t & 0x40000) >> 4) | ((t & 0x4000) >> 1) | ((t & 0x800) << 1) |
              ((t & 0x200) << 2) | ((t & 0x20) << 5) | ((t & 0x04) << 7) | ((t & 0x01) << 8);
          break;
        }
        case WV_COMBINED: w = (uint32_t)x->table[(a >> 12) & x->mask] << 8; break;
        case WV_COMBINED_PULSE:
          w = (a >> 12) >= x->pw || x->test ? (uint32_t)x->table[(a >> 12) & x->mask] << 8 : 0;
          break;
        case WV_PULTRI: {
          const uint32_t t = a ^ (x->ring ? ring : 0);
          w = (a >> 12) >= x->pw || x->test ? (uint32_t)x->table[t >> 12] << 8 : 0;
          break;
        }
        case WV_DIGI: w = x->digi_out; digi[i] = DIGI_HOLD; break;
        case WV_ZERO: w = 0; break;
        default: break;                                          /* WV_HOLD: ЦАП держит */
      }
      out[i] = w;
      *prevp = a;
      *accp = a;
      ring = msb;
      if (x->dest == 2) to_filt += ((int)w - WAVE_MID) * x->env / ENV_DIV;
      else if (x->dest == 1) to_nonfilt += ((int)w - WAVE_MID) * x->env / ENV_DIV;
    }
    /* Чебышёв 2-го порядка ~18 кГц: output += (input + output*3 - prev*4) >> 3 */
    int t = prev_nonfilt;
    prev_nonfilt = nonfilt;
    nonfilt += (to_nonfilt + nonfilt * 3 - (t << 2)) >> 3;
    t = prev_filt;
    prev_filt = filt;
    filt += (to_filt + filt * 3 - (t << 2)) >> 3;
    sum_nonfilt += nonfilt;
    sum_filt += filt;
  }
  sid->PhaseAccu[0] = (int)acc0, sid->PhaseAccu[7] = (int)acc1, sid->PhaseAccu[14] = (int)acc2;
  sid->PrevPhaseAccu[0] = (int)prev0, sid->PrevPhaseAccu[7] = (int)prev1, sid->PrevPhaseAccu[14] = (int)prev2;
  for (unsigned i = 0; i < 3; ++i) {
    sid->PrevWavGenOut[7 * i] = out[i];
    sid->PrevSounDemonDigiWF[7 * i] = (signed char)digi[i];
    sid->NoiseLFSR[7 * i] = lfsr[i];
  }
  sid->RingSourceMSB = ring;
  sid->SyncSourceMSBrise = (unsigned char)rise;
  sid->BasePtrRD[0x1B] = (unsigned char)(out[2] >> 8);           /* OSC3 */
  *nonfilt_io = nonfilt, *prev_nonfilt_io = prev_nonfilt, *filt_io = filt, *prev_filt_io = prev_filt;
  sid->NonFiltedSample = sum_nonfilt / count;
  sid->FilterInputSample = sum_filt / count;
}

/* cRSID_emulateOversampledSIDwaves + cRSID_emulateOversampledSIDoutputs (C64_SIDrouting.c) и конец
 * cRSID_generateSample (host/audio.c) — на копиях SID synth. Отсчёт int16 в pcm[0..1]. */
/* Frozen DC voices, two routed through the filter: D418's filter/voice3 bits
 * form additional DAC levels. Splitting D418 into a smoothed volume and a
 * separate 4-bit digi, as cRSID does, destroys this encoding. Match the hardware
 * setup, not a tune name. The measured table applies only to an 8580. */
static INLINE int mahoney_mode(const cRSID_SIDinstance* sid, const uint8_t* r) {
  return sid->ChipModel == 8580 && r[4] == 0x49 && r[11] == 0x49 && r[18] == 0x49 &&
         (r[6] & 0xF0) == 0xF0 && (r[13] & 0xF0) == 0xF0 && (r[20] & 0xF0) == 0xF0 &&
         (r[21] & 7) == 7 && r[22] == 0xFF && r[23] == 3;
}

static INLINE int synth_output(unsigned n) {
  cRSID_SIDinstance* sid = &synth.sid[n];
  const int ordinary = synth_cached_output(sid,&synth_params[n]); /* keep state on mode transitions */
  if (RARELY(cRSID_C64.RealSIDmode && mahoney_mode(sid, sid->BasePtr))) {
    const int signal = mahoney_dac_8580[sid->BasePtr[24]] * 4;
    /* AC coupling: keep DC out of normalization/limiting and DMA gaps.
     * One pole ~14 Hz at 44.1 kHz; Q8 retains fractional state. */
    if (!synth.dac_active[n]) synth.dac_dc[n] = signal * 256;
    synth.dac_active[n] = 1;
    synth.dac_dc[n] += (signal * 256 - synth.dac_dc[n]) / 512;
    sid->Output = signal - synth.dac_dc[n] / 256;
    return sid->Output;
  }
  synth.dac_active[n] = 0;
  return ordinary;
}

static INLINE void synth_sample(const Snap* snap, int16_t* pcm) {
  Synth* const st = &synth;
  FASTVAR int Temp;
  FASTVAR unsigned char HQsampleCount = 0;
  cRSID_SIDwavOutput w;
  const unsigned chips = st->chips;

#ifndef SID_CRSID_WAVES
  /* Шагов передискретизации в отсчёте (5 или 6) — как считает cRSID. */
  unsigned steps = 0;
  while (st->oversample_cnt <= st->sample_clock_ratio) {
    st->oversample_cnt += (CRSID_OVERSAMPLING_CYCLES << CRSID_CLOCK_FRACTIONAL_BITS);
    ++steps;
  }
  st->oversample_cnt -= st->sample_clock_ratio;
  PROF_AT(t4);
  for (unsigned n = 1; n <= chips; ++n) {
    cRSID_SIDinstance* sid = &st->sid[n];
    sid->BasePtr = (unsigned char*)snap->chip[n - 1];
    synth_chip(sid, snap->chip[n - 1], snap->chip[n - 1] + SNAP_ENV, steps, &st->nonfilt[n], &st->prev_nonfilt[n],
               &st->filt[n], &st->prev_filt[n], &synth_params[n]);
  }
  PROF_ADD(PROF_WAVES, t4);
  (void)Temp;
  (void)HQsampleCount;
  (void)w;
#else
  /* Сверка: волны самим cRSID (cRSID_emulateHQwaves на каждом шаге). */
  for (unsigned n = 1; n <= chips; ++n) {
    cRSID_SIDinstance* sid = &st->sid[n];
    sid->BasePtr = (unsigned char*)snap->chip[n - 1];
    sid->EnvelopeCounter[0] = snap->chip[n - 1][SNAP_ENV];
    sid->EnvelopeCounter[7] = snap->chip[n - 1][SNAP_ENV + 1];
    sid->EnvelopeCounter[14] = snap->chip[n - 1][SNAP_ENV + 2];
    sid->NonFiltedSample = sid->FilterInputSample = 0;
  }
  while (st->oversample_cnt <= st->sample_clock_ratio) {
    for (unsigned n = 1; n <= chips; ++n) {
      cRSID_SIDinstance* sid = &st->sid[n];
      PROF_AT(t4);
      w = cRSID_emulateHQwaves(sid, CRSID_OVERSAMPLING_CYCLES);
      PROF_ADD(PROF_WAVES, t4);
      PROF_AT(t5);
      /* Чебышёв 2-го порядка ~18 кГц: output += (input + output*3 - prev*4) >> 3 */
      Temp = st->prev_nonfilt[n];
      st->prev_nonfilt[n] = st->nonfilt[n];
      st->nonfilt[n] += (w.NonFilted + st->nonfilt[n] * 3 - (Temp << 2)) >> 3;
      Temp = st->prev_filt[n];
      st->prev_filt[n] = st->filt[n];
      st->filt[n] += (w.FilterInput + st->filt[n] * 3 - (Temp << 2)) >> 3;
      /* cRSID 1.58 четвёртой SID складывает фильтр третьей (FiltSample3) — здесь каждой свой. */
      sid->NonFiltedSample += st->nonfilt[n];
      sid->FilterInputSample += st->filt[n];
      PROF_ADD(PROF_NYQ, t5);
    }
    ++HQsampleCount;
    st->oversample_cnt += (CRSID_OVERSAMPLING_CYCLES << CRSID_CLOCK_FRACTIONAL_BITS);
  }
  st->oversample_cnt -= st->sample_clock_ratio;

  for (unsigned n = 1; n <= chips; ++n) {
    st->sid[n].NonFiltedSample /= HQsampleCount;
    st->sid[n].FilterInputSample /= HQsampleCount;
  }
#endif
  PROF_AT(t6);

  int L, R;
  if (st->stereo == CRSID_CHANNELMODE_MONO) {
    L = synth_output(1);
    for (unsigned n = 2; n <= chips; ++n) L += synth_output(n);
    R = L /= st->attenuation;
  } else {                                 /* CRSID_CHANNELMODE_NARROW: узкое стерео */
    L = R = 0;
    for (unsigned n = 1; n <= chips; ++n) {
      cRSID_SIDinstance* sid = &st->sid[n];
      const int Tmp = synth_output(n);
      const int Tmp2 = Tmp >> CRSID_CHANNELPANNING_DIVSHIFTS;
      if (sid->Channel == CRSID_CHANNEL_LEFT) {
        if (st->dac_active[n]) L += Tmp * 2; /* a stereo DAC has independent channels */
        else { L += Tmp + Tmp2; R += Tmp - Tmp2; }
      } else if (sid->Channel == CRSID_CHANNEL_RIGHT) {
        if (st->dac_active[n]) R += Tmp * 2;
        else { R += Tmp + Tmp2; L += Tmp - Tmp2; }
      } else {
        L += Tmp;
        R += Tmp;
      }
    }
    L /= st->attenuation;
    R /= st->attenuation;
  }
  PROF_ADD(PROF_OUT, t6);
  PROF_AT(t7);
  /* Конец cRSID_generateSample: цифровой звук PSID, громкость, шум дизеринга, насыщение. */
  enum { MIX_SCALING = 256 * 16, DITHERNOISE_DIVSHIFTS = 6, DITHERNOISE_MID = 2 };
  L += snap->digi;
  R += snap->digi;
  L = (L * st->volume_mul) / MIX_SCALING;
  R = (R * st->volume_mul) / MIX_SCALING;
  const int fb = ((st->lfsr & 0x800000) ^ ((st->lfsr & 0x004000) << 9));
  st->lfsr <<= 1;
  if (fb) st->lfsr |= 2;
  const int lfsr = st->lfsr;
  const signed short Insert =
      (signed char)((unsigned char)((((lfsr >> 16) & 0x99) + ((lfsr >> 8) & 0x72) + (lfsr & 0xB4) + 0x7C) &
                                    0xFF) >> DITHERNOISE_DIVSHIFTS) -
      DITHERNOISE_MID;
  L += Insert;
  R += Insert;
  if (RARELY(L >= 32767)) L = 32767;
  else if (RARELY(L <= -32768)) L = -32768;
  if (RARELY(R >= 32767)) R = 32767;
  else if (RARELY(R <= -32768)) R = -32768;
  pcm[0] = (int16_t)L;
  pcm[1] = (int16_t)R;
  PROF_ADD(PROF_POST, t7);
}

static void synth_job(void* arg) {
  const Job* job = (const Job*)arg;
  const uint32_t started = ccount();
  for (uint32_t i = 0; i < job->n; ++i) synth_sample(SNAP_AT(job, i), job->pcm + 2 * i);
  cycles_sound += ccount() - started;
  frames_sound += job->n;
}

/* Звуковая часть — заново для подмелодии: копии SID с их состоянием после init (cRSID_initC64). */
static void synth_reset(unsigned chips) {
  static int lfsr = 0x0055A5AA;            /* шум дизеринга идёт дальше, как static в cRSID */
  if (synth.chips != 0) lfsr = synth.lfsr;
  memset(&synth, 0, sizeof(synth));
  memset(synth_params,0,(chips+1)*sizeof(*synth_params));
  synth.chips = (uint8_t)chips;
  synth.stereo = chips > 1 ? cRSID.Stereo : CRSID_CHANNELMODE_MONO;
  synth.sample_clock_ratio = cRSID_C64.SampleClockRatio;
  synth.attenuation = cRSID_C64.Attenuation;
  synth.oversample_cnt = cRSID_C64.OverSampleCycleCnt;
  synth.volume_mul = (short)(cRSID.MainVolume * cRSID_C64.FadeLevel);
  synth.lfsr = lfsr;
  for (unsigned n = 1; n <= chips; ++n) {
    synth.sid[n] = cRSID_C64.SID[n];
    synth.sid[n].BasePtrRD = synth.rd[n];
  }
  /* Теневые копии звуковой задачи cRSID (cRSID_generateSound) — для кода cRSID на ядре 1. */
  cRSID_C64.RealSIDmode = cRSID.RealSIDmode;
  cRSID_C64.AudioThread_SIDchipCount = cRSID_C64.SIDchipCount;
  cRSID_C64.Stereo = synth.stereo;
  cRSID_C64.HighQualitySID = cRSID.HighQualitySID;
  cRSID_C64.HighQualityResampler = cRSID.HighQualityResampler;
}

/* OSC3 одной SID за отсчёт на ядре 0, по тому же снимку, что получит ядро 1 (1.2: развёрнуто, всё в
 * регистрах): фазы трёх голосов на всех шагах — голос синхронизуется по подъёму старшего бита фазы
 * предыдущего (у голоса 1 — голоса 3 прошлого шага), — шум голоса 3 тикает на каждом шаге, треугольник
 * SounDemon держит ЦАП по шагам; прочие волны голоса 3 зависят только от его последней фазы и старшего
 * бита голоса 2 — считаются раз, после шагов. Итог — в регистр чтения $1B, как его пишет
 * cRSID_emulateHQwaves. */
static INLINE void osc3_chip(Osc3* o, uint8_t* rd, const uint8_t* regs, unsigned steps, int is6581, int real) {
  const uint8_t w1 = regs[4], w2 = regs[11], w3 = regs[18];
  /* OSC3 в том же режиме постоянен; шум и SounDemon-состояние не меняются. */
  if (RARELY((w1 & w2 & w3 & 0x08) && (w3 & 0xF0) == 0x40)) {
    if (o->held_test) return;
    o->held_test = 1;
    o->acc[0] = o->acc[1] = o->acc[2] = 0;
    o->prev[0] = o->prev[1] = o->prev[2] = 0;
    o->ring = o->rise = 0;
    o->out3 = 0xFFFF;
    rd[0x1B] = 0xFF;
    return;
  }
  o->held_test = 0;

  const uint32_t f1 = ((uint32_t)regs[1] << 8 | regs[0]) * CYCLES;
  const uint32_t f2 = ((uint32_t)regs[8] << 8 | regs[7]) * CYCLES;
  const uint32_t f3 = ((uint32_t)regs[15] << 8 | regs[14]) * CYCLES;
  const unsigned kind3 = w3 & 0xF0, test3 = (w3 & 0x08) != 0;
  const uint32_t ringmask3 = (w3 & 0x04) ? ACC_MSB : 0;
  uint32_t a1 = o->acc[0], a2 = o->acc[1], a3 = o->acc[2];
  uint32_t p1 = o->prev[0], p2 = o->prev[1], p3 = o->prev[2];
  uint32_t ring = o->ring, out = o->out3, t3 = o->lfsr3, m2 = 0;
  unsigned rise = o->rise;
  int digi = o->digi3;
/* Голос за шаг: тест или синхронизация — фаза 0, иначе + шаг по модулю 2^24; подъём и старший бит —
 * следующему голосу. */
#define OSC3_VOICE(a, p, w, f)                                     \
  if (RARELY(((w) & 0x08) || (((w) & 0x02) && rise))) (a) = 0;     \
  else (a) = ((a) + (f)) & (ACC_RANGE - 1);                        \
  rise = ((a) & ACC_MSB) > ((p) & ACC_MSB);                        \
  ring = (a) & ACC_MSB
  if(steps&&!((w1|w2|w3)&2)&&kind3<0x80&&!(kind3==0x10&&real&&digi>0)) {
    /* No synchronization: intermediate phases cannot affect another voice.
     * Keep the last rise, ring source, waveform and all state for transitions. */
    a1=(w1&8)?0:(a1+f1*steps)&(ACC_RANGE-1);
    a2=(w2&8)?0:(a2+f2*steps)&(ACC_RANGE-1);
    a3=test3?0:(a3+f3*steps)&(ACC_RANGE-1);
    const uint32_t before_last=test3?0:steps>1?(a3-f3)&(ACC_RANGE-1):p3;
    p1=a1;p2=a2;p3=a3;m2=a2&ACC_MSB;ring=a3&ACC_MSB;
    rise=ring>(before_last&ACC_MSB);
    if(kind3==0x10) {
      const uint32_t t=a3^(m2&ringmask3);
      out=((t^(t&ACC_MSB?ACC_RANGE-1:0))>>7)&0xFFFF;
    }
  } else for (unsigned s = 0; s < steps; ++s) {
    OSC3_VOICE(a1, p1, w1, f1);
    p1 = a1;
    OSC3_VOICE(a2, p2, w2, f2);
    p2 = a2;
    m2 = ring;                             /* кольцо голоса 3 — старший бит голоса 2 этого шага */
    const uint32_t old3 = p3;
    OSC3_VOICE(a3, p3, w3, f3);
    p3 = a3;
    if (kind3 == 0x80) {
      if (RARELY((a3 & NOISE_CLOCK) != (old3 & NOISE_CLOCK))) {
        const uint32_t fb = ((t3 & 0x400000) ^ ((t3 & 0x20000) << 5)) != 0;
        t3 = ((t3 << 1) | fb | test3) & 0x7FFFFF;
      }
    } else if (kind3 == 0x10) {
      if (MOSTLY(!real || digi <= 0)) {
        const uint32_t t = a3 ^ (m2 & ringmask3);
        out = ((t ^ (t & ACC_MSB ? ACC_RANGE - 1 : 0)) >> 7) & 0xFFFF;
      } else {
        digi -= CYCLES;
      }
    }
  }
#undef OSC3_VOICE
  switch (kind3) {
    case 0x80:
      out = ((t3 & 0x100000) >> 5) | ((t3 & 0x40000) >> 4) | ((t3 & 0x4000) >> 1) | ((t3 & 0x800) << 1) |
            ((t3 & 0x200) << 2) | ((t3 & 0x20) << 5) | ((t3 & 0x04) << 7) | ((t3 & 0x01) << 8);
      break;
    case 0x70:
    case 0x60: {
      const unsigned char* table = kind3 == 0x70 ? cRSID_PulseSawTriangle : cRSID_PulseSawtooth;
      const uint32_t pw12 = (uint32_t)((regs[17] & 0x0F) << 8 | regs[16]);
      out = (a3 >> 12) >= pw12 || test3 ? (uint32_t)table[(a3 >> 12) & (is6581 ? 0x7FF : 0xFFF)] << 8 : 0;
      break;
    }
    case 0x30: out = (uint32_t)cRSID_SawTriangle[(a3 >> 12) & (is6581 ? 0x7FF : 0xFFF)] << 8; break;
    case 0x50: {
      const uint32_t pw12 = (uint32_t)((regs[17] & 0x0F) << 8 | regs[16]);
      const uint32_t t = a3 ^ (m2 & ringmask3);
      out = (a3 >> 12) >= pw12 || test3 ? (uint32_t)cRSID_PulseTriangle[t >> 12] << 8 : 0;
      break;
    }
    case 0x40: {
      const uint32_t pw16 = (uint32_t)((regs[17] & 0x0F) << 8 | regs[16]) << 4;
      out = (a3 >> 8) >= pw16 || test3 ? 0xFFFF : 0;
      break;
    }
    case 0x20: out = a3 >> 8; break;
    case 0x10: break;                      /* треугольник — по шагам */
    case 0x00:
      if (real && w3 == 0x01) {            /* SounDemon: частота — ЦАП напрямую */
        out = (uint32_t)regs[15] << 8;
        digi = DIGI_HOLD;
      }
      break;
    default: out = 0; break;
  }
  o->acc[0] = a1, o->acc[1] = a2, o->acc[2] = a3;
  o->prev[0] = p1, o->prev[1] = p2, o->prev[2] = p3;
  o->ring = ring;
  o->rise = rise;
  o->out3 = out;
  o->lfsr3 = t3;
  o->digi3 = digi;
  rd[0x1B] = (uint8_t)(out >> 8);
}

static void osc3_sample(const Snap* snap) {
  Hot* const h = hot;
  const int quantum = CRSID_OVERSAMPLING_CYCLES << CRSID_CLOCK_FRACTIONAL_BITS;
  const int ratio = cRSID_C64.SampleClockRatio;
  const unsigned steps = h->osc_cnt <= ratio ? (unsigned)(ratio - h->osc_cnt) / quantum + 1 : 0;
  h->osc_cnt += (int)steps * quantum - ratio;
  const int real = cRSID_C64.RealSIDmode;
  for (unsigned n = 1; n <= h->chips; ++n) osc3_chip(&h->osc[n], h->rd[n], snap->chip[n - 1], steps, h->is6581[n], real);
}

/* Горячее состояние — с C64 после init подмелодии (cRSID_initC64). */
static void hot_reset(unsigned chips) {
  cia_cache_unbind();
  Hot* const h = hot;
  memset(h, 0, sizeof(*h));
  h->chips = (uint8_t)chips;
  h->cpu = cRSID_C64.CPU;
  memcpy(h->cia, cRSID_C64.CIA, sizeof(h->cia));
  h->vic = cRSID_C64.VIC;
  h->osc_cnt = cRSID_C64.OverSampleCycleCnt;
  for (unsigned n = 1; n <= chips; ++n) {
    const cRSID_SIDinstance* sid = &cRSID_C64.SID[n];
    h->regs[n] = sid->BasePtr;
    h->rd[n] = sid->BasePtrRD;
    h->last_env3[n] = sid->BasePtrRD[0x1C];
    h->is6581[n] = sid->ChipModel != 8580;
    Adsr* e = &h->adsr[n];
    Osc3* o = &h->osc[n];
    for (unsigned i = 0; i < 3; ++i) {
      e->state[i] = sid->ADSRstate[7 * i];
      e->rc[i] = sid->RateCounter[7 * i];
      e->env[i] = sid->EnvelopeCounter[7 * i];
      e->exp[i] = sid->ExponentCounter[7 * i];
      o->acc[i] = (uint32_t)sid->PhaseAccu[7 * i];
      o->prev[i] = (uint32_t)sid->PrevPhaseAccu[7 * i];
    }
    o->out3 = sid->PrevWavGenOut[14];
    o->lfsr3 = sid->NoiseLFSR[14];
    o->digi3 = sid->PrevSounDemonDigiWF[14];
    o->ring = sid->RingSourceMSB;
    o->rise = sid->SyncSourceMSBrise;
  }
}

/* --- длины и метки -------------------------------------------------------------------------- */

/* Строки заголовка — Windows-1252 (Latin-1), а шрифт экрана — латиница и кириллица CP866: буквы с
 * диакритикой — без неё («Hülsbeck» — «Hulsbeck»), прочее — «?». */
static void text_ascii(char* out, size_t size, const char* in, size_t length) {
  static const char upper[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPs";   /* 0xC0..0xDF */
  static const char lower[] = "aaaaaaaceeeeiiiidnooooo/ouuuuypy";   /* 0xE0..0xFF */
  size_t n = 0;
  for (size_t i = 0; i < length && in[i] != 0 && n + 1 < size; ++i) {
    const uint8_t c = (uint8_t)in[i];
    out[n++] = c < 0x80 ? (char)c : c >= 0xE0 ? lower[c - 0xE0] : c >= 0xC0 ? upper[c - 0xC0] : '?';
  }
  while (n > 0 && out[n - 1] == ' ') --n;
  out[n] = 0;
}

/* «1987 Ocean» — год и издатель; без года впереди — строка целиком. */
static void release_info(Source* s) {
  char released[33];
  text_ascii(released, sizeof(released), (const char*)s->header->ReleaseInfo, 32);
  const char* rest = released;
  if (released[0] >= '1' && released[0] <= '2' && released[1] >= '0' && released[1] <= '9' &&
      released[2] >= '0' && released[2] <= '9' && released[3] >= '0' && released[3] <= '9') {
    s->year = (uint16_t)((released[0] - '0') * 1000 + (released[1] - '0') * 100 +
                         (released[2] - '0') * 10 + (released[3] - '0'));
    rest = released + 4;
    while (*rest == ' ' || *rest == '-' || *rest == '?') ++rest;   /* «1987-88», «198?» */
  }
  snprintf(s->album, sizeof(s->album), "%s", rest);
}

/* Длины подмелодий из SIDLEN.BIN (tools/make_sidlen.py): «SIDL», версия 1, 4096 корзин; смещения
 * корзин — 4097 слов LE с BASE_TABLE; запись — байты 2..6 MD5, число длин (0 — 256) и длины в
 * секундах (слова LE). */
static void lengths_from_base(Source* s) {
  const uint8_t* base = NULL;
  size_t size = 0;
  if (host->resource == NULL || host->resource(BASE_NAME, &base, &size) != ZM_OK || base == NULL ||
      size < BASE_TABLE + 4 * (BASE_BUCKETS + 1) || memcmp(base, "SIDL", 4) != 0 || base[4] != 1) {
    return;
  }
  uint8_t md5[16];
  cRSID_MD5_calculateSum(s->file, (int)s->size, md5);
  const uint32_t bucket = (uint32_t)md5[0] << 4 | md5[1] >> 4;
  uint32_t at = le32(base + BASE_TABLE + 4 * bucket);
  const uint32_t end = le32(base + BASE_TABLE + 4 * (bucket + 1));
  if (end > size || at > end) return;
  while (at + 6 <= end) {
    const unsigned count = base[at + 5] != 0 ? base[at + 5] : 256;
    const uint32_t next = at + 6 + 2 * count;
    if (next > end) return;
    if (memcmp(base + at, md5 + 2, 5) == 0) {
      for (unsigned i = 0; i < count && i < SONGS_MAX; ++i) {
        s->seconds[i + 1] = (uint16_t)(base[at + 6 + 2 * i] | base[at + 7 + 2 * i] << 8);
      }
      s->known = 1;
      return;
    }
    at = next;
  }
}

/* --- конвейер ------------------------------------------------------------------------------- */

static void rest(void) {
  if (host->size >= offsetof(zm_host_t, rest) + sizeof(host->rest) && host->rest != NULL) host->rest();
}

/* Дождаться помощника: его кусок посчитан, копии SID ядро 0 может трогать. */
static void finish_job(Source* s) {
  if (s->pending && s->pipelined) host->worker_wait();
}

/* Ядро 0: следующий кусок C64 в jobs[slot]; 0 — подмелодия кончилась. */
static uint32_t emulate_chunk(Source* s, unsigned slot) {
  uint32_t n = s->end > s->emulated ? s->end - s->emulated : 0;
  if (n > CHUNK) n = CHUNK;
  Job* job = &s->jobs[slot];
  const uint32_t started = ccount();
  for (uint32_t i = 0; i < n; ++i) c64_sample(SNAP_AT(job, i));
  cycles_c64 += ccount() - started;
  frames_c64 += n;
  job->n = n;
  s->emulated += n;
  return n;
}

/* Звуковая часть куска slot — помощнику на ядре 1 (занят или нет его — здесь же). */
static void start_job(Source* s, unsigned slot) {
  Job* job = &s->jobs[slot];
  if (!s->pipelined || host->worker_start(synth_job, job) != ZM_OK) {
    synth_job(job);
  }
}

/* Подмелодия с начала. Команда Evo не исполняет программу init: её продолжает
 * render на ядре 0 короткими порциями, освобождая замок PLAYER после каждой. */
static void start_song(Source* s, uint16_t song) {
  cia_cache_unbind();
  finish_job(s);
  s->pending = 0;
  s->ready_pos = s->ready_n = 0;
  adsr_pending = 0;
  s->song = song;
  const uint32_t started = ccount();
  cRSID.SIDfileData = (char*)s->file;
  cRSID.SIDfileSize = (int)s->size;
  cRSID_processSIDfileData(s->file, (int)s->size);
  /* cRSID сохраняет фазу строки VIC между подмелодиями; вернуть горячий счётчик. */
  if (hot->chips && !s->initializing) cRSID_C64.VIC.RowCycleCnt = hot->vic.RowCycleCnt;
  fast_init_begin(s->header, (char)song);
  s->chips = cRSID_C64.SIDchipCount;
  s->initializing = 1;
  s->init_left = cRSID.RealSIDmode ? 0 : 10000000u;
  s->init_steps = 0;
  s->init_cycles = s->init_max_slice = ccount() - started;
  const uint32_t seconds = s->seconds[song] != 0 ? s->seconds[song] : DEFAULT_SECONDS;
  s->frame = s->emulated = 0;
  s->end = seconds * RATE;
}

/* Лимит в 10 миллионов команд — исходный предел cRSID. Паузы не меняют такты
 * эмулируемого C64. До окончания init PCM не генерируется и время песни стоит. */
static int init_slice(Source* s) {
  const uint32_t started = ccount();
  for (unsigned n = 0; s->init_left && n < INIT_STEPS; ++n) {
    --s->init_left;
    ++s->init_steps;
    if (fast_cpu(&cRSID_C64.CPU) >= 0xFE) { s->init_left = 0; break; }
    if ((n & 63u) == 63u && ccount() - started >= SLICE_CYCLES) break;
  }
  if (s->init_left == 0) {
    /* begin уже нормализовал номер по правилам cRSID. */
    fast_init_end(s->header, (char)cRSID.SubTune);
    synth_reset(s->chips);
    hot_reset(s->chips);
    s->initializing = 0;
  }
  const uint32_t elapsed = ccount() - started;
  s->init_cycles += elapsed;
  if (elapsed > s->init_max_slice) s->init_max_slice = elapsed;
  return !s->initializing;
}

/* Догон без звуковой части. Один снимок нужен OSC3; помощник не запускается.
 * Лимит отсчётов ограничивает порцию и на стенде без настоящего ccount. */
static int seek_slice(Source* s) {
  const uint32_t started = ccount();
  Snap snap;
  unsigned n = 0;
  while (s->emulated < s->frame && n < 4096u) {
    c64_sample(&snap);
    ++s->emulated;
    ++n;
    if ((n & 63u) == 0 && ccount() - started >= SLICE_CYCLES) break;
  }
  const uint32_t elapsed = ccount() - started;
  frames_c64 += n;
  cycles_c64 += elapsed;
  s->seek_frames += n;
  s->seek_cycles += elapsed;
  ++s->seek_slices;
  if (elapsed > s->seek_max_slice) s->seek_max_slice = elapsed;
  if (s->emulated < s->frame) return 0;
  /* После пропущенного звука старые фазы и фильтры больше не нужны.
   * Состояние C64, ADSR и OSC3 при этом остаётся в точке назначения. */
  synth_reset(s->chips);
  s->seeking = 0;
  return 1;
}

static int probe(const uint8_t* head, size_t length) {
  return head != NULL && length >= 0x76 && (memcmp(head, "PSID", 4) == 0 || memcmp(head, "RSID", 4) == 0)
             ? 100
             : 0;
}

static void close_source(void* instance) {
  Source* s = (Source*)instance;
  if (s == NULL) return;
  finish_job(s);
  cia_cache_unbind();
  if (s->level_frames) {
    host->log("sid: level %d tenths LUFS, gain %d tenths dB, %u bytes, %u cycles/frame, max %u us",
              (int)(s->level.song_lufs * 10), (int)(s->level.gain_db * 10), (unsigned)sizeof(s->level),
              (unsigned)(s->level_cycles / s->level_frames),
              (unsigned)(s->level_max / (CPU_HZ / 1000000u)));
  }
  if (frames_c64 != 0 && frames_sound != 0) {
    host->log("sid: cycles per sample of %u: core 0 C64 %u, core %u sound %u", (unsigned)(240000000u / RATE),
              (unsigned)(cycles_c64 / frames_c64), s->pipelined ? 1u : 0u, (unsigned)(cycles_sound / frames_sound));
    if (SID_DETAIL_CYCLES) host->log("sid: core 0 per sample: cpu %u, cia+vic %u, adsr %u, snapshot+osc3 %u", (unsigned)(cycles_cpu / frames_c64),
              (unsigned)(cycles_ciavic / frames_c64), (unsigned)(cycles_adsr / frames_c64),
              (unsigned)(cycles_snap / frames_c64));
  }
  host->log("sid: song %u init %u instructions, %u us active, max slice %u us%s",
            (unsigned)s->song, (unsigned)s->init_steps, (unsigned)(s->init_cycles / (CPU_HZ / 1000000u)),
            (unsigned)(s->init_max_slice / (CPU_HZ / 1000000u)), s->initializing ? " pending" : "");
  if (s->seek_slices) host->log("sid: seek %u frames, %u slices, %u us in slices, max slice %u us",
      (unsigned)s->seek_frames, (unsigned)s->seek_slices,
      (unsigned)(s->seek_cycles / (CPU_HZ / 1000000u)),
      (unsigned)(s->seek_max_slice / (CPU_HZ / 1000000u)));
  cycles_c64 = cycles_sound = frames_c64 = frames_sound = 0;
  cycles_cpu = cycles_ciavic = cycles_adsr = cycles_snap = 0;
  cRSID.Paused = 1;
  for (unsigned i = 0; i < 2; ++i) {
    if (s->jobs[i].snap != NULL) host->free(s->jobs[i].snap);
    if (s->jobs[i].pcm != NULL) host->free(s->jobs[i].pcm);
  }
  if (s->file != NULL) host->free(s->file);
  if (hot != NULL) host->free(hot);
  hot = NULL;
  if(synth_params!=params_fallback) {
    host->free(synth_params);synth_params=params_fallback;
  }
  if (synth_state != &synth_fallback) {
    synth_fallback = *synth_state;
    host->free(synth_state);
    synth_state = &synth_fallback;
  }
  host->free(s);
  busy = 0;
}

static void* open_source(uint32_t file_size, const uint8_t* file) {
  if (busy || file == NULL || probe(file, file_size) == 0 || file_size > 0x10000 + 0x7C + 2) {
    return NULL;
  }
  Source* s = (Source*)host->alloc(sizeof(Source), ZM_MEM_INTERNAL);
  if (s == NULL) s = (Source*)host->alloc(sizeof(Source), ZM_MEM_PSRAM);
  if (s == NULL) return NULL;
  memset(s, 0, sizeof(*s));
  busy = 1;
  hot = (Hot*)host->alloc(sizeof(Hot), ZM_MEM_INTERNAL);
  if (hot == NULL) hot = (Hot*)host->alloc(sizeof(Hot), ZM_MEM_PSRAM);
  if (hot != NULL) memset(hot, 0, sizeof(*hot));
  s->file = (uint8_t*)host->alloc(file_size, ZM_MEM_PSRAM);
  if (hot == NULL || s->file == NULL) {
    close_source(s);
    return NULL;
  }
  memcpy(s->file, file, file_size);
  s->size = file_size;
  s->pipelined = host->size >= offsetof(zm_host_t, worker_wait) + sizeof(host->worker_wait) &&
                 host->worker_start != NULL && host->worker_wait != NULL;
  cRSID_init(RATE, 0);
  cRSID.Stereo = CRSID_CHANNELMODE_NARROW;   /* 2SID и больше — стерео, узкое (как cRSID 1.58) */
  s->header = cRSID_processSIDfileData(s->file, (int)s->size);
  if (s->header == NULL) {
    close_source(s);
    return NULL;
  }
  /* Номера — 16 бит (до 256), а cRSID берёт подмелодию байтом со знаком: больше 255 — не дальше. */
  s->songs = (uint16_t)(s->header->SubtuneAmountH00 << 8 | s->header->SubtuneAmount);
  if (s->songs == 0) s->songs = 1;
  if (s->songs > 255) s->songs = 255;
  uint16_t song = (uint16_t)(s->header->DefaultSubtuneH00 << 8 | s->header->DefaultSubtune);
  if (song == 0 || song > s->songs) song = 1;
  text_ascii(s->title, sizeof(s->title), (const char*)s->header->Title, 32);
  text_ascii(s->artist, sizeof(s->artist), (const char*)s->header->Author, 32);
  release_info(s);
  lengths_from_base(s);
  if (SID_LEVEL_ENABLED) zm_level_init(&s->level, RATE, 0.0f, -18.0f);
  start_song(s, song);
  /* Куски — когда число SID известно (cRSID_startSubtune): снимок — на столько SID, сколько их
   * есть. Снимки и звук — во внутренней памяти (1.3): у одной SID 2 × 64 × 30 байтов. У 2SID и
   * больше снимки — в PSRAM: внутренней во время игры ~19 КБ, и она нужна Wi-Fi. Нет внутренней —
   * хост даёт PSRAM. */
  const uint32_t stride = (uint32_t)(offsetof(Snap, chip) + SNAP_CHIP * s->chips + 3) & ~3u;
  for (unsigned i = 0; i < 2; ++i) {
    s->jobs[i].stride = stride;
    s->jobs[i].snap = (Snap*)host->alloc(CHUNK * stride, s->chips == 1 ? ZM_MEM_INTERNAL : ZM_MEM_PSRAM);
    s->jobs[i].pcm = (int16_t*)host->alloc(CHUNK * 2 * sizeof(int16_t), ZM_MEM_INTERNAL);
  }
  if (s->jobs[0].snap == NULL || s->jobs[0].pcm == NULL || s->jobs[1].snap == NULL || s->jobs[1].pcm == NULL) {
    close_source(s);
    return NULL;
  }
  /* Allocate after required buffers so this optimization cannot prevent open. */
  Synth* fast_synth = (Synth*)host->alloc(sizeof(Synth), ZM_MEM_INTERNAL);
  if (fast_synth != NULL) {
    *fast_synth = synth_fallback;
    synth_state = fast_synth;
  }
  VoiceCache* fast_params=(VoiceCache*)host->alloc((s->chips+1)*sizeof(VoiceCache),ZM_MEM_INTERNAL);
  if(fast_params) {
    synth_params=fast_params;
    memset(synth_params,0,(s->chips+1)*sizeof(*synth_params));
  }
  host->log("sid: synthesis parameters %u bytes at %08X",(unsigned)((s->chips+1)*sizeof(VoiceCache)),
            (unsigned)(uintptr_t)synth_params);
  host->log("sid: synthesis state %u bytes at %08X", (unsigned)sizeof(Synth),
            (unsigned)(uintptr_t)synth_state);
  const unsigned model = cRSID_getSIDmodel(1);
  if (s->chips > 1) {
    snprintf(s->format, sizeof(s->format), "SID %ux%u %s", (unsigned)s->chips, model,
             cRSID.RealSIDmode ? "RSID" : "PSID");
  } else {
    snprintf(s->format, sizeof(s->format), "SID %u %s", model, cRSID.RealSIDmode ? "RSID" : "PSID");
  }
  host->log("sid: %s, %u songs from %u, %s, length %s, %s", s->format, (unsigned)s->songs, (unsigned)song,
            cRSID.VideoStandard ? "PAL" : "NTSC", s->known ? "from " BASE_NAME : "default 3 min",
            s->pipelined ? "2 cores" : "1 core");
  return s;
}

/* Следующий посчитанный кусок. С помощником: ядро 0 гонит C64 следующего куска, пока ядро 1 считает
 * звук прошлого; готов — прошлый. Первый раз конвейер заводится (два куска C64). 0 — конец. */
static uint32_t advance(Source* s) {
  if (!s->pending) {
    if (emulate_chunk(s, s->current) == 0) return 0;
    start_job(s, s->current);
    s->pending = 1;
  }
  const unsigned next = s->current ^ 1u;
  const uint32_t ahead = emulate_chunk(s, next);   /* ядро 0 — пока ядро 1 считает current */
  finish_job(s);
  s->ready = s->current;
  if (ahead != 0) {
    start_job(s, next);
    s->current = next;
  } else {
    s->pending = 0;
  }
  s->ready_pos = 0;
  s->ready_n = s->jobs[s->ready].n;
  return s->ready_n;
}

static int render(void* instance, float* out, uint32_t max_frames, uint32_t* frames) {
  Source* s = (Source*)instance;
  *frames = 0;
  rest();
  if (s->seek_prepare) {
    finish_job(s);
    s->pending = 0;
    s->ready_pos = s->ready_n = 0;
    const uint32_t target = s->frame;
    if (target < s->end && target < s->emulated) {
      start_song(s, s->song);
      s->frame = target;
    }
    s->seek_prepare = 0;
  }
  /* Переход за конец не требует исполнять ни init, ни весь остаток C64. */
  if (s->frame >= s->end) { s->seeking = 0; return ZM_RENDER_END; }
  if (s->initializing && !init_slice(s)) return ZM_RENDER_NEED_INPUT;
  if (s->seeking) {
    /* Даже завершив догон, вернуть управление PLAYER до первого звукового куска. */
    seek_slice(s);
    return ZM_RENDER_NEED_INPUT;
  }
  const uint32_t started = ccount();
  if (max_frames > RENDER_MAX) max_frames = RENDER_MAX;
  const uint32_t fade_from = s->end > 4 * RATE ? s->end - FADE_FRAMES : s->end;
  uint32_t got = 0;
  while (got < max_frames) {
    if (s->ready_pos >= s->ready_n && advance(s) == 0) break;
    const Job* done = &s->jobs[s->ready];
    uint32_t count = s->ready_n - s->ready_pos;
    if (count > max_frames - got) count = max_frames - got;
    const int16_t* pcm = done->pcm + 2 * s->ready_pos;
    float* o = out + 2 * got;
    for (uint32_t i = 0; i < count; ++i) {
      o[2 * i] = (float)pcm[2 * i] * (1.0f / 32768.0f);
      o[2 * i + 1] = (float)pcm[2 * i + 1] * (1.0f / 32768.0f);
    }
    s->ready_pos += count;
    s->frame += count;
    got += count;
    if (ccount() - started >= SLICE_CYCLES) break;
  }
  if (got == 0) return ZM_RENDER_END;
  if (SID_LEVEL_ENABLED) {
    const uint32_t level_started = ccount();
    PROF_AT(level_at);
    zm_level_process(&s->level, out, got);
    PROF_ADD(PROF_LEVEL, level_at);
    const uint32_t elapsed = ccount() - level_started;
    s->level_cycles += elapsed;
    s->level_frames += got;
    if (elapsed > s->level_max) s->level_max = elapsed;
  }
  /* Затухание после замера: нормализатор не должен его компенсировать. */
  for (uint32_t i = 0; i < got; ++i) {
    const uint32_t frame = s->frame - got + i;
    if (frame >= fade_from) {
      const float fade = (float)(s->end - frame) / (float)FADE_FRAMES;
      out[2 * i] *= fade;
      out[2 * i + 1] *= fade;
    }
  }
  *frames = got;
  return ZM_RENDER_OK;
}

/* Относительная перемотка в миллисекундах. Команда только меняет цель:
 * всё ожидание помощника, повторный init и догон происходят в render. */
static int seek(void* instance, int32_t delta, uint32_t* file_offset) {
  Source* s = (Source*)instance;
  int64_t target = (int64_t)s->frame + (int64_t)delta * RATE / 1000;
  if (target < 0) target = 0;
  if (target > s->end) target = s->end;
  s->frame = (uint32_t)target;
  s->seeking = s->seek_prepare = 1;
  if (SID_LEVEL_ENABLED) zm_level_discontinuity(&s->level);
  *file_offset = UINT32_MAX;
  return ZM_OK;
}

static void info(const void* instance, zm_info_t* out) {
  const Source* s = (const Source*)instance;
  memset(out, 0, sizeof(*out));
  snprintf(out->format, sizeof(out->format), "%s", s->format);
  out->rate = RATE;
  out->channels = 2;
  out->duration_ms = (uint32_t)((uint64_t)s->end * 1000u / RATE);
  out->position_ms = (uint32_t)((uint64_t)s->frame * 1000u / RATE);
  snprintf(out->title, sizeof(out->title), "%s", s->title);
  snprintf(out->artist, sizeof(out->artist), "%s", s->artist);
  snprintf(out->album, sizeof(out->album), "%s", s->album);
  out->year = s->year;
  out->voices = (uint8_t)(3 * s->chips);
}

/* Уровни голосов — огибающие (0..255): три голоса на микросхему; регистры голоса — 0, 7, 14. */
static int levels(const void* instance, uint8_t* out, int max) {
  const Source* s = (const Source*)instance;
  int n = 0;
  for (unsigned chip = 1; chip <= s->chips && chip <= CRSID_SIDCOUNT_MAX; ++chip) {
    for (unsigned voice = 0; voice < 3 && n < max; ++voice) {
      out[n++] = hot != NULL ? hot->adsr[chip].env[voice] : 0;
    }
  }
  return n;
}

int main(int argc, char* argv[]) {
  if (argc < 3 || argv[1] == NULL || argv[2] == NULL) return 0;
  host = (const zm_host_t*)argv[1];
  if (host->abi_major != ZM_SOURCE_ABI_MAJOR ||
      host->size < offsetof(zm_host_t, free) + sizeof(host->free) || host->alloc == NULL ||
      host->free == NULL) {
    return 0;
  }
  memset(&table, 0, sizeof(table));
  table.size = sizeof(table);
  table.abi_major = ZM_SOURCE_ABI_MAJOR;
  table.abi_minor = ZM_SOURCE_ABI_MINOR;
  table.name = "sid";
  table.version = "1.9-opt10";
  table.extensions = "SID";
  table.input = ZM_INPUT_WHOLE;
  table.keeps_file = 0;
  table.seek_unit = ZM_SEEK_MS;
  table.resources = BASE_NAME;
  table.probe = probe;
  table.open = open_source;
  table.render = render;
  table.seek = seek;
  table.info = info;
  table.levels = levels;
  table.close = close_source;
  *(const zm_source_t**)argv[2] = &table;
  return ZM_SOURCE_ABI_MAJOR;
}
