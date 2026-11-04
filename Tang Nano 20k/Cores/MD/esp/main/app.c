/* SPDX-License-Identifier: GPL-3.0-or-later
 * MD.ELF 1.4 (04.11.2026): слой ESP ядра Mega Drive VDAC4 (Cores/MD, личность 06). Всё, что можно,
 * делает ESP: ROM с карты Tang (меню F1), его разбор и загрузка в SDRAM ядра, регион и особенности
 * игр, пады из клавиатуры Evo и Kempston, звук YM2612 и SN76489 (md_audio.h, md_sound.cpp).
 * Команды плагина WC VDAC4 Cores — те же, что у NES.ELF: START 0x70 (без образа — ядро с меню F1),
 * INPUT 0x71, STATUS 0x72, EXIT 0x73, приём образа 0x77…0x79, меню карты Evo 0x6E/0x6F.
 *
 * Клавиатура Evo -> пад 1 (6 кнопок), по умолчанию: стрелки — крестовина, Z X C — A B C, A S D — X Y Z,
 * Enter — Start, Space — Mode; свои клавиши — вкладка KEYS меню F1 (md_keys.h, /CORES/md.cfg). Kempston (#1F, как у Sega-адаптера Evo): 0 вправо, 1 влево, 2 вниз, 3 вверх,
 * 4 огонь — B, 5 — C, 6 — A, 7 — Start. F1 — меню, F4 — сброс игры, F5/F6 — сохранить и загрузить снимок
 * (ядро 0.6, md_snapshot.h), F7 — 4:3/16:9 (ядро 0.2), F8 — размер картинки (и в игре, с 1.3), F12 — назад в WC (плагин).
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "md_rom.h"
#include "ps2_hid.h"
#include "zm_app.h"

#define APP_VERSION "1.4"
#define MD_PERSONALITY 0x06u
#define SILENCE_MS 1500u          /* плагин молчит дольше — Evo сбросилась: назад в слот 0 */

static const zm_core_t *core;
static volatile int state, running, exit_req;
static volatile int worker_done = 1, audio_done = 1;
static char core_name[16] = "MD";
static char error_text[64];
static uint32_t core_version;
static void *lock, *wake, *link_lock;
static volatile int session_live;
static volatile uint32_t session_id;

/* очередь от плагина (под lock) */
static uint8_t ps2_ring[256];
static uint8_t ps2_head, ps2_tail, ps2_lost;
static uint8_t joy;
static uint32_t input_ms, alive_ms;
static volatile int evo_rom_wait;
static int menu_start;
static volatile int menu_loading;

static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t gray_dec(uint32_t g) { for (uint32_t s = 1; s < 32; s <<= 1) g ^= g >> s; return g; }
static int link_rd(uint32_t a, uint32_t *v) { return core->link_read(a, v, 1); }
static int link_wr(uint32_t a, uint32_t v) { return core->link_write(a, &v, 1); }
static int live_read(uint32_t a, uint32_t *words, uint32_t n)
{
    core->mutex_lock(link_lock);
    const int r = session_live ? core->link_read(a, words, n) : ZM_ERROR;
    core->mutex_unlock(link_lock);
    return r;
}
static void end_session(void)
{
    core->mutex_lock(link_lock);
    session_live = 0;
    core->mutex_unlock(link_lock);
}
static void fail(const char *text)
{
    strncpy(error_text, text, sizeof error_text - 1);
    error_text[sizeof error_text - 1] = 0;
    core->log("md: %s", text);
    if (!menu_loading) state = 255;
}

/* --- то, что ждут файлы меню и приёма из NES.ELF --- */
#define NES_ROM_MAX (0x5D0000u + 512u)  /* SMD — с заголовком 512 байт */
#define rom_check md_file_check
#define R_KEYS_CLEAR 0x148u             /* пады — отпустить всё */
static volatile int snap_rx;
static uint32_t game_crc;
static int menu_key(uint16_t ev);

#include "md_saves.h"
#include "rom_source.h"
#include "rom_browser.h"
#include "md_loader.h"
#include "md_settings.h"
#include "md_keys.h"
#include "game_menu.h"
#include "md_audio.h"
#include "md_snapshot.h"

/* --- ввод --- */
static uint8_t keys[128];
static uint32_t pad_of(uint8_t j)
{
    /* {Z,Y,X,Mode,Start,C,B,A,Вверх,Вниз,Влево,Вправо}: клавиатура — по вкладке KEYS (md_keys.h), Kempston — всегда */
    uint32_t p = kj_pad(keys);
    if (j & 1) p |= 1u << 0;                       /* → */
    if (j & 2) p |= 1u << 1;                       /* ← */
    if (j & 4) p |= 1u << 2;                       /* ↓ */
    if (j & 8) p |= 1u << 3;                       /* ↑ */
    if (j & 64) p |= 1u << 4;                      /* A Sega — A */
    if (j & 16) p |= 1u << 5;                      /* огонь (B Sega) — B */
    if (j & 32) p |= 1u << 6;                      /* C */
    if (j & 128) p |= 1u << 7;                     /* Start */
    return p;
}

static void input_step(ps2_hid_t *kb, uint32_t *pad_sent)
{
    uint8_t bytes[64];
    unsigned n = 0;
    core->mutex_lock(lock);
    const int lost = ps2_lost;
    ps2_lost = 0;
    while (ps2_tail != ps2_head && n < sizeof bytes) bytes[n++] = ps2_ring[ps2_tail++];
    const uint8_t j = joy;
    core->mutex_unlock(lock);
    if (lost) { ps2_hid_init(kb); memset(keys, 0, sizeof keys); }
    for (unsigned i = 0; i < n; i++) {
        uint16_t ev[2];
        const int m = ps2_hid_byte(kb, bytes[i], ev);
        for (int k = 0; k < m; k++) {
            if (ev[k] == PS2_HID_CLEAR) { memset(keys, 0, sizeof keys); continue; }
            if (menu_key(ev[k])) { memset(keys, 0, sizeof keys); continue; }
            const unsigned code = ev[k] & 0x7f, up = (ev[k] & 0x80) != 0;
            if (code == 61 && !up && !menu_open) { md_reset(); continue; }   /* F4 */
            keys[code] = !up;
        }
    }
    keys_blink_step();
    const uint32_t p = menu_open ? 0 : pad_of(j);
    if (p != *pad_sent && link_wr(R_PADS, p) == ZM_OK) *pad_sent = p;
}

/* --- ядро --- */
static int load_core(void)
{
    menu_open = menu_force = 0;
    if (!menu_start && rom_from_sd && !rom_read_sd()) return 0;
    if (!running || exit_req) return 0;
    if (!menu_start && (!rom_ready || !rom_data)) { fail("Choose a Mega Drive ROM first"); return 0; }
    if (!md_cfg_prepare()) return 0;
    kj_load(&md_cfg);
    slot0_size_read();
    core->log("md: loading core %s", core_name);
    const int r = core->core_load(core_name, &core_version);
    if (r == ZM_NOT_FOUND) { fail("core not found in the BL616 cache nor on the Tang SD (\\CORES)"); return 0; }
    if (r != ZM_OK) { fail("core did not start (Tang is back in TS-Config)"); return 0; }
    if ((core_version >> 24) != MD_PERSONALITY) {
        fail("loaded core is not Mega Drive (VERSION personality)");
        core->core_back(10000);
        return 0;
    }
    uint32_t v = 0;
    if (link_rd(R_BR_CMD, &v) == ZM_OK) br_seq = (uint16_t)v;
    md_cfg_apply();
    if (!menu_start) {
        if (!load_cartridge(rom_data, rom_size)) {
            if (!error_text[0]) fail("ROM transfer to SDRAM failed");
            core->core_back(10000);
            return 0;
        }
        snap_game(rom_data, rom_size);
        rom_discard();
    }
    link_wr(R_PADS, 0);
    core->mutex_lock(lock);
    ps2_head = ps2_tail = 0;
    ps2_lost = 0;
    input_ms = alive_ms = core->millis();
    core->mutex_unlock(lock);
    core->mutex_lock(link_lock);
    session_id = session_id + 1 == 0 ? 1 : session_id + 1;
    session_live = 1;
    core->mutex_unlock(link_lock);
    state = 2;
    core->log("md: core runs, VERSION %08X%s", (unsigned)core_version, menu_start ? ", game from the F1 menu" : "");
    if (menu_start) menu_show_forced();
    return 1;
}

static void leave_core(void)
{
    menu_open = menu_force = 0;
    menu_items_free();
    rom_from_sd = 0;
    md_cfg_save();
    end_session();
    if (core->core_back(10000) != ZM_OK) { fail("could not return the Tang to TS-Config (slot 0)"); return; }
    state = 3;
}

static void worker(void *arg)
{
    (void)arg;
    ps2_hid_t kb;
    uint32_t pad_sent = 0;
    while (running) {
        if (state == 4 && browser_phase == 1) { browser_read(); continue; }
        if (state != 1) { core->signal_wait(wake, 50); continue; }
        if (!load_core()) continue;
        ps2_hid_init(&kb);
        memset(keys, 0, sizeof keys);
        pad_sent = 0;
        while (running && state == 2) {
            input_step(&kb, &pad_sent);
            core->mutex_lock(lock);
            const uint32_t now = core->millis();
            const int32_t quiet = (int32_t)(now - alive_ms);
            core->mutex_unlock(lock);
            if (exit_req || quiet > (int32_t)SILENCE_MS) {
                core->log(exit_req ? "md: exit — back to slot 0" : "md: the plugin is silent (Evo reset?) — back to slot 0");
                leave_core();
                break;
            }
            core->signal_wait(wake, 5);
        }
    }
    end_session();
    worker_done = 1;
}

static int command(uint8_t cmd, const uint8_t *p, uint16_t n)
{
    uint8_t ok = 0;
    core->mutex_lock(lock);
    alive_ms = core->millis();                       /* любая команда — плагин жив */
    core->mutex_unlock(lock);
    if (cmd == 0x6E || cmd == 0x6F) return evo_command(cmd, p, n);
    if (cmd >= 0x74 && cmd <= 0x76) return browser_command(cmd, p, n);
    if (cmd >= 0x77 && cmd <= 0x79) {
        const int r = rom_receive(cmd, p, n);
        if (cmd == 0x77 && rom_data && !rom_received && !rom_ready) rom_from_sd = 0;
        return r;
    }
    if (cmd == 0x7a) {
        const uint8_t caps[] = {1, (uint8_t)(5 | (sd_available() ? 2 : 0))};
        return core->reply(0xfa, caps, sizeof caps);
    }
    if (cmd == 0x70) {
        const int idle = state == 0 || state == 3 || state == 255;
        if (idle && n <= 16 && (n == 0 || p[n - 1] == 0)) {
            if (n > 1) memcpy(core_name, p, n);
            menu_start = !(rom_ready || rom_from_sd);
            if (menu_start) rom_discard();
            error_text[0] = 0;
            exit_req = 0;
            ok = 1;
            state = 1;
        }
        core->reply(0xF0, &ok, 1);
        if (ok) core->signal_give(wake);
    } else if (cmd == 0x71) {
        /* [Kempston #1F, кнопки мыши, X, Y, байты PS/2…] — мышь Mega Drive не нужна */
        if (n >= 4) {
            core->mutex_lock(lock);
            input_ms = core->millis();
            joy = p[0];
            for (unsigned i = 4; i < n; i++) {
                if ((uint8_t)(ps2_head + 1) == ps2_tail) { ps2_head = ps2_tail; ps2_lost = 1; break; }
                ps2_ring[ps2_head++] = p[i];
            }
            core->mutex_unlock(lock);
            ok = 1;
        }
        evo_reply_input(ok);
        if (ok) core->signal_give(wake);
    } else if (cmd == 0x72) {
        uint8_t st[5 + sizeof error_text + 1];
        st[0] = (uint8_t)state;
        wr32(st + 1, core_version);
        const size_t len = strlen(error_text);
        memcpy(st + 5, error_text, len + 1);
        core->reply(0xF2, st, (uint16_t)(5 + len + 1));
    } else if (cmd == 0x73) {
        exit_req = 1;
        if (state == 0 || state == 3 || state == 255) state = 3;
        ok = 1;
        core->reply(0xF3, &ok, 1);
        core->signal_give(wake);
    } else {
        return ZM_ERROR;
    }
    return ZM_OK;
}

/* память ymfm (каналы, операторы): быстрая, если есть; сколько куда легло — в журнал (синтез в PSRAM медленнее) */
static unsigned snd_int_bytes, snd_psram_bytes;
static void *snd_alloc(size_t n)
{
    void *p = core->alloc(n, ZM_MEM_INTERNAL);
    if (p) { snd_int_bytes += n; return p; }
    p = core->alloc(n, ZM_MEM_PSRAM);
    if (p) snd_psram_bytes += n;
    return p;
}
static void snd_release(void *p) { core->free(p); }

static int start(const zm_core_t *c)
{
    core = c;
    if (c->abi_major != 1 || c->abi_minor < 9 ||
        c->size < offsetof(zm_core_t, sd_write) + sizeof c->sd_write || !c->core_load || !c->sd_read)
        return ZM_ERROR;
    lock = c->mutex_create();
    link_lock = c->mutex_create();
    wake = c->signal_create();
    snd_go = c->signal_create();
    snd_done = c->signal_create();
    snd_mem = c->alloc(md_snd_size(), ZM_MEM_INTERNAL);
    const int snd_mem_int = snd_mem != NULL;
    if (!snd_mem) snd_mem = c->alloc(md_snd_size(), ZM_MEM_PSRAM);
    if (!lock || !link_lock || !wake || !snd_go || !snd_done || !snd_mem) return ZM_NO_MEMORY;
    md_snd_set_alloc(snd_alloc, snd_release);
    snd_int_bytes = snd_psram_bytes = 0;
    md_snd_init(snd_mem);
    c->log("md: synth memory: engine %u B %s, ymfm %u B internal + %u B PSRAM", (unsigned)md_snd_size(),
           snd_mem_int ? "internal" : "PSRAM", snd_int_bytes, snd_psram_bytes);
    rom_data = NULL; rom_size = rom_received = 0; rom_ready = 0; rom_from_sd = 0;
    browser_phase = 0; browser_count = browser_page = 0; browser_path[0] = '/'; browser_path[1] = 0;
    state = 0;
    exit_req = 0;
    error_text[0] = 0;
    session_live = 0;
    running = 1;
    worker_done = 0;
    int r = c->task_start(worker, NULL, 8192, 3, 1);
    if (r != ZM_OK) { worker_done = 1; running = 0; return r; }
    audio_done = 0;
    r = c->task_start(audio_task, NULL, 6144, 4, 0);
    if (r != ZM_OK) { audio_done = 1; running = 0; return r; }
    snd_helper_done = 0;
    r = c->task_start(snd_helper, NULL, 4096, 1, 1);  /* половина YM — на ядре 1 рядом с loopTask */
    if (r != ZM_OK) { snd_helper_done = 1; running = 0; return r; }
    c->log("md: MD.ELF " APP_VERSION " ready");
    return ZM_OK;
}

static void stop(void)
{
    running = 0;
    if (wake) core->signal_give(wake);
    for (unsigned i = 0; i < 2000 && !(worker_done && audio_done && snd_helper_done); i++) core->delay(10);
    if (worker_done) { rom_discard(); menu_items_free(); }
}

#ifndef MD_APP_HOST_TEST
int main(int argc, char **argv)
{
    static const zm_app_t app = {sizeof(zm_app_t), ZM_APP_ABI_MAJOR, ZM_APP_ABI_MINOR, "md", APP_VERSION,
                                 0x6E, 0x7e, start, stop, command, NULL, NULL, NULL};
    if (argc != 3) return 0;
    *(const zm_app_t **)argv[2] = &app;
    return ZM_APP_ABI_MAJOR;
}
#endif
