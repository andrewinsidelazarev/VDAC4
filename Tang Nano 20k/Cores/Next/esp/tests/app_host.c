/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Проверка app.c NEXT.ELF на ПК: настоящий код приложения с поддельным ядром ESP (zm_core_t) и
 * моделью регистров LINK ядра Next (next_link.sv). Что проверяется (разбор Codex 21.10.2026):
 *  - UART ESP → Next: пакет не больше свободного места очереди (0/1/2/64 байта), очередь ESP
 *    двигается только после удачной записи, «рваное» UART_FREE (два чтения не совпали) — пропуск;
 *  - смена скорости — только при пустой очереди и стоящем передатчике (UART_FREE[7]); START
 *    возвращает 115 200;
 *  - выход: состояние 3 — только после удачного core_back, иначе 255; сеанс снят до core_back;
 *  - переполнение очереди PS/2: очередь выброшена, KEYS_CLEAR, разбор с начала (старый F0 не
 *    превращает следующее нажатие в отпускание);
 *  - мышь: первый пакет сеанса — только отсчёт; приращение больше ±127 — по частям;
 *  - звук: pcm_write принял не всё — остаток дописан; кольцо давно не читали — догнать;
 *    кольцо не пустеет никогда — сеанс звука спит после каждой порции, указатель кольца — одним
 *    чтением на порцию (0.2 и 0.3: сторожевой таймер задач ESP).
 * Ожидается: «ТЕСТ ПРОЙДЕН».
 */
#define NEXT_APP_HOST_TEST 1
#include "../main/app.c"

#include <stdarg.h>
#include <stdio.h>

static int bad, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { bad++; printf("ОШИБКА %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- модель ядра Next и ядра ESP ---- */
#include <stdlib.h>
static uint32_t hw_version=0x03010011, hw_choice;
static int tsconfig=1, strict_files, cfg_exists, cfg_write_error, cfg_read_error, cfg_writes;
static char cfg_disk[2049];
static int cfg_read(const char*p,uint8_t**data,size_t*n,size_t max,const volatile int*active){
    (void)active;CHECK(!strict_files || tsconfig,"read .cfg only in TS-Config");
    CHECK(!strcmp(p,"/CORES/next.cfg") || !strcmp(p,"/CORES/next.cfg.bak"),"sibling config path");
    *data=NULL;*n=0;if(cfg_read_error)return ZM_ERROR;
    if(!cfg_exists || strstr(p,".bak"))return ZM_NOT_FOUND;
    *n=strlen(cfg_disk);if(*n>max)return ZM_ERROR;
    *data=malloc(*n);memcpy(*data,cfg_disk,*n);return ZM_OK;
}
static int cfg_write(const char*p,const uint8_t*data,size_t n,const volatile int*active){
    (void)active;CHECK(tsconfig,"write .cfg only after core_back");
    CHECK(!strcmp(p,"/CORES/next.cfg"),"write alongside NEXT.BIN");++cfg_writes;
    if(cfg_write_error)return ZM_ERROR;
    memcpy(cfg_disk,data,n);cfg_disk[n]=0;cfg_exists=1;return ZM_OK;
}
static uint32_t now_ms = 1000;
static uint32_t reg_free = UART_TX_FIFO;   /* UART_FREE: [6:0] свободно, [7] передатчик занят */
static int free_tear;                      /* столько ближайших чтений UART_FREE — разные */
static int fail_writes;                    /* столько ближайших записей LINK — с ошибкой */
static int max_packet_over;                /* пакет UART_TX больше свободного места */
static char uart_out[256];
static uint32_t uart_out_n, reg_div = 244, audio_wp, uart_wp;
static struct { uint32_t a, v; } wlog[256];
static int nlog;
static int back_result = ZM_OK, back_calls, state_at_back = -1, live_at_back = -1;
static int pcm_accept = 16;
static uint32_t pcm_frames, pcm_calls;
static uint8_t replies[64];
static int audio_feed;                     /* каждое чтение AUDIO_WP — ещё 16 отсчётов */
static int pace_delays, pace_steps, pace_max;   /* сон в сеансе звука: сколько, шагов между ними */

static uint32_t gray(uint32_t b) { return b ^ (b >> 1); }

static int f_read(uint32_t a, uint32_t *w, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++, a += 4) {
        if(a==4)w[i]=hw_version;
        else if(a==0x80)w[i]=0x53500000u|(hw_choice<<8)|(255u^hw_choice);
        else if (a == R_UART_FREE)
            w[i] = free_tear > 0 ? (uint32_t)(free_tear-- & 1 ? 0x7F : 0x00) : reg_free;
        else if (a == R_UART_WP)
            w[i] = gray(uart_wp & 0x3FF);
        else if (a == R_AUDIO_WP) {
            if (audio_feed) {
                audio_wp += 16;
                pace_steps++;
            }
            w[i] = gray(audio_wp & 0x7FF);
        }
        else if (a == R_AUDIO_DIV)
            w[i] = 586;
        else if (a == R_CLK_KHZ)
            w[i] = 28125;
        else if (a >= W_AUDIO)
            w[i] = (a - W_AUDIO) / 4;            /* отсчёт — номер ячейки */
        else if (a >= W_UART)
            w[i] = 'a' + (a - W_UART) / 4 % 26;
        else
            w[i] = 0;
    }
    return ZM_OK;
}

static int f_write(uint32_t a, const uint32_t *w, uint32_t n)
{
    if (fail_writes > 0) {
        fail_writes--;
        return ZM_ERROR;
    }
    for (uint32_t i = 0; i < n; i++, a += 4) {
        if (nlog < 256) {
            wlog[nlog].a = a;
            wlog[nlog].v = w[i];
            nlog++;
        }
        if(a==0x80)hw_choice=w[i]&7u;
        if (a == R_UART_TX) {
            const uint32_t k = w[i] >> 24;
            if (k > (reg_free & 0x7F)) {
                max_packet_over = 1;              /* ядро выбросило бы пакет целиком */
            } else {
                for (uint32_t j = 0; j < k; j++)
                    uart_out[uart_out_n++] = (char)(w[i] >> (8 * j));
                reg_free -= k;
            }
        } else if (a == R_UART_DIV) {
            reg_div = w[i];
        }
    }
    return ZM_OK;
}

static void f_log(const char *format, ...) { (void)format; }
static uint32_t f_millis(void) { return now_ms; }
static void f_delay(uint32_t ms)
{
    now_ms += ms;
    if (audio_feed) {
        if (pace_steps > pace_max)
            pace_max = pace_steps;
        pace_steps = 0;
        if (++pace_delays >= 20)
            session_live = 0;                       /* хватит: сеанс снят */
    }
}
static void f_mutex(void *m) { (void)m; }
static void f_signal(void *s) { (void)s; }
static int f_reply(uint8_t r, const void *d, uint16_t n)
{
    (void)r;
    memcpy(replies, d, n < sizeof replies ? n : sizeof replies);
    return ZM_OK;
}
static int f_back(uint32_t t)
{
    (void)t;
    back_calls++;
    state_at_back = state;
    live_at_back = session_live;
    if(back_result==ZM_OK){tsconfig=1;hw_version=0x0101011e;}
    return back_result;
}
static int f_load(const char *name, uint32_t *v) { (void)name; *v = hw_version = 0x03010011;hw_choice=0;tsconfig=0; return ZM_OK; }
static int f_pcm_open(const zm_pcm_format_t *f) { (void)f; return ZM_OK; }
static int f_pcm_write(const float *fr, uint32_t n)
{
    (void)fr;
    pcm_calls++;
    const uint32_t k = n < (uint32_t)pcm_accept ? n : (uint32_t)pcm_accept;
    pcm_frames += k;
    return (int)k;
}
static void f_pcm_close(void) {}
static int f_sock_open(int udp, const char *h, uint16_t p, uint32_t t) { (void)udp; (void)h; (void)p; (void)t; return -1; }
static int f_sock_send(int s, const void *d, uint32_t n) { (void)s; (void)d; (void)n; return ZM_ERROR; }
static int f_sock_recv(int s, void *d, uint32_t n, uint32_t t) { (void)s; (void)d; (void)n; (void)t; return -1; }
static void f_sock_close(int s) { (void)s; }
static int f_resolve(const char *h, char ip[16]) { (void)h; (void)ip; return ZM_NOT_FOUND; }
static int f_wifi(char s[33], char ip[16], char mac[18]) { (void)s; (void)ip; (void)mac; return ZM_ERROR; }

static zm_core_t fake;

static int wrote(uint32_t a, uint32_t v)
{
    for (int i = 0; i < nlog; i++)
        if (wlog[i].a == a && wlog[i].v == v)
            return i;
    return -1;
}

static void queue_tx(const char *s)
{
    at_out(NULL, (const uint8_t *)s, (uint32_t)strlen(s));
}

static void input(const uint8_t *p, uint16_t n) { command(0x71, p, n); }

/* ---- UART ---- */
static void test_uart(void)
{
    session_live = 1;
    session_id = 1;
    CHECK(net_session_start(1) && reg_div == 244 && baud == UART_BAUD0, "начало сеанса: делитель %u", (unsigned)reg_div);
    queue_tx("ABCDEFGHIJ");
    reg_free = 0;
    uart_give();
    CHECK(uart_out_n == 0 && tx_tail == 0, "места нет — ничего: %u", (unsigned)uart_out_n);
    reg_free = 1;
    uart_give();
    CHECK(uart_out_n == 1 && tx_tail == 1, "свободен 1 байт — пакет из одного: %u", (unsigned)uart_out_n);
    reg_free = 2;
    uart_give();
    CHECK(uart_out_n == 3 && tx_tail == 3, "свободно 2 — пакет из двух: %u", (unsigned)uart_out_n);
    reg_free = 64;
    uart_give();
    CHECK(uart_out_n == 10 && memcmp(uart_out, "ABCDEFGHIJ", 10) == 0 && tx_tail == tx_head, "остальное: [%.*s]",
          (int)uart_out_n, uart_out);
    CHECK(!max_packet_over, "пакет больше свободного места");
    /* запись не прошла — байты остаются в очереди ESP */
    queue_tx("XYZ");
    reg_free = 64;
    fail_writes = 1;
    uart_give();
    CHECK(tx_tail + 3 == tx_head && uart_out_n == 10, "ошибка записи — очередь не сдвинута");
    uart_give();
    CHECK(tx_tail == tx_head && uart_out_n == 13 && memcmp(uart_out + 10, "XYZ", 3) == 0, "повтор — дошло");
    /* «рваное» UART_FREE: три пары чтений не совпали — пропуск */
    queue_tx("Q");
    free_tear = 6;
    uart_give();
    CHECK(tx_tail + 1 == tx_head, "два чтения UART_FREE разные — не писать");
    free_tear = 0;
    uart_give();
    CHECK(tx_tail == tx_head, "чтения совпали — записано");

    /* смена скорости: передатчик занят — ждать; стоит — сменить */
    baud_pending = 230400;
    reg_free = UART_TX_FIFO | UART_TX_BUSY;
    uart_step();
    CHECK(baud == UART_BAUD0 && reg_div == 244, "передатчик шлёт — скорость прежняя");
    reg_free = UART_TX_FIFO;
    const uint32_t t0 = now_ms;
    uart_step();
    CHECK(baud == 230400 && reg_div == 122 && now_ms > t0, "передатчик стоит — 230 400 (делитель %u)", (unsigned)reg_div);
    /* новый сеанс — снова 115 200 */
    session_id = 2;
    CHECK(net_session_start(2) && baud == UART_BAUD0 && reg_div == 244 && tx_head == tx_tail,
          "START — 115 200 и пустая очередь (делитель %u)", (unsigned)reg_div);
    /* сеанс снят — LINK ядра не трогать */
    end_session();
    const int before = nlog;
    queue_tx("late");
    uart_step();
    CHECK(nlog == before, "после конца сеанса записей LINK нет");
}

/* ---- выход ---- */
static void test_leave(void)
{
    state = 2;
    session_live = 1;
    back_result = ZM_ERROR;
    leave_core();
    CHECK(state == 255 && strstr(error_text, "could not return") && state_at_back == 2 && live_at_back == 0,
          "возврат не вышел: состояние %d, при core_back %d/%d", state, state_at_back, live_at_back);
    state = 2;
    session_live = 1;
    back_result = ZM_OK;
    leave_core();
    CHECK(state == 3 && state_at_back == 2, "возврат вышел — 3 только после него");
    CHECK(back_calls == 2, "core_back: %d вызова", back_calls);
}

/* ---- ввод ---- */
static void test_input(void)
{
    ps2_hid_t kb;
    uint8_t joy_sent = 0, btn_sent = 0;
    ps2_hid_init(&kb);
    load_core();
    nlog = 0;
    /* мышь: первый пакет — только отсчёт */
    const uint8_t first[4] = {0, 0xFF, 100, 50};
    input(first, 4);
    input_step(&kb, &joy_sent, &btn_sent);
    int mouse = 0;
    for (int i = 0; i < nlog; i++)
        mouse |= wlog[i].a == R_MOUSE;
    CHECK(!mouse, "первый пакет — без движения");
    const uint8_t second[4] = {0, 0xFF, 110, 50};
    input(second, 4);
    input_step(&kb, &joy_sent, &btn_sent);
    CHECK(nlog > 0 && wlog[nlog - 1].a == R_MOUSE && (wlog[nlog - 1].v & 0xFF) == 20, "второй — dx 20 (2x)");
    /* три пакета по +100 без разбора — 300, по частям 127 + 127 + 46 */
    uint8_t x = 110;
    for (int i = 0; i < 3; i++) {
        x = (uint8_t)(x + 100);
        const uint8_t p[4] = {0, 0xFF, x, 50};
        input(p, 4);
    }
    uint32_t parts[5] = {0};
    for (int i = 0; i < 5; i++) {
        nlog = 0;
        input_step(&kb, &joy_sent, &btn_sent);
        parts[i] = nlog ? wlog[nlog - 1].v & 0xFF : 0;
    }
    CHECK(parts[0] == 127 && parts[1] == 127 && parts[2] == 127 && parts[3] == 127 && parts[4] == 92, "300 по частям: %u %u %u", (unsigned)parts[0],
          (unsigned)parts[1], (unsigned)parts[2]);

    /* PS/2: префикс отпускания, затем переполнение очереди */
    const uint8_t f0[5] = {0, 0xFF, x, 50, 0xF0};
    input(f0, 5);
    input_step(&kb, &joy_sent, &btn_sent);
    uint8_t big[4 + 100];
    memcpy(big, f0, 4);
    memset(big + 4, 0x1C, 100);
    for (int i = 0; i < 3; i++)
        input(big, sizeof big);
    CHECK(ps2_lost && ps2_head == ps2_tail, "переполнение: очередь выброшена");
    const uint8_t a[5] = {0, 0xFF, x, 50, 0x1C};
    input(a, 5);
    nlog = 0;
    input_step(&kb, &joy_sent, &btn_sent);
    const int clear = wrote(R_KEYS_CLEAR, 0), key = wrote(R_KEY, 4);
    CHECK(clear >= 0 && key > clear && wrote(R_KEY, 4 | PS2_HID_RELEASE) < 0,
          "после переполнения: KEYS_CLEAR (%d), затем нажатие A (%d), не отпускание", clear, key);
    end_session();
}

/* ---- звук ---- */
static void test_audio(void)
{
    session_live = 1;
    running = 1;
    audio_pos_t a = {0, now_ms, 0};
    audio_wp = 40;
    pcm_accept = 5;
    pcm_frames = pcm_calls = 0;
    CHECK(audio_step(&a) == 40 && a.rp == 40 && pcm_frames == 40 && pcm_calls == 10,
          "частичный pcm_write: принято %u за %u вызовов, rp %u", (unsigned)pcm_frames, (unsigned)pcm_calls, (unsigned)a.rp);
    now_ms += AUDIO_LAG_MS + 5;                     /* не читали дольше: обороты не сосчитать */
    audio_wp = 100;
    pcm_accept = 16;
    CHECK(audio_step(&a) == 256 && a.rp == 100, "догнать после паузы: rp %u", (unsigned)a.rp);
    end_session();
    CHECK(!audio_step(&a), "сеанс снят — LINK не читать");

    /* Ядро пишет быстрее, чем читаем: кольцо не пустеет — спать всё равно */
    session_live = 1;
    audio_feed = 1;
    pace_delays = pace_steps = pace_max = 0;
    audio_session();
    audio_feed = 0;
    CHECK(pace_delays == 20 && pace_max >= 1 && pace_max <= 2,
          "сеанс звука: %d чтений указателя между снами (не больше 2), снов %d", pace_max, pace_delays);
}

int main(void)
{
    fake.size = sizeof fake;
    fake.abi_major = 1;fake.abi_minor=9;fake.free=free;fake.sd_read=cfg_read;fake.sd_write=cfg_write;
    fake.log = f_log;
    fake.millis = f_millis;
    fake.delay = f_delay;
    fake.mutex_lock = f_mutex;
    fake.mutex_unlock = f_mutex;
    fake.signal_give = f_signal;
    fake.reply = f_reply;
    fake.pcm_open = f_pcm_open;
    fake.pcm_write = f_pcm_write;
    fake.pcm_close = f_pcm_close;
    fake.core_load = f_load;
    fake.core_back = f_back;
    fake.link_read = f_read;
    fake.link_write = f_write;
    fake.sock_open = f_sock_open;
    fake.sock_send = f_sock_send;
    fake.sock_recv = f_sock_recv;
    fake.sock_close = f_sock_close;
    fake.net_resolve = f_resolve;
    fake.wifi_info = f_wifi;
    core = &fake;
    (void)start;                                    /* таблицу приложения здесь не собираем */
    (void)stop;
    running=1;CHECK(load_core(),"initial core load");
    test_uart();
    test_leave();
    test_input();
    test_audio();
    printf("проверок %d, ошибок %d\n%s\n", checks, bad, bad ? "ТЕСТ НЕ ПРОЙДЕН" : "ТЕСТ ПРОЙДЕН");
    return bad != 0;
}
