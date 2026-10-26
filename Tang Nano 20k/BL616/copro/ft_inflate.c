/* SPDX-License-Identifier: Apache-2.0 */
/*
 * CMD_INFLATE: поток zlib из кольца, по кускам — как приходит. Распаковка — tinfl из miniz
 * 3.1.2 (third_party/miniz, лицензия MIT) в кольцевой словарь 32 КБ; каждый вызов tinfl
 * отдаёт непрерывный кусок словаря, он сразу уходит в RAM_G.
 */
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_ZLIB_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_MALLOC

#include "third_party/miniz/miniz_tinfl.c"

#include "ft_copro.h"

typedef struct {
    tinfl_decompressor d;
    mz_uint8 dict[TINFL_LZ_DICT_SIZE];
    size_t pos;                 /* куда пойдёт следующий выход */
    int done;
} inflate_state_t;

_Static_assert(sizeof(inflate_state_t) <= FTC_INFLATE_BYTES, "FTC_INFLATE_BYTES");

size_t ftc_inflate_state_size(void)
{
    return sizeof(inflate_state_t);
}

void ftc_inflate_begin(void *st)
{
    inflate_state_t *s = (inflate_state_t *)st;
    tinfl_init(&s->d);
    s->pos = 0;
    s->done = 0;
}

int ftc_inflate_run(void *st, const uint8_t *in, size_t n, size_t *used,
                    void (*out)(void *u, const uint8_t *p, size_t n), void *u)
{
    inflate_state_t *s = (inflate_state_t *)st;
    *used = 0;
    if (s->done)
        return FTC_INF_DONE;
    for (;;) {
        size_t in_n = n - *used;
        size_t out_n = TINFL_LZ_DICT_SIZE - s->pos;
        tinfl_status r = tinfl_decompress(&s->d, in + *used, &in_n, s->dict, s->dict + s->pos, &out_n,
                                          TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT);
        *used += in_n;
        if (out_n)
            out(u, s->dict + s->pos, out_n);
        s->pos = (s->pos + out_n) & (TINFL_LZ_DICT_SIZE - 1);
        if (r < TINFL_STATUS_DONE)
            return FTC_INF_ERROR;
        if (r == TINFL_STATUS_DONE) {
            s->done = 1;
            return FTC_INF_DONE;
        }
        if (r == TINFL_STATUS_NEEDS_MORE_INPUT && (*used == n || (in_n == 0 && out_n == 0)))
            return FTC_INF_NEED_INPUT;
        /* HAS_MORE_OUTPUT — словарь дошёл до конца, дальше с начала */
    }
}
