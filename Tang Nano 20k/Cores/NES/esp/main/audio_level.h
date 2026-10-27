/* SPDX-License-Identifier: Apache-2.0
 * Звук ядра NES (NES.ELF 0.8): отсчёт кольца → float для pcm_write.
 *
 * Ядро кладёт в кольцо {sample, sample} — 16 бит БЕЗ знака: у обычных мапперов это инвертированный
 * выход APU (65535 − сумма каналов, тишина — FFFF, громче всего — около 0890), у мапперов со своим
 * звуком (19, 24, 26) — смесь картриджа. До 0.8 NES.ELF читал его как int16: звук шёл на полную шкалу, а в
 * громких местах (сумма каналов больше половины) значение перескакивало с −32768 на +32767 — щелчки.
 *
 * Теперь: отсчёт без знака → 0…1, постоянная составляющая убирается фильтром высоких частот первого
 * порядка около 37 Гц (как выход Famicom), усиление NES_AUDIO_GAIN, мягкий предел у ±1 (без libm).
 * Отсчёт 0 у обычного маппера невозможен — это тишина ядра на паузе и при загрузке картриджа
 * (nes_link.sv): держится прежний уровень, и фильтр плавно уходит к нулю без щелчка. */
#define NES_AUDIO_HPF_HZ 37.0f
#define NES_AUDIO_GAIN   1.6f     /* обычная громкая музыка — около ±0,4 */
#define NES_AUDIO_KNEE   0.9f     /* выше — мягкое ограничение к 1,0 */

typedef struct {
    float level;                  /* последний уровень (0…1) — для отсчётов-пауз */
    float previous, filtered;     /* состояние фильтра */
    float pole;
    int started;                  /* пришёл первый настоящий отсчёт */
} nes_audio_t;

static void nes_audio_reset(nes_audio_t *a, float rate)
{
    a->level = a->previous = a->filtered = 0;
    a->pole = 1.0f - 2.0f * 3.14159265f * NES_AUDIO_HPF_HZ / (rate > 1000.0f ? rate : 48000.0f);
    a->started = 0;
}

static float nes_audio_limit(float y)
{
    const float m = y < 0 ? -y : y;
    if (m <= NES_AUDIO_KNEE) return y;
    const float e = m - NES_AUDIO_KNEE, top = 1.0f - NES_AUDIO_KNEE;
    const float v = NES_AUDIO_KNEE + top * e / (e + top);   /* к 1,0, не выше */
    return y < 0 ? -v : v;
}

/* n слов кольца (старшая половина — левый, младшая — правый; у ядра они равны) → 2n float */
static void nes_audio_convert(nes_audio_t *a, const uint32_t *words, uint32_t n, float *frames)
{
    for (uint32_t i = 0; i < n; i++) {
        const uint16_t raw = (uint16_t)(words[i] >> 16);
        if (raw) {
            a->level = (float)raw * (1.0f / 65536.0f);
            if (!a->started) {                    /* без скачка от нуля к первому уровню */
                a->previous = a->level;
                a->filtered = 0;
                a->started = 1;
            }
        }
        float y = 0;
        if (a->started) {
            y = a->level - a->previous + a->pole * a->filtered;
            if (y < 1.0e-12f && y > -1.0e-12f) y = 0;
            a->previous = a->level;
            a->filtered = y;
        }
        const float out = nes_audio_limit(y * NES_AUDIO_GAIN);
        frames[2 * i] = out;
        frames[2 * i + 1] = out;
    }
}
