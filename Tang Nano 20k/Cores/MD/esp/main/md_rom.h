/* SPDX-License-Identifier: GPL-3.0-or-later
 * ROM Mega Drive на ESP (MD.ELF, 03.11.2026): проверка, формат SMD, регион и особенности игр —
 * всё, что Genesis-MiSTer делает при загрузке в ПЛИС (Genesis.sv), здесь — на ESP до старта ядра.
 *
 * Форматы: .bin/.md/.gen — как есть (заголовок «SEGA» в 0x100); .smd — 512 байтов заголовка (байты 8,9 —
 * AA BB), дальше блоки по 16 КБ: первая половина блока — нечётные байты, вторая — чётные.
 * Регион (0x1F0): буквы J/U/E или старая шестнадцатеричная цифра (бит 0 — Япония, 2 — США, 3 — Европа);
 * выбор как у MiSTer по умолчанию: США > Европа > Япония, без указания — США.
 * Особенности — по серийному номеру (байты 0x183–0x18A), таблица Genesis.sv: SRAM, SRAM00, NORAM,
 * FMBUSY, SCHAN, FAST_FIFO поддержаны; EEPROM, Pier Solar и SVP (Virtua Racing) — нет, игра пойдёт без них.
 */
#ifndef MD_ROM_H
#define MD_ROM_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MD_ROM_MAX 0x5D0000u            /* SDRAM до ОЗУ 68000 (md_vdac4.sv): 5,8 МБ */

/* биты регистра LINK 50 (md_link.sv) */
#define MD_CTRL_RUN    0x0001u
#define MD_CTRL_PAL    0x0004u
#define MD_CTRL_EXPORT 0x0008u
#define MD_CTRL_3BTN   0x0010u
#define MD_CTRL_SRAM   0x0100u
#define MD_CTRL_SRAM00 0x0200u
#define MD_CTRL_NORAM  0x0400u
#define MD_CTRL_FMBUSY 0x0800u
#define MD_CTRL_SCHAN  0x1000u
#define MD_CTRL_FIFO   0x2000u

typedef struct {
    uint16_t ctrl;                      /* PAL, экспорт, особенности (без RUN) */
    char region;                        /* 'J', 'U' или 'E' */
    const char *unsupported;            /* особенность, которой ядро не умеет, или NULL */
    uint32_t sram_start, sram_end;      /* батарейная SRAM из заголовка (0 — нет) */
} md_rom_info_t;

/* имя файла ROM: .md, .bin, .gen, .smd (регистр не важен) */
static int md_rom_name(const char *name)
{
    static const char *const ext[] = {".md", ".bin", ".gen", ".smd"};
    const size_t n = strlen(name);
    for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++) {
        const size_t e = strlen(ext[i]);
        if (n <= e) continue;
        size_t k = 0;
        while (k < e && (name[n - e + k] | 0x20) == ext[i][k]) k++;
        if (k == e) return 1;
    }
    return 0;
}

/* Образ, который ядро не запустит: нет «SEGA» в 0x100 (не Mega Drive) или 32X / Pico (у них «SEGA» там тоже) */
#define MD_WRONG_IMAGE "Wrong image!"
static int md_header_wrong(const uint8_t *p)
{
    if (memcmp(p + 0x100, "SEGA", 4) && memcmp(p + 0x101, "SEGA", 4)) return 1;
    return !memcmp(p + 0x100, "SEGA 32X", 8) || !memcmp(p + 0x100, "SEGA PICO", 9);
}

/* Проверка файла до перевода SMD: заголовок SEGA или подпись SMD. Смотрит только первые 512 байт — её зовут
 * и по первому сектору файла с карты Tang, и по первому пакету с карты Evo, до чтения всего файла (MD.ELF 1.2:
 * «Wrong image!» сразу, а не после чтения мегабайтов). n — длина файла. */
static const char *md_file_check(const uint8_t *p, size_t n)
{
    if (p && n > 512 && (n - 512) % 16384 == 0 && p[8] == 0xAA && p[9] == 0xBB) return NULL;
    if (!p || n < 0x200) return MD_WRONG_IMAGE;
    if (n > 0x5D0000u) return "ROM is larger than 5.8 MB";
    if (md_header_wrong(p)) return MD_WRONG_IMAGE;
    return NULL;
}

static int md_rom_is_smd(const uint8_t *p, size_t n)
{
    return n > 512 && (n - 512) % 16384 == 0 && p[8] == 0xAA && p[9] == 0xBB;
}

/* SMD -> обычный образ на месте: возвращает новый размер */
static size_t md_rom_from_smd(uint8_t *p, size_t n, uint8_t *tmp16k)
{
    const size_t blocks = (n - 512) / 16384;
    for (size_t b = 0; b < blocks; b++) {
        const uint8_t *src = p + 512 + b * 16384;
        for (size_t i = 0; i < 8192; i++) {
            tmp16k[2 * i + 1] = src[i];
            tmp16k[2 * i] = src[8192 + i];
        }
        memcpy(p + b * 16384, tmp16k, 16384);
    }
    return blocks * 16384;
}

static const char *md_rom_check(const uint8_t *p, size_t n)
{
    if (!p || n < 0x200) return MD_WRONG_IMAGE;
    if (n > MD_ROM_MAX) return "ROM is larger than 5.8 MB";
    if (md_header_wrong(p)) return MD_WRONG_IMAGE;
    return NULL;
}

static int md_id_is(const uint8_t *p, const char *id)
{
    return memcmp(p + 0x183, id, 8) == 0;
}

static void md_rom_info(const uint8_t *p, size_t n, md_rom_info_t *r)
{
    (void)n;
    int j = 0, u = 0, e = 0;
    for (int k = 0x1F0; k < 0x1F3; k++) {
        const uint8_t c = p[k];
        if (c == 'J') j = 1;
        else if (c == 'U') u = 1;
        else if (c == 'E') e = 1;
        else if (k == 0x1F0 && c >= '0' && c <= '9') { j = c & 1; u = (c >> 2) & 1; e = (c >> 3) & 1; }
        else if (k == 0x1F0 && c >= 'A' && c <= 'F') { const int h = c - 'A' + 10; j = h & 1; u = (h >> 2) & 1; e = (h >> 3) & 1; }
    }
    r->region = u ? 'U' : e ? 'E' : j ? 'J' : 'U';
    r->ctrl = r->region == 'E' ? (MD_CTRL_PAL | MD_CTRL_EXPORT) : r->region == 'U' ? MD_CTRL_EXPORT : 0;
    r->unsupported = NULL;
    static const struct { const char *id; uint16_t ctrl; const char *what; } q[] = {
        {"T-081276", MD_CTRL_SRAM, 0},   /* NFL Quarterback Club */
        {"T-81406 ", MD_CTRL_SRAM, 0},   /* NBA Jam TE */
        {"T-081586", MD_CTRL_SRAM, 0},   /* NFL Quarterback Club '96 */
        {"T-81576 ", MD_CTRL_SRAM, 0},   /* College Slam */
        {"T-81476 ", MD_CTRL_SRAM, 0},   /* Frank Thomas Big Hurt Baseball */
        {"MK-1215 ", 0, "EEPROM"},       /* Evander Holyfield's Real Deal Boxing */
        {"G-4060  ", 0, "EEPROM"},       /* Wonder Boy */
        {"00001211", 0, "EEPROM"},       /* Sports Talk Baseball */
        {"MK-1228 ", 0, "EEPROM"},       /* Greatest Heavyweights */
        {"G-5538  ", 0, "EEPROM"},       /* Greatest Heavyweights JP */
        {"00004076", 0, "EEPROM"},       /* Honoo no Toukyuuji Dodge Danpei */
        {"T-12046 ", 0, "EEPROM"},       /* Mega Man - The Wily Wars */
        {"T-12053 ", 0, "EEPROM"},       /* Rockman Mega World */
        {"G-4524  ", 0, "EEPROM"},       /* Ninja Burai Densetsu */
        {"T-113016", MD_CTRL_NORAM, 0},  /* Puggsy */
        {"T-89016 ", MD_CTRL_FIFO, 0},   /* Clue */
        {"T-574023", 0, "Pier Solar mapper"},
        {"T-574013", 0, "Pier Solar mapper"},
        {"MK-1229 ", 0, "SVP chip"},     /* Virtua Racing EU/US */
        {"G-7001  ", 0, "SVP chip"},     /* Virtua Racing JP */
        {"T-35036 ", MD_CTRL_FMBUSY, 0}, /* Hellfire US */
        {"T-25073 ", MD_CTRL_FMBUSY, 0}, /* Hellfire JP */
        {"MK-1137-", MD_CTRL_FMBUSY, 0}, /* Hellfire EU */
        {"T-68?" "?" "?-", MD_CTRL_SCHAN, 0}, /* Game no Kanzume Otokuyou («??-» слитно — триграф) */
        {" GM 0000", MD_CTRL_SRAM00, 0}, /* Sonic 1 Remastered */
    };
    for (size_t i = 0; i < sizeof q / sizeof q[0]; i++)
        if (md_id_is(p, q[i].id)) { r->ctrl |= q[i].ctrl; r->unsupported = q[i].what; break; }
    /* SRAM в заголовке: «RA», тип, затем начало и конец (большой порядок байтов) */
    r->sram_start = r->sram_end = 0;
    if (p[0x1B0] == 'R' && p[0x1B1] == 'A') {
        r->sram_start = (uint32_t)p[0x1B4] << 24 | (uint32_t)p[0x1B5] << 16 | (uint32_t)p[0x1B6] << 8 | p[0x1B7];
        r->sram_end = (uint32_t)p[0x1B8] << 24 | (uint32_t)p[0x1B9] << 16 | (uint32_t)p[0x1BA] << 8 | p[0x1BB];
    }
}
#endif
