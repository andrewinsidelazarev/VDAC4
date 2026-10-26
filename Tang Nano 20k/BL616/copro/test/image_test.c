/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Проверка ft_image.c (CMD_LOADIMAGE) на ПК: картинки data/image (make_image_data.py) — кусками
 * разной длины, как из кольца; выход — байт в байт с ожидаемым (JPEG — libjpeg, PNG — FT812).
 *
 *   image_test DIR      (DIR/list.txt: файл, ожидание, fmt, w, h, stride, байтов палитры)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft_copro.h"

static uint8_t *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*n + 4);
    if (fread(p, 1, *n, f) != *n) {
        fclose(f);
        free(p);
        return NULL;
    }
    fclose(f);
    return p;
}

typedef struct {
    uint8_t *buf;
    size_t n, cap;
} sink_t;

static void emit(void *u, const uint8_t *p, size_t n)
{
    sink_t *s = (sink_t *)u;
    if (s->n + n > s->cap) {
        s->cap = (s->n + n) * 2;
        s->buf = realloc(s->buf, s->cap);
    }
    memcpy(s->buf + s->n, p, n);
    s->n += n;
}

static _Alignas(8) uint8_t img_state[FTC_IMG_BYTES];
static _Alignas(8) uint8_t infl_state[FTC_INFLATE_BYTES];

/* chunk: 0 — всё сразу, иначе куски 1…chunk (псевдослучайно) */
static int run_case(const uint8_t *data, size_t n, size_t chunk, sink_t *s, ftc_img_info_t *info,
                    size_t *consumed)
{
    unsigned seed = 12345;
    ftc_img_begin(img_state, 0, infl_state);
    size_t at = 0;
    int r = FTC_IMG_NEED_INPUT;
    while (r == FTC_IMG_NEED_INPUT) {
        size_t k = n - at;
        if (chunk && k) {
            seed = seed * 1103515245u + 12345u;
            size_t c = 1 + (seed >> 8) % chunk;
            if (k > c)
                k = c;
        }
        size_t used = 0;
        r = ftc_img_run(img_state, data + at, k, &used, emit, s);
        at += used;
        if (r == FTC_IMG_NEED_INPUT && used == 0 && at >= n)
            break;
    }
    ftc_img_get_info(img_state, info);
    *consumed = at;
    return r;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "image_test DIR\n");
        return 2;
    }
    char path[1024], line[512];
    snprintf(path, sizeof path, "%s/list.txt", argv[1]);
    FILE *lst = fopen(path, "r");
    if (!lst) {
        fprintf(stderr, "нет %s\n", path);
        return 2;
    }
    printf("состояние декодера: %u байт\n", (unsigned)ftc_img_state_size());
    int fails = 0, cases = 0;
    while (fgets(line, sizeof line, lst)) {
        char file[256], expf[256];
        unsigned fmt, w, h, stride, pal;
        if (sscanf(line, "%255s %255s %u %u %u %u %u", file, expf, &fmt, &w, &h, &stride, &pal) != 7)
            continue;
        size_t n, en;
        snprintf(path, sizeof path, "%s/%s", argv[1], file);
        uint8_t *data = load(path, &n);
        snprintf(path, sizeof path, "%s/%s", argv[1], expf);
        uint8_t *exp = load(path, &en);
        if (!data || !exp) {
            printf("%-14s нет файлов\n", file);
            fails++;
            continue;
        }
        /* как из кольца: данные дополнены до кратного 4 */
        size_t padded = (n + 3) & ~(size_t)3;
        memset(data + n, 0, padded - n);
        static const size_t chunks[] = {0, 1, 7, 64, 1000, 4096};
        for (size_t ci = 0; ci < sizeof chunks / sizeof chunks[0]; ci++) {
            sink_t s = {0};
            ftc_img_info_t info;
            size_t consumed = 0;
            int r = run_case(data, padded, chunks[ci], &s, &info, &consumed);
            cases++;
            size_t diff = 0, first = (size_t)-1;
            for (size_t i = 0; i < en && i < s.n; i++)
                if (s.buf[i] != exp[i]) {
                    if (first == (size_t)-1)
                        first = i;
                    diff++;
                }
            int ok = r == FTC_IMG_DONE && s.n == en && diff == 0 && info.fmt == fmt && info.w == w &&
                     info.h == h && info.stride == stride && info.pal_bytes == pal &&
                     consumed <= padded && consumed + 4 > n;
            if (!ok || ci == 0)
                printf("%-14s куски %4u: %s r=%d вывод %u/%u расходятся %u (первый %d) fmt %u %ux%u шаг %u пал %u взято %u/%u\n",
                       file, (unsigned)chunks[ci], ok ? "да " : "НЕТ", r, (unsigned)s.n, (unsigned)en,
                       (unsigned)diff, (int)first, info.fmt, info.w, info.h, info.stride, info.pal_bytes,
                       (unsigned)consumed, (unsigned)n);
            if (!ok)
                fails++;
            free(s.buf);
        }
        free(data);
        free(exp);
    }
    fclose(lst);
    printf("%s: %d из %d\n", fails ? "НЕ ПРОЙДЕН" : "ПРОЙДЕН", cases - fails, cases);
    return fails ? 1 : 0;
}
