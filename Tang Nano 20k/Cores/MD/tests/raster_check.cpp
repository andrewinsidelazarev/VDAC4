// Развёртка md_video по стандарту CEA-861 и стоянка Mega Drive под неё (ядро 0.8, 04.11.2026). Такты: MCLK
// 54 МГц, hclk 74,25 МГц (шаг 1/1188 мкс: MCLK — 22 доли, hclk — 16). «VDP»: строка 3420 тактов MCLK, NTSC 262
// строки, PAL 313, видимых 224 или 240; гашение строки с такта 2560, синхроимпульс 2700–2800, точка — раз в 8
// тактов; гашение кадра кончается в гашении последней строки до синхроимпульса, как у VDP. Время «VDP» идёт,
// только пока sync_stall = 0 (стоянка машины).
// Проверки каждого кадра HDMI после 3-го: строки ровно 1650 (720p60) или 1980 (720p50) точек, 750 строк;
// видимых 720 строк по 1280 точек; строчный импульс 40 точек с 1390 / 1720; кадровый — строки 725–729 от
// переднего фронта строчного; захват держится; опозданий VDP нет; стоянка — одна длина ±16 тактов.
// raster_check [pal 0|1] [v30 0|1] [кадров]
#include "Vraster_top.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
    Verilated::commandArgs(argc, argv);
    const int pal = argc > 1 ? atoi(argv[1]) : 0, v30 = argc > 2 ? atoi(argv[2]) : 0;
    const int frames = argc > 3 ? atoi(argv[3]) : 24;
    const int HT = pal ? 1980 : 1650, HS0 = pal ? 1720 : 1390;
    Vraster_top t;
    const long lines = pal ? 313 : 262, active = v30 ? 240 : 224;
    const long frame_m = lines * 3420;
    t.pal = pal; t.v30 = v30; t.vblank = 1; t.hblank = 1; t.hsync = 0; t.ce_pix = 0; t.hclk = 0; t.mclk = 0; t.eval();
    long vdp = 0, stall_cycles = 0; uint64_t next_m = 11, next_h = 8;
    int hframe = -1, line_px = 0, lines_in_frame = 0, de_px = 0, de_lines = 0, hs_start = -1, hs_len = 0;
    int jumps_prev = 0, bad = 0, checked = 0, de_r = 0, hs_r = 0, vs_r = 0, vs_line = -1, vs_hx = -1, vs_end_line = -1, vs_end_hx = -1;
    long stall_ref = -1, stall_frame = 0; bool stall_r = false;
    auto fail = [&](const char *what, long a, long b) {
        if (bad++ < 12) std::printf("  кадр %d: %s: %ld (ждали %ld)\n", hframe, what, a, b);
    };
    while (hframe < frames) {
        if (next_m <= next_h) {
            next_m += 11; t.mclk = !t.mclk;
            if (t.mclk) {
                if (t.sync_stall) stall_cycles++;
                else {
                    const long p = vdp % frame_m, l = p / 3420, c = p % 3420;
                    t.vblank = !(l < active || (l == lines - 1 && c >= 2600));
                    t.hblank = c >= 2560;
                    t.hsync = c >= 2700 && c < 2800;
                    t.ce_pix = c < 2560 && c % 8 == 0;
                    vdp++;
                }
                // длина стоянки — за каждую стоянку
                if (t.sync_stall && !stall_r) stall_frame = 0;
                if (t.sync_stall) stall_frame++;
                if (!t.sync_stall && stall_r && hframe >= 3) {
                    if (stall_ref < 0) stall_ref = stall_frame;
                    else if (stall_frame > stall_ref + 16 || stall_frame < stall_ref - 16) fail("стоянка, тактов MCLK", stall_frame, stall_ref);
                }
                stall_r = t.sync_stall;
            }
        } else {
            next_h += 8; t.hclk = !t.hclk;
            t.eval();
            if (!t.hclk) continue;
            // фронт hclk: выходы de/hs/vs — точки (hx, vy) тактом раньше; считаем по ним
            line_px++;
            if (t.de) de_px++;
            if (t.hs && !hs_r) hs_start = line_px - 1;
            if (t.hs) hs_len++;
            if (t.vs && !vs_r) { vs_line = t.vy; vs_hx = t.hx; }
            if (!t.vs && vs_r) { vs_end_line = t.vy; vs_end_hx = t.hx; }
            de_r = t.de; hs_r = t.hs; vs_r = t.vs;
            if (t.hx == 0) {                                     // новая строка развёртки
                if (hframe >= 3 && lines_in_frame > 0) {
                    if (line_px != HT) fail("длина строки", line_px, HT);
                    if (de_px && de_px != 1280) fail("видимых точек в строке", de_px, 1280);
                    if (hs_len != 40) fail("строчный импульс, точек", hs_len, 40);
                    if (hs_start != HS0 + 1 && hs_start != HS0) fail("начало строчного импульса", hs_start, HS0);
                }
                if (de_px) de_lines++;
                line_px = 0; de_px = 0; hs_len = 0; hs_start = -1;
                lines_in_frame++;
                if (t.vy == 0) {                                 // новый кадр HDMI
                    if (hframe >= 3) {
                        checked++;
                        if (lines_in_frame - 1 != 750) fail("строк в кадре", lines_in_frame - 1, 750);
                        if (de_lines != 720) fail("видимых строк", de_lines, 720);
                        if (vs_line != 724 || vs_end_line != 729) fail("кадровый импульс, строки", vs_line * 1000 + vs_end_line, 724729);
                        if (!t.locked) fail("захват", 0, 1);
                        if (((t.jumps - jumps_prev) & 3) != 0) fail("опоздания VDP", (t.jumps - jumps_prev) & 3, 0);
                    }
                    jumps_prev = t.jumps;
                    hframe++; lines_in_frame = 1; de_lines = 0;
                }
            }
        }
        t.eval();
    }
    const double stall_us = stall_ref / 54.0;
    std::printf("%s: %s, %s %d: кадров сверено %d, стоянка %ld тактов MCLK (%.1f мкс на кадр), ошибок %d\n",
                bad == 0 && checked > 0 ? "ВЕРНО" : "ОШИБКА", pal ? "PAL 720p50" : "NTSC 720p60", v30 ? "V30" : "V28",
                (int)active, checked, stall_ref, stall_us, bad);
    return bad == 0 && checked > 0 ? 0 : 1;
}
