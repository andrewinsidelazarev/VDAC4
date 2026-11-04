// SPDX-License-Identifier: GPL-3.0-or-later
// Звук Mega Drive на ESP (MD.ELF, 03.11.2026) — см. md_sound.h.
//
// YM2612 — ymfm (Aaron Giles, BSD-3) в локальной копии с ym2612::generate_part: два экземпляра,
// каналы 0–2 и 3–5; ЦАП (канал 6) — у второго. Искажение ЦАП («лесенка» YM2612) — как в ymfm.
//
// SN76489 (PSG в VDP Mega Drive): такт — MCLK/15, счётчики тонов — его /16, то есть 4,2 шага на отсчёт
// YM (1008/240). Тон: счётчик доходит до нуля — перезагрузка периодом, выход меняется; период 0 и 1
// у Sega — как 1 (выход мигает каждый шаг, в среднем половина уровня: так игры играют отсчёты
// громкостью). Шум: период 0x10/0x20/0x40 или период тона 2; регистр 16 бит, белый — отводы 0 и 3,
// запись в регистр шума сбрасывает его в 0x8000. Громкость — 2 дБ на шаг, 15 — тишина. Выход
// однополярный (как у микросхемы), постоянную составляющую убирает фильтр смесителя около 20 Гц.
// Отсчёт PSG — среднее за шаги отсчёта YM. Сумма уровней каналов (level) пересчитывается только при смене
// выхода, шума или громкости (MD.ELF 0.4: раньше — на каждом шаге), выражение то же — звук бит в бит прежний.
//
// MD.ELF 0.5 (плата: PSG — около 750 тактов ESP на отсчёт, даже молча): PSG считается отдельным проходом по блоку
// после YM (код синтеза и код PSG не чередуются на каждом отсчёте) и с быстрым путём — если ни один счётчик
// за шаги отсчёта не дойдёт до перезагрузки, выходы не меняются и отсчёт равен уровню (это и есть среднее);
// деление на число шагов — умножением. От 0.4 отличие — в младших разрядах float.
// MD.ELF 0.7: быстрый путь не смотрит на молчащие каналы (громкость 15 — уровень 0, их переключения звука не
// меняют). У неиспользуемых каналов период 0 (как 1): переключение на каждом шаге — в 0.5–0.6 быстрый путь
// поэтому не срабатывал никогда. Их счётчики, выходы и регистр шума сдвигаются арифметикой сразу на весь отсчёт,
// состояние то же, что после пошагового счёта.
//
// PSG считает та половина, у которой FM прошлого блока занял меньше тактов ESP (на ПК тактов нет — половина 1
// при включённом ЦАП, иначе 0): выбор — на блок, в md_snd_block_begin, до запуска половин; PSG в блоке считает
// ровно одна половина, поэтому звук от выбора не зависит.
#include "md_sound.h"
#include "ymfm/ymfm_opn.h"
#include <math.h>
#include <new>
#include <string.h>

namespace {

inline uint32_t cycles() {
#if defined(__XTENSA__)
    uint32_t c; __asm__ __volatile__("rsr %0, ccount" : "=a"(c)); return c;
#else
    return 0;
#endif
}

struct Intf : ymfm::ymfm_interface {};

struct Psg {
    uint16_t period[3], cnt[4];
    uint8_t vol[4], out[4], noise, latch_ch, latch_vol, sub;
    uint16_t lfsr;
    float amp[16];
    float level;                                         // сумма уровней каналов с выходом 1
    void relevel() {
        level = (out[0] ? amp[vol[0]] : 0.0f) + (out[1] ? amp[vol[1]] : 0.0f) +
                (out[2] ? amp[vol[2]] : 0.0f) + ((lfsr & 1) ? amp[vol[3]] : 0.0f);
    }
    void reset() {
        memset(period, 0, sizeof period); memset(cnt, 0, sizeof cnt); memset(out, 0, sizeof out);
        for (int c = 0; c < 4; c++) vol[c] = 15;
        noise = 0; latch_ch = 0; latch_vol = 0; sub = 0; lfsr = 0x8000;
        for (int i = 0; i < 15; i++) amp[i] = powf(10.0f, -0.1f * float(i));   // −2 дБ на шаг
        amp[15] = 0.0f;
        relevel();
    }
    void write(uint8_t d) {
        if (d & 0x80) { latch_ch = (d >> 5) & 3; latch_vol = (d >> 4) & 1; }
        if (latch_vol) { vol[latch_ch] = d & 15; relevel(); return; }
        if (latch_ch < 3) {
            if (d & 0x80) period[latch_ch] = uint16_t((period[latch_ch] & 0x3F0) | (d & 15));
            else period[latch_ch] = uint16_t((period[latch_ch] & 15) | ((d & 0x3F) << 4));
        } else { noise = d & 7; lfsr = 0x8000; relevel(); }
    }
    // шаг счётчиков (MCLK/240); true — сменился выход тона или шума
    inline bool step() {
        bool changed = false;
        for (int c = 0; c < 3; c++) {
            if (cnt[c] > 1) cnt[c]--;
            else { cnt[c] = period[c] > 1 ? period[c] : 1; out[c] ^= 1; changed = true; }
        }
        if (cnt[3] > 1) cnt[3]--;
        else {
            const uint16_t r = noise & 3;
            cnt[3] = r == 3 ? (period[2] > 1 ? period[2] : 1) : uint16_t(0x10 << r);
            out[3] ^= 1;
            if (out[3]) {                                    // регистр сдвигается на подъёме
                const uint16_t fb = (noise & 4) ? ((lfsr ^ (lfsr >> 3)) & 1) : (lfsr & 1);
                lfsr = uint16_t((lfsr >> 1) | (fb << 15));
                changed = true;
            }
        }
        return changed;
    }
    // k шагов тона разом: перезагрузка — через cnt шагов, дальше — каждые p; выход меняется на каждой
    inline void skip_tone(int c, unsigned k) {
        const unsigned p = period[c] > 1 ? period[c] : 1u, v = cnt[c];
        if (v > k) { cnt[c] = uint16_t(v - k); return; }
        const unsigned r = k - v;                        // шагов после первой перезагрузки
        cnt[c] = uint16_t(p - r % p);
        out[c] ^= uint8_t((1u + r / p) & 1u);
    }
    // то же для шума; регистр сдвигается на каждом подъёме выхода (переключение 0 -> 1)
    inline void skip_noise(unsigned k) {
        const unsigned rr = noise & 3u;
        const unsigned p = rr == 3u ? (period[2] > 1 ? period[2] : 1u) : (0x10u << rr), v = cnt[3];
        if (v > k) { cnt[3] = uint16_t(v - k); return; }
        const unsigned r = k - v, t = 1u + r / p;
        cnt[3] = uint16_t(p - r % p);
        unsigned rises = (t + (out[3] ? 0u : 1u)) / 2u;
        out[3] ^= uint8_t(t & 1u);
        while (rises--) {
            const uint16_t fb = (noise & 4) ? ((lfsr ^ (lfsr >> 3)) & 1) : (lfsr & 1);
            lfsr = uint16_t((lfsr >> 1) | (fb << 15));
        }
    }
    // отсчёт: 4 или 5 шагов (sub + 21 — от 21 до 25: 5 шагов только при 25), среднее уровня
    inline float sample() {
        const unsigned s = sub + 21u, k = s >= 25u ? 5u : 4u;
        sub = uint8_t(s - 5u * k);
        // пошагово — только если за k шагов переключится слышный канал; остальные — разом, уровень тот же
        if (!((vol[0] != 15 && cnt[0] <= k) || (vol[1] != 15 && cnt[1] <= k) ||
              (vol[2] != 15 && cnt[2] <= k) || (vol[3] != 15 && cnt[3] <= k))) {
            skip_tone(0, k); skip_tone(1, k); skip_tone(2, k); skip_noise(k);
            return level;
        }
        float acc = 0.0f;
        for (unsigned j = 0; j < k; j++) {
            if (step()) relevel();
            acc += level;
        }
        return acc * (k == 5u ? 0.2f : 0.25f);
    }
};

struct Engine {
    Intf intf0, intf1;
    ymfm::ym2612 ym0, ym1;
    Psg psg;
    md_snd_write_t w[MD_SND_MAX_WRITES];
    unsigned nw, n;
    int32_t lr0[MD_SND_BLOCK * 2], lr1[MD_SND_BLOCK * 2];
    float psg_out[MD_SND_BLOCK];
    float hp_x[2], hp_y[2];
    uint32_t cyc[2];
    uint32_t parts[5];                                   // такт 0, выход 0, такт 1, выход 1, PSG (последний блок)
    int psg_half;                                        // половина, считающая PSG в этом блоке
    struct { uint16_t addr; uint8_t mode; } sh[2];       // адрес YM ({часть, адрес}) и биты 7-6 регистра 27 у экземпляров
    Engine() : ym0(intf0), ym1(intf1), nw(0), n(0), parts{}, psg_half(0) { reset(); }
    void reset() {
        ym0.reset(); ym1.reset(); psg.reset();
        sh[0].addr = sh[1].addr = 0; sh[0].mode = sh[1].mode = 0;
        hp_x[0] = hp_x[1] = hp_y[0] = hp_y[1] = 0.0f;
    }
};

// MD.ELF 0.9 (03.11.2026, плата: Jungle Book на тяжёлых местах не успевал): драйвер ЦАП на каждый байт пишет и
// регистр таймеров 27 (сброс флага таймера A), около 5,5 тысячи раз в секунду. ymfm на запись 27 помечает все
// каналы изменёнными (engine_mode_write) и на следующем отсчёте пересчитывает кэш всех операторов половины.
// Таймеры для процессоров считает ядро (md_ym_stub), а таймеры ymfm на ESP не запущены вовсе (ни прерываний,
// ни CSM). Поэтому в ymfm запись 27 уходит только при смене режима канала 3 (биты 7-6).
void ym_write(ymfm::ym2612 &ym, decltype(Engine::sh[0]) &sh, const md_snd_write_t &e) {
    if (e.kind == 2) { ym.reset(); sh.addr = 0; sh.mode = 0; return; }
    if (e.kind != 0) return;
    if (!(e.port & 1)) sh.addr = uint16_t((e.port >> 1) << 8 | e.data);
    else if (e.port == 1 && sh.addr == 0x27) {
        if (((e.data ^ sh.mode) & 0xC0) == 0) return;
        sh.mode = e.data;
    }
    ym.write(e.port, e.data);
}

void *(*g_alloc)(size_t) = nullptr;
void (*g_release)(void *) = nullptr;

}  // namespace

extern "C" void md_snd_set_alloc(void *(*alloc)(size_t), void (*release)(void *)) { g_alloc = alloc; g_release = release; }

#if defined(__XTENSA__)
// Загрузчик ELF прошивки ESP не даёт стандартной библиотеки C++: new/delete — память приложения
// (как в TSFM.ELF); пути ymfm для сохранения состояния и журнала зовут помощники рантайма — заглушки.
void *operator new(size_t n) { return g_alloc ? g_alloc(n) : nullptr; }
void *operator new[](size_t n) { return g_alloc ? g_alloc(n) : nullptr; }
void operator delete(void *p) noexcept { if (p && g_release) g_release(p); }
void operator delete[](void *p) noexcept { if (p && g_release) g_release(p); }
void operator delete(void *p, size_t) noexcept { if (p && g_release) g_release(p); }
void operator delete[](void *p, size_t) noexcept { if (p && g_release) g_release(p); }
extern "C" void __cxa_pure_virtual() { for (;;) {} }
namespace std { void __throw_length_error(const char *) { for (;;) {} } }
#endif

// Снимок F5/F6 (MD.ELF 1.1): оба экземпляра ymfm — их save_restore (регистры, операторы, огибающие, фазы, LFO),
// PSG — поля счётчиков и регистров, «тень» адреса и режима 27 у экземпляров, фильтр смесителя (иначе выход
// после загрузки не повторит выход после снимка). Формат: 'M','S',версия 1, 0, длина ym0, длина ym1 (по 2 байта),
// байты ym0, ym1, PSG (28), тень (6), фильтр (4 float).
namespace {
const unsigned kPsgBytes = 28, kShBytes = 6, kHpBytes = 16;
void put16(uint8_t *&p, unsigned v) { *p++ = uint8_t(v); *p++ = uint8_t(v >> 8); }
unsigned get16(const uint8_t *&p) { const unsigned v = p[0] | p[1] << 8; p += 2; return v; }
}

extern "C" size_t md_snd_save(void *mem, uint8_t *buf, size_t cap) {
    Engine &e = *static_cast<Engine *>(mem);
    std::vector<uint8_t> v0, v1;
    { ymfm::ymfm_saved_state s(v0, true); e.ym0.save_restore(s); }
    { ymfm::ymfm_saved_state s(v1, true); e.ym1.save_restore(s); }
    const size_t need = 8 + v0.size() + v1.size() + kPsgBytes + kShBytes + kHpBytes;
    if (need > cap || v0.size() > 0xFFFF || v1.size() > 0xFFFF) return 0;
    uint8_t *p = buf;
    *p++ = 'M'; *p++ = 'S'; *p++ = 1; *p++ = 0;
    put16(p, unsigned(v0.size())); put16(p, unsigned(v1.size()));
    memcpy(p, v0.data(), v0.size()); p += v0.size();
    memcpy(p, v1.data(), v1.size()); p += v1.size();
    const Psg &g = e.psg;
    for (int i = 0; i < 3; i++) put16(p, g.period[i]);
    for (int i = 0; i < 4; i++) put16(p, g.cnt[i]);
    for (int i = 0; i < 4; i++) *p++ = g.vol[i];
    for (int i = 0; i < 4; i++) *p++ = g.out[i];
    *p++ = g.noise; *p++ = g.latch_ch; *p++ = g.latch_vol; *p++ = g.sub;
    put16(p, g.lfsr);
    for (int h = 0; h < 2; h++) { put16(p, e.sh[h].addr); *p++ = e.sh[h].mode; }
    memcpy(p, e.hp_x, 8); memcpy(p + 8, e.hp_y, 8); p += kHpBytes;
    return size_t(p - buf);
}

extern "C" int md_snd_load(void *mem, const uint8_t *buf, size_t n) {
    Engine &e = *static_cast<Engine *>(mem);
    if (n < 8 || buf[0] != 'M' || buf[1] != 'S' || buf[2] != 1) return 0;
    const uint8_t *p = buf + 4;
    const size_t n0 = get16(p), n1 = get16(p);
    if (n != 8 + n0 + n1 + kPsgBytes + kShBytes + kHpBytes) return 0;
    {
        std::vector<uint8_t> v(p, p + n0);
        ymfm::ymfm_saved_state s(v, false); e.ym0.save_restore(s);
        p += n0;
    }
    {
        std::vector<uint8_t> v(p, p + n1);
        ymfm::ymfm_saved_state s(v, false); e.ym1.save_restore(s);
        p += n1;
    }
    Psg &g = e.psg;
    for (int i = 0; i < 3; i++) g.period[i] = uint16_t(get16(p));
    for (int i = 0; i < 4; i++) g.cnt[i] = uint16_t(get16(p));
    for (int i = 0; i < 4; i++) g.vol[i] = *p++;
    for (int i = 0; i < 4; i++) g.out[i] = *p++;
    g.noise = *p++; g.latch_ch = *p++; g.latch_vol = *p++; g.sub = *p++;
    g.lfsr = uint16_t(get16(p));
    g.relevel();
    for (int h = 0; h < 2; h++) { e.sh[h].addr = uint16_t(get16(p)); e.sh[h].mode = *p++; }
    memcpy(e.hp_x, p, 8); memcpy(e.hp_y, p + 8, 8);
    return 1;
}

extern "C" size_t md_snd_size(void) { return sizeof(Engine); }
extern "C" void md_snd_init(void *mem) { new (mem) Engine(); }
extern "C" void md_snd_reset(void *mem) { static_cast<Engine *>(mem)->reset(); }

extern "C" int md_snd_block_begin(void *mem, const md_snd_write_t *w, unsigned nw, unsigned n) {
    Engine &e = *static_cast<Engine *>(mem);
    if (nw > MD_SND_MAX_WRITES || n > MD_SND_BLOCK) return 0;
    memcpy(e.w, w, nw * sizeof *w);
    e.nw = nw; e.n = n;
    // PSG — половине, у которой FM прошлого блока занял меньше тактов (ESP); без счётчика тактов (ПК) — по ЦАП
    const uint32_t fm0 = e.parts[0] + e.parts[1], fm1 = e.parts[2] + e.parts[3];
    e.psg_half = (fm0 || fm1) ? (fm0 > fm1 ? 1 : 0) : (e.ym1.dac_enabled() ? 1 : 0);
    return 1;
}

extern "C" void md_snd_render_half(void *mem, int half) {
    Engine &e = *static_cast<Engine *>(mem);
    const uint32_t t0 = cycles();
    ymfm::ym2612 &ym = half ? e.ym1 : e.ym0;
    const uint32_t mask = half ? 0x38u : 0x07u;
    int32_t *lr = half ? e.lr1 : e.lr0;
    const bool psg = half == e.psg_half;
    ym.m_cyc_clock = ym.m_cyc_out = 0;
    // YM: записи PSG — во втором проходе
    unsigned k = 0;
    for (unsigned i = 0; i < e.n; i++) {
        while (k < e.nw && e.w[k].pos <= i) {
            const md_snd_write_t &x = e.w[k++];
            if (x.kind != 1) ym_write(ym, e.sh[half], x);
        }
        ym.generate_part(lr[2 * i], lr[2 * i + 1], mask);
    }
    // записи после последнего отсчёта блока — не должно быть, но не потерять
    for (; k < e.nw; k++) if (e.w[k].kind != 1) ym_write(ym, e.sh[half], e.w[k]);
    e.parts[2 * half] = ym.m_cyc_clock; e.parts[2 * half + 1] = ym.m_cyc_out;
    if (psg) {                                           // PSG: свой проход по блоку
        const uint32_t p0 = cycles();
        k = 0;
        for (unsigned i = 0; i < e.n; i++) {
            while (k < e.nw && e.w[k].pos <= i) { if (e.w[k].kind == 1) e.psg.write(e.w[k].data); k++; }
            e.psg_out[i] = e.psg.sample();
        }
        for (; k < e.nw; k++) if (e.w[k].kind == 1) e.psg.write(e.w[k].data);
        e.parts[4] = cycles() - p0;
    }
    e.cyc[half] = cycles() - t0;
}

extern "C" void md_snd_mix(void *mem, float *out, float gain) {
    Engine &e = *static_cast<Engine *>(mem);
    // YM: канал — ±256, шесть — ±1536; PSG: канал 0..1, вес 0,2 (размах канала YM — 0,33)
    const float k_fm = 1.0f / 1536.0f, k_psg = 0.2f;
    const float R = 0.99766f;                          // фильтр постоянной составляющей ~20 Гц
    for (unsigned i = 0; i < e.n; i++) {
        const float p = e.psg_out[i] * k_psg;
        for (int c = 0; c < 2; c++) {
            const float x = float(e.lr0[2 * i + c] + e.lr1[2 * i + c]) * k_fm + p;
            const float y = x - e.hp_x[c] + R * e.hp_y[c];
            e.hp_x[c] = x; e.hp_y[c] = y;
            float v = y * gain;
            out[2 * i + c] = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
        }
    }
}

extern "C" void md_snd_cycles(const void *mem, uint32_t *half0, uint32_t *half1) {
    const Engine &e = *static_cast<const Engine *>(mem);
    *half0 = e.cyc[0]; *half1 = e.cyc[1];
}

// Микрозамеры (MD.ELF 0.6, 03.11.2026): на плате код синтеза стоит около 10 тактов на команду даже в тёплом
// цикле (PSG — ~750 тактов на отсчёт при ~45 командах), а код модуля исполняется из PSRAM. Каждый замер — 4096
// итераций одного вида работы: только регистры, чтение внутренней памяти, чтение PSRAM (.bss модуля), загрузка
// литерала l32r, вызов функции; последний — частота процессора по millis (2 млн итераций регистрового цикла).
namespace {
uint32_t g_psram_words[64];                              // .bss модуля — в PSRAM (CONFIG_ELF_LOADER_LOAD_PSRAM)
volatile uint32_t g_sink;
#if defined(__GNUC__)
__attribute__((noinline))
#else
__declspec(noinline)
#endif
uint32_t mb_leaf(uint32_t x) { return x * 5u + 1u; }
}

#if !defined(__XTENSA__)
extern "C" void md_cpu_bench(void *, uint32_t out[6], uint32_t (*)(void)) {   // ПК: замер только на ESP
    for (int i = 0; i < 6; i++) out[i] = 0;
    g_sink = mb_leaf(0);
}
#else
extern "C" void md_cpu_bench(void *mem, uint32_t out[6], uint32_t (*ms)(void)) {
    Engine &e = *static_cast<Engine *>(mem);
    volatile int32_t *iram = e.lr0;                      // движок — во внутренней памяти
    volatile uint32_t *psram = g_psram_words;
    const unsigned N = 4096;
    uint32_t x = 1, t;
    t = cycles();
    for (unsigned i = 0; i < N; i++) { x = x * 3u + i; __asm__ __volatile__("" : "+a"(x)); }
    out[0] = cycles() - t;
    t = cycles();
    for (unsigned i = 0; i < N; i++) x += uint32_t(iram[i & 63]);
    out[1] = cycles() - t;
    t = cycles();
    for (unsigned i = 0; i < N; i++) x += psram[i & 63];
    out[2] = cycles() - t;
    t = cycles();
    for (unsigned i = 0; i < N; i++) {
        uint32_t c;
#if defined(__XTENSA__)
        __asm__ __volatile__(".literal .Lmd_mb%=, 0x9E3779B9\n\tl32r %0, .Lmd_mb%=" : "=a"(c));
#else
        c = 0x9E3779B9u;
#endif
        x += c;
    }
    out[3] = cycles() - t;
    t = cycles();
    for (unsigned i = 0; i < N; i++) { x = mb_leaf(x); __asm__ __volatile__("" : "+a"(x)); }
    out[4] = cycles() - t;
    const uint32_t m0 = ms();
    t = cycles();
    for (unsigned i = 0; i < 2000000u; i++) { x = x * 3u + i; __asm__ __volatile__("" : "+a"(x)); }
    const uint32_t c6 = cycles() - t, dm = ms() - m0;
    out[5] = dm ? c6 / dm / 1000u : 0;                   // МГц
    g_sink = x;
}
#endif

// Замер на месте (MD.ELF 0.5): три канала FM звучат (алгоритм 4, обратная связь), два тона PSG; половина 0
// считает три блока подряд — первый с «холодным» кэшем команд и данных, второй и третий — с тёплым. Сравнение
// с тактами половины в игре показывает, сколько отнимают промахи кэша (код модуля исполняется из PSRAM).
// После замера движок сбрасывается. out: всего, такт, выход, PSG — по три блока (12 чисел).
extern "C" void md_snd_bench(void *mem, uint32_t out[12]) {
    Engine &e = *static_cast<Engine *>(mem);
    e.reset();
    auto wr = [&](uint8_t reg, uint8_t val) { e.ym0.write(0, reg); e.ym0.write(1, val); };
    for (int c = 0; c < 3; c++) {
        for (int op = 0; op < 4; op++) {
            const uint8_t o = uint8_t(c + 4 * op);
            wr(uint8_t(0x30 + o), uint8_t(0x01 + op * 2)); wr(uint8_t(0x40 + o), uint8_t(op == 3 ? 0x08 : 0x20));
            wr(uint8_t(0x50 + o), 0x1F); wr(uint8_t(0x60 + o), 0x05); wr(uint8_t(0x70 + o), 0x02); wr(uint8_t(0x80 + o), 0x1F);
        }
        wr(uint8_t(0xB0 + c), 0x3C); wr(uint8_t(0xB4 + c), 0xC0);
        wr(uint8_t(0xA4 + c), 0x22); wr(uint8_t(0xA0 + c), uint8_t(0x69 + 37 * c)); wr(0x28, uint8_t(0xF0 | c));
    }
    e.psg.write(0x8E); e.psg.write(0x0F); e.psg.write(0x92); e.psg.write(0xA5); e.psg.write(0x1A); e.psg.write(0xB4);
    for (int b = 0; b < 3; b++) {
        e.nw = 0; e.n = MD_SND_BLOCK; e.psg_half = 0;
        md_snd_render_half(mem, 0);
        out[4 * b] = e.cyc[0]; out[4 * b + 1] = e.parts[0]; out[4 * b + 2] = e.parts[1]; out[4 * b + 3] = e.parts[4];
    }
    e.reset();
    for (int i = 0; i < 5; i++) e.parts[i] = 0;
}

extern "C" void md_snd_parts(const void *mem, uint32_t parts[5], int *psg_half) {
    const Engine &e = *static_cast<const Engine *>(mem);
    for (int i = 0; i < 5; i++) parts[i] = e.parts[i];
    *psg_half = e.psg_half;
}
