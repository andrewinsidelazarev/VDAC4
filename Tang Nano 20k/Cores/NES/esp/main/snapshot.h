/* SPDX-License-Identifier: Apache-2.0
 * Снимки F5/F6 (ядро NES 0.8+) — сохранить и загрузить игру в любой момент.
 * Машина встаёт на паузу; слова шины снимка (LINK c8 — адрес, cc — слово, запись — только на
 * паузе, адрес после неё +1) и память SDRAM мостом на паузе (6c, пакеты 5c/60/64/5000) уходят в
 * файл <образ>.sst рядом с образом: microSD Tang (файловый API прошивки) или карта Evo (плагин
 * VDAC4 Cores 0.9+, запросы в ответе на ВВОД, как у меню F1). Загрузка — только для той же игры
 * (CRC32 образа) и того же набора областей.
 * Порядок загрузки (как в тесте машины): мост на паузе, слова, память, мост снят, слово 0x00C
 * (байты SDRAM, которые видит NES) последним, пауза снята — машина продолжает с того же такта.
 * .sst: заголовок 64 байта {"NVD4SST1", версия, CRC32 образа, 0x00C, число областей,
 * области {адрес, длина}…}, слова шины (SS_WORDS × LE32), память областей, CRC32 всего прежнего;
 * длина дополнена нулями до 512 (сектор карты Evo). */
#define HID_F5      62
#define HID_F6      63
#define R_SS_ADDR   0xC8u
#define R_SS_DATA   0xCCu
#define R_SS_MEM    0x6Cu
#define SS_WORDS    0x240u
#define SS_HEADER   64u
#define SS_VERSION  1u
#define SS_SEEN     0x00Cu        /* байты SDRAM, которые видит NES: пишется последним */
#define SS_CART     0x05Fu        /* {"SS", …, маппер умеет снимок} — только чтение */
#define SS_CHUNK    128u          /* байтов в запросе плагину (кадр ESP -> Z80 не длиннее 256) */
#define EVO_SS_OPEN   3
#define EVO_SS_PUT    4
#define EVO_SS_COMMIT 5
#define EVO_SS_READ   6

static const uint16_t ss_ranges[][2] = {{0x000, 0x00B}, {0x010, 0x022}, {0x040, 0x06F}, {0x100, 0x23F}};
typedef struct { uint32_t at, size; } ss_region_t;

static int game_chr_ram, game_card;   /* карта образа: 0 — Tang (rom_path), 1 — Evo (знает плагин) */
static char game_path[512];

/* Образ загружен в ядро: запомнить, к какой игре и где лежит снимок */
static void snap_game(const uint8_t *rom, size_t size)
{
    game_crc = rom_crc(rom, size);
    game_chr_ram = size > 5 && rom[5] == 0;
    game_card = rom_from_sd ? 0 : 1;
    if (!game_card) memcpy(game_path, rom_path, strlen(rom_path) + 1);
}

static unsigned ss_regions(ss_region_t *r)
{
    unsigned n = 0;
    r[n++] = (ss_region_t){0x380000, 0x800};      /* ОЗУ процессора */
    r[n++] = (ss_region_t){0x300000, 0x800};      /* видеопамять (CIRAM) */
    r[n++] = (ss_region_t){0x3C0000, 0x2000};     /* ОЗУ картриджа */
    r[n++] = (ss_region_t){0x3FE000, 0x2000};     /* CHR-RAM и 4 экрана MMC3 */
    if (game_chr_ram) r[n++] = (ss_region_t){0x200000, 0x2000};
    return n;
}

static size_t ss_size(void)
{
    ss_region_t r[5];
    const unsigned n = ss_regions(r);
    size_t size = SS_HEADER + SS_WORDS * 4u + 4u;
    for (unsigned i = 0; i < n; i++) size += r[i].size;
    return (size + 511u) & ~511u;
}

static int ss_word(uint32_t a, uint32_t *v) { return link_wr(R_SS_ADDR, a) == ZM_OK && link_rd(R_SS_DATA, v) == ZM_OK; }

static int ss_pause(int on)
{
    if (link_wr(R_PAUSE, (uint32_t)on) != ZM_OK) return 0;
    const uint32_t t = core->millis();
    uint32_t v = 0;
    do {
        if (link_rd(R_PAUSE, &v) == ZM_OK && (v & 1) == (uint32_t)on) return 1;
        core->delay(1);
    } while (running && core->millis() - t < 200);
    return 0;
}

/* Мост SDRAM на паузе: on — взять, off — отдать NES. Номер пакета — от моста. */
static int ss_bridge(int on)
{
    uint32_t v = 0;
    if (link_wr(R_SS_MEM, (uint32_t)on) != ZM_OK || link_rd(R_SS_MEM, &v) != ZM_OK || (v & 1) != (uint32_t)on)
        return 0;
    if (on) {
        if (link_rd(0x60, &v) != ZM_OK) return 0;
        save_seq = (uint16_t)v;
        save_mode_bit = 8;
    } else save_mode_bit = 1;
    return 1;
}

/* Машина уже стоит. Снимок — в buf (ss_size() байтов). 0 — нет, текст — в *why. */
static int ss_capture(uint8_t *buf, const char **why)
{
    uint32_t v = 0;
    if (!ss_word(SS_CART, &v) || (v >> 16) != 0x5353) { *why = "NO SNAPSHOT BUS IN THE CORE"; return 0; }
    if (!(v & 1)) { *why = "THIS MAPPER HAS NO SNAPSHOTS"; return 0; }
    uint8_t *words = buf + SS_HEADER;
    memset(buf, 0, ss_size());
    for (unsigned k = 0; k < sizeof ss_ranges / sizeof ss_ranges[0]; k++)
        for (uint32_t a = ss_ranges[k][0]; a <= ss_ranges[k][1]; a++) {
            if (!ss_word(a, &v)) { *why = "LINK ERROR (SNAPSHOT BUS)"; return 0; }
            wr32(words + 4 * a, v);
        }
    uint32_t seen = 0;
    if (!ss_word(SS_SEEN, &seen)) { *why = "LINK ERROR (SNAPSHOT BUS)"; return 0; }
    ss_region_t r[5];
    const unsigned n = ss_regions(r);
    memcpy(buf, "NVD4SST1", 8);
    wr32(buf + 8, SS_VERSION);
    wr32(buf + 12, game_crc);
    wr32(buf + 16, seen);
    wr32(buf + 20, n);
    if (!ss_bridge(1)) { *why = "SDRAM BRIDGE REFUSED"; return 0; }
    uint8_t *at = words + SS_WORDS * 4u;
    int ok = 1;
    for (unsigned i = 0; i < n && ok; i++) {
        wr32(buf + 24 + 8 * i, r[i].at);
        wr32(buf + 28 + 8 * i, r[i].size);
        for (uint32_t o = 0; o < r[i].size && ok; o += 256, at += 256)
            ok = save_block(r[i].at + o, at, 0);
    }
    /* мост — назад NES, затем 0x00C: что NES видела до моста */
    if (!ss_bridge(0) || link_wr(R_SS_ADDR, SS_SEEN) != ZM_OK || link_wr(R_SS_DATA, seen) != ZM_OK) ok = 0;
    if (!ok) { *why = "SDRAM READ FAILED"; return 0; }
    wr32(at, rom_crc(buf, (size_t)(at - buf)));
    return 1;
}

static int ss_valid(const uint8_t *p, size_t n, const char **why)
{
    ss_region_t r[5];
    const unsigned count = ss_regions(r);
    if (!p || n != ss_size() || memcmp(p, "NVD4SST1", 8) || rom_u32(p + 8) != SS_VERSION) {
        *why = "NOT A SNAPSHOT OF THIS VERSION"; return 0;
    }
    if (rom_u32(p + 12) != game_crc) { *why = "SNAPSHOT IS OF ANOTHER GAME"; return 0; }
    size_t end = SS_HEADER + SS_WORDS * 4u;
    if (rom_u32(p + 20) != count) { *why = "SNAPSHOT DOES NOT FIT THIS GAME"; return 0; }
    for (unsigned i = 0; i < count; i++) {
        if (rom_u32(p + 24 + 8 * i) != r[i].at || rom_u32(p + 28 + 8 * i) != r[i].size) {
            *why = "SNAPSHOT DOES NOT FIT THIS GAME"; return 0;
        }
        end += r[i].size;
    }
    if (rom_u32(p + end) != rom_crc(p, end)) { *why = "SNAPSHOT IS DAMAGED (CRC)"; return 0; }
    return 1;
}

/* Машина уже стоит; снимок проверен */
static int ss_restore(const uint8_t *p, const char **why)
{
    uint32_t v = 0;
    if (!ss_word(SS_CART, &v) || (v >> 16) != 0x5353 || !(v & 1)) { *why = "THIS MAPPER HAS NO SNAPSHOTS"; return 0; }
    if (!ss_bridge(1)) { *why = "SDRAM BRIDGE REFUSED"; return 0; }
    /* слова — при мосте: запросы NES к SDRAM сняты, промежуточное состояние ничего не пишет */
    const uint8_t *words = p + SS_HEADER;
    int ok = 1;
    for (unsigned k = 0; k < sizeof ss_ranges / sizeof ss_ranges[0] && ok; k++) {
        ok = link_wr(R_SS_ADDR, ss_ranges[k][0]) == ZM_OK;
        for (uint32_t a = ss_ranges[k][0]; a <= ss_ranges[k][1] && ok; a++)
            ok = link_wr(R_SS_DATA, rom_u32(words + 4 * a)) == ZM_OK;
    }
    ss_region_t r[5];
    const unsigned n = ss_regions(r);
    const uint8_t *at = words + SS_WORDS * 4u;
    uint8_t block[256];
    for (unsigned i = 0; i < n && ok; i++)
        for (uint32_t o = 0; o < r[i].size && ok; o += 256, at += 256) {
            memcpy(block, at, 256);
            ok = save_block(r[i].at + o, block, 1);
        }
    if (!ss_bridge(0) || link_wr(R_SS_ADDR, SS_SEEN) != ZM_OK || link_wr(R_SS_DATA, rom_u32(p + 16)) != ZM_OK) ok = 0;
    if (!ok) *why = "SDRAM WRITE FAILED - RESET THE GAME (F4)";
    return ok;
}

/* Путь снимка на карте Tang: образ с .sst вместо .nes */
static int ss_tang_path(char *out)
{
    const size_t n = strlen(game_path);
    if (n < 5 || n >= 512 || game_path[n - 4] != '.') return 0;
    memcpy(out, game_path, n + 1);
    memcpy(out + n - 3, "sst", 3);
    return 1;
}

/* Карта Evo: открыть временный файл, части по SS_CHUNK, заменить .sst. 1 — записан. */
static int ss_evo_op(const uint8_t *req, uint16_t n)
{
    return evo_request(req, n, EVO_WAIT_MS) && evo_code == 0;
}

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
        if (!ss_evo_op(req, sizeof req)) { *why = "EVO SD: WRITE FAILED, OLD SNAPSHOT KEPT"; return 0; }
    }
    req[0] = EVO_SS_COMMIT;
    if (!ss_evo_op(req, 1)) { *why = "EVO SD: WRITE FAILED, OLD SNAPSHOT KEPT"; return 0; }
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

static void ss_done(int paused)
{
    if (menu_open) return;
    link_wr(R_OSD_CTRL, 63u << 8);
    if (paused) ss_pause(0);
    link_wr(R_KEYS_CLEAR, 0);                     /* что держали до F5/F6 — отпустить */
}

static void snap_save(void)
{
    const char *why = NULL;
    char path[512];
    if (core_version < 0x04010008u) { ss_message("SNAPSHOTS NEED NES CORE 0.8", C_YELLOW, 1500); ss_done(0); return; }
    if (!game_crc) { ss_message("NO GAME TO SAVE", C_YELLOW, 1500); ss_done(0); return; }
    if (game_card == 0 && (!save_sd_available() || !ss_tang_path(path))) {
        ss_message("TANG SD: NO FILE ACCESS FOR SNAPSHOTS", C_YELLOW, 1500); ss_done(0); return;
    }
    if (!ss_pause(1)) { ss_message("THE CORE DID NOT PAUSE", C_YELLOW, 1500); ss_done(0); return; }
    ss_message("SAVING STATE...", C_YELLOW, 0);
    const size_t size = ss_size();
    uint8_t *buf = core->alloc(size, ZM_MEM_PSRAM);
    int ok = buf != NULL;
    if (!ok) why = "NO PSRAM FOR THE SNAPSHOT";
    if (ok) ok = ss_capture(buf, &why);
    if (ok) {
        if (game_card == 0) {
            ok = core->sd_write(path, buf, size, &running) == ZM_OK;
            if (!ok) why = "TANG SD: WRITE FAILED, OLD SNAPSHOT KEPT";
        } else ok = ss_evo_write(buf, size, &why);
    }
    if (buf) core->free(buf);
    core->log("nes: snapshot save %s", ok ? "ok" : why);
    ss_message(ok ? "STATE SAVED  (F6 LOADS IT)" : why, ok ? C_CYAN : C_YELLOW, ok ? 700 : 2000);
    ss_done(1);
}

static void snap_load(void)
{
    const char *why = NULL;
    char path[512];
    if (core_version < 0x04010008u) { ss_message("SNAPSHOTS NEED NES CORE 0.8", C_YELLOW, 1500); ss_done(0); return; }
    if (!game_crc) { ss_message("NO GAME TO LOAD INTO", C_YELLOW, 1500); ss_done(0); return; }
    if (game_card == 0 && (!save_sd_available() || !ss_tang_path(path))) {
        ss_message("TANG SD: NO FILE ACCESS FOR SNAPSHOTS", C_YELLOW, 1500); ss_done(0); return;
    }
    if (!ss_pause(1)) { ss_message("THE CORE DID NOT PAUSE", C_YELLOW, 1500); ss_done(0); return; }
    ss_message("LOADING STATE...", C_YELLOW, 0);
    uint8_t *data = NULL;
    size_t length = 0;
    int ok;
    if (game_card == 0) {
        const int r = core->sd_read(path, &data, &length, ss_size(), &running);
        ok = r == ZM_OK;
        if (!ok) why = r == ZM_NOT_FOUND ? "NO SNAPSHOT YET (F5 SAVES)" : "TANG SD: CANNOT READ THE SNAPSHOT";
    } else {
        ok = ss_evo_read(&why);
        if (ok) { data = rom_data; length = rom_size; rom_data = NULL; rom_discard(); }
    }
    if (ok) ok = ss_valid(data, length, &why);
    if (ok) ok = ss_restore(data, &why);
    if (data) core->free(data);
    core->log("nes: snapshot load %s", ok ? "ok" : why);
    if (ok && !menu_open) { ss_done(1); return; }    /* игра — сразу, без окна */
    ss_message(ok ? "STATE LOADED" : why, ok ? C_CYAN : C_YELLOW, ok ? 0 : 2000);
    ss_done(1);
}

/* F5/F6 (нажатие): 1 — клавиша съедена */
static int snap_key(unsigned code, unsigned up)
{
    if (code != HID_F5 && code != HID_F6) return 0;
    if (!up) { if (code == HID_F5) snap_save(); else snap_load(); }
    return 1;
}
