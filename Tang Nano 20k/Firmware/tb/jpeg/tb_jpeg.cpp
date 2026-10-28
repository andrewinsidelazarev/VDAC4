// Стенд Verilator декодера JPEG (third_party/core_jpeg, jpeg_core): файл JPEG — потоком AXI по
// 32 бита (младший байт первым, как ждёт jpeg_input), точки RGB по координатам — в картинку
// RGB888 W×H. Такты — от первого слова до последней точки картинки.
//
//   tb_jpeg.exe кадр.jpg выход.rgb
//
// Печатает: «ширина высота тактов точек» (точки за краем картинки — блоки 8×8 — не в счёт).
#include "Vjpeg_core.h"
#include "verilated.h"
#ifdef TRACE
#include "verilated_vcd_c.h"
#endif

#include <cstdint>
#include <cstdio>
#include <vector>

double sc_time_stamp() { return 0; }

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "tb_jpeg кадр.jpg выход.rgb\n");
        return 2;
    }
    std::FILE *f = std::fopen(argv[1], "rb");
    if (!f)
        return 2;
    std::vector<uint8_t> in;
    for (int c; (c = std::fgetc(f)) != EOF;)
        in.push_back((uint8_t)c);
    std::fclose(f);

    Verilated::commandArgs(argc, argv);
    Vjpeg_core t;
#ifdef TRACE
    // трасса VCD: tb_jpeg кадр.jpg выход.rgb трасса.vcd
    Verilated::traceEverOn(true);
    VerilatedVcdC vcd;
    t.trace(&vcd, 99);
    vcd.open(argc > 3 ? argv[3] : "tb_jpeg.vcd");
    uint64_t tt = 0;
#define DUMP() vcd.dump(tt++)
#else
#define DUMP()
#endif
    t.clk_i = 0;
    t.rst_i = 1;
    t.inport_valid_i = 0;
    t.outport_accept_i = 1;
    for (int i = 0; i < 8; i++) {
        t.clk_i = 0; t.eval();
        t.clk_i = 1; t.eval();
    }
    t.rst_i = 0;

    size_t pos = 0;
    uint64_t cyc = 0, last_px = 0, got = 0, idle_run = 0;
    unsigned w = 0, h = 0;
    std::vector<uint8_t> img;
    std::vector<uint8_t> seen;
    while (cyc < 20000000ull) {
        t.clk_i = 0;
        if (pos < in.size()) {
            uint32_t word = 0, strb = 0;
            for (int k = 0; k < 4 && pos + k < in.size(); k++) {
                word |= (uint32_t)in[pos + k] << (8 * k);
                strb |= 1u << k;
            }
            t.inport_valid_i = 1;
            t.inport_data_i = word;
            t.inport_strb_i = strb;
            t.inport_last_i = 0;   // конец кадра — по EOI: с last в последнем слове jpeg_input теряет FF перед D9
        } else {
            t.inport_valid_i = 0;
        }
        t.eval();
        DUMP();
        const bool in_fire = t.inport_valid_i && t.inport_accept_o;
        if (t.outport_valid_o && t.outport_accept_i) {
            if (!w) {
                w = t.outport_width_o;
                h = t.outport_height_o;
                img.assign((size_t)w * h * 3, 0);
                seen.assign((size_t)w * h, 0);
            }
            const unsigned x = t.outport_pixel_x_o, y = t.outport_pixel_y_o;
            if (x < w && y < h) {
                const size_t i = (size_t)y * w + x;
                img[3 * i] = t.outport_pixel_r_o;
                img[3 * i + 1] = t.outport_pixel_g_o;
                img[3 * i + 2] = t.outport_pixel_b_o;
                if (!seen[i]) {
                    seen[i] = 1;
                    got++;
                    last_px = cyc;
                }
            }
        }
        t.clk_i = 1;
        t.eval();
        DUMP();
        if (in_fire)
            pos += 4;
        cyc++;
        if (w && got == (uint64_t)w * h)
            break;
        idle_run = (pos >= in.size() && t.idle_o) ? idle_run + 1 : 0;
        if (idle_run > 100000)
            break;
    }
#ifdef TRACE
    vcd.close();
#endif
    f = std::fopen(argv[2], "wb");
    if (f) {
        std::fwrite(img.data(), 1, img.size(), f);
        std::fclose(f);
    }
    std::printf("%u %u %llu %llu\n", w, h, (unsigned long long)(last_px + 1), (unsigned long long)got);
    // недошедшие точки — участками строк (первые 12): «y x0-x1»
    int runs = 0;
    for (unsigned y = 0; y < h && runs < 12; y++)
        for (unsigned x = 0; x < w && runs < 12; x++)
            if (!seen[(size_t)y * w + x]) {
                unsigned x1 = x;
                while (x1 + 1 < w && !seen[(size_t)y * w + x1 + 1])
                    x1++;
                std::fprintf(stderr, "нет точек: y %u x %u-%u\n", y, x, x1);
                runs++;
                x = x1;
            }
    return w && got == (uint64_t)w * h ? 0 : 1;
}
