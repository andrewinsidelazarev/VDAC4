// Замер синтеза звука Mega Drive на ПК (03.11.2026): тот же движок, что на ESP (esp/main/md_sound.cpp),
// искусственная нагрузка по шагам — что сколько стоит. На плате (Jungle Book) половина блока заняла до
// 1,1–1,3 млн тактов при бюджете 1,148 млн; здесь — доли по видам работы, такты ESP пересчитываются по
// соотношению с прогоном «6 каналов» (его стоимость на плате — из журнала).
// snd_profile [секунд звука]
#include "md_sound.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

struct Ev { uint64_t s; md_snd_write_t w; };   // s — отсчёт YM

struct Gen {
    std::vector<Ev> ev;
    uint64_t s = 0;
    void ym(int bank, uint8_t reg, uint8_t val) {
        ev.push_back({s, {0, 0, uint8_t(bank ? 2 : 0), reg}});
        ev.push_back({s, {0, 0, uint8_t(bank ? 3 : 1), val}});
    }
    void dac(uint8_t v) { ev.push_back({s, {0, 0, 1, v}}); }
    void psg(uint8_t v) { ev.push_back({s, {0, 1, 0, v}}); }
    void psg_tone(int ch, unsigned period, unsigned vol) {           // защёлка с младшими битами, байт старших, громкость
        psg(uint8_t(0x80 | ch << 5 | (period & 15))); psg(uint8_t(period >> 4 & 0x3F)); psg(uint8_t(0x90 | ch << 5 | vol));
    }
    // голос: алгоритм, обратная связь, огибающие с длинным звучанием
    void voice(int ch, int alg, bool lfo) {
        const int b = ch / 3, c = ch % 3;
        for (int op = 0; op < 4; op++) {
            const uint8_t o = uint8_t(c + 4 * op);
            ym(b, 0x30 + o, uint8_t(0x01 + op * 2 + (op & 1) * 0x30));   // DT, MUL
            ym(b, 0x40 + o, uint8_t(op == 3 ? 0x08 : 0x20 + op * 4));     // TL
            ym(b, 0x50 + o, 0x1F);                                        // AR
            ym(b, 0x60 + o, uint8_t(0x05 | (lfo ? 0x80 : 0)));            // D1R, AM
            ym(b, 0x70 + o, 0x02);                                        // D2R
            ym(b, 0x80 + o, 0x1F);                                        // SL, RR
            ym(b, 0x90 + o, 0x00);
        }
        ym(b, 0xB0 + c, uint8_t(0x38 | alg));                             // FB 7
        ym(b, 0xB4 + c, uint8_t(0xC0 | (lfo ? 0x37 : 0)));                // L R, AMS 3, FMS 7
    }
    void freq(int ch, unsigned f) {
        const int b = ch / 3, c = ch % 3;
        ym(b, 0xA4 + c, uint8_t(0x20 | (f >> 8)));                        // блок 4
        ym(b, 0xA0 + c, uint8_t(f));
    }
    void key(int ch, bool on) {
        const int c = ch % 3 + (ch >= 3 ? 4 : 0);
        ym(0, 0x28, uint8_t((on ? 0xF0 : 0) | c));
    }
};

struct Case { const char *name; int chans; bool lfo, driver, dac; };

double run(const Case &k, unsigned seconds, double *t0, double *t1, size_t *n_ev, uint64_t *hash) {
    const double rate = 54e6 / 1008.0;
    const uint64_t total = uint64_t(seconds * rate);
    Gen g;
    g.ym(0, 0x22, k.lfo ? 0x0B : 0x00);                                  // LFO 6,02 Гц
    g.ym(0, 0x2B, k.dac ? 0x80 : 0x00);
    for (int ch = 0; ch < k.chans; ch++) { g.voice(ch, ch % 8, k.lfo); g.freq(ch, 600 + 37 * ch); g.key(ch, true); }
    if (k.driver) {                                                       // PSG: два тона, период 1 (отсчёты), белый шум
        g.psg_tone(0, 0x0FE, 2); g.psg_tone(1, 0x1AB, 4); g.psg_tone(2, 1, 6);
        g.psg(0xE4); g.psg(0xF5);
    }
    const unsigned frame = unsigned(rate / 60.0);                         // кадр 60 Гц
    const unsigned dac_step = unsigned(rate / 16000.0);                   // ЦАП 16 кГц (ударные)
    uint8_t dac = 0x80;
    for (uint64_t s = 1; s < total; s++) {
        g.s = s;
        if (k.driver && s % frame == 0) {                                 // драйвер раз в кадр
            const unsigned f = unsigned(s / frame);
            for (int ch = 0; ch < k.chans; ch++) {
                g.freq(ch, 600 + 37 * ch + (f & 7));                      // вибрато
                const int b = ch / 3, c = ch % 3;
                for (int op = 0; op < 4; op++) g.ym(b, uint8_t(0x40 + c + 4 * op), uint8_t(op == 3 ? 0x08 + (f & 3) : 0x20 + op * 4));
                if (f % 8 == ch) { g.key(ch, false); g.key(ch, true); }   // новая нота
            }
            g.psg(uint8_t(0x90 | (f & 15)));                              // громкость тона 0 — огибающей драйвера
            if (f % 16 == 0) g.psg(0xE4 | (f >> 4 & 3));                  // шум: новая частота — регистр сброшен
            if (k.dac) g.ev.push_back({s, {0, 0, 0, 0x2A}});              // драйвер снова выбирает ЦАП
        }
        if (k.dac && s % dac_step == 0) { dac = uint8_t(dac + 37); g.dac(dac); }
    }
    *n_ev = g.ev.size();
    std::vector<uint8_t> mem(md_snd_size());
    md_snd_init(mem.data());
    std::vector<md_snd_write_t> bw;
    static float out[(MD_SND_BLOCK + 1) * 2];
    size_t i = 0;
    double h0 = 0, h1 = 0;
    *hash = 0xCBF29CE484222325ull;
    using clk = std::chrono::steady_clock;
    for (uint64_t s = 0; s < total; s += MD_SND_BLOCK) {
        const unsigned n = unsigned(total - s < MD_SND_BLOCK ? total - s : MD_SND_BLOCK);
        bw.clear();
        while (i < g.ev.size() && g.ev[i].s < s + n) { md_snd_write_t w = g.ev[i].w; w.pos = uint16_t(g.ev[i].s - s); bw.push_back(w); i++; }
        md_snd_block_begin(mem.data(), bw.data(), unsigned(bw.size()), n);
        auto a = clk::now();
        md_snd_render_half(mem.data(), 0);
        auto b = clk::now();
        md_snd_render_half(mem.data(), 1);
        auto c = clk::now();
        md_snd_mix(mem.data(), out, 0.9f);
        for (unsigned j = 0; j < 2 * n; j++) {                            // FNV-1a по 16-битным отсчётам
            const int16_t v = int16_t(out[j] * 32767.0f);
            *hash = (*hash ^ uint16_t(v)) * 0x100000001B3ull;
        }
        h0 += std::chrono::duration<double>(b - a).count();
        h1 += std::chrono::duration<double>(c - b).count();
    }
    *t0 = h0 * 1e9 / double(total); *t1 = h1 * 1e9 / double(total);
    return double(total);
}

}  // namespace

int main(int argc, char **argv) {
    const unsigned sec = argc > 1 ? unsigned(std::atoi(argv[1])) : 20;
    const Case cases[] = {
        {"тишина (ключей нет)", 0, false, false, false},
        {"6 каналов, без записей", 6, false, false, false},
        {"6 каналов + LFO", 6, true, false, false},
        {"6 каналов + драйвер 60 Гц", 6, false, true, false},
        {"6 каналов + драйвер + ЦАП 16 кГц", 6, false, true, true},
        {"6 каналов + драйвер + ЦАП + LFO", 6, true, true, true},
    };
    double base = 0;
    for (const Case &k : cases) {
        double t0, t1; size_t n; uint64_t hash;
        run(k, sec, &t0, &t1, &n, &hash);
        if (k.chans == 6 && !k.lfo && !k.driver) base = t0 + t1;
        std::printf("%-36s событий %8zu: половина 0 %6.1f нс/отсчёт, половина 1 %6.1f, сумма %6.1f, звук %016llx\n", k.name, n, t0, t1,
                    t0 + t1, (unsigned long long)hash);
        if (base > 0) std::printf("%38s к «6 каналам»: x%.2f\n", "", (t0 + t1) / base);
    }
    return 0;
}
