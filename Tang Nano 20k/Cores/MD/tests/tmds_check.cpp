// Слова TMDS md_video (03.11.2026): выход HDMI (hdmi_tx — задержка 11 тактов). На каждом такте слово канала
// сверяется со входами hdmi_tx 11 тактов назад: в DE — данные, которые декодируются обратно в байт цвета; вне DE —
// управляющий код (канал 0 — {vsync, hsync}), защитная полоса или символ TERC4 островка. Считаются островки
// (по кадру — один) и преамбулы видео (перед каждой активной строкой: канал 1 — управляющий код CTL0 = 1).
#include "Vtmds_top.h"
#include "verilated.h"
#include <cstdint>
#include <cstdio>

static const uint16_t CTRL[4] = {0x354, 0x0AB, 0x154, 0x2AB};   // 1101010100 0010101011 0101010100 1010101011
static const uint16_t TERC4[16] = {0x29C, 0x263, 0x2E4, 0x2E2, 0x171, 0x11E, 0x18E, 0x13C,
                                   0x2CC, 0x139, 0x19C, 0x2C6, 0x28E, 0x271, 0x163, 0x2C3};
static const uint16_t VGB_02 = 0x2CC, GB_1 = 0x133;

static int decode(uint16_t w)                                    // TMDS 10 -> 8
{
    uint16_t d = w & 0xFF;
    if (w & 0x200) d ^= 0xFF;
    uint8_t q = d & 1;
    for (int i = 1; i < 8; i++) {
        const uint8_t b = (d >> i) & 1, p = (d >> (i - 1)) & 1;
        q |= (uint8_t)(((w & 0x100) ? (b ^ p) : !(b ^ p)) << i);
    }
    return q;
}
static int ctrl_of(uint16_t w) { for (int i = 0; i < 4; i++) if (CTRL[i] == w) return i; return -1; }
static bool terc4(uint16_t w) { for (int i = 0; i < 16; i++) if (TERC4[i] == w) return true; return false; }

int main(int argc, char** argv)
{
    Verilated::commandArgs(argc, argv);
    Vtmds_top t;
    t.hclk = 0; t.mclk = 0; t.eval();
    const int L = 11;
    struct In { uint8_t de, hs, vs; uint32_t rgb; } hist[16] = {};
    long bad_ctrl = 0, bad_data = 0, de_cycles = 0, lit = 0, islands = 0, preambles = 0, lines = 0, frames = 0;
    int island_run = 0, pre_run = 0;
    uint8_t de_in_p = 0, vs_in_p = 0;
    for (long c = 0; c < 2 * 1700 * 760; c++) {
        t.hclk = 1; t.mclk = !t.mclk; t.eval();
        const uint16_t w[3] = {t.t0, t.t1, t.t2};
        const In& in = hist[(c - L) & 15];
        if (c > 4000) {
            if (in.de) {
                de_cycles++;
                if (in.rgb) lit++;
                for (int k = 0; k < 3; k++) {
                    const int v = decode(w[k]);
                    if (v != (int)((in.rgb >> (8 * k)) & 0xFF)) { if (bad_data++ < 5) std::printf("DE: канал %d слово %03X -> %d, ждали %u (такт %ld)\n", k, w[k], v, (in.rgb >> (8 * k)) & 0xFF, c); }
                }
            } else {
                const int c0 = ctrl_of(w[0]);
                const bool guard = w[0] == VGB_02 && w[1] == GB_1 && w[2] == VGB_02;
                const bool isl = terc4(w[0]) && (terc4(w[1]) || w[1] == GB_1) && (terc4(w[2]) || w[2] == GB_1);
                const bool ctl = c0 >= 0 && ctrl_of(w[1]) >= 0 && ctrl_of(w[2]) >= 0;
                if (ctl && c0 != ((in.vs << 1) | in.hs)) { if (bad_ctrl++ < 5) std::printf("гашение: канал 0 %03X, ждали {vs %d hs %d} (такт %ld)\n", w[0], in.vs, in.hs, c); }
                if (!ctl && !guard && !isl) { if (bad_ctrl++ < 5) std::printf("гашение: %03X %03X %03X — не код, не полоса, не TERC4 (такт %ld)\n", w[0], w[1], w[2], c); }
                if (isl) island_run++; else { if (island_run >= 30) islands++; island_run = 0; }
                if (ctl && ctrl_of(w[1]) == 1) pre_run++; else { if (pre_run == 8) preambles++; pre_run = 0; }
            }
        }
        if (t.de && !de_in_p) lines++;
        if (t.vs && !vs_in_p) frames++;
        de_in_p = t.de; vs_in_p = t.vs;
        hist[c & 15] = In{(uint8_t)t.de, (uint8_t)t.hs, (uint8_t)t.vs, (uint32_t)t.rgb};
        t.hclk = 0; t.eval();
    }
    std::printf("кадров %ld, активных строк %ld; преамбул видео %ld, островков %ld; тактов DE %ld, не чёрных %ld\n",
                frames, lines, preambles, islands, de_cycles, lit);
    std::printf("неверных слов: в гашении %ld, в DE %ld\n", bad_ctrl, bad_data);
    return (bad_ctrl || bad_data || islands < frames - 1 || preambles < lines - 2) ? 1 : 0;
}
