// Модель Mega Drive VDAC4 (03.11.2026): прогон ROM на md_model (Verilator).
// md_model.exe <кадров> <папка> <размер ROM> <ctrl> <пады|-> <кадры для снимка через запятую|all|-N>
//   пады — файл строк «кадр значение» (значение — слово LINK 52 в hex): с этого кадра пады такие.
// Пишет: frame_NNNN.ppm (кадры VDP 320x240, как пишет их md_video), events.bin (слова md_snd_events),
// stats.json (по кадрам: запросы видеопамяти, MBUS и ожидание MBUS в тактах MCLK).
//
// MD_SNAP=S[,L[,K]] — снимок F5/F6, модель за ESP (порты те же, что LINK 5c–6d; ОЗУ 68000 и видеопамять —
// прямо из модели SDRAM, на плате их возит мост): на кадре S — снимок и продолжение, на кадре L — загрузка
// того же снимка. Возврат из обработчика в обоих случаях — через 20000 тактов после начала гашения кадра,
// поэтому после загрузки кадры обязаны повторить кадры после снимка: snap.log сверяет K кадров (5) и
// события звука до конца прогона (вид, порт, байт и промежутки времени между событиями).
#include "Vmd_model.h"
#include "Vmd_model___024root.h"
#include "Vmd_model_md_model.h"
#include "Vmd_model_sdr_chip_model.h"
#include "verilated.h"
#include "../esp/main/md_snap_68k.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <set>
#include <fstream>
#include <sstream>
#include <algorithm>

// снимок: всё, что ESP пишет в файл
struct MdSnap {
    uint16_t regs68[36];                 // слова вставки 80–a1: D0–D7, A0–A6, USP, SSP (старшее первым)
    uint8_t vdp_reg[24];
    uint32_t vdp_addr; uint8_t vdp_code; bool vdp_pending;
    uint16_t cram[64], vsram[40];
    uint8_t io[6];                       // управление портов 1–3, данные портов 1–3
    bool z80_run, z80_busreq;            // линия сброса снята; шина Z80 запрошена игрой
    uint32_t z80_reg[7];                 // T80 REG, PC = $0066
    uint8_t zram[8192];
    uint64_t sys, ym;
    uint32_t ram[16384], vram[16384];    // слова SDRAM: ОЗУ 68000 (174000) и видеопамять (180000)
};
struct Op { uint16_t op; uint32_t addr; uint16_t data; };

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    if (argc < 7) { std::fprintf(stderr, "аргументы: кадров папка размер ctrl пады снимки\n"); return 2; }
    const int frames = std::atoi(argv[1]);
    const std::string out = argv[2];
    const uint32_t rom_size = std::strtoul(argv[3], nullptr, 0);
    const uint32_t ctrl = std::strtoul(argv[4], nullptr, 0);
    std::vector<std::pair<int, uint32_t>> pad_plan;
    if (std::string(argv[5]) != "-") {
        std::ifstream f(argv[5]); int fr; std::string v;
        while (f >> fr >> v) pad_plan.push_back({fr, (uint32_t)std::strtoul(v.c_str(), nullptr, 16)});
    }
    std::set<int> shots; int every = 0; bool all = false;
    { std::string s = argv[6];
      if (s == "all") all = true; else if (!s.empty() && s[0] == '-') every = std::atoi(s.c_str() + 1);
      else { std::stringstream ss(s); std::string x; while (std::getline(ss, x, ',')) if (!x.empty()) shots.insert(std::atoi(x.c_str())); } }

    Vmd_model* m = new Vmd_model;
    m->ctrl = 0; m->rom_size = rom_size; m->pads = 0; m->pause = 0; m->rst_n = 0;
    if (const char *d = std::getenv("MD_MEM_DELAY")) m->mem_delay = std::atoi(d); else m->mem_delay = 1;
    m->picture_size = std::getenv("MD_PIC") ? std::atoi(std::getenv("MD_PIC")) & 7 : 0;   // [2] — 16:9
    int snap_save = -1, snap_load = -1, snap_cmp = 5;
    if (const char *s = std::getenv("MD_SNAP")) std::sscanf(s, "%d,%d,%d", &snap_save, &snap_load, &snap_cmp);
    std::FILE* sl = snap_save >= 0 ? std::fopen((out + "/snap.log").c_str(), "w") : nullptr;
    m->snap_act = 0; m->snap_irq = 0; m->snap_cmd = 0; m->snap_ovl_a = 0; m->snap_ovl_we = 0; m->snap_ovl_d = 0;
    m->snap_z80_ctl = 0; m->snap_zram_a = 0; m->snap_zram_we = 0; m->snap_zram_d = 0;
    m->snap_sys_d = 0; m->snap_ym_d = 0; m->snap_sys_we = 0; m->snap_vdp_a = 0;
    for (int i = 0; i < 7; i++) m->snap_z80_dir[i] = 0;
    m->clk = 0; m->clk_z80 = 0; m->hclk = 0; m->eval();
    // WITH_VIDEO (MD_VIDEO=1): такт HDMI 74,25 МГц точно 11/8 MCLK — время в долях 1/(2*54e6*11):
    // полупериод MCLK — 11 долей, HDMI — 8. Кадры HDMI 1280x720 — hdmi_NNNN.ppm, по кадру — hdmi.log.
    const bool video = std::getenv("MD_VIDEO") != nullptr;
    uint64_t tick = 0, next_h = 8;
    std::vector<uint8_t> himg(1280 * 720 * 3, 0);
    std::FILE* hl = video ? std::fopen((out + "/hdmi.log").c_str(), "w") : nullptr;
    int hframe = 0, hx = 0, hy = -1, hlines = 0; bool hde_r = false, hvs_r = false; uint64_t hlit = 0;
    auto hdmi_until = [&](uint64_t t) {
        while (video && next_h <= t) {
            m->hclk = !m->hclk; m->eval();
            if (m->hclk) {
                const bool de = m->h_de, vs = m->h_vs;
                if (vs && !hvs_r) {
                    std::fprintf(hl, "кадр HDMI %d: строк %d, ярких точек %llu, захват %d, поправка %d, перескоков %d\n", hframe, hlines,
                        (unsigned long long)hlit, (int)m->h_locked, (int)(int16_t)(m->h_trim << 7) >> 7, (int)m->h_jumps);
                    std::fflush(hl);
                    if (all || shots.count(hframe) || (every && hframe % every == 0)) {
                        char name[64]; std::snprintf(name, sizeof name, "/hdmi_%04d.ppm", hframe);
                        std::FILE* f = std::fopen((out + name).c_str(), "wb");
                        std::fprintf(f, "P6\n1280 720\n255\n"); std::fwrite(himg.data(), 1, himg.size(), f); std::fclose(f);
                    }
                    hframe++; hy = -1; hlines = 0; hlit = 0;
                }
                if (de) {
                    if (!hde_r) { hy++; hx = 0; hlines++; }
                    if (hy >= 0 && hy < 720 && hx < 1280) {
                        const uint32_t c = m->h_rgb; uint8_t* q = &himg[(hy * 1280 + hx) * 3];
                        q[0] = c >> 16; q[1] = c >> 8; q[2] = c; if (c) hlit++;
                    }
                    hx++;
                }
                hde_r = de; hvs_r = vs;
            }
            next_h += 8;
        }
    };

    std::vector<uint8_t> img(320 * 240 * 3, 0);
    std::vector<uint64_t> fhash;                    // FNV-1a каждого кадра VDP
    std::vector<uint32_t> evlog;                    // все события звука (для сверки снимка)
    std::FILE* ev = std::fopen((out + "/events.bin").c_str(), "wb");
    std::FILE* st = std::fopen((out + "/stats.json").c_str(), "w");
    std::fprintf(st, "{\"frames\":[\n");

    uint64_t cyc = 0;
    int frame = 0; bool first_stat = true, stop = false;
    // запись строк — как md_video
    bool ce_r = false, hbl_r = false, vbl_r = true, hsync_seen = false;
    int wx = 0, wy = 0, width = 320, height = 224, max_x = 0, max_y = 0;
    bool vr_r = false, v32_r = false, mr_r = false;
    uint64_t n_v = 0, n_v32 = 0, n_m = 0, wait_m = 0, max_wait = 0, m_start = 0, events = 0;
    bool m_open = false;
    // циклы шины 68000: сколько тактов процессора (M68K_CLKENp) держится AS; ROM/ОЗУ отдельно
    uint64_t bus_hist[2][16] = {}; bool as_r = true, as_rom = false; int as_len = 0;
    size_t plan_i = 0;
    // один такт MCLK со всеми наблюдениями
    auto step = [&]() {
        // фронт MCLK; Z80 поднимается на чётных фронтах
        hdmi_until(tick);
        m->clk = 1; m->clk_z80 = (cyc & 1) == 0; m->eval();
        if (cyc == 20) m->rst_n = 1;
        if (cyc == 30) m->ctrl = ctrl;
        // наблюдения после фронта
        if (m->snd_push) { uint32_t w = m->snd_word; std::fwrite(&w, 4, 1, ev); events++; if (sl) evlog.push_back(w); }
        if (m->vram_req != vr_r) { n_v++; vr_r = m->vram_req; }
        if (m->vram32_req != v32_r) { n_v32++; v32_r = m->vram32_req; }
        if (m->mem_req != mr_r) { n_m++; mr_r = m->mem_req; m_open = true; m_start = cyc; }
        if (m_open && m->mem_req == m->mem_ack) {
            m_open = false; uint64_t w = cyc - m_start; wait_m += w; if (w > max_wait) max_wait = w;
        }
        if (!m->m68k_as_n) {
            if (as_r) { as_len = 0; as_rom = m->m68k_rom; }
            if (m->m68k_clken) as_len++;
        } else if (!as_r) bus_hist[as_rom][as_len > 15 ? 15 : as_len]++;
        as_r = m->m68k_as_n;
        const bool ce = m->ce_pix, hbl = m->hbl, vbl = m->vbl;
        if (vbl) { wy = 0; hsync_seen = false; }
        if (m->hs) hsync_seen = true;
        if (hsync_seen && !vbl) {
            if (ce && !ce_r && !hbl && wx < 320) {
                if (wy < 240) {
                    uint8_t* p = &img[(wy * 320 + wx) * 3];
                    p[0] = m->red * 17; p[1] = m->green * 17; p[2] = m->blue * 17;
                }
                wx++; if (wx > max_x) max_x = wx;
            }
            if (hbl) { if (!hbl_r) { wy++; if (wy > max_y) max_y = wy; } wx = 0; }
        }
        if (vbl && !vbl_r) {
            // кадр готов
            width = max_x; height = max_y;
            bool shot = all || shots.count(frame) || (every && frame % every == 0);
            if (shot) {
                char name[64]; std::snprintf(name, sizeof name, "/frame_%04d.ppm", frame);
                std::FILE* f = std::fopen((out + name).c_str(), "wb");
                std::fprintf(f, "P6\n%d %d\n255\n", width ? width : 320, height ? height : 224);
                for (int y = 0; y < (height ? height : 224); y++)
                    std::fwrite(&img[y * 320 * 3], 3, width ? width : 320, f);
                std::fclose(f);
            }
            uint64_t h = 1469598103934665603ull;
            for (uint8_t b : img) { h ^= b; h *= 1099511628211ull; }
            fhash.push_back(h);
            std::fprintf(st, "%s{\"frame\":%d,\"w\":%d,\"h\":%d,\"vram\":%llu,\"vram32\":%llu,\"mbus\":%llu,"
                "\"mbus_wait\":%llu,\"mbus_wait_max\":%llu,\"events\":%llu,\"cycle\":%llu,\"lace\":%d,\"field\":%d,\"z80\":\"%016llX\"}",
                first_stat ? "" : ",\n", frame, width, height, (unsigned long long)n_v, (unsigned long long)n_v32,
                (unsigned long long)n_m, (unsigned long long)wait_m, (unsigned long long)max_wait,
                (unsigned long long)events, (unsigned long long)cyc, (int)m->lace, (int)m->field,
                (unsigned long long)m->z80_dbg);
            first_stat = false;
            std::fflush(st);
            n_v = n_v32 = n_m = wait_m = max_wait = 0;
            max_x = max_y = 0;
            frame++;
            while (plan_i < pad_plan.size() && pad_plan[plan_i].first <= frame) m->pads = pad_plan[plan_i++].second;
            if (frame % 20 == 0) { std::printf("кадр %d, %.1f млн тактов\n", frame, cyc / 1e6); std::fflush(stdout); }
        }
        ce_r = ce; hbl_r = hbl; vbl_r = vbl;
        // спад MCLK
        hdmi_until(tick + 11);
        m->clk = 0; m->eval();
        cyc++; tick += 22;
        // кадр PAL — 1,08 млн тактов
        if (cyc > (uint64_t)(frames + 30) * 1200000ull) { std::printf("нет кадров: VDP не дошёл до гашения\n"); stop = true; }
    };

    // ---- модель за ESP: снимок ----
    auto& sdram = m->rootp->md_model->__PVT__chip->mem;
    auto fail = [&](const char* what) { std::fprintf(sl, "ОШИБКА: %s (кадр %d, такт %llu)\n", what, frame, (unsigned long long)cyc); std::fflush(sl); return false; };
    auto wait_until = [&](auto cond, uint64_t limit, const char* what) {
        const uint64_t t0 = cyc;
        while (!cond()) { step(); if (cyc - t0 > limit || stop) return fail(what); }
        return true;
    };
    auto ovl_wr = [&](unsigned a, uint16_t d) { m->snap_ovl_a = a; m->snap_ovl_d = d; m->snap_ovl_we = 1; step(); m->snap_ovl_we = 0; };
    auto ovl_rd = [&](unsigned a) -> uint16_t { m->snap_ovl_a = a; step(); return m->snap_ovl_q; };
    auto vdp_rd = [&](unsigned a) -> uint8_t { m->snap_vdp_a = a; step(); return m->snap_vdp_q; };
    auto zram_rd = [&](unsigned a) -> uint8_t { m->snap_zram_a = a; step(); step(); return m->snap_zram_q; };
    auto zram_wr = [&](unsigned a, uint8_t d) { m->snap_zram_a = a; m->snap_zram_d = d; m->snap_zram_we = 1; step(); m->snap_zram_we = 0; };
    unsigned seq = 0;
    auto command = [&](unsigned code) {
        seq = seq % 255 + 1;
        const uint16_t c = (uint16_t)(seq << 8 | code);
        const uint16_t want = code == MD_SNAP_CMD_SCRIPT ? c : 0;
        m->snap_cmd = c;
        return wait_until([&] { return ovl_rd(MD_SNAP_STATE) == want; }, 30000000, "обработчик не выполнил команду");
    };
    auto script = [&](std::vector<Op>& ops) {
        if (ops.size() >= MD_SNAP_SCRIPT_MAX) return fail("сценарий длиннее вставки");
        for (size_t i = 0; i < ops.size(); i++) {
            const unsigned a = MD_SNAP_SCRIPT + 4 * (unsigned)i;
            ovl_wr(a, ops[i].op); ovl_wr(a + 1, ops[i].addr >> 16); ovl_wr(a + 2, ops[i].addr & 0xFFFF); ovl_wr(a + 3, ops[i].data);
        }
        ovl_wr(MD_SNAP_SCRIPT + 4 * (unsigned)ops.size(), MD_SNAP_OP_END);
        if (!command(MD_SNAP_CMD_SCRIPT)) return false;
        for (size_t i = 0; i < ops.size(); i++)
            if (ops[i].op == MD_SNAP_OP_RW || ops[i].op == MD_SNAP_OP_RB) ops[i].data = ovl_rd(MD_SNAP_SCRIPT + 4 * (unsigned)i + 3);
        return true;
    };
    const uint32_t VDP_DATA = 0xC00000, VDP_CTRL = 0xC00004;
    // остановить 68000 в обработчике (уровень 7)
    auto freeze = [&]() {
        m->snap_z80_ctl = 0; step();
        for (unsigned i = 0; i < sizeof md_snap_68k / 2; i++) ovl_wr(i, md_snap_68k[i]);
        ovl_wr(MD_SNAP_STATE, 0);
        m->snap_cmd = 0; seq = 0; m->snap_act = 1; m->snap_irq = 1;
        const uint64_t t0 = cyc;
        if (!wait_until([&] { return ovl_rd(MD_SNAP_STATE) == 1; }, 5000000, "68000 не принял уровень 7")) return false;
        m->snap_irq = 0;
        std::fprintf(sl, "кадр %d: 68000 в обработчике через %llu тактов MCLK\n", frame, (unsigned long long)(cyc - t0));
        return true;
    };
    // ADDR, CODE, PENDING порта VDP — командой (запись регистра даёт CODE[1:0] = 10, поэтому так)
    auto vdp_port = [&](std::vector<Op>& s, const MdSnap& S) {
        const uint16_t w1 = (uint16_t)((S.vdp_code & 3) << 14 | (S.vdp_addr & 0x3FFF));
        const uint16_t w2 = (uint16_t)(((S.vdp_code >> 2) & 7) << 4 | ((S.vdp_addr >> 14) & 7));
        if ((S.vdp_code & 3) == 2) {
            s.push_back({MD_SNAP_OP_WW, VDP_CTRL, (uint16_t)(w1 & 0x3FFF)}); s.push_back({MD_SNAP_OP_WW, VDP_CTRL, w2});
            s.push_back({MD_SNAP_OP_WW, VDP_CTRL, (uint16_t)(0x8F00 | S.vdp_reg[15])});
        } else {
            s.push_back({MD_SNAP_OP_WW, VDP_CTRL, w1}); s.push_back({MD_SNAP_OP_WW, VDP_CTRL, w2});
            if (S.vdp_pending) s.push_back({MD_SNAP_OP_WW, VDP_CTRL, w1});
        }
    };
    auto save = [&](MdSnap& S) {
        if (!freeze()) return false;
        for (unsigned i = 0; i < 36; i++) S.regs68[i] = ovl_rd(MD_SNAP_REGS + i);
        const uint64_t zd = m->z80_dbg;                     // линии Z80 — до того, как сценарий их тронет
        S.z80_run = (zd >> 31) & 1; S.z80_busreq = !((zd >> 30) & 1);
        for (unsigned r = 0; r < 24; r++) S.vdp_reg[r] = vdp_rd(r);
        const uint8_t a0 = vdp_rd(0x20), a1 = vdp_rd(0x21), a2 = vdp_rd(0x22);
        S.vdp_addr = a0 | a1 << 8 | (a2 & 1) << 16; S.vdp_code = (a2 >> 1) & 0x3F; S.vdp_pending = a2 >> 7;
        std::vector<Op> s = {{MD_SNAP_OP_WAIT0, VDP_CTRL, 0x0002}, {MD_SNAP_OP_WAIT1, VDP_CTRL, 0x0200},
                             {MD_SNAP_OP_WW, VDP_CTRL, 0x8F02}, {MD_SNAP_OP_WW, VDP_CTRL, 0x0000}, {MD_SNAP_OP_WW, VDP_CTRL, 0x0020}};
        for (int i = 0; i < 64; i++) s.push_back({MD_SNAP_OP_RW, VDP_DATA, 0});
        s.push_back({MD_SNAP_OP_WW, VDP_CTRL, 0x0000}); s.push_back({MD_SNAP_OP_WW, VDP_CTRL, 0x0010});
        for (int i = 0; i < 40; i++) s.push_back({MD_SNAP_OP_RW, VDP_DATA, 0});
        for (uint32_t a : {0xA10009u, 0xA1000Bu, 0xA1000Du, 0xA10003u, 0xA10005u, 0xA10007u}) s.push_back({MD_SNAP_OP_RB, a, 0});
        if (!script(s)) return false;
        for (int i = 0; i < 64; i++) S.cram[i] = s[5 + i].data & 0x0EEE;
        for (int i = 0; i < 40; i++) S.vsram[i] = s[71 + i].data & 0x07FF;
        for (int i = 0; i < 6; i++) S.io[i] = (uint8_t)s[111 + i].data;
        // Z80: свой NMI — стоянка во вставке $0066
        if (S.z80_run) {
            if (S.z80_busreq) { std::vector<Op> r = {{MD_SNAP_OP_WW, 0xA11100, 0x0000}}; if (!script(r)) return false; }
            m->snap_z80_ctl = 1; step(); step();
            m->snap_z80_ctl = 5;
            const uint64_t t0 = cyc;
            if (!wait_until([&] { return (m->snap_z80_st & 1) != 0; }, 2000000, "Z80 не встал во вставку")) return false;
            m->snap_z80_ctl = 1;
            for (int i = 0; i < 7; i++) S.z80_reg[i] = m->snap_z80_reg[i];
            std::fprintf(sl, "Z80 во вставке через %llu тактов, PC %04X, SP %04X, AF %02X%02X, IFF %d%d, IM %d%s\n",
                (unsigned long long)(cyc - t0), S.z80_reg[2] & 0xFFFF, S.z80_reg[1] >> 16, S.z80_reg[0] & 0xFF, (S.z80_reg[0] >> 8) & 0xFF,
                (S.z80_reg[6] >> 19) & 1, (S.z80_reg[6] >> 18) & 1, (S.z80_reg[6] >> 16) & 3, S.z80_busreq ? ", шина была у 68000" : "");
            S.z80_reg[2] = (S.z80_reg[2] & 0xFFFF0000u) | 0x0066;
        } else std::fprintf(sl, "Z80 в сбросе\n");
        for (unsigned i = 0; i < 16384; i++) { S.ram[i] = sdram[0x174000 + i]; S.vram[i] = sdram[0x180000 + i]; }
        for (unsigned i = 0; i < 8192; i++) S.zram[i] = zram_rd(i);
        S.sys = m->snap_sys_q; S.ym = m->snap_ym_q;
        std::fprintf(sl, "VDP: ADDR %05X CODE %02X%s, рег. 1 %02X, 15 %02X; банки %016llX, YM %016llX\n", S.vdp_addr, S.vdp_code,
            S.vdp_pending ? " ждёт 2-е слово" : "", S.vdp_reg[1], S.vdp_reg[15], (unsigned long long)S.sys, (unsigned long long)S.ym);
        return true;
    };
    // вернуть игру: порт VDP, шина Z80, отпустить Z80, возврат в начале кадра + 20000 тактов
    int ret_frame = -1;
    auto resume = [&](const MdSnap& S, bool load) {
        std::vector<Op> s = {{MD_SNAP_OP_WW, VDP_CTRL, (uint16_t)(0x8100 | S.vdp_reg[1])}, {MD_SNAP_OP_WW, VDP_CTRL, (uint16_t)(0x8F00 | S.vdp_reg[15])}};
        vdp_port(s, S);
        if (S.z80_run && S.z80_busreq) s.push_back({MD_SNAP_OP_WW, 0xA11100, 0x0100});
        if (!script(s)) return false;
        m->snap_z80_ctl = S.z80_run ? 3 : 0;
        const int f0 = frame;
        if (!wait_until([&] { return frame > f0; }, 2000000, "нет кадра")) return false;
        for (int i = 0; i < 20000; i++) step();
        ret_frame = frame;
        if (!command(load ? MD_SNAP_CMD_LOAD : MD_SNAP_CMD_RET)) return false;
        m->snap_act = 0;
        std::fprintf(sl, "кадр %d: игра продолжена%s\n", frame, load ? " после загрузки" : "");
        return true;
    };
    auto load = [&](const MdSnap& S) {
        if (!freeze()) return false;
        m->snap_z80_ctl = 8;                                 // T80 в сбросе, пока пишутся ОЗУ и линии
        std::vector<Op> s = {{MD_SNAP_OP_WAIT0, VDP_CTRL, 0x0002}, {MD_SNAP_OP_WAIT1, VDP_CTRL, 0x0200},
                             {MD_SNAP_OP_WW, VDP_CTRL, (uint16_t)(0x8100 | (vdp_rd(1) & ~0x40))}};   // гасим экран
        if (!script(s)) return false;
        for (unsigned i = 0; i < 16384; i++) { sdram[0x174000 + i] = S.ram[i]; sdram[0x180000 + i] = S.vram[i]; }
        for (unsigned i = 0; i < 8192; i++) zram_wr(i, S.zram[i]);
        s = {{MD_SNAP_OP_WW, VDP_CTRL, (uint16_t)(0x8100 | (S.vdp_reg[1] & ~0x40))}};   // режим 5 — до регистров 11–23
        for (unsigned r = 0; r < 24; r++) if (r != 1) s.push_back({MD_SNAP_OP_WW, VDP_CTRL, (uint16_t)(0x8000 | r << 8 | (r == 15 ? 2 : S.vdp_reg[r]))});
        s.push_back({MD_SNAP_OP_WW, VDP_CTRL, 0xC000}); s.push_back({MD_SNAP_OP_WW, VDP_CTRL, 0x0000});
        for (int i = 0; i < 64; i++) s.push_back({MD_SNAP_OP_WW, VDP_DATA, S.cram[i]});
        s.push_back({MD_SNAP_OP_WW, VDP_CTRL, 0x4000}); s.push_back({MD_SNAP_OP_WW, VDP_CTRL, 0x0010});
        for (int i = 0; i < 40; i++) s.push_back({MD_SNAP_OP_WW, VDP_DATA, S.vsram[i]});
        const uint32_t io[6] = {0xA10009, 0xA1000B, 0xA1000D, 0xA10003, 0xA10005, 0xA10007};
        for (int i = 0; i < 6; i++) s.push_back({MD_SNAP_OP_WB, io[i], S.io[i]});
        s.push_back({MD_SNAP_OP_WW, 0xA11200, (uint16_t)(S.z80_run ? 0x0100 : 0)});
        s.push_back({MD_SNAP_OP_WW, 0xA11100, (uint16_t)(S.z80_busreq ? 0x0100 : 0)});
        if (!script(s)) return false;
        // банки и таймеры YM — после линии сброса Z80 (она сбрасывает и YM)
        m->snap_sys_d = S.sys; m->snap_ym_d = S.ym; m->snap_sys_we = 1; step(); m->snap_sys_we = 0;
        if (S.z80_run) {
            for (int i = 0; i < 7; i++) m->snap_z80_dir[i] = S.z80_reg[i];
            m->snap_z80_ctl = 9; for (int i = 0; i < 8; i++) step();   // вставка взведена, T80 ещё в сбросе
            m->snap_z80_ctl = 1; for (int i = 0; i < 8; i++) step();   // сброс снят — DIRSet
        } else m->snap_z80_ctl = 0;
        for (unsigned i = 0; i < 36; i++) ovl_wr(MD_SNAP_REGS + i, S.regs68[i]);
        return true;
    };
    static MdSnap S0;
    bool saved = false, loaded = false; int ret_save = -1, ret_load = -1; size_t ev_save = 0, ev_load = 0;

    while (frame < frames && !stop) {
        if (snap_save >= 0 && !saved && frame >= snap_save) {
            saved = true;
            if (!save(S0) || !resume(S0, false)) break;
            ret_save = ret_frame; ev_save = evlog.size();
        }
        if (snap_load >= 0 && saved && !loaded && frame >= snap_load) {
            loaded = true;
            if (!load(S0) || !resume(S0, true)) break;
            ret_load = ret_frame; ev_load = evlog.size();
        }
        step();
    }
    if (sl && ret_save >= 0 && ret_load >= 0) {
        int same = 0;
        for (int k = 0; k < snap_cmp; k++) {
            const size_t a = ret_save + k, b = ret_load + k;
            if (b >= fhash.size()) break;
            const bool eq = fhash[a] == fhash[b];
            same += eq;
            std::fprintf(sl, "кадр %zu после снимка и %zu после загрузки: %s\n", a, b, eq ? "совпали" : "РАЗНЫЕ");
        }
        std::fprintf(sl, "ИТОГ: совпало %d из %d кадров\n", same, snap_cmp);
        // звук: после снимка — до загрузки, после загрузки — до конца; сверка по короткому
        const size_t n = std::min(ev_load - ev_save, evlog.size() - ev_load);
        size_t diff = n; int dt_max = 0;
        for (size_t i = 0; i < n; i++) {
            const uint32_t a = evlog[ev_save + i], b = evlog[ev_load + i];
            if ((a & 0xF00000FFu) != (b & 0xF00000FFu)) { diff = i; break; }
            if (i) {
                const int da = ((a >> 8) - (evlog[ev_save + i - 1] >> 8)) & 0xFFFFF;
                const int db = ((b >> 8) - (evlog[ev_load + i - 1] >> 8)) & 0xFFFFF;
                if (std::abs(da - db) > dt_max) dt_max = std::abs(da - db);
            }
        }
        if (diff == n) std::fprintf(sl, "ЗВУК: %zu событий совпали (вид, порт, байт), промежутки расходятся не больше %d тактов YM\n", n, dt_max);
        else std::fprintf(sl, "ЗВУК: РАСХОЖДЕНИЕ на событии %zu из %zu: %08X после снимка, %08X после загрузки\n", diff, n,
                          evlog[ev_save + diff], evlog[ev_load + diff]);
    }
    std::fprintf(st, "\n],\"bus_cycles\":{\"other\":[");
    for (int i = 0; i < 16; i++) std::fprintf(st, "%s%llu", i ? "," : "", (unsigned long long)bus_hist[0][i]);
    std::fprintf(st, "],\"rom_ram\":[");
    for (int i = 0; i < 16; i++) std::fprintf(st, "%s%llu", i ? "," : "", (unsigned long long)bus_hist[1][i]);
    std::fprintf(st, "]}}\n");
    std::fclose(st); std::fclose(ev); if (hl) std::fclose(hl); if (sl) std::fclose(sl);
    std::printf("готово: %d кадров, %llu тактов, событий звука %llu\n", frame, (unsigned long long)cyc,
        (unsigned long long)events);
    m->final(); delete m;
    return 0;
}
