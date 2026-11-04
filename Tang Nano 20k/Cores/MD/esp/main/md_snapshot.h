/* SPDX-License-Identifier: GPL-3.0-or-later
 * Снимки F5/F6 (MD.ELF 1.1, ядро Mega Drive 0.6, 03.11.2026) — сохранить и загрузить игру в любой момент.
 *
 * Внутреннее состояние процессоров ядро наружу не отдаёт, поэтому:
 *  - 68000 сам сохраняет и загружает свои регистры: ядро даёт прерывание уровня 7 с вектором во вставку $A12000
 *    (1К слов, LINK 5c–5f), где лежит обработчик (md_snap_68k.h — собирает tests/make_snap_handler.py). Он кладёт
 *    D0–A6, USP и SSP в буфер вставки и исполняет сценарии ESP — записи и чтения шины 68000 (порты VDP и ввода,
 *    линии Z80). SR и PC — кадр исключения на стеке в ОЗУ: он едет вместе с ОЗУ;
 *  - Z80 встаёт своим NMI во вставку $0066 (LINK 60), регистры — T80 REG/DIR (61–67);
 *  - VDP: регистры, ADDR, CODE, PENDING — отвод (6d); CRAM и VSRAM — сценарием через порт данных;
 *  - ОЗУ 68000, видеопамять, SRAM — мост SDRAM; ОЗУ Z80 — тот же мост (бит 14 команды); банки и таймеры YM — 68–6c;
 *  - звук — состояние синтеза ESP (md_snd_save / md_snd_load) в потоке звука между блоками (md_audio.h).
 * Порядок проверен на модели всей машины (tests/md_model.cpp, MD_SNAP): после загрузки кадры и события звука
 * повторяют то, что было после снимка. Таймеры YM ядра на время снимка стоят (SNAP_ACT), время звука тоже.
 *
 * Файл <образ>.sst рядом с образом, как у NES.ELF: карта Tang — файловым API прошивки, карта Evo — плагином
 * VDAC4 Cores (запросы 3–6 в ответе на ВВОД). Загрузка — только для той же игры (CRC32 образа).
 * .sst: заголовок 64 байта {"MDV4SST1", версия, CRC32 образа, флаги [0] Z80 работает, [1] шина Z80 у 68000,
 * [2] VDP ждёт второе слово команды, длина состояния звука, слов SRAM}, тело (SS_*), SRAM, звук, CRC32 всего
 * прежнего; длина дополнена нулями до 512 (сектор карты Evo).
 */
#include "md_snap_68k.h"

#define HID_F5      62
#define HID_F6      63
#define MD_SNAP_CORE 0x06010006u   /* ядро 0.6: снимок целиком */
#define R_SNAP      0x170u        /* 5c {[1] уровень 7, [0] вставка видна}; R [2] — принят */
#define R_SNAP_A    0x174u        /* 5d адрес слова вставки; R {адрес, слово} */
#define R_SNAP_W    0x178u        /* 5e слово во вставку, адрес +1 */
#define R_SNAP_CMD  0x17Cu        /* 5f команда обработчику */
#define R_Z80_CTL   0x180u        /* 60 {[3] T80 в сбросе, [2] NMI, [1] отпустить, [0] взвести}; R [4] стоит */
#define R_Z80_REG   0x184u        /* 61–67 W DIR, R REG */
#define R_SNAP_SYS  0x1A0u        /* 68/69 банки, 6a/6b таймеры YM */
#define R_SNAP_SET  0x1B0u        /* 6c загрузить банки и таймеры */
#define R_VDP_DBG   0x1B4u        /* 6d отвод VDP */
#define VDP_DATA    0xC00000u
#define VDP_CTRL    0xC00004u

#define SS_HEADER   64u
#define SS_VERSION  1u
#define SS_REGS68   64u           /* 36 слов */
#define SS_VDP      136u          /* 24 регистра, ADDR (4), CODE (1) */
#define SS_CRAM     168u          /* 64 слова */
#define SS_VSRAM    296u          /* 40 слов */
#define SS_IO       376u          /* управление портов 1–3, данные 1–3 */
#define SS_Z80      384u          /* T80 REG, 7 слов, PC = $0066 */
#define SS_SYS      416u          /* банки (8), таймеры YM (8) */
#define SS_ZRAM     432u          /* 8 КБ */
#define SS_RAM      8624u         /* 64 КБ: слова SDRAM */
#define SS_VRAM     74160u        /* 64 КБ */
#define SS_FIXED    139696u
#define SS_CHUNK    128u          /* байтов в запросе плагину (кадр ESP -> Z80 не длиннее 256) */
#define EVO_SS_OPEN   3
#define EVO_SS_PUT    4
#define EVO_SS_COMMIT 5
#define EVO_SS_READ   6

static int game_card;             /* карта образа: 0 — Tang (rom_path), 1 — Evo (знает плагин) */
static char game_path[512];

/* CRC32 файла снимка (~140 КБ): таблицей и с уступкой каждые 4 КБ — рабочий поток выше loopTask, который
 * отвечает плагину (MD.ELF 1.1 на плате, 03.11.2026: CRC32 образа 2 МБ по биту — 480 мс без уступки,
 * плагин «ESP connection lost» сразу после загрузки игры). */
static uint32_t ss_crc32(const uint8_t *p, size_t n)
{
    static uint32_t tab[256];
    if (!tab[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int b = 0; b < 8; b++) c = (c >> 1) ^ (c & 1 ? 0xEDB88320u : 0);
            tab[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c = tab[(c ^ p[i]) & 255u] ^ (c >> 8);
        if ((i & 4095u) == 4095u) core->delay(1);
    }
    return ~c;
}

/* Образ загружен в ядро: к какой игре снимок и где лежит. Метка игры — CRC32 слов ROM, который load_cartridge
 * уже посчитал при проверке записи в SDRAM (второй проход по 2–5 МБ не нужен). */
static void snap_game(const uint8_t *rom, size_t size)
{
    (void)rom; (void)size;
    game_crc = md_rom_crc;
    game_card = rom_from_sd ? 0 : 1;
    if (!game_card) memcpy(game_path, rom_path, strlen(rom_path) + 1);
}

static size_t ss_size(uint32_t snd_len, uint32_t sram_words)
{
    return (SS_FIXED + sram_words * 4u + snd_len + 4u + 511u) & ~(size_t)511u;
}
static size_t ss_size_max(void) { return ss_size(MD_SND_STATE_MAX, md_sram_words); }

/* --- вставка и обработчик ---
 * Рабочий поток выше loopTask (ответы плагину) на том же ядре: сотни обращений к LINK подряд без уступки
 * плагин не дождётся (MD.ELF 0.2, «ESP connection lost»). Тик — раз в 64 обращения, как в мосте. */
static void ss_tick(void)
{
    static unsigned n;
    if ((++n & 63) == 0) core->delay(1);
}
static int ss_ovl_wr(unsigned a, const uint16_t *w, unsigned n)
{
    if (link_wr(R_SNAP_A, a) != ZM_OK) return 0;
    for (unsigned i = 0; i < n; i++) { ss_tick(); if (link_wr(R_SNAP_W, w[i]) != ZM_OK) return 0; }
    return 1;
}
static int ss_ovl_rd(unsigned a, uint16_t *v)
{
    uint32_t r = 0;
    ss_tick();
    if (link_wr(R_SNAP_A, a) != ZM_OK || link_rd(R_SNAP_A, &r) != ZM_OK) return 0;
    *v = (uint16_t)r;
    return 1;
}
static int ss_wait_state(uint16_t want, uint32_t ms)
{
    const uint32_t t = core->millis();
    uint16_t s = 0;
    do {
        if (!ss_ovl_rd(MD_SNAP_STATE, &s)) return 0;
        if (s == want) return 1;
        core->delay(1);
    } while (core->millis() - t < ms);
    return 0;
}
static uint8_t ss_seq;
static int ss_command(unsigned code)
{
    ss_seq = (uint8_t)(ss_seq % 255u + 1u);
    const uint16_t c = (uint16_t)(ss_seq << 8 | code);
    if (link_wr(R_SNAP_CMD, c) != ZM_OK) return 0;
    return ss_wait_state(code == MD_SNAP_CMD_SCRIPT ? c : 0, 1000);
}

/* сценарий: записи и чтения шины 68000 руками обработчика */
typedef struct { uint16_t op; uint32_t addr; uint16_t data; } ss_op_t;
static ss_op_t ss_ops[MD_SNAP_SCRIPT_MAX];
static unsigned ss_n;
static void ss_add(uint16_t op, uint32_t addr, uint16_t data)
{
    if (ss_n < MD_SNAP_SCRIPT_MAX - 1u) ss_ops[ss_n++] = (ss_op_t){op, addr, data};
}
static int ss_run(void)
{
    if (link_wr(R_SNAP_A, MD_SNAP_SCRIPT) != ZM_OK) return 0;
    for (unsigned i = 0; i <= ss_n; i++) {
        const ss_op_t o = i < ss_n ? ss_ops[i] : (ss_op_t){MD_SNAP_OP_END, 0, 0};
        const uint16_t w[4] = {o.op, (uint16_t)(o.addr >> 16), (uint16_t)o.addr, o.data};
        for (unsigned k = 0; k < (i < ss_n ? 4u : 1u); k++) { ss_tick(); if (link_wr(R_SNAP_W, w[k]) != ZM_OK) return 0; }
    }
    if (!ss_command(MD_SNAP_CMD_SCRIPT)) return 0;
    for (unsigned i = 0; i < ss_n; i++)
        if ((ss_ops[i].op == MD_SNAP_OP_RW || ss_ops[i].op == MD_SNAP_OP_RB) &&
            !ss_ovl_rd(MD_SNAP_SCRIPT + 4u * i + 3u, &ss_ops[i].data)) return 0;
    return 1;
}

/* 68000 — в обработчик (уровень 7). На паузе меню 68000 стоит: пауза снимается, вставка уже ждёт. */
static int ss_freeze(void)
{
    static const uint16_t zero = 0;
    link_wr(R_Z80_CTL, 0);
    link_wr(R_SNAP, 0);
    if (!ss_ovl_wr(0, md_snap_68k, sizeof md_snap_68k / 2) || !ss_ovl_wr(MD_SNAP_STATE, &zero, 1) ||
        link_wr(R_SNAP_CMD, 0) != ZM_OK) return 0;
    ss_seq = 0;
    if (link_wr(R_SNAP, 3) != ZM_OK) return 0;
    if (menu_open) link_wr(R_PAUSE, 0);
    if (!ss_wait_state(1, 300)) { link_wr(R_SNAP, 0); if (menu_open) link_wr(R_PAUSE, 1); return 0; }
    return link_wr(R_SNAP, 1) == ZM_OK;           /* уровень 7 снят: VDP снова видит подтверждения прерываний */
}

/* --- память мостом --- */
static int ss_mem(uint32_t addr, uint8_t *p, size_t words, int write)
{
    uint32_t w[64];
    for (size_t k = 0; k < words; k += 64) {
        const unsigned n = words - k > 64 ? 64 : (unsigned)(words - k);
        if (write) for (unsigned i = 0; i < n; i++) w[i] = rom_u32(p + 4 * (k + i));
        if (!br_packet((uint32_t)(addr + k), w, n, write)) return 0;
        if (!write) for (unsigned i = 0; i < n; i++) wr32(p + 4 * (k + i), w[i]);
    }
    return 1;
}
static int ss_zram(uint8_t *p, int write)
{
    uint32_t w[64];
    int ok = 1;
    br_flags = 0x4000u;                           /* пакет из ОЗУ Z80: слово — байт */
    for (unsigned k = 0; k < 8192 && ok; k += 64) {
        if (write) for (unsigned i = 0; i < 64; i++) w[i] = p[k + i];
        ok = br_packet(k, w, 64, write);
        if (ok && !write) for (unsigned i = 0; i < 64; i++) p[k + i] = (uint8_t)w[i];
    }
    br_flags = 0;
    return ok;
}

/* --- VDP: ADDR, CODE, PENDING командой; запись регистра даёт CODE[1:0] = 10, поэтому так --- */
static void ss_vdp_port(const uint8_t *b, unsigned flags)
{
    const uint32_t addr = rom_u32(b + SS_VDP + 24);
    const uint8_t code = b[SS_VDP + 28];
    const uint16_t w1 = (uint16_t)((code & 3u) << 14 | (addr & 0x3FFFu));
    const uint16_t w2 = (uint16_t)(((code >> 2) & 7u) << 4 | ((addr >> 14) & 7u));
    if ((code & 3u) == 2u) {
        ss_add(MD_SNAP_OP_WW, VDP_CTRL, (uint16_t)(w1 & 0x3FFFu)); ss_add(MD_SNAP_OP_WW, VDP_CTRL, w2);
        ss_add(MD_SNAP_OP_WW, VDP_CTRL, (uint16_t)(0x8F00u | b[SS_VDP + 15]));
    } else {
        ss_add(MD_SNAP_OP_WW, VDP_CTRL, w1); ss_add(MD_SNAP_OP_WW, VDP_CTRL, w2);
        if (flags & 4u) ss_add(MD_SNAP_OP_WW, VDP_CTRL, w1);
    }
}

/* звук: просьба потоку звука (md_audio.h) и ожидание; 0 — потока нет или не ответил */
static int ss_sound(int req, uint8_t *p, size_t *len)
{
    snd_req_buf = p; snd_req_len = *len; snd_req = req;
    const uint32_t t = core->millis();
    while (snd_req && core->millis() - t < 500) core->delay(2);
    if (snd_req) { snd_req = 0; *len = 0; return 0; }
    *len = snd_req_len;
    return 1;
}

/* Вернуть игру: регистр 1, шаг адреса, порт VDP, шина Z80, отпустить Z80, возврат. load — регистры 68000 из буфера.
 * После сбоя снимка — только то, что успели прочитать: VDP не трогается, пока его не прочли (ss_vdp_read). */
static int ss_vdp_read;
static int ss_resume(const uint8_t *b, int load)
{
    const unsigned flags = rom_u32(b + 16);
    ss_n = 0;
    if (load || ss_vdp_read) {
        ss_add(MD_SNAP_OP_WW, VDP_CTRL, (uint16_t)(0x8100u | b[SS_VDP + 1]));
        ss_add(MD_SNAP_OP_WW, VDP_CTRL, (uint16_t)(0x8F00u | b[SS_VDP + 15]));
        ss_vdp_port(b, flags);
    }
    if ((flags & 3u) == 3u) ss_add(MD_SNAP_OP_WW, 0xA11100u, 0x0100u);   /* шина Z80 снова у игры */
    if (!ss_run()) return 0;
    link_wr(R_Z80_CTL, (flags & 1u) ? 3u : 0u);                         /* RETN на ближайшей выборке $0066 */
    if (!ss_command(load ? MD_SNAP_CMD_LOAD : MD_SNAP_CMD_RET)) return 0;
    link_wr(R_SNAP, 0);
    if (menu_open) link_wr(R_PAUSE, 1);
    return 1;
}

/* Машина в обработчике. Снимок — в b (ss_size_max() байт); длина файла — *size. */
static int ss_capture(uint8_t *b, size_t *size, const char **why)
{
    memset(b, 0, SS_FIXED);
    ss_vdp_read = 0;
    uint16_t w16 = 0;
    for (unsigned i = 0; i < 36; i++) {
        if (!ss_ovl_rd(MD_SNAP_REGS + i, &w16)) { *why = "LINK ERROR (68000 REGISTERS)"; return 0; }
        b[SS_REGS68 + 2 * i] = (uint8_t)w16; b[SS_REGS68 + 2 * i + 1] = (uint8_t)(w16 >> 8);
    }
    uint32_t zd = 0, v = 0;
    if (link_rd(R_Z80_A, &zd) != ZM_OK) { *why = "LINK ERROR (Z80 LINES)"; return 0; }
    unsigned flags = ((zd >> 31) & 1u) | (((zd >> 30) & 1u) ? 0u : 2u);   /* линии — до того, как сценарий их тронет */
    wr32(b + 16, flags);                          /* возврат после сбоя знает, отпускать ли Z80 */
    for (unsigned r = 0; r < 0x23; r++) {
        if (r == 24) r = 0x20;
        ss_tick();
        if (link_wr(R_VDP_DBG, r) != ZM_OK || link_rd(R_VDP_DBG, &v) != ZM_OK) { *why = "LINK ERROR (VDP)"; return 0; }
        if (r < 24) b[SS_VDP + r] = (uint8_t)v;
        else if (r == 0x20) b[SS_VDP + 24] = (uint8_t)v;
        else if (r == 0x21) b[SS_VDP + 25] = (uint8_t)v;
        else { b[SS_VDP + 26] = v & 1u; b[SS_VDP + 28] = (v >> 1) & 0x3Fu; if (v & 0x80u) flags |= 4u; }
    }
    wr32(b + 16, flags);
    ss_vdp_read = 1;
    ss_n = 0;
    ss_add(MD_SNAP_OP_WAIT0, VDP_CTRL, 0x0002);   /* DMA */
    ss_add(MD_SNAP_OP_WAIT1, VDP_CTRL, 0x0200);   /* очередь записи пуста */
    ss_add(MD_SNAP_OP_WW, VDP_CTRL, 0x8F02);      /* шаг адреса 2 */
    ss_add(MD_SNAP_OP_WW, VDP_CTRL, 0x0000); ss_add(MD_SNAP_OP_WW, VDP_CTRL, 0x0020);   /* чтение CRAM с 0 */
    for (int i = 0; i < 64; i++) ss_add(MD_SNAP_OP_RW, VDP_DATA, 0);
    ss_add(MD_SNAP_OP_WW, VDP_CTRL, 0x0000); ss_add(MD_SNAP_OP_WW, VDP_CTRL, 0x0010);   /* чтение VSRAM */
    for (int i = 0; i < 40; i++) ss_add(MD_SNAP_OP_RW, VDP_DATA, 0);
    static const uint32_t io[6] = {0xA10009u, 0xA1000Bu, 0xA1000Du, 0xA10003u, 0xA10005u, 0xA10007u};
    for (int i = 0; i < 6; i++) ss_add(MD_SNAP_OP_RB, io[i], 0);
    if (!ss_run()) { *why = "68000 SCRIPT FAILED (VDP)"; return 0; }
    for (int i = 0; i < 64; i++) { const uint16_t c = ss_ops[5 + i].data & 0x0EEEu; b[SS_CRAM + 2 * i] = (uint8_t)c; b[SS_CRAM + 2 * i + 1] = (uint8_t)(c >> 8); }
    for (int i = 0; i < 40; i++) { const uint16_t c = ss_ops[71 + i].data & 0x07FFu; b[SS_VSRAM + 2 * i] = (uint8_t)c; b[SS_VSRAM + 2 * i + 1] = (uint8_t)(c >> 8); }
    for (int i = 0; i < 6; i++) b[SS_IO + i] = (uint8_t)ss_ops[111 + i].data;
    /* Z80: своим NMI — во вставку $0066 (шину у игры на это время забираем) */
    if (flags & 1u) {
        if (flags & 2u) { ss_n = 0; ss_add(MD_SNAP_OP_WW, 0xA11100u, 0); if (!ss_run()) { *why = "68000 SCRIPT FAILED (Z80 BUS)"; return 0; } }
        link_wr(R_Z80_CTL, 1);
        link_wr(R_Z80_CTL, 5);
        const uint32_t t = core->millis();
        do { if (link_rd(R_Z80_CTL, &v) != ZM_OK) v = 0; } while (!(v & 0x10u) && core->millis() - t < 50);
        link_wr(R_Z80_CTL, 1);
        if (!(v & 0x10u)) { *why = "Z80 DID NOT STOP (NMI)"; return 0; }
        uint32_t r[7];
        if (core->link_read(R_Z80_REG, r, 7) != ZM_OK) { *why = "LINK ERROR (Z80 REGISTERS)"; return 0; }
        r[2] = (r[2] & 0xFFFF0000u) | 0x0066u;
        for (int i = 0; i < 7; i++) wr32(b + SS_Z80 + 4 * i, r[i]);
    }
    uint32_t sys[4];
    if (core->link_read(R_SNAP_SYS, sys, 4) != ZM_OK) { *why = "LINK ERROR (BANKS)"; return 0; }
    for (int i = 0; i < 4; i++) wr32(b + SS_SYS + 4 * i, sys[i]);
    if (!ss_zram(b + SS_ZRAM, 0) || !ss_mem(SD_RAM, b + SS_RAM, 16384, 0) || !ss_mem(SD_VRAM, b + SS_VRAM, 16384, 0) ||
        !ss_mem(SD_SRAM + md_sram_at, b + SS_FIXED, md_sram_words, 0)) { *why = "SDRAM READ FAILED"; return 0; }
    size_t snd = MD_SND_STATE_MAX;
    ss_sound(SND_REQ_SAVE, b + SS_FIXED + md_sram_words * 4u, &snd);   /* нет потока звука — снимок без звука */
    memcpy(b, "MDV4SST1", 8);
    wr32(b + 8, SS_VERSION);
    wr32(b + 12, game_crc);
    wr32(b + 16, flags);
    wr32(b + 20, (uint32_t)snd);
    wr32(b + 24, md_sram_words);
    const size_t end = SS_FIXED + md_sram_words * 4u + snd;
    wr32(b + end, ss_crc32(b, end));
    *size = ss_size((uint32_t)snd, md_sram_words);
    memset(b + end + 4, 0, *size - end - 4);
    return 1;
}

static int ss_valid(const uint8_t *p, size_t n, const char **why)
{
    if (!p || n < SS_FIXED + 4u || memcmp(p, "MDV4SST1", 8) || rom_u32(p + 8) != SS_VERSION) {
        *why = "NOT A SNAPSHOT OF THIS VERSION"; return 0;
    }
    if (rom_u32(p + 12) != game_crc) { *why = "SNAPSHOT IS OF ANOTHER GAME"; return 0; }
    const uint32_t snd = rom_u32(p + 20), sram = rom_u32(p + 24);
    if (snd > MD_SND_STATE_MAX || sram != md_sram_words || n != ss_size(snd, sram)) {
        *why = "SNAPSHOT DOES NOT FIT THIS GAME"; return 0;
    }
    const size_t end = SS_FIXED + sram * 4u + snd;
    if (rom_u32(p + end) != ss_crc32(p, end)) { *why = "SNAPSHOT IS DAMAGED (CRC)"; return 0; }
    return 1;
}

/* Машина в обработчике; снимок проверен */
static int ss_restore(const uint8_t *b, const char **why)
{
    const unsigned flags = rom_u32(b + 16);
    uint32_t v = 0;
    link_wr(R_Z80_CTL, 8);                        /* T80 в сбросе, пока пишутся его ОЗУ и линии */
    ss_n = 0;
    ss_add(MD_SNAP_OP_WAIT0, VDP_CTRL, 0x0002);
    ss_add(MD_SNAP_OP_WAIT1, VDP_CTRL, 0x0200);
    if (link_wr(R_VDP_DBG, 1) == ZM_OK && link_rd(R_VDP_DBG, &v) == ZM_OK)
        ss_add(MD_SNAP_OP_WW, VDP_CTRL, (uint16_t)(0x8100u | (v & 0xBFu)));   /* экран погашен, пока грузится */
    if (!ss_run()) { *why = "68000 SCRIPT FAILED"; return 0; }
    if (!ss_zram((uint8_t *)(b + SS_ZRAM), 1) || !ss_mem(SD_RAM, (uint8_t *)(b + SS_RAM), 16384, 1) ||
        !ss_mem(SD_VRAM, (uint8_t *)(b + SS_VRAM), 16384, 1) ||
        !ss_mem(SD_SRAM + md_sram_at, (uint8_t *)(b + SS_FIXED), md_sram_words, 1)) {
        *why = "SDRAM WRITE FAILED - RESET THE GAME (F4)"; return 0;
    }
    ss_n = 0;
    ss_add(MD_SNAP_OP_WW, VDP_CTRL, (uint16_t)(0x8100u | (b[SS_VDP + 1] & 0xBFu)));   /* режим 5 — до регистров 11–23 */
    for (unsigned r = 0; r < 24; r++)
        if (r != 1) ss_add(MD_SNAP_OP_WW, VDP_CTRL, (uint16_t)(0x8000u | r << 8 | (r == 15 ? 2u : b[SS_VDP + r])));
    ss_add(MD_SNAP_OP_WW, VDP_CTRL, 0xC000); ss_add(MD_SNAP_OP_WW, VDP_CTRL, 0x0000);   /* запись CRAM */
    for (int i = 0; i < 64; i++) ss_add(MD_SNAP_OP_WW, VDP_DATA, (uint16_t)(b[SS_CRAM + 2 * i] | b[SS_CRAM + 2 * i + 1] << 8));
    ss_add(MD_SNAP_OP_WW, VDP_CTRL, 0x4000); ss_add(MD_SNAP_OP_WW, VDP_CTRL, 0x0010);   /* запись VSRAM */
    for (int i = 0; i < 40; i++) ss_add(MD_SNAP_OP_WW, VDP_DATA, (uint16_t)(b[SS_VSRAM + 2 * i] | b[SS_VSRAM + 2 * i + 1] << 8));
    static const uint32_t io[6] = {0xA10009u, 0xA1000Bu, 0xA1000Du, 0xA10003u, 0xA10005u, 0xA10007u};
    for (int i = 0; i < 6; i++) ss_add(MD_SNAP_OP_WB, io[i], b[SS_IO + i]);
    ss_add(MD_SNAP_OP_WW, 0xA11200u, (flags & 1u) ? 0x0100u : 0);
    ss_add(MD_SNAP_OP_WW, 0xA11100u, (flags & 2u) ? 0x0100u : 0);
    if (!ss_run()) { *why = "68000 SCRIPT FAILED (VDP)"; return 0; }
    /* банки и таймеры YM — после линии сброса Z80 (она сбрасывает и YM) */
    uint32_t sys[4];
    for (int i = 0; i < 4; i++) sys[i] = rom_u32(b + SS_SYS + 4 * i);
    if (core->link_write(R_SNAP_SYS, sys, 4) != ZM_OK || link_wr(R_SNAP_SET, 1) != ZM_OK) { *why = "LINK ERROR (BANKS)"; return 0; }
    if (flags & 1u) {
        uint32_t r[7];
        for (int i = 0; i < 7; i++) r[i] = rom_u32(b + SS_Z80 + 4 * i);
        if (core->link_write(R_Z80_REG, r, 7) != ZM_OK) { *why = "LINK ERROR (Z80 REGISTERS)"; return 0; }
        link_wr(R_Z80_CTL, 9);                    /* вставка взведена, T80 ещё в сбросе */
        link_wr(R_Z80_CTL, 1);                    /* сброс снят — DIRSet: регистры из снимка, PC = $0066 */
    } else link_wr(R_Z80_CTL, 0);
    uint16_t regs[36];
    for (unsigned i = 0; i < 36; i++) regs[i] = (uint16_t)(b[SS_REGS68 + 2 * i] | b[SS_REGS68 + 2 * i + 1] << 8);
    if (!ss_ovl_wr(MD_SNAP_REGS, regs, 36)) { *why = "LINK ERROR (68000 REGISTERS)"; return 0; }
    size_t snd = rom_u32(b + 20);
    if (snd) ss_sound(SND_REQ_LOAD, (uint8_t *)(b + SS_FIXED + md_sram_words * 4u), &snd);
    return 1;
}

/* Путь снимка на карте Tang: образ с .sst вместо расширения */
static int ss_tang_path(char *out)
{
    const size_t n = strlen(game_path);
    const char *dot = strrchr(game_path, '.'), *slash = strrchr(game_path, '/');
    if (!dot || (slash && dot < slash) || n + 4 >= 512) return 0;
    const size_t base = (size_t)(dot - game_path);
    memcpy(out, game_path, base);
    memcpy(out + base, ".sst", 5);
    return 1;
}

/* Карта Evo: открыть временный файл, части по SS_CHUNK, заменить .sst. 1 — записан. */
static int ss_evo_write(const uint8_t *buf, size_t size, const char **why)
{
    uint8_t req[1 + 4 + 1 + SS_CHUNK + 2];
    req[0] = EVO_SS_OPEN;
    wr32(req + 1, (uint32_t)size);
    if (!evo_request(req, 5, EVO_WAIT_MS)) { *why = "EVO SD: NO ANSWER (UPDATE WC/VDAC4COR.WMF)"; return 0; }
    if (evo_code) {
        *why = evo_code == 3 ? "EVO SD: UPDATE WILD COMMANDER" : evo_code == 1 ? "EVO SD: GAME FOLDER NOT FOUND" :
               "EVO SD: CANNOT CREATE THE SNAPSHOT";
        return 0;
    }
    for (size_t at = 0; at < size; at += SS_CHUNK) {
        req[0] = EVO_SS_PUT;
        wr32(req + 1, (uint32_t)at);
        req[5] = SS_CHUNK;
        memcpy(req + 6, buf + at, SS_CHUNK);
        const uint16_t crc = rom_crc16(req + 1, 5 + SS_CHUNK);
        req[6 + SS_CHUNK] = (uint8_t)crc;
        req[7 + SS_CHUNK] = (uint8_t)(crc >> 8);
        if (!evo_request(req, sizeof req, EVO_WAIT_MS) || evo_code) { *why = "EVO SD: WRITE FAILED, OLD SNAPSHOT KEPT"; return 0; }
    }
    req[0] = EVO_SS_COMMIT;
    if (!evo_request(req, 1, EVO_WAIT_MS) || evo_code) { *why = "EVO SD: WRITE FAILED, OLD SNAPSHOT KEPT"; return 0; }
    return 1;
}

/* Карта Evo: плагин шлёт .sst прежними 0x77…0x79 (в rom_data) или отказ 0x6F */
static int ss_evo_read(const char **why)
{
    uint8_t req[1] = {EVO_SS_READ};
    rom_discard();
    error_text[0] = 0;
    snap_rx = 1;
    evo_rom_wait = 1;
    const int answered = evo_request(req, 1, EVO_WAIT_MS);
    evo_rom_wait = 0;
    snap_rx = 0;
    if (rom_ready && rom_data) return 1;
    rom_discard();
    *why = !answered ? "EVO SD: NO ANSWER (UPDATE WC/VDAC4COR.WMF)" :
           evo_code == 1 ? "NO SNAPSHOT YET (F5 SAVES)" : "EVO SD: CANNOT READ THE SNAPSHOT";
    return 0;
}

/* Сообщение: при открытом меню — в его строке состояния, иначе окном на ms миллисекунд */
static void ss_message(const char *text, int color, uint32_t ms)
{
    if (menu_open) { menu_set_status(text, color); menu_draw(); return; }
    osd_clear();
    osd_put(13, (OSD_COLS - (unsigned)strlen(text)) / 2, text, color, OSD_COLS);
    osd_flush();
    link_wr(R_OSD_CTRL, 63u << 8 | 1u);
    if (ms) core->delay(ms);
}

static void ss_done(void)
{
    if (menu_open) return;
    link_wr(R_OSD_CTRL, 63u << 8);
    link_wr(R_KEYS_CLEAR, 0);                     /* что держали до F5/F6 — отпустить */
}

static const char *ss_check(char *path)
{
    if (core_version < MD_SNAP_CORE) return "SNAPSHOTS NEED MEGA DRIVE CORE 0.6";
    if (!game_crc) return "NO GAME IN THE CORE";
    if (game_card == 0 && (!core->sd_read || !core->sd_write || !ss_tang_path(path))) return "TANG SD: NO FILE ACCESS FOR SNAPSHOTS";
    return NULL;
}

static void snap_save(void)
{
    char path[512];
    const char *why = ss_check(path);
    if (why) { ss_message(why, C_YELLOW, 1500); ss_done(); return; }
    ss_message("SAVING STATE...", C_YELLOW, 0);
    uint8_t *buf = core->alloc(ss_size_max(), ZM_MEM_PSRAM);
    size_t size = 0;
    int ok = buf != NULL;
    if (!ok) why = "NO PSRAM FOR THE SNAPSHOT";
    else if (!ss_freeze()) { ok = 0; why = "68000 DID NOT ENTER THE SNAPSHOT HANDLER"; }
    else {
        ok = ss_capture(buf, &size, &why);
        if (!ss_resume(buf, 0) && ok) { ok = 0; why = "GAME DID NOT RESUME - RESET IT (F4)"; }
    }
    if (ok) {
        if (game_card == 0) {
            ok = core->sd_write(path, buf, size, &running) == ZM_OK;
            if (!ok) why = "TANG SD: WRITE FAILED, OLD SNAPSHOT KEPT";
        } else {
            ss_message("SAVING STATE TO EVO SD...", C_YELLOW, 0);
            ok = ss_evo_write(buf, size, &why);
        }
    }
    if (buf) core->free(buf);
    core->log("md: snapshot save %s, %u bytes", ok ? "ok" : why, (unsigned)size);
    ss_message(ok ? "STATE SAVED  (F6 LOADS IT)" : why, ok ? C_CYAN : C_YELLOW, ok ? 700 : 2000);
    ss_done();
}

static void snap_load(void)
{
    char path[512];
    const char *why = ss_check(path);
    if (why) { ss_message(why, C_YELLOW, 1500); ss_done(); return; }
    ss_message("LOADING STATE...", C_YELLOW, 0);
    uint8_t *data = NULL;
    size_t length = 0;
    int ok;
    if (game_card == 0) {
        const int r = core->sd_read(path, &data, &length, ss_size_max(), &running);
        ok = r == ZM_OK;
        if (!ok) why = r == ZM_NOT_FOUND ? "NO SNAPSHOT YET (F5 SAVES)" : "TANG SD: CANNOT READ THE SNAPSHOT";
    } else {
        ok = ss_evo_read(&why);
        if (ok) { data = rom_data; length = rom_size; rom_data = NULL; rom_discard(); }
    }
    if (ok) ok = ss_valid(data, length, &why);
    if (ok && !ss_freeze()) { ok = 0; why = "68000 DID NOT ENTER THE SNAPSHOT HANDLER"; }
    else if (ok) {
        ok = ss_restore(data, &why);
        if (!ss_resume(data, ok) && ok) { ok = 0; why = "GAME DID NOT RESUME - RESET IT (F4)"; }
    }
    if (data) core->free(data);
    core->log("md: snapshot load %s", ok ? "ok" : why);
    if (ok && !menu_open) { ss_done(); return; }  /* игра — сразу, без окна */
    ss_message(ok ? "STATE LOADED" : why, ok ? C_CYAN : C_YELLOW, ok ? 0 : 2000);
    ss_done();
}

/* F5/F6 (нажатие): 1 — клавиша съедена */
static int snap_key(unsigned code, unsigned up)
{
    if (code != HID_F5 && code != HID_F6) return 0;
    if (!up) { if (code == HID_F5) snap_save(); else snap_load(); }
    return 1;
}
