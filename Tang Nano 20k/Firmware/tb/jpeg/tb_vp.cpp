// Стенд Verilator: декодер JPEG → запись кадра в SDRAM (vp_tb_top: jpeg_core + vp_fbwrite).
// SDRAM — модель клиента sdram_burst: ack через 1…6 тактов, затем 8 тактов wr_take подряд
// (слова — в память по адресу пакета); читатель кадров (fb_core) каждые FRAME_CYC тактов
// переходит на объявленный буфер. Каждый объявленный кадр сверяется с эталоном (RGB888 из
// tb_jpeg, сведённый к RGB565) — точка в точку на своём месте холста (размер картинки — из SOF
// файла, холст и место — как в vp_fbwrite), поля холста должны быть чёрными.
//
//   tb_vp.exe кадр1.jpg эталон1.rgb [кадр2.jpg эталон2.rgb …]
#include "Vvp_tb_top.h"
#include "verilated.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

double sc_time_stamp() { return 0; }

static const uint32_t FB_BASE = 0x128000, FT_BUF_WORDS = 0x060000, T_LO = 0x080000, T_HI = 0x0C9000;
static const unsigned FRAME_CYC = 2000;   // «выходной кадр» читателя (коротко: перебрать буферы)

static uint32_t line_base(unsigned b, unsigned y)
{
    if (b == 0) return FB_BASE + y * 512;
    if (b == 1) return FB_BASE + FT_BUF_WORDS + y * 512;
    return y < 512 ? T_LO + y * 512 : T_HI + (y - 512) * 512;
}

static std::vector<uint8_t> load(const char *p)
{
    std::vector<uint8_t> v;
    std::FILE *f = std::fopen(p, "rb");
    if (!f) { std::fprintf(stderr, "нет %s\n", p); std::exit(2); }
    for (int c; (c = std::fgetc(f)) != EOF;) v.push_back((uint8_t)c);
    std::fclose(f);
    return v;
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    Vvp_tb_top t;
    std::vector<uint32_t> mem(1u << 21, 0xDEADBEEF);
    uint64_t cyc = 0;
    // модель SDRAM
    int sd_state = 0, sd_wait = 0, sd_n = 0;
    uint32_t sd_addr = 0;
    // читатель
    bool showing = false;
    unsigned front = 0;
    unsigned seed = 12345;
    auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7FFF; };

    auto tick = [&]() {
        // входы на спаде
        t.clk = 0;
        t.s_ack = sd_state == 2;
        t.s_wr_take = sd_state == 4;
        t.fb_showing = showing;
        t.fb_front = front;
        t.eval();
        // модель SDRAM: слово записи берётся в такт wr_take
        if (sd_state == 4)
            mem[(sd_addr & ~7u) + sd_n] = t.s_wdata;
        t.clk = 1;
        t.eval();
        // переходы модели после фронта
        switch (sd_state) {
        case 0: if (t.s_req) { sd_state = 1; sd_wait = 1 + rnd() % 6; } break;
        case 1: if (--sd_wait == 0) { sd_state = 2; sd_addr = t.s_addr; } break;
        case 2: sd_state = 3; sd_wait = 2; break;                 // ack — один такт
        case 3: if (--sd_wait == 0) { sd_state = 4; sd_n = 0; } break;
        case 4: if (++sd_n == 8) sd_state = 0; break;
        }
        cyc++;
        if (cyc % FRAME_CYC == 0 && t.pub_v) { showing = true; front = t.pub_buf; }
    };

    t.rst_n = 0; t.in_valid = 0; t.frame_start = 0; t.pub_go = 0;
    for (int i = 0; i < 10; i++) tick();
    t.rst_n = 1;
    for (int i = 0; i < 10; i++) tick();

    int bad_frames = 0;
    for (int a = 1; a + 1 < argc; a += 2) {
        std::vector<uint8_t> jpg = load(argv[a]), ref = load(argv[a + 1]);
        while (t.busy) tick();
        t.frame_start = 1; tick(); t.frame_start = 0;
        uint64_t c0 = cyc;
        size_t pos = 0;
        while (!t.frame_ready && cyc - c0 < 3000000ull) {
            if (pos < jpg.size()) {
                uint32_t w = 0, s = 0;
                for (int k = 0; k < 4 && pos + k < jpg.size(); k++) { w |= (uint32_t)jpg[pos + k] << (8 * k); s |= 1u << k; }
                t.in_valid = 1; t.in_data = w; t.in_strb = s;
            } else t.in_valid = 0;
            t.clk = 0; t.eval();
            bool fire = t.in_valid && t.in_accept;
            tick();
            if (fire) pos += 4;
        }
        t.in_valid = 0;
        uint64_t dec_cyc = cyc - c0;
        if (!t.frame_ready) {
            unsigned g = t.dbg;
            std::printf("%s: кадр не записан: подано %zu из %zu; fst %u tst %u %u wst %u fill %u fl %u got %u pxv %u pxr %u idle %u\n",
                        argv[a], pos, jpg.size(), g >> 13, (g >> 11) & 3, (g >> 9) & 3, (g >> 6) & 7, (g >> 5) & 1,
                        (g >> 4) & 1, (g >> 3) & 1, (g >> 2) & 1, (g >> 1) & 1, g & 1);
            bad_frames++;
            break;
        }
        t.pub_go = 1; tick(); t.pub_go = 0;
        unsigned b = t.pub_buf, cw = t.pub_w, ch = t.pub_h, words = t.pub_words;
        // размер картинки — из SOF; холст и место — как в vp_fbwrite
        unsigned w = 0, h = 0;
        for (size_t i = 2; i + 8 < jpg.size(); i++)
            if (jpg[i] == 0xFF && jpg[i + 1] == 0xC0) { h = jpg[i + 5] << 8 | jpg[i + 6]; w = jpg[i + 7] << 8 | jpg[i + 8]; break; }
        // широкая (9·w ≥ 14·h, h ≤ 576) — холст 16:9: 512×288, 640×360, 1024×576 (vp_fbwrite 1.36)
        unsigned gw = w > 1024 ? 1024 : w, gh = h > 768 ? 768 : h;
        bool wide = 9 * gw >= 14 * gh && gh <= 576;
        unsigned ecw, ech;
        if (wide) {
            ecw = (gw <= 512 && gh <= 288) ? 512 : (gw <= 640 && gh <= 360) ? 640 : 1024;
            ech = ecw == 512 ? 288 : ecw == 640 ? 360 : 576;
        } else {
            ecw = (gw <= 512 && gh <= 384) ? 512 : (gw <= 640 && gh <= 480) ? 640 : 1024;
            ech = ecw == 512 ? 384 : ecw == 640 ? 480 : 768;
        }
        unsigned ox = ((ecw - w) / 2) & ~15u, oy = (ech - h) / 2;
        if (cw != ecw || ch != ech || words != cw / 2) {
            std::printf("%s: холст %ux%u слов %u, ждали %ux%u\n", argv[a], cw, ch, words, ecw, ech);
            bad_frames++;
            continue;
        }
        // сверка: картинка на своём месте, поля — чёрные
        size_t diff = 0, first = (size_t)-1, border = 0;
        for (unsigned y = 0; y < ch; y++)
            for (unsigned x = 0; x < cw; x++) {
                uint32_t word = mem[line_base(b, y) + x / 2];
                uint16_t got = (x & 1) ? (uint16_t)(word >> 16) : (uint16_t)word;
                if (x < ox || x >= ox + w || y < oy || y >= oy + h) { border += got != 0; continue; }
                const uint8_t *p = &ref[3 * ((size_t)(y - oy) * w + (x - ox))];
                uint16_t exp = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
                if (got != exp) { if (first == (size_t)-1) first = (size_t)(y - oy) * w + (x - ox); diff++; }
            }
        std::printf("%s: буфер %u %ux%u на холсте %ux%u (x %u y %u), тактов %llu, расхождений %zu, не чёрных точек поля %zu",
                    argv[a], b, w, h, cw, ch, ox, oy, (unsigned long long)dec_cyc, diff, border);
        if (diff) std::printf(" (первое x %zu y %zu)", first % w, first / w);
        std::printf("\n");
        diff += border;
        if (diff) bad_frames++;
    }
    std::printf("%s\n", bad_frames ? "ЕСТЬ ОШИБКИ" : "ВСЕ КАДРЫ ВЕРНЫ");
    return bad_frames ? 1 : 0;
}
