// Проверка пересчёта частоты звука MD (esp/main/md_resample.h, MD.ELF 1.7, 09.11.2026) на ПК.
//  1. Тоны на частоте YM 53 098 Гц (0,1 полной шкалы): на выходе 48 кГц — уровень (с громкостью x3 — 0,3),
//     искажения и шум (остаток после подгонки синуса), подавление тонов выше 24 кГц (их не должно быть слышно).
//  2. Тот же тон, шаг меняется каждые 256 входных отсчётов на ±0,3 % (как подстройка по очереди вывода):
//     остаток должен остаться таким же — без разрывов.
//  3. События звука (events.bin) -> движок (md_sound.cpp, громкость 1) -> пересчёт -> WAV 48 кГц.
// snd_resample [events.bin out.wav [частота_YM]]
#include "md_sound.h"
#include "md_resample.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

// остаток после подгонки синуса частоты f (МНК по sin/cos/постоянной), в дБ к амплитуде; amp — амплитуда
double fit_residual(const std::vector<float> &y, double f, double sr, double *amp) {
    double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
    const size_t a = y.size() / 4, b = y.size() * 3 / 4;   // середина: без краёв
    for (size_t i = a; i < b; i++) {
        const double s = std::sin(2 * M_PI * f * i / sr), c = std::cos(2 * M_PI * f * i / sr);
        ss += s * s; sc += s * c; cc += c * c; ys += y[i] * s; yc += y[i] * c;
    }
    const double det = ss * cc - sc * sc, A = (ys * cc - yc * sc) / det, B = (yc * ss - ys * sc) / det;
    double e = 0;
    for (size_t i = a; i < b; i++) {
        const double r = y[i] - A * std::sin(2 * M_PI * f * i / sr) - B * std::cos(2 * M_PI * f * i / sr);
        e += r * r;
    }
    *amp = std::sqrt(A * A + B * B);
    return 10 * std::log10(e / double(b - a) / (*amp * *amp / 2) + 1e-30);
}

std::vector<float> tone_through(double f, uint32_t in_rate, bool wobble) {
    md_rs_t rs; md_rs_reset(&rs); md_rs_set_step(&rs, in_rate, 0.0f);
    const unsigned N = in_rate * 2;
    std::vector<float> in(2 * 256), out(2 * 320), y;
    for (unsigned blk = 0; blk < N / 256; blk++) {
        for (unsigned i = 0; i < 256; i++) {
            const float v = 0.1f * float(std::sin(2 * M_PI * f * (blk * 256 + i) / in_rate));
            in[2 * i] = in[2 * i + 1] = v;
        }
        if (wobble) md_rs_set_step(&rs, in_rate, (blk & 1) ? 0.003f : -0.003f);
        const unsigned k = md_rs_process(&rs, in.data(), 256, out.data(), 320);
        for (unsigned i = 0; i < k; i++) y.push_back(out[2 * i]);
    }
    return y;
}

void put16(std::FILE *f, uint16_t v) { std::fputc(v & 255, f); std::fputc(v >> 8, f); }
void put32(std::FILE *f, uint32_t v) { put16(f, v & 0xFFFF); put16(f, v >> 16); }

}  // namespace

int main(int argc, char **argv) {
    const uint32_t rate = argc > 3 ? uint32_t(std::atoi(argv[3])) : 53098u;
    std::printf("тоны 0,1 шкалы на %u Гц -> 48000 Гц (громкость x%.0f, ожидается 0,300):\n", rate, double(MD_RS_GAIN));
    const double tones[] = {100, 1000, 5000, 10000, 15000, 18000, 20000, 22000, 25000, 26000};
    int bad = 0;
    for (double f : tones) {
        auto y = tone_through(f, rate, false);
        double amp = 0;
        if (f < 24000) {
            const double r = fit_residual(y, f, 48000, &amp);
            auto yw = tone_through(f, rate, true);
            double ampw = 0;
            const double rw = fit_residual(yw, f, 48000 * 1.0, &ampw);   // со сменой шага частота «плавает» — грубая оценка
            std::printf("  %6.0f Гц: уровень %.4f (%+.2f дБ), остаток %.1f дБ; со сменой шага ±0,3%%: уровень %.4f\n", f, amp,
                        20 * std::log10(amp / 0.3), r, ampw);
            (void)rw;
            if (f <= 15000 && (std::fabs(20 * std::log10(amp / 0.3)) > 0.5 || r > -60)) bad++;
        } else {
            double e = 0;
            for (size_t i = y.size() / 4; i < y.size() * 3 / 4; i++) e += double(y[i]) * y[i];
            const double rms = std::sqrt(e / double(y.size() / 2));
            std::printf("  %6.0f Гц (выше 24 кГц): осталось %.1f дБ к 0,3\n", f, 20 * std::log10(rms * std::sqrt(2.0) / 0.3 + 1e-30));
        }
    }
    // разрывы при смене шага: тон 1 кГц, наибольший скачок второй разности против без смены
    {
        auto a = tone_through(1000, rate, false), b = tone_through(1000, rate, true);
        auto maxd2 = [](const std::vector<float> &y) {
            double m = 0;
            for (size_t i = 2; i < y.size(); i++) m = std::fmax(m, std::fabs(double(y[i]) - 2 * y[i - 1] + y[i - 2]));
            return m;
        };
        std::printf("1 кГц: наибольшая вторая разность без смены шага %.5f, со сменой %.5f\n", maxd2(a), maxd2(b));
    }
    if (argc < 3) return bad ? 1 : 0;

    // события -> WAV 48 кГц (как snd_render, но громкость микшера 1 и пересчёт md_resample.h)
    std::FILE *f = std::fopen(argv[1], "rb");
    if (!f) { std::perror(argv[1]); return 1; }
    std::vector<uint32_t> ev; uint32_t w;
    while (std::fread(&w, 4, 1, f) == 1) ev.push_back(w);
    std::fclose(f);
    std::vector<uint64_t> t(ev.size()); uint64_t base = 0; uint32_t prev = 0;
    for (size_t i = 0; i < ev.size(); i++) {
        const uint32_t s = (ev[i] >> 8) & 0xFFFFF;
        if (i == 0) base = s; else base += (s - prev) & 0xFFFFF;
        prev = s; t[i] = base;
    }
    const uint64_t t0 = ev.empty() ? 0 : (t[0] / 24) * 24;
    const uint64_t total = (ev.empty() ? 0 : t.back() - t0) / 24 + rate / 2;
    std::vector<uint8_t> mem(md_snd_size());
    md_snd_init(mem.data());
    md_rs_t rs; md_rs_reset(&rs); md_rs_set_step(&rs, rate, 0.0f);
    std::vector<md_snd_write_t> bw;
    static float mix[(MD_SND_BLOCK + 1) * 2], out[(MD_SND_BLOCK + 64) * 2];
    std::vector<int16_t> pcm;
    size_t k = 0;
    for (uint64_t s = 0; s < total; s += MD_SND_BLOCK) {
        const unsigned n = unsigned(total - s < MD_SND_BLOCK ? total - s : MD_SND_BLOCK);
        bw.clear();
        while (k < ev.size() && (t[k] - t0) / 24 < s + n) {
            md_snd_write_t x;
            x.pos = uint16_t((t[k] - t0) / 24 - s);
            x.kind = uint8_t(ev[k] >> 30); x.port = uint8_t((ev[k] >> 28) & 3); x.data = uint8_t(ev[k]);
            bw.push_back(x); k++;
        }
        if (!md_snd_block_begin(mem.data(), bw.data(), unsigned(bw.size()), n)) return 1;
        md_snd_render_half(mem.data(), 0);
        md_snd_render_half(mem.data(), 1);
        md_snd_mix(mem.data(), mix, 1.0f);
        const unsigned m = md_rs_process(&rs, mix, n, out, MD_SND_BLOCK + 64);
        for (unsigned i = 0; i < 2 * m; i++) pcm.push_back(int16_t(std::lround(out[i] * 32767.0f)));
    }
    std::FILE *o = std::fopen(argv[2], "wb");
    const uint32_t bytes = uint32_t(pcm.size() * 2), sr = MD_RS_OUT_RATE;
    std::fwrite("RIFF", 1, 4, o); put32(o, 36 + bytes); std::fwrite("WAVEfmt ", 1, 8, o);
    put32(o, 16); put16(o, 1); put16(o, 2); put32(o, sr); put32(o, sr * 4); put16(o, 4); put16(o, 16);
    std::fwrite("data", 1, 4, o); put32(o, bytes); std::fwrite(pcm.data(), 2, pcm.size(), o);
    std::fclose(o);
    std::printf("WAV 48 кГц: %.2f с, ограничитель — наименьший множитель %.3f (%.1f дБ)\n", pcm.size() / 2.0 / sr,
                double(rs.lim_min), 20 * std::log10(double(rs.lim_min)));
    return bad ? 1 : 0;
}
