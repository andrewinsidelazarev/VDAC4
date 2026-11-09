/* SPDX-License-Identifier: GPL-3.0-or-later
 * Звук Mega Drive (MD.ELF 1.7, 09.11.2026): пересчёт частоты YM -> 48 000 Гц, громкость и ограничитель.
 *
 * Почему (плата, Аладдин, запись пользователя: «хрип, а не звук», «тихий звук»). До 1.7 MD.ELF отдавал в I2S
 * сырые отсчёты YM на их собственной частоте — 53 098 Гц (PAL) или 53 336 Гц (NTSC), — без среза верхов и
 * тихо: канал YM на полной громкости — 0,17 шкалы, музыка игр на выходе около −30 дБFS по среднему.
 * Остальные приложения VDAC4 так не делают: TSFM (тот же ymfm) пересчитывает FM в 44,1 кГц фильтром со срезом
 * 19,8 кГц и выводит громкость к −12 LUFS с ограничителем, Amiga выводит 48 кГц. У ЦАП PCM5102 вывод SCK на
 * земле — тактовую он берёт из BCK своей PLL; стандартная частота снимает и этот вопрос.
 * Синтез YM сам даёт много выше 20 кГц: во вступлении Аладдина оператор канала 3 в особом режиме стоит на
 * ~70,8 кГц и отражается на 17,5 кГц, «лесенка» ЦАП YM2612 и ступени ЦАП (оцифровки) — тоже верха; на ПК у
 * этого места выше 16 кГц было на 9 дБ ниже всего сигнала.
 *
 * Пересчёт — свёртка с фильтром из md_resample_kernel.h (sinc с окном Блэкмана, 32 точки, срез 0,40 частоты
 * входа ≈ 21,2 кГц). Фаза точки выхода — 32 бита дроби; строка фильтра — линейно между двумя из 65 фаз. Шаг
 * (вход на выход) задаёт вызывающий: отношение частот с поправкой по очереди вывода — вместо прежнего «лишний
 * отсчёт выбросить / последний повторить» (разрыв волны раз в блок).
 *
 * Громкость: MD_RS_GAIN (x3, +9,5 дБ к прежнему уровню 0,9 — на ПК: музыка Jungle Book около −20 дБFS по
 * среднему, вступление Аладдина −16). Ограничитель как у TSFM: пик выше MD_RS_CEILING (−1 дБ) — множитель сразу
 * убавляется до нужного, затем за ~150 мс возвращается к 1. На музыке Jungle Book не срабатывает; на громком
 * вступлении Аладдина убавляет больше 1 дБ 3 % времени.
 *
 * Чистый C без библиотек (на ПК — тот же код в tests/snd_resample.cpp).
 */
#ifndef MD_RESAMPLE_H
#define MD_RESAMPLE_H
#include <stdint.h>
#include <string.h>
#include "md_resample_kernel.h"

#define MD_RS_OUT_RATE 48000u
#define MD_RS_RING     64u                  /* входные отсчёты: окно 32 точки + запас; степень двойки */
#define MD_RS_GAIN     3.0f
#define MD_RS_CEILING  0.891f               /* −1 дБ */
#define MD_RS_RELEASE  1.3888e-4f           /* 1 − exp(−1 / (0,15 с × 48 000)) */

typedef struct {
    float l[MD_RS_RING], r[MD_RS_RING];     /* последние входные отсчёты (кольцо) */
    uint32_t w;                             /* входных отсчётов записано всего (по модулю 2^32) */
    uint32_t ti, tf;                        /* точка следующего выхода: номер входного отсчёта и дробь (2^-32) */
    uint32_t si, sf;                        /* шаг на выходной отсчёт: целая часть и дробь */
    float lim;                              /* множитель ограничителя */
    float lim_min;                          /* наименьший множитель (для журнала) */
} md_rs_t;

static void md_rs_reset(md_rs_t *s)
{
    memset(s, 0, sizeof *s);
    s->lim = 1.0f;
    s->lim_min = 1.0f;
}

/* шаг = частота входа / частота выхода × (1 + eps); eps — поправка по очереди вывода */
static void md_rs_set_step(md_rs_t *s, uint32_t in_rate, float eps)
{
    const float step = (float)in_rate / (float)MD_RS_OUT_RATE * (1.0f + eps);
    s->si = (uint32_t)step;
    s->sf = (uint32_t)((step - (float)s->si) * 4294967296.0f);
}

/* n входных кадров (стерео float) -> выходные кадры в out (не больше max); ответ — сколько выдано.
 * Выход — с громкостью и ограничителем, в пределах ±MD_RS_CEILING. */
static unsigned md_rs_process(md_rs_t *s, const float *in, unsigned n, float *out, unsigned max)
{
    unsigned k = 0;
    float lim = s->lim, lim_min = s->lim_min;
    for (unsigned i = 0; i < n; i++) {
        const uint32_t wi = s->w & (MD_RS_RING - 1u);
        s->l[wi] = in[2 * i];
        s->r[wi] = in[2 * i + 1];
        s->w++;
        /* выход в точке ti + tf: нужны входы ti − 15 … ti + 16 */
        while ((int32_t)(s->w - (s->ti + 17u)) >= 0 && k < max) {
            const uint32_t p = s->tf >> 26;                          /* фаза 0..63 */
            const float a = (float)((s->tf >> 10) & 0xFFFFu) * (1.0f / 65536.0f);
            const float *k0 = md_rs_kernel[p], *k1 = md_rs_kernel[p + 1];
            const uint32_t base = s->ti - 15u;
            float yl = 0.0f, yr = 0.0f;
            for (unsigned j = 0; j < MD_RS_TAPS; j++) {
                const float c = k0[j] + a * (k1[j] - k0[j]);
                const uint32_t x = (base + j) & (MD_RS_RING - 1u);
                yl += s->l[x] * c;
                yr += s->r[x] * c;
            }
            yl *= MD_RS_GAIN;
            yr *= MD_RS_GAIN;
            const float al = yl < 0.0f ? -yl : yl, ar = yr < 0.0f ? -yr : yr, peak = al > ar ? al : ar;
            lim += (1.0f - lim) * MD_RS_RELEASE;
            if (peak * lim > MD_RS_CEILING) lim = MD_RS_CEILING / peak;
            if (lim < lim_min) lim_min = lim;
            out[2 * k] = yl * lim;
            out[2 * k + 1] = yr * lim;
            k++;
            const uint32_t f = s->tf + s->sf;
            s->ti += s->si + (f < s->tf ? 1u : 0u);
            s->tf = f;
        }
    }
    s->lim = lim;
    s->lim_min = lim_min;
    return k;
}

#endif
