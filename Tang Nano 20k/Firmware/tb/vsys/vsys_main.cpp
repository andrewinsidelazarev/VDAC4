// Стенд Verilator ядра FT812 (tb/vsys): ts_fb_core слота 1 + SDRAM + вывод 720p60, хост Evo —
// здесь: игра в замкнутом цикле по REG_DLSWAP, как R-Type (Source/ASM/v30z80_host.asm,
// v30z80_runtime.asm) на плате.
//
// Шаг игры k (запись трассы, tb/vsys/rtype_trace.py):
//   1. HostVideoSkip: REG_FRAMES по SPI; долг кадров развёртки → шаг без вывода (не больше одного
//      подряд, долг 0…2);
//   2. с выводом — HostVideoBefore (t_v тактов Z80);
//   3. логика машины и звук (t_l);
//   4. с выводом — FrameEmit: опрос REG_DLSWAP (раз в 15 мкс) до 0, лист в RAM_DL по SPI
//      (28 МГц подряд), остаток t_e, REG_DLSWAP = 2 (FRAME).
// Такты Z80 — из эмулятора R-Type (там FT812 отвечает сразу: ожиданий в тактах нет).
//
// Замеры: шаги игры в секунду (полная скорость — 59,08), выводы (смены листа), кадры
// растеризатора и их время (FT_RTIME), кадры, взятые выводом, ожидание хоста на DLSWAP.
//
// vsys.exe --trace T.bin --ramg R.bin [--seconds S] [--steps N] [--csv steps.csv] [--z80 14]
//          [--hex M.hex] [--loop] [--fixed-dl] [--no-skip] [--dump F.hex] [--quiet]
//   --ref F.hex — эталон кадра (как --dump): каждый объявленный кадр сверяется с ним целиком через
//           10 мкс после объявления (с SD_FAST растеризатор отдаёт последние слова в очередь моста,
//           и в SDRAM они попадают позже; вывод читает буфер после смены листа, через тот же мост)
//   --dump — в конце выводимый кадр FT из SDRAM: 768 строк × 512 слов RGB565 (младшая половина —
//           левая точка), слово на строку hex, как ft_frame.hex tb_ft_render (ft_rtl_check.py frame)
//   --hex — память SDRAM словами в hex ($readmemh: строка — слово, @адрес — адрес слова), например
//           RAM_G и шрифты ПЗУ кадра tb_ft_render (frame_ramg.hex); --no-skip — без HostVideoSkip
//           (каждый шаг с выводом: игра, которая ждёт смены листа, как Zuma Deluxe)
//   --ramg-wr N [--ramg-at A] — на каждом шаге в начале логики машины хост пишет N байт RAM_G
//           подряд (SPI 28 МГц): узор по кругу в окне A…0xFFFFF (по умолчанию 0x80000 — у сцен
//           DOOM и DEMO3D там пусто; окно должно быть вне данных сцены, иначе кадр разойдётся с
//           моделью). Очередь хоста к RAM_G нагружена, как у Zuma и прогноза погоды на плате
//           (FT_STAT 25 у ядра 1.27). В конце окно в SDRAM сверяется с записанным, печатаются
//           FT_STAT 25 и 26 (чтением по SPI не сверяем: на 28 МГц первые байты посылки чтения
//           промахом мимо кэша опаздывают — так и задумано, см. tb_ft_ramg)
#include "Vvsys_top.h"
#include "Vvsys_top___024root.h"
#include "verilated.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

double sc_time_stamp() { return 0.0; }        // время ведёт стенд сам (VerilatedContext не нужен)

namespace {

constexpr double PS_MEM = 1e6 / 61.875;     // период clk_mem, пс
#ifdef VSYS_768
constexpr double PS_PIX = 1e6 / 72.0;       // период pixel_clk, пс: 1366×768 RB (ядро с 1.41)
#else
constexpr double PS_PIX = 1e6 / 74.25;      // период pixel_clk, пс
#endif
constexpr double SPI_HP = 1e6 / 28.0 / 2;   // полупериод SCK 28 МГц, пс
constexpr double MS = 1e9;                  // пс в мс

constexpr uint32_t RAM_DL = 0x300000, REG_FRAMES = 0x302004, REG_HCYCLE = 0x30202C,
                   REG_HOFFSET = 0x302030, REG_HSIZE = 0x302034, REG_HSYNC0 = 0x302038,
                   REG_HSYNC1 = 0x30203C, REG_VCYCLE = 0x302040, REG_VOFFSET = 0x302044,
                   REG_VSIZE = 0x302048, REG_VSYNC0 = 0x30204C, REG_VSYNC1 = 0x302050,
                   REG_DLSWAP = 0x302054, REG_PCLK = 0x302070;

struct Step {
    uint32_t frame, t_v, t_l, t_e;
    std::vector<uint32_t> dl;
};

struct Stats {
    // события ядра (время, пс)
    std::vector<double> swaps, takes, renders;
    std::vector<uint16_t> rtimes;
    // сверка объявленных кадров с эталоном
    long ref_frames = 0, ref_bad = 0, ref_words = 0;
    int ref_y0 = 9999, ref_y1 = -1, ref_x0 = 9999, ref_x1 = -1;
};

// база половины кадра b, как ft_render.lbase (FB_BASE ts_fb_core — 0x128000)
static uint32_t lbase(unsigned b, bool hi) {
    if (b == 0) return 0x128000u + (hi ? 0x40000u : 0u);
    if (b == 1) return 0x188000u + (hi ? 0x40000u : 0u);
    return hi ? 0x0C9000u : 0x080000u;
}

class Sim {
public:
    Vvsys_top* top;
    double now = 0.0;
    Stats st;
    uint32_t last_swaps = 0, last_takes = 0;
    uint16_t last_rf = 0;
    static constexpr double REF_DELAY = 10e6;   // 10 мкс, пс
    // плитки 64×16, изменившиеся от прошлого объявленного кадра (сверка — вместе с эталоном)
    std::vector<uint32_t> prev;                  // прошлый объявленный кадр, 768 × 512 слов
    long td_frames = 0, td_sum = 0, td_min = 1 << 30, td_max = 0, td_zero = 0;
    long td_hist[8] = {0};                       // 0, 1–16, 17–64, 65–192, 193–384, 385–576, 577–767, 768
    double ref_at = -1.0;                        // когда сверить объявленный кадр (−1 — нечего)
    unsigned ref_buf = 0;
    std::vector<uint32_t> ref;              // эталон кадра (--ref), 768 × 512 слов

    explicit Sim(VerilatedContext* ctx) : top(new Vvsys_top(ctx)) {
        top->clk_mem = 0; top->clk_sd = 0; top->pixel_clk = 0; top->rst_n = 0;
        top->ft_sck = 0; top->ft_cs_n = 1; top->ft_mosi = 0;
        top->eval();
    }
    ~Sim() { top->final(); delete top; }

    // Время вперёд до t: фронты клоков по порядку. clk_sd = 2 × clk_mem: переключается на
    // каждом шаге PS_MEM / 4, clk_mem — на каждом втором, вместе с ним (фронты совпадают)
    void run_to(double t) {
        while (true) {
            double tn = std::min(t_sd, t_pix);
            if (tn > t) break;
            now = tn;
            if (t_sd <= t_pix) {
                bool mem_edge = (e_sd & 1) == 0;
                top->clk_sd = !top->clk_sd;
                if (mem_edge) top->clk_mem = !top->clk_mem;
                ++e_sd;
                t_sd = PS_MEM / 4 * double(e_sd + 1);
                top->eval();
                if (mem_edge && top->clk_mem) observe();
            } else {
                top->pixel_clk = !top->pixel_clk;
                t_pix += PS_PIX / 2;
                top->eval();
            }
        }
        now = t;
    }
    void idle(double dt) { run_to(now + dt); }
    void pins() { top->eval(); }

private:
    double t_sd = PS_MEM / 4, t_pix = PS_PIX / 2 + 1000.0;
    uint64_t e_sd = 0;

    void check_ref(unsigned b) {
        if (st.swaps.size() < 2) return;     // до листа трассы — пустой лист, не как эталон
        auto& mem = top->rootp->vsys_top__DOT__sdram__DOT__mem;
        long bad = 0;
        for (int y = 0; y < 768; ++y) {
            uint32_t base = lbase(b, y >= 512) + uint32_t(y & 511) * 512;
            for (int w = 0; w < 512; ++w)
                if (mem[base + w] != ref[size_t(y) * 512 + w]) {
                    ++bad;
                    st.ref_y0 = std::min(st.ref_y0, y); st.ref_y1 = std::max(st.ref_y1, y);
                    st.ref_x0 = std::min(st.ref_x0, 2 * w); st.ref_x1 = std::max(st.ref_x1, 2 * w + 1);
                }
        }
        st.ref_frames++;
        if (bad) {
            if (st.ref_bad < 5)
                std::printf("  кадр %ld (буфер %u, %.3f мс): слов не как эталон %ld\n", st.ref_frames, b, now / 1e9, bad);
            st.ref_bad++;
            st.ref_words += bad;
        }
    }

    // кадр объявлен и дописан: сверка с эталоном и плитки, изменившиеся от прошлого кадра
    void settled(unsigned b) {
        if (!ref.empty()) check_ref(b);
        auto& mem = top->rootp->vsys_top__DOT__sdram__DOT__mem;
        std::vector<uint32_t> cur(768u * 512u);
        for (int y = 0; y < 768; ++y) {
            uint32_t base = lbase(b, y >= 512) + uint32_t(y & 511) * 512;
            for (int w = 0; w < 512; ++w) cur[size_t(y) * 512 + w] = mem[base + w];
        }
        if (!prev.empty() && st.swaps.size() >= 2) {
            long n = 0;
            for (int ty = 0; ty < 48; ++ty)
                for (int tx = 0; tx < 16; ++tx) {
                    bool d = false;
                    for (int y = ty * 16; y < ty * 16 + 16 && !d; ++y)
                        for (int w = tx * 32; w < tx * 32 + 32; ++w)
                            if (cur[size_t(y) * 512 + w] != prev[size_t(y) * 512 + w]) { d = true; break; }
                    n += d;
                }
            td_frames++; td_sum += n; td_min = std::min(td_min, n); td_max = std::max(td_max, n);
            td_zero += n == 0;
            int h = n == 0 ? 0 : n <= 16 ? 1 : n <= 64 ? 2 : n <= 192 ? 3 : n <= 384 ? 4 : n <= 576 ? 5 : n < 768 ? 6 : 7;
            td_hist[h]++;
        }
        prev.swap(cur);
    }

    void observe() {
        if (top->r_frames != last_rf) {
            last_rf = top->r_frames;
            st.renders.push_back(now);
            st.rtimes.push_back(top->r_time);
            if (ref_at >= 0.0) settled(ref_buf);   // прошлый не дождался — сверить сейчас
            ref_at = now + REF_DELAY;
            ref_buf = top->r_pub_buf;
        }
        if (ref_at >= 0.0 && now >= ref_at) { settled(ref_buf); ref_at = -1.0; }
        if (top->n_swaps != last_swaps) { last_swaps = top->n_swaps; st.swaps.push_back(now); }
        if (top->n_takes != last_takes) { last_takes = top->n_takes; st.takes.push_back(now); }
    }
};

// --- SPI хоста (режим 0, старший бит первым; MISO выставляется ядром по фронту SCK) ----------
class Spi {
public:
    explicit Spi(Sim& s) : sim(s) {}

    void select() { sim.top->ft_cs_n = 0; sim.pins(); sim.idle(40000.0); }
    void deselect() { sim.idle(40000.0); sim.top->ft_cs_n = 1; sim.top->ft_mosi = 0; sim.pins(); sim.idle(100000.0); }

    uint8_t byte(uint8_t out) {
        uint8_t in = 0;
        for (int b = 7; b >= 0; --b) {
            sim.top->ft_mosi = (out >> b) & 1;
            sim.pins();
            sim.idle(SPI_HP);
            in = uint8_t((in << 1) | (sim.top->ft_miso & 1));
            sim.top->ft_sck = 1;
            sim.pins();
            sim.idle(SPI_HP);
            sim.top->ft_sck = 0;
            sim.pins();
        }
        return in;
    }

    void write(uint32_t addr, const uint8_t* d, size_t n) {
        select();
        byte(uint8_t(0x80 | ((addr >> 16) & 0x3F))); byte(uint8_t(addr >> 8)); byte(uint8_t(addr));
        for (size_t i = 0; i < n; ++i) byte(d[i]);
        deselect();
    }
    void write32(uint32_t addr, uint32_t v) {
        uint8_t d[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
        write(addr, d, 4);
    }
    void write8(uint32_t addr, uint8_t v) { write(addr, &v, 1); }
    uint32_t read(uint32_t addr, int n) {
        select();
        byte(uint8_t((addr >> 16) & 0x3F)); byte(uint8_t(addr >> 8)); byte(uint8_t(addr));
        byte(0);                                         // пустой байт после адреса
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v |= uint32_t(byte(0)) << (8 * i);
        deselect();
        return v;
    }

private:
    Sim& sim;
};

bool load_trace(const char* path, std::vector<Step>& steps) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char magic[4];
    uint32_t n = 0;
    if (std::fread(magic, 1, 4, f) != 4 || std::memcmp(magic, "VTR1", 4) != 0 ||
        std::fread(&n, 4, 1, f) != 1) { std::fclose(f); return false; }
    steps.resize(n);
    for (auto& s : steps) {
        uint32_t h[5];
        if (std::fread(h, 4, 5, f) != 5) { std::fclose(f); return false; }
        s.frame = h[0]; s.t_v = h[1]; s.t_l = h[2]; s.t_e = h[3];
        s.dl.resize(h[4]);
        if (h[4] && std::fread(s.dl.data(), 4, h[4], f) != h[4]) { std::fclose(f); return false; }
    }
    std::fclose(f);
    return true;
}

double ms(double ps) { return ps / MS; }

}  // namespace

int main(int argc, char** argv) {
    const char *trace = nullptr, *ramg = nullptr, *csv = nullptr, *hex = nullptr, *dump = nullptr, *refp = nullptr;
    double seconds = 5.0, z80_mhz = 14.0;
    long max_steps = -1, ramg_wr = 0, ramg_at = 0x80000;
    bool loop = false, fixed_dl = false, quiet = false, no_skip = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? argv[++i] : (char*)""; };
        if (a == "--trace") trace = next();
        else if (a == "--ramg") ramg = next();
        else if (a == "--csv") csv = next();
        else if (a == "--hex") hex = next();
        else if (a == "--no-skip") no_skip = true;
        else if (a == "--dump") dump = next();
        else if (a == "--ref") refp = next();
        else if (a == "--seconds") seconds = std::atof(next());
        else if (a == "--steps") max_steps = std::atol(next());
        else if (a == "--z80") z80_mhz = std::atof(next());
        else if (a == "--loop") loop = true;
        else if (a == "--fixed-dl") fixed_dl = true;
        else if (a == "--quiet") quiet = true;
        else if (a == "--ramg-wr") ramg_wr = std::atol(next());
        else if (a == "--ramg-at") ramg_at = std::strtol(next(), nullptr, 0);
        else { std::fprintf(stderr, "неизвестный ключ %s\n", a.c_str()); return 2; }
    }
    std::vector<Step> steps;
    if (!trace || !load_trace(trace, steps) || steps.empty()) {
        std::fprintf(stderr, "нет трассы (--trace)\n");
        return 2;
    }
    const double tick = 1e6 / z80_mhz;         // такт Z80, пс

    auto ctx = new VerilatedContext;
    ctx->commandArgs(argc, argv);
    Sim sim(ctx);
    Spi spi(sim);

    // RAM_G — в SDRAM с нулевого слова
    auto& mem = sim.top->rootp->vsys_top__DOT__sdram__DOT__mem;
    // Слово RAM_G 0xC0000 сцены: ядро после сброса кладёт туда CHIPID (как FT812), а игра потом
    // затирает его своими данными — снимок RAM_G кадра взят уже после этого. Без возврата слова
    // кадр расходится с моделью (R-Type 09600: 24 точки спрайта с текселями 0xC0000…0xC0003).
    constexpr uint32_t kChipIdWord = 0xC0000 / 4;
    bool chip_have = false;
    uint32_t chip_word = 0;
    if (ramg) {
        FILE* f = std::fopen(ramg, "rb");
        if (!f) { std::fprintf(stderr, "нет %s\n", ramg); return 2; }
        uint32_t w, i = 0;
        while (std::fread(&w, 4, 1, f) == 1 && i < (1u << 20)) mem[i++] = w;
        std::fclose(f);
        chip_have = i > kChipIdWord;
        chip_word = chip_have ? mem[kChipIdWord] : 0;
    }
    if (hex) {
        FILE* f = std::fopen(hex, "r");
        if (!f) { std::fprintf(stderr, "нет %s\n", hex); return 2; }
        char line[64];
        uint32_t a = 0;
        while (std::fgets(line, sizeof line, f)) {
            if (line[0] == '@') { a = uint32_t(std::strtoul(line + 1, nullptr, 16)); continue; }
            if (line[0] == '\n' || line[0] == '\r' || line[0] == 0) continue;
            if (a < (1u << 21)) mem[a] = uint32_t(std::strtoul(line, nullptr, 16));
            ++a;
        }
        std::fclose(f);
    }

    // --ramg-wr: окно RAM_G хоста — что в нём должно оказаться (слова как в SDRAM); пока хост туда
    // не писал — что было после запуска ядра (оно само кладёт CHIPID в 0xC0000)
    const uint32_t g_lo = uint32_t(std::clamp<long>(ramg_at, 0, (1L << 20) - 4)) & ~3u, g_hi = 1u << 20;
    std::vector<uint32_t> g_exp;
    uint32_t g_at = g_lo, g_rnd = 0x12345678u;
    long g_written = 0;

    if (refp) {
        FILE* f = std::fopen(refp, "r");
        if (!f) { std::fprintf(stderr, "нет %s\n", refp); return 2; }
        char line[64];
        while (std::fgets(line, sizeof line, f))
            if (line[0] != '\n' && line[0] != '\r' && line[0]) sim.ref.push_back(uint32_t(std::strtoul(line, nullptr, 16)));
        std::fclose(f);
        if (sim.ref.size() != 768u * 512u) { std::fprintf(stderr, "эталон: слов %zu, нужно 393216\n", sim.ref.size()); return 2; }
    }

    auto wall0 = std::chrono::steady_clock::now();
    sim.idle(2e6);                             // сброс 2 мкс
    sim.top->rst_n = 1;
    sim.pins();
    while (!sim.top->mem_ready && sim.now < 2e9) sim.idle(1e6);
    if (!sim.top->mem_ready) { std::fprintf(stderr, "SDRAM не готова\n"); return 1; }
    sim.idle(1e8);                             // 100 мкс: flash, ядро после сброса
    if (chip_have) mem[kChipIdWord] = chip_word;   // CHIPID ядро уже записало: слово сцены — назад

    // Развёртка FT812 — TSLib VM_1024_768_59Hz (64 МГц, PCLK 1): 59,08 Гц
    spi.write32(REG_HCYCLE, 1344); spi.write32(REG_HOFFSET, 296); spi.write32(REG_HSYNC0, 0);
    spi.write32(REG_HSYNC1, 136); spi.write32(REG_VCYCLE, 806); spi.write32(REG_VOFFSET, 35);
    spi.write32(REG_VSYNC0, 0); spi.write32(REG_VSYNC1, 6); spi.write32(REG_HSIZE, 1024);
    spi.write32(REG_VSIZE, 768); spi.write32(REG_PCLK, 1);
    if (ramg_wr > 0)
        for (uint32_t i = g_lo >> 2; i < (g_hi >> 2); ++i) g_exp.push_back(mem[i]);
    const double t0 = sim.now;
    std::printf("старт: %.3f мс модели (SDRAM готова, развёртка задана)\n", ms(t0));

    FILE* fcsv = csv ? std::fopen(csv, "w") : nullptr;
    if (fcsv) std::fprintf(fcsv, "step,frame,skip,t_start_ms,t_emit_ms,t_go_ms,t_req_ms,wait_ms\n");

    // HostVideoSkip
    uint8_t skip_frames = 0, skip_lag = 0, skip_run = 0;
    bool video_ready = false;
    long n_steps = 0, n_out = 0, n_skip = 0;
    std::vector<double> req_times, waits;
    double next_report = t0 + 1e12;
    size_t rep_swaps = 0, rep_renders = 0, rep_takes = 0;
    long rep_steps = 0, rep_out = 0, rep_skip = 0;
    size_t k = 0;

    auto report = [&](double t_end, bool final) {
        size_t sw = sim.st.swaps.size(), rr = sim.st.renders.size(), tk = sim.st.takes.size();
        double rmax = 0, rsum = 0;
        for (size_t i = rep_renders; i < rr; ++i) {
            double v = sim.st.rtimes[i] * 1024.0 / 61875.0;
            rsum += v; rmax = std::max(rmax, v);
        }
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
        std::printf("%s%6.2f с: шагов %3ld (без вывода %2ld), выводов %3ld, смен листа %3zu, кадров FT %3zu "
                    "(%.1f/%.1f мс), взято выводом %3zu | %.0f с счёта\n",
                    final ? "ИТОГ " : "", (t_end - t0) / 1e12, n_steps - rep_steps, n_skip - rep_skip,
                    n_out - rep_out, sw - rep_swaps, rr - rep_renders,
                    rr > rep_renders ? rsum / double(rr - rep_renders) : 0.0, rmax, tk - rep_takes, secs);
        std::fflush(stdout);
        rep_swaps = sw; rep_renders = rr; rep_takes = tk;
        rep_steps = n_steps; rep_out = n_out; rep_skip = n_skip;
    };

    const double t_stop = t0 + seconds * 1e12;
    while (sim.now < t_stop && (max_steps < 0 || n_steps < max_steps)) {
        if (k >= steps.size()) { if (!loop) break; k = 1; }   // по кругу — без листа установки
        const Step& s = steps[fixed_dl ? 0 : k];
        const Step& tm = steps[k];
        double t_start = sim.now;

        // 1. HostVideoSkip
        uint8_t fr = uint8_t(spi.read(REG_FRAMES, 2));
        uint8_t prev = skip_frames;
        skip_frames = fr;
        bool skip = false;
        if (no_skip || !video_ready || k == 0) {
            skip_lag = 0; skip_run = 0;
        } else {
            int a = int(uint8_t(fr - prev)) + skip_lag;
            if (a > 2 + 1) a = 2 + 1;
            a -= 1;
            if (a < 0) a = 0;
            skip_lag = uint8_t(a);
            if (skip_lag && skip_run < 1) { skip_run++; skip = true; }
            else skip_run = 0;
        }
        // 2–3. подготовка видео и логика машины
        if (!skip) sim.idle(tm.t_v * tick);
        const double t_l0 = sim.now;
        if (ramg_wr > 0) {
            const uint32_t n = uint32_t(std::min<long>(ramg_wr, long(g_hi - g_lo)));
            if (g_at + n > g_hi) g_at = g_lo;
            std::vector<uint8_t> gb(n);
            for (uint32_t i = 0; i < n; ++i) {
                g_rnd = g_rnd * 1103515245u + 12345u;
                gb[i] = uint8_t(g_rnd >> 16);
                const uint32_t a = g_at + i, sh = 8 * (a & 3);
                uint32_t& w = g_exp[(a - g_lo) >> 2];
                w = (w & ~(0xFFu << sh)) | (uint32_t(gb[i]) << sh);
            }
            spi.write(g_at, gb.data(), n);
            g_written += n;
            g_at += n;
        }
        const double rest_l = tm.t_l * tick - (sim.now - t_l0);
        if (rest_l > 0) sim.idle(rest_l);
        double t_emit = sim.now, t_go = 0, t_req = 0;
        // 4. FrameEmit
        if (!skip) {
            while ((spi.read(REG_DLSWAP, 2) & 0xFF) != 0 && sim.now < t_stop) sim.idle(12e6);   // опрос ~15 мкс
            if (sim.now >= t_stop) break;
            t_go = sim.now;
            double t_dl = sim.now;
            std::vector<uint8_t> bytes(s.dl.size() * 4);
            std::memcpy(bytes.data(), s.dl.data(), bytes.size());
            spi.write(RAM_DL, bytes.data(), bytes.size());
            double rest = tm.t_e * tick - (sim.now - t_dl);
            if (rest > 0) sim.idle(rest);
            spi.write8(REG_DLSWAP, 2);
            t_req = sim.now;
            req_times.push_back(t_req);
            waits.push_back(t_go - t_emit);
            video_ready = true;
            n_out++;
        } else {
            n_skip++;
        }
        n_steps++;
        if (fcsv)
            std::fprintf(fcsv, "%ld,%u,%d,%.4f,%.4f,%.4f,%.4f,%.4f\n", n_steps - 1, tm.frame, skip ? 1 : 0,
                         ms(t_start - t0), ms(t_emit - t0), skip ? 0.0 : ms(t_go - t0),
                         skip ? 0.0 : ms(t_req - t0), skip ? 0.0 : ms(t_go - t_emit));
        ++k;
        while (!quiet && sim.now >= next_report) { report(next_report, false); next_report += 1e12; }
    }
    double t_end = sim.now;
    // сводка за весь прогон
    rep_swaps = rep_renders = rep_takes = 0; rep_steps = rep_out = rep_skip = 0;
    report(t_end, true);
    double secs_model = (t_end - t0) / 1e12;
    double wsum = 0, wmax = 0;
    for (double w : waits) { wsum += w; wmax = std::max(wmax, w); }
    std::printf("в секунду: шагов игры %.2f (полная скорость 59,08), выводов %.2f, кадров FT %.2f, взято выводом %.2f\n",
                n_steps / secs_model, n_out / secs_model, sim.st.renders.size() / secs_model,
                sim.st.takes.size() / secs_model);
    {
        double cyc = (t_end - t0) / (sim.top->sd_fast ? PS_MEM / 2 : PS_MEM);
        static const char* nm[7] = {"вывод/захват", "RAM_G", "тексели", "выгрузка плиток", "оверлеи", "регенерация", "всего"};
        std::printf("SDRAM занята (доля тактов %s):", sim.top->sd_fast ? "clk_sd 123,75 МГц" : "clk_mem");
        for (int i = 0; i < 7; ++i) std::printf(" %s %.1f%%%s", nm[i], 100.0 * sim.top->busy[i] / cyc, i < 6 ? "," : "\n");
    }
    if (ramg_wr > 0) {
        sim.idle(1e8);                         // 100 мкс: очередь хоста и запись — до SDRAM
        long bad = 0;
        for (uint32_t i = 0; i < g_exp.size(); ++i)
            for (uint32_t b = 0; b < 4; ++b)
                if (((mem[(g_lo >> 2) + i] ^ g_exp[i]) >> (8 * b)) & 0xFF) {
                    if (++bad <= 8)
                        std::printf("  RAM_G %05X: в SDRAM %02X, записано %02X (последний байт хоста — %05X)\n",
                                    g_lo + 4 * i + b, (mem[(g_lo >> 2) + i] >> (8 * b)) & 0xFF,
                                    (g_exp[i] >> (8 * b)) & 0xFF, g_at - 1);
                }
        std::printf("RAM_G хоста: записано байтов %ld в окно %05X…%05X, в SDRAM не тех байтов %ld; FT_STAT 25 "
                    "(очередь переполнялась) %d, FT_STAT 26 (ответ на чтение опоздал) %d\n",
                    g_written, g_lo, g_hi - 1, bad, int(sim.top->ramg_drop), int(sim.top->ramg_late));
    }
    std::printf("ожидание хоста на DLSWAP: среднее %.2f мс, наибольшее %.2f мс (выводов %zu)\n",
                waits.empty() ? 0.0 : ms(wsum / waits.size()), ms(wmax), waits.size());
    // задержка смены листа: запрос DLSWAP → смена
    {
        std::vector<double> lat;
        size_t j = 0;
        for (double r : req_times) {
            while (j < sim.st.swaps.size() && sim.st.swaps[j] < r) ++j;
            if (j < sim.st.swaps.size()) lat.push_back(sim.st.swaps[j] - r);
        }
        if (!lat.empty()) {
            std::sort(lat.begin(), lat.end());
            double sum = 0;
            for (double v : lat) sum += v;
            std::printf("запрос DLSWAP → смена листа: среднее %.2f мс, медиана %.2f, 90%% %.2f, наибольшее %.2f\n",
                        ms(sum / lat.size()), ms(lat[lat.size() / 2]), ms(lat[lat.size() * 9 / 10]), ms(lat.back()));
        }
    }
    // растеризатор: такты по причинам (ядра с 1.24)
    if (sim.top->rc[0]) {
        auto rc = [&](int i) { return double(sim.top->rc[i]); };
        double fr = std::max<double>(1.0, double(sim.st.renders.size()));   // на объявленный кадр
        double xb = rc(26);
        std::printf("растеризатор: кадров начато %.0f, брошено %.0f, объявлено %zu; такты кадра в среднем %.2f мс\n",
                    rc(27), rc(28), sim.st.renders.size(), ms(rc(0) / fr * PS_MEM));
        static const char* xn[] = {"", "точки", "промах кэша", "сглаживание", "буфер точек", "палитра",
                                   "смена строки/слив", "ждёт обходчик", "слово состояния", "подготовка",
                                   "перенос (пустой конвейер)", "начало плитки", "конец плитки", "SAVE/RESTORE"};
        std::printf("  исполнитель занят %.2f мс на кадр:", ms(xb / fr * PS_MEM));
        for (int i = 1; i <= 13; ++i) std::printf(" %s %.1f%%%s", xn[i], 100.0 * rc(i) / std::max(1.0, xb), i < 13 ? "," : "\n");
        if (rc(29)) std::printf("  (прочие состояния: %.0f тактов)\n", rc(29));
        std::printf("  кадр идёт, исполнитель свободен: %.2f мс на кадр\n", ms(rc(14) / fr * PS_MEM));
        std::printf("  на кадр: точек %.0f, плиток %.0f, заполнений кэша %.0f; заданий %.0f (BITMAPS %.0f, RECTS %.0f, POINTS %.0f, состояние/CLEAR %.0f)\n",
                    rc(1) / fr, rc(19) / fr, rc(20) / fr, rc(18) / fr, rc(21) / fr, rc(22) / fr, rc(23) / fr, rc(24) / fr);
        std::printf("  заполнение кэша текселей: заявка ждёт приёма %.1f такта, место занято %.1f такта (на заполнение, clk_mem)\n",
                    rc(30) / std::max(1.0, rc(20)), rc(31) / std::max(1.0, rc(20)));
        std::printf("  выгрузка плиток: идёт %.2f мс на кадр, запрос ждёт приёма %.2f мс, слов %.0f на кадр; мост: очередь команд полна %.1f%%, запись ждёт места/сбора %.2f мс на кадр\n",
                    ms(rc(32) / fr * PS_MEM), ms(rc(33) / fr * PS_MEM), rc(34) / fr,
                    100.0 * rc(35) / std::max(1.0, rc(0)), ms(rc(36) / fr * PS_MEM));
        std::printf("  мост (такты clk_sd): голова — чтение без места в FIFO чтения %.2f мс на кадр, запись без слов %.2f мс, очередь пуста %.1f%%\n",
                    ms(rc(37) / fr * PS_MEM / 2), ms(rc(38) / fr * PS_MEM / 2), 100.0 * rc(39) / std::max(1.0, 2.0 * rc(0)));
        std::printf("  строка плитки ждёт приёма (мс на кадр): сбор прошлой %.2f, места в FIFO записи %.2f, места в очереди %.2f, арбитр отдал другому %.2f\n",
                    ms(rc(40) / fr * PS_MEM), ms(rc(41) / fr * PS_MEM), ms(rc(42) / fr * PS_MEM), ms(rc(43) / fr * PS_MEM));
        std::printf("  обходчик: занят %.2f мс на кадр, слов листа %.0f на кадр (%.1f на плитку), ждёт места в очереди %.1f%%, выборка пуста %.1f%%\n",
                    ms(rc(17) / fr * PS_MEM), rc(15) / fr, rc(15) / std::max(1.0, rc(19)),
                    100.0 * rc(16) / std::max(1.0, rc(17)), 100.0 * rc(25) / std::max(1.0, rc(17)));
    }
    if (sim.td_frames)
        std::printf("плитки 64×16, изменившиеся от прошлого кадра: среднее %.1f из 768, от %ld до %ld; кадров %ld"
                    " (0: %ld, 1–16: %ld, 17–64: %ld, 65–192: %ld, 193–384: %ld, 385–576: %ld, 577–767: %ld, все: %ld)\n",
                    double(sim.td_sum) / sim.td_frames, sim.td_min, sim.td_max, sim.td_frames,
                    sim.td_hist[0], sim.td_hist[1], sim.td_hist[2], sim.td_hist[3], sim.td_hist[4],
                    sim.td_hist[5], sim.td_hist[6], sim.td_hist[7]);
    if (refp)
        std::printf("сверка с эталоном: кадров %ld, не как эталон %ld (слов %ld; строки %d…%d, x %d…%d)\n",
                    sim.st.ref_frames, sim.st.ref_bad, sim.st.ref_words,
                    sim.st.ref_bad ? sim.st.ref_y0 : 0, sim.st.ref_bad ? sim.st.ref_y1 : 0,
                    sim.st.ref_bad ? sim.st.ref_x0 : 0, sim.st.ref_bad ? sim.st.ref_x1 : 0);
    if (fcsv) std::fclose(fcsv);
    if (dump) {
        // база половины кадра b, как ft_render.lbase (FB_BASE ts_fb_core — 0x128000)
        auto lb = [](unsigned b, bool hi) -> uint32_t {
            if (b == 0) return 0x128000u + (hi ? 0x40000u : 0u);
            if (b == 1) return 0x188000u + (hi ? 0x40000u : 0u);
            return hi ? 0x0C9000u : 0x080000u;
        };
        FILE* f = std::fopen(dump, "w");
        unsigned b = sim.top->fb_front;
        for (uint32_t y = 0; y < 768; ++y)
            for (uint32_t w = 0; w < 512; ++w)
                std::fprintf(f, "%08x\n", unsigned(mem[lb(b, y >= 512) + (y & 511) * 512 + w]));
        std::fclose(f);
        std::printf("кадр FT (буфер %u) -> %s\n", b, dump);
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
    std::printf("модель %.2f с за %.0f с счёта (%.1f с счёта на секунду модели)\n", secs_model, secs,
                secs / std::max(secs_model, 1e-9));
    std::fflush(stdout);
    // контекст нужен модели до её конца (~Sim: final, delete) — его не удалять: процесс кончается
    return 0;
}
