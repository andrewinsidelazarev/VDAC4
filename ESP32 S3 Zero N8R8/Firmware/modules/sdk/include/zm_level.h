/* Нормализация стерео PCM: общий алгоритм TRACKER.ELF и OPL.ELF.
 * Фильтр K влияет только на замер. 400 мс / 100 мс, абсолютный и относительный
 * пороги, накопительная громкость песни; цель -12 LUFS, плавность 0,5 дБ/с.
 * Начальное усиление и нижний предел задаёт декодер; верхний предел +18 дБ.
 * Пики выше 0,75 мягко ограничиваются. Состояние отдельно для каждой песни.
 * Без ожидания, предварительного прохода и динамических выделений памяти.
 */
#ifndef ZM_LEVEL_H
#define ZM_LEVEL_H
#include <math.h>
#include <stdint.h>
#include <string.h>

#define ZM_LEVEL_KNEE 0.75f               /* выше — мягкое сжатие к 1,0 (как в MIDI.ELF) */
#define ZM_LEVEL_BASE_DB 9.0f       /* усиление до замера (и всё усиление в 1.6) */
#define ZM_LEVEL_TARGET (-12.0f)    /* к чему подводится песня, LUFS (MIDI — −11,7) */
#define ZM_LEVEL_MIN_DB (ZM_LEVEL_BASE_DB - 6.0f)
#define ZM_LEVEL_MAX_DB (ZM_LEVEL_BASE_DB + 9.0f)
#define ZM_LEVEL_SLEW_DB 0.05f      /* за шаг 100 мс: 0,5 дБ/с — без рывков */
#define ZM_LEVEL_WARMUP_STEPS 50    /* блоков звука выше −70 LUFS до первой подстройки (5 с) */
#define ZM_LEVEL_BINS 320           /* блоки по громкости: −70…+10 LUFS шагом 0,25 */

/* Двухполюсный фильтр (прямая форма II, транспонированная), на два канала. */
typedef struct {
  float b0, b1, b2, a1, a2;
  float z1[2], z2[2];
} zm_level_biquad_t;

/* Замер и усиление (level_*). */
typedef struct {
  int ready;
  zm_level_biquad_t shelf, highpass;              /* фильтр K: полка +4 дБ и срез 38 Гц */
  float step_sum;                        /* сумма квадратов шага (оба канала): float — у ESP
                                         * double программный, а здесь сумма за каждый отсчёт */
  uint32_t step_count, step_len;         /* кадров в шаге, шаг — 100 мс */
  double steps[4];                       /* средние квадраты последних четырёх шагов */
  uint32_t step_number;
  uint32_t histogram[ZM_LEVEL_BINS];        /* блоки 400 мс выше −70 LUFS по громкости */
  uint32_t blocks;
  float song_lufs;                       /* громкость песни до усиления (0 — ещё нет) */
  float min_db;                          /* нижний предел для декодера */
  float gain_db;                         /* усиление сейчас */
  float gain;                            /* то же, линейное, — на конце прошлого куска */
} zm_level_t;

/* Энергия середины каждой корзины гистограммы: громкость −70 + 0,25·(i + 0,5) LUFS. */
static float zm_level_bin_energy[ZM_LEVEL_BINS];

/* Коэффициенты фильтра K для частоты rate (формулы libebur128). */
static void zm_level_init(zm_level_t* lv, uint32_t rate, float base_db, float min_db) {
  memset(lv, 0, sizeof(*lv));
  lv->min_db = min_db;
  const double fs = (double)rate;
  double f0 = 1681.974450955533, gain = 3.999843853973347, q = 0.7071752369554196;
  double k = tan(3.14159265358979323846 * f0 / fs);
  const double vh = pow(10.0, gain / 20.0);
  const double vb = pow(vh, 0.4996667741545416);
  double a0 = 1.0 + k / q + k * k;
  lv->shelf.b0 = (float)((vh + vb * k / q + k * k) / a0);
  lv->shelf.b1 = (float)(2.0 * (k * k - vh) / a0);
  lv->shelf.b2 = (float)((vh - vb * k / q + k * k) / a0);
  lv->shelf.a1 = (float)(2.0 * (k * k - 1.0) / a0);
  lv->shelf.a2 = (float)((1.0 - k / q + k * k) / a0);
  f0 = 38.13547087602444;
  q = 0.5003270373238773;
  k = tan(3.14159265358979323846 * f0 / fs);
  a0 = 1.0 + k / q + k * k;
  lv->highpass.b0 = 1.0f;
  lv->highpass.b1 = -2.0f;
  lv->highpass.b2 = 1.0f;
  lv->highpass.a1 = (float)(2.0 * (k * k - 1.0) / a0);
  lv->highpass.a2 = (float)((1.0 - k / q + k * k) / a0);
  lv->step_len = rate / 10;
  lv->gain_db = base_db;
  lv->gain = powf(10.0f, base_db / 20.0f);
  if (zm_level_bin_energy[0] == 0.0f) {
    for (int i = 0; i < ZM_LEVEL_BINS; ++i) {
      zm_level_bin_energy[i] = powf(10.0f, (-70.0f + 0.25f * ((float)i + 0.5f) + 0.691f) / 10.0f);
    }
  }
  lv->ready = 1;
}

static float zm_level_biquad(zm_level_biquad_t* f, int channel, float x) {
  const float y = f->b0 * x + f->z1[channel];
  f->z1[channel] = f->b1 * x - f->a1 * y + f->z2[channel];
  f->z2[channel] = f->b2 * x - f->a2 * y;
  return y;
}

/* Громкость песни по гистограмме блоков: средняя энергия блоков выше −70 LUFS,
 * затем — тех, что выше её на −10 LU (EBU R128). 0 — блоков ещё нет. */
static float zm_level_song(const zm_level_t* lv) {
  double total = 0.0;
  uint32_t count = 0;
  for (int i = 0; i < ZM_LEVEL_BINS; ++i) {
    total += (double)lv->histogram[i] * zm_level_bin_energy[i];
    count += lv->histogram[i];
  }
  if (count == 0) {
    return 0.0f;
  }
  const float gate = -0.691f + 10.0f * log10f((float)(total / count)) - 10.0f;
  int first = (int)ceilf((gate + 70.0f) * 4.0f - 0.5f);
  if (first < 0) {
    first = 0;
  }
  total = 0.0;
  count = 0;
  for (int i = first; i < ZM_LEVEL_BINS; ++i) {
    total += (double)lv->histogram[i] * zm_level_bin_energy[i];
    count += lv->histogram[i];
  }
  return count != 0 ? -0.691f + 10.0f * log10f((float)(total / count)) : 0.0f;
}

/* Конец шага 100 мс: блок 400 мс (четыре шага) в гистограмму, громкость песни,
 * шаг усиления к цели. */
static void zm_level_step(zm_level_t* lv) {
  lv->steps[lv->step_number & 3] = (double)lv->step_sum / (double)lv->step_count;
  lv->step_sum = 0.0f;
  lv->step_count = 0;
  if (++lv->step_number < 4) {
    return;
  }
  const double energy = (lv->steps[0] + lv->steps[1] + lv->steps[2] + lv->steps[3]) / 4.0;
  if (energy <= 0.0) {
    return;
  }
  const float block = -0.691f + 10.0f * log10f((float)energy);
  if (block <= -70.0f) {
    return;                              /* тишина: в замер не идёт */
  }
  int bin = (int)((block + 70.0f) * 4.0f);
  if (bin >= ZM_LEVEL_BINS) {
    bin = ZM_LEVEL_BINS - 1;
  }
  lv->histogram[bin]++;
  if (++lv->blocks < ZM_LEVEL_WARMUP_STEPS) {
    return;
  }
  lv->song_lufs = zm_level_song(lv);
  float target = ZM_LEVEL_TARGET - lv->song_lufs;
  if (target < lv->min_db) {
    target = lv->min_db;
  }
  if (target > ZM_LEVEL_MAX_DB) {
    target = ZM_LEVEL_MAX_DB;
  }
  float step = target - lv->gain_db;
  if (step > ZM_LEVEL_SLEW_DB) {
    step = ZM_LEVEL_SLEW_DB;
  }
  if (step < -ZM_LEVEL_SLEW_DB) {
    step = -ZM_LEVEL_SLEW_DB;
  }
  lv->gain_db += step;
}

/* Кусок звука: замер (до усиления), затем усиление — плавно от прежнего к
 * новому за кусок — и мягкое ограничение: отсчёты выше ZM_LEVEL_KNEE — к 1,0 по x/(1+x)
 * (у колена наклон 1, дальше — плавно), как soft_limit в MIDI.ELF. */
static void zm_level_process(zm_level_t* lv, float* samples, uint32_t frames) {
  for (uint32_t i = 0; i < frames; ++i) {
    const float l = zm_level_biquad(&lv->highpass, 0, zm_level_biquad(&lv->shelf, 0, samples[2 * i]));
    const float r = zm_level_biquad(&lv->highpass, 1, zm_level_biquad(&lv->shelf, 1, samples[2 * i + 1]));
    lv->step_sum += l * l + r * r;
    if (++lv->step_count == lv->step_len) {
      zm_level_step(lv);
    }
  }
  const float from = lv->gain;
  const float to = powf(10.0f, lv->gain_db / 20.0f);
  const float delta = frames != 0 ? (to - from) / (float)frames : 0.0f;
  float gain = from;
  for (uint32_t i = 0; i < frames * 2; i += 2) {
    gain += delta;
    for (int c = 0; c < 2; ++c) {
      const float value = samples[i + c] * gain;
      const float magnitude = value < 0.0f ? -value : value;
      if (magnitude > ZM_LEVEL_KNEE) {
        const float over = (magnitude - ZM_LEVEL_KNEE) * (1.0f / (1.0f - ZM_LEVEL_KNEE));
        const float limited = ZM_LEVEL_KNEE + (1.0f - ZM_LEVEL_KNEE) * over / (1.0f + over);
        samples[i + c] = value < 0.0f ? -limited : limited;
      } else {
        samples[i + c] = value;
      }
    }
  }
  lv->gain = to;
}

/* После перемотки накопленная громкость и усиление остаются, но фильтры и
 * неполный блок не должны смешивать звук из двух разных мест песни. */
static inline void zm_level_discontinuity(zm_level_t* lv) {
  memset(lv->shelf.z1, 0, sizeof(lv->shelf.z1));
  memset(lv->shelf.z2, 0, sizeof(lv->shelf.z2));
  memset(lv->highpass.z1, 0, sizeof(lv->highpass.z1));
  memset(lv->highpass.z2, 0, sizeof(lv->highpass.z2));
  memset(lv->steps, 0, sizeof(lv->steps));
  lv->step_sum = 0.0f;
  lv->step_count = 0;
  lv->step_number = 0;
}
#endif
