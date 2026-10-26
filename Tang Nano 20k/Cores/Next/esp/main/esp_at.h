/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Эмулятор ESP-AT (прошивка AT ESP8266, как у модуля Wi-Fi ZX Next) поверх сети ESP32-S3 платы
 * VDAC4: Next шлёт команды в свой UART ESP — ядро отдаёт их в LINK (кольцо UART), NEXT.ELF — сюда;
 * ответы и данные сокетов идут обратно в UART Next. Wi-Fi своё у ESP (настраивается прошивкой
 * ESP), поэтому AT+CWJAP только сообщает о подключении.
 *
 * Команды: AT, ATE0/ATE1, AT+RST, AT+GMR, AT+CWMODE[_CUR|_DEF], AT+CWJAP[_CUR|_DEF], AT+CWQAP,
 * AT+CWLAP, AT+CIFSR, AT+CIPMUX, AT+CIPMODE (только 0), AT+CIPSTART (TCP, UDP), AT+CIPSEND,
 * AT+CIPCLOSE, AT+CIPSTATUS, AT+CIPDOMAIN, AT+CIPDINFO, AT+UART[_CUR|_DEF], AT+PING; прочее —
 * ERROR. Принятое из сокетов — «+IPD,[id,]len:данные», закрытие удалённой стороной —
 * «[id,]CLOSED». Соединений — до пяти (CIPMUX=1: id 0…4).
 *
 * Код переносимый (тест на ПК — tests/host_test.c): сеть, скорость UART и вывод — через
 * esp_at_io_t. Принятое из сокета берётся, только когда в выводе есть место (out_room): Next
 * читает UART медленнее сети.
 */
#ifndef ESP_AT_H
#define ESP_AT_H

#include <stdint.h>

#define ESP_AT_LINKS   5
#define ESP_AT_LINE    256

typedef struct {
    void *ctx;
    /* сокет к host:port; номер ≥ 0 или −1 */
    int (*sock_open)(void *ctx, int udp, const char *host, uint16_t port);
    int (*sock_send)(void *ctx, int sock, const uint8_t *data, uint32_t n);   /* 0 — отправлено */
    /* без ожидания: > 0 — сколько, 0 — нечего, −1 — закрыт */
    int (*sock_recv)(void *ctx, int sock, uint8_t *data, uint32_t max);
    void (*sock_close)(void *ctx, int sock);
    /* имя → адрес «a.b.c.d»; 0 — нашёлся */
    int (*resolve)(void *ctx, const char *host, char ip[16]);
    /* Wi-Fi ESP: 1 — подключена (ssid, ip, mac заполнены) */
    int (*wifi)(void *ctx, char ssid[33], char ip[16], char mac[18]);
    /* новая скорость UART Next (после ответа OK) */
    void (*set_baud)(void *ctx, uint32_t baud);
    /* байты в UART Next; out_room — сколько ещё поместится */
    void (*out)(void *ctx, const uint8_t *data, uint32_t n);
    uint32_t (*out_room)(void *ctx);
    uint32_t (*ms)(void *ctx);
} esp_at_io_t;

typedef struct {
    const esp_at_io_t *io;
    int echo, mux;
    char line[ESP_AT_LINE];
    uint32_t len;
    /* AT+CIPSEND: ждём данные */
    int send_link;
    uint32_t send_left, send_total;
    uint8_t send_buf[2048];
    uint32_t send_at;
    int sock[ESP_AT_LINKS];          /* −1 — нет соединения */
    uint8_t udp[ESP_AT_LINKS];
    char remote[ESP_AT_LINKS][64];
    uint16_t port[ESP_AT_LINKS];
    uint32_t baud;
    uint32_t pending_baud;           /* сменить после OK */
    uint8_t rx_poll;                 /* с какого соединения смотреть приём */
    uint8_t skip_lf;                 /* строка кончилась на CR: следующий LF — её же */
} esp_at_t;

void esp_at_init(esp_at_t *s, const esp_at_io_t *io);
/* байты от Next */
void esp_at_input(esp_at_t *s, const uint8_t *data, uint32_t n);
/* принятое из сокетов → +IPD (сколько поместится в вывод) */
void esp_at_poll(esp_at_t *s);
/* закрыть всё (выход приложения) */
void esp_at_close_all(esp_at_t *s);

#endif
