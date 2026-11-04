/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Проверка ft_image.c (CMD_LOADIMAGE) на ПК: картинки data/image (make_image_data.py) — кусками
 * разной длины, как из кольца; выход — байт в байт с ожидаемым (JPEG — libjpeg, PNG — FT812).
 *
 *   image_test DIR      (DIR/list.txt: файл, ожидание, fmt, w, h, stride, байтов палитры[, opts])
 *
 * Ожидание — файл или «crc:XXXXXXXX:длина» (CRC32 вывода: холст вписывания — 1,5 МБ); opts —
 * параметр CMD_LOADIMAGE в шестнадцатеричном виде (нет — 0). Расхождение с CRC — вывод
 * в DIR/<файл>.got для разбора.
 *
 *   image_test --dump DIR LIST OUT   (прошивка 39: картинки задания, сверка — test_task_images.py)
 *
 * LIST: файл метка opts max_w max_h room (room 0 и max_w 0 — CMD_LOADIMAGE, иначе
 * CMD_LOADIMAGE_BOX). Каждая картинка — целиком и кусками 1…7 и до 4096 байт: выход и итог
 * обязаны совпасть. Выход — OUT/<файл>.<метка>.got, итог — строка «case …» (код, причина,
 * формат, размеры, байтов, время). Черновик в RAM_G — массив room байтов за выходом: стенд
 * проверяет, что разбор пишет в него только в пределах room и не поверх выхода.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* Черновик в RAM_G (рамка): массив room байтов с ptr; min_off — откуда писал разбор */
static uint8_t *draft_mem;
static uint32_t draft_room, draft_min, draft_bad;

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

static void d_wr(void *u, uint32_t off, const uint8_t *p, size_t n)
{
    (void)u;
    if ((off & 3u) || (n & 3u) || off + n > draft_room) {
        draft_bad++;
        return;
    }
    if (off < draft_min)
        draft_min = off;
    memcpy(draft_mem + off, p, n);
}

static void d_rd(void *u, uint32_t off, uint8_t *p, size_t n)
{
    (void)u;
    if ((off & 3u) || (n & 3u) || off + n > draft_room) {
        draft_bad++;
        memset(p, 0, n);
        return;
    }
    memcpy(p, draft_mem + off, n);
}

/* Состояние и inflate — подряд, как в рабочей памяти сопроцессора (FTC_INFLATE_MEM) */
static _Alignas(8) uint8_t scratch[FTC_IMG_BYTES + FTC_INFLATE_BYTES];
#define img_state  scratch
#define infl_state (scratch + FTC_IMG_BYTES)

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* chunk: 0 — всё сразу, иначе куски 1…chunk (псевдослучайно). box_w = 0 — CMD_LOADIMAGE. */
static int busy_calls;
static int run_box(const uint8_t *data, size_t n, size_t chunk, uint32_t opts, uint32_t box_w, uint32_t box_h,
                   uint32_t room, sink_t *s, ftc_img_info_t *info, size_t *consumed)
{
    unsigned seed = 12345;
    ftc_img_req_t rq = {opts, box_w != 0, box_w, box_h, room, d_wr, d_rd};
    free(draft_mem);
    draft_mem = calloc(1, room + 4);
    draft_room = room;
    draft_min = 0xFFFFFFFFu;
    draft_bad = 0;
    busy_calls = 0;
    ftc_img_begin_req(img_state, &rq, infl_state);
    size_t at = 0;
    int r = FTC_IMG_NEED_INPUT;
    while (r == FTC_IMG_NEED_INPUT || r == FTC_IMG_BUSY) {
        int was_busy = r == FTC_IMG_BUSY;
        if (was_busy)
            busy_calls++;
        size_t k = n - at;
        if (chunk && k) {
            seed = seed * 1103515245u + 12345u;
            size_t c = 1 + (seed >> 8) % chunk;
            if (k > c)
                k = c;
        }
        size_t used = 0;
        r = ftc_img_run(img_state, data + at, k, &used, emit, s);
        if (was_busy && used)
            draft_bad |= 0x80000000u;            /* в работе без данных данные не берутся */
        at += used;
        if (r == FTC_IMG_NEED_INPUT && used == 0 && at >= n)
            break;
    }
    ftc_img_get_info(img_state, info);
    *consumed = at;
    return r;
}

static int run_case(const uint8_t *data, size_t n, size_t chunk, uint32_t opts, sink_t *s,
                    ftc_img_info_t *info, size_t *consumed)
{
    return run_box(data, n, chunk, opts, 0, 0, 0, s, info, consumed);
}

/* --dump: картинки задания — выход в файлы, итог строкой; куски — тот же выход */
static int dump_main(const char *dir, const char *list, const char *out)
{
    char path[1024], line[512];
    FILE *lst = fopen(list, "r");
    if (!lst) {
        fprintf(stderr, "нет %s\n", list);
        return 2;
    }
    int fails = 0;
    while (fgets(line, sizeof line, lst)) {
        char file[256], tag[64];
        unsigned opts = 0, bw = 0, bh = 0, room = 0;
        if (sscanf(line, "%255s %63s %x %u %u %u", file, tag, &opts, &bw, &bh, &room) != 6)
            continue;
        size_t n;
        snprintf(path, sizeof path, "%s/%s", dir, file);
        uint8_t *data = load(path, &n);
        if (!data) {
            printf("case %s %s нет файла\n", file, tag);
            fails++;
            continue;
        }
        size_t padded = (n + 3) & ~(size_t)3;
        memset(data + n, 0, padded - n);
        static const size_t chunks[] = {0, 7, 4096};
        sink_t ref = {0};
        ftc_img_info_t rinfo;
        int rr = 0, bad = 0, busy0 = 0;
        double ms = 0;
        uint32_t dmin = 0, dbad = 0;
        for (size_t ci = 0; ci < sizeof chunks / sizeof chunks[0]; ci++) {
            sink_t s = {0};
            ftc_img_info_t info;
            size_t consumed = 0;
            clock_t t0 = clock();
            int r = run_box(data, padded, chunks[ci], opts, bw, bh, room ? room : (bw ? 0u : 0u), &s, &info,
                            &consumed);
            double t = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
            if (ci == 0) {
                ref = s;
                rinfo = info;
                rr = r;
                ms = t;
                busy0 = busy_calls;
                dmin = draft_min;
                dbad = draft_bad;
                continue;
            }
            if (r == FTC_IMG_ERROR && rr == FTC_IMG_ERROR) {
                /* ошибка: кусками — та же причина (выход до неё зависит от кусков) */
                if (info.err != rinfo.err)
                    bad = 1;
            } else if (r != rr || s.n != ref.n || (s.n && memcmp(s.buf, ref.buf, s.n)) ||
                       memcmp(&info, &rinfo, sizeof info) || draft_bad) {
                bad = 1;
            }
            free(s.buf);
        }
        /* черновик — не поверх выхода и в пределах room */
        if (dbad || (dmin != 0xFFFFFFFFu && dmin < ref.n))
            bad = 1;
        snprintf(path, sizeof path, "%s/%s.%s.got", out, file, tag);
        FILE *g = fopen(path, "wb");
        if (g) {
            if (ref.n)
                fwrite(ref.buf, 1, ref.n, g);
            fclose(g);
        }
        printf("case %s %s r=%d err=%u arg=%u fmt=%u w=%u h=%u stride=%u pal=%u alpha=%u src=%ux%u bytes=%u "
               "busy=%d draft=%d ms=%.1f chunks=%s\n",
               file, tag, rr, rinfo.err, rinfo.err_arg, rinfo.fmt, rinfo.w, rinfo.h, rinfo.stride,
               rinfo.pal_bytes, rinfo.alpha, rinfo.src_w, rinfo.src_h, (unsigned)ref.n, busy0,
               dmin != 0xFFFFFFFFu, ms, bad ? "НЕТ" : "да");
        if (bad)
            fails++;
        free(ref.buf);
        free(data);
    }
    fclose(lst);
    printf("dump: %s\n", fails ? "куски или черновик расходятся" : "куски сходятся");
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "--dump"))
        return dump_main(argv[2], argv[3], argv[4]);
    if (argc < 2) {
        fprintf(stderr, "image_test DIR\n       image_test --dump DIR LIST OUT\n");
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
        unsigned fmt, w, h, stride, pal, opts = 0, ecrc = 0;
        int nf = sscanf(line, "%255s %255s %u %u %u %u %u %x", file, expf, &fmt, &w, &h, &stride, &pal, &opts);
        if (nf < 7)
            continue;
        size_t n, en = 0;
        snprintf(path, sizeof path, "%s/%s", argv[1], file);
        uint8_t *data = load(path, &n);
        uint8_t *exp = NULL;
        int by_crc = !strncmp(expf, "crc:", 4);
        if (by_crc) {
            unsigned elen = 0;
            if (sscanf(expf + 4, "%x:%u", &ecrc, &elen) == 2) {
                en = elen;
                exp = malloc(1);
            }
        } else {
            snprintf(path, sizeof path, "%s/%s", argv[1], expf);
            exp = load(path, &en);
        }
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
            int r = run_case(data, padded, chunks[ci], opts, &s, &info, &consumed);
            cases++;
            size_t diff = 0, first = (size_t)-1;
            if (by_crc) {
                if (s.n != en || crc32(s.buf, s.n) != ecrc) {
                    diff = 1;
                    if (ci == 0) {
                        snprintf(path, sizeof path, "%s/%s.got", argv[1], file);
                        FILE *g = fopen(path, "wb");
                        if (g) {
                            fwrite(s.buf, 1, s.n, g);
                            fclose(g);
                        }
                    }
                }
            } else {
                for (size_t i = 0; i < en && i < s.n; i++)
                    if (s.buf[i] != exp[i]) {
                        if (first == (size_t)-1)
                            first = i;
                        diff++;
                    }
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
