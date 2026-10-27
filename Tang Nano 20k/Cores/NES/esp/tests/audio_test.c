/* SPDX-License-Identifier: Apache-2.0 */
/* Звук NES.ELF 0.8 (esp/main/audio_level.h) на отсчётах, какие кладёт в кольцо ядро NES: {sample, sample},
 * sample = 65535 − выход APU (upstream/src/nes.v: sample_inverted; микшер APUMixer — 0…F76F).
 * Проверяется:
 *  - линейная развёртка уровня через середину шкалы (8000) — без скачков; прежнее чтение int16 даёт здесь
 *    скачок почти на всю шкалу (так звучали щелчки);
 *  - громкий прямоугольник (два квадрата и треугольник с шумом на максимуме, A51B) — без постоянной
 *    составляющей, в пределах ±1;
 *  - самый большой размах (0890 ↔ FFFF) — мягкий предел, не выше 1;
 *  - пауза ядра (отсчёты 0) после звука — без щелчка, затухание к нулю;
 *  - начало сеанса (нули, затем тишина FFFF) — ноль на выходе.
 * Ожидается: «PASS». */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_level.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define RATE 48000u

static uint32_t word_of(uint16_t apu) { const uint16_t s = (uint16_t)(0xFFFFu - apu); return (uint32_t)s << 16 | s; }
static float absf(float v) { return v < 0 ? -v : v; }

/* n отсчётов через преобразование; итог — в out (левый канал), обе половины равны */
static void run(nes_audio_t *a, const uint32_t *w, uint32_t n, float *out)
{
    static float frames[2 * 16];
    for (uint32_t at = 0; at < n; at += 16) {
        const uint32_t k = n - at > 16 ? 16 : n - at;
        nes_audio_convert(a, w + at, k, frames);
        for (uint32_t i = 0; i < k; i++) {
            CHECK(frames[2 * i] == frames[2 * i + 1]);
            out[at + i] = frames[2 * i];
        }
    }
}

int main(void)
{
    static uint32_t w[RATE];
    static float out[RATE];
    nes_audio_t a;

    /* 1. развёртка 0 → F000 → 0 за 20 мс (треугольник 50 Гц): шаг уровня ≈ 0,0026 */
    const uint32_t ramp = RATE / 50;
    for (uint32_t i = 0; i < RATE / 2; i++) {
        const uint32_t p = i % ramp, half = ramp / 2;
        const uint32_t apu = (p < half ? p : ramp - p) * 0xF000u / half;
        w[i] = word_of((uint16_t)apu);
    }
    nes_audio_reset(&a, RATE);
    run(&a, w, RATE / 2, out);
    float jump = 0, peak = 0, old_jump = 0;
    for (uint32_t i = 1; i < RATE / 2; i++) {
        const float d = absf(out[i] - out[i - 1]);
        if (d > jump) jump = d;
        if (absf(out[i]) > peak) peak = absf(out[i]);
        const float o1 = (float)(int16_t)(w[i] >> 16) / 32768.0f, o0 = (float)(int16_t)(w[i - 1] >> 16) / 32768.0f;
        if (absf(o1 - o0) > old_jump) old_jump = absf(o1 - o0);
    }
    printf("развёртка: наибольший шаг %.4f (прежнее чтение int16 — %.3f), пик %.3f\n", jump, old_jump, peak);
    CHECK(old_jump > 1.5f);                     /* прежняя ошибка: перескок через 8000 */
    CHECK(jump < 0.01f && peak <= 1.0f);

    /* 2. громкий прямоугольник 440 Гц: 0 ↔ A51B — без постоянной, в пределах ±1 */
    const uint32_t period = RATE / 440;
    for (uint32_t i = 0; i < RATE; i++) w[i] = word_of((i % period) < period / 2 ? 0xA51Bu : 0);
    nes_audio_reset(&a, RATE);
    run(&a, w, RATE, out);
    double sum = 0;
    float lo = 0, hi = 0;
    for (uint32_t i = RATE / 2; i < RATE; i++) {
        sum += out[i];
        if (out[i] < lo) lo = out[i];
        if (out[i] > hi) hi = out[i];
    }
    const double mean = sum / (RATE / 2);
    printf("прямоугольник A51B: среднее %.4f, от %.3f до %.3f\n", mean, lo, hi);
    CHECK(mean > -0.01 && mean < 0.01);
    CHECK(hi > 0.4f && lo < -0.4f && hi <= 1.0f && lo >= -1.0f);

    /* 3. наибольший размах 0890…FFFF (выход APU F76F ↔ 0): предел, не выше 1 */
    for (uint32_t i = 0; i < RATE / 4; i++) w[i] = (i % period) < period / 2 ? 0x08900890u : 0xFFFFFFFFu;
    nes_audio_reset(&a, RATE);
    run(&a, w, RATE / 4, out);
    peak = 0;
    for (uint32_t i = 0; i < RATE / 4; i++) if (absf(out[i]) > peak) peak = absf(out[i]);
    printf("наибольший размах: пик %.3f\n", peak);
    CHECK(peak <= 1.0f && peak > 0.9f);

    /* 4. пауза (ядро пишет 0) после звука: без щелчка, к нулю */
    for (uint32_t i = 0; i < RATE / 4; i++) w[i] = word_of((uint16_t)((i % period) < period / 2 ? 0x4000u : 0x3000u));
    for (uint32_t i = RATE / 4; i < RATE / 2; i++) w[i] = 0;
    nes_audio_reset(&a, RATE);
    run(&a, w, RATE / 2, out);
    float step = 0;
    for (uint32_t i = RATE / 4; i < RATE / 2; i++) {
        const float d = absf(out[i] - out[i - 1]);
        if (d > step) step = d;
    }
    const float tail = absf(out[RATE / 2 - 1]);
    printf("пауза: наибольший шаг %.4f, через 250 мс %.6f\n", step, tail);
    CHECK(step < 0.15f && tail < 0.001f);       /* не больше размаха самого звука (≈0,1) */

    /* 5. начало сеанса: нули (загрузка), затем тишина FFFF — на выходе ноль */
    for (uint32_t i = 0; i < 2000; i++) w[i] = i < 1000 ? 0 : 0xFFFFFFFFu;
    nes_audio_reset(&a, RATE);
    run(&a, w, 2000, out);
    peak = 0;
    for (uint32_t i = 0; i < 2000; i++) if (absf(out[i]) > peak) peak = absf(out[i]);
    CHECK(peak == 0.0f);

    printf("PASS %u checks: unsigned samples, no wrap at 8000, DC removed, soft limit, pause and start without clicks\n", checks);
    return 0;
}
