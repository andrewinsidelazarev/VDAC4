// События звука модели (events.bin, слова md_snd_events) -> WAV через тот же движок, что на ESP
// (esp/main/md_sound.cpp). Время — 20 бит тактов синтеза YM (MCLK/42), разворачивается по порядку.
// snd_render.exe events.bin out.wav [хвост_мс]
#include "md_sound.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>

static void put16(std::FILE *f, uint16_t v) { std::fputc(v & 255, f); std::fputc(v >> 8, f); }
static void put32(std::FILE *f, uint32_t v) { put16(f, v & 0xFFFF); put16(f, v >> 16); }

int main(int argc, char **argv) {
    if (argc < 3) { std::fprintf(stderr, "snd_render events.bin out.wav [хвост_мс]\n"); return 2; }
    std::FILE *f = std::fopen(argv[1], "rb");
    if (!f) { std::perror(argv[1]); return 1; }
    std::vector<uint32_t> ev;
    uint32_t w;
    while (std::fread(&w, 4, 1, f) == 1) ev.push_back(w);
    std::fclose(f);
    const unsigned tail_ms = argc > 3 ? std::atoi(argv[3]) : 500;
    // развернуть время
    std::vector<uint64_t> t(ev.size());
    uint64_t base = 0; uint32_t prev = 0;
    for (size_t i = 0; i < ev.size(); i++) {
        const uint32_t s = (ev[i] >> 8) & 0xFFFFF;
        if (i == 0) base = s; else base += (s - prev) & 0xFFFFF;
        prev = s; t[i] = base;
    }
    const uint64_t t0 = ev.empty() ? 0 : (t[0] / 24) * 24;
    const double rate = 54e6 / 1008.0;
    const uint64_t end = (ev.empty() ? 0 : t.back() - t0) + uint64_t(tail_ms * rate / 1000.0) * 24;
    const uint64_t total = end / 24;

    std::vector<uint8_t> mem(md_snd_size());
    md_snd_init(mem.data());
    // MD_SND_SPLIT_TEST=1 — каналы между половинами переставляются каждый блок (MD.ELF 1.6): WAV обязан совпасть
    const char *split_test = std::getenv("MD_SND_SPLIT_TEST");
    if (split_test && *split_test == '1') md_snd_test_split(mem.data(), 1);
    std::vector<int16_t> pcm;
    pcm.reserve(total * 2);
    std::vector<md_snd_write_t> bw;
    float out[MD_SND_BLOCK * 2];
    size_t k = 0;
    auto c0 = std::chrono::steady_clock::now();
    double half_s[2] = {0, 0}, max_s = 0;
    for (uint64_t s = 0; s < total; s += MD_SND_BLOCK) {
        const unsigned n = unsigned(total - s < MD_SND_BLOCK ? total - s : MD_SND_BLOCK);
        bw.clear();
        while (k < ev.size() && (t[k] - t0) / 24 < s + n) {
            md_snd_write_t x;
            x.pos = uint16_t((t[k] - t0) / 24 - s);
            x.kind = uint8_t(ev[k] >> 30); x.port = uint8_t((ev[k] >> 28) & 3); x.data = uint8_t(ev[k]);
            bw.push_back(x);
            k++;
        }
        if (!md_snd_block_begin(mem.data(), bw.data(), unsigned(bw.size()), n)) {
            std::fprintf(stderr, "блок %llu: записей %zu — больше предела\n", (unsigned long long)s, bw.size());
            return 1;
        }
        const auto h0 = std::chrono::steady_clock::now();
        md_snd_render_half(mem.data(), 0);
        const auto h1 = std::chrono::steady_clock::now();
        md_snd_render_half(mem.data(), 1);
        const auto h2 = std::chrono::steady_clock::now();
        // половины идут на ESP параллельно: блок стоит столько, сколько дольшая
        half_s[0] += std::chrono::duration<double>(h1 - h0).count();
        half_s[1] += std::chrono::duration<double>(h2 - h1).count();
        max_s += std::max(std::chrono::duration<double>(h1 - h0).count(), std::chrono::duration<double>(h2 - h1).count());
        md_snd_mix(mem.data(), out, 1.0f);
        for (unsigned i = 0; i < n * 2; i++) pcm.push_back(int16_t(out[i] * 32767.0f));
    }
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
    std::FILE *o = std::fopen(argv[2], "wb");
    const uint32_t bytes = uint32_t(pcm.size() * 2), sr = uint32_t(rate + 0.5);
    std::fwrite("RIFF", 1, 4, o); put32(o, 36 + bytes); std::fwrite("WAVEfmt ", 1, 8, o);
    put32(o, 16); put16(o, 1); put16(o, 2); put32(o, sr); put32(o, sr * 4); put16(o, 4); put16(o, 16);
    std::fwrite("data", 1, 4, o); put32(o, bytes); std::fwrite(pcm.data(), 2, pcm.size(), o);
    std::fclose(o);
    std::printf("событий %zu, отсчётов %llu (%.2f с звука), синтез на ПК %.3f с\n", ev.size(),
                (unsigned long long)total, total / rate, sec);
    unsigned moves = 0;
    const unsigned split = md_snd_split(mem.data(), &moves);
    std::printf("каналы половины 0: 0..%u, смен деления %u\n", split - 1, moves);
    std::printf("нс на отсчёт: половина 0 %.1f, половина 1 %.1f, дольшая из двух (блок на ESP) %.1f\n",
                half_s[0] * 1e9 / double(total), half_s[1] * 1e9 / double(total), max_s * 1e9 / double(total));
    return 0;
}
