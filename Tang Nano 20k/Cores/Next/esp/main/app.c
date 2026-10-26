/* SPDX-License-Identifier: Apache-2.0 */
/*
 * NEXT.ELF — слой ESP ядра ZX Next на VDAC4 (docs/next-core.md в проекте VDAC4): приложение ESP
 * (ABI 1.4). Лежит рядом с ядром на microSD Tang — /CORES/NEXT.ELF; ставит его в кэш и
 * запускает прошивка ESP по команде CORE_APP плагина WC VDAC4 Cores. Сеансы повторяются: после
 * выхода (или сброса Evo) приложение снова ждёт START.
 *
 *  - START: ядро NEXT из кэша BL616 (core_load; есть на SD Tang /CORES/NEXT.BIN — сначала оттуда);
 *  - INPUT от плагина: очередь PS/2 Evo (набор 2) → события KEY ядра (ps2_hid), мышь Kempston →
 *    MOUSE, джойстик Kempston → JOY;
 *  - звук: кольцо отсчётов ядра (LINK 0x2000…) → pcm_write, на ЦАП ESP (задача звука);
 *  - UART ESP Next ↔ эмулятор ESP-AT (esp_at) поверх сети ESP: кольцо Next → ESP (LINK 0x1000…),
 *    ответы — UART_TX. Своя задача сети: DNS, соединение и отправка ждут сколько надо, а ввод и
 *    выход идут без них;
 *  - EXIT от плагина (F12) или тишина плагина дольше 1,5 с (Evo сбросилась) — назад в слот 0
 *    (core_back). Состояние 3 — только когда TS-Config снова на LINK; не вышло — 255 и текст.
 *
 * Команды Z80 (ответ — команда | 0x80):
 *   0x70 START  [имя ядра, 0] (пусто — NEXT)   → F0 {1 — принято}
 *   0x71 INPUT  [джойстик, кнопки мыши, dx, dy, байты очереди PS/2…] → F1 {1}
 *   0x72 STATUS                                 → F2 {состояние, VERSION ядра LE32, текст ошибки, 0}
 *   0x73 EXIT                                   → F3 {1}
 * Состояния: 0 — ждёт START, 1 — грузит ядро, 2 — ядро работает, 3 — назад в слоте 0, 255 — ошибка.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_at.h"
#include "ps2_hid.h"
#include "zm_app.h"

#define APP_VERSION "0.7"

/* регистры LINK ядра Next (next_link.sv) */
#define R_STATUS     0x08u
#define R_UART_FREE  0x0Cu
#define R_KEY        0x10u
#define R_MOUSE      0x14u
#define R_JOY        0x18u
#define R_KEYS_CLEAR 0x1Cu
#define R_AUDIO_WP   0x20u
#define R_AUDIO_DIV  0x24u
#define R_CLK_KHZ    0x28u
#define R_UART_WP    0x30u
#define R_UART_RP    0x34u
#define R_UART_TX    0x38u
#define R_UART_DIV   0x40u
#define W_UART       0x1000u
#define W_AUDIO      0x2000u
#define AUDIO_CELLS  2048u
#define UART_CELLS   1024u
#define UART_TX_FIFO 64u          /* очередь UART_TX ядра, байтов (UART_FREE[6:0] — свободно) */
#define UART_TX_BUSY 0x80u        /* UART_FREE[7] (ядро 0.3+): передатчик ещё шлёт байт */
#define UART_BAUD0   115200u      /* скорость ESP-AT после START (как у модуля после сброса) */

#define SILENCE_MS   1500u        /* плагин молчит дольше — Evo сбросилась: назад в слот 0 */
#define AUDIO_LAG_MS 30u          /* кольцо звука не читали дольше (оборот — 42,7 мс): догнать */
#define AUDIO_CHUNK  512u         /* отсчётов за порцию не больше (10,7 мс звука; по 16 за обмен LINK) */
#define AUDIO_SLEEP_MS 4u         /* сон между порциями: 192 отсчёта, оборот кольца — 42,7 мс */

static const zm_core_t *core;
static volatile int state, running, exit_req;
static volatile int worker_done = 1, audio_done = 1, net_done = 1;
static char core_name[16] = "NEXT";
static char error_text[64];
static uint32_t core_version, clk_khz = 28125;
static void *lock, *wake, *link_lock;

/* Сеанс ядра Next: пока session_live, задачи сети и звука трогают LINK ядра — под link_lock и с
   проверкой. Выход снимает сеанс под тем же замком до core_back: после возврата в слот 0 по этим
   адресам уже регистры TS-Config. session_id — номер сеанса (задача сети начинает свой). */
static volatile int session_live;
static volatile uint32_t session_id;

/* очередь от плагина (под lock) */
static uint8_t ps2_ring[256];
static uint8_t ps2_head, ps2_tail, ps2_lost;
/* Только задача ввода: готовые события остаются до успешной записи LINK.
   64 байта дают не более 128 событий плюс CLEAR при переполнении. */
static uint16_t key_pending[129];
static unsigned key_pending_n, key_pending_at;
static uint8_t joy, mouse_btn, mouse_base, last_x, last_y;
static int32_t mouse_dx, mouse_dy;
static uint32_t input_ms;

/* задача сети: вывод в UART Next (эмулятор ESP-AT → UART_TX), кольцо */
static uint8_t tx_ring[8192];
static uint32_t tx_head, tx_tail;
static uint32_t baud = UART_BAUD0, baud_pending;
static uint32_t uart_rp;
static esp_at_t at;

static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static uint32_t gray_dec(uint32_t g)
{
    for (uint32_t s = 1; s < 32; s <<= 1)
        g ^= g >> s;
    return g;
}

static int link_rd(uint32_t a, uint32_t *v) { return core->link_read(a, v, 1); }
static int link_wr(uint32_t a, uint32_t v) { return core->link_write(a, &v, 1); }

#include "audio_normalizer.h"

/* LINK из задач сети и звука: только пока сеанс жив */
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

static uint32_t uart_div(uint32_t b) { return (clk_khz * 1000u + b / 2) / b; }

static int32_t clamp127(int32_t v) { return v > 127 ? 127 : v < -127 ? -127 : v; }

static void fail(const char *text)
{
    strncpy(error_text, text, sizeof error_text - 1);
    error_text[sizeof error_text - 1] = 0;
    core->log("next: %s", text);
    state = 255;
}

/* ---- ESP-AT поверх сети ESP ---- */
static int at_open(void *ctx, int udp, const char *host, uint16_t port)
{
    (void)ctx;
    return core->sock_open(udp, host, port, 5000);
}
static int at_send(void *ctx, int s, const uint8_t *d, uint32_t n)
{
    (void)ctx;
    return core->sock_send(s, d, n) == ZM_OK ? 0 : -1;
}
static int at_recv(void *ctx, int s, uint8_t *d, uint32_t max)
{
    (void)ctx;
    return core->sock_recv(s, d, max, 0);
}
static void at_close(void *ctx, int s) { (void)ctx; core->sock_close(s); }
static int at_resolve(void *ctx, const char *host, char ip[16])
{
    (void)ctx;
    return core->net_resolve(host, ip) == ZM_OK ? 0 : -1;
}
static int at_wifi(void *ctx, char ssid[33], char ip[16], char mac[18])
{
    (void)ctx;
    return core->wifi_info(ssid, ip, mac) == ZM_OK;      /* Wi-Fi держит прошивка ESP */
}
static void at_baud(void *ctx, uint32_t b) { (void)ctx; baud_pending = b; }
static void at_out(void *ctx, const uint8_t *d, uint32_t n)
{
    (void)ctx;
    for (uint32_t i = 0; i < n && tx_head - tx_tail < sizeof tx_ring; i++)
        tx_ring[tx_head++ % sizeof tx_ring] = d[i];
}
static uint32_t at_room(void *ctx) { (void)ctx; return (uint32_t)sizeof tx_ring - (tx_head - tx_tail); }
static uint32_t at_ms(void *ctx) { (void)ctx; return core->millis(); }
static const esp_at_io_t at_io = {NULL, at_open, at_send, at_recv, at_close, at_resolve, at_wifi,
                                  at_baud, at_out, at_room, at_ms};

/* ---- UART Next ↔ ESP (задача сети; функции uart_* — под link_lock, при живом сеансе) ---- */

/* UART_FREE идёт из домена ядра: годится, только если два чтения подряд совпали */
static int uart_free(uint32_t *room, int *busy)
{
    for (int i = 0; i < 3; i++) {
        uint32_t a = 0, b = 0;
        if (link_rd(R_UART_FREE, &a) != ZM_OK || link_rd(R_UART_FREE, &b) != ZM_OK)
            return 0;
        if (a == b && (a & 0x7F) <= UART_TX_FIFO) {
            *room = a & 0x7F;
            *busy = (a & UART_TX_BUSY) != 0;
            return 1;
        }
    }
    return 0;
}

/* Байты Next → ESP: из кольца ядра в buf (до max), указатель чтения — ядру */
static uint32_t uart_take(uint8_t *buf, uint32_t max)
{
    uint32_t v = 0, words[16], got = 0;
    if (link_rd(R_UART_WP, &v) != ZM_OK)
        return 0;
    const uint32_t wp = gray_dec(v & 0x3FF);
    while (uart_rp != wp && got < max) {
        uint32_t n = (wp - uart_rp) & (UART_CELLS - 1);
        const uint32_t to_end = UART_CELLS - uart_rp;
        if (n > to_end) n = to_end;
        if (n > 16) n = 16;
        if (n > max - got) n = max - got;
        if (core->link_read(W_UART + 4 * uart_rp, words, n) != ZM_OK)
            break;
        for (uint32_t i = 0; i < n; i++)
            buf[got++] = (uint8_t)words[i];
        uart_rp = (uart_rp + n) & (UART_CELLS - 1);
    }
    if (got)
        link_wr(R_UART_RP, uart_rp);
    return got;
}

/* ESP → UART Next: пакет — до трёх байтов и не больше свободного (UART_FREE — в байтах; пакет,
   который не влезает весь, ядро выбрасывает). Очередь ESP двигается после удачной записи. */
static void uart_give(void)
{
    uint32_t room = 0;
    int busy = 0;
    if (tx_head == tx_tail || !uart_free(&room, &busy))
        return;
    while (room > 0 && tx_head != tx_tail) {
        uint32_t k = tx_head - tx_tail;
        if (k > 3) k = 3;
        if (k > room) k = room;
        uint32_t w = 0;
        for (uint32_t i = 0; i < k; i++)
            w |= (uint32_t)tx_ring[(tx_tail + i) % sizeof tx_ring] << (8 * i);
        if (link_wr(R_UART_TX, w | (k << 24)) != ZM_OK)
            return;
        tx_tail += k;
        room -= k;
    }
}

/* Новая скорость (AT+UART_CUR) — когда старое ушло из линии целиком: очередь ESP и ядра пуста и
   передатчик стоит (UART_FREE[7], ядро 0.3+) */
static int uart_baud_ready(void)
{
    uint32_t room = 0;
    int busy = 0;
    return baud_pending && tx_head == tx_tail && uart_free(&room, &busy) && room == UART_TX_FIFO && !busy;
}

static void uart_step(void)
{
    static uint8_t rx[256];
    core->mutex_lock(link_lock);
    const uint32_t n = session_live ? uart_take(rx, sizeof rx) : 0;
    core->mutex_unlock(link_lock);
    if (n)
        esp_at_input(&at, rx, n);                   /* сеть — без замка: ждать её можно долго */
    esp_at_poll(&at);
    core->mutex_lock(link_lock);
    int change = 0;
    if (session_live) {
        uart_give();
        change = uart_baud_ready();
    }
    core->mutex_unlock(link_lock);
    if (!change)
        return;
    /* у ядра 0.2 бита «передаёт» нет: стоп-бит последнего байта — ещё два знака прежней скорости */
    core->delay(1 + 20000u / baud);
    core->mutex_lock(link_lock);
    if (session_live) {
        baud = baud_pending;
        baud_pending = 0;
        link_wr(R_UART_DIV, uart_div(baud));
        core->log("next: UART %u baud", (unsigned)baud);
    }
    core->mutex_unlock(link_lock);
}

/* Начало сеанса сети: UART — как у модуля после сброса (115 200, очереди пусты), ESP-AT заново */
static int net_session_start(uint32_t id)
{
    int ok = 0;
    core->mutex_lock(link_lock);
    if (session_live && session_id == id) {
        tx_head = tx_tail = 0;
        baud = UART_BAUD0;
        baud_pending = 0;
        link_wr(R_UART_DIV, uart_div(baud));
        uint32_t v = 0;
        uart_rp = link_rd(R_UART_WP, &v) == ZM_OK ? gray_dec(v & 0x3FF) : 0;
        ok = 1;
    }
    core->mutex_unlock(link_lock);
    if (ok)
        esp_at_init(&at, &at_io);
    return ok;
}

static void net_task(void *arg)
{
    (void)arg;
    uint32_t mine = 0;                              /* сеанс, который ведёт задача; 0 — никакого */
    while (running) {
        const uint32_t id = session_id;
        if (mine && (!session_live || id != mine)) {
            esp_at_close_all(&at);                  /* сеанс кончился: сокеты закрыть */
            mine = 0;
            continue;
        }
        if (!mine) {
            if (session_live && id != 0 && net_session_start(id))
                mine = id;
            else
                core->delay(20);
            continue;
        }
        uart_step();
        core->delay(1);
    }
    if (mine)
        esp_at_close_all(&at);
    net_done = 1;
}

/* ---- ввод (задача ядра) ---- */
static void input_step(ps2_hid_t *kb, uint8_t *joy_sent, uint8_t *btn_sent)
{
    uint8_t bytes[64];
    unsigned n = 0;
    core->mutex_lock(lock);
    const int lost = ps2_lost;
    ps2_lost = 0;
    while ((lost || key_pending_at == key_pending_n) && ps2_tail != ps2_head && n < sizeof bytes)
        bytes[n++] = ps2_ring[ps2_tail++];
    /* мышь: не больше ±127 за запись, остаток — следующей */
    const int32_t dx = clamp127(mouse_dx), dy = clamp127(mouse_dy);
    mouse_dx -= dx;
    mouse_dy -= dy;
    const uint8_t j = joy, b = mouse_btn;
    core->mutex_unlock(lock);
    if (lost || key_pending_at == key_pending_n) {
        key_pending_at = key_pending_n = 0;
        if (lost) {
            ps2_hid_init(kb);
            key_pending[key_pending_n++] = PS2_HID_CLEAR;
        }
        for (unsigned i = 0; i < n; i++) {
            uint16_t ev[2];
            const int m = ps2_hid_byte(kb, bytes[i], ev);
            for (int k = 0; k < m; k++)
                key_pending[key_pending_n++] = ev[k];
        }
    }
    while (key_pending_at < key_pending_n) {
        const uint16_t ev = key_pending[key_pending_at];
        const int clear = ev == PS2_HID_CLEAR;
        if (link_wr(clear ? R_KEYS_CLEAR : R_KEY, clear ? 0 : ev) != ZM_OK)
            break;
        key_pending_at++;
        if (clear)
            *btn_sent = *joy_sent = 0;
    }
    if (dx || dy || b != *btn_sent) {
        link_wr(R_MOUSE, (uint32_t)(uint8_t)dx | (uint32_t)(uint8_t)dy << 8 | (uint32_t)(b & 7) << 16);
        *btn_sent = b;
    }
    if (j != *joy_sent) {
        link_wr(R_JOY, j);
        *joy_sent = j;
    }
}

/* ---- звук: кольцо отсчётов (LINK 0x2000…) → pcm_write ---- */
typedef struct {
    uint32_t rp, last_ms;
    uint32_t lags;                /* сколько раз догоняли (кольцо давно не читали или почти полно) */
} audio_pos_t;

/* Порция: указатель кольца — одним чтением, затем все новые отсчёты (не больше AUDIO_CHUNK) по 16
   за обмен LINK → pcm_write. Вернёт, сколько отсчётов взято (0 — нечего). Обмен LINK — опрос SPI в
   цикле: читать указатель перед каждыми 16 отсчётами (0.3) значило два обмена на 0,33 мс звука, и с
   прочей работой ядро 0 оставалось без простоя дольше сторожевого таймера (0.3 на плате) */
static uint32_t audio_step(audio_pos_t *a)
{
    static float frames[16 * 2];
    uint32_t v = 0, words[16];
    if (live_read(R_AUDIO_WP, &v, 1) != ZM_OK)
        return 0;
    const uint32_t now = core->millis();
    const uint32_t wp = gray_dec(v & 0x7FF);
    uint32_t have = (wp - a->rp) & (AUDIO_CELLS - 1);
    /* отстали (ESP была занята): кольцо давно не читали — разность указателей уже не говорит,
       сколько оборотов пропущено, — или оно почти полно: догнать */
    if (now - a->last_ms > AUDIO_LAG_MS || have > AUDIO_CELLS - 256) {
        a->rp = (wp - 256) & (AUDIO_CELLS - 1);
        have = 256;
        a->lags++;
    }
    a->last_ms = now;
    if (have > AUDIO_CHUNK)
        have = AUDIO_CHUNK;
    uint32_t taken = 0;
    while (taken < have && running && session_live) {
        uint32_t n = have - taken > 16 ? 16 : have - taken;
        if (n > AUDIO_CELLS - a->rp)
            n = AUDIO_CELLS - a->rp;
        if (live_read(W_AUDIO + 4 * a->rp, words, n) != ZM_OK)
            break;
        for (uint32_t i = 0; i < n; i++) {
            frames[2 * i] = (float)(uint16_t)(words[i] >> 16) * (1.0f / 65536.0f);
            frames[2 * i + 1] = (float)(uint16_t)words[i] * (1.0f / 65536.0f);
        }
        next_audio_normalize(frames, n);
        /* pcm_write ждёт места и может принять не всё: дописать остаток */
        uint32_t done = 0;
        while (done < n && running && session_live) {
            const int w = core->pcm_write(frames + 2 * done, n - done);
            if (w <= 0)
                break;
            done += (uint32_t)w;
        }
        a->rp = (a->rp + n) & (AUDIO_CELLS - 1);
        taken += n;
    }
    return taken;
}

/* Сеанс звука — пока работает ядро этого сеанса */
static void audio_session(void)
{
    const uint32_t id = session_id;
    uint32_t div = 0, v = 0;
    zm_pcm_format_t fmt = {0, 2};
    if (live_read(R_AUDIO_DIV, &div, 1) == ZM_OK && div >= 100) {
        fmt.rate = clk_khz * 1000u / div;
        if (core->pcm_open(&fmt) != ZM_OK)
            fmt.rate = 0;
    }
    if (fmt.rate == 0) {
        while (running && session_live && session_id == id)
            core->delay(50);
        return;
    }
    next_audio_normalizer_reset((float)fmt.rate);
    core->log("next: audio %u Hz", (unsigned)fmt.rate);
    audio_pos_t a = {0, core->millis(), 0};
    if (live_read(R_AUDIO_WP, &v, 1) == ZM_OK)
        a.rp = gray_dec(v & 0x7FF);
    /* Порция, затем сон AUDIO_SLEEP_MS — всегда. Задача звука — на ядре 0 с приоритетом 4: в 0.2
       она спала только при пустом кольце, в 0.3 — 1 мс после 32 шагов, и оба раза сторожевой
       таймер задач перезапускал ESP (ядро 0 без простоя 5 с) */
    while (running && session_live && session_id == id) {
        audio_step(&a);
        core->delay(AUDIO_SLEEP_MS);
    }
    core->pcm_close();
    if (a.lags)
        core->log("next: audio caught up %u times", (unsigned)a.lags);
}

static void audio_task(void *arg)
{
    (void)arg;
    while (running) {
        while (running && !session_live)
            core->delay(20);
        if (running)
            audio_session();
    }
    audio_done = 1;
}

#include "next_cfg.h"

/* ---- ядро ---- */

/* Загрузка ядра; 1 — работает */
static int load_core(void)
{
    key_pending_at = key_pending_n = 0;
    if (!next_cfg_prepare()) return 0;
    core->log("next: loading core %s", core_name);
    const int r = core->core_load(core_name, &core_version);
    if (r == ZM_NOT_FOUND) {
        fail("core not found in the BL616 cache nor on the Tang SD (\\CORES)");
        return 0;
    }
    if (r != ZM_OK) {
        fail("core did not start (Tang is back in TS-Config)");
        return 0;
    }
    if ((core_version >> 24) != 0x03) {
        fail("loaded core is not ZX Next (VERSION personality)");
        core->core_back(10000);
        return 0;
    }
    if (!next_cfg_apply()) {
        core->core_back(10000);
        fail("Cannot apply core .cfg to Next");
        return 0;
    }
    uint32_t v = 0;
    if (link_rd(R_CLK_KHZ, &v) == ZM_OK && v > 1000 && v < 200000)
        clk_khz = v;
    link_wr(R_KEYS_CLEAR, 0);
    core->mutex_lock(lock);
    ps2_head = ps2_tail = 0;                        /* ввод — с нуля: очередь, мышь от первого пакета */
    ps2_lost = 0;
    mouse_dx = mouse_dy = 0;
    mouse_base = 1;
    input_ms = core->millis();
    core->mutex_unlock(lock);
    core->mutex_lock(link_lock);
    session_id = session_id + 1 == 0 ? 1 : session_id + 1;
    session_live = 1;
    core->mutex_unlock(link_lock);
    state = 2;
    core->log("next: core runs, VERSION %08X", (unsigned)core_version);
    return 1;
}

/* Назад в слот 0: сеть и звук сначала отпускают LINK ядра; «вернулись» — по ответу core_back */
static void leave_core(void)
{
    end_session();
    const int captured=next_cfg_capture();
    if (core->core_back(10000) != ZM_OK) {
        fail("could not return the Tang to TS-Config (slot 0)"); return;
    }
    next_cfg_returned=1;
    next_cfg_capture_failed=!captured;
    next_cfg_finish();
}

/* Сеансы: START → ядро → ввод → выход (F12 или тишина плагина) → снова ждать START */
static void worker(void *arg)
{
    (void)arg;
    ps2_hid_t kb;
    uint8_t joy_sent = 0, btn_sent = 0;
    while (running) {
        if (state==5 && next_cfg_returned) { next_cfg_finish(); continue; }
        if (state != 1) {
            core->signal_wait(wake, 50);
            continue;
        }
        if (!load_core())
            continue;
        ps2_hid_init(&kb);
        joy_sent = btn_sent = 0;                    /* после KEYS_CLEAR в ядре */
        while (running && state == 2) {
            input_step(&kb, &joy_sent, &btn_sent);
            core->mutex_lock(lock);
            const uint32_t quiet = core->millis() - input_ms;
            core->mutex_unlock(lock);
            if (exit_req || quiet > SILENCE_MS) {
                core->log(exit_req ? "next: exit — back to slot 0" : "next: the plugin is silent (Evo reset?) — back to slot 0");
                leave_core();
                break;
            }
            core->signal_wait(wake, 5);
        }
    }
    end_session();                                  /* выгрузка: ядро вернёт прошивка ESP */
    worker_done = 1;
}

static int command(uint8_t cmd, const uint8_t *p, uint16_t n)
{
    if (cmd == 0x7e) return next_cfg_retry(cmd,p,n);
    uint8_t ok = 0;
    if (cmd == 0x70) {
        /* новый сеанс — и после выхода или ошибки прежнего (приложение остаётся загруженным,
           например после сброса Evo) */
        const int idle = state == 0 || state == 3 || state == 255;
        if (idle && n <= 16 && (n == 0 || p[n - 1] == 0)) {
            if (n > 1)
                memcpy(core_name, p, n);
            error_text[0] = 0;
            exit_req = 0;
            ok = 1;
            state = 1;
        }
        core->reply(0xF0, &ok, 1);
        if (ok)
            core->signal_give(wake);
    } else if (cmd == 0x71) {
        /* [джойстик Kempston (#1F, биты 0…4), кнопки мыши (#FADF: 0 левая, 1 правая, 2 средняя,
           нажатая — 0), X (#FBDF), Y (#FFDF) — счётчики по модулю 256, байты PS/2…]: приращение
           мыши — здесь, по разности с прошлым пакетом (потерянный пакет движения не теряет);
           первый пакет сеанса и пакет после паузы только задают отсчёт */
        if (n >= 4) {
            core->mutex_lock(lock);
            const uint32_t now = core->millis();
            const int fresh = mouse_base || now - input_ms > 500;
            mouse_base = 0;
            input_ms = now;
            joy = p[0] & 31;
            mouse_btn = (uint8_t)(~p[1] & 7);
            if (!fresh) {
                /* Удваиваем после знакового разбора кольцевого счётчика.
                   Остаток хранится в int32_t и отправляется порциями по 127. */
                mouse_dx += 2 * (int32_t)(int8_t)(uint8_t)(p[2] - last_x);
                mouse_dy += 2 * (int32_t)(int8_t)(uint8_t)(p[3] - last_y);
            }
            last_x = p[2];
            last_y = p[3];
            for (unsigned i = 4; i < n; i++) {
                if ((uint8_t)(ps2_head + 1) == ps2_tail) {
                    /* переполнение: очередь выбросить целиком — задача ядра отпустит клавиши и
                       начнёт разбор заново (иначе старый код нажатия без отпускания) */
                    ps2_head = ps2_tail;
                    ps2_lost = 1;
                    break;
                }
                ps2_ring[ps2_head++] = p[i];
            }
            core->mutex_unlock(lock);
            ok = 1;
        }
        core->reply(0xF1, &ok, 1);
        if (ok)
            core->signal_give(wake);
    } else if (cmd == 0x72) {
        uint8_t st[5 + sizeof error_text + 1];
        st[0] = (uint8_t)state;
        wr32(st + 1, core_version);
        const size_t len = strlen(error_text);
        memcpy(st + 5, error_text, len + 1);
        core->reply(0xF2, st, (uint16_t)(5 + len + 1));
    } else if (cmd == 0x73) {
        exit_req = 1;
        ok = 1;
        core->reply(0xF3, &ok, 1);
        core->signal_give(wake);
    } else {
        return ZM_ERROR;
    }
    return ZM_OK;
}

static int start(const zm_core_t *c)
{
    core = c;
    if (c->abi_major != 1 || c->abi_minor < 9 ||
        c->size < offsetof(zm_core_t, sd_write) + sizeof c->sd_write ||
        !c->core_load || !c->sd_read || !c->sd_write)
        return ZM_ERROR;                               /* общий файловый транспорт */
    lock = c->mutex_create();
    link_lock = c->mutex_create();
    wake = c->signal_create();
    if (!lock || !link_lock || !wake)
        return ZM_NO_MEMORY;
    state = 0;
    exit_req = 0;
    error_text[0] = 0;
    session_live = 0;
    running = 1;
    worker_done = 0;
    /* После F12 эта задача вызывает полный FAT-путь записи .cfg.
       4096 байт дали stack overflow на плате; только кадр tang_files::write
       в ESP 0.1.99 занимает 2176 байт, сверх него идут VFS/FAT/SPI. */
    int r = c->task_start(worker, NULL, 8192, 3, 1);
    if (r != ZM_OK) {
        worker_done = 1;
        running = 0;
        return r;
    }
    net_done = 0;
    r = c->task_start(net_task, NULL, 8192, 2, 1);   /* ниже задачи ядра: ввод и выход — первыми */
    if (r != ZM_OK) {
        net_done = 1;
        running = 0;
        return r;
    }
    audio_done = 0;
    r = c->task_start(audio_task, NULL, 4096, 4, 0);
    if (r != ZM_OK) {
        audio_done = 1;
        running = 0;
        return r;
    }
    c->log("next: NEXT.ELF " APP_VERSION " ready");
    return ZM_OK;
}

static void stop(void)
{
    running = 0;
    if (wake)
        core->signal_give(wake);
    /* дать задачам кончиться самим (сеть — соединение до 5 с, DNS), прежде чем ядро ESP их снимет;
       загрузка ядра идёт в задаче прошивки и выгрузку не держит */
    for (unsigned i = 0; i < 2000 && !(worker_done && audio_done && net_done); i++)
        core->delay(10);
}

#ifndef NEXT_APP_HOST_TEST
int main(int argc, char **argv)
{
    static const zm_app_t app = {sizeof(zm_app_t), ZM_APP_ABI_MAJOR, ZM_APP_ABI_MINOR, "next", APP_VERSION,
                                 0x70, 0x7e, start, stop, command, NULL, NULL, NULL};
    if (argc != 3)
        return 0;
    *(const zm_app_t **)argv[2] = &app;
    return ZM_APP_ABI_MAJOR;
}
#endif
