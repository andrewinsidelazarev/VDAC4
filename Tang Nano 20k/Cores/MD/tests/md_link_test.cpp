// Стенд LINK ядра Mega Drive (03.11.2026): ведущий QSPI как у ESP (команда 8 бит по IO0, адрес 32 бита
// тетрадами, чтение — 8 тактов разворота, слова по 32 бита), MCLK 54 МГц, SCK 40/20/5 МГц с разными
// фазами. Проверки: метка и VERSION, регистры управления, мост SDRAM (пакеты в банки 0–2 и 3, запись и
// чтение обратно, неполный пакет, отказ пакета с чужим номером), кольцо событий звука с оборотом.
#include "Vmd_link_top.h"
#include "verilated.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::printf("ОШИБКА, строка %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

struct Sim {
    VerilatedContext ctx; Vmd_link_top q{&ctx};
    uint64_t t = 0, edge = 926;                    // время — в 10 пс; полупериод MCLK 9,26 нс
    unsigned period;
    Sim(unsigned p, unsigned phase) : period(p) {
        q.clk = 0; q.sck = 0; q.cs_n = 1; q.io_in = 0; q.rst_n = 0; q.snd_push = 0; q.snd_word = 0; q.snd_time = 0;
        q.eval(); advance(20000); q.rst_n = 1; advance(20000 + phase);
    }
    void advance(uint64_t d) {
        const uint64_t end = t + d;
        while (edge <= end) { t = edge; edge += 926; ctx.time(t); q.clk = !q.clk; q.eval(); }
        t = end; ctx.time(t); q.eval();
    }
    void mclk_cycles(unsigned n) { advance(uint64_t(n) * 1852); }
    unsigned cycle(unsigned v) {
        q.io_in = v; q.eval(); advance(period / 2); q.sck = 1; q.eval(); const unsigned got = q.io_out;
        advance(period - period / 2); q.sck = 0; q.eval(); return got;
    }
    void begin(unsigned cmd, uint32_t a) {
        q.cs_n = 0; q.eval();
        for (int i = 7; i >= 0; i--) cycle((cmd >> i) & 1);
        for (int i = 7; i >= 0; i--) cycle((a >> (i * 4)) & 15);
    }
    void end() { q.cs_n = 1; q.eval(); advance(period); }
    void write(uint32_t a, const std::vector<uint32_t> &words) {
        begin(1, a); for (auto v : words) for (int i = 7; i >= 0; i--) cycle((v >> (i * 4)) & 15); end();
    }
    void wr(uint32_t a, uint32_t v) { write(a, {v}); mclk_cycles(8); }
    std::vector<uint32_t> read(uint32_t a, unsigned n) {
        begin(2, a); for (int i = 0; i < 8; i++) cycle(0);
        std::vector<uint32_t> out;
        for (unsigned k = 0; k < n; k++) { uint32_t v = 0; for (int i = 0; i < 8; i++) v = (v << 4) | cycle(0); out.push_back(v); }
        end(); return out;
    }
    uint32_t rd(uint32_t a) { return read(a, 1)[0]; }
    // пакет моста: запись (w) или чтение (в w); 1 — выполнен
    unsigned seq = 0;
    bool packet(uint32_t addr, std::vector<uint32_t> &w, bool write_it, bool z80 = false) {
        if (write_it) for (unsigned i = 0; i < w.size(); i += 16)
            this->write(0x5000 + 4 * i, std::vector<uint32_t>(w.begin() + i, w.begin() + std::min<size_t>(w.size(), i + 16)));
        seq = (seq + 1) & 0xFFFF;
        wr(0x160, addr);
        wr(0x164, (seq << 16) | (write_it ? 0x8000u : 0) | (z80 ? 0x4000u : 0) | unsigned(w.size()));
        for (int k = 0; k < 200; k++) {
            mclk_cycles(64);
            if ((rd(0x164) & 0xFFFF) == seq && !(rd(0x168) & 1)) {
                if (!write_it) for (unsigned i = 0; i < w.size(); i += 16) {
                    auto part = read(0x5000 + 4 * i, unsigned(std::min<size_t>(16, w.size() - i)));
                    for (unsigned j = 0; j < part.size(); j++) w[i + j] = part[j];
                }
                return true;
            }
        }
        return false;
    }
};

static uint32_t rnd() { static uint32_t s = 0x12345678; s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    unsigned cases = 0, words_checked = 0;
    for (unsigned p : {2500u, 5000u, 20000u}) for (unsigned phase = 0; phase < 1852; phase += 617) {
        Sim s(p, phase);
        for (int k = 0; k < 200 && !s.q.sd_ready; k++) s.mclk_cycles(2000);
        CHECK(s.q.sd_ready);
        CHECK(s.rd(0) == 0x56444334);
        CHECK(s.rd(4) == 0x06010008);
        CHECK(s.rd(0x150) == 0x89ABCDEF); CHECK(s.rd(0x154) == 0x01234567);   // состояние Z80
        s.wr(0x170, 3); CHECK(s.rd(0x170) == 7);                               // снимок: вставка, уровень 7, принят
        s.wr(0x174, 0x155); CHECK(s.rd(0x174) == 0x0155BEEF);                  // адрес вставки и слово
        s.wr(0x178, 0x1234); CHECK((s.rd(0x174) >> 16) == 0x156);              // запись — адрес +1
        s.wr(0x17C, 2); CHECK(s.rd(0x17C) == 2); s.wr(0x170, 0); CHECK(s.rd(0x170) == 4);
        s.wr(0x180, 0xF); CHECK(s.rd(0x180) == 0x2F);                          // Z80: управление и состояние
        for (unsigned i = 0; i < 7; i++) s.wr(0x184 + 4 * i, 0xA5000000u + i * 0x01010101u);
        for (unsigned i = 0; i < 6; i++) CHECK(s.rd(0x184 + 4 * i) == 0xA5000000u + i * 0x01010101u);
        CHECK(s.rd(0x19C) == ((0xA5000000u + 6 * 0x01010101u) & 0xFFFFF));    // DIR -> REG, 212 бит
        s.wr(0x1A0, 0x11223344); s.wr(0x1A4, 0x55667788); s.wr(0x1A8, 0x99AABBCC); s.wr(0x1AC, 0xDDEEFF00);
        CHECK(s.rd(0x1A0) == 0x11223344); CHECK(s.rd(0x1A4) == 0x55667788);
        CHECK(s.rd(0x1A8) == ~0x99AABBCCu); CHECK(s.rd(0x1AC) == ~0xDDEEFF00u);
        s.wr(0x1B4, 0x22); CHECK(s.rd(0x1B4) == (0x2200u | (0x22 ^ 0x5A)));     // отвод VDP
        {   // ОЗУ Z80 пакетами моста: с переходом через конец и неполный
            const uint32_t za[] = {0x1FE0, 0x0123}; const unsigned zn[] = {64, 17};
            std::vector<std::vector<uint32_t>> zd;
            for (unsigned i = 0; i < 2; i++) {
                std::vector<uint32_t> w(zn[i]); for (auto &v : w) v = rnd();
                zd.push_back(w); CHECK(s.packet(za[i], w, true, true));
            }
            for (unsigned i = 0; i < 2; i++) {
                std::vector<uint32_t> r(zn[i], 0xFFFFFFFF); CHECK(s.packet(za[i], r, false, true));
                for (unsigned j = 0; j < zn[i]; j++) { CHECK(r[j] == (zd[i][j] & 0xFF)); words_checked++; }
            }
        }
        CHECK(s.rd(0x28) == 54000);
        s.wr(0x140, 0x290D); CHECK(s.q.ctrl == 0x290D); CHECK(s.rd(0x140) == 0x290D);
        s.wr(0x144, 0x400000); CHECK(s.q.rom_size == 0x400000); CHECK(s.rd(0x144) == 0x400000);
        s.wr(0x148, 0x0ABC0123); CHECK(s.q.pads == 0xABC123); CHECK(s.rd(0x148) == 0x0ABC0123);
        s.wr(0x84, 0x535A0002); CHECK(s.q.picture_size == 2);
        s.wr(0x84, 0x12340001); CHECK(s.q.picture_size == 2);           // без метки не действует
        s.wr(0x84, 0x535A0005); CHECK(s.q.picture_size == 5); CHECK(s.rd(0x84) == 0x535A0005);   // 16:9, 95 %
        s.wr(0xC4, 1); CHECK(s.q.pause); s.wr(0xC4, 0); CHECK(!s.q.pause);
        s.wr(0xC0, 0x0501); CHECK(s.q.osd_show); s.wr(0xC0, 0x3F00); CHECK(!s.q.osd_show);
        // мост: банки 0, 1, ОЗУ 68000 (банк 2), видеопамять (банк 3), неполный пакет
        const uint32_t addrs[] = {0x000000, 0x0803F0, 0x174000, 0x180000, 0x1801C0};
        const unsigned counts[] = {64, 64, 64, 64, 17};
        std::vector<std::vector<uint32_t>> data;
        for (unsigned i = 0; i < 5; i++) {
            std::vector<uint32_t> w(counts[i]); for (auto &v : w) v = rnd();
            data.push_back(w);
            CHECK(s.packet(addrs[i], w, true));
        }
        for (unsigned i = 0; i < 5; i++) {
            std::vector<uint32_t> r(counts[i], 0);
            CHECK(s.packet(addrs[i], r, false));
            for (unsigned j = 0; j < counts[i]; j++) {
                if (r[j] != data[i][j]) std::printf("слово %u пакета %u: %08x вместо %08x\n", j, i, r[j], data[i][j]);
                CHECK(r[j] == data[i][j]);
                words_checked++;
            }
        }
        // пакет с чужим номером — отказ, номер не меняется
        const uint32_t done = s.rd(0x164) & 0xFFFF;
        s.wr(0x164, ((done + 5) << 16) | 0x8000 | 4);
        s.mclk_cycles(50);
        CHECK(s.rd(0x168) & 2);
        CHECK((s.rd(0x164) & 0xFFFF) == done);
        // кольцо звука: 1100 событий (оборот), указатель и обороты, последние слова
        s.q.snd_time = 0x12345; s.mclk_cycles(4);
        CHECK(s.rd(0x2C) == 0x12345);
        for (unsigned i = 0; i < 1100; i++) {
            s.q.snd_push = 1; s.q.snd_word = 0xC0000000u | i; s.mclk_cycles(1); s.q.snd_push = 0; s.mclk_cycles(1);
        }
        s.mclk_cycles(8);
        const uint32_t v = s.rd(0x20);
        uint32_t g = v & 0x3FF, b = g; for (unsigned sh = 1; sh < 16; sh <<= 1) b ^= b >> sh;
        CHECK((v >> 16) == 1); CHECK(b == 1100 - 1024);
        auto tail = s.read(0x6000 + 4 * 60, 16);
        for (unsigned j = 0; j < 16; j++) CHECK(tail[j] == (0xC0000000u | (1024 + 60 + j)));
        cases++;
        std::printf("SCK %u нс, фаза %u: всё верно\n", p / 100, phase);
    }
    std::printf("ВСЁ ВЕРНО: %u прогонов, слов моста сверено %u\n", cases, words_checked);
    return 0;
}
