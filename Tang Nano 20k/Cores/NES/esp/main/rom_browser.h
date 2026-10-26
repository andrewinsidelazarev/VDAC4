/* SPDX-License-Identifier: Apache-2.0
 * Браузер microSD Tang: полные UTF-8 имена остаются в ESP. Плагин передаёт
 * индекс строки, поэтому усечение экранного имени не меняет выбранный файл. */
static zm_sd_entry_t browser_entries[8];
static char browser_path[512] = "/";
static uint32_t browser_count, browser_page;
static int browser_more;
static volatile int browser_phase; /* 0 нет, 1 чтение, 2 готово, 255 ошибка */
static int rom_from_sd;
static char rom_path[512];

static int sd_available(void)
{
    return core->size >= offsetof(zm_core_t, sd_read) + sizeof core->sd_read &&
           core->sd_list && core->sd_read;
}

static void browser_display(uint8_t *out, size_t capacity, const char *text)
{
    size_t used = 0;
    const uint8_t *p = (const uint8_t *)text;
    while (*p && used + 1 < capacity) {
        unsigned ch = *p++;
        if (ch >= 0xc2 && ch <= 0xdf && (*p & 0xc0) == 0x80) {
            ch = ((ch & 31) << 6) | (*p++ & 63);
            if (ch >= 0x410 && ch <= 0x43f) ch = ch - 0x410 + 0x80;
            else if (ch >= 0x440 && ch <= 0x44f) ch = ch - 0x440 + 0xe0;
            else if (ch == 0x401) ch = 0xf0;
            else if (ch == 0x451) ch = 0xf1;
            else ch = '?';
        } else if (ch >= 128) {
            while ((*p & 0xc0) == 0x80) ++p;
            ch = '?';
        }
        out[used++] = ch < 32 ? '.' : (uint8_t)ch;
    }
    out[used] = 0;
}

static int browser_child(char out[512], const char *name)
{
    const size_t prefix = strlen(browser_path), n = strlen(name);
    const size_t slash = prefix > 1;
    if (!n || strchr(name, '/') || strchr(name, '\\') || !strcmp(name, ".") ||
        !strcmp(name, "..") || prefix + slash + n >= 512) return 0;
    memcpy(out, browser_path, prefix);
    if (slash) out[prefix] = '/';
    memcpy(out + prefix + slash, name, n + 1);
    return 1;
}

/* Только worker: файловый API не блокирует обработчик команд WC. */
static void browser_read(void)
{
    const int r = core->sd_list(browser_path, ".nes", browser_page * 8,
                                browser_entries, 8, &browser_count, &browser_more, &running);
    core->mutex_lock(lock);
    if (r == ZM_OK) browser_phase = 2;
    else {
        browser_phase = 255;
        fail(r == ZM_BUSY ? "Tang SD is busy; try again" :
             r == ZM_NOT_FOUND ? "Folder not found on Tang SD" : "Cannot read Tang SD folder");
    }
    if (state == 4) state = 0;
    core->mutex_unlock(lock);
}

static int browser_command(uint8_t cmd, const uint8_t *p, uint16_t n)
{
    uint8_t reply[49 + 8 * 44] = {0};
    core->mutex_lock(lock);
    const int idle = state == 0 || state == 3 || state == 255;
    if (cmd == 0x75) {
        reply[0] = (uint8_t)browser_phase;
        if (browser_phase == 2) {
            reply[1] = (uint8_t)browser_count;
            reply[2] = (browser_more ? 1 : 0) | (browser_path[1] ? 2 : 0) | (browser_page ? 4 : 0);
            reply[3] = (uint8_t)browser_page;
            reply[4] = (uint8_t)(browser_page >> 8);
            browser_display(reply + 5, 44, browser_path);
            for (unsigned i = 0; i < browser_count; ++i) {
                reply[49 + 44 * i] = browser_entries[i].flags;
                browser_display(reply + 50 + 44 * i, 43, browser_entries[i].name);
            }
        }
        core->mutex_unlock(lock);
        return core->reply(0xf5, reply, (uint16_t)(49 + 44 * reply[1]));
    }
    if (idle && sd_available() && browser_phase != 1) {
        if (cmd == 0x74 && n == 2) {
            const unsigned op = p[0], row = p[1];
            if (op == 0) { strcpy(browser_path, "/"); browser_page = 0; reply[0] = 1; }
            else if (op == 1 && browser_phase == 2 && row < browser_count && (browser_entries[row].flags & 1)) {
                char child[512];
                if (browser_child(child, browser_entries[row].name)) {
                    strcpy(browser_path, child); browser_page = 0; reply[0] = 1;
                }
            } else if (op == 2 && browser_path[1]) {
                char *slash = strrchr(browser_path, '/');
                if (slash == browser_path) slash[1] = 0; else *slash = 0;
                browser_page = 0; reply[0] = 1;
            } else if (op == 3 && browser_phase == 2 && browser_more && browser_page < 65535) {
                ++browser_page; reply[0] = 1;
            } else if (op == 4 && browser_page) { --browser_page; reply[0] = 1; }
            if (reply[0]) { browser_phase = 1; state = 4; error_text[0] = 0; }
        } else if (cmd == 0x76 && n == 1 && browser_phase == 2 && p[0] < browser_count &&
                   !(browser_entries[p[0]].flags & 1) && browser_child(rom_path, browser_entries[p[0]].name)) {
            rom_discard();
            rom_from_sd = 1;
            reply[0] = 1;
        }
    }
    core->mutex_unlock(lock);
    const int r = core->reply(cmd | 0x80, reply, 1);
    if (reply[0] && cmd == 0x74) core->signal_give(wake);
    return r;
}

static int rom_read_sd(void)
{
    rom_discard();
    const int r = core->sd_read(rom_path, &rom_data, &rom_size, NES_ROM_MAX, &running);
    if (r != ZM_OK) {
        fail(r == ZM_NO_MEMORY ? "Not enough PSRAM for cartridge" :
             r == ZM_NOT_FOUND ? "Cartridge file not found" : "Cannot read cartridge from Tang SD");
        return 0;
    }
    const char *error = rom_check(rom_data, rom_size);
    if (error) { fail(error); rom_discard(); return 0; }
    rom_received = rom_size;
    rom_ready = 1;
    return 1;
}
