// Окно и масштаб md_video по точкам (03.11.2026, ядро 0.2: 16:9). «VDP» — H40, NTSC, 224 строки: строка
// 3420 тактов MCLK, точка — каждые 8 тактов (320 точек), гашение строки, синхроимпульс в нём; гашение кадра
// кончается в гашении последней строки до синхроимпульса, как у VDP. Цвет точки — её координаты:
// R = x[3:0], G = x[7:4], B = {x[8], y[2:0]}. HDMI: каждая точка окна — точка источника
// (floor(k*320/W), floor(m*224/H)), вне окна — чёрное. Проверяются кадры после захвата.
// Ядро 0.8 (04.11.2026): развёртка по стандарту 720p60, «VDP» стоит, пока sync_stall = 1 (как машина).
// pic_check <picture_size: [1:0] размер, [2] 16:9> [кадров] [pal 0|1] [v30 0|1]
// (PAL — 313 строк в кадре VDP, развёртка 720p50: запас кольца там меньше всего — до 59 строк из 64)
#include "Vpic_top.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
    Verilated::commandArgs(argc, argv);
    const int ps = argc > 1 ? atoi(argv[1]) : 0, frames = argc > 2 ? atoi(argv[2]) : 8;
    const int wide = ps >> 2, size = ps & 3;
    const int pal = argc > 3 ? atoi(argv[3]) : 0, v30 = argc > 4 ? atoi(argv[4]) : 0, SH = v30 ? 240 : 224;
    const int W = wide ? (size == 1 ? 1216 : size == 2 ? 1152 : 1280) : (size == 1 ? 912 : size == 2 ? 864 : 960);
    const int X0 = (1280 - W) / 2;
    const int H = size == 1 ? 684 : size == 2 ? 648 : 720, Y0 = size == 1 ? 18 : size == 2 ? 36 : 0;   // ядро 0.7: на всю высоту
    Vpic_top t;
    t.picture_size = ps; t.pal = pal; t.v30 = v30; t.vblank = 1; t.hblank = 1; t.ce_pix = 0; t.hsync = 0; t.hclk = 0; t.mclk = 0; t.eval();
    long mcyc = 0, vdp = 0; uint64_t next_m = 11, next_h = 8;
    const long frame_m = (pal ? 313L : 262L) * 3420;
    int hframe = 0, line = -1, px = 0, de_r = 0, locked_frames = 0;
    long bad = 0, checked = 0, lit = 0;
    bool frame_ok = false;
    while (hframe < frames) {
        bool h_rise = false;
        if (next_m <= next_h) {
            next_m += 11; t.mclk = !t.mclk;
            if (t.mclk && !t.sync_stall) {
                const long pos = vdp % frame_m, l = pos / 3420, c = pos % 3420;
                t.vblank = !(l < SH || (l == (pal ? 312 : 261) && c >= 2600));
                t.hblank = c >= 2560;
                t.hsync = c >= 2700 && c < 2800;
                t.ce_pix = c < 2560 && c % 8 == 0;
                const int x = int(c / 8), y = int(l);
                t.r = x & 15; t.g = (x >> 4) & 15; t.b = ((x >> 8) & 1) << 3 | (y & 7);
                vdp++;
            }
        } else {
            next_h += 8; t.hclk = !t.hclk; h_rise = t.hclk;
        }
        t.eval();
        if (!h_rise) continue;
        // фронт hclk: выходы — точка px строки line
        if (t.de && !de_r) {
            line = t.vy; px = 0;
            if (line == 0) {                                    // новый кадр HDMI
                if (frame_ok) locked_frames++;
                frame_ok = t.locked && hframe >= 3;
                hframe++;
            }
        }
        if (t.de && frame_ok) {
            int exp = 0;
            if (line >= Y0 && line < Y0 + H && px >= X0 && px < X0 + W) {
                const int sx = (px - X0) * 320 / W, sy = (line - Y0) * SH / H;
                const int r = sx & 15, g = (sx >> 4) & 15, b = ((sx >> 8) & 1) << 3 | (sy & 7);
                exp = (r * 17) << 16 | (g * 17) << 8 | (b * 17);
            }
            checked++;
            if ((int)t.rgb != exp) {
                if (bad++ < 8) std::printf("  строка %d точка %d: %06X, ждали %06X\n", line, px, (unsigned)t.rgb, (unsigned)exp);
            } else if (exp) lit++;
        }
        if (t.de) px++;
        de_r = t.de;
    }
    std::printf("%s: %s %d строк, размер %d, 16:9 %d (W %d X0 %d, H %d Y0 %d): точек сверено %ld, в окне верных %ld, ошибок %ld, кадров с захватом %d\n",
                bad == 0 && checked > 0 ? "ВЕРНО" : "ОШИБКА", pal ? "PAL" : "NTSC", SH, size, wide, W, X0, H, Y0, checked, lit, bad, locked_frames);
    return bad == 0 && checked > 0 ? 0 : 1;
}
