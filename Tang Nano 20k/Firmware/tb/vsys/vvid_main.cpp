// Стенд Verilator ядра видео целиком (vvid_top: ts_fb_core HAS_VIDEO + потактовая SDRAM).
//
// Хост — модель плагина FTVIEW4 по выводам SPI FT812 (режим 0, SCK 14 МГц): REG_ID, сброс
// REG_CPURESET, блок состояния, подача AVI в медиа-FIFO (REG_MEDIAFIFO_READ → блок в окно
// 0xE0000 → REG_MEDIAFIFO_WRITE). Выход 720p50 снимается кадр за кадром; каждый выведенный кадр
// сверяется точка в точку с моделью развёртки (fb_scanout: окно 4:3 100 % или 16:9 у холста 16:9, класс 512 —
// ×15/8, сглаживание по долям) от эталона кадра ролика; каждый кадр ролика должен выводиться
// ровно два выходных кадра (25 кадров/с на 50 Гц), повтор (пустой чанк) — ещё два.
//
//   vvid.exe поток.avi ожидание.txt [--dump КАТАЛОГ]
//
// ожидание.txt — как у tb_vpc (make_vp_avi.py): строки «кадр ЭТАЛОН.rgb» / «повтор», последняя —
// «отменено N». --dump — снимки выхода (PPM) по кадрам ролика.
#include "Vvvid_top.h"
#include "verilated.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

double sc_time_stamp() { return 0; }

constexpr double PS_MEM = 1e6 / 61.875;     // период clk_mem, пс
constexpr double PS_PIX = 1e6 / 74.25;      // период pixel_clk, пс
constexpr double SPI_HP = 1e6 / 14.0 / 2;   // полпериода SCK 14 МГц, пс

static std::vector<uint8_t> load(const std::string &p)
{
    std::vector<uint8_t> v;
    std::FILE *f = std::fopen(p.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "нет %s\n", p.c_str()); std::exit(2); }
    for (int c; (c = std::fgetc(f)) != EOF;) v.push_back((uint8_t)c);
    std::fclose(f);
    return v;
}

// --- модель развёртки (fb_scanout, ft = 1; как scripts/ft_scan_check.py) ----------------------
struct Frac { int nx, dx, ny, dy; };
static const int WIN[2][3][4] = {{{160, 1120, 0, 720}, {184, 1096, 18, 702}, {208, 1072, 36, 684}},
                                 {{0, 1280, 0, 720}, {32, 1248, 18, 702}, {64, 1216, 36, 684}}};
// холсты 4:3 — cls 0: 1024×768, 1: 640×480, 2: 512×384 [asp][size] (fracs43 в fb_scanout);
// холст 16:9 той же ширины — x оттуда же, y — по его высоте (288, 360, 576), WY[cls][size]
static const Frac F43[3][2][3] = {
    {{{16, 15, 16, 15}, {64, 57, 64, 57}, {32, 27, 32, 27}}, {{4, 5, 16, 15}, {16, 19, 64, 57}, {8, 9, 32, 27}}},
    {{{2, 3, 2, 3}, {40, 57, 40, 57}, {20, 27, 20, 27}}, {{1, 2, 2, 3}, {10, 19, 40, 57}, {5, 9, 20, 27}}},
    {{{8, 15, 8, 15}, {32, 57, 32, 57}, {16, 27, 16, 27}}, {{2, 5, 8, 15}, {8, 19, 32, 57}, {4, 9, 16, 27}}}};
static const int WY[3][3][2] = {{{4, 5}, {16, 19}, {8, 9}}, {{1, 2}, {10, 19}, {5, 9}}, {{2, 5}, {8, 19}, {4, 9}}};
static int recip(int n) { return (65536 + n / 2) / n; }
static void dda(int &acc, int n, int d, int &st)
{
    int s = acc + n;
    if (s >= 2 * d) { st = 2; acc = s - 2 * d; } else if (s >= d) { st = 1; acc = s - d; } else { st = 0; acc = s; }
}
static int over_part(int acc, int n, int d)
{
    int o = acc + n;
    return o >= 2 * d ? o - 2 * d : o > d ? o - d : 0;
}
static int weight(int part, int rc) { return part == 0 ? 0 : std::min(16, (part * rc + 2048) >> 12); }

// эталон кадра ролика (RGB888, w × h) → ожидаемый выход 1280 × 720 (RGB888)
static std::vector<uint32_t> model(const std::vector<uint8_t> &rgb, int w, int h, int asp, int size)
{
    auto px = [&](int x, int y, int c) -> int {
        if (x >= w) x = w - 1;                     // за краем — вес 0, значение не важно
        const uint8_t *p = &rgb[3 * ((size_t)y * w + x)];
        int r5 = p[0] >> 3, g6 = p[1] >> 2, b5 = p[2] >> 3;
        int v[3] = {(r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2)};
        return v[c];
    };
    // окно — по ASPECT и SIZE (1.39), холст растянут на него; холст 16:9 (высота меньше 3/4
    // ширины) — шаг по y по его высоте
    const bool wide = h * 4 < w * 3;
    const int cls = w <= 512 ? 2 : w <= 640 ? 1 : 0;
    const int *wn = WIN[asp][size];
    Frac f = F43[cls][asp][size];
    if (wide) { f.ny = WY[cls][size][0]; f.dy = WY[cls][size][1]; }
    int rx = recip(f.nx), ry = recip(f.ny);
    std::vector<uint32_t> out(1280 * 720, 0);
    int sy = 0, sya = 0;
    for (int v = 0; v < 720; v++) {
        if (v < wn[2] || v >= wn[3]) continue;
        int st;
        if (v > wn[2]) { dda(sya, f.ny, f.dy, st); sy += st; }
        int wy = weight(over_part(sya, f.ny, f.dy), ry);
        int syb = sy >= h - 1 ? sy : sy + 1;
        int sx = 0, sxa = 0;
        for (int x = wn[0]; x < wn[1]; x++) {
            if (x > wn[0]) { dda(sxa, f.nx, f.dx, st); sx += st; }
            int wx = weight(over_part(sxa, f.nx, f.dx), rx);
            uint32_t o = 0;
            for (int c = 0; c < 3; c++) {
                int tl = px(sx, sy, c), tr = px(sx + 1, sy, c), bl = px(sx, syb, c), br = px(sx + 1, syb, c);
                int top = 16 * tl + (tr - tl) * wx, bot = 16 * bl + (br - bl) * wx;
                o |= (uint32_t)(((16 * top + (bot - top) * wy + 128) >> 8) & 255) << (16 - 8 * c);
            }
            out[(size_t)v * 1280 + x] = o;
        }
    }
    return out;
}

struct Sim {
    Vvvid_top *top;
    double now = 0, t_mem = PS_MEM / 2, t_pix = PS_PIX / 2 + 1000.0;
    // захват выхода
    std::vector<uint32_t> cur = std::vector<uint32_t>(1280 * 720, 0);
    int x = 0, y = -1;
    bool prev_vs = false, prev_de = false;
    long out_frames = 0;
    struct Out { long n; unsigned front; bool showing; std::vector<uint32_t> img; };
    std::vector<Out> outs;
    // объявления кадров: буфер → номер объявленного кадра
    unsigned last_slots = 0;
    struct Pub { double t; unsigned slot; bool frame; unsigned buf, w, h; long out_at; };
    std::vector<Pub> pubs;
    int pend = 0;
    uint32_t last_frames_seen = 0;
    // flash Tang: только чтение 0x03 (fb_flash после сброса читает сектор настроек 0x7FF000);
    // режим 0 — MOSI берётся по фронту SCK, MISO меняется по спаду
    std::vector<uint8_t> fl_set = std::vector<uint8_t>(4096, 0xFF);
    bool fl_sck = false, fl_cs = true;
    int fl_bit = 0, fl_byte = 0;
    uint8_t fl_in = 0, fl_out = 0xFF, fl_cmd = 0;
    uint32_t fl_addr = 0;
    uint8_t fl_rd(uint32_t a) { return (a & 0xFFF000) == 0x7FF000 ? fl_set[a & 0xFFF] : 0xFF; }
    void flash_step()
    {
        bool cs = top->flash_cs_n, sck = top->flash_sck;
        if (cs) { fl_cs = true; fl_sck = sck; top->flash_miso = 1; return; }
        if (fl_cs) { fl_cs = false; fl_bit = fl_byte = 0; fl_addr = 0; fl_out = 0xFF; }
        if (sck && !fl_sck) {
            fl_in = uint8_t(fl_in << 1 | (top->flash_mosi & 1));
            if (++fl_bit == 8) {
                fl_bit = 0;
                if (fl_byte == 0) fl_cmd = fl_in;
                else if (fl_byte <= 3) fl_addr = fl_addr << 8 | fl_in;
                ++fl_byte;
                if (fl_cmd == 0x03 && fl_byte >= 4) fl_out = fl_rd(fl_addr++);
            }
        } else if (!sck && fl_sck) {
            top->flash_miso = (fl_out >> 7) & 1;
            fl_out = uint8_t(fl_out << 1);
        }
        fl_sck = sck;
    }

    explicit Sim(VerilatedContext *ctx) : top(new Vvvid_top(ctx))
    {
        top->clk_mem = 0; top->pixel_clk = 0; top->rst_n = 0;
        top->ft_sck = 0; top->ft_cs_n = 1; top->ft_mosi = 0;
        top->blank = 1;
        top->flash_miso = 1;
        top->eval();
    }
    ~Sim() { top->final(); delete top; }

    void on_mem()
    {
        if (top->st_slots != last_slots) {
            last_slots = top->st_slots;
            pubs.push_back(Pub{now, last_slots, false, 0, 0, 0, out_frames});
            pend = 4;
        }
        if (pend && --pend == 0) {
            // кадр объявлен — сменились объявленный буфер или его размер; иначе повтор
            Pub &p = pubs.back();
            static unsigned pv_buf = 99, pv_w = 0, pv_h = 0, pv_n = 0;
            unsigned n = (unsigned)top->pub_v * 1000 + top->pub_buf;
            (void)pv_n;
            if (n != pv_buf || top->pub_w != pv_w || top->pub_h != pv_h || pubs.size() == 1) p.frame = true;
            p.buf = top->pub_buf; p.w = top->pub_w; p.h = top->pub_h;
            pv_buf = n; pv_w = top->pub_w; pv_h = top->pub_h;
        }
    }
    void on_pix()
    {
        bool vs = top->vid_vs, de = top->vid_de;
        if (vs && !prev_vs) {
            if (y >= 0) outs.push_back(Out{out_frames, top->fb_front, (bool)top->fb_showing, cur});
            out_frames++;
            y = -1;
            std::fill(cur.begin(), cur.end(), 0);
        }
        if (de) {
            if (!prev_de) { y++; x = 0; }
            if (y >= 0 && y < 720 && x < 1280)
                cur[(size_t)y * 1280 + x] = (uint32_t)top->vid_r << 16 | (uint32_t)top->vid_g << 8 | top->vid_b;
            x++;
        }
        prev_vs = vs; prev_de = de;
    }
    void run_to(double t)
    {
        while (true) {
            double tn = std::min(t_mem, t_pix);
            if (tn > t) break;
            now = tn;
            if (t_mem <= t_pix) {
                top->clk_mem = !top->clk_mem;
                t_mem += PS_MEM / 2;
                top->eval();
                if (top->clk_mem) on_mem();
            } else {
                top->pixel_clk = !top->pixel_clk;
                t_pix += PS_PIX / 2;
                top->eval();
                flash_step();
                if (top->pixel_clk) on_pix();
            }
        }
        now = t;
    }
    void idle(double dt) { run_to(now + dt); }
};

// SPI хоста (режим 0, старший бит первым; MISO ядро выставляет по фронту SCK)
struct Spi {
    Sim &sim;
    void select() { sim.top->ft_cs_n = 0; sim.top->eval(); sim.idle(200000.0); }
    void deselect() { sim.idle(200000.0); sim.top->ft_cs_n = 1; sim.top->ft_mosi = 0; sim.top->eval(); sim.idle(300000.0); }
    uint8_t byte(uint8_t out)
    {
        uint8_t in = 0;
        for (int b = 7; b >= 0; --b) {
            sim.top->ft_mosi = (out >> b) & 1;
            sim.top->eval();
            sim.idle(SPI_HP);
            in = uint8_t((in << 1) | (sim.top->ft_miso & 1));
            sim.top->ft_sck = 1;
            sim.top->eval();
            sim.idle(SPI_HP);
            sim.top->ft_sck = 0;
            sim.top->eval();
        }
        return in;
    }
    void write(uint32_t addr, const uint8_t *d, size_t n)
    {
        select();
        byte(uint8_t(0x80 | ((addr >> 16) & 0x3F))); byte(uint8_t(addr >> 8)); byte(uint8_t(addr));
        for (size_t i = 0; i < n; ++i) byte(d[i]);
        deselect();
    }
    void write32(uint32_t addr, uint32_t v)
    {
        uint8_t d[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
        write(addr, d, 4);
    }
    void write8(uint32_t addr, uint8_t v) { write(addr, &v, 1); }
    uint32_t read32(uint32_t addr)
    {
        select();
        byte(uint8_t((addr >> 16) & 0x3F)); byte(uint8_t(addr >> 8)); byte(uint8_t(addr));
        byte(0);
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(byte(0)) << (8 * i);
        deselect();
        return v;
    }
};

int main(int argc, char **argv)
{
    if (argc < 3) { std::fprintf(stderr, "vvid.exe поток.avi ожидание.txt [--dump КАТАЛОГ]\n"); return 2; }
    std::string dump;
    for (int i = 3; i + 1 < argc; i++) if (!std::strcmp(argv[i], "--dump")) dump = argv[i + 1];
    std::vector<uint8_t> avi = load(argv[1]);
    std::vector<std::string> exp;
    {
        std::ifstream f(argv[2]);
        for (std::string l; std::getline(f, l);) if (!l.empty()) exp.push_back(l);
    }
    std::string last_exp = exp.back();
    exp.pop_back();

    VerilatedContext ctx;
    ctx.commandArgs(argc, argv);
    Sim sim(&ctx);
    Spi spi{sim};
    // --settings ASPECT SIZE: во flash — ячейка настроек слота 0 (fb_flash: «VDST», слово 1 —
    // формат 1, [16] ASPECT, [18:17] SIZE; слово 7 — ~сумма). Ядро видео (1.39) берёт из неё
    // ASPECT и SIZE: окно слота 0, холст растянут на него (в 1.37–1.38 — только SIZE)
    int set_size = 0, set_asp = 0;
    for (int i = 3; i + 2 < argc; i++)
        if (!std::strcmp(argv[i], "--settings")) {
            uint32_t asp = (uint32_t)std::atoi(argv[i + 1]), sz = (uint32_t)std::atoi(argv[i + 2]);
            uint32_t w[8] = {0x56445354u, (1u << 8) | (asp << 16) | (sz << 17), 0, 0, 0, 0, 0, 0}, sum = 0;
            for (int k = 0; k < 7; k++) sum += w[k];
            w[7] = ~sum;
            for (int k = 0; k < 8; k++)
                for (int b = 0; b < 4; b++) sim.fl_set[4 * k + b] = uint8_t(w[k] >> (24 - 8 * b));
            set_size = (int)sz;
            set_asp = (int)asp;
            std::printf("flash: настройки слота 0 — ASPECT %u, SIZE %u\n", asp, sz);
        }
    sim.idle(200000.0);
    sim.top->rst_n = 1;
    while (!sim.top->mem_ready && sim.now < 5e9) sim.idle(100000.0);
    std::printf("SDRAM готова: %.2f мс\n", sim.now / 1e9);

    // --boot: правило тишины (в стенде 2 мс). Ядро загрузила ESP, M_SEL = 0, хост молчит —
    // ядро не уходит в слот 0 (плагин ждёт готовности ядра); после первого обмена хоста при
    // M_SEL = 0 и тишине — уходит (сброс Evo).
    for (int i = 3; i < argc; i++)
        if (!std::strcmp(argv[i], "--boot")) {
            sim.top->blank = 0;
            sim.idle(6e9);
            bool early = sim.top->boot_req;
            std::printf("M_SEL = 0, хоста не было 6 мс: возврат в слот 0 %s\n", early ? "ЕСТЬ" : "нет");
            uint32_t id = spi.read32(0x302000) & 0xFF;
            bool mid = sim.top->boot_req;
            sim.idle(6e9);
            bool late = sim.top->boot_req;
            std::printf("обмен хоста (REG_ID %02X), затем тишина 6 мс: возврат %s\n", id, late ? "есть" : "НЕТ");
            bool ok = !early && !mid && late && id == 0x7C;
            std::printf("%s\n", ok ? "ВСЁ ВЕРНО" : "ЕСТЬ ОШИБКИ");
            return ok ? 0 : 1;
        }

    int bad = 0;
    uint32_t id = spi.read32(0x302000) & 0xFF;
    if (id != 0x7C) { std::printf("REG_ID %02X, а не 7C\n", id); bad++; }
    spi.write8(0x302020, 1);
    spi.write8(0x302020, 0);
    uint32_t magic = spi.read32(0x000000), size = spi.read32(0x000004);
    std::printf("состояние %08X, FIFO %u байтов\n", magic, size);
    if (magic != 0x31444956 || !size || (size & (size - 1))) { std::printf("блок состояния неверен\n"); return 1; }

    uint32_t wpos = 0;
    size_t pos = 0;
    double t0 = sim.now;
    while (pos < avi.size()) {
        uint32_t rpos = spi.read32(0x309014);
        uint32_t free = (rpos - wpos - 4) & (size - 1);
        size_t count = std::min<size_t>(avi.size() - pos, 512);
        uint32_t padded = (uint32_t)(count + 3) & ~3u;
        if (padded > size - wpos) padded = size - wpos, count = padded;
        if (free < padded) { sim.idle(200000000.0); continue; }   // 0,2 мс
        std::vector<uint8_t> blk(padded, 0);
        std::memcpy(blk.data(), &avi[pos], std::min<size_t>(count, avi.size() - pos));
        spi.write(0xE0000 + wpos, blk.data(), padded);
        wpos = (wpos + padded) & (size - 1);
        spi.write32(0x309018, wpos);
        pos += count;
    }
    std::printf("поток подан за %.1f мс\n", (sim.now - t0) / 1e9);
    // все слоты и ещё 4 выходных кадра
    double lim = sim.now + 1e12;
    while (sim.pubs.size() < exp.size() && sim.now < lim) sim.idle(1e9);
    long stop_at = sim.out_frames + 4;
    while (sim.out_frames < stop_at) sim.idle(1e9);
    uint32_t aborts = spi.read32(0x000010);
    std::printf("слотов %zu, выходных кадров %ld, отменено %u\n", sim.pubs.size(), sim.out_frames, aborts);

    // сверка: объявленный кадр k — в выходных кадрах, где выводится его буфер, до следующего
    std::vector<int> shown(sim.pubs.size(), 0);
    size_t cmp_frames = 0;
    for (const Sim::Out &o : sim.outs) {
        if (!o.showing) continue;
        // последний кадр, объявленный до начала этого выходного кадра, с этим буфером
        int k = -1;
        for (size_t i = 0; i < sim.pubs.size(); i++)
            if (sim.pubs[i].frame && sim.pubs[i].out_at < o.n && sim.pubs[i].buf == o.front) k = (int)i;
        if (k < 0) continue;
        // слот, к которому относится этот выходной кадр: последний слот до него
        int s = -1;
        for (size_t i = 0; i < sim.pubs.size(); i++) if (sim.pubs[i].out_at < o.n) s = (int)i;
        if (s >= 0) shown[s]++;
        const Sim::Pub &p = sim.pubs[k];
        // «кадр ЭТАЛОН ШИРИНА ВЫСОТА»: холст как в vp_fbwrite — картинка по центру, поля чёрные
        char path[1024];
        unsigned w = 0, h = 0;
        std::sscanf(exp[k].c_str() + strlen("кадр "), "%1023s %u %u", path, &w, &h);
        std::vector<uint8_t> ref = load(path);
        // холст — по правилу vp_fbwrite 1.36: широкая картинка (9·w ≥ 14·h) — на 16:9
        const bool wd = 9 * w >= 14 * h && h <= 576;
        unsigned ecw = wd ? (w <= 512 && h <= 288 ? 512 : w <= 640 && h <= 360 ? 640 : 1024)
                          : (w <= 512 && h <= 384 ? 512 : w <= 640 && h <= 480 ? 640 : 1024);
        unsigned ech = wd ? (ecw == 512 ? 288 : ecw == 640 ? 360 : 576) : (ecw == 512 ? 384 : ecw == 640 ? 480 : 768);
        if (p.w != ecw || p.h != ech) {
            std::printf("кадр ролика %d (%ux%u): холст %ux%u вместо %ux%u\n", k, w, h, p.w, p.h, ecw, ech);
            bad++;
            continue;
        }
        unsigned ox = ((p.w - w) / 2) & ~15u, oy = (p.h - h) / 2;
        std::vector<uint8_t> canvas((size_t)p.w * p.h * 3, 0);
        for (unsigned yy = 0; yy < h; yy++)
            std::memcpy(&canvas[3 * ((size_t)(yy + oy) * p.w + ox)], &ref[3 * (size_t)yy * w], 3 * (size_t)w);
        std::vector<uint32_t> m = model(canvas, (int)p.w, (int)p.h, set_asp, set_size);
        size_t diff = 0, first = (size_t)-1;
        for (size_t i = 0; i < m.size(); i++) if (m[i] != o.img[i]) { if (first == (size_t)-1) first = i; diff++; }
        cmp_frames++;
        if (diff) {
            std::printf("выходной кадр %ld (кадр ролика %d, буфер %u): расхождений %zu, первое x %zu y %zu: %06X вместо %06X\n",
                        o.n, k, o.front, diff, first % 1280, first / 1280, o.img[first], m[first]);
            bad++;
        }
        if (!dump.empty()) {
            std::string fn = dump + "/out_" + std::to_string(o.n) + ".ppm";
            if (std::FILE *f = std::fopen(fn.c_str(), "wb")) {
                std::fprintf(f, "P6 1280 720 255\n");
                for (uint32_t v : o.img) { uint8_t c[3] = {uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; std::fwrite(c, 1, 3, f); }
                std::fclose(f);
            }
        }
    }
    for (size_t i = 0; i < sim.pubs.size(); i++) {
        bool want_frame = i < exp.size() && exp[i].rfind("кадр ", 0) == 0;
        std::printf("слот %2zu: %s, объявлен перед выходным кадром %ld, выведен %d раз%s\n", i,
                    sim.pubs[i].frame ? "кадр  " : "повтор", sim.pubs[i].out_at, shown[i],
                    sim.pubs[i].frame != want_frame ? " — НЕ ТОТ ВИД" : "");
        if (sim.pubs[i].frame != want_frame) bad++;
        if (i + 1 < sim.pubs.size() && shown[i] != 2) { std::printf("  — темп: ждали 2\n"); bad++; }
    }
    unsigned want_ab = (unsigned)std::atoi(last_exp.c_str() + last_exp.rfind(' ') + 1);
    if (aborts != want_ab) { std::printf("отменено %u, ждали %u\n", aborts, want_ab); bad++; }
    std::printf("сверено выходных кадров: %zu\n%s\n", cmp_frames, bad ? "ЕСТЬ ОШИБКИ" : "ВСЁ ВЕРНО");
    return bad ? 1 : 0;
}
