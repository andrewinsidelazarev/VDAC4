/* SPDX-License-Identifier: Apache-2.0 */
/* Меню F1 NES.ELF 0.9 (esp/main/game_menu.h) на модели ядра NES 0.10: окно nes_osd (текст 0x3000, 0xC0 — показ
 * и подсветка, d0/d4/d8 — подложки, разделители, полоса хода), пауза 0xC4, загрузка картриджа 0x44…0x54;
 * карта Tang и плагин WC (карта Evo) — модели каталогов, записи нарочно вразнобой. Проверяются:
 *  - разметка: шапка с вкладками на подложке, путь и страница, разделители 2/24/26, строка состояния на
 *    подложке, подсказки 27…29 (клавиши голубым); значки 0.10: папка, треугольник выбора, многоточие;
 *  - сортировка: «..», папки, затем игры — по имени без учёта регистра; у карты Evo READ просит исходный
 *    номер записи выбранной игры;
 *  - курсор: ↑/↓ через страницы, PgUp/PgDn, Home/End; после выхода из папки — на ней;
 *  - размеры игр карты Tang справа; длинное имя — с многоточием;
 *  - загрузка: «LOADING имя», полоса хода растёт (снимки регистра d8), окно — до конца передачи;
 *  - старое ядро 0.9: то же знаками ASCII, подложек и полосы нет;
 *  - вся память — освобождена.
 * С «--dump КАТАЛОГ» пишет туда снимки окна (osd_*.hex: 512 слов текста и регистры) для стенда
 * отрисовки tb_nes_osd_render.sv. Ожидается: «PASS». */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#define NES_APP_HOST_TEST
#include "app.c"
#include "cartridge.h"

static unsigned checks, allocated, freed;
static uint32_t now;
static uint8_t answer[1024];
static uint16_t answer_n;
static const char *dump_dir;
#define CHECK(x) do { ++checks; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static void *mem_alloc(size_t n, uint32_t caps) { (void)caps; ++allocated; return malloc(n); }
static void mem_free(void *p) { if (p) { ++freed; free(p); } }
static void no_lock(void *p) { (void)p; }
static int reply(uint8_t cmd, const void *p, uint16_t n) { (void)cmd; memcpy(answer, p, n); answer_n = n; return ZM_OK; }
static void log_none(const char *format, ...) { (void)format; }
static uint32_t millis(void) { return now; }
static void plugin_step(void);
static int plugin_on;
static void delay(uint32_t n) { now += n; if (plugin_on) plugin_step(); }

/* ---- модель ядра NES 0.10 ---- */
static uint32_t hw_version = 0x0401000A, osd[512], osd_ctrl = 63u << 8, pause_reg, keys_clear;
static uint32_t band_reg, rule_reg, bar_reg, bar_writes, bar_max_fill, size_reg;
static unsigned band_writes;
static int loading;
static uint32_t seq, received, crc = 0xffffffffu, packet_words[64];
static uint8_t rom_seen[65536];
static uint32_t bar_trace[64];
static unsigned bar_trace_n;
static uint32_t crc_byte(uint32_t c, uint8_t b) { c ^= b; for (int i = 0; i < 8; i++) c = (c >> 1) ^ (c & 1 ? 0xedb88320u : 0); return c; }
static int wr(uint32_t a, const uint32_t *w, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++, a += 4) {
        const uint32_t v = w[i];
        if (a >= W_OSD && a < W_OSD + 2048) osd[(a - W_OSD) / 4] = v;
        else if (a == R_OSD_CTRL) osd_ctrl = v;
        else if (a == R_PAUSE) pause_reg = v;
        else if (a == R_KEYS_CLEAR) keys_clear++;
        else if (a == R_OSD_BAND && hw_version >= 0x0401000A) { band_reg = v; band_writes++; }
        else if (a == R_OSD_RULE && hw_version >= 0x0401000A) rule_reg = v;
        else if (a == R_OSD_BAR && hw_version >= 0x0401000A) {
            bar_reg = v; bar_writes++;
            if ((v & 0x3FF) > bar_max_fill) bar_max_fill = v & 0x3FF;
            if (bar_trace_n < 64) bar_trace[bar_trace_n++] = v;
        }
        else if (a == 0x84 && (v >> 16) == 0x535A) size_reg = v & 3;
        else if (a >= 0x4000 && a < 0x4100) packet_words[(a - 0x4000) / 4] = v;
        else if (a == 0x44) {
            if (v == 1) { loading = 1; seq = received = 0; crc = 0xffffffffu; }
            else if (v == 2) loading = 0;
        } else if (a == 0x48) {
            CHECK(loading && (v >> 16) == seq + 1);
            CHECK(osd_ctrl & 1);                  /* окно видно, пока идёт передача */
            const uint32_t count = v & 0xffff;
            for (uint32_t k = 0; k < count; k++) {
                const uint8_t b = (uint8_t)(packet_words[k / 4] >> (8 * (k & 3)));
                if (received < sizeof rom_seen) rom_seen[received] = b;
                crc = crc_byte(crc, b);
                received++;
            }
            seq = v >> 16;
            now += 7;                             /* передача пакета — время идёт (ход раз в 100 мс) */
        }
    }
    return ZM_OK;
}
static int rd(uint32_t a, uint32_t *w, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++, a += 4) {
        uint32_t v = 0;
        if (a == 4) v = hw_version;
        else if (a == 0x44) v = (uint32_t)loading | (received == sizeof cartridge ? 4u : 0u);
        else if (a == 0x48) v = seq;
        else if (a == 0x4c) v = received;
        else if (a == 0x50) v = ~crc;
        else if (a == 0x84) v = 0x535A0000u | size_reg;
        else if (a == R_PAUSE) v = pause_reg;
        w[i] = v;
    }
    return ZM_OK;
}

/* ---- карта Tang: / — zelda.nes, Games/, contra.nes, Arcade/, b.nes; /Games — 30 игр, вразнобой;
   /Arcade — длинное имя ---- */
static char last_read[512];
static const char *tang_root[] = {"zelda.nes", "Games", "contra.nes", "Arcade", "b.nes"};
static const int tang_root_dir[] = {0, 1, 0, 1, 0};
static int sd_list(const char *path, const char *suffix, uint32_t skip, zm_sd_entry_t *out,
                   uint32_t cap, uint32_t *count, int *more, const volatile int *active)
{
    CHECK(!strcmp(suffix, ".nes") && cap >= 1 && cap <= 16 && *active);
    unsigned total;
    if (!strcmp(path, "/")) total = 5;
    else if (!strcmp(path, "/Games")) total = 30;
    else if (!strcmp(path, "/Arcade")) total = 1;
    else return ZM_NOT_FOUND;
    *count = 0;
    for (uint32_t i = skip; i < total && *count < cap; i++) {
        zm_sd_entry_t *e = &out[(*count)++];
        memset(e, 0, sizeof *e);
        if (total == 5) { strcpy(e->name, tang_root[i]); e->flags = (uint8_t)tang_root_dir[i]; e->size = 24592 + 1024 * i; }
        else if (total == 30) { snprintf(e->name, sizeof e->name, "Game %02u (Europe).nes", 30 - (unsigned)i); e->size = 131088; }
        else { strcpy(e->name, "A very long cartridge file name that will not fit into the menu row.nes"); e->size = 1048592; }
    }
    *more = skip + *count < total;
    return ZM_OK;
}
static int sd_read(const char *path, uint8_t **out, size_t *size, size_t max, const volatile int *active)
{
    CHECK(*active);
    *out = NULL; *size = 0;
    if (strstr(path, ".sav") || strstr(path, ".svb") || strstr(path, ".bak") || strstr(path, ".cfg")) return ZM_NOT_FOUND;
    CHECK(max == NES_ROM_MAX);
    strcpy(last_read, path);
    *size = sizeof cartridge; *out = mem_alloc(*size, ZM_MEM_PSRAM); memcpy(*out, cartridge, *size);
    return ZM_OK;
}
static int sd_write(const char *path, const uint8_t *data, size_t n, const volatile int *active)
{
    (void)path; (void)data; (void)n; (void)active;
    return ZM_OK;
}

/* ---- плагин WC: карта Evo, / — Mario.nes, RPG/, aladdin.nes (в каталоге — номера 0, 1, 2) ---- */
static unsigned plugin_reads, plugin_lists, read_index = 999;
static size_t plugin_at = (size_t)-1;            /* отдаёт файл: следующий пакет, по одному за шаг */
static const char *evo_root[] = {"Mario.nes", "RPG", "aladdin.nes"};
static void plugin_step(void)
{
    if (plugin_at != (size_t)-1) {
        if (plugin_at < sizeof cartridge) {
            const size_t at = plugin_at, k = sizeof cartridge - at > 512 ? 512 : sizeof cartridge - at;
            uint8_t pk[518];
            wr32(pk, (uint32_t)at);
            memcpy(pk + 4, cartridge + at, k);
            const uint16_t c = rom_crc16(pk, k + 4);
            pk[k + 4] = (uint8_t)c;
            pk[k + 5] = (uint8_t)(c >> 8);
            command(0x78, pk, (uint16_t)(k + 6));
            plugin_at += k;
        } else {
            command(0x79, NULL, 0);
            plugin_at = (size_t)-1;
        }
        return;
    }
    if (!evo_req_pending) return;
    const uint8_t input[4] = {0, 0xff, 0, 0};
    command(0x71, input, 4);
    CHECK(answer[0] == 1 && answer_n >= 4);
    uint8_t req[600];
    const uint16_t n = answer_n;
    memcpy(req, answer, n);
    if (req[1] == 1) {                            /* LIST [пропуск][до][путь] */
        plugin_lists++;
        const unsigned skip = req[2] | req[3] << 8, max = req[4];
        CHECK(max >= 1 && max <= 8 && !strcmp((const char *)req + 5, "/"));
        uint8_t out[600];
        unsigned at = 3, count = 0, i = skip;
        out[0] = 0;
        for (; i < 3 && count < max; i++, count++) {
            out[at++] = i == 1;
            strcpy((char *)out + at, evo_root[i]);
            at += (unsigned)strlen(evo_root[i]) + 1;
        }
        out[1] = i < 3;
        out[2] = (uint8_t)count;
        command(0x6E, out, (uint16_t)at);
    } else {                                      /* READ [номер][путь] */
        CHECK(req[1] == 2);
        plugin_reads++;
        read_index = req[2] | req[3] << 8;
        uint8_t size[4];
        wr32(size, sizeof cartridge);
        command(0x77, size, 4);
        plugin_at = 0;
    }
}

/* строка окна текстом: значки 0.10 — как '#' (папка), '>' (треугольник), '~' (многоточие) */
static const char *row_text(unsigned r)
{
    static char s[OSD_COLS + 1];
    for (unsigned c = 0; c < OSD_COLS; c++) {
        const uint8_t b = (uint8_t)(osd[r * 16 + c / 4] >> (8 * (c & 3)));
        char ch = (char)((b & 63) + 0x20);
        if (hw_version >= 0x0401000A) ch = ch == G_FOLDER ? '#' : ch == G_MARK ? '>' : ch == G_ELLIPSIS ? '~' : ch;
        s[c] = ch;
    }
    s[OSD_COLS] = 0;
    for (int c = OSD_COLS - 1; c >= 0 && s[c] == ' '; c--) s[c] = 0;
    return s;
}
static unsigned color_at(unsigned r, unsigned c) { return (osd[r * 16 + c / 4] >> (8 * (c & 3) + 6)) & 3; }
static unsigned sel_row(void) { return (osd_ctrl >> 8) & 63; }
static int shown(void) { return osd_ctrl & 1; }
static int press(unsigned code) { const int a = menu_key((uint16_t)code); const int b = menu_key((uint16_t)(code | 0x80)); return a && b; }

static void dump(const char *name)
{
    if (!dump_dir) return;
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dump_dir, name);
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    for (unsigned i = 0; i < 512; i++) fprintf(f, "%08X\n", osd[i]);
    fprintf(f, "%08X\n%08X\n%08X\n%08X\n", osd_ctrl, band_reg, rule_reg, bar_reg);
    fclose(f);
}

int main(int argc, char **argv)
{
    dump_dir = argc > 2 && !strcmp(argv[1], "--dump") ? argv[2] : NULL;
    zm_core_t api = {0};
    api.size = sizeof api; api.abi_major = 1; api.abi_minor = 9;
    api.alloc = mem_alloc; api.free = mem_free; api.reply = reply; api.log = log_none; api.delay = delay;
    api.millis = millis; api.mutex_lock = api.mutex_unlock = api.signal_give = no_lock;
    api.link_read = rd; api.link_write = wr; api.sd_list = sd_list; api.sd_read = sd_read; api.sd_write = sd_write;
    core = &api; running = 1; state = 2; core_version = hw_version;
    slot0_size_known = 1; slot0_size = 1;           /* SIZE слота 0 — 95 % */

    /* F1: окно, пауза, оформление */
    CHECK(menu_key(HID_F1) && menu_open && pause_reg == 1 && shown());
    CHECK(band_reg == (1u << 0 | 1u << 25) && rule_reg == (1u << 2 | 1u << 24 | 1u << 26) && bar_reg == 0);
    CHECK(!strcmp(row_text(0), " NES SELECT A GAME                  >TANG SD   EVO SD"));
    CHECK(color_at(0, 1) == C_YELLOW && color_at(0, 37) == C_YELLOW && color_at(0, 47) == C_GRAY);
    CHECK(strstr(row_text(1), " /") == row_text(1) && strstr(row_text(1), "PAGE 1/1"));
    /* «..» нет в корне; папки (по имени), затем игры (без учёта регистра) */
    CHECK(!strcmp(row_text(3), " > # ARCADE"));
    CHECK(!strcmp(row_text(4), "   # GAMES"));
    CHECK(strstr(row_text(5), "     B.NES") == row_text(5) && strstr(row_text(5), "29K"));
    CHECK(strstr(row_text(6), "     CONTRA.NES") && strstr(row_text(6), "27K"));
    CHECK(strstr(row_text(7), "     ZELDA.NES") && strstr(row_text(7), "25K"));
    CHECK(color_at(4, 3) == C_CYAN && color_at(4, 5) == C_CYAN && color_at(5, 5) == C_WHITE && color_at(5, 53) == C_GRAY);
    CHECK(!strcmp(row_text(25), " 3 GAMES  2 FOLDERS") && color_at(25, 1) == C_GRAY);
    CHECK(!strcmp(row_text(27), " ENTER OPEN  BKSP BACK  TAB TANG/EVO  PGUP/PGDN PAGE"));
    CHECK(color_at(27, 1) == C_CYAN && color_at(27, 7) == C_GRAY);
    CHECK(!strcmp(row_text(28), " F8 SIZE AUTO 95%  F7 4:3/16:9  F5/F6 STATE  ESC CLOSE"));
    CHECK(!strcmp(row_text(29), " GAME  X/Z A/B  SPACE SELECT  ENTER START  F12 WC"));
    CHECK(sel_row() == 3);
    dump("osd_root.hex");

    /* в папку Games: «..», затем 30 игр по порядку (в каталоге — от 30 к 1), страницы по 21 */
    CHECK(press(HID_DOWN) && sel_row() == 4 && press(HID_ENTER));
    CHECK(!strcmp(menu_path[0], "/Games") && strstr(row_text(1), "PAGE 1/2"));
    CHECK(!strcmp(row_text(3), "   # .."));
    CHECK(strstr(row_text(4), " >   GAME 01 (EUROPE).NES") == row_text(4) && sel_row() == 4);
    CHECK(strstr(row_text(23), "GAME 20 (EUROPE).NES") && strstr(row_text(23), "129K"));
    CHECK(!strcmp(row_text(25), " 30 GAMES  0 FOLDERS"));
    dump("osd_games.hex");
    CHECK(press(HID_PGDN) && strstr(row_text(1), "PAGE 2/2") && sel_row() == 4);   /* курсор 22 — Game 22 */
    CHECK(strstr(row_text(4), ">   GAME 22 (EUROPE).NES") && strstr(row_text(11), "GAME 29") && !row_text(13)[0]);
    CHECK(press(HID_END) && sel_row() == 12 && strstr(row_text(12), ">   GAME 30"));
    CHECK(press(HID_DOWN) && sel_row() == 12);       /* дальше некуда */
    CHECK(press(HID_HOME) && strstr(row_text(1), "PAGE 1/2") && sel_row() == 3);
    for (int i = 0; i < 20; i++) CHECK(press(HID_DOWN));
    CHECK(sel_row() == 23 && press(HID_DOWN) && strstr(row_text(1), "PAGE 2/2") && sel_row() == 3);   /* ↓ — через страницу */
    CHECK(press(HID_UP) && strstr(row_text(1), "PAGE 1/2") && sel_row() == 23);
    CHECK(press(HID_PGUP) && sel_row() == 3);
    /* назад: курсор — на папке Games */
    CHECK(press(HID_BKSP) && !strcmp(menu_path[0], "/") && sel_row() == 4 && strstr(row_text(4), "> # GAMES"));

    /* длинное имя — с многоточием, размер справа */
    CHECK(press(HID_UP) && press(HID_ENTER) && !strcmp(menu_path[0], "/Arcade"));
    CHECK(strstr(row_text(4), "A VERY LONG CARTRIDGE FILE NAME") && strstr(row_text(4), "~") && strstr(row_text(4), "1.0M"));
    CHECK(press(HID_BKSP) && sel_row() == 3);

    /* загрузка с карты Tang: строка «LOADING», полоса растёт, окно — до конца передачи */
    CHECK(press(HID_DOWN) && press(HID_DOWN) && press(HID_DOWN));                    /* CONTRA.NES */
    bar_writes = 0; bar_max_fill = 0; bar_trace_n = 0;
    CHECK(menu_key(HID_ENTER));
    CHECK(!strcmp(last_read, "/contra.nes") && received == sizeof cartridge && !memcmp(rom_seen, cartridge, sizeof cartridge));
    CHECK(!menu_open && !shown() && pause_reg == 0 && !menu_key(HID_ENTER | 0x80));
    CHECK(bar_trace_n >= 3);
    {
        unsigned grows = 1, last = 0;
        for (unsigned i = 0; i < bar_trace_n; i++) {
            if (!(bar_trace[i] >> 31)) continue;
            CHECK(((bar_trace[i] >> 26) & 31) == ROW_STATUS && ((bar_trace[i] >> 20) & 63) == BAR_COL &&
                  ((bar_trace[i] >> 14) & 63) == BAR_COLS);
            if ((bar_trace[i] & 0x3FF) < last) grows = 0;
            last = bar_trace[i] & 0x3FF;
        }
        CHECK(grows && last == BAR_COLS * 16 - 14);   /* до 100 % */
    }
    CHECK(strstr(row_text(25), "LOADING CONTRA.NES") && strstr(row_text(25), "100%"));

    /* вкладка Evo: сортировка, READ — исходный номер выбранной игры */
    plugin_on = 1;
    CHECK(menu_key(HID_F1) && menu_open && press(HID_TAB) && menu_card == 1);
    CHECK(strstr(row_text(0), ">EVO SD") && color_at(0, 47) == C_YELLOW && color_at(0, 37) == C_GRAY);
    CHECK(!strcmp(row_text(3), " > # RPG") && strstr(row_text(4), "     ALADDIN.NES") && strstr(row_text(5), "     MARIO.NES"));
    CHECK(!strstr(row_text(4), "K"));               /* у карты Evo размера нет */
    CHECK(press(HID_DOWN) && press(HID_DOWN));      /* MARIO.NES — в каталоге номер 0 */
    bar_trace_n = 0;
    dump("osd_evo.hex");
    CHECK(menu_key(HID_ENTER) && plugin_reads == 1 && read_index == 0 && !menu_open);
    {
        unsigned seen_mid = 0;                    /* ход приёма с карты Evo — между 0 и 70 % */
        for (unsigned i = 0; i < bar_trace_n; i++) {
            const unsigned f = bar_trace[i] & 0x3FF;
            if (f > 0 && f < (BAR_COLS * 16 - 14) * 70 / 100) seen_mid = 1;
        }
        CHECK(seen_mid);
    }
    plugin_on = 0;

    /* снимок окна во время загрузки — для стенда отрисовки */
    menu_open = 1;
    menu_card = 0;
    menu_read_dir();
    menu_cursor = 3;
    menu_set_status("LOADING CONTRA.NES", C_WHITE);
    menu_progress = 62;
    menu_draw();
    CHECK(strstr(row_text(25), "62%") && (bar_reg >> 31) && (bar_reg & 0x3FF) == (BAR_COLS * 16 - 14) * 62 / 100);
    dump("osd_loading.hex");
    menu_progress = -1;
    menu_status[0] = 0;

    /* старое ядро 0.9: ASCII, без подложек и полосы */
    hw_version = core_version = 0x04010009;
    band_writes = 0;
    menu_draw();
    CHECK(band_writes == 0 && strstr(row_text(3), "   ARCADE/") == row_text(3) && strstr(row_text(2), "-----"));
    CHECK(strstr(row_text(6), " > CONTRA.NES") == row_text(6) && sel_row() == 6);
    CHECK(strstr(row_text(0), ">TANG SD"));
    menu_close();
    hw_version = core_version = 0x0401000A;

    menu_items_free();
    CHECK(allocated == freed);
    printf("PASS %u checks: sorted list (dirs, then names), layout rows, glyphs, paging, back to the folder, sizes, ellipsis, load progress, Evo index, old core fallback\n", checks);
    return 0;
}
