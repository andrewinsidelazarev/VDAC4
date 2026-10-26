/* SPDX-License-Identifier: Apache-2.0 */
/* Эмулятор ESP-AT для ZX Next на VDAC4 (esp_at.h). */
#include "esp_at.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void out(esp_at_t *s, const char *text)
{
    s->io->out(s->io->ctx, (const uint8_t *)text, (uint32_t)strlen(text));
}

static void outf(esp_at_t *s, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0)
        s->io->out(s->io->ctx, (const uint8_t *)buf, (uint32_t)(n < (int)sizeof buf ? n : (int)sizeof buf - 1));
}

static void ok(esp_at_t *s) { out(s, "\r\nOK\r\n"); }
static void error(esp_at_t *s) { out(s, "\r\nERROR\r\n"); }

void esp_at_init(esp_at_t *s, const esp_at_io_t *io)
{
    memset(s, 0, sizeof *s);
    s->io = io;
    s->echo = 1;
    s->send_link = -1;
    s->baud = 115200;
    for (int i = 0; i < ESP_AT_LINKS; i++)
        s->sock[i] = -1;
}

void esp_at_close_all(esp_at_t *s)
{
    for (int i = 0; i < ESP_AT_LINKS; i++)
        if (s->sock[i] >= 0) {
            s->io->sock_close(s->io->ctx, s->sock[i]);
            s->sock[i] = -1;
        }
    s->send_link = -1;
}

/* аргументы «a,"b",c»: в out[] — указатели на строки в buf; кавычки снимаются */
static int split(char *p, char **arg, int max)
{
    int n = 0;
    while (*p && n < max) {
        if (*p == '"') {
            p++;
            arg[n++] = p;
            while (*p && *p != '"')
                p++;
            if (*p == '"')
                *p++ = 0;
            while (*p && *p != ',')
                p++;
        } else {
            arg[n++] = p;
            while (*p && *p != ',')
                p++;
        }
        if (*p == ',')
            *p++ = 0;
    }
    return n;
}

/* число из одних цифр, не больше max: 1 — годится (atoi не ловит ни мусор, ни переполнение) */
static int num(const char *p, uint32_t max, uint32_t *v)
{
    uint32_t r = 0;
    if (!*p)
        return 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9')
            return 0;
        const uint32_t d = (uint32_t)(*p - '0');
        if (d > max || r > (max - d) / 10)
            return 0;
        r = r * 10 + d;
    }
    *v = r;
    return 1;
}

/* имя команды: «AT+NAME» без _CUR/_DEF; rest — после «=» или «?» */
static int cmd_is(const char *line, const char *name, const char **rest)
{
    size_t n = strlen(name);
    if (strncmp(line, name, n) != 0)
        return 0;
    const char *p = line + n;
    if (strncmp(p, "_CUR", 4) == 0 || strncmp(p, "_DEF", 4) == 0)
        p += 4;
    if (*p != 0 && *p != '=' && *p != '?')
        return 0;
    *rest = p;
    return 1;
}

static void cmd_start(esp_at_t *s, char *args)
{
    char *a[6];
    int n = split(args, a, 6);
    uint32_t id = 0, port = 0;
    int k = 0;
    if (s->mux) {
        if (n < 1 || !num(a[0], ESP_AT_LINKS - 1, &id)) {
            error(s);
            return;
        }
        k = 1;
    }
    if (n - k < 3 || !a[k + 1][0] || !num(a[k + 2], 65535, &port) || port == 0) {
        error(s);
        return;
    }
    const int udp = strcmp(a[k], "UDP") == 0;
    if (!udp && strcmp(a[k], "TCP") != 0) {      /* SSL — нет */
        error(s);
        return;
    }
    if (s->sock[id] >= 0) {
        out(s, "ALREADY CONNECTED\r\n\r\nERROR\r\n");
        return;
    }
    const int sock = s->io->sock_open(s->io->ctx, udp, a[k + 1], (uint16_t)port);
    if (sock < 0) {
        if (s->mux)
            outf(s, "%u,CLOSED\r\n\r\nERROR\r\n", (unsigned)id);
        else
            out(s, "ERROR\r\nCLOSED\r\n");
        return;
    }
    s->sock[id] = sock;
    s->udp[id] = (uint8_t)udp;
    snprintf(s->remote[id], sizeof s->remote[id], "%s", a[k + 1]);
    s->port[id] = (uint16_t)port;
    if (s->mux)
        outf(s, "%u,CONNECT\r\n\r\nOK\r\n", (unsigned)id);
    else
        out(s, "CONNECT\r\n\r\nOK\r\n");
}

static void cmd_send(esp_at_t *s, char *args)
{
    char *a[3];
    int n = split(args, a, 3);
    uint32_t id = 0, len = 0;
    int k = 0;
    if (s->mux) {
        if (n < 2 || !num(a[0], ESP_AT_LINKS - 1, &id)) {
            error(s);
            return;
        }
        k = 1;
    }
    if (n - k < 1 || !num(a[k], sizeof s->send_buf, &len)) {
        error(s);
        return;
    }
    if (s->sock[id] < 0) {
        out(s, "link is not valid\r\n\r\nERROR\r\n");
        return;
    }
    if (len == 0) {
        error(s);
        return;
    }
    s->send_link = (int)id;
    s->send_left = len;
    s->send_total = len;
    s->send_at = 0;
    out(s, "\r\nOK\r\n> ");
}

static void send_done(esp_at_t *s)
{
    const int id = s->send_link;
    s->send_link = -1;
    outf(s, "\r\nRecv %u bytes\r\n", (unsigned)s->send_total);
    if (s->sock[id] >= 0 && s->io->sock_send(s->io->ctx, s->sock[id], s->send_buf, s->send_total) == 0) {
        out(s, "\r\nSEND OK\r\n");
    } else {
        out(s, "\r\nSEND FAIL\r\n");
    }
}

static void cmd_close(esp_at_t *s, const char *rest)
{
    uint32_t id = 0;
    if (*rest == '=') {
        if (!num(rest + 1, ESP_AT_LINKS, &id)) {
            error(s);
            return;
        }
        if (id == ESP_AT_LINKS) {                  /* 5 — все */
            for (int i = 0; i < ESP_AT_LINKS; i++)
                if (s->sock[i] >= 0) {
                    s->io->sock_close(s->io->ctx, s->sock[i]);
                    s->sock[i] = -1;
                }
            ok(s);
            return;
        }
    } else if (s->mux) {
        error(s);
        return;
    }
    if (s->sock[id] < 0) {
        out(s, "\r\nERROR\r\n");
        return;
    }
    s->io->sock_close(s->io->ctx, s->sock[id]);
    s->sock[id] = -1;
    if (s->mux)
        outf(s, "%u,CLOSED\r\n\r\nOK\r\n", (unsigned)id);
    else
        out(s, "CLOSED\r\n\r\nOK\r\n");
}

static void command(esp_at_t *s, char *line)
{
    const char *rest = NULL;
    char ssid[33] = "", ip[16] = "0.0.0.0", mac[18] = "00:00:00:00:00:00";
    if (strcmp(line, "AT") == 0) {
        ok(s);
    } else if (strcmp(line, "ATE0") == 0) {
        s->echo = 0;
        ok(s);
    } else if (strcmp(line, "ATE1") == 0) {
        s->echo = 1;
        ok(s);
    } else if (strcmp(line, "AT+RST") == 0) {
        ok(s);
        esp_at_close_all(s);
        s->echo = 1;
        s->mux = 0;
        out(s, "\r\nready\r\n");
    } else if (strcmp(line, "AT+GMR") == 0) {
        out(s, "AT version:1.7.4.0(VDAC4 ESP32-S3 emulation)\r\nSDK version:3.0.4\r\n"
               "compile time:NEXT.ELF\r\nBin version(Wroom 02):1.7.4\r\n");
        ok(s);
    } else if (cmd_is(line, "AT+CWMODE", &rest)) {
        if (*rest == '?')
            out(s, "+CWMODE:1\r\n");
        ok(s);
    } else if (cmd_is(line, "AT+CWJAP", &rest)) {
        const int up = s->io->wifi(s->io->ctx, ssid, ip, mac);
        if (*rest == '?') {
            if (up)
                outf(s, "+CWJAP:\"%s\"\r\n", ssid);
            else
                out(s, "No AP\r\n");
            ok(s);
        } else if (up) {
            out(s, "WIFI CONNECTED\r\nWIFI GOT IP\r\n");
            ok(s);
        } else {
            out(s, "+CWJAP:3\r\n\r\nFAIL\r\n");
        }
    } else if (strcmp(line, "AT+CWQAP") == 0 || strcmp(line, "AT+CWLAP") == 0) {
        ok(s);
    } else if (strcmp(line, "AT+CIFSR") == 0) {
        s->io->wifi(s->io->ctx, ssid, ip, mac);
        outf(s, "+CIFSR:STAIP,\"%s\"\r\n+CIFSR:STAMAC,\"%s\"\r\n", ip, mac);
        ok(s);
    } else if (cmd_is(line, "AT+CIPMUX", &rest)) {
        if (*rest == '?') {
            outf(s, "+CIPMUX:%d\r\n", s->mux);
            ok(s);
        } else if (rest[0] == '=' && (rest[1] == '0' || rest[1] == '1') && rest[2] == 0) {
            s->mux = rest[1] == '1';
            ok(s);
        } else {
            error(s);
        }
    } else if (cmd_is(line, "AT+CIPMODE", &rest)) {
        if (*rest == '?') {
            out(s, "+CIPMODE:0\r\n");
            ok(s);
        } else if (strcmp(rest, "=0") == 0) {
            ok(s);
        } else {
            error(s);                               /* прозрачный режим — нет */
        }
    } else if (cmd_is(line, "AT+CIPDINFO", &rest) || cmd_is(line, "AT+CIPRECVMODE", &rest)) {
        if (cmd_is(line, "AT+CIPRECVMODE", &rest) && strcmp(rest, "=0") != 0 && *rest != '?')
            error(s);                               /* пассивный приём — нет */
        else
            ok(s);
    } else if (cmd_is(line, "AT+CIPSTART", &rest) && *rest == '=') {
        cmd_start(s, (char *)rest + 1);
    } else if (cmd_is(line, "AT+CIPSEND", &rest) && *rest == '=') {
        cmd_send(s, (char *)rest + 1);
    } else if (cmd_is(line, "AT+CIPCLOSE", &rest)) {
        cmd_close(s, rest);
    } else if (strcmp(line, "AT+CIPSTATUS") == 0) {
        int any = 0;
        for (int i = 0; i < ESP_AT_LINKS; i++)
            any |= s->sock[i] >= 0;
        const int up = s->io->wifi(s->io->ctx, ssid, ip, mac);
        outf(s, "STATUS:%d\r\n", !up ? 5 : any ? 3 : 2);
        for (int i = 0; i < ESP_AT_LINKS; i++)
            if (s->sock[i] >= 0)
                outf(s, "+CIPSTATUS:%d,\"%s\",\"%s\",%u,0,0\r\n", i, s->udp[i] ? "UDP" : "TCP", s->remote[i],
                     (unsigned)s->port[i]);
        ok(s);
    } else if (cmd_is(line, "AT+CIPDOMAIN", &rest) && *rest == '=') {
        char *a[1];
        char host[64];
        snprintf(host, sizeof host, "%s", rest + 1);
        if (split(host, a, 1) < 1 || !a[0][0]) {
            error(s);
        } else if (s->io->resolve(s->io->ctx, a[0], ip) == 0) {
            outf(s, "+CIPDOMAIN:%s\r\n", ip);
            ok(s);
        } else {
            out(s, "DNS Fail\r\n\r\nERROR\r\n");
        }
    } else if (cmd_is(line, "AT+PING", &rest) && *rest == '=') {
        char *a[1];
        char host[64];
        snprintf(host, sizeof host, "%s", rest + 1);
        if (split(host, a, 1) < 1 || !a[0][0]) {
            error(s);
        } else if (s->io->resolve(s->io->ctx, a[0], ip) == 0) {
            out(s, "+1\r\n");
            ok(s);
        } else {
            out(s, "+timeout\r\n\r\nERROR\r\n");
        }
    } else if (cmd_is(line, "AT+UART", &rest)) {
        char args[64], *a[5];
        uint32_t b = 0;
        snprintf(args, sizeof args, "%s", *rest == '=' ? rest + 1 : "");
        if (*rest == '?') {
            outf(s, "+UART_CUR:%u,8,1,0,0\r\n", (unsigned)s->baud);
            ok(s);
        } else if (*rest == '=' && split(args, a, 5) >= 1 && num(a[0], 4000000, &b) && b >= 1200) {
            s->pending_baud = b;
            ok(s);                                  /* ESP8266 отвечает ещё на прежней скорости */
        } else {
            error(s);
        }
    } else {
        error(s);
    }
}

void esp_at_input(esp_at_t *s, const uint8_t *data, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t c = data[i];
        if (s->skip_lf) {                           /* CR LF конца строки: LF — её же */
            s->skip_lf = 0;
            if (c == '\n')
                continue;
        }
        if (s->send_link >= 0) {                    /* данные AT+CIPSEND */
            s->send_buf[s->send_at++] = c;
            if (--s->send_left == 0)
                send_done(s);
            continue;
        }
        /* эхо — как у ESP8266: «AT» и CR, после конца строки — CR LF (ответ на «AT» CR LF —
           «AT» CR CR LF CR LF «OK» CR LF), сам LF не повторяется */
        if (s->echo && c != '\n')
            s->io->out(s->io->ctx, &c, 1);
        if (c == '\n' || c == '\r') {
            if (s->len > 0) {
                s->line[s->len] = 0;
                if (s->echo)
                    out(s, "\r\n");
                s->skip_lf = c == '\r';
                command(s, s->line);
                s->len = 0;
                if (s->pending_baud) {
                    s->baud = s->pending_baud;
                    s->pending_baud = 0;
                    s->io->set_baud(s->io->ctx, s->baud);
                }
            }
            continue;
        }
        if (s->len < ESP_AT_LINE - 1)
            s->line[s->len++] = (char)c;
    }
}

void esp_at_poll(esp_at_t *s)
{
    static uint8_t buf[1472];                       /* датаграмма UDP при MTU 1500 */
    for (int k = 0; k < ESP_AT_LINKS; k++) {
        const int id = (s->rx_poll + k) % ESP_AT_LINKS;
        if (s->sock[id] < 0 || s->send_link >= 0)
            continue;
        const uint32_t room = s->io->out_room(s->io->ctx);
        if (room < 64)
            return;
        uint32_t max = room - 32;
        if (max > sizeof buf)
            max = sizeof buf;
        if (s->udp[id] && max < sizeof buf)
            continue;                               /* датаграмму — только целиком: остаток пропал бы */
        const int got = s->io->sock_recv(s->io->ctx, s->sock[id], buf, max);
        if (got > 0) {
            if (s->mux)
                outf(s, "\r\n+IPD,%d,%d:", id, got);
            else
                outf(s, "\r\n+IPD,%d:", got);
            s->io->out(s->io->ctx, buf, (uint32_t)got);
            s->rx_poll = (uint8_t)((id + 1) % ESP_AT_LINKS);
        } else if (got < 0) {
            s->io->sock_close(s->io->ctx, s->sock[id]);
            s->sock[id] = -1;
            if (s->mux)
                outf(s, "%d,CLOSED\r\n", id);
            else
                out(s, "CLOSED\r\n");
        }
    }
}
