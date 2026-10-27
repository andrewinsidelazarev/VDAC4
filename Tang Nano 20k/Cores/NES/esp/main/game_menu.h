/* SPDX-License-Identifier: Apache-2.0
 * Меню F1 поверх игры (ядро NES 0.7+, окно nes_osd): выбор образа на microSD Tang и Evo.
 * Пока меню открыто, машина NES стоит (пауза ядра), клавиши идут меню, а не игре.
 * Карта Tang — файловый API прошивки (0.1.96+: мост к карте есть и в ядре NES); выбранный образ
 * загружается в работающее ядро тем же путём, что и при старте (load_cartridge).
 * Окно — 56 × 30 знаков; знак — {цвет[1:0], ASCII − 0x20}; строчные показываются заглавными.
 * 0.7: меню без выхода (menu_force) — игры в ядре нет: старт NES из меню ядер плагина (START без
 * картриджа) или сбой передачи картриджа. Esc и F1 его не закрывают, пока не загружен .nes; F12 — в
 * WC, как всегда. Ошибка загрузки из меню (файл не читается, не тот маппер) остаётся в строке
 * состояния меню, сеанс ядра не кончается (menu_loading: fail() не ставит состояние 255).
 * 0.8: F8 — размер картинки (ядро 0.9): AUTO (как SIZE слота 0) → 100 → 95 → 90 %; в ядро — сразу,
 * в nes.cfg (picture_size) — при выходе.
 * 0.9: каталог читается целиком и сортируется: «..», папки, затем файлы — по имени без учёта регистра
 * (у карты Evo запоминается номер записи: файл плагин отдаёт по номеру). Разметка окна:
 *   0 — шапка на подложке: «NES — SELECT A GAME», вкладки TANG SD / EVO SD;   1 — путь, страница;
 *   2, 24, 26 — разделители;   3…23 — список: значок папки, имя, у игр с карты Tang — размер;
 *   25 — строка состояния на подложке: итог папки, ошибка или ход загрузки с полосой и процентом;
 *   27…29 — подсказки: клавиши голубым, действия серым.
 * Подложки, разделители, полоса и значки (папка, многоточие, треугольник выбора) — у ядра 0.10+;
 * у старого ядра то же — знаками ASCII, без подложек и полосы. */
#define R_OSD_CTRL  0xC0u         /* W {подсветка[13:8], показ[0]} */
#define R_PAUSE     0xC4u         /* W {пауза}; R {машина стоит} */
#define R_OSD_BAND  0xD0u         /* ядро 0.10: строки на подложке [29:0] */
#define R_OSD_RULE  0xD4u         /* ядро 0.10: строки с разделителем [29:0] */
#define R_OSD_BAR   0xD8u         /* ядро 0.10: полоса хода (nes_osd.sv) */
#define OSD_FANCY_CORE 0x0401000Au
#define W_OSD       0x3000u       /* строка r — слова r·16…r·16+15 */
#define OSD_COLS    56
#define OSD_ROWS    30
#define ROW_TITLE   0
#define ROW_PATH    1
#define MENU_TOP    3             /* первая строка списка */
#define MENU_LINES  21            /* строк списка на странице: 3…23 */
#define ROW_STATUS  25
#define ROW_HELP    27
#define MENU_MAX_ITEMS 2000u      /* записей папки в списке; дальше — «FIRST 2000 ENTRIES» */
#define BAR_COL     34            /* полоса хода в строке состояния: столбцы 34…49, процент — 51…54 */
#define BAR_COLS    16
#define HID_F1      58
#define HID_F8      65            /* размер картинки: AUTO → 100 → 95 → 90 % (ядро 0.9+) */
#define HID_ENTER   40
#define HID_ESC     41
#define HID_BKSP    42
#define HID_TAB     43
#define HID_HOME    74
#define HID_PGUP    75
#define HID_END     77
#define HID_PGDN    78
#define HID_RIGHT   79
#define HID_LEFT    80
#define HID_DOWN    81
#define HID_UP      82
/* значки шрифта ядра 0.10 — на местах знаков, которых нет в именах FAT */
#define G_ELLIPSIS  '"'
#define G_MARK      '*'
#define G_FOLDER    '\\'

enum { C_WHITE = 0, C_YELLOW = 1, C_CYAN = 2, C_GRAY = 3 };

static uint8_t osd_shadow[OSD_ROWS][64];
static uint32_t osd_band_rows, osd_rule_rows, osd_bar;   /* оформление 0.10 — с текстом, osd_flush */
static int menu_open, menu_card;                  /* карта: 0 — Tang, 1 — Evo */
static int menu_force;                            /* меню без выхода: игры в ядре нет */
static char menu_path[2][512] = {"/", "/"};
static char menu_status[OSD_COLS + 1];
static int menu_status_color;
static int menu_progress = -1;                    /* ход в строке состояния, 0…100; −1 — нет */

/* список папки: «..» (не в корне), затем отсортированные папки и игры */
#define MI_DIR    1u
#define MI_PARENT 2u
#define MI_SIZE   4u              /* размер известен (карта Tang) */
typedef struct {
    char name[256];
    uint32_t size;
    uint16_t index;               /* номер записи в каталоге (у карты Evo — для READ) */
    uint8_t flags;
} menu_item_t;
static menu_item_t *menu_items;
static unsigned menu_item_n, menu_item_cap, menu_cursor, menu_games, menu_dirs;
static int menu_truncated;

/* snapshot.h: F5/F6; образ в ядре — для снимков (CRC32, 0 — игры нет) */
static int snap_key(unsigned code, unsigned up);
static void snap_game(const uint8_t *rom, size_t size);
static uint32_t game_crc;

static int osd_fancy(void) { return core_version >= OSD_FANCY_CORE; }

static void osd_clear(void)
{
    memset(osd_shadow, 0, sizeof osd_shadow);
    osd_band_rows = osd_rule_rows = osd_bar = 0;
}

static void osd_glyph(unsigned row, unsigned col, char ch, int color)
{
    if (row < OSD_ROWS && col < OSD_COLS) osd_shadow[row][col] = (uint8_t)(color << 6 | ((unsigned)ch - 0x20));
}

/* Текст UTF-8 в строку окна с колонки col, не дальше width знаков; длиннее — последний знак —
   многоточие (у старого ядра '>'). Вернёт число знаков. */
static unsigned osd_put(unsigned row, unsigned col, const char *text, int color, unsigned width)
{
    if (row >= OSD_ROWS || col >= OSD_COLS) return 0;
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
        else if (ch == '"' || ch == '*' || ch == '\\') ch = '\'';   /* их места — значки 0.10 */
        else if (ch < 0x20 || ch > 0x5f) ch = '?';
        if (n + 1 == width && *p) ch = osd_fancy() ? G_ELLIPSIS : '>';
        osd_shadow[row][col + n++] = (uint8_t)(color << 6 | (ch - 0x20));
    }
    return n;
}

/* Строка подсказки: '~' переключает цвет — клавиша голубым, действие серым */
static void osd_help(unsigned row, const char *text)
{
    char part[OSD_COLS + 1];
    unsigned col = 1, k = 0;
    int key = 0;
    for (const char *p = text;; p++) {
        if (*p == '~' || !*p) {
            part[k] = 0;
            if (col < OSD_COLS - 1) col += osd_put(row, col, part, key ? C_CYAN : C_GRAY, OSD_COLS - 1 - col);
            k = 0;
            if (!*p) break;
            key = !key;
        } else if (k + 1 < sizeof part) part[k++] = *p;
    }
}

/* текст справа, кончается в столбце OSD_COLS − 2 */
static void osd_put_right(unsigned row, const char *text, int color)
{
    const unsigned n = (unsigned)strlen(text);
    osd_put(row, OSD_COLS - 1 - (n < OSD_COLS - 2 ? n : OSD_COLS - 2), text, color, OSD_COLS - 2);
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
    if (osd_fancy()) {
        link_wr(R_OSD_BAND, osd_band_rows);
        link_wr(R_OSD_RULE, osd_rule_rows);
        link_wr(R_OSD_BAR, osd_bar);
    }
    return 1;
}

static void menu_set_status(const char *text, int color)
{
    strncpy(menu_status, text, OSD_COLS);
    menu_status[OSD_COLS] = 0;
    menu_status_color = color;
    menu_progress = -1;
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
static void (*evo_wait_tick)(void);               /* меню: ход приёма образа, пока ждём плагин */

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
        if (evo_rom_wait && evo_wait_tick) evo_wait_tick();
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

/* Записи каталога с номера skip: Tang — файловый API прошивки, Evo — плагин; 1 — да */
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

/* ---- список папки ---- */
static int menu_reserve(unsigned want)
{
    if (want <= menu_item_cap) return 1;
    unsigned cap = menu_item_cap ? menu_item_cap * 2 : 64;
    while (cap < want) cap *= 2;
    if (cap > MENU_MAX_ITEMS + 1) cap = MENU_MAX_ITEMS + 1;
    if (cap < want) return 0;
    menu_item_t *v = core->alloc(cap * sizeof *v, ZM_MEM_PSRAM);
    if (!v) return 0;
    if (menu_items) {
        memcpy(v, menu_items, menu_item_n * sizeof *v);
        core->free(menu_items);
    }
    menu_items = v;
    menu_item_cap = cap;
    return 1;
}

static void menu_items_free(void)
{
    if (menu_items) core->free(menu_items);
    menu_items = NULL;
    menu_item_n = menu_item_cap = menu_cursor = menu_games = menu_dirs = 0;
}

static unsigned menu_fold(unsigned char c) { return c >= 'a' && c <= 'z' ? c - 32u : c; }

/* папки раньше файлов, затем имя без учёта регистра (ASCII), затем порядок в каталоге */
static int menu_item_less(const menu_item_t *a, const menu_item_t *b)
{
    if ((a->flags & MI_DIR) != (b->flags & MI_DIR)) return (a->flags & MI_DIR) != 0;
    const unsigned char *x = (const unsigned char *)a->name, *y = (const unsigned char *)b->name;
    for (;; x++, y++) {
        const unsigned p = menu_fold(*x), q = menu_fold(*y);
        if (p != q) return p < q;
        if (!p) return a->index < b->index;
    }
}

static void menu_sort(menu_item_t *v, unsigned n)
{
    static const unsigned gaps[] = {701, 301, 132, 57, 23, 10, 4, 1};
    static menu_item_t t;
    for (unsigned g = 0; g < sizeof gaps / sizeof gaps[0]; g++) {
        const unsigned gap = gaps[g];
        for (unsigned i = gap; i < n; i++) {
            t = v[i];
            unsigned j = i;
            while (j >= gap && menu_item_less(&t, &v[j - gap])) { v[j] = v[j - gap]; j -= gap; }
            v[j] = t;
        }
    }
}

static int menu_root(void) { return !strcmp(menu_path[menu_card], "/"); }

static void menu_draw_status_row(void);

/* Папка целиком, сортировка; 1 — прочитана (ошибка — в строке состояния, список — что успели) */
static int menu_read_dir(void)
{
    static zm_sd_entry_t batch[16];
    menu_item_n = menu_cursor = menu_games = menu_dirs = 0;
    menu_truncated = 0;
    if (!menu_root() && menu_reserve(1)) {
        memset(&menu_items[0], 0, sizeof menu_items[0]);
        strcpy(menu_items[0].name, "..");
        menu_items[0].flags = MI_DIR | MI_PARENT;
        menu_item_n = 1;
    }
    if (menu_card == 0 && !sd_available()) {
        menu_set_status("THE ESP FIRMWARE HAS NO TANG SD ACCESS", C_YELLOW);
        return 0;
    }
    const unsigned first = menu_item_n;
    uint32_t skip = 0, shown = core->millis();
    int ok = 1;
    for (;;) {
        unsigned count = 0;
        int more = 0;
        if (!menu_fetch(skip, batch, menu_card ? EVO_BATCH : 16, &count, &more)) { ok = 0; break; }
        for (unsigned i = 0; i < count; i++) {
            if (menu_item_n - first >= MENU_MAX_ITEMS || !menu_reserve(menu_item_n + 1)) { menu_truncated = 1; break; }
            menu_item_t *it = &menu_items[menu_item_n++];
            memcpy(it->name, batch[i].name, sizeof it->name);
            it->name[sizeof it->name - 1] = 0;
            it->size = batch[i].size;
            it->index = (uint16_t)(skip + i);
            it->flags = (uint8_t)((batch[i].flags & 1 ? MI_DIR : 0) | (menu_card == 0 ? MI_SIZE : 0));
            if (it->flags & MI_DIR) menu_dirs++; else menu_games++;
        }
        skip += count;
        if (menu_truncated || !more || !count) break;
        if (core->millis() - shown > 150) {       /* большая папка: сколько уже прочитано */
            char text[OSD_COLS + 1];
            snprintf(text, sizeof text, "READING FOLDER... %u", menu_games + menu_dirs);
            menu_set_status(text, C_WHITE);
            menu_draw_status_row();
            shown = core->millis();
        }
    }
    menu_sort(menu_items + first, menu_item_n - first);
    if (ok) menu_status[0] = 0;
    return ok;
}

/* курсор — на запись name (после выхода из папки — на неё), иначе на первую после «..» */
static void menu_select(const char *name)
{
    menu_cursor = menu_item_n > 1 && (menu_items[0].flags & MI_PARENT) ? 1 : 0;
    if (!name) return;
    for (unsigned i = 0; i < menu_item_n; i++)
        if (!strcmp(menu_items[i].name, name)) { menu_cursor = i; return; }
}

/* ---- отрисовка ---- */
static void menu_size_text(char *out, size_t cap)
{
    if (core_version < NES_SIZE_CORE) { out[0] = 0; return; }
    if (nes_size_cfg) snprintf(out, cap, "SIZE %u%%", nes_size_percent(nes_size_cfg - 1));
    else if (slot0_size_known) snprintf(out, cap, "SIZE AUTO %u%%", nes_size_percent(slot0_size));
    else snprintf(out, cap, "SIZE AUTO");
}

static void menu_size_human(uint32_t size, char *out, size_t cap)
{
    if (size < 1024) snprintf(out, cap, "%uB", (unsigned)size);
    else if (size < 1024u * 1024u) snprintf(out, cap, "%uK", (unsigned)((size + 1023) / 1024));
    else snprintf(out, cap, "%u.%uM", (unsigned)(size >> 20), (unsigned)((size & 0xFFFFF) * 10 >> 20));
}

/* строка состояния: текст, ход загрузки (полоса и процент) или итог папки */
static void menu_put_status(void)
{
    const unsigned r = ROW_STATUS;
    memset(osd_shadow[r], 0, sizeof osd_shadow[r]);
    osd_bar = 0;
    if (menu_progress >= 0) {
        osd_put(r, 1, menu_status, menu_status_color, BAR_COL - 2);
        char pct[16];
        snprintf(pct, sizeof pct, "%d%%", menu_progress);
        osd_put_right(r, pct, C_WHITE);
        if (osd_fancy()) {
            const unsigned inner = BAR_COLS * 16 - 6 - 8;
            const unsigned fill = inner * (unsigned)menu_progress / 100u;
            osd_bar = 1u << 31 | (uint32_t)r << 26 | (uint32_t)BAR_COL << 20 | (uint32_t)BAR_COLS << 14 | fill;
        } else {
            char bar[BAR_COLS + 1];
            const unsigned k = (BAR_COLS - 2) * (unsigned)menu_progress / 100u;
            bar[0] = '[';
            for (unsigned i = 0; i < BAR_COLS - 2; i++) bar[1 + i] = i < k ? '#' : '-';
            bar[BAR_COLS - 1] = ']';
            bar[BAR_COLS] = 0;
            osd_put(r, BAR_COL, bar, C_CYAN, BAR_COLS);
        }
    } else if (menu_status[0]) {
        osd_put(r, 1, menu_status, menu_status_color, OSD_COLS - 2);
    } else {
        char text[OSD_COLS + 1];
        if (!menu_games && !menu_dirs) snprintf(text, sizeof text, "NO .NES FILES IN THIS FOLDER");
        else snprintf(text, sizeof text, "%u GAME%s  %u FOLDER%s%s", menu_games, menu_games == 1 ? "" : "S",
                      menu_dirs, menu_dirs == 1 ? "" : "S", menu_truncated ? "  (FIRST 2000 SHOWN)" : "");
        osd_put(r, 1, text, C_GRAY, OSD_COLS - 2);
    }
}

/* только строка состояния и полоса — во время чтения и загрузки */
static void menu_draw_status_row(void)
{
    if (!menu_open) return;
    menu_put_status();
    uint32_t words[16];
    for (unsigned w = 0; w < 16; w++) {
        const uint8_t *b = osd_shadow[ROW_STATUS] + 4 * w;
        words[w] = b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    }
    core->link_write(W_OSD + ROW_STATUS * 64u, words, 16);
    if (osd_fancy()) link_wr(R_OSD_BAR, osd_bar);
}

static void menu_draw(void)
{
    const int fancy = osd_fancy();
    osd_clear();
    if (fancy) {
        osd_band_rows = 1u << ROW_TITLE | 1u << ROW_STATUS;
        osd_rule_rows = 1u << 2 | 1u << (MENU_TOP + MENU_LINES) | 1u << (ROW_STATUS + 1);
    }
    /* шапка: название, вкладки карт (у активной — треугольник) */
    osd_put(ROW_TITLE, 1, "NES", C_YELLOW, 3);
    osd_put(ROW_TITLE, 5, "SELECT A GAME", C_WHITE, 20);
    if (fancy) osd_glyph(ROW_TITLE, menu_card == 0 ? 36 : 46, G_MARK, C_YELLOW);
    osd_put(ROW_TITLE, 37, "TANG SD", menu_card == 0 ? C_YELLOW : C_GRAY, 7);
    osd_put(ROW_TITLE, 47, "EVO SD", menu_card == 1 ? C_YELLOW : C_GRAY, 6);
    if (!fancy) osd_put(ROW_TITLE, menu_card == 0 ? 36 : 46, ">", C_YELLOW, 1);
    /* путь (длинный — хвост) и страница */
    const unsigned pages = menu_item_n ? (menu_item_n + MENU_LINES - 1) / MENU_LINES : 1;
    const unsigned page = menu_cursor / MENU_LINES;
    char where[32];
    snprintf(where, sizeof where, "PAGE %u/%u", page + 1, pages);
    const char *path = menu_path[menu_card];
    const size_t n = strlen(path), room = OSD_COLS - 4 - strlen(where);
    if (n > room) {                               /* длинный путь — многоточие и хвост */
        if (fancy) osd_glyph(ROW_PATH, 1, G_ELLIPSIS, C_GRAY);
        else osd_put(ROW_PATH, 1, "...", C_GRAY, 3);
        osd_put(ROW_PATH, fancy ? 2 : 4, path + n - (room - (fancy ? 1 : 3)), C_GRAY, (unsigned)room);
    } else osd_put(ROW_PATH, 1, path, C_GRAY, (unsigned)room);
    osd_put_right(ROW_PATH, where, C_GRAY);
    /* список */
    const unsigned first = page * MENU_LINES;
    for (unsigned i = 0; i < MENU_LINES && first + i < menu_item_n; i++) {
        const menu_item_t *it = &menu_items[first + i];
        const unsigned row = MENU_TOP + i;
        if (first + i == menu_cursor) osd_glyph(row, 1, fancy ? G_MARK : '>', C_YELLOW);
        if (it->flags & MI_DIR) {
            if (fancy) {
                osd_glyph(row, 3, G_FOLDER, C_CYAN);
                osd_put(row, 5, it->name, C_CYAN, OSD_COLS - 6);
            } else {
                char text[260];
                snprintf(text, sizeof text, "%s/", it->name);
                osd_put(row, 3, text, C_CYAN, OSD_COLS - 4);
            }
        } else {
            char size[12] = "";
            if (it->flags & MI_SIZE) menu_size_human(it->size, size, sizeof size);
            osd_put(row, fancy ? 5 : 3, it->name, C_WHITE, OSD_COLS - (fancy ? 6 : 4) - (size[0] ? 7 : 0));
            if (size[0]) osd_put_right(row, size, C_GRAY);
        }
    }
    menu_put_status();
    /* подсказки */
    char size[24], line[OSD_COLS * 2];
    menu_size_text(size, sizeof size);
    osd_help(ROW_HELP, "~ENTER~ OPEN  ~BKSP~ BACK  ~TAB~ TANG/EVO  ~PGUP/PGDN~ PAGE");
    snprintf(line, sizeof line, "%s%s%s~F7~ 4:3/16:9  ~F5/F6~ STATE  %s", size[0] ? "~F8~ " : "", size,
             size[0] ? "  " : "", menu_force ? "~F12~ EXIT" : "~ESC~ CLOSE");
    osd_help(ROW_HELP + 1, line);
    osd_help(ROW_HELP + 2, "GAME  ~X/Z~ A/B  ~SPACE~ SELECT  ~ENTER~ START  ~F12~ WC");
    if (!fancy) {                                 /* старое ядро: разделители — чёрточками */
        static const unsigned rules[] = {2, MENU_TOP + MENU_LINES, ROW_STATUS + 1};
        for (unsigned k = 0; k < sizeof rules / sizeof rules[0]; k++)
            for (unsigned c = 1; c < OSD_COLS - 1; c++) osd_glyph(rules[k], c, '-', C_GRAY);
    }
    osd_flush();
    const unsigned sel = menu_item_n ? MENU_TOP + menu_cursor % MENU_LINES : 63;
    link_wr(R_OSD_CTRL, sel << 8 | 1u);
}

/* ход загрузки: этап [lo, hi] по доле done/total; строка — не чаще раза в 100 мс */
static uint32_t menu_progress_ms;
static unsigned menu_progress_lo, menu_progress_hi;
static void menu_progress_set(unsigned percent, int force)
{
    if (percent > 100) percent = 100;
    const uint32_t now = core->millis();
    if (!force && (int)percent == menu_progress) return;
    if (!force && now - menu_progress_ms < 100) return;
    menu_progress = (int)percent;
    menu_progress_ms = now;
    menu_draw_status_row();
}
static void menu_progress_part(size_t done, size_t total)
{
    if (!total) return;
    menu_progress_set(menu_progress_lo + (unsigned)((menu_progress_hi - menu_progress_lo) * (uint64_t)done / total), 0);
}
static void menu_evo_tick(void) { menu_progress_part(rom_received, rom_size); }

static void menu_enter_dir(const char *name)
{
    char *path = menu_path[menu_card];
    char back[256] = "";
    if (!strcmp(name, "..")) {
        char *slash = strrchr(path, '/');
        if (slash) { strncpy(back, slash + 1, sizeof back - 1); back[sizeof back - 1] = 0; }
        if (slash == path) path[1] = 0; else if (slash) *slash = 0;
    } else {
        const size_t a = strlen(path), b = strlen(name);
        if (a + b + 2 >= sizeof menu_path[0]) { menu_set_status("PATH IS TOO LONG", C_YELLOW); return; }
        if (a > 1) path[a] = '/';
        memcpy(path + (a > 1 ? a + 1 : a), name, b + 1);
    }
    menu_set_status("READING FOLDER...", C_WHITE);
    menu_draw_status_row();
    menu_read_dir();
    menu_select(back[0] ? back : NULL);
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
    error_text[0] = 0;                            /* показано в меню; сеанс ядра продолжается */
    rom_discard();
    save_discard();
    menu_set_status(text, C_YELLOW);
    menu_draw();
}

/* Образ с карты Evo: плагин шлёт файл номер index каталога прежними 0x77…0x79 */
static int menu_read_evo(const menu_item_t *it)
{
    uint8_t req[sizeof evo_req];
    const uint32_t index = it->index;
    req[0] = EVO_READ;
    req[1] = (uint8_t)index;
    req[2] = (uint8_t)(index >> 8);
    const size_t n = 3 + evo_put_path(req + 3, menu_path[1]);
    rom_discard();
    error_text[0] = 0;
    evo_rom_wait = 1;
    evo_wait_tick = menu_evo_tick;
    const int answered = evo_request(req, (uint16_t)n, EVO_WAIT_MS);
    evo_wait_tick = NULL;
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

/* Образ — в работающее ядро: как при старте (с карты Tang подхватится .sav). Ошибки — в меню
   (menu_loading), не в состояние слоя */
static void menu_load_file(const menu_item_t *it);
static void menu_load(const menu_item_t *it)
{
    menu_loading = 1;
    menu_load_file(it);
    load_progress = NULL;
    menu_loading = 0;
}

static void menu_load_file(const menu_item_t *it)
{
    char path[512], text[OSD_COLS + 1];
    const char *dir = menu_path[menu_card];
    const size_t a = strlen(dir), b = strlen(it->name);
    if (a + b + 2 >= sizeof path) { menu_set_status("PATH IS TOO LONG", C_YELLOW); menu_draw(); return; }
    memcpy(path, dir, a);
    if (a > 1) path[a] = '/';
    memcpy(path + (a > 1 ? a + 1 : a), it->name, b + 1);
    snprintf(text, sizeof text, "LOADING %s", it->name);
    menu_set_status(text, C_WHITE);
    menu_progress = 0;
    menu_draw();
    error_text[0] = 0;
    if (menu_card == 1) {                         /* приём с карты Evo — 0…70 % */
        menu_progress_lo = 0;
        menu_progress_hi = 70;
        if (!menu_read_evo(it)) { menu_fail_load("CANNOT LOAD THIS FILE"); return; }
        menu_progress_lo = 70;
    } else {                                      /* чтение с карты Tang — до 40 %, .sav — до 45 % */
        memcpy(rom_path, path, strlen(path) + 1);
        if (!rom_read_sd()) { menu_fail_load("CANNOT LOAD THIS FILE"); return; }
        menu_progress_set(40, 1);
        if (!save_prepare(rom_data, rom_size, 1) || !save_prepared || !save_restored) {
            menu_fail_load("CANNOT LOAD THIS FILE");
            return;
        }
        menu_progress_set(45, 1);
        rom_from_sd = 1;
        menu_progress_lo = 45;
    }
    /* передача в ядро — до 100 %; окно меню видно, пауза снята до загрузки: иначе после сброса
       машина сразу встанет снова */
    menu_progress_hi = 100;
    load_progress = menu_progress_part;
    link_wr(R_PAUSE, 0);
    game_crc = 0;                                 /* прежней игры в ядре уже нет */
    if (!load_cartridge(rom_data, rom_size)) {
        rom_discard();
        save_discard();
        fail("cartridge transfer failed (length, CRC or parser)");
        error_text[0] = 0;
        menu_force = 1;                           /* прежней игры в ядре нет: только выбрать другую */
        link_wr(R_PAUSE, 1);
        menu_set_status("LOAD FAILED: LENGTH, CRC OR MAPPER", C_YELLOW);
        menu_draw();
        return;
    }
    menu_progress_set(100, 1);
    link_wr(R_OSD_CTRL, 63u << 8);
    menu_open = 0;
    menu_force = 0;
    menu_progress = -1;
    menu_status[0] = 0;
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
    char keep[256] = "";
    if (menu_item_n && menu_cursor < menu_item_n) strcpy(keep, menu_items[menu_cursor].name);
    menu_set_status("READING FOLDER...", C_WHITE);
    menu_item_n = 0;
    menu_draw();
    menu_read_dir();
    menu_select(keep[0] ? keep : NULL);
    menu_draw();
}

/* Старт без картриджа (app.c, START из меню ядер): меню без выхода, пока не загружен .nes */
static void menu_show_forced(void)
{
    menu_force = 1;
    menu_show();
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
        if (menu_force) {                         /* игры нет — закрывать нечего */
            menu_set_status("CHOOSE A .NES FILE  (F12: EXIT TO WC)", C_YELLOW);
            menu_draw();
        } else menu_close();
        break;
    case HID_UP:
        if (menu_cursor) menu_cursor--;
        menu_draw();
        break;
    case HID_DOWN:
        if (menu_cursor + 1 < menu_item_n) menu_cursor++;
        menu_draw();
        break;
    case HID_PGUP:
        menu_cursor = menu_cursor >= MENU_LINES ? menu_cursor - MENU_LINES : 0;
        menu_draw();
        break;
    case HID_PGDN:
        if (menu_item_n) menu_cursor = menu_cursor + MENU_LINES < menu_item_n ? menu_cursor + MENU_LINES : menu_item_n - 1;
        menu_draw();
        break;
    case HID_HOME:
        menu_cursor = 0;
        menu_draw();
        break;
    case HID_END:
        if (menu_item_n) menu_cursor = menu_item_n - 1;
        menu_draw();
        break;
    case HID_BKSP:
        if (!menu_root()) { menu_enter_dir(".."); menu_draw(); }
        break;
    case HID_F8:                                  /* размер картинки: сразу в ядро, в .cfg — при выходе */
        if (core_version >= NES_SIZE_CORE) {
            nes_size_cfg = (nes_size_cfg + 1) & 3;
            if (!nes_size_apply()) menu_set_status("PICTURE SIZE WAS NOT APPLIED", C_YELLOW);
        }
        menu_draw();
        break;
    case HID_TAB: case HID_LEFT: case HID_RIGHT:
        menu_card ^= 1;
        menu_set_status("READING FOLDER...", C_WHITE);
        menu_item_n = 0;
        menu_draw();
        menu_read_dir();
        menu_select(NULL);
        menu_draw();
        break;
    case HID_ENTER:
        if (menu_cursor < menu_item_n) {
            const menu_item_t *it = &menu_items[menu_cursor];
            if (it->flags & MI_DIR) {
                char name[256];
                strcpy(name, it->name);
                menu_enter_dir(name);
                menu_draw();
            } else menu_load(it);
        }
        break;
    default:
        break;
    }
    return 1;
}
