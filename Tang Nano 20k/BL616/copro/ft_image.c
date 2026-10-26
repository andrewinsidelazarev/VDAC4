/* SPDX-License-Identifier: Apache-2.0 */
/*
 * CMD_LOADIMAGE: JPEG baseline и PNG 8 бит из кольца — по кускам, как приходят (как
 * CMD_INFLATE). Выход — байты подряд с ptr (палитра PNG, затем точки), формат и размеры —
 * ftc_img_get_info (дисплей-лист и CMD_GETPROPS — в ft_copro.c).
 *
 * Форматы — как у FT812 (замер bt8xxemu, fpga/vdac2/scripts/ft_probe_loadimage.py): JPEG —
 * всегда RGB565 (и серый; OPT_MONO — L8); PNG: серый — L8, RGB — RGB565, RGBA — ARGB4,
 * палитра — PALETTED565 (с tRNS — PALETTED4444), палитра с ptr, точки за ней. Каналы
 * усекаются (младшие разряды отбрасываются).
 *
 * JPEG считается как libjpeg (решение пользователя 2026-10-20: «если libjpg делает лучше —
 * пусть он будет эталоном»; bt8xxemu не сглаживает цветность 4:2:0/4:2:2): обратное ДКП
 * целочисленное islow (jidctint.c), «гладкое» восстановление цветности h2v1/h2v2/h1v2
 * (jdsample.c), YCbCr → RGB таблицами jdcolor.c. Проверка — test/image_test.c против
 * Pillow (libjpeg-turbo) бит в бит.
 *
 * JPEG: заголовок — побайтно; сжатые данные — в буфер jb (до JB_BYTES), MCU декодируется,
 * когда в буфере не меньше JB_NEED байтов или пришёл EOI (худшая MCU 4:2:0 со вставками
 * 0xFF00 — ~2,5 КБ). Строк MCU в памяти две: выводится прошлая, когда у нынешней есть первая
 * строка цветности (нижний сосед для «гладкого» восстановления по вертикали). Плоскости —
 * в общей области arena по размерам картинки; не помещается — ошибка.
 *
 * PNG: блоки — побайтно, IDAT — в tinfl (ft_inflate.c, состояние — c->inflate), строки —
 * снятие фильтров (Paeth и прочие) и перевод в формат FT812.
 */
#include <stddef.h>
#include <string.h>

#include "ft_copro.h"

#define JB_BYTES  8192u           /* буфер сжатых данных JPEG */
#define JB_NEED   3072u           /* столько — на одну MCU с запасом */
#define IMG_WMAX  2048u           /* ширина картинки — до 2047 (поле BITMAP_SIZE_H) */
#define ARENA     (80u * 1024u)   /* плоскости двух строк MCU и строки вывода */

enum { FMT_ARGB4 = 6, FMT_RGB565 = 7, FMT_L8 = 3, FMT_PAL565 = 14, FMT_PAL4444 = 15 };
enum { OPT_MONO = 1, OPT_NODL = 2 };

/* --------------------------------------------------------------------------------------- */
/* JPEG                                                                                    */
/* --------------------------------------------------------------------------------------- */

typedef struct {
    uint8_t  look_len[512];      /* 9 разрядов вперёд: длина кода (0 — длиннее 9) */
    uint8_t  look_val[512];
    int32_t  maxcode[18];        /* как в libjpeg (jdhuff.c) */
    int32_t  valoff[18];
    uint8_t  vals[256];
    int      ok;
} huff_t;

typedef struct {
    int      id, h, v, tq, td, ta;   /* номер, выборка, таблицы квантования, DC, AC */
    int      pred;               /* DC предыдущего блока */
    uint32_t bw, rows;           /* плоскость строки MCU: ширина (кратна 8) и строк */
    uint32_t dw, dh;             /* размер в своей выборке (libjpeg downsampled_*) */
} jcomp_t;

typedef struct {
    int      state;
    uint32_t opts;
    /* заголовок */
    int      marker, seg_len, seg_got;
    uint8_t  seg[1200];          /* DHT (4 таблицы — до 1092), DQT, SOF, SOS, DRI */
    uint16_t qt[4][64];          /* в естественном порядке */
    huff_t   hdc[2], hac[2];
    jcomp_t  comp[3];
    int      ncomp, hmax, vmax;
    uint32_t w, h, mcux, mcuy;   /* MCU по горизонтали и вертикали */
    int      restart, rst_left;
    /* сжатые данные */
    uint32_t jb_r, jb_n;         /* читаем с jb_r, данных до jb_n */
    int      eoi, prev_ff;
    uint32_t bits;               /* буфер разрядов, старшие — первые */
    int      nbits;
    int      hit_marker;
    /* строки MCU: в rows[cur] идёт декодирование, rows[cur ^ 1] — ждёт вывода */
    uint32_t mcu_x, mcu_y;       /* следующая MCU */
    int      cur;
    uint8_t *pl[2][3];           /* плоскости в arena */
    uint8_t *above[3];           /* последняя строка цветности прошлой выведенной строки MCU */
    uint8_t *cbuf[2];            /* восстановленная во всю ширину цветность строки вывода */
    uint8_t *out;                /* строка вывода (RGB565 или L8) */
    uint8_t  jb[JB_BYTES];
    _Alignas(4) uint8_t arena[ARENA];
} jpeg_t;

/* --------------------------------------------------------------------------------------- */
/* PNG                                                                                     */
/* --------------------------------------------------------------------------------------- */

typedef struct {
    int      state;
    uint32_t opts;
    uint8_t  hdr[8];
    uint32_t hdr_got;
    uint32_t len, type, got;     /* блок: длина, тип, принято */
    uint8_t  ihdr[13];
    uint32_t w, h;
    int      ctype, bpp, rowb;   /* тип цвета, байтов на точку, байтов в строке */
    uint8_t  plte[768];
    uint32_t npal;
    uint8_t  trns[256];
    uint32_t ntrns;
    int      started;            /* палитра выведена, идут строки */
    uint8_t *infl;               /* состояние tinfl (c->inflate) */
    int      infl_done;
    int      lcur;
    uint32_t lgot;               /* байтов нынешней строки */
    uint32_t y;
    void   (*emit)(void *u, const uint8_t *p, size_t n);
    void    *eu;
    int      err;
    uint8_t  line[2][IMG_WMAX * 4 + 1];   /* фильтр + строка: нынешняя и прошлая */
    uint8_t  out[IMG_WMAX * 2];
} png_t;

typedef struct {
    int kind;                    /* 0 — не известно, 1 — JPEG, 2 — PNG */
    ftc_img_info_t info;
    union {
        jpeg_t j;
        png_t  p;
    } u;
} img_t;

_Static_assert(sizeof(img_t) <= FTC_IMG_BYTES, "FTC_IMG_BYTES");

size_t ftc_img_state_size(void)
{
    return sizeof(img_t);
}

/* ---- JPEG: таблицы ---------------------------------------------------------------------- */

static const uint8_t zz[64 + 16] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
    63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63,   /* k > 63 — как libjpeg */
};

static int build_huff(huff_t *t, const uint8_t *counts, const uint8_t *vals, int nvals)
{
    uint16_t code = 0;
    int k = 0;
    memset(t->look_len, 0, sizeof t->look_len);
    memcpy(t->vals, vals, (size_t)nvals);
    for (int l = 1; l <= 16; l++) {
        t->valoff[l] = k - code;
        if (counts[l - 1]) {
            for (int i = 0; i < counts[l - 1]; i++, k++, code++) {
                if (l <= 9) {
                    int sh = 9 - l;
                    for (int f = 0; f < (1 << sh); f++) {
                        t->look_len[(code << sh) | f] = (uint8_t)l;
                        t->look_val[(code << sh) | f] = vals[k];
                    }
                }
            }
            t->maxcode[l] = code - 1;
        } else {
            t->maxcode[l] = -1;
        }
        if (code > (1u << l))
            return 0;
        code <<= 1;
    }
    t->maxcode[17] = 0x7FFFFFFF;
    t->ok = 1;
    return 1;
}

/* ---- JPEG: разряды ---------------------------------------------------------------------- */

/* Подкачать байт в буфер разрядов; на маркере — нули (как libjpeg). */
static void fill(jpeg_t *j)
{
    while (j->nbits <= 24) {
        uint32_t b = 0;
        if (!j->hit_marker && j->jb_r < j->jb_n) {
            b = j->jb[j->jb_r];
            if (b == 0xFF) {
                uint8_t nx = j->jb_r + 1 < j->jb_n ? j->jb[j->jb_r + 1] : 0xD9;
                if (nx == 0x00) {
                    j->jb_r += 2;
                } else {
                    j->hit_marker = 1;          /* RSTn или EOI: дальше нули, маркер не берём */
                    b = 0;
                }
            } else {
                j->jb_r++;
            }
        }
        j->bits |= b << (24 - j->nbits);
        j->nbits += 8;
    }
}

static inline uint32_t peek(jpeg_t *j, int n)
{
    if (j->nbits < n)
        fill(j);
    return j->bits >> (32 - n);
}

static inline void skip(jpeg_t *j, int n)
{
    j->bits <<= n;
    j->nbits -= n;
}

static inline int getb(jpeg_t *j, int n)
{
    if (!n)
        return 0;
    uint32_t v = peek(j, n);
    skip(j, n);
    return (int)v;
}

static inline int extend(int v, int s)
{
    return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
}

static int decode(jpeg_t *j, const huff_t *t)
{
    uint32_t p = peek(j, 9);
    int l = t->look_len[p];
    if (l) {
        skip(j, l);
        return t->look_val[p];
    }
    uint32_t code = peek(j, 16);
    for (l = 10; l <= 16; l++) {
        int32_t c = (int32_t)(code >> (16 - l));
        if (c <= t->maxcode[l]) {
            skip(j, l);
            return t->vals[t->valoff[l] + c];
        }
    }
    skip(j, 16);                 /* испорченный поток: как libjpeg — значение 0 */
    return 0;
}

/* ---- JPEG: обратное ДКП (jidctint.c, islow) --------------------------------------------- */

#define CONST_BITS 13
#define PASS1_BITS 2
#define FIX_0_298631336 2446
#define FIX_0_390180644 3196
#define FIX_0_541196100 4433
#define FIX_0_765366865 6270
#define FIX_0_899976223 7373
#define FIX_1_175875602 9633
#define FIX_1_501321110 12299
#define FIX_1_847759065 15137
#define FIX_1_961570560 16069
#define FIX_2_053119869 16819
#define FIX_2_562915447 20995
#define FIX_3_072711026 25172
#define DESCALE(x, n) (((x) + (1 << ((n) - 1))) >> (n))

/* Таблица после ДКП: (значение & 1023) → отсчёт (как prepare_range_limit_table) */
static uint8_t idct_lim[1024];
static uint8_t clamp_tab[256 * 3];     /* sample_range_limit: [-256, 512) → 0…255 */
static int     cr_r[256], cb_b[256];
static int32_t cr_g[256], cb_g[256];
static int     tabs_ready;

static void tables(void)
{
    if (tabs_ready)
        return;
    for (int m = 0; m < 1024; m++) {
        int s = m < 512 ? m : m - 1024;
        int v = s + 128;
        idct_lim[m] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    /* «очень далеко за пределами»: 128…511 → 255, −512…−129 → 0 — уже так */
    for (int i = 0; i < 768; i++) {
        int v = i - 256;
        clamp_tab[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    for (int i = 0, x = -128; i < 256; i++, x++) {
        cr_r[i] = (int)((91881 * (int32_t)x + 32768) >> 16);          /* FIX(1.40200) */
        cb_b[i] = (int)((116130 * (int32_t)x + 32768) >> 16);         /* FIX(1.77200) */
        cr_g[i] = -46802 * (int32_t)x;                                /* FIX(0.71414) */
        cb_g[i] = -22554 * (int32_t)x + 32768;                        /* FIX(0.34414) */
    }
    tabs_ready = 1;
}

static void idct(const int16_t *in, const uint16_t *q, uint8_t *out, uint32_t stride)
{
    int32_t ws[64];
    for (int c = 0; c < 8; c++) {
        const int16_t *ip = in + c;
        const uint16_t *qp = q + c;
        int32_t *wp = ws + c;
        if (!ip[8] && !ip[16] && !ip[24] && !ip[32] && !ip[40] && !ip[48] && !ip[56]) {
            int32_t dc = ((int32_t)ip[0] * qp[0]) << PASS1_BITS;
            for (int r = 0; r < 8; r++)
                wp[8 * r] = dc;
            continue;
        }
        int32_t z2 = (int32_t)ip[16] * qp[16], z3 = (int32_t)ip[48] * qp[48];
        int32_t z1 = (z2 + z3) * FIX_0_541196100;
        int32_t tmp2 = z1 + z3 * (-FIX_1_847759065);
        int32_t tmp3 = z1 + z2 * FIX_0_765366865;
        z2 = (int32_t)ip[0] * qp[0];
        z3 = (int32_t)ip[32] * qp[32];
        int32_t tmp0 = (z2 + z3) << CONST_BITS, tmp1 = (z2 - z3) << CONST_BITS;
        int32_t tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3, tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
        tmp0 = (int32_t)ip[56] * qp[56];
        tmp1 = (int32_t)ip[40] * qp[40];
        tmp2 = (int32_t)ip[24] * qp[24];
        tmp3 = (int32_t)ip[8] * qp[8];
        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        int32_t z4 = tmp1 + tmp3;
        int32_t z5 = (z3 + z4) * FIX_1_175875602;
        tmp0 *= FIX_0_298631336;
        tmp1 *= FIX_2_053119869;
        tmp2 *= FIX_3_072711026;
        tmp3 *= FIX_1_501321110;
        z1 *= -FIX_0_899976223;
        z2 *= -FIX_2_562915447;
        z3 *= -FIX_1_961570560;
        z4 *= -FIX_0_390180644;
        z3 += z5;
        z4 += z5;
        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;
        wp[0]  = DESCALE(tmp10 + tmp3, CONST_BITS - PASS1_BITS);
        wp[56] = DESCALE(tmp10 - tmp3, CONST_BITS - PASS1_BITS);
        wp[8]  = DESCALE(tmp11 + tmp2, CONST_BITS - PASS1_BITS);
        wp[48] = DESCALE(tmp11 - tmp2, CONST_BITS - PASS1_BITS);
        wp[16] = DESCALE(tmp12 + tmp1, CONST_BITS - PASS1_BITS);
        wp[40] = DESCALE(tmp12 - tmp1, CONST_BITS - PASS1_BITS);
        wp[24] = DESCALE(tmp13 + tmp0, CONST_BITS - PASS1_BITS);
        wp[32] = DESCALE(tmp13 - tmp0, CONST_BITS - PASS1_BITS);
    }
    for (int r = 0; r < 8; r++) {
        const int32_t *wp = ws + 8 * r;
        uint8_t *op = out + r * stride;
        if (!wp[1] && !wp[2] && !wp[3] && !wp[4] && !wp[5] && !wp[6] && !wp[7]) {
            uint8_t v = idct_lim[DESCALE(wp[0], PASS1_BITS + 3) & 1023];
            for (int k = 0; k < 8; k++)
                op[k] = v;
            continue;
        }
        int32_t z2 = wp[2], z3 = wp[6];
        int32_t z1 = (z2 + z3) * FIX_0_541196100;
        int32_t tmp2 = z1 + z3 * (-FIX_1_847759065);
        int32_t tmp3 = z1 + z2 * FIX_0_765366865;
        int32_t tmp0 = (wp[0] + wp[4]) << CONST_BITS, tmp1 = (wp[0] - wp[4]) << CONST_BITS;
        int32_t tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3, tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
        tmp0 = wp[7];
        tmp1 = wp[5];
        tmp2 = wp[3];
        tmp3 = wp[1];
        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        int32_t z4 = tmp1 + tmp3;
        int32_t z5 = (z3 + z4) * FIX_1_175875602;
        tmp0 *= FIX_0_298631336;
        tmp1 *= FIX_2_053119869;
        tmp2 *= FIX_3_072711026;
        tmp3 *= FIX_1_501321110;
        z1 *= -FIX_0_899976223;
        z2 *= -FIX_2_562915447;
        z3 *= -FIX_1_961570560;
        z4 *= -FIX_0_390180644;
        z3 += z5;
        z4 += z5;
        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;
        const int sh = CONST_BITS + PASS1_BITS + 3;
        op[0] = idct_lim[DESCALE(tmp10 + tmp3, sh) & 1023];
        op[7] = idct_lim[DESCALE(tmp10 - tmp3, sh) & 1023];
        op[1] = idct_lim[DESCALE(tmp11 + tmp2, sh) & 1023];
        op[6] = idct_lim[DESCALE(tmp11 - tmp2, sh) & 1023];
        op[2] = idct_lim[DESCALE(tmp12 + tmp1, sh) & 1023];
        op[5] = idct_lim[DESCALE(tmp12 - tmp1, sh) & 1023];
        op[3] = idct_lim[DESCALE(tmp13 + tmp0, sh) & 1023];
        op[4] = idct_lim[DESCALE(tmp13 - tmp0, sh) & 1023];
    }
}

/* ---- JPEG: заголовок -------------------------------------------------------------------- */

enum { J_SOI0, J_SOI1, J_FF, J_MARK, J_LEN0, J_LEN1, J_BODY, J_SKIP, J_SCAN, J_DONE, J_ERR };

static uint8_t *plane(jpeg_t *j, int row, int ci, uint32_t r)
{
    return j->pl[row][ci] + r * j->comp[ci].bw;
}

/* Плоскости двух строк MCU, соседи сверху, строки восстановления и вывода — в arena */
static int layout(jpeg_t *j)
{
    uint8_t *a = j->arena, *end = j->arena + ARENA;
    for (int row = 0; row < 2; row++)
        for (int ci = 0; ci < j->ncomp; ci++) {
            j->pl[row][ci] = a;
            a += j->comp[ci].bw * j->comp[ci].rows;
        }
    for (int ci = 1; ci < j->ncomp; ci++) {
        j->above[ci] = a;
        a += j->comp[ci].bw;
    }
    for (int k = 0; k < 2; k++) {
        j->cbuf[k] = a;
        a += j->mcux * 8 * j->hmax + 16;
    }
    j->out = a;
    a += 2 * j->w + 4;
    return a <= end;
}

static int seg_sof(jpeg_t *j, const uint8_t *s, int n)
{
    if (n < 6 || s[0] != 8)
        return 0;
    j->h = ((uint32_t)s[1] << 8) | s[2];
    j->w = ((uint32_t)s[3] << 8) | s[4];
    j->ncomp = s[5];
    if (!j->w || !j->h || j->w >= IMG_WMAX || j->h >= 2048 || (j->ncomp != 1 && j->ncomp != 3) ||
        n < 6 + 3 * j->ncomp)
        return 0;
    j->hmax = j->vmax = 1;
    for (int i = 0; i < j->ncomp; i++) {
        jcomp_t *c = &j->comp[i];
        c->id = s[6 + 3 * i];
        c->h = s[7 + 3 * i] >> 4;
        c->v = s[7 + 3 * i] & 15;
        c->tq = s[8 + 3 * i] & 3;
        if (c->h < 1 || c->h > 2 || c->v < 1 || c->v > 2)
            return 0;
        if (c->h > j->hmax) j->hmax = c->h;
        if (c->v > j->vmax) j->vmax = c->v;
    }
    if (j->ncomp == 1)
        j->comp[0].h = j->comp[0].v = j->hmax = j->vmax = 1;   /* один канал — блоками 8×8 */
    else if (j->comp[1].h != j->comp[2].h || j->comp[1].v != j->comp[2].v ||
             j->comp[0].h != j->hmax || j->comp[0].v != j->vmax ||
             (j->comp[1].h == j->hmax && j->comp[1].v == j->vmax && j->hmax > 1))
        return 0;      /* цветность — одинаковая и не больше яркости; 2×2 у всех — не нужно */
    j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
    j->mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
    for (int i = 0; i < j->ncomp; i++) {
        jcomp_t *c = &j->comp[i];
        c->bw = j->mcux * c->h * 8;
        c->rows = 8 * c->v;
        c->dw = (j->w * c->h + j->hmax - 1) / j->hmax;
        c->dh = (j->h * c->v + j->vmax - 1) / j->vmax;
    }
    return layout(j);
}

static int seg_dht(jpeg_t *j, const uint8_t *s, int n)
{
    while (n > 0) {
        if (n < 17)
            return 0;
        int tc = s[0] >> 4, th = s[0] & 15, cnt = 0;
        for (int i = 0; i < 16; i++)
            cnt += s[1 + i];
        if (tc > 1 || th > 1 || cnt > 256 || n < 17 + cnt)
            return 0;
        if (!build_huff(tc ? &j->hac[th] : &j->hdc[th], s + 1, s + 17, cnt))
            return 0;
        s += 17 + cnt;
        n -= 17 + cnt;
    }
    return 1;
}

static int seg_dqt(jpeg_t *j, const uint8_t *s, int n)
{
    while (n > 0) {
        int pq = s[0] >> 4, tq = s[0] & 15;
        if (tq > 3 || pq > 1 || n < 1 + 64 * (pq + 1))
            return 0;
        for (int i = 0; i < 64; i++)
            j->qt[tq][zz[i]] = pq ? (uint16_t)((s[1 + 2 * i] << 8) | s[2 + 2 * i]) : s[1 + i];
        s += 1 + 64 * (pq + 1);
        n -= 1 + 64 * (pq + 1);
    }
    return 1;
}

static int seg_sos(jpeg_t *j, const uint8_t *s, int n)
{
    if (n < 1 || s[0] != j->ncomp || n < 4 + 2 * s[0])
        return 0;                                    /* одна развёртка, все каналы */
    for (int i = 0; i < j->ncomp; i++) {
        if (s[1 + 2 * i] != j->comp[i].id)
            return 0;                                /* порядок — как в SOF */
        j->comp[i].td = s[2 + 2 * i] >> 4;
        j->comp[i].ta = s[2 + 2 * i] & 15;
        if (j->comp[i].td > 1 || j->comp[i].ta > 1 || !j->hdc[j->comp[i].td].ok ||
            !j->hac[j->comp[i].ta].ok)
            return 0;
        j->comp[i].pred = 0;
    }
    int p = 1 + 2 * j->ncomp;
    if (s[p] != 0 || s[p + 1] != 63 || s[p + 2] != 0)
        return 0;                                    /* baseline: Ss 0, Se 63, Ah/Al 0 */
    j->rst_left = j->restart;
    j->mcu_x = j->mcu_y = 0;
    j->cur = 0;
    return 1;
}

/* ---- JPEG: вывод строк ------------------------------------------------------------------ */

static void put_row(jpeg_t *j, const uint8_t *yrow, const uint8_t *cb, const uint8_t *cr,
                    void (*emit)(void *, const uint8_t *, size_t), void *u)
{
    uint8_t *o = j->out;
    if (j->opts & OPT_MONO) {
        emit(u, yrow, j->w);
        return;
    }
    for (uint32_t x = 0; x < j->w; x++) {
        int y = yrow[x], r, g, b;
        if (cb) {
            r = clamp_tab[256 + y + cr_r[cr[x]]];
            g = clamp_tab[256 + y + (int)((cb_g[cb[x]] + cr_g[cr[x]]) >> 16)];
            b = clamp_tab[256 + y + cb_b[cb[x]]];
        } else {
            r = g = b = y;
        }
        uint32_t v = ((uint32_t)(r >> 3) << 11) | ((uint32_t)(g >> 2) << 5) | (uint32_t)(b >> 3);
        o[2 * x] = (uint8_t)v;
        o[2 * x + 1] = (uint8_t)(v >> 8);
    }
    emit(u, o, 2 * j->w);
}

/* По горизонтали во всю ширину: h2 — h2v1_fancy_upsample, иначе как есть */
static void up_h(const jcomp_t *c, int h2, const uint8_t *in, uint8_t *out)
{
    uint32_t n = c->dw;
    if (!h2) {
        memcpy(out, in, n);
        return;
    }
    /* ширина 1 — как libjpeg: сосед справа — отсчёт запаса блока за краем */
    int v = in[0];
    out[0] = (uint8_t)v;
    out[1] = (uint8_t)((v * 3 + in[1] + 2) >> 2);
    uint32_t k = 2;
    for (uint32_t i = 1; i + 1 < n; i++) {
        v = in[i] * 3;
        out[k++] = (uint8_t)((v + in[i - 1] + 1) >> 2);
        out[k++] = (uint8_t)((v + in[i + 1] + 2) >> 2);
    }
    v = in[n - 1];
    out[k++] = (uint8_t)((v * 3 + in[n - 2] + 1) >> 2);
    out[k] = (uint8_t)v;
}

/* h2v2_fancy_upsample: строка вывода из строки mid и соседа nb (сверху или снизу) */
static void up_h2v2(const jcomp_t *c, const uint8_t *mid, const uint8_t *nb, uint8_t *out)
{
    uint32_t n = c->dw;
    /* ширина 1 — как libjpeg: второй столбец — запас блока за краем */
    int this_s = mid[0] * 3 + nb[0];
    int next_s = mid[1] * 3 + nb[1];
    out[0] = (uint8_t)((this_s * 4 + 8) >> 4);
    out[1] = (uint8_t)((this_s * 3 + next_s + 7) >> 4);
    int last_s = this_s;
    this_s = next_s;
    uint32_t k = 2;
    for (uint32_t i = 2; i < n; i++) {
        next_s = mid[i] * 3 + nb[i];
        out[k++] = (uint8_t)((this_s * 3 + last_s + 8) >> 4);
        out[k++] = (uint8_t)((this_s * 3 + next_s + 7) >> 4);
        last_s = this_s;
        this_s = next_s;
    }
    out[k++] = (uint8_t)((this_s * 3 + last_s + 8) >> 4);
    out[k] = (uint8_t)((this_s * 4 + 7) >> 4);
}

/* h1v2_fancy_upsample (libjpeg-turbo) */
static void up_h1v2(const jcomp_t *c, const uint8_t *mid, const uint8_t *nb, int lower, uint8_t *out)
{
    int bias = lower ? 2 : 1;
    for (uint32_t i = 0; i < c->dw; i++)
        out[i] = (uint8_t)((mid[i] * 3 + nb[i] + bias) >> 2);
}

/* Строки картинки строки MCU mr из rows[row]. Соседи цветности по вертикали: сверху —
   above (последняя строка прошлой строки MCU), у первой строки картинки — она сама; снизу —
   первая строка rows[row ^ 1] (next), за последней настоящей строкой — она сама (libjpeg). */
static void emit_mcu_row(jpeg_t *j, int row, uint32_t mr, int next,
                         void (*emit)(void *, const uint8_t *, size_t), void *u)
{
    uint32_t ys = 8 * j->vmax, y0 = mr * ys;
    for (uint32_t ry = 0; ry < ys && y0 + ry < j->h; ry++) {
        const uint8_t *yrow = plane(j, row, 0, ry);
        if (j->ncomp == 1) {
            put_row(j, yrow, NULL, NULL, emit, u);
            continue;
        }
        const jcomp_t *cc = &j->comp[1];
        int h2 = cc->h < j->hmax, v2 = cc->v < j->vmax;
        for (int ci = 1; ci <= 2; ci++) {
            uint8_t *dst = j->cbuf[ci - 1];
            if (h2 && cc->dw <= 2) {
                /* libjpeg: «гладко» по h2 — только при ширине цветности больше 2, иначе
                   повтор отсчёта (h2v1_upsample / h2v2_upsample, по вертикали — тоже повтор) */
                const uint8_t *in = plane(j, row, ci, v2 ? ry >> 1 : ry);
                for (uint32_t i = 0; i < cc->dw; i++)
                    dst[2 * i] = dst[2 * i + 1] = in[i];
                continue;
            }
            if (!v2) {
                up_h(cc, h2, plane(j, row, ci, ry), dst);
                continue;
            }
            uint32_t r = ry >> 1, gr = mr * 8 + r;
            int lower = ry & 1;
            const uint8_t *mid = plane(j, row, ci, r), *nb;
            if (!lower)
                nb = r ? plane(j, row, ci, r - 1) : (mr ? j->above[ci] : mid);
            else if (gr + 1 >= cc->dh)
                nb = mid;
            else if (r + 1 < 8)
                nb = plane(j, row, ci, r + 1);
            else
                nb = next ? plane(j, row ^ 1, ci, 0) : mid;
            if (h2)
                up_h2v2(cc, mid, nb, dst);
            else
                up_h1v2(cc, mid, nb, lower, dst);
        }
        put_row(j, yrow, j->cbuf[0], j->cbuf[1], emit, u);
    }
    for (int ci = 1; ci < j->ncomp; ci++)
        memcpy(j->above[ci], plane(j, row, ci, j->comp[ci].rows - 1), j->comp[ci].bw);
}

/* ---- JPEG: MCU -------------------------------------------------------------------------- */

static int jb_ready(const jpeg_t *j)
{
    return j->eoi || j->jb_n - j->jb_r >= JB_NEED;
}

/* Конец интервала RST: разряды до байта — прочь, маркер RSTn — взять, DC — с нуля. */
static int restart(jpeg_t *j)
{
    j->bits = 0;
    j->nbits = 0;
    j->hit_marker = 0;
    while (j->jb_r + 1 < j->jb_n &&
           !(j->jb[j->jb_r] == 0xFF && j->jb[j->jb_r + 1] != 0 && j->jb[j->jb_r + 1] != 0xFF))
        j->jb_r++;
    if (j->jb_r + 1 >= j->jb_n)
        return 0;
    uint8_t m = j->jb[j->jb_r + 1];
    if (m >= 0xD0 && m <= 0xD7)
        j->jb_r += 2;
    for (int i = 0; i < j->ncomp; i++)
        j->comp[i].pred = 0;
    j->rst_left = j->restart;
    return 1;
}

static void decode_mcu(jpeg_t *j)
{
    int16_t blk[64];
    for (int ci = 0; ci < j->ncomp; ci++) {
        jcomp_t *c = &j->comp[ci];
        const huff_t *dc = &j->hdc[c->td], *ac = &j->hac[c->ta];
        for (int by = 0; by < c->v; by++)
            for (int bx = 0; bx < c->h; bx++) {
                memset(blk, 0, sizeof blk);
                int s = decode(j, dc);
                if (s)
                    s = extend(getb(j, s), s);
                c->pred += s;
                blk[0] = (int16_t)c->pred;
                for (int k = 1; k < 64; k++) {
                    int rs = decode(j, ac), r = rs >> 4;
                    s = rs & 15;
                    if (s) {
                        k += r;
                        blk[zz[k]] = (int16_t)extend(getb(j, s), s);
                    } else {
                        if (r != 15)
                            break;
                        k += 15;
                    }
                }
                uint32_t x = (j->mcu_x * c->h + (uint32_t)bx) * 8;
                idct(blk, j->qt[c->tq], plane(j, j->cur, ci, (uint32_t)by * 8) + x, c->bw);
            }
    }
}

/* 1 — картинка готова, 0 — ждать данных, -1 — ошибка */
static int jpeg_scan(jpeg_t *j, void (*emit)(void *, const uint8_t *, size_t), void *u)
{
    while (jb_ready(j)) {
        if (j->restart && j->rst_left == 0) {
            if (!restart(j))
                return j->eoi ? -1 : 0;
        }
        decode_mcu(j);
        if (j->restart)
            j->rst_left--;
        if (++j->mcu_x == j->mcux) {
            j->mcu_x = 0;
            if (j->mcu_y)                        /* прошлая строка: нижний сосед теперь есть */
                emit_mcu_row(j, j->cur ^ 1, j->mcu_y - 1, 1, emit, u);
            if (++j->mcu_y == j->mcuy) {
                emit_mcu_row(j, j->cur, j->mcu_y - 1, 0, emit, u);
                return 1;
            }
            j->cur ^= 1;
        }
    }
    return 0;
}

static int jpeg_run(img_t *im, const uint8_t *in, size_t n, size_t *used,
                    void (*emit)(void *, const uint8_t *, size_t), void *u)
{
    jpeg_t *j = &im->u.j;
    size_t i = 0;
    while (j->state != J_DONE && j->state != J_ERR) {
        if (j->state == J_SCAN) {
            /* сжатые данные — в буфер, до EOI включительно; дальше — не наши */
            if (j->jb_r > JB_BYTES / 2) {
                memmove(j->jb, j->jb + j->jb_r, j->jb_n - j->jb_r);
                j->jb_n -= j->jb_r;
                j->jb_r = 0;
            }
            while (i < n && !j->eoi && j->jb_n < JB_BYTES) {
                uint8_t b = in[i++];
                j->jb[j->jb_n++] = b;
                if (j->prev_ff && b == 0xD9)
                    j->eoi = 1;
                j->prev_ff = b == 0xFF;
            }
            int r = jpeg_scan(j, emit, u);
            if (r > 0)
                j->state = J_DONE;
            else if (r < 0 || j->eoi)            /* EOI раньше конца картинки */
                j->state = J_ERR;
            else if (i >= n)
                break;
            continue;
        }
        if (i >= n)
            break;
        uint8_t b = in[i++];
        switch (j->state) {
        case J_SOI0: j->state = b == 0xFF ? J_SOI1 : J_ERR; break;
        case J_SOI1: j->state = b == 0xD8 ? J_FF : J_ERR; break;
        case J_FF:   j->state = b == 0xFF ? J_MARK : J_ERR; break;
        case J_MARK:
            if (b == 0xFF)
                break;                             /* заполняющие 0xFF */
            j->marker = b;
            if (b == 0xD9)
                j->state = J_ERR;                  /* EOI до картинки */
            else if (b == 0xD8 || (b >= 0xD0 && b <= 0xD7) || b == 0x01)
                j->state = J_FF;                   /* маркеры без длины */
            else
                j->state = J_LEN0;
            break;
        case J_LEN0: j->seg_len = b << 8; j->state = J_LEN1; break;
        case J_LEN1: {
            int m = j->marker;
            j->seg_len = (j->seg_len | b) - 2;
            j->seg_got = 0;
            int keep = m == 0xC0 || m == 0xC1 || m == 0xC4 || m == 0xDB || m == 0xDA || m == 0xDD;
            if (j->seg_len < 0 || (m >= 0xC2 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) ||
                (keep && j->seg_len > (int)sizeof j->seg)) {
                j->state = J_ERR;                  /* progressive, lossless, арифметика */
                break;
            }
            j->state = keep ? J_BODY : J_SKIP;
            if (j->seg_len == 0)
                j->state = keep ? J_ERR : J_FF;
            break;
        }
        case J_SKIP:
            if (++j->seg_got >= j->seg_len)
                j->state = J_FF;
            break;
        case J_BODY: {
            j->seg[j->seg_got++] = b;
            if (j->seg_got < j->seg_len)
                break;
            int ok = 1, m = j->marker;
            j->state = J_FF;
            if (m == 0xC0 || m == 0xC1)
                ok = !j->w && seg_sof(j, j->seg, j->seg_len);
            else if (m == 0xC4)
                ok = seg_dht(j, j->seg, j->seg_len);
            else if (m == 0xDB)
                ok = seg_dqt(j, j->seg, j->seg_len);
            else if (m == 0xDD)
                j->restart = j->seg_len >= 2 ? (j->seg[0] << 8) | j->seg[1] : 0;
            else if (m == 0xDA) {
                ok = j->w && seg_sos(j, j->seg, j->seg_len);
                if (ok) {
                    j->state = J_SCAN;
                    im->info.fmt = (j->opts & OPT_MONO) ? FMT_L8 : FMT_RGB565;
                    im->info.w = j->w;
                    im->info.h = j->h;
                    im->info.stride = (j->opts & OPT_MONO) ? j->w : 2 * j->w;
                }
            }
            if (!ok)
                j->state = J_ERR;
            break;
        }
        default:
            break;
        }
    }
    *used = i;
    if (j->state == J_ERR)
        return FTC_IMG_ERROR;
    return j->state == J_DONE ? FTC_IMG_DONE : FTC_IMG_NEED_INPUT;
}

/* --------------------------------------------------------------------------------------- */
/* PNG                                                                                     */
/* --------------------------------------------------------------------------------------- */

enum { P_SIG, P_LEN, P_TYPE, P_DATA, P_CRC, P_DONE, P_ERR };

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
}

/* Готовая строка (без байта фильтра) → формат FT812 */
static void png_row(png_t *p, const uint8_t *s)
{
    uint8_t *o = p->out;
    uint32_t n = 0;
    switch (p->ctype) {
    case 0:                                         /* L8 */
    case 3:                                         /* индексы */
        p->emit(p->eu, s, p->w);
        return;
    case 2:
        for (uint32_t x = 0; x < p->w; x++, s += 3) {
            uint32_t v = ((uint32_t)(s[0] >> 3) << 11) | ((uint32_t)(s[1] >> 2) << 5) | (s[2] >> 3);
            o[n++] = (uint8_t)v;
            o[n++] = (uint8_t)(v >> 8);
        }
        break;
    case 6:
        for (uint32_t x = 0; x < p->w; x++, s += 4) {
            uint32_t v = ((uint32_t)(s[3] >> 4) << 12) | ((uint32_t)(s[0] >> 4) << 8) |
                         ((uint32_t)(s[1] >> 4) << 4) | (s[2] >> 4);
            o[n++] = (uint8_t)v;
            o[n++] = (uint8_t)(v >> 8);
        }
        break;
    default:
        return;
    }
    p->emit(p->eu, o, n);
}

/* Палитра — первой, с ptr: 565 или (есть tRNS) 4444 */
static void png_start(png_t *p)
{
    if (p->started)
        return;
    p->started = 1;
    if (p->ctype != 3)
        return;
    uint8_t buf[512];
    for (uint32_t i = 0; i < p->npal; i++) {
        const uint8_t *e = p->plte + 3 * i;
        uint32_t v;
        if (p->ntrns) {
            uint32_t a = i < p->ntrns ? p->trns[i] : 255;
            v = ((a >> 4) << 12) | ((uint32_t)(e[0] >> 4) << 8) | ((uint32_t)(e[1] >> 4) << 4) | (e[2] >> 4);
        } else {
            v = ((uint32_t)(e[0] >> 3) << 11) | ((uint32_t)(e[1] >> 2) << 5) | (e[2] >> 3);
        }
        buf[2 * i] = (uint8_t)v;
        buf[2 * i + 1] = (uint8_t)(v >> 8);
    }
    p->emit(p->eu, buf, 2 * p->npal);
}

/* Выход tinfl: байты строк с фильтром */
static void png_inflated(void *u, const uint8_t *d, size_t n)
{
    png_t *p = (png_t *)u;
    while (n && !p->err && p->y < p->h) {
        uint8_t *cur = p->line[p->lcur];
        size_t k = (size_t)(p->rowb + 1) - p->lgot;
        if (k > n)
            k = n;
        memcpy(cur + p->lgot, d, k);
        p->lgot += (uint32_t)k;
        d += k;
        n -= k;
        if (p->lgot < (uint32_t)p->rowb + 1)
            continue;
        const uint8_t *prev = p->line[p->lcur ^ 1] + 1;
        uint8_t *s = cur + 1;
        int f = cur[0], bpp = p->bpp;
        if (p->y == 0)
            prev = NULL;
        for (int i = 0; i < p->rowb; i++) {
            int a = i >= bpp ? s[i - bpp] : 0, b = prev ? prev[i] : 0, c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            switch (f) {
            case 0: break;
            case 1: s[i] = (uint8_t)(s[i] + a); break;
            case 2: s[i] = (uint8_t)(s[i] + b); break;
            case 3: s[i] = (uint8_t)(s[i] + ((a + b) >> 1)); break;
            case 4: s[i] = (uint8_t)(s[i] + paeth(a, b, c)); break;
            default: p->err = 1; return;
            }
        }
        png_row(p, s);
        p->y++;
        p->lcur ^= 1;
        p->lgot = 0;
    }
}

static int png_run(img_t *im, const uint8_t *in, size_t n, size_t *used,
                   void (*emit)(void *, const uint8_t *, size_t), void *u)
{
    png_t *p = &im->u.p;
    size_t i = 0;
    p->emit = emit;
    p->eu = u;
    while (i < n && p->state != P_DONE && p->state != P_ERR) {
        switch (p->state) {
        case P_SIG:
            p->hdr[p->hdr_got++] = in[i++];
            if (p->hdr_got == 8) {
                p->state = memcmp(p->hdr, "\x89PNG\r\n\x1A\n", 8) ? P_ERR : P_LEN;
                p->hdr_got = 0;
            }
            break;
        case P_LEN:
        case P_TYPE:
            p->hdr[p->hdr_got++] = in[i++];
            if (p->hdr_got == 4) {
                p->hdr_got = 0;
                if (p->state == P_LEN) {
                    p->len = be32(p->hdr);
                    p->state = P_TYPE;
                } else {
                    p->type = be32(p->hdr);
                    p->got = 0;
                    p->state = p->len ? P_DATA : P_CRC;
                    if (p->type == 0x49444154u && !p->w)          /* IDAT до IHDR */
                        p->state = P_ERR;
                    if (p->type == 0x49444154u && p->state != P_ERR)
                        png_start(p);
                    if (p->type == 0x49454E44u)                   /* IEND */
                        p->state = (p->y == p->h && p->w) ? P_CRC : P_ERR;
                }
            }
            break;
        case P_DATA: {
            size_t k = n - i;
            if (k > p->len - p->got)
                k = p->len - p->got;
            const uint8_t *d = in + i;
            if (p->type == 0x49444154u) {                         /* IDAT */
                size_t done = 0;
                while (done < k && !p->infl_done) {
                    size_t u2 = 0;
                    int r = ftc_inflate_run(p->infl, d + done, k - done, &u2, png_inflated, p);
                    done += u2;
                    if (r == FTC_INF_ERROR || p->err) {
                        p->state = P_ERR;
                        break;
                    }
                    if (r == FTC_INF_DONE)
                        p->infl_done = 1;
                    if (u2 == 0)
                        break;
                }
                if (p->state == P_ERR)
                    break;
            } else {
                for (size_t q = 0; q < k; q++) {
                    uint32_t at = p->got + (uint32_t)q;
                    if (p->type == 0x49484452u && at < 13) p->ihdr[at] = d[q];
                    else if (p->type == 0x504C5445u && at < 768) p->plte[at] = d[q];
                    else if (p->type == 0x74524E53u && at < 256) p->trns[at] = d[q];
                }
            }
            p->got += (uint32_t)k;
            i += k;
            if (p->got == p->len)
                p->state = P_CRC;
            break;
        }
        case P_CRC:
            p->hdr[p->hdr_got++] = in[i++];
            if (p->hdr_got < 4)
                break;
            p->hdr_got = 0;
            p->state = P_LEN;
            if (p->type == 0x49484452u) {                         /* IHDR */
                p->w = be32(p->ihdr);
                p->h = be32(p->ihdr + 4);
                p->ctype = p->ihdr[9];
                int ok = p->ihdr[8] == 8 && !p->ihdr[10] && !p->ihdr[11] && !p->ihdr[12] &&
                         p->w && p->h && p->w < IMG_WMAX && p->h < 2048 &&
                         (p->ctype == 0 || p->ctype == 2 || p->ctype == 3 || p->ctype == 6);
                if (!ok) {
                    p->state = P_ERR;
                    break;
                }
                p->bpp = p->ctype == 2 ? 3 : p->ctype == 6 ? 4 : 1;
                p->rowb = (int)p->w * p->bpp;
                im->info.w = p->w;
                im->info.h = p->h;
            } else if (p->type == 0x504C5445u) {
                p->npal = p->len / 3 > 256 ? 256 : p->len / 3;
            } else if (p->type == 0x74524E53u) {
                p->ntrns = p->len > 256 ? 256 : p->len;
            } else if (p->type == 0x49454E44u) {
                p->state = P_DONE;
            }
            if (p->type == 0x49444154u || p->type == 0x49454E44u) {
                uint32_t fmt = p->ctype == 0 ? FMT_L8 : p->ctype == 2 ? FMT_RGB565 :
                               p->ctype == 6 ? FMT_ARGB4 : p->ntrns ? FMT_PAL4444 : FMT_PAL565;
                im->info.fmt = fmt;
                im->info.stride = p->ctype == 0 || p->ctype == 3 ? p->w : 2 * p->w;
                im->info.pal_bytes = p->ctype == 3 ? 2 * p->npal : 0;
            }
            break;
        default:
            break;
        }
    }
    *used = i;
    if (p->state == P_ERR)
        return FTC_IMG_ERROR;
    return p->state == P_DONE ? FTC_IMG_DONE : FTC_IMG_NEED_INPUT;
}

/* --------------------------------------------------------------------------------------- */

void ftc_img_begin(void *st, uint32_t opts, void *inflate_state)
{
    img_t *im = (img_t *)st;
    tables();
    im->kind = 0;
    memset(&im->info, 0, sizeof im->info);
    memset(&im->u.j, 0, offsetof(jpeg_t, jb));
    memset(&im->u.p, 0, offsetof(png_t, line));
    im->u.j.opts = opts;
    im->u.p.opts = opts;
    im->u.p.infl = (uint8_t *)inflate_state;
}

int ftc_img_run(void *st, const uint8_t *in, size_t n, size_t *used,
                void (*emit)(void *u, const uint8_t *p, size_t n), void *u)
{
    img_t *im = (img_t *)st;
    *used = 0;
    if (!im->kind) {
        if (!n)
            return FTC_IMG_NEED_INPUT;
        if (in[0] == 0xFF) {
            jpeg_t *j = &im->u.j;
            uint32_t opts = j->opts;
            memset(j, 0, offsetof(jpeg_t, jb));
            j->opts = opts;
            j->state = J_SOI0;
            im->kind = 1;
            im->info.jpeg = 1;
        } else if (in[0] == 0x89) {
            png_t *p = &im->u.p;
            uint32_t opts = p->opts;
            uint8_t *infl = p->infl;
            memset(p, 0, offsetof(png_t, line));
            p->opts = opts;
            p->infl = infl;
            p->state = P_SIG;
            ftc_inflate_begin(infl);
            im->kind = 2;
        } else {
            return FTC_IMG_ERROR;
        }
    }
    return im->kind == 1 ? jpeg_run(im, in, n, used, emit, u) : png_run(im, in, n, used, emit, u);
}

void ftc_img_get_info(const void *st, ftc_img_info_t *info)
{
    *info = ((const img_t *)st)->info;
}
