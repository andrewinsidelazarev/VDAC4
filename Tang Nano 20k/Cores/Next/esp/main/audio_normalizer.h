/* Микшер Next выдаёт UNSIGNED 13 бит, LINK дополняет их тремя нулями.
   Удаляем постоянную составляющую; общий коэффициент сохраняет стереобаланс.
   Усиление ограничено 16x, пики — 0,85; уменьшение немедленное, подъём плавный. */
typedef struct {
    float previous[2], filtered[2], peak, gain, inverse_rate;
    int initialized;
} next_audio_normalizer_t;

static next_audio_normalizer_t next_audio_normalizer = {{0,0},{0,0},0,8,1.0f/48000.0f,0};

static void next_audio_normalizer_reset(float rate)
{
    memset(&next_audio_normalizer, 0, sizeof next_audio_normalizer);
    next_audio_normalizer.gain = 8.0f;
    next_audio_normalizer.inverse_rate = 1.0f / rate;
}

static void next_audio_normalize(float *frames, uint32_t count)
{
    next_audio_normalizer_t *s = &next_audio_normalizer;
    float peak = 0;
    /* Фильтр около 8 Гц. Частота дискретизации берётся из регистра ядра. */
    const float pole = 1.0f - 50.0f * s->inverse_rate;
    for (uint32_t i = 0; i < count; ++i) {
        if (!s->initialized) {
            s->previous[0] = frames[2*i];
            s->previous[1] = frames[2*i+1];
            s->initialized = 1;
        }
        for (unsigned channel = 0; channel < 2; ++channel) {
            const float input = frames[2*i+channel];
            float value = input - s->previous[channel] + pole*s->filtered[channel];
            if (value < 1.0e-12f && value > -1.0e-12f) value = 0;
            s->previous[channel] = input;
            s->filtered[channel] = value;
            frames[2*i+channel] = value;
            const float magnitude = value < 0 ? -value : value;
            if (magnitude > peak) peak = magnitude;
        }
    }
    const float seconds = (float)count*s->inverse_rate;
    s->peak *= 1.0f - seconds;
    if (peak > s->peak) s->peak = peak;
    float target = s->peak > 0.85f/16.0f ? 0.85f/s->peak : 16.0f;
    if (target < s->gain) s->gain = target;
    else s->gain += (target-s->gain)*(seconds/0.4f);
    for (uint32_t i = 0; i < 2*count; ++i) {
        float value = frames[i]*s->gain;
        /* Защита от округления и неожиданных переходных процессов. */
        if (value > 0.98f) value = 0.98f;
        if (value < -0.98f) value = -0.98f;
        frames[i] = value;
    }
}
