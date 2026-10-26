/* SPDX-License-Identifier: Apache-2.0
 * Меню F1 поверх игры (ядро NES 0.7+, окно nes_osd): выбор образа на microSD Tang и Evo.
 * Пока меню открыто, машина NES стоит (пауза ядра), клавиши идут меню, а не игре.
 * Карта Tang — файловый API прошивки (0.1.96+: мост к карте есть и в ядре NES); выбранный образ
 * загружается в работающее ядро тем же путём, что и при старте (load_cartridge).
 * Окно — 56 × 30 знаков; знак — {цвет[1:0], ASCII − 0x20}; строчные показываются заглавными. */
#define R_OSD_CTRL  0xC0u         /* W {подсветка[13:8], показ[0]} */
#define R_PAUSE     0xC4u         /* W {пауза}; R {машина стоит} */
#define W_OSD       0x3000u       /* строка r — слова r·16…r·16+15 */
#define OSD_COLS    56
#define OSD_ROWS    30
#define MENU_TOP    3             /* первая строка списка */
#define MENU_LINES  22            /* строк списка на странице */
#define HID_F1      58
#define HID_ENTER   40
#define HID_ESC     41
#define HID_BKSP    42
#define HID_TAB     43
#define HID_PGUP    75
#define HID_PGDN    78
#define HID_RIGHT   79
#define HID_LEFT    80
#define HID_DOWN    81
#define HID_UP      82

enum { C_WHITE = 0, C_YELLOW = 1, C_CYAN = 2, C_GRAY = 3 };

static uint8_t osd_shadow[OSD_ROWS][64];
static int menu_open, menu_card;                  /* карта: 0 — Tang, 1 — Evo */
static char menu_path[2][512] = {"/", "/"};
static zm_sd_entry_t menu_entries[MENU_LINES];
static unsigned menu_count, menu_page, menu_sel;  /* строк на странице, страница, выбранная */
static int menu_more, menu_parent;                /* есть следующая страница; строка ".." */
static char menu_status[OSD_COLS + 1];
static int menu_status_color;

/* snapshot.h: F5/F6; образ в ядре — для снимков (CRC32, 0 — игры нет) */
static int snap_key(unsigned code, unsigned up);
static void snap_game(const uint8_t *rom, size_t size);
static uint32_t game_crc;

static void osd_clear(void) { memset(osd_shadow, 0, sizeof osd_shadow); }

/* Текст UTF-8 в строку окна с колонки col, не дальше width знаков; длиннее — последний знак '>' */
static void osd_put(unsigned row, unsigned col, const char *text, int color, unsigned width)
{
    if (row >= OSD_ROWS || col >= OSD_COLS) return;
    if (col + width > OSD_COLS) width = OSD_COLS - col;
    unsigned n = 0;
    const uint8_t *p = (const uint8_t *)text;
    while (*p && n < width) {
        unsigned ch = *p++;
        if (ch >= 0x80) {                         /* знак не ASCII — '?' */
            while ((*p & 0xc0) == 0x80) ++p;
            ch = '?';
        }
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        else if (ch == '`') ch = '\'';
        else if (ch == '{') ch = '(';
        else if (ch == '}') ch = ')';
        else if (ch == '|') ch = '!';
        else if (ch == '~') ch = '-';
        else if (ch < 0x20 || ch > 0x5f) ch = '?';
        if (n + 1 == width && *p) ch = '>';
        osd_shadow[row][col + n++] = (uint8_t)(color << 6 | (ch - 0x20));
    }
}

static int osd_flush(void)
{
    for (unsigned r = 0; r < OSD_ROWS; r++) {
        uint32_t words[16];
        for (unsigned w = 0; w < 16; w++) {
            const uint8_t *b = osd_shadow[r] + 4 * w;
            words[w] = b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
        }
        if (core->link_write(W_OSD + r * 64u, words, 16) != ZM_OK) return 0;
    }
    return 1;
}

static void menu_set_status(const char *text, int color)
{
    strncpy(menu_status, text, OSD_COLS);
    menu_status[OSD_COLS] = 0;
    menu_status_color = color;
}

/* ---- карта Evo: запросы плагину WC VDAC4 Cores 0.7+ ----
 * Запрос уходит в ответе на очередной ВВОД (0x71 -> 0xF1 [ok][операция]…), плагин отвечает
 * командами 0x6E (каталог) / 0x6F (отказ), файл — прежними 0x77…0x79. Имена и путь — байты
 * WC (CP866) как есть; файл ESP просит по номеру записи в каталоге, не по имени.
 *   1 LIST [пропуск LE16][до EVO_BATCH][путь, 0] -> 0x6E [итог][ещё][число][флаги, имя, 0]…
 *   2 READ [номер LE16][путь, 0] -> 0x77…0x79 или 0x6F [код] */
#define EVO_LIST    1
#define EVO_READ    2
#define EVO_BATCH   8
#define EVO_WAIT_MS 3000u
static uint8_t evo_req[1 + 3 + 512];
static uint16_t evo_req_n;
static volatile int evo_req_pending, evo_done, evo_status, evo_more, evo_code;   /* evo_code — 0x6F как есть */
static volatile unsigned evo_got;
static zm_sd_entry_t *evo_out;
static unsigned evo_cap;

/* Ответ на ВВОД: запрос карты Evo, если ждёт (команда — задача обмена) */
static void evo_reply_input(uint8_t ok)
{
    uint8_t reply[1 + sizeof evo_req];
    uint16_t n = 1;
    reply[0] = ok;
    core->mutex_lock(lock);
    if (ok && evo_req_pending) {
        memcpy(reply + 1, evo_req, evo_req_n);
        n = (uint16_t)(1 + evo_req_n);
        evo_req_pending = 0;
    }
    core->mutex_unlock(lock);
    core->reply(0xF1, reply, n);
}

/* 0x6E / 0x6F от плагина (задача обмена) */
static int evo_command(uint8_t cmd, const uint8_t *p, uint16_t n)
{
    const uint8_t ok = 1;
    core->mutex_lock(lock);
    input_ms = core->millis();                    /* плагин жив, хоть ВВОДа и нет */
    if (cmd == 0x6E && n >= 3) {
        evo_status = p[0];
        evo_more = p[1];
        unsigned at = 3, got = 0;
        for (unsigned i = 0; i < p[2] && got < evo_cap && at < n; i++) {
            zm_sd_entry_t *e = &evo_out[got];
            memset(e, 0, sizeof *e);
            e->flags = p[at++] & 1;
            unsigned k = 0;
            while (at < n && p[at] && k + 1 < sizeof e->name) e->name[k++] = (char)p[at++];
            while (at < n && p[at]) at++;
            at++;
            got++;
        }
        evo_got = got;
        evo_done = 1;
    } else if (cmd == 0x6F && n >= 1) {
        evo_status = p[0] ? p[0] : 255;
        evo_code = p[0];
        evo_done = 1;
    }
    core->mutex_unlock(lock);
    core->reply(cmd | 0x80, &ok, 1);
    core->signal_give(wake);
    return ZM_OK;
}

/* Запрос плагину и ожидание итога: 1 — пришёл; 0 — плагин молчит (старый WMF) */
static int evo_request(const uint8_t *req, uint16_t n, uint32_t wait_ms)
{
    core->mutex_lock(lock);
    memcpy(evo_req, req, n);
    evo_req_n = n;
    evo_done = 0;
    evo_got = 0;
    evo_status = 0;
    evo_code = 255;
    evo_req_pending = 1;
    core->mutex_unlock(lock);
    uint32_t start = core->millis(), seen = 0;
    while (running && !evo_done) {
        if (evo_rom_wait && rom_received != seen) { seen = (uint32_t)rom_received; start = core->millis(); }
        if (evo_rom_wait && rom_ready) return 1;
        if (core->millis() - start > wait_ms) break;
        core->delay(5);
    }
    core->mutex_lock(lock);
    evo_req_pending = 0;
    core->mutex_unlock(lock);
    return evo_done;
}

static size_t evo_put_path(uint8_t *at, const char *path)
{
    const size_t n = strlen(path) + 1;
    memcpy(at, path, n);
    return n;
}

/* Записи каталога: Tang — файловый API прошивки, Evo — плагин; 1 — да */
static int menu_fetch(uint32_t skip, zm_sd_entry_t *out, unsigned want, unsigned *count, int *more)
{
    *count = 0;
    *more = 0;
    if (menu_card == 0) {
        uint32_t n = 0;
        const int r = core->sd_list(menu_path[0], ".nes", skip, out, want, &n, more, &running);
        if (r != ZM_OK) {
            menu_set_status(r == ZM_BUSY ? "TANG SD IS BUSY, TRY AGAIN" :
                            r == ZM_NOT_FOUND ? "FOLDER NOT FOUND" : "CANNOT READ TANG SD", C_YELLOW);
            return 0;
        }
        *count = n;
        return 1;
    }
    uint8_t req[sizeof evo_req];
    req[0] = EVO_LIST;
    req[1] = (uint8_t)skip;
    req[2] = (uint8_t)(skip >> 8);
    req[3] = (uint8_t)(want > EVO_BATCH ? EVO_BATCH : want);
    const size_t n = 4 + evo_put_path(req + 4, menu_path[1]);
    evo_out = out;
    evo_cap = req[3];
    if (!evo_request(req, (uint16_t)n, EVO_WAIT_MS)) {
        menu_set_status("EVO SD: NO ANSWER (UPDATE WC/VDAC4COR.WMF)", C_YELLOW);
        return 0;
    }
    if (evo_status) { menu_set_status("FOLDER NOT FOUND ON EVO SD", C_YELLOW); return 0; }
    *count = evo_got;
    *more = evo_more;
    return 1;
}

static uint32_t menu_first;                       /* номер первой записи страницы (без "..") */

/* Список страницы: ".." (не в корне, только на первой странице), затем каталоги и *.nes */
static int menu_list(void)
{
    menu_count = menu_more = 0;
    menu_parent = strcmp(menu_path[menu_card], "/") != 0 && menu_page == 0;
    if (menu_card == 0 && !sd_available()) {
        menu_set_status("THE ESP FIRMWARE HAS NO TANG SD ACCESS", C_YELLOW);
        return 0;
    }
    const unsigned slots = MENU_LINES - (unsigned)menu_parent;
    uint32_t skip = menu_page ? menu_page * MENU_LINES - (strcmp(menu_path[menu_card], "/") != 0) : 0;
    menu_first = skip;
    unsigned got = 0;
    while (got < slots) {
        unsigned count = 0;
        int more = 0;
        const unsigned want = slots - got > 16 ? 16 : slots - got;
        if (!menu_fetch(skip, menu_entries + menu_parent + got, want, &count, &more)) {
            menu_count = (unsigned)menu_parent + got;
            if (menu_parent) { strcpy(menu_entries[0].name, ".."); menu_entries[0].flags = 1; }
            return 0;
        }
        got += count;
        skip += count;
        menu_more = more;
        if (!more || !count) break;
    }
    menu_count = (unsigned)menu_parent + got;
    if (menu_parent) { strcpy(menu_entries[0].name, ".."); menu_entries[0].flags = 1; }
    if (menu_more && got == slots) {
        /* «есть ещё» после полной страницы — проверить, что следующая страница не пуста */
        unsigned count = 0;
        int more = 0;
        zm_sd_entry_t probe;
        if (menu_fetch(skip, &probe, 1, &count, &more))
            menu_more = count != 0;
    }
    char text[OSD_COLS + 1];
    snprintf(text, sizeof text, "PAGE %u%s", menu_page + 1, menu_more ? "  (PGDN: MORE)" : "");
    menu_set_status(menu_count ? text : "NO .NES FILES HERE", C_YELLOW);
    return 1;
}

static void menu_draw(void)
{
    osd_clear();
    osd_put(0, 1, "NES VDAC4 - SELECT GAME", C_YELLOW, 30);
    osd_put(0, 36, menu_card == 0 ? "[TANG SD]" : " TANG SD ", menu_card == 0 ? C_YELLOW : C_GRAY, 9);
    osd_put(0, 46, menu_card == 1 ? "[EVO SD]" : " EVO SD ", menu_card == 1 ? C_YELLOW : C_GRAY, 8);
    const char *path = menu_path[menu_card];
    const size_t n = strlen(path);
    if (n > OSD_COLS - 2) {                       /* длинный путь — хвост */
        osd_put(1, 1, "...", C_GRAY, 3);
        osd_put(1, 4, path + n - (OSD_COLS - 5), C_GRAY, OSD_COLS - 5);
    } else osd_put(1, 1, path, C_GRAY, OSD_COLS - 2);
    for (unsigned i = 0; i < menu_count; i++) {
        const zm_sd_entry_t *e = &menu_entries[i];
        char text[260];
        if (e->flags & 1) snprintf(text, sizeof text, "%s/", e->name);
        else snprintf(text, sizeof text, "%s", e->name);
        osd_put(MENU_TOP + i, 2, text, (e->flags & 1) ? C_CYAN : C_WHITE, OSD_COLS - 3);
    }
    osd_put(26, 1, menu_status, menu_status_color, OSD_COLS - 2);
    osd_put(27, 1, "X/Z: A/B  SPACE: SELECT  ENTER: START  F12: EXIT TO WC", C_GRAY, OSD_COLS - 2);
    osd_put(28, 1, "UP/DN/PGUP/PGDN  ENTER: OPEN  BKSP: BACK  TAB: CARD", C_CYAN, OSD_COLS - 2);
    osd_put(29, 1, "ESC: CLOSE  F4: RESET  F5/F6: SAVE/LOAD  F7: 4:3/16:9", C_CYAN, OSD_COLS - 2);
    osd_flush();
    const unsigned sel = menu_count ? MENU_TOP + menu_sel : 63;
    link_wr(R_OSD_CTRL, sel << 8 | 1u);
}

static void menu_enter_dir(const char *name)
{
    char *path = menu_path[menu_card];
    if (!strcmp(name, "..")) {
        char *slash = strrchr(path, '/');
        if (slash == path) path[1] = 0; else if (slash) *slash = 0;
    } else {
        const size_t a = strlen(path), b = strlen(name);
        if (a + b + 2 >= sizeof menu_path[0]) { menu_set_status("PATH IS TOO LONG", C_YELLOW); return; }
        if (a > 1) path[a] = '/';
        memcpy(path + (a > 1 ? a + 1 : a), name, b + 1);
    }
    menu_page = menu_sel = 0;
    menu_list();
}

static void menu_close(void)
{
    link_wr(R_OSD_CTRL, 63u << 8);
    link_wr(R_PAUSE, 0);
    menu_open = 0;
}

static void menu_fail_load(const char *fallback)
{
    char text[OSD_COLS + 1];
    snprintf(text, sizeof text, "%.*s", OSD_COLS, error_text[0] ? error_text : fallback);
    rom_discard();
    save_discard();
    menu_set_status(text, C_YELLOW);
    menu_draw();
}

/* Образ с карты Evo: плагин шлёт файл номер index каталога прежними 0x77…0x79 */
static int menu_read_evo(void)
{
    uint8_t req[sizeof evo_req];
    const uint32_t index = menu_first + (menu_sel - (unsigned)menu_parent);
    req[0] = EVO_READ;
    req[1] = (uint8_t)index;
    req[2] = (uint8_t)(index >> 8);
    const size_t n = 3 + evo_put_path(req + 3, menu_path[1]);
    rom_discard();
    error_text[0] = 0;
    evo_rom_wait = 1;
    const int answered = evo_request(req, (uint16_t)n, EVO_WAIT_MS);
    evo_rom_wait = 0;
    if (!rom_ready || !rom_data) {
        if (!error_text[0])
            fail(!answered ? "EVO SD: NO ANSWER (UPDATE WC/VDAC4COR.WMF)" :
                 evo_status == 1 ? "FILE NOT FOUND ON EVO SD" : "CANNOT READ THE FILE FROM EVO SD");
        return 0;
    }
    save_discard();                               /* .sav с Evo при загрузке из меню не берётся */
    save_prepared = save_restored = 1;
    rom_from_sd = 0;
    return 1;
}

/* Образ — в работающее ядро: как при старте (с карты Tang подхватится .sav) */
static void menu_load(const zm_sd_entry_t *e)
{
    char path[512];
    const char *dir = menu_path[menu_card];
    const size_t a = strlen(dir), b = strlen(e->name);
    if (a + b + 2 >= sizeof path) { menu_set_status("PATH IS TOO LONG", C_YELLOW); menu_draw(); return; }
    memcpy(path, dir, a);
    if (a > 1) path[a] = '/';
    memcpy(path + (a > 1 ? a + 1 : a), e->name, b + 1);
    menu_set_status("LOADING...", C_YELLOW);
    menu_draw();
    error_text[0] = 0;
    if (menu_card == 1) {
        if (!menu_read_evo()) { menu_fail_load("CANNOT LOAD THIS FILE"); return; }
    } else {
        memcpy(rom_path, path, strlen(path) + 1);
        if (!rom_read_sd() || !save_prepare(rom_data, rom_size, 1) || !save_prepared || !save_restored) {
            menu_fail_load("CANNOT LOAD THIS FILE");
            return;
        }
        rom_from_sd = 1;
    }
    /* пауза снята до загрузки: иначе после сброса машина сразу встанет снова */
    link_wr(R_OSD_CTRL, 63u << 8);
    link_wr(R_PAUSE, 0);
    menu_open = 0;
    game_crc = 0;                                 /* прежней игры в ядре уже нет */
    if (!load_cartridge(rom_data, rom_size)) {
        rom_discard();
        save_discard();
        fail("cartridge transfer failed (length, CRC or parser)");
        menu_open = 1;
        link_wr(R_PAUSE, 1);
        menu_set_status("LOAD FAILED: LENGTH, CRC OR MAPPER", C_YELLOW);
        menu_draw();
        return;
    }
    snap_game(rom_data, rom_size);
    rom_discard();
    link_wr(R_KEYS_CLEAR, 0);
    core->log("nes: menu loaded %s", path);
}

static void menu_show(void)
{
    menu_open = 1;
    link_wr(R_KEYS_CLEAR, 0);                     /* что держали в игре — отпустить */
    link_wr(R_PAUSE, 1);
    menu_set_status("READING...", C_YELLOW);
    menu_draw();
    menu_list();
    if (menu_sel >= menu_count) menu_sel = menu_count ? menu_count - 1 : 0;
    menu_draw();
}

/* Событие клавиши (HID: код [6:0], отпускание — бит 7). 1 — съедено меню, игре не идёт. */
static int menu_key(uint16_t ev)
{
    if (core_version < 0x04010007) return 0;      /* окна меню в ядре нет */
    const unsigned code = ev & 0x7f, up = (ev & 0x80) != 0;
    if (snap_key(code, up)) return 1;             /* F5/F6 — и в игре, и в меню */
    if (!menu_open) {
        if (code == HID_F1 && !up) { menu_show(); return 1; }
        return 0;
    }
    if (up) return 1;
    switch (code) {
    case HID_F1: case HID_ESC:
        menu_close();
        break;
    case HID_UP:
        if (menu_sel) menu_sel--;
        else if (menu_page) { menu_page--; menu_list(); menu_sel = menu_count ? menu_count - 1 : 0; }
        menu_draw();
        break;
    case HID_DOWN:
        if (menu_sel + 1 < menu_count) menu_sel++;
        else if (menu_more) { menu_page++; menu_sel = 0; menu_list(); }
        menu_draw();
        break;
    case HID_PGUP:
        if (menu_page) { menu_page--; menu_list(); }
        menu_sel = 0;
        menu_draw();
        break;
    case HID_PGDN:
        if (menu_more) { menu_page++; menu_sel = 0; menu_list(); }
        else if (menu_count) menu_sel = menu_count - 1;
        menu_draw();
        break;
    case HID_BKSP:
        if (strcmp(menu_path[menu_card], "/")) { menu_enter_dir(".."); menu_draw(); }
        break;
    case HID_TAB: case HID_LEFT: case HID_RIGHT:
        menu_card ^= 1;
        menu_page = menu_sel = 0;
        menu_list();
        menu_draw();
        break;
    case HID_ENTER:
        if (menu_sel < menu_count) {
            const zm_sd_entry_t *e = &menu_entries[menu_sel];
            if (e->flags & 1) { menu_enter_dir(e->name); menu_draw(); }
            else menu_load(e);
        }
        break;
    default:
        break;
    }
    return 1;
}
