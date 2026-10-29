// Стенд Verilator: декодер JPEG → запись кадра в SDRAM (vp_tb_top: jpeg_core + vp_fbwrite).
// SDRAM — модель клиента sdram_burst: ack через 1…6 тактов, затем 8 тактов wr_take подряд
// (слова — в память по адресу пакета); читатель кадров (fb_core) каждые FRAME_CYC тактов
// переходит на объявленный буфер. Каждый объявленный кадр сверяется с эталоном (RGB888 из
// tb_jpeg, сведённый к RGB565) — точка в точку на своём месте холста (размер картинки — из SOF
// файла, холст, место и уменьшение больше 1024×768 — как в vp_fbwrite), поля холста должны быть
// чёрными. Деблокер (DEBLOCK 1 или 2) и зерно (DITHER 1), как у vp_tb_top, повторены здесь по
// описанию vp_fbwrite, vp_dbk_lp и vp_noise_rom: эталон — картинка холста после деблокера, сведённая
// к RGB565 с порогами зерна (таблица — vp_noise64.bin в рабочем каталоге или путь в VP_NOISE, сдвиг
// кадра — тот же LFSR).
//
//   tb_vp.exe кадр1.jpg эталон1.rgb [кадр2.jpg эталон2.rgb …]
#include "Vvp_tb_top.h"
#include "verilated.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#ifndef DEBLOCK
#define DEBLOCK 2
#endif
#ifndef DITHER
#define DITHER 1
#endif
#ifndef DB_HALF
#define DB_HALF 1
#endif

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

// фильтр окна края (vp_dbk_lp): n точек одной составляющей (8, у уменьшенной 4); веса ×16, одно
// округление. Плоско — размах зелёной < 2·QP (решение одно на R, G, B окна)
static bool flatg(uint8_t *v[], unsigned n, int qp2)
{
    int mx = 0, mn = 255;
    for (unsigned i = 0; i < n; i++) { mx = *v[i] > mx ? *v[i] : mx; mn = *v[i] < mn ? *v[i] : mn; }
    return mx - mn < qp2;
}
static void winf(uint8_t *v[], unsigned n)
{
    static const int W8[8][8] = {{16, 0, 0, 0, 0, 0, 0, 0}, {10, 2, 1, 1, 1, 1, 0, 0}, {6, 1, 3, 2, 2, 1, 1, 0},
                                 {3, 1, 2, 4, 2, 2, 1, 1}, {1, 1, 2, 2, 4, 2, 1, 3}, {0, 1, 1, 2, 2, 3, 1, 6},
                                 {0, 0, 1, 1, 1, 1, 2, 10}, {0, 0, 0, 0, 0, 0, 0, 16}};
    static const int W4[4][4] = {{16, 0, 0, 0}, {6, 5, 4, 1}, {1, 4, 5, 6}, {0, 0, 0, 16}};
    int in[8], out[8];
    for (unsigned i = 0; i < n; i++) in[i] = *v[i];
    for (unsigned i = 0; i < n; i++) {
        int s = 0;
        for (unsigned k = 0; k < n; k++) s += (n == 8 ? W8[i][k] : W4[i][k]) * in[k];
        out[i] = (s + 8) >> 4;
    }
    for (unsigned i = 0; i < n; i++) *v[i] = (uint8_t)out[i];
}

// деблокер картинки холста fw×fh (RGB888): сетка g (8, у уменьшенной 4), окно — g точек вокруг
// края; сначала все вертикальные края, потом горизонтальные; край — если всё окно на картинке
static unsigned deblock(std::vector<uint8_t> &img, unsigned fw, unsigned fh, unsigned g, int qp2, int mode)
{
    std::vector<uint8_t> before = img;
    unsigned r = g / 2;
    auto px = [&](unsigned x, unsigned y, unsigned c) -> uint8_t * { return &img[3 * ((size_t)y * fw + x) + c]; };
    uint8_t *v[8];
    if (mode >= 1)
        for (unsigned y = 0; y < fh; y++)
            for (unsigned xe = g; xe + r <= fw; xe += g) {
                for (unsigned i = 0; i < g; i++) v[i] = px(xe - r + i, y, 1);
                if (!flatg(v, g, qp2)) continue;
                for (unsigned c = 0; c < 3; c++) {
                    for (unsigned i = 0; i < g; i++) v[i] = px(xe - r + i, y, c);
                    winf(v, g);
                }
            }
    if (mode >= 2)
        for (unsigned ye = g; ye + r <= fh; ye += g)
            for (unsigned x = 0; x < fw; x++) {
                for (unsigned i = 0; i < g; i++) v[i] = px(x, ye - r + i, 1);
                if (!flatg(v, g, qp2)) continue;
                for (unsigned c = 0; c < 3; c++) {
                    for (unsigned i = 0; i < g; i++) v[i] = px(x, ye - r + i, c);
                    winf(v, g);
                }
            }
    unsigned changed = 0;
    for (size_t i = 0; i < img.size(); i += 3)
        changed += img[i] != before[i] || img[i + 1] != before[i + 1] || img[i + 2] != before[i + 2];
    return changed;
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
    uint64_t all_cyc = 0;
    // зерно: таблица 64×64 и сдвиг кадра (LFSR vp_fbwrite: шаг в начале каждого кадра)
    const char *np = std::getenv("VP_NOISE");
    std::vector<uint8_t> noise = load(np ? np : "vp_noise64.bin");
    unsigned lfsr = 0xACE1;
    for (int a = 1; a + 1 < argc; a += 2) {
        std::vector<uint8_t> jpg = load(argv[a]), ref = load(argv[a + 1]);
        while (t.busy) tick();
        t.frame_start = 1; tick(); t.frame_start = 0;
        lfsr = (lfsr >> 1) ^ ((lfsr & 1) ? 0xB400u : 0u);
        unsigned nz_ox = lfsr & 31, nz_oy = (lfsr >> 5) & 63;
        uint64_t c0 = cyc;
        size_t pos = 0;
        while (!t.frame_ready && cyc - c0 < 6000000ull) {
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
        all_cyc += dec_cyc;
        if (!t.frame_ready) {
            unsigned g = t.dbg;
            std::printf("%s: кадр не записан: подано %zu из %zu; fst %u плитки %u %u %u %u wst %u est %u pxv %u idle %u\n",
                        argv[a], pos, jpg.size(), g >> 13, (g >> 11) & 3, (g >> 9) & 3, (g >> 7) & 3, (g >> 5) & 3,
                        (g >> 3) & 3, (g >> 2) & 1, (g >> 1) & 1, g & 1);
            unsigned long long g2 = t.dbg2;
            std::printf("  точка x %llu y %llu, режим %llu, f_open %llu fill %llu eng %llu fl %llu, точек в плитке %llu, geo_ok %llu half %llu th %llu\n",
                        g2 >> 48, (g2 >> 32) & 0xFFFF, (g2 >> 24) & 3, (g2 >> 23) & 1, (g2 >> 21) & 3, (g2 >> 19) & 3,
                        (g2 >> 17) & 3, (g2 >> 8) & 511, (g2 >> 7) & 1, (g2 >> 6) & 1, (g2 >> 4) & 3);
            bad_frames++;
            break;
        }
        t.pub_go = 1; tick(); t.pub_go = 0;
        unsigned b = t.pub_buf, cw = t.pub_w, ch = t.pub_h, words = t.pub_words;
        // размер картинки и таблица яркости — из SOF; шаг таблиц (наибольший из DC и первого AC) —
        // из DQT, как берёт его jpeg_dqt (в сегменте — байт номера таблицы, дальше значения подряд)
        unsigned w = 0, h = 0, ytab = 0, q0[4] = {0, 0, 0, 0};
        for (size_t i = 2; i + 4 < jpg.size();) {
            if (jpg[i] != 0xFF) { i++; continue; }
            unsigned m = jpg[i + 1];
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7) || m == 0xFF) { i += m == 0xFF ? 1 : 2; continue; }
            unsigned len = jpg[i + 2] << 8 | jpg[i + 3];
            const uint8_t *seg = &jpg[i + 4];
            if (m == 0xDB) {
                unsigned tab = seg[0] & 3;
                for (unsigned k = 1; k + 2 < len; k++) {
                    if (((k - 1) & 63) == 0) q0[tab] = seg[k];
                    else if (((k - 1) & 63) == 1 && seg[k] > q0[tab]) q0[tab] = seg[k];
                }
            } else if (m == 0xC0) {
                h = seg[1] << 8 | seg[2]; w = seg[3] << 8 | seg[4];
                ytab = seg[6 + 2] & 3;                            // первая составляющая — яркость
            } else if (m == 0xDA) break;
            i += 2 + len;
        }
        int QP = q0[ytab] >= 61 ? 31 : (int)((q0[ytab] + 1) >> 1), S = 2 * QP;   // S — 2·QP
        // картинка холста: больше 1024×768 — каждая вторая точка (с округлением вверх), обрезка до
        // 1024×768; широкая (9·w ≥ 14·h, h ≤ 576) — холст 16:9: 512×288, 640×360, 1024×576
        bool half = w > 1024 || h > 768;
        unsigned iw = half ? (w + 1) / 2 : w, ih = half ? (h + 1) / 2 : h;
        unsigned gw = iw > 1024 ? 1024 : iw, gh = ih > 768 ? 768 : ih;
        bool wide = 9 * gw >= 14 * gh && gh <= 576;
        unsigned ecw, ech;
        if (wide) {
            ecw = (gw <= 512 && gh <= 288) ? 512 : (gw <= 640 && gh <= 360) ? 640 : 1024;
            ech = ecw == 512 ? 288 : ecw == 640 ? 360 : 576;
        } else {
            ecw = (gw <= 512 && gh <= 384) ? 512 : (gw <= 640 && gh <= 480) ? 640 : 1024;
            ech = ecw == 512 ? 384 : ecw == 640 ? 480 : 768;
        }
        unsigned ox = ((ecw - gw) / 2) & ~15u, oy = (ech - gh) / 2;
        if (cw != ecw || ch != ech || words != cw / 2) {
            std::printf("%s: холст %ux%u слов %u, ждали %ux%u\n", argv[a], cw, ch, words, ecw, ech);
            bad_frames++;
            continue;
        }
        std::vector<uint8_t> img((size_t)gw * gh * 3);
        for (unsigned y = 0; y < gh; y++)
            for (unsigned x = 0; x < gw; x++)
                for (unsigned c = 0; c < 3; c++)
                    img[3 * ((size_t)y * gw + x) + c] = ref[3 * ((size_t)(half ? 2 * y : y) * w + (half ? 2 * x : x)) + c];
        unsigned changed = deblock(img, gw, gh, half ? 4 : 8, S, half && !DB_HALF ? 0 : DEBLOCK);
        // RGB565: с порогом зерна места точки (у R и B — /2, у G — /4, с насыщением) или усечением
        auto to565 = [&](const uint8_t *p, unsigned x, unsigned y) -> uint16_t {
            unsigned tq = 0;
            if (DITHER) tq = noise[((y + nz_oy) & 63) * 64 + 2 * (((x >> 1) + nz_ox) & 31) + (x & 1)];
            unsigned r = p[0] + (tq >> 1), g = p[1] + (tq >> 2), b = p[2] + (tq >> 1);
            r = r > 255 ? 255 : r; g = g > 255 ? 255 : g; b = b > 255 ? 255 : b;
            return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        };
        // сверка: картинка на своём месте, поля — чёрные
        size_t diff = 0, first = (size_t)-1, border = 0;
        for (unsigned y = 0; y < ch; y++)
            for (unsigned x = 0; x < cw; x++) {
                uint32_t word = mem[line_base(b, y) + x / 2];
                uint16_t got = (x & 1) ? (uint16_t)(word >> 16) : (uint16_t)word;
                if (x < ox || x >= ox + gw || y < oy || y >= oy + gh) { border += got != 0; continue; }
                const uint8_t *p = &img[3 * ((size_t)(y - oy) * gw + (x - ox))];
                uint16_t exp = to565(p, x - ox, y - oy);
                if (got != exp) { if (first == (size_t)-1) first = (size_t)(y - oy) * gw + (x - ox); diff++; }
            }
        std::printf("%s: буфер %u %ux%u → %ux%u на холсте %ux%u (x %u y %u), 2QP %d, деблокер изменил %u точек, "
                    "тактов %llu, расхождений %zu, не чёрных точек поля %zu",
                    argv[a], b, w, h, gw, gh, cw, ch, ox, oy, S, changed, (unsigned long long)dec_cyc, diff, border);
        if (diff) std::printf(" (первое x %zu y %zu)", first % gw, first / gw);
        std::printf("\n");
        diff += border;
        if (diff) bad_frames++;
    }
    std::printf("DEBLOCK %d DITHER %d: тактов всего %llu; %s\n", DEBLOCK, DITHER, (unsigned long long)all_cyc,
                bad_frames ? "ЕСТЬ ОШИБКИ" : "ВСЕ КАДРЫ ВЕРНЫ");
    return bad_frames ? 1 : 0;
}
