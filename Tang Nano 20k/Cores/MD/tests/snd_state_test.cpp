// Снимок звука F5/F6 (MD.ELF 1.1, 03.11.2026): md_snd_save / md_snd_load на настоящих событиях игры.
// События модели (events.bin) идут блоками, как в snd_render.cpp. На каждой точке снимка: состояние — в буфер,
// дальше M блоков (выход запомнен), загрузка того же состояния и те же M блоков ещё раз — выход обязан
// совпасть бит в бит; сохранение после загрузки обязано дать те же байты.
// snd_state_test.exe events.bin [блоков_после=64]
#include "md_sound.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::printf("ОШИБКА, строка %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "snd_state_test events.bin [блоков]\n"); return 2; }
    std::FILE *f = std::fopen(argv[1], "rb");
    if (!f) { std::perror(argv[1]); return 1; }
    std::vector<uint32_t> ev;
    uint32_t w;
    while (std::fread(&w, 4, 1, f) == 1) ev.push_back(w);
    std::fclose(f);
    const unsigned after = argc > 2 ? std::atoi(argv[2]) : 64;
    std::vector<uint64_t> t(ev.size());
    uint64_t base = 0; uint32_t prev = 0;
    for (size_t i = 0; i < ev.size(); i++) {
        const uint32_t s = (ev[i] >> 8) & 0xFFFFF;
        if (i == 0) base = s; else base += (s - prev) & 0xFFFFF;
        prev = s; t[i] = base;
    }
    const uint64_t t0 = ev.empty() ? 0 : (t[0] / 24) * 24;
    const uint64_t total = ev.empty() ? 0 : (t.back() - t0) / 24 + 1;
    const unsigned blocks = unsigned(total / MD_SND_BLOCK);
    // записи блока b
    std::vector<size_t> first(blocks + 1);
    { size_t k = 0; for (unsigned b = 0; b <= blocks; b++) { while (k < ev.size() && (t[k] - t0) / 24 < uint64_t(b) * MD_SND_BLOCK) k++; first[b] = k; } }
    std::vector<uint8_t> mem(md_snd_size());
    md_snd_init(mem.data());
    // MD_SND_SPLIT_TEST=1 (MD.ELF 1.6): каналы между половинами переставляются каждый блок — снимок и загрузка
    // обязаны дать тот же звук и при другом делении (снимок пишется с делением 3)
    const char *split_test = std::getenv("MD_SND_SPLIT_TEST");
    if (split_test && *split_test == '1') md_snd_test_split(mem.data(), 1);
    float out[MD_SND_BLOCK * 2];
    std::vector<md_snd_write_t> bw;
    auto block = [&](unsigned b, std::vector<float> *pcm) {
        bw.clear();
        for (size_t k = first[b]; k < first[b + 1]; k++) {
            md_snd_write_t x;
            x.pos = uint16_t((t[k] - t0) / 24 - uint64_t(b) * MD_SND_BLOCK);
            x.kind = uint8_t(ev[k] >> 30); x.port = uint8_t((ev[k] >> 28) & 3); x.data = uint8_t(ev[k]);
            bw.push_back(x);
        }
        md_snd_block_begin(mem.data(), bw.data(), unsigned(bw.size()), MD_SND_BLOCK);
        md_snd_render_half(mem.data(), 0);
        md_snd_render_half(mem.data(), 1);
        md_snd_mix(mem.data(), out, 1.0f);
        if (pcm) pcm->insert(pcm->end(), out, out + MD_SND_BLOCK * 2);
    };
    // точки снимка — по всей записи: каждые blocks/8 блоков (там, где после точки хватает after блоков)
    unsigned points = 0, max_state = 0;
    double loud = 0;
    for (unsigned b = 0; b + after < blocks; b++) {
        const bool point = b > 0 && b % (blocks / 8 ? blocks / 8 : 1) == 0;
        if (!point) { block(b, nullptr); continue; }
        static uint8_t s1[MD_SND_STATE_MAX], s2[MD_SND_STATE_MAX];
        const size_t n1 = md_snd_save(mem.data(), s1, sizeof s1);
        CHECK(n1 > 0);
        if (n1 > max_state) max_state = unsigned(n1);
        std::vector<float> a, c;
        for (unsigned k = 0; k < after; k++) block(b + k, &a);
        CHECK(md_snd_load(mem.data(), s1, n1));
        const size_t n2 = md_snd_save(mem.data(), s2, sizeof s2);
        CHECK(n2 == n1 && std::memcmp(s1, s2, n1) == 0);
        for (unsigned k = 0; k < after; k++) block(b + k, &c);
        CHECK(a.size() == c.size());
        for (size_t i = 0; i < a.size(); i++) {
            if (std::memcmp(&a[i], &c[i], sizeof(float)) != 0) {
                std::printf("ОШИБКА: точка %u, отсчёт %zu: %.6f после снимка, %.6f после загрузки\n", b, i / 2, a[i], c[i]);
                return 1;
            }
            loud += a[i] * a[i];
        }
        points++;
        b += after - 1;
    }
    CHECK(points > 0);
    std::printf("ВСЁ ВЕРНО: %u точек снимка по %u блоков, событий %zu, состояние до %u байт, средний уровень %.4f\n",
                points, after, ev.size(), max_state, loud > 0 ? loud / (double(points) * after * MD_SND_BLOCK * 2) : 0.0);
    return 0;
}
