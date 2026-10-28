// Стенд Verilator ядра видео целиком (vp_core): хост — модель плагина FTVIEW4 событиями ft_spi
// (сброс REG_CPURESET, REG_ID, блок состояния, подача AVI в медиа-FIFO: REG_MEDIAFIFO_READ →
// блок в окно 0xE0000 → REG_MEDIAFIFO_WRITE; блоки — случайной длины, кратной 4, как хвосты
// чанков у плагина: неполные группы из 8 слов медиа-FIFO в SDRAM), SDRAM — модель клиента
// sdram_burst (запись с маской слов, чтение; изредка — долгая занятость), читатель кадров
// (fb_core) переходит на объявленный буфер с каждым выходным кадром (vtick). Медиа-FIFO ядра
// (1.38) — в той же SDRAM; сборка с -GFIFO_AW=10 (4 КБ) проверяет его кольцо.
// Кадр больше 1024×768 ждём вдвое меньше: точки эталона с чётными x и y (vp_fbwrite 1.38).
//
// Каждый объявленный кадр сверяется с эталоном (RGB888 → RGB565) точка в точку, порядок
// слотов темпа — с ожидаемым (make_vp_avi.py: «кадр ЭТАЛОН» / «повтор», в конце «отменено N»),
// и каждый слот должен выйти ровно через период кадра (40 мс = 2 выходных кадра по 20 мс).
//
// --audio: модель приложения ESP включает приём звука (подтверждает эпоху, читает FIFO звука
// окном раз в ~0,3 мс «времени ядра») — принятые байты сверяются с ожидание.aud (make_vp_avi.py);
// --aclk: ещё и темп по часам звука (ESP пишет время звука) — слот n не раньше n·40 мс.
// --pause N: после слота N плагин ставит паузу (REG_CPURESET бит 2) на 10 выходных кадров —
// слоты стоят, бит виден в REG_CPURESET и слове 7 блока состояния, следующий слот — через
// 2 выходных кадра темпа плюс 10 кадров паузы (без догоняющих).
//
// --no-pace: темп не сверять (поток hd: кадр HD — 118–161 КБ, подача стенда — байт за 36 тактов,
// она и задаёт темп; проверяются картинки и звук).
//
//   tb_vpc.exe поток.avi ожидание.txt [--audio] [--aclk] [--pause N] [--no-pace]
#include "Vvp_core.h"
#include "verilated.h"
#ifdef VP_TRACE   // сборка с --public-flat-rw: журнал чанков разбора и состояние при остановке
#include "Vvp_core___024root.h"
#include "Vvp_core_vp_core.h"
#include "Vvp_core_jpeg_core__S1.h"
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

double sc_time_stamp() { return 0; }

static const uint32_t FB_BASE = 0x128000, FT_BUF_WORDS = 0x060000, T_LO = 0x080000, T_HI = 0x0C9000;
static const unsigned VT_CYC = 500000;    // выходной кадр стенда (20 мс «ядра» — в тактах стенда)
static const unsigned EV_CYC = 36;        // событие SPI: байт при SCK 14 МГц (8 тактов SCK)

static uint32_t line_base(unsigned b, unsigned y)
{
    if (b == 0) return FB_BASE + y * 512;
    if (b == 1) return FB_BASE + FT_BUF_WORDS + y * 512;
    return y < 512 ? T_LO + y * 512 : T_HI + (y - 512) * 512;
}

static std::vector<uint8_t> load(const std::string &p)
{
    std::vector<uint8_t> v;
    std::FILE *f = std::fopen(p.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "нет %s\n", p.c_str()); std::exit(2); }
    for (int c; (c = std::fgetc(f)) != EOF;) v.push_back((uint8_t)c);
    std::fclose(f);
    return v;
}

struct Bench {
    Vvp_core t;
    std::vector<uint32_t> mem = std::vector<uint32_t>(1u << 21, 0xDEADBEEF);
    uint64_t cyc = 0;
    int sd_state = 0, sd_wait = 0, sd_n = 0;
    uint32_t sd_addr = 0;
    bool sd_we = false;
    uint64_t sd_reads = 0, sd_writes = 0;
    unsigned seed = 777;
    bool showing = false;
    unsigned front = 0;
    // слоты: такт vtick, после которого вышел слот; объявления кадров
    uint64_t vticks = 0;
    uint32_t last_slots = 0, last_frames = 0;
    int pend = 0;
    struct Slot { uint64_t vt; bool frame; unsigned buf, w, h; std::vector<uint32_t> img; uint64_t cyc; };
    std::vector<Slot> slots;
    // модель приложения ESP: звук и часы звука
    bool esp_audio = false, esp_aclk = false;
    unsigned esp_rp = 0, esp_ack = 0;
    uint64_t aclk_start = 0;
    std::vector<uint8_t> audio;
    void esp_step()
    {
        if (!esp_audio) return;
        t.au_en = 1;
        if (t.au_epoch != esp_ack) {                    // новый поток: свой указатель — в 0
            esp_ack = t.au_epoch; esp_rp = 0; audio.clear(); aclk_start = cyc;
            t.au_rp = 0; t.au_ack_epoch = esp_ack;
        }
        if (esp_aclk) {                                 // время звука: такты стенда → мкс «ядра»
            t.aclk_en = 1;
            t.aclk_us = (uint32_t)((cyc - aclk_start) * 20000ull / VT_CYC);
        }
        if (cyc % 20000) return;                        // опрос FIFO — пачкой
        while (((t.au_wp - esp_rp) & 2047) != 0) {
            t.au_rd_addr = esp_rp & 1023;
            t.au_rd_clk = 0; t.eval();
            t.au_rd_clk = 1; t.eval();
            uint32_t w = t.au_rd_q;
            for (int k = 0; k < 4; k++) audio.push_back((uint8_t)(w >> (8 * k)));
            esp_rp = (esp_rp + 1) & 2047;
        }
        t.au_rp = esp_rp;
    }

    unsigned rnd() { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7FFF; }

    void tick()
    {
        t.clk = 0;
        t.s_ack = sd_state == 2;
        t.s_wr_take = sd_state == 4;
        t.s_rd_valid = sd_state == 5;
        t.s_rd_data = sd_state == 5 ? mem[(sd_addr & ~7u) + sd_n] : 0xA5A5A5A5u;
        t.fb_showing = showing;
        t.fb_front = front;
        t.vtick = cyc % VT_CYC == 0 && cyc;
        t.eval();
        // маска: ядро пишет слова целиком (F) или пропускает (0)
        if (sd_state == 4 && t.s_wmask == 0xF) mem[(sd_addr & ~7u) + sd_n] = t.s_wdata;
        if (sd_state == 4 && t.s_wmask != 0xF && t.s_wmask != 0) { std::printf("МАСКА %X\n", t.s_wmask); std::exit(1); }
        t.clk = 1;
        t.eval();
        switch (sd_state) {
        case 0:
            if (t.s_req) {
                sd_state = 1;
                sd_wait = 1 + rnd() % 6;
                if (rnd() % 64 == 0) sd_wait += rnd() % 600;    // разъём долго занят (вывод, оверлеи)
            }
            break;
        case 1: if (--sd_wait == 0) { sd_state = 2; sd_addr = t.s_addr; sd_we = t.s_we; } break;
        case 2: sd_state = 3; sd_wait = 2; (sd_we ? sd_writes : sd_reads)++; break;
        case 3: if (--sd_wait == 0) { sd_state = sd_we ? 4 : 5; sd_n = 0; } break;
        case 4: case 5: if (++sd_n == 8) sd_state = 0; break;
        }
        if (cyc % VT_CYC == 0 && cyc) {
            vticks++;
            if (t.pub_v) { showing = true; front = t.pub_buf; }   // fb_core: с начала кадра
        }
        // новый слот темпа (счётчик слотов — на такт раньше объявления кадра: кадр это или
        // повтор, видно через такт-два)
        if (t.st_slots != last_slots) {
            last_slots = t.st_slots;
            slots.push_back(Slot{vticks, false, 0, 0, 0, {}, cyc});
            pend = 3;
        }
        if (pend && --pend == 0 && t.st_frames != last_frames) {
            last_frames = t.st_frames;
            Slot &s = slots.back();
            s.frame = true;
            // объявленный кадр снимаем сразу: его буфер не пишется, пока он объявлен
            s.buf = t.pub_buf; s.w = t.pub_w; s.h = t.pub_h;
            s.img.resize((size_t)s.w * s.h);
            for (unsigned y = 0; y < s.h; y++)
                for (unsigned x = 0; x < s.w; x++) {
                    uint32_t word = mem[line_base(s.buf, y) + x / 2];
                    s.img[(size_t)y * s.w + x] = (x & 1) ? word >> 16 : word & 0xFFFF;
                }
        }
        esp_step();
#ifdef VP_TRACE
        {
            Vvp_core_vp_core *v = t.rootp->vp_core;
            unsigned pst = v->avi__DOT__pst;
            if (pst != last_pst && last_pst == 0) {      // заголовок чанка разобран
                uint64_t h = v->avi__DOT__hdr;
                char fcc[5] = {(char)(h & 0xFF), (char)(h >> 8 & 0xFF), (char)(h >> 16 & 0xFF), (char)(h >> 24 & 0xFF), 0};
                for (int i = 0; i < 4; i++) if (fcc[i] < 32 || fcc[i] > 126) fcc[i] = '?';
                std::printf("  такт %llu: чанк %s размер %u → состояние %u; READ %u WRITE %u выборка %u\n",
                            (unsigned long long)cyc, fcc, (unsigned)(h >> 32), pst, v->rd_pos, v->wr_pos, v->fw);
            }
            last_pst = pst;
        }
#endif
        cyc++;
    }
    unsigned last_pst = 0;
    void dump_state()
    {
#ifdef VP_TRACE
        Vvp_core_vp_core *v = t.rootp->vp_core;
        std::printf("  разбор: состояние %u осталось %u маркеры %u вставка %u упаковка %u очередь %u; запись: кадр %u SDRAM %u; "
                    "вход декодера %u; READ %u WRITE %u выборка %u в очереди %u сброс %u\n",
                    v->avi__DOT__pst, v->avi__DOT__left, v->avi__DOT__jst, v->avi__DOT__inj, v->avi__DOT__pk_n,
                    v->avi__DOT__oq_n, v->wr__DOT__fst, v->wr__DOT__wst, v->dec->u_jpeg_input__DOT__state_q,
                    v->rd_pos, v->wr_pos, v->fw, v->ob_v, v->creset);
#endif
    }
    void wait(unsigned n) { while (n--) tick(); }

    // события ft_spi
    void ev(unsigned kind, uint8_t h0, uint8_t h1, uint8_t b)
    {
        t.spi_kind = kind; t.spi_h0 = h0; t.spi_h1 = h1; t.spi_b = b;
        t.spi_tg = !t.spi_tg;
        wait(EV_CYC);
    }
    void write(uint32_t addr, const uint8_t *d, size_t n)
    {
        ev(0, 0x80 | (addr >> 16 & 0x3F), addr >> 8 & 0xFF, addr & 0xFF);
        for (size_t i = 0; i < n; i++) ev(1, 0, 0, d[i]);
        wait(EV_CYC);                                   // CS поднят
    }
    void write32(uint32_t addr, uint32_t v)
    {
        uint8_t d[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
        write(addr, d, 4);
    }
    void write8(uint32_t addr, uint8_t v) { write(addr, &v, 1); }
    uint32_t read32(uint32_t addr)
    {
        ev(0, addr >> 16 & 0x3F, addr >> 8 & 0xFF, addr & 0xFF);   // + байт-пустышка
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            v |= (uint32_t)t.spi_rd_byte << (8 * i);    // байт, готовый к концу предыдущего
            ev(2, 0, 0, 0);
        }
        wait(EV_CYC);
        return v;
    }
};

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0);       // ход прогона виден сразу
    if (argc < 3) { std::fprintf(stderr, "tb_vpc.exe поток.avi ожидание.txt\n"); return 2; }
    std::vector<uint8_t> avi = load(argv[1]);
    std::vector<std::string> exp;
    {
        std::ifstream f(argv[2]);
        for (std::string l; std::getline(f, l);) if (!l.empty()) exp.push_back(l);
    }
    std::string last_exp = exp.back();
    exp.pop_back();

    Bench b;
    long pause_at = -1;
    bool no_pace = false;                            // --no-pace: темп не сверять                              // --pause: после этого слота
    size_t pause_slot = 0;                           // индекс слота после паузы
    uint64_t pause_vt = 0;                           // выходных кадров за паузу
    for (int i = 3; i < argc; i++) {
        if (!std::strcmp(argv[i], "--audio")) b.esp_audio = true;
        if (!std::strcmp(argv[i], "--aclk")) b.esp_audio = b.esp_aclk = true;
        if (!std::strcmp(argv[i], "--pause") && i + 1 < argc) pause_at = std::atol(argv[++i]);
        if (!std::strcmp(argv[i], "--no-pace")) no_pace = true;
    }
    b.t.rst_n = 0; b.t.spi_tg = 0;
    b.wait(10);
    b.t.rst_n = 1;
    b.wait(10);

    int bad = 0;
    if ((b.read32(0x302000) & 0xFF) != 0x7C) { std::printf("REG_ID не 7C\n"); bad++; }
    b.write8(0x302020, 1);
    b.write8(0x302020, 0);
    uint32_t magic = b.read32(0x000000), size = b.read32(0x000004);
    std::printf("состояние: %08X, FIFO %u байтов\n", magic, size);
    if (magic != 0x31444956 || size == 0 || (size & (size - 1))) { std::printf("блок состояния неверен\n"); return 1; }

    // подача, как avi_feed: блоки до 512 байтов, кратные 4 (хвост файла — добивка нулями)
    uint32_t wpos = 0;
    size_t pos = 0;
    uint64_t c0 = b.cyc, moved_at = b.cyc;
    uint32_t last_rpos = ~0u;
    while (pos < avi.size()) {
        if (pause_at >= 0 && b.slots.size() == (size_t)pause_at + 1) {
            // пауза: 10 выходных кадров слоты стоят (подача тоже — как у плагина на паузе)
            pause_at = -1;
            b.write8(0x302020, 4);
            const size_t n0 = b.slots.size();
            const uint64_t v0 = b.vticks;
            const uint32_t reg = b.read32(0x302020) & 0xFF, st7 = b.read32(0x00001C);
            b.wait(10 * VT_CYC);
            pause_vt = b.vticks - v0;
            pause_slot = n0;
            std::printf("пауза после слота %zu: выходных кадров %llu, слотов за паузу %zu, REG_CPURESET %02X, слово 7 %08X\n",
                        n0 - 1, (unsigned long long)pause_vt, b.slots.size() - n0, reg, st7);
            if (b.slots.size() != n0 || reg != 4 || !(st7 & 8)) { std::printf("ПАУЗА НЕ ДЕРЖИТ\n"); bad++; }
            b.write8(0x302020, 0);
        }
        uint32_t rpos = b.read32(0x309014);
        uint32_t free = (rpos - wpos - 4) & (size - 1);
        size_t count = std::min<size_t>(avi.size() - pos, 4 * (1 + b.rnd() % 128));
        uint32_t padded = (uint32_t)(count + 3) & ~3u;
        if (padded > size - wpos) padded = size - wpos, count = padded;
        if (free < padded) {
            b.wait(2000);
            if (rpos != last_rpos) { last_rpos = rpos; moved_at = b.cyc; }
            if (b.cyc - moved_at > 10000000ull) {           // READ стоит 10 млн тактов: поток встал
                std::printf("ПОТОК ВСТАЛ: подано %zu из %zu, READ %u WRITE %u, слотов %u, кадров %u, отменено %u, флаги %08X\n",
                            pos, avi.size(), rpos, wpos, b.read32(0x000008), b.read32(0x00000C),
                            b.read32(0x000010), b.read32(0x00001C));
                b.dump_state();
                return 1;
            }
            continue;
        }
        std::vector<uint8_t> blk(padded, 0);
        std::memcpy(blk.data(), &avi[pos], std::min<size_t>(count, avi.size() - pos));
        b.write(0xE0000 + wpos, blk.data(), padded);
        wpos = (wpos + padded) & (size - 1);
        b.write32(0x309018, wpos);
        pos += count;
    }
    std::printf("поток подан за %llu тактов\n", (unsigned long long)(b.cyc - c0));
    // дождаться всех слотов
    for (uint64_t lim = b.cyc + 20ull * VT_CYC; b.slots.size() < exp.size() && b.cyc < lim;) b.tick();
    b.wait(4 * VT_CYC);

    uint32_t aborts = b.read32(0x000010), slots_reg = b.read32(0x000008), frames_reg = b.read32(0x00000C);
    const uint32_t st7 = b.read32(0x00001C);
    std::printf("слотов %zu (регистр %u), кадров %u, отменено %u, вставок DHT %u; SDRAM: пакетов записи %llu, "
                "чтения %llu\n", b.slots.size(), slots_reg, frames_reg, aborts, st7 >> 16,
                (unsigned long long)b.sd_writes, (unsigned long long)b.sd_reads);
    if (st7 & 0x30) { std::printf("медиа-FIFO: %s%s\n", st7 & 0x10 ? "приёмник переполнялся " : "",
                                  st7 & 0x20 ? "запись не подряд" : ""); bad++; }
    if (!b.sd_reads) { std::printf("медиа-FIFO не читался из SDRAM\n"); bad++; }

    for (size_t i = 0; i < std::max(b.slots.size(), exp.size()); i++) {
        if (i >= b.slots.size()) { std::printf("слот %zu: нет (ждали «%s»)\n", i, exp[i].c_str()); bad++; continue; }
        const Bench::Slot &s = b.slots[i];
        long dvt = i ? (long)(s.vt - b.slots[i - 1].vt) : 0;
        if (i >= exp.size()) { std::printf("слот %zu: лишний\n", i); bad++; continue; }
        bool want_frame = exp[i].rfind("кадр ", 0) == 0;
        std::printf("слот %2zu: выходных кадров от прошлого %ld, %s", i, dvt, s.frame ? "кадр" : "повтор");
        if (s.frame != want_frame) { std::printf(" — ждали «%s»\n", exp[i].c_str()); bad++; continue; }
        // первый кадр объявляется посреди выходного кадра и виден со следующего: до второго —
        // 3 счёта (показан он, как все, два выходных кадра — это проверяет системный стенд vvid)
        if (b.esp_aclk) {
            // часы звука: слот n — не раньше n·40 мс звука (кадр мог и опоздать: поток, декодер)
            uint64_t due = b.aclk_start + (uint64_t)i * 2 * VT_CYC;
            std::printf(" (после срока на %.1f мс)", s.cyc >= due ? (s.cyc - due) * 20.0 / VT_CYC : -1.0);
            if (s.cyc + 2 < due) { std::printf(" — РАНЬШЕ СРОКА"); bad++; }
        } else {
            const long want_dvt = (pause_slot && i == pause_slot) ? 2 + (long)pause_vt : 2;
            if (!no_pace && i > 1 && dvt != want_dvt) { std::printf(" — темп не %ld", want_dvt); bad++; }
            if (!no_pace && i == 1 && dvt != 2 && dvt != 3) { std::printf(" — темп не 2–3"); bad++; }
        }
        if (s.frame) {
            // «кадр ЭТАЛОН ШИРИНА ВЫСОТА»: картинка — на холсте, как в vp_fbwrite, поля чёрные
            char path[1024];
            unsigned w = 0, h = 0;
            std::sscanf(exp[i].c_str() + strlen("кадр "), "%1023s %u %u", path, &w, &h);
            std::vector<uint8_t> full = load(path);
            if (full.size() != (size_t)w * h * 3) {
                std::printf(" эталон %zu байтов, ждали %ux%u\n", full.size(), w, h);
                bad++;
                continue;
            }
            // больше 1024×768 — вдвое меньше: точки с чётными x и y (vp_fbwrite 1.38)
            std::vector<uint8_t> ref = full;
            if (w > 1024 || h > 768) {
                const unsigned hw = (w + 1) / 2, hh = (h + 1) / 2;
                ref.assign((size_t)hw * hh * 3, 0);
                for (unsigned y = 0; y < hh; y++)
                    for (unsigned x = 0; x < hw; x++)
                        std::memcpy(&ref[3 * ((size_t)y * hw + x)], &full[3 * ((size_t)(2 * y) * w + 2 * x)], 3);
                std::printf(" (%ux%u вдвое меньше)", w, h);
                w = hw; h = hh;
            }
            // широкая (9·w ≥ 14·h, до 576 строк) — холст 16:9 (vp_fbwrite 1.36)
            const bool wd = 9 * w >= 14 * h && h <= 576;
            unsigned cw = wd ? ((w <= 512 && h <= 288) ? 512 : (w <= 640 && h <= 360) ? 640 : 1024)
                             : ((w <= 512 && h <= 384) ? 512 : (w <= 640 && h <= 480) ? 640 : 1024);
            unsigned ch = wd ? (cw == 512 ? 288 : cw == 640 ? 360 : 576) : (cw == 512 ? 384 : cw == 640 ? 480 : 768);
            unsigned ox = ((cw - w) / 2) & ~15u, oy = (ch - h) / 2;
            if (s.w != cw || s.h != ch || ref.size() != (size_t)w * h * 3) {
                std::printf(" холст %ux%u, ждали %ux%u (картинка %ux%u)\n", s.w, s.h, cw, ch, w, h);
                bad++;
                continue;
            }
            size_t diff = 0;
            for (unsigned y = 0; y < ch; y++)
                for (unsigned x = 0; x < cw; x++) {
                    uint16_t e = 0;
                    if (x >= ox && x < ox + w && y >= oy && y < oy + h) {
                        const uint8_t *p = &ref[3 * ((size_t)(y - oy) * w + (x - ox))];
                        e = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
                    }
                    diff += s.img[(size_t)y * cw + x] != e;
                }
            std::printf(" %ux%u на холсте %ux%u, буфер %u, расхождений %zu", w, h, cw, ch, s.buf, diff);
            if (diff) bad++;
        }
        std::printf("\n");
    }
    if (b.esp_audio) {
        std::string ap = argv[2];
        ap = ap.substr(0, ap.rfind('.')) + ".aud";
        std::vector<uint8_t> want = load(ap);
        size_t n = want.size() & ~(size_t)3;            // хвост неполного слова остаётся в упаковке
        size_t diff = 0;
        for (size_t i = 0; i < std::min(n, b.audio.size()); i++) diff += want[i] != b.audio[i];
        std::printf("звук: принято %zu байтов, ждали %zu, расхождений %zu\n", b.audio.size(), n, diff);
        if (b.audio.size() != n || diff) bad++;
    }
    unsigned want_ab = (unsigned)std::atoi(last_exp.c_str() + last_exp.rfind(' ') + 1);
    if (aborts != want_ab) { std::printf("отменено %u, ждали %u\n", aborts, want_ab); bad++; }
    std::printf("%s\n", bad ? "ЕСТЬ ОШИБКИ" : "ВСЁ ВЕРНО");
    return bad ? 1 : 0;
}
