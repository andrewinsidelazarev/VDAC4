/* Спектр для окна проигрывателя (view.h). */
#include "view.h"

#include <math.h>
#include <string.h>

#define FFT_N 512
#define FFT_BITS 9
#define PERIOD_MS 50
/* 12,8 с: больше очереди PCM 256 КиБ даже при 8 кГц (8,2 с + DMA). */
#define HISTORY_SIZE 256

typedef struct {
  uint64_t frame;
  uint8_t bands[VIEW_BANDS];
} snapshot_t;

static const zm_core_t* core;
static float* tables;                  /* одним куском во внутренней памяти */
static float* re;
static float* im;
static float* window_curve;            /* Ханн */
static float* cos_table;               /* FFT_N/2 */
static float* sin_table;
static float* scope;                   /* последние FFT_N отсчётов, кольцом */
static uint32_t scope_pos;
static uint16_t edges[VIEW_BANDS + 1]; /* границы полос в номерах бинов */
static float thresholds[VIEW_LEVELS];  /* мощность, с которой начинается ступень */
static uint8_t bands[VIEW_BANDS];
static uint8_t displayed[VIEW_BANDS];
static snapshot_t* history;              /* 10 КиБ PSRAM, только готовые полосы */
static void* lock;                      /* короткий замок спектра, отдельно от декодера */
static uint32_t head, used;
static uint32_t sample_rate, period_frames, pending_frames;
static uint64_t accepted_frames, played_frames;

static void reset_locked(void) {
  memset(scope, 0, FFT_N * sizeof(float));
  memset(bands, 0, sizeof(bands));
  memset(displayed, 0, sizeof(displayed));
  scope_pos = head = used = sample_rate = period_frames = pending_frames = 0;
  accepted_frames = played_frames = 0;
}

static uint32_t reverse_bits(uint32_t value) {
  uint32_t result = 0;
  for (int bit = 0; bit < FFT_BITS; ++bit) {
    result = (result << 1) | (value & 1);
    value >>= 1;
  }
  return result;
}

int view_begin(const zm_core_t* c) {
  core = c;
  const size_t floats = FFT_N * 4 + FFT_N;           /* re, im, окно, scope + cos/sin */
  tables = (float*)core->alloc(floats * sizeof(float), ZM_MEM_INTERNAL);
  history = (snapshot_t*)core->alloc(HISTORY_SIZE * sizeof(snapshot_t), ZM_MEM_PSRAM);
  lock = core->mutex_create();
  if (tables == NULL || history == NULL || lock == NULL) {
    core->free(tables);
    core->free(history);
    tables = NULL;
    history = NULL;
    return ZM_NO_MEMORY;
  }
  re = tables;
  im = re + FFT_N;
  window_curve = im + FFT_N;
  scope = window_curve + FFT_N;
  cos_table = scope + FFT_N;
  sin_table = cos_table + FFT_N / 2;
  const float two_pi = 6.2831853f;
  for (int i = 0; i < FFT_N; ++i) {
    window_curve[i] = 0.5f - 0.5f * cosf(two_pi * (float)i / (float)FFT_N);
  }
  for (int i = 0; i < FFT_N / 2; ++i) {
    cos_table[i] = cosf(two_pi * (float)i / (float)FFT_N);
    sin_table[i] = sinf(two_pi * (float)i / (float)FFT_N);
  }
  /* Полосы: от бина 1 до FFT_N/2 по логарифму, не уже бина. */
  edges[0] = 1;
  for (int b = 1; b <= VIEW_BANDS; ++b) {
    uint16_t edge = (uint16_t)(powf((float)(FFT_N / 2), (float)b / VIEW_BANDS) + 0.5f);
    if (edge <= edges[b - 1]) {
      edge = (uint16_t)(edges[b - 1] + 1);
    }
    edges[b] = edge;
  }
  edges[VIEW_BANDS] = FFT_N / 2;
  /* Синус полной громкости с окном Ханна даёт в своём бине |X| = N/4;
   * верхняя ступень — на 6 дБ ниже, дальше по 4 дБ. */
  const float full = (float)(FFT_N / 4) * (float)(FFT_N / 4);
  for (int k = 0; k < VIEW_LEVELS; ++k) {
    thresholds[k] = full * powf(10.0f, -(0.6f + 0.4f * (float)(VIEW_LEVELS - 1 - k)));
  }
  view_reset();
  return ZM_OK;
}

void view_reset(void) {
  if (tables == NULL) {
    return;
  }
  core->mutex_lock(lock);
  reset_locked();
  core->mutex_unlock(lock);
}

static void feed(const float* frames, uint32_t count, uint8_t channels) {
  if (tables == NULL || frames == NULL || channels == 0) {
    return;
  }
  /* Нужны только последние FFT_N кадров. */
  uint32_t first = count > FFT_N ? count - FFT_N : 0;
  for (uint32_t i = first; i < count; ++i) {
    float sample = frames[i * channels];
    if (channels > 1) {
      sample = 0.5f * (sample + frames[i * channels + 1]);
    }
    scope[scope_pos] = sample;
    scope_pos = (scope_pos + 1) % FFT_N;
  }
}

static void update(void) {
  if (tables == NULL) {
    return;
  }
  for (uint32_t i = 0; i < FFT_N; ++i) {
    const uint32_t j = reverse_bits(i);
    re[j] = scope[(scope_pos + i) % FFT_N] * window_curve[i];
    im[j] = 0.0f;
  }
  for (uint32_t length = 2; length <= FFT_N; length <<= 1) {
    const uint32_t half = length >> 1;
    const uint32_t step = FFT_N / length;
    for (uint32_t i = 0; i < FFT_N; i += length) {
      for (uint32_t k = 0; k < half; ++k) {
        const float c = cos_table[k * step];
        const float s = sin_table[k * step];
        const uint32_t a = i + k;
        const uint32_t b = a + half;
        const float tr = re[b] * c + im[b] * s;
        const float ti = im[b] * c - re[b] * s;
        re[b] = re[a] - tr;
        im[b] = im[a] - ti;
        re[a] += tr;
        im[a] += ti;
      }
    }
  }
  for (int b = 0; b < VIEW_BANDS; ++b) {
    float power = 0.0f;
    for (uint32_t bin = edges[b]; bin < edges[b + 1]; ++bin) {
      const float p = re[bin] * re[bin] + im[bin] * im[bin];
      if (p > power) {
        power = p;
      }
    }
    uint8_t level = 0;
    while (level < VIEW_LEVELS && power >= thresholds[level]) {
      ++level;
    }
    bands[b] = level;
  }
}

void view_feed(const float* frames, uint32_t count, uint8_t channels, uint32_t rate) {
  if (tables == NULL || frames == NULL || channels == 0 || rate == 0) {
    return;
  }
  core->mutex_lock(lock);
  if (rate != sample_rate) {
    reset_locked();
    sample_rate = rate;
    period_frames = rate * PERIOD_MS / 1000;
    if (period_frames == 0) period_frames = 1;
  }
  /* Ритм — по принятым отсчётам: при быстром заполнении очереди нельзя
   * пропускать целые секунды спектра из-за ограничения по millis(). */
  while (count != 0) {
    uint32_t take = period_frames - pending_frames;
    if (take > count) take = count;
    feed(frames, take, channels);
    frames += take * channels;
    count -= take;
    accepted_frames += take;
    pending_frames += take;
    if (pending_frames == period_frames) {
      update();
      const uint32_t at = (head + used) % HISTORY_SIZE;
      history[at].frame = accepted_frames;
      memcpy(history[at].bands, bands, sizeof(bands));
      if (used == HISTORY_SIZE) {
        head = (head + 1) % HISTORY_SIZE;
      } else {
        ++used;
      }
      pending_frames = 0;
    }
  }
  core->mutex_unlock(lock);
}

void view_bands(uint8_t* out, int paused) {
  memset(out, 0, VIEW_BANDS);
  if (tables == NULL) return;
  core->mutex_lock(lock);
  if (!paused) {
    /* Ядро учитывает и кольцо PCM, и ещё не прозвучавший хвост DMA.
     * Пишущий поток публикует принятые кадры под этим же замком; если
     * pcm_write ещё идёт, оценка лишь немного отстаёт, но не опережает. */
    const uint64_t queued = (uint64_t)core->pcm_queued_ms() * sample_rate / 1000;
    const uint64_t played = accepted_frames > queued ? accepted_frames - queued : 0;
    if (played > played_frames) played_frames = played;
    while (used != 0 && history[head].frame <= played_frames) {
      memcpy(displayed, history[head].bands, sizeof(displayed));
      head = (head + 1) % HISTORY_SIZE;
      --used;
    }
  }
  memcpy(out, displayed, VIEW_BANDS);
  core->mutex_unlock(lock);
}
