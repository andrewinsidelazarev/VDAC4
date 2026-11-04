/* SPDX-License-Identifier: GPL-3.0-or-later
 * ROM Mega Drive — в SDRAM ядра (MD.ELF, 03.11.2026) мостом LINK md_link.sv: пакеты по 64 слова,
 * буфер 0x5000 (слова 1400…143f), адрес слова SDRAM — 0x160 (58), команда — 0x164 (59)
 * {seq[31:16], запись[15], слов[6:0]}, выполненный seq — 0x164, {ошибка, занят} — 0x168 (5a).
 *
 * Раскладка SDRAM (md_vdac4.sv): слово k = два слова 68000 — младшая половина по чётному адресу:
 * {байт 4k+2, 4k+3, 4k, 4k+1}. ROM — с 0, ОЗУ 68000 — 5D0000, SRAM — 5E0000, видеопамять — 600000.
 * Порядок: машина в сбросе → ROM (и чтение обратно: CRC32 записанного и прочитанного) → видеопамять
 * и ОЗУ — нули, SRAM — FF (сохранения — md_saves.h) → размер, регион, особенности → сброс снят.
 */
#define R_CTRL     0x140u         /* 50: управление (md_rom.h: MD_CTRL_*) */
#define R_ROMSZ    0x144u         /* 51: размер ROM, байт */
#define R_PADS     0x148u         /* 52: пады */
#define R_MDSTATE  0x14Cu         /* 53: {развёртка подстроена, работает, кадры} */
#define R_BR_ADDR  0x160u
#define R_BR_CMD   0x164u
#define R_BR_STAT  0x168u
#define W_BR_BUF   0x5000u
#define SD_RAM     (0x5D0000u / 4)
#define SD_SRAM    (0x5E0000u / 4)
#define SD_VRAM    (0x600000u / 4)

static uint16_t br_seq;
static uint32_t br_flags;                      /* бит 14 команды — пакет из ОЗУ Z80 (снимок, ядро 0.6) */
static uint32_t md_sram_at, md_sram_words;     /* SRAM игры в словах от SD_SRAM — для снимка (0 — нет) */
static uint32_t md_rom_crc;                    /* CRC32 слов ROM в SDRAM — метка игры для снимков */
static uint16_t md_ctrl;                       /* что записано в 50 (без RUN — в md_ctrl_run) */
static void (*load_progress)(size_t done, size_t total);

/* один пакет: n слов с адреса слова SDRAM addr; запись — из w, чтение — в w.
 * Уступать процессор (03.11.2026, на плате): рабочий поток — на ядре 1 с приоритетом 3, выше loopTask
 * (приоритет 1), который отвечает плагину. Передача ROM без уступок держала ядро 1 больше секунды —
 * плагин не дождался ответа («ESP connection lost») и выключил ядро. Теперь — тик раз в 8 пакетов
 * (2 КБ) и при каждом ожидании моста, как у NES (rom_loader.h). */
static int br_packet(uint32_t addr, uint32_t *w, unsigned n, int write)
{
    static unsigned br_count;
    if ((++br_count & 7) == 0) core->delay(1);
    if (write)
        for (unsigned i = 0; i < n; i += 16)
            if (core->link_write(W_BR_BUF + 4 * i, w + i, n - i > 16 ? 16 : n - i) != ZM_OK) return 0;
    const uint16_t seq = (uint16_t)(br_seq + 1);
    if (link_wr(R_BR_ADDR, addr) != ZM_OK ||
        link_wr(R_BR_CMD, (uint32_t)seq << 16 | (write ? 0x8000u : 0) | br_flags | n) != ZM_OK) return 0;
    const uint32_t t = core->millis();
    uint32_t done = 0, st = 0;
    do {
        if (link_rd(R_BR_CMD, &done) != ZM_OK || link_rd(R_BR_STAT, &st) != ZM_OK) return 0;
        if (st & 2) return 0;                          /* ошибка протокола */
        if ((done & 0xFFFF) == seq && !(st & 1)) break;
        core->delay(1);
    } while (running && core->millis() - t < 200);
    if ((done & 0xFFFF) != seq) return 0;
    br_seq = seq;
    if (!write)
        for (unsigned i = 0; i < n; i += 16)
            if (core->link_read(W_BR_BUF + 4 * i, w + i, n - i > 16 ? 16 : n - i) != ZM_OK) return 0;
    return 1;
}

static uint32_t md_word(const uint8_t *p, size_t size, size_t k)
{
    uint8_t b[4] = {0, 0, 0, 0};
    for (unsigned i = 0; i < 4; i++) if (4 * k + i < size) b[i] = p[4 * k + i];
    return (uint32_t)b[2] << 24 | (uint32_t)b[3] << 16 | (uint32_t)b[0] << 8 | b[1];
}

/* заполнить words слов значением v */
static int br_fill(uint32_t addr, size_t words, uint32_t v)
{
    uint32_t w[64];
    for (unsigned i = 0; i < 64; i++) w[i] = v;
    for (size_t k = 0; k < words && running; k += 64)
        if (!br_packet(addr + (uint32_t)k, w, words - k > 64 ? 64 : (unsigned)(words - k), 1)) return 0;
    return running;
}

static uint32_t crc32_add(uint32_t c, uint32_t w)
{
    c = ~c;
    for (int i = 0; i < 4; i++) {
        c ^= (w >> (8 * i)) & 255;
        for (int b = 0; b < 8; b++) c = (c >> 1) ^ (c & 1 ? 0xedb88320u : 0);
    }
    return ~c;
}

static int md_set_ctrl(uint16_t ctrl)
{
    uint32_t r = 0;
    return link_wr(R_CTRL, ctrl) == ZM_OK && link_rd(R_CTRL, &r) == ZM_OK && (r & 0xFFFF) == ctrl;
}

/* ROM (обычный образ, SMD — уже переведён) — в ядро и пуск. Имя load_cartridge — как у меню NES. */
static int load_cartridge(uint8_t *p, size_t size)
{
    static uint8_t tmp[16384];
    if (md_rom_is_smd(p, size)) size = md_rom_from_smd(p, size, tmp);
    const char *bad = md_rom_check(p, size);
    if (bad) { fail(bad); return 0; }
    md_rom_info_t info;
    md_rom_info(p, size, &info);
    /* SRAM для снимка: по заголовку (окно 200000–21FFFF), иначе по особенности игры — 64 КБ */
    md_sram_at = md_sram_words = 0;
    if (info.sram_end > info.sram_start && info.sram_start >= 0x200000u && info.sram_start < 0x220000u) {
        const uint32_t a = (info.sram_start - 0x200000u) >> 2, e = ((info.sram_end > 0x21FFFFu ? 0x21FFFFu : info.sram_end) - 0x200000u) >> 2;
        md_sram_at = a; md_sram_words = e - a + 1;
    } else if (info.ctrl & (MD_CTRL_SRAM | MD_CTRL_SRAM00)) md_sram_words = 16384;
    if (!md_set_ctrl(0)) return 0;                     /* машина в сбросе */
    const size_t words = (size + 3) / 4;
    uint32_t w[64], crc_w = 0, crc_r = 0;
    for (size_t k = 0; k < words && running && !exit_req; k += 64) {
        const unsigned n = words - k > 64 ? 64 : (unsigned)(words - k);
        for (unsigned i = 0; i < n; i++) { w[i] = md_word(p, size, k + i); crc_w = crc32_add(crc_w, w[i]); }
        if (!br_packet((uint32_t)k, w, n, 1)) return 0;
        if (load_progress && ((k / 64) & 15) == 0) load_progress(k * 2, words * 4 + 1);
    }
    for (size_t k = 0; k < words && running && !exit_req; k += 64) {   /* чтение обратно */
        const unsigned n = words - k > 64 ? 64 : (unsigned)(words - k);
        if (!br_packet((uint32_t)k, w, n, 0)) return 0;
        for (unsigned i = 0; i < n; i++) crc_r = crc32_add(crc_r, w[i]);
        if (load_progress && ((k / 64) & 15) == 0) load_progress(words * 2 + k * 2, words * 4 + 1);
    }
    if (!running || exit_req) return 0;
    if (crc_r != crc_w) { fail("ROM read back from SDRAM differs (CRC)"); return 0; }
    md_rom_crc = crc_w;
    if (!br_fill(SD_VRAM, 16384, 0) || !br_fill(SD_RAM, 16384, 0)) return 0;
    if (!md_saves_restore(SD_SRAM, &info)) return 0;
    md_ctrl = info.ctrl;
    uint32_t r = 0;
    if (link_wr(R_ROMSZ, (uint32_t)size) != ZM_OK || link_rd(R_ROMSZ, &r) != ZM_OK || r != size) return 0;
    if (!md_set_ctrl(md_ctrl | MD_CTRL_RUN)) return 0;
    if (load_progress) load_progress(1, 1);
    core->log("md: ROM %u bytes, region %c%s, CRC32 %08X verified%s%s", (unsigned)size, info.region,
              (info.ctrl & MD_CTRL_PAL) ? " (PAL)" : "", (unsigned)crc_w,
              info.unsupported ? ", unsupported: " : "", info.unsupported ? info.unsupported : "");
    return 1;
}

/* F4: сброс игры — машина в сбросе на миллисекунду, ROM в SDRAM остаётся */
static void md_reset(void)
{
    md_set_ctrl(md_ctrl);
    core->delay(2);
    md_set_ctrl(md_ctrl | MD_CTRL_RUN);
}
