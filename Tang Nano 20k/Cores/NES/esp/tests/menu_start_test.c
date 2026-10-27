/* SPDX-License-Identifier: Apache-2.0 */
/* Старт NES из меню ядер плагина (NES.ELF 0.7): START без картриджа — ядро грузится без игры, машина на
 * паузе, сразу меню F1 без выхода, пока не загружен .nes. Настоящие app.c и game_menu.h на модели ядра NES
 * 0.8 (окно OSD 0x3000, 0xC0 — показ, 0xC4 — пауза, 0x80 — пропорции, загрузка картриджа 0x44…0x54) и
 * карты Tang. Проверяются:
 *  - START без образа принят (menu_start), с образом — прежний путь (картридж — до показа);
 *  - загрузка ядра без картриджа: ни одного байта картриджа, меню показано, пауза, внизу «CHOOSE A .NES»;
 *  - Esc и F1 меню не закрывают; F5 — «нет игры», меню на месте;
 *  - файл не читается — ошибка в строке меню, сеанс жив (состояние 2, не 255), меню без выхода;
 *  - выбран .nes — картридж в ядре (CRC), меню закрыто, пауза снята; дальше Esc закрывает меню, как всегда;
 *  - ядро без окна меню (0.6) — отказ с текстом и возврат в слот 0;
 *  - F12 из меню без игры — выход в WC без записи сохранений;
 *  - следующий START без образа в том же NES.ELF после игры с карты Tang — снова меню, не прошлая игра;
 *  - размер картинки (NES.ELF 0.8, ядро 0.9): AUTO — SIZE слота 0 (CTRL 0x40 до загрузки ядра) → регистр 0x84;
 *    F8 в меню: AUTO → 100 → 95 → 90 %, сразу в ядро, подсказка 28; при выходе — picture_size в nes.cfg, в
 *    следующем сеансе — из него; nes.cfg без перемен не переписывается; у ядра 0.8 F8 в подсказке нет.
 * Ожидается: «PASS». */
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
#define CHECK(x) do { ++checks; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static void *mem_alloc(size_t n, uint32_t caps) { (void)caps; ++allocated; return malloc(n); }
static void mem_free(void *p) { if (p) { ++freed; free(p); } }
static void no_lock(void *p) { (void)p; }
static int reply(uint8_t cmd, const void *p, uint16_t n) { (void)cmd; memcpy(answer, p, n); answer_n = n; return ZM_OK; }
static char log_last[256];
static void log_line(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(log_last, sizeof log_last, format, args);
    va_end(args);
}
static uint32_t millis(void) { return now; }
static void delay(uint32_t n) { now += n; }

/* ---- модель ядра NES ---- */
static uint32_t hw_version = 0x04010008, osd[512], osd_ctrl = 63u << 8, pause_reg, keys_clear, aspect = 1;
static int loading, loads, backs;
/* слот 0 на LINK до core_load (после core_back — снова): TS-Config 1.32, CTRL с SIZE в битах 16:15 */
static int ts_on_link = 1;
static uint32_t ts_ctrl, size_reg;
static uint32_t seq, received, crc = 0xffffffffu, packet_words[64];
static uint8_t rom_seen[65536];
static uint32_t crc_byte(uint32_t c, uint8_t b) { c ^= b; for (int i = 0; i < 8; i++) c = (c >> 1) ^ (c & 1 ? 0xedb88320u : 0); return c; }
static int wr(uint32_t a, const uint32_t *w, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++, a += 4) {
        const uint32_t v = w[i];
        if (a >= W_OSD && a < W_OSD + 2048) osd[(a - W_OSD) / 4] = v;
        else if (a == R_OSD_CTRL) osd_ctrl = v;
        else if (a == R_PAUSE) pause_reg = v;
        else if (a == R_KEYS_CLEAR) keys_clear++;
        else if (a == 0x80 && (v >> 16) == 0x4153) aspect = v & 1;
        else if (a == 0x84 && (v >> 16) == 0x535A && !ts_on_link && hw_version >= 0x04010009u) size_reg = v & 3;
        else if (a >= 0x4000 && a < 0x4100) packet_words[(a - 0x4000) / 4] = v;
        else if (a == 0x44) {
            if (v == 1) { loading = 1; seq = received = 0; crc = 0xffffffffu; }
            else if (v == 2) loading = 0;
        } else if (a == 0x48) {
            CHECK(loading && (v >> 16) == seq + 1);
            const uint32_t count = v & 0xffff;
            for (uint32_t k = 0; k < count; k++) {
                const uint8_t b = (uint8_t)(packet_words[k / 4] >> (8 * (k & 3)));
                if (received < sizeof rom_seen) rom_seen[received] = b;
                crc = crc_byte(crc, b);
                received++;
            }
            seq = v >> 16;
        }
    }
    return ZM_OK;
}
static int rd(uint32_t a, uint32_t *w, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++, a += 4) {
        uint32_t v = 0;
        if (a == 4) v = ts_on_link ? 0x01010120u : hw_version;
        else if (a == 0x40 && ts_on_link) v = ts_ctrl;
        else if (a == 0x84 && !ts_on_link && hw_version >= 0x04010009u) v = 0x535A0000u | size_reg;
        else if (a == 0x44) v = (uint32_t)loading | (received == sizeof cartridge ? 4u : 0u);
        else if (a == 0x48) v = seq;
        else if (a == 0x4c) v = received;
        else if (a == 0x50) v = ~crc;
        else if (a == 0x80) v = 0x41530000u | aspect << 8 | (aspect ^ 255u);
        else if (a == R_PAUSE) v = pause_reg;
        else if (a == R_CLK_KHZ) v = 21477;
        w[i] = v;
    }
    return ZM_OK;
}
static int core_load(const char *name, uint32_t *version)
{
    CHECK(!strcmp(name, "NES"));
    loads++;
    pause_reg = 0;                                       /* новое ядро: машина идёт, окна нет */
    osd_ctrl = 63u << 8;
    loading = 0;
    ts_on_link = 0;
    *version = hw_version;
    return ZM_OK;
}
static int core_back(uint32_t ms) { (void)ms; backs++; ts_on_link = 1; return ZM_OK; }

/* ---- модель карты Tang: / — a.nes, bad.nes (не читается); nes.cfg — нет ---- */
static char last_read[512];
static unsigned rom_reads;
static char cfg_disk[2049];
static size_t cfg_disk_n;
static int sd_list(const char *path, const char *suffix, uint32_t skip, zm_sd_entry_t *out,
                   uint32_t cap, uint32_t *count, int *more, const volatile int *active)
{
    CHECK(!strcmp(suffix, ".nes") && *active);
    static const char *names[] = {"a.nes", "bad.nes"};
    if (strcmp(path, "/")) return ZM_NOT_FOUND;
    *count = 0;
    for (uint32_t i = skip; i < 2 && *count < cap; i++) {
        memset(&out[*count], 0, sizeof out[0]);
        strcpy(out[(*count)++].name, names[i]);
    }
    *more = 0;
    return ZM_OK;
}
static int sd_read(const char *path, uint8_t **out, size_t *size, size_t max, const volatile int *active)
{
    CHECK(*active);
    *out = NULL;
    *size = 0;
    if (strstr(path, "nes.cfg") && !strstr(path, ".bak") && cfg_disk_n) {   /* nes.cfg — что записали */
        *size = cfg_disk_n;
        *out = mem_alloc(cfg_disk_n, ZM_MEM_PSRAM);
        memcpy(*out, cfg_disk, cfg_disk_n);
        return ZM_OK;
    }
    if (strstr(path, ".cfg") || strstr(path, ".sav") || strstr(path, ".svb") || strstr(path, ".bak")) return ZM_NOT_FOUND;
    CHECK(max == NES_ROM_MAX);
    strcpy(last_read, path);
    rom_reads++;
    if (strstr(path, "bad.nes")) return ZM_ERROR;
    *size = sizeof cartridge;
    *out = mem_alloc(*size, ZM_MEM_PSRAM);
    memcpy(*out, cartridge, *size);
    return ZM_OK;
}
static unsigned cfg_writes;
static int sd_write(const char *path, const uint8_t *data, size_t n, const volatile int *active)
{
    (void)active;
    CHECK(strstr(path, "nes.cfg") != NULL);              /* только настройки ядра, не .sav */
    CHECK(n < sizeof cfg_disk);
    memcpy(cfg_disk, data, n);
    cfg_disk[n] = 0;
    cfg_disk_n = n;
    cfg_writes++;
    return ZM_OK;
}

static const char *row_text(unsigned r)
{
    static char s[OSD_COLS + 1];
    for (unsigned c = 0; c < OSD_COLS; c++) {
        const uint8_t b = (uint8_t)(osd[r * 16 + c / 4] >> (8 * (c & 3)));
        s[c] = (char)((b & 63) + 0x20);
    }
    s[OSD_COLS] = 0;
    for (int c = OSD_COLS - 1; c >= 0 && s[c] == ' '; c--) s[c] = 0;
    return s;
}
static int shown(void) { return osd_ctrl & 1; }
static int press(unsigned code) { const int a = menu_key((uint16_t)code); const int b = menu_key((uint16_t)(code | 0x80)); return a && b; }

static void start_cmd(void)
{
    static const uint8_t name[] = "NES";
    answer[0] = 0;
    command(0x70, name, sizeof name);
}

int main(void)
{
    zm_core_t api = {0};
    api.size = sizeof api; api.abi_major = 1; api.abi_minor = 9;
    api.alloc = mem_alloc; api.free = mem_free; api.reply = reply; api.log = log_line; api.delay = delay;
    api.millis = millis; api.mutex_lock = api.mutex_unlock = api.signal_give = no_lock;
    api.link_read = rd; api.link_write = wr; api.sd_list = sd_list; api.sd_read = sd_read; api.sd_write = sd_write;
    api.core_load = core_load; api.core_back = core_back;
    core = &api;
    running = 1;
    CHECK(!(cartridge[6] & 2));                          /* тестовый образ — без battery */

    /* START без образа — принят, это старт из меню */
    state = 0;
    start_cmd();
    CHECK(answer[0] == 1 && state == 1 && menu_start == 1);
    CHECK(load_core());
    CHECK(state == 2 && loads == 1 && backs == 0 && received == 0 && !loading);   /* картриджа не было */
    CHECK(game_crc == 0 && !rom_data);
    CHECK(menu_open && menu_force && pause_reg == 1 && shown());
    CHECK(strstr(row_text(0), "NES SELECT A GAME") && strstr(row_text(0), ">TANG SD"));
    CHECK(strstr(row_text(MENU_TOP), " > A.NES") == row_text(MENU_TOP) && strstr(row_text(MENU_TOP + 1), "   BAD.NES") == row_text(MENU_TOP + 1));
    CHECK(strstr(row_text(ROW_HELP + 1), "F12 EXIT") && !strstr(row_text(ROW_HELP + 1), "ESC"));
    CHECK(strstr(log_last, "game from the F1 menu"));

    /* Esc и F1 не закрывают: игры нет */
    CHECK(press(HID_ESC) && menu_open && pause_reg == 1 && shown());
    CHECK(strstr(row_text(ROW_STATUS), "CHOOSE A .NES FILE") && strstr(row_text(ROW_STATUS), "F12"));
    CHECK(press(HID_F1) && menu_open && pause_reg == 1 && shown());
    /* F5 без игры — отказ в строке меню, меню на месте */
    CHECK(press(HID_F5) && menu_open && strstr(row_text(ROW_STATUS), "NO GAME TO SAVE") && pause_reg == 1);

    /* файл не читается: ошибка в меню, сеанс жив */
    CHECK(press(HID_DOWN) && press(HID_ENTER));
    CHECK(strstr(last_read, "/bad.nes") && state == 2 && menu_open && menu_force && pause_reg == 1);
    CHECK(row_text(ROW_STATUS)[0] && !strstr(row_text(ROW_STATUS), "LOADING") && !error_text[0]);
    CHECK(received == 0 && !rom_data);

    /* выбран a.nes: картридж в ядре, меню закрыто, пауза снята */
    CHECK(press(HID_UP));
    CHECK(menu_key(HID_ENTER));
    CHECK(!menu_key(HID_ENTER | 0x80));                   /* меню закрыто: отпускание — игре */
    CHECK(strstr(last_read, "/a.nes") && state == 2);
    CHECK(!menu_open && !menu_force && pause_reg == 0 && !shown());
    CHECK(received == sizeof cartridge && !memcmp(rom_seen, cartridge, sizeof cartridge) && game_crc != 0);
    CHECK(!rom_data && !menu_loading);

    /* дальше меню — обычное: Esc закрывает, внизу прежние клавиши */
    CHECK(menu_key(HID_F1) && menu_open && !menu_force && strstr(row_text(ROW_HELP + 1), "F7 4:3/16:9") && strstr(row_text(ROW_HELP + 1), "ESC CLOSE"));
    CHECK(menu_key(HID_ESC) && !menu_open && pause_reg == 0);

    /* F12 (EXIT) после игры из меню: назад в слот 0, сохранений нет — только nes.cfg */
    command(0x73, NULL, 0);
    CHECK(answer[0] == 1 && exit_req);
    leave_core();
    CHECK(backs == 1 && state == 3 && cfg_writes >= 1);

    /* Снова START без образа в том же NES.ELF (так бывает после сброса Evo): прошлая игра была с карты
       Tang, но это снова меню — rom_from_sd снят в leave_core. И F12 прямо из меню без игры */
    state = 0;
    const unsigned writes_before = cfg_writes;
    received = 0;
    start_cmd();
    CHECK(answer[0] == 1 && menu_start);
    CHECK(load_core() && menu_open && menu_force && received == 0);
    command(0x73, NULL, 0);
    leave_core();
    /* nes.cfg тот же — не переписывается */
    CHECK(backs == 2 && state == 3 && cfg_writes == writes_before && strstr(cfg_disk, "aspect_wide=") && !menu_open);

    /* ядро без окна меню (0.6): отказ с текстом, Tang — назад в слот 0 */
    hw_version = 0x04010006;
    state = 0;
    start_cmd();
    CHECK(answer[0] == 1 && menu_start);
    CHECK(!load_core());
    CHECK(state == 255 && backs == 3 && strstr(error_text, "NES core 0.7+"));
    hw_version = 0x04010008;

    /* с образом (панель WC, 0x77…0x79) — прежний путь: картридж до игры, меню нет */
    state = 0;
    save_discard();                                       /* как у только что запущенного NES.ELF */
    {
        uint8_t size[4];
        wr32(size, sizeof cartridge);
        command(0x77, size, 4);
        CHECK(answer[0] == 1);
        for (size_t at = 0; at < sizeof cartridge; at += 512) {
            const size_t k = sizeof cartridge - at > 512 ? 512 : sizeof cartridge - at;
            uint8_t pk[518];
            wr32(pk, (uint32_t)at);
            memcpy(pk + 4, cartridge + at, k);
            const uint16_t c = rom_crc16(pk, k + 4);
            pk[k + 4] = (uint8_t)c;
            pk[k + 5] = (uint8_t)(c >> 8);
            command(0x78, pk, (uint16_t)(k + 6));
            CHECK(answer[0] == 1);
        }
        command(0x79, NULL, 0);
        CHECK(answer[0] == 1 && rom_ready);
    }
    received = 0;
    start_cmd();
    CHECK(answer[0] == 1 && !menu_start);
    CHECK(load_core());
    CHECK(state == 2 && !menu_open && !menu_force && pause_reg == 0 && received == sizeof cartridge);
    command(0x73, NULL, 0);
    leave_core();
    CHECK(state == 3);

    /* ---- размер картинки (NES.ELF 0.8, ядро 0.9) ---- */
    hw_version = 0x04010009;
    ts_ctrl = 1u << 15;                                   /* SIZE слота 0 — 95 % */
    cfg_disk_n = 0;                                       /* nes.cfg нет: AUTO */
    received = 0;
    state = 0;
    start_cmd();
    CHECK(answer[0] == 1 && menu_start);
    CHECK(load_core());
    CHECK(slot0_size_known && slot0_size == 1 && nes_size_cfg == 0 && size_reg == 1);   /* AUTO — как слот 0 */
    CHECK(strstr(row_text(ROW_HELP + 1), "F8 SIZE AUTO 95%"));
    CHECK(press(HID_F8) && nes_size_cfg == 1 && size_reg == 0 && strstr(row_text(ROW_HELP + 1), "F8 SIZE 100%"));
    CHECK(press(HID_F8) && nes_size_cfg == 2 && size_reg == 1 && strstr(row_text(ROW_HELP + 1), "F8 SIZE 95%"));
    CHECK(press(HID_F8) && nes_size_cfg == 3 && size_reg == 2 && strstr(row_text(ROW_HELP + 1), "F8 SIZE 90%"));
    CHECK(press(HID_F8) && nes_size_cfg == 0 && size_reg == 1 && strstr(row_text(ROW_HELP + 1), "F8 SIZE AUTO 95%"));
    CHECK(press(HID_F8) && press(HID_F8) && nes_size_cfg == 2 && size_reg == 1);
    CHECK(menu_open && menu_force && received == 0);    /* F8 игру не выбирает и меню не закрывает */
    command(0x73, NULL, 0);
    leave_core();
    CHECK(state == 3 && strstr(cfg_disk, "picture_size=2\n") && strstr(cfg_disk, "aspect_wide="));
    /* следующий сеанс: 95 % из nes.cfg, хотя у слота 0 теперь 90 % */
    ts_ctrl = 2u << 15;
    state = 0;
    start_cmd();
    CHECK(load_core());
    CHECK(slot0_size == 2 && nes_size_cfg == 2 && size_reg == 1 && strstr(row_text(ROW_HELP + 1), "F8 SIZE 95%"));
    command(0x73, NULL, 0);
    leave_core();
    /* AUTO снова: размер слота 0 — 90 % */
    {
        char *k = strstr(cfg_disk, "picture_size=2");
        CHECK(k);
        k[13] = '0';
    }
    state = 0;
    start_cmd();
    CHECK(load_core() && nes_size_cfg == 0 && size_reg == 2 && strstr(row_text(ROW_HELP + 1), "F8 SIZE AUTO 90%"));
    command(0x73, NULL, 0);
    leave_core();
    /* ядро 0.8 (регистра нет): строка — про ядро 0.9, F8 ничего не меняет */
    hw_version = 0x04010008;
    size_reg = 0;
    state = 0;
    start_cmd();
    CHECK(load_core() && !strstr(row_text(ROW_HELP + 1), "F8") && strstr(row_text(ROW_HELP + 1), "F7 4:3/16:9"));   /* размера нет — и F8 нет */
    CHECK(press(HID_F8) && nes_size_cfg == 0 && size_reg == 0);
    command(0x73, NULL, 0);
    leave_core();

    CHECK(allocated == freed);
    printf("PASS %u checks: START without a cartridge -> F1 menu that stays until a .nes loads, read errors stay in the menu, old core refused, WC file path unchanged\n", checks);
    return 0;
}
