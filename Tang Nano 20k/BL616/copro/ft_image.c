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
 *
 * OPT_FULLSCREEN (8, расширение VDAC4; у FT812 такой флаг есть только у CMD_PLAYVIDEO) —
 * вписать картинку в холст 1024×768, см. «Вписывание» ниже.
 */
#include <stddef.h>
#include <string.h>

#include "ft_copro.h"

#define JB_BYTES  8192u           /* буфер сжатых данных JPEG */
#define JB_NEED   3072u           /* столько — на одну MCU с запасом */
#define IMG_WMAX  2048u           /* ширина картинки — до 2047 (поле BITMAP_SIZE_H) */
#define ARENA     (80u * 1024u)   /* плоскости строки MCU, строки восстановления и вывода */
#define FIT_W     1024u           /* OPT_FULLSCREEN: холст */
#define FIT_H     768u
#define FIT_KMAX  9               /* отсчётов фильтра на точку (уменьшение до 4 раз) */

enum { FMT_ARGB4 = 6, FMT_RGB565 = 7, FMT_L8 = 3, FMT_PAL565 = 14, FMT_PAL4444 = 15 };
enum { OPT_MONO = 1, OPT_NODL = 2, OPT_FULLSCREEN = 8 };
#ifdef FTC_IMG_TEST
#define OPT_TEST_DRAFT 0x80000000u   /* только стенд: JPEG с уменьшением ДКП, без вписывания */
#endif

/* Холст 1024×768 (у стенда с OPT_TEST_DRAFT — только уменьшение ДКП, вывод как есть) */
static int fit_on(uint32_t opts)
{
#ifdef FTC_IMG_TEST
    if (opts & OPT_TEST_DRAFT)
        return 0;
#endif
    return (opts & OPT_FULLSCREEN) != 0;
}

/* --------------------------------------------------------------------------------------- */
/* Вписывание (OPT_FULLSCREEN)                                                             */
/* --------------------------------------------------------------------------------------- */

typedef struct {
    uint32_t iw, ih;              /* источник (у JPEG — после уменьшения ДКП) */
    uint32_t ow, oh, x0, y0;      /* вписанная картинка и её место на холсте */
    int      hk;                  /* шаг таблицы весов hkk (отсчётов на точку, не меньше) */
    int      vfirst;              /* сначала по y (Image.resize: высота > ширины · 100) */
    uint32_t ring, rstride;       /* строк в кольце, байтов на строку */
    uint32_t got, yy;             /* принято строк источника, выведено строк картинки */
    uint32_t vmin, vn;            /* окно строки yy в источнике */
    int32_t  vkk[FIT_KMAX];       /* её веса */
    uint16_t *hmin;               /* по точкам: первая точка окна, */
    uint8_t  *hn;                 /* точек в окне */
    int32_t  *hkk;                /* и веса */
    uint8_t  *rows;               /* кольцо строк после прохода по x (vfirst — до него) */
    uint8_t  *src;                /* строка источника RGB: iw × 3 */
    uint8_t  *line;               /* строка после прохода по y */
    uint8_t  *out;                /* строка холста RGB565 (поля — нули) */
} fit_t;

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
    int      ss;                 /* сторона блока после ДКП: 8, у уменьшенного — 4, 2, 1 */
    uint32_t bw, rows;           /* плоскость строки MCU: ширина (кратна ss) и строк */
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
    int      m;                  /* сторона блока яркости после ДКП (libjpeg min_DCT_scaled_size) */
    uint32_t ow, oh;             /* размер на выходе ДКП (output_width/height) */
    int      h2, v2;             /* цветность вдвое уже / ниже яркости после ДКП */
    int      hrep, vfancy;       /* по x — повтор, а не «гладко»; по y — «гладко» (нужен сосед) */
    fit_t   *fit;                /* OPT_FULLSCREEN */
    int      restart, rst_left;
    /* сжатые данные */
    uint32_t jb_r, jb_n;         /* читаем с jb_r, данных до jb_n */
    int      eoi, prev_ff;
    uint32_t bits;               /* буфер разрядов, старшие — первые */
    int      nbits;
    int      hit_marker;
    /* Строка MCU — одна: выводится, как только декодирована. «Гладкой» цветности по
       вертикали (h2v2, h1v2) последняя строка яркости ждёт первую строку цветности следующей
       строки MCU: она откладывается (hold_y, её цветность — above) до конца следующей. */
    uint32_t mcu_x, mcu_y;       /* следующая MCU */
    uint8_t *pl[3];              /* плоскости в arena */
    uint8_t *above[3];           /* последняя строка цветности прошлой строки MCU */
    uint8_t *hold_y;             /* отложенная строка яркости */
    int      held;
    uint8_t *cbuf[2];            /* восстановленная во всю ширину цветность строки вывода */
    uint8_t *out;                /* строка вывода (RGB565 или L8) */
    uint8_t *free_at;            /* arena свободна отсюда */
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
    fit_t   *fit;                /* OPT_FULLSCREEN */
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
    uint8_t *infl;               /* память inflate (FTC_INFLATE_BYTES): у JPEG свободна */
    fit_t    fit;
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

/* ---- Вписывание ------------------------------------------------------------------------ */
/*
 * Картинка w×h уменьшается или растягивается с сохранением пропорций до ow×oh — широкая во
 * всю ширину 1024, узкая во всю высоту 768, другая сторона округлением — и встаёт по центру
 * холста RGB565 1024×768 с чёрными полями. Выход — весь холст подряд с ptr (1,5 МБ: RAM_G
 * VDAC4 — 2 МБ); дисплей-лист и CMD_GETPROPS — как у картинки 1024×768 RGB565.
 *
 * Эталон — Pillow 12 (решение пользователя 2026-10-26: «автомасштаб до 1024*768 в bl616»):
 * Image.resize(BILINEAR) — «треугольник», при уменьшении растянутый на коэффициент
 * уменьшения (сглаживание); веса — в double, затем 22-битные целые; сначала проход по x в
 * 8-битные строки, потом по y (resample.c: precompute_coeffs, normalize_coeffs_8bpc,
 * ImagingResampleHorizontal/Vertical_8bpc). Большой JPEG до этого уменьшается при ДКП в 2, 4
 * или 8 раз, как Image.draft (libjpeg-turbo scale_denom): наибольшее s из 8, 4, 2, 1, не
 * большее min(w / ow, h / oh) нацело. Прозрачность PNG (RGBA, палитра с tRNS) — наложение на
 * чёрный: c·a/255 с округлением (DIV255 Pillow). Каналы в RGB565 — усечением, как у FT812.
 *
 * Строки источника идут по одной: проход по x — сразу в кольцо, по y — когда пришли все
 * строки окна выходной строки (окна сдвигаются только вниз, кольцо — на самое длинное окно).
 * Image.resize Pillow 12 у источника выше ширины более чем в 100 раз, когда высота
 * уменьшается, делает проходы наоборот — сначала по y, потом по x (два вызова self.im.resize);
 * здесь — так же (vfirst). Ядро Pillow — docs/references/pillow-resample-20261026/Resample.c.
 */

static void fit_size(uint32_t w, uint32_t h, uint32_t *ow, uint32_t *oh)
{
    if (w * FIT_H >= h * FIT_W) {
        *ow = FIT_W;
        *oh = (h * FIT_W + w / 2) / w;
    } else {
        *oh = FIT_H;
        *ow = (w * FIT_H + h / 2) / h;
    }
    if (!*ow)
        *ow = 1;
    if (!*oh)
        *oh = 1;
}

/* Наибольшее число отсчётов в окне (ksize в precompute_coeffs); 0 — больше FIT_KMAX */
static int fit_ksize(uint32_t in, uint32_t out)
{
    double scale = (double)(float)in / out, support = scale < 1.0 ? 1.0 : scale;
    int k = (int)support;
    if ((double)k < support)
        k++;
    k = 2 * k + 1;
    return k <= FIT_KMAX ? k : 0;
}

/* Окно и веса отсчёта xx (precompute_coeffs + normalize_coeffs_8bpc, BILINEAR, поддержка 1).
   Порядок действий в double — как у Pillow, иначе последний разряд весов уйдёт. */
static uint32_t fit_coeffs(uint32_t in, uint32_t out, uint32_t xx, int32_t *k, uint32_t *xmin)
{
    double scale = (double)(float)in / out, fs = scale < 1.0 ? 1.0 : scale, support = fs;
    double center = 0.0 + (xx + 0.5) * scale, ss = 1.0 / fs, ww = 0.0, w[FIT_KMAX];
    int lo = (int)(center - support + 0.5), hi = (int)(center + support + 0.5);
    if (lo < 0)
        lo = 0;
    if (hi > (int)in)
        hi = (int)in;
    int n = hi - lo;
    for (int x = 0; x < n; x++) {
        double t = (x + lo - center + 0.5) * ss;
        if (t < 0.0)
            t = -t;
        w[x] = t < 1.0 ? 1.0 - t : 0.0;
        ww += w[x];
    }
    for (int x = 0; x < n; x++) {
        double v = ww != 0.0 ? w[x] / ww : w[x];
        k[x] = (int32_t)(v < 0 ? -0.5 + v * (1 << 22) : 0.5 + v * (1 << 22));
    }
    *xmin = (uint32_t)lo;
    return (uint32_t)n;
}

typedef struct {
    uint8_t *a, *end;
} span_t;

/* Кусок из первой подходящей свободной области */
static void *take(span_t *sp, int nsp, size_t bytes)
{
    bytes = (bytes + 3u) & ~(size_t)3;
    for (int i = 0; i < nsp; i++)
        if ((size_t)(sp[i].end - sp[i].a) >= bytes) {
            void *p = sp[i].a;
            sp[i].a += bytes;
            return p;
        }
    return NULL;
}

/* Картинка w×h, строки источника iw×ih (у JPEG — после ДКП): размер, веса по x, кольцо.
   0 — не помещается в память или уменьшение больше чем в 4 раза по стороне. */
static int fit_init(fit_t *f, uint32_t w, uint32_t h, uint32_t iw, uint32_t ih, span_t *sp, int nsp)
{
    memset(f, 0, sizeof *f);
    fit_size(w, h, &f->ow, &f->oh);
    f->iw = iw;
    f->ih = ih;
    f->x0 = (FIT_W - f->ow) / 2;
    f->y0 = (FIT_H - f->oh) / 2;
    int hk = iw != f->ow ? fit_ksize(iw, f->ow) : 1, vk = ih != f->oh ? fit_ksize(ih, f->oh) : 1;
    if (!hk || !vk)
        return 0;
    f->hk = hk;
    f->ring = (uint32_t)vk;
    f->vfirst = ih > iw * 100 && f->oh < ih;
    f->rstride = (f->vfirst ? iw : f->ow) * 3u;
    if (iw != f->ow) {
        f->hkk = take(sp, nsp, (size_t)f->ow * (size_t)hk * 4u);
        f->hmin = take(sp, nsp, (size_t)f->ow * 2u);
        f->hn = take(sp, nsp, f->ow);
        if (!f->hkk || !f->hmin || !f->hn)
            return 0;
        for (uint32_t xx = 0; xx < f->ow; xx++) {
            uint32_t lo;
            f->hn[xx] = (uint8_t)fit_coeffs(iw, f->ow, xx, f->hkk + xx * (uint32_t)hk, &lo);
            f->hmin[xx] = (uint16_t)lo;
        }
    }
    f->rows = take(sp, nsp, (size_t)f->ring * f->rstride);
    f->src = take(sp, nsp, (size_t)iw * 3u);
    f->line = take(sp, nsp, (size_t)(iw > f->ow ? iw : f->ow) * 3u);
    f->out = take(sp, nsp, 2u * FIT_W);
    if (!f->rows || !f->src || !f->line || !f->out)
        return 0;
    if (ih != f->oh)
        f->vn = fit_coeffs(ih, f->oh, 0, f->vkk, &f->vmin);
    return 1;
}

/* Верхнее поле холста */
static void fit_start(fit_t *f, void (*emit)(void *, const uint8_t *, size_t), void *u)
{
    memset(f->out, 0, 2u * FIT_W);
    for (uint32_t y = 0; y < f->y0; y++)
        emit(u, f->out, 2u * FIT_W);
}

static inline uint8_t clip8(int32_t v)
{
    if (v >= (1 << 22 << 8))
        return 255;
    if (v <= 0)
        return 0;
    return (uint8_t)(v >> 22);
}

static inline void px565(uint8_t *o, uint32_t r, uint32_t g, uint32_t b)
{
    uint32_t v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    o[0] = (uint8_t)v;
    o[1] = (uint8_t)(v >> 8);
}

/* Проход по x: строка iw → ow (ImagingResampleHorizontal_8bpc) */
static void fit_hpass(const fit_t *f, const uint8_t *src, uint8_t *dst)
{
    for (uint32_t xx = 0; xx < f->ow; xx++) {
        const int32_t *k = f->hkk + xx * (uint32_t)f->hk;
        const uint8_t *s = src + 3u * f->hmin[xx];
        int32_t r = 1 << 21, g = 1 << 21, b = 1 << 21;
        for (uint32_t i = 0; i < f->hn[xx]; i++, s += 3) {
            r += s[0] * k[i];
            g += s[1] * k[i];
            b += s[2] * k[i];
        }
        dst[3 * xx] = clip8(r);
        dst[3 * xx + 1] = clip8(g);
        dst[3 * xx + 2] = clip8(b);
    }
}

/* Проход по y: окно vmin…vmin+vn строк кольца шириной w → dst (ImagingResampleVertical_8bpc) */
static void fit_vpass(const fit_t *f, uint32_t w, uint8_t *dst)
{
    const uint8_t *rp[FIT_KMAX];
    for (uint32_t i = 0; i < f->vn; i++)
        rp[i] = f->rows + ((f->vmin + i) % f->ring) * f->rstride;
    for (uint32_t x = 0; x < 3u * w; x++) {
        int32_t v = 1 << 21;
        for (uint32_t i = 0; i < f->vn; i++)
            v += rp[i][x] * f->vkk[i];
        dst[x] = clip8(v);
    }
}

/* Строка картинки (RGB, ow) — в строку холста и в RAM_G */
static void fit_put(fit_t *f, const uint8_t *rgb, void (*emit)(void *, const uint8_t *, size_t), void *u)
{
    uint8_t *o = f->out + 2u * f->x0;
    for (uint32_t x = 0; x < f->ow; x++, rgb += 3)
        px565(o + 2 * x, rgb[0], rgb[1], rgb[2]);
    emit(u, f->out, 2u * FIT_W);
}

/* Строка источника готова в f->src (RGB): в кольцо (обычно — после прохода по x); готовые
   строки картинки — на холст; после последней — нижнее поле. */
static void fit_row(fit_t *f, void (*emit)(void *, const uint8_t *, size_t), void *u)
{
    if (f->got >= f->ih)
        return;
    uint8_t *slot = f->rows + (f->got % f->ring) * f->rstride;
    if (f->vfirst || f->iw == f->ow)
        memcpy(slot, f->src, f->rstride);
    else
        fit_hpass(f, f->src, slot);
    f->got++;
    if (f->ih == f->oh) {                        /* по y — как есть */
        fit_put(f, slot, emit, u);
        f->yy++;
    } else {
        while (f->yy < f->oh && f->vmin + f->vn <= f->got) {
            const uint8_t *res = f->line;
            fit_vpass(f, f->vfirst ? f->iw : f->ow, f->line);
            if (f->vfirst && f->iw != f->ow) {   /* ow ≤ iw: строка источника свободна */
                fit_hpass(f, f->line, f->src);
                res = f->src;
            }
            fit_put(f, res, emit, u);
            if (++f->yy < f->oh)
                f->vn = fit_coeffs(f->ih, f->oh, f->yy, f->vkk, &f->vmin);
        }
    }
    if (f->yy == f->oh) {                        /* нижнее поле */
        memset(f->out, 0, 2u * FIT_W);
        for (uint32_t y = f->y0 + f->oh; y < FIT_H; y++)
            emit(u, f->out, 2u * FIT_W);
        f->yy++;
    }
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

/* точка YCbCr → RGB565 (ycc_rgb_convert libjpeg, затем 5-6-5 отбрасыванием младших разрядов) */
static inline uint32_t ycc565(int y, int cb, int cr)
{
    uint32_t r = clamp_tab[256 + y + cr_r[cr]];
    uint32_t g = clamp_tab[256 + y + (int)((cb_g[cb] + cr_g[cr]) >> 16)];
    uint32_t b = clamp_tab[256 + y + cb_b[cb]];
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static inline uint32_t gray565(uint32_t y)
{
    return ((y >> 3) << 11) | ((y >> 2) << 5) | (y >> 3);
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

/* ---- JPEG: уменьшенное ДКП (jidctred.c libjpeg-turbo 3.1.4.1) -------------------------- *
 * Блок 8×8 → 4×4, 2×2 или 1×1: средние соседних выходов полного ДКП, упрощённые (LL&M), как
 * у Pillow с Image.draft. Исходник — docs/references/libjpeg-turbo-20261026/jidctred.c;
 * сдвиги влево — через uint32_t (у libjpeg — LEFT_SHIFT). */

#define FIX_0_211164243 1730
#define FIX_0_509795579 4176
#define FIX_0_601344887 4926
#define FIX_0_720959822 5906
#define FIX_0_850430095 6967
#define FIX_1_061594337 8697
#define FIX_1_272758580 10426
#define FIX_1_451774981 11893
#define FIX_2_172734803 17799
#define FIX_3_624509785 29692
#define LSH(x, n) ((int32_t)((uint32_t)(x) << (n)))

static void idct4(const int16_t *in, const uint16_t *q, uint8_t *out, uint32_t stride)
{
    int32_t ws[8 * 4];
    for (int c = 0; c < 8; c++) {
        if (c == 4)                      /* столбец 4 второму проходу не нужен */
            continue;
        const int16_t *ip = in + c;
        const uint16_t *qp = q + c;
        int32_t *wp = ws + c;
        if (!ip[8] && !ip[16] && !ip[24] && !ip[40] && !ip[48] && !ip[56]) {
            int32_t dc = LSH((int32_t)ip[0] * qp[0], PASS1_BITS);
            wp[0] = wp[8] = wp[16] = wp[24] = dc;
            continue;
        }
        int32_t tmp0 = LSH((int32_t)ip[0] * qp[0], CONST_BITS + 1);
        int32_t z2 = (int32_t)ip[16] * qp[16], z3 = (int32_t)ip[48] * qp[48];
        int32_t tmp2 = z2 * FIX_1_847759065 + z3 * -FIX_0_765366865;
        int32_t tmp10 = tmp0 + tmp2, tmp12 = tmp0 - tmp2;
        int32_t z1 = (int32_t)ip[56] * qp[56];
        z2 = (int32_t)ip[40] * qp[40];
        z3 = (int32_t)ip[24] * qp[24];
        int32_t z4 = (int32_t)ip[8] * qp[8];
        tmp0 = z1 * -FIX_0_211164243 + z2 * FIX_1_451774981 + z3 * -FIX_2_172734803 +
               z4 * FIX_1_061594337;
        tmp2 = z1 * -FIX_0_509795579 + z2 * -FIX_0_601344887 + z3 * FIX_0_899976223 +
               z4 * FIX_2_562915447;
        wp[0]  = DESCALE(tmp10 + tmp2, CONST_BITS - PASS1_BITS + 1);
        wp[24] = DESCALE(tmp10 - tmp2, CONST_BITS - PASS1_BITS + 1);
        wp[8]  = DESCALE(tmp12 + tmp0, CONST_BITS - PASS1_BITS + 1);
        wp[16] = DESCALE(tmp12 - tmp0, CONST_BITS - PASS1_BITS + 1);
    }
    for (int r = 0; r < 4; r++) {
        const int32_t *wp = ws + 8 * r;
        uint8_t *op = out + r * stride;
        if (!wp[1] && !wp[2] && !wp[3] && !wp[5] && !wp[6] && !wp[7]) {
            uint8_t v = idct_lim[DESCALE(wp[0], PASS1_BITS + 3) & 1023];
            op[0] = op[1] = op[2] = op[3] = v;
            continue;
        }
        int32_t tmp0 = LSH(wp[0], CONST_BITS + 1);
        int32_t tmp2 = wp[2] * FIX_1_847759065 + wp[6] * -FIX_0_765366865;
        int32_t tmp10 = tmp0 + tmp2, tmp12 = tmp0 - tmp2;
        int32_t z1 = wp[7], z2 = wp[5], z3 = wp[3], z4 = wp[1];
        tmp0 = z1 * -FIX_0_211164243 + z2 * FIX_1_451774981 + z3 * -FIX_2_172734803 +
               z4 * FIX_1_061594337;
        tmp2 = z1 * -FIX_0_509795579 + z2 * -FIX_0_601344887 + z3 * FIX_0_899976223 +
               z4 * FIX_2_562915447;
        const int sh = CONST_BITS + PASS1_BITS + 3 + 1;
        op[0] = idct_lim[DESCALE(tmp10 + tmp2, sh) & 1023];
        op[3] = idct_lim[DESCALE(tmp10 - tmp2, sh) & 1023];
        op[1] = idct_lim[DESCALE(tmp12 + tmp0, sh) & 1023];
        op[2] = idct_lim[DESCALE(tmp12 - tmp0, sh) & 1023];
    }
}

static void idct2(const int16_t *in, const uint16_t *q, uint8_t *out, uint32_t stride)
{
    int32_t ws[8 * 2];
    for (int c = 0; c < 8; c++) {
        if (c == 2 || c == 4 || c == 6)  /* столбцы 2, 4, 6 второму проходу не нужны */
            continue;
        const int16_t *ip = in + c;
        const uint16_t *qp = q + c;
        int32_t *wp = ws + c;
        if (!ip[8] && !ip[24] && !ip[40] && !ip[56]) {
            int32_t dc = LSH((int32_t)ip[0] * qp[0], PASS1_BITS);
            wp[0] = wp[8] = dc;
            continue;
        }
        int32_t tmp10 = LSH((int32_t)ip[0] * qp[0], CONST_BITS + 2);
        int32_t tmp0 = (int32_t)ip[56] * qp[56] * -FIX_0_720959822;
        tmp0 += (int32_t)ip[40] * qp[40] * FIX_0_850430095;
        tmp0 += (int32_t)ip[24] * qp[24] * -FIX_1_272758580;
        tmp0 += (int32_t)ip[8] * qp[8] * FIX_3_624509785;
        wp[0] = DESCALE(tmp10 + tmp0, CONST_BITS - PASS1_BITS + 2);
        wp[8] = DESCALE(tmp10 - tmp0, CONST_BITS - PASS1_BITS + 2);
    }
    for (int r = 0; r < 2; r++) {
        const int32_t *wp = ws + 8 * r;
        uint8_t *op = out + r * stride;
        if (!wp[1] && !wp[3] && !wp[5] && !wp[7]) {
            op[0] = op[1] = idct_lim[DESCALE(wp[0], PASS1_BITS + 3) & 1023];
            continue;
        }
        int32_t tmp10 = LSH(wp[0], CONST_BITS + 2);
        int32_t tmp0 = wp[7] * -FIX_0_720959822 + wp[5] * FIX_0_850430095 +
                       wp[3] * -FIX_1_272758580 + wp[1] * FIX_3_624509785;
        const int sh = CONST_BITS + PASS1_BITS + 3 + 2;
        op[0] = idct_lim[DESCALE(tmp10 + tmp0, sh) & 1023];
        op[1] = idct_lim[DESCALE(tmp10 - tmp0, sh) & 1023];
    }
}

static void idct1(const int16_t *in, const uint16_t *q, uint8_t *out)
{
    out[0] = idct_lim[DESCALE((int32_t)in[0] * q[0], 3) & 1023];
}

/* ---- JPEG: заголовок -------------------------------------------------------------------- */

enum { J_SOI0, J_SOI1, J_FF, J_MARK, J_LEN0, J_LEN1, J_BODY, J_SKIP, J_SCAN, J_DONE, J_ERR };

static uint8_t *plane(jpeg_t *j, int ci, uint32_t r)
{
    return j->pl[ci] + r * j->comp[ci].bw;
}

/* Плоскости строки MCU, соседи сверху, отложенная строка, строки восстановления и вывода — в
   arena; у вписывания строки вывода нет (своя строка RGB в fit_t). */
static int layout(jpeg_t *j)
{
    uint8_t *a = j->arena, *end = j->arena + ARENA;
    /* начала плоскостей и строки вывода — по словам: заливка блока без AC и слова RGB565 */
#define ALIGN4(p) ((uint8_t *)(((uintptr_t)(p) + 3u) & ~(uintptr_t)3))
    for (int ci = 0; ci < j->ncomp; ci++) {
        j->pl[ci] = a = ALIGN4(a);
        a += j->comp[ci].bw * j->comp[ci].rows;
    }
    for (int ci = 1; ci < j->ncomp; ci++) {
        j->above[ci] = a;
        a += j->comp[ci].bw;
    }
    for (int k = 0; k < 2; k++) {
        j->cbuf[k] = a;
        a += j->mcux * (uint32_t)j->m * j->hmax + 16;
    }
    j->hold_y = a;
    if (j->vfancy)
        a += j->ow;
    j->out = a = ALIGN4(a);
    if (!fit_on(j->opts))
        a += 2 * j->ow + 4;
    a = (uint8_t *)(((uintptr_t)a + 3u) & ~(uintptr_t)3);
    j->free_at = a;
    return a <= end;
}

/* Сторона блока компонента после ДКП (jdmaster.c, jpeg_calc_output_dimensions): цветность
   растягивается самим ДКП, пока выходит целое число раз — тогда восстанавливать её не надо. */
static int comp_ss(const jpeg_t *j, const jcomp_t *c)
{
    int ss = j->m;
    while (ss < 8 && (j->hmax * j->m) % (c->h * ss * 2) == 0 && (j->vmax * j->m) % (c->v * ss * 2) == 0)
        ss *= 2;
    return ss;
}

static int seg_sof(jpeg_t *j, const uint8_t *s, int n)
{
    if (n < 6 || s[0] != 8)
        return 0;
    j->h = ((uint32_t)s[1] << 8) | s[2];
    j->w = ((uint32_t)s[3] << 8) | s[4];
    j->ncomp = s[5];
    if (!j->w || !j->h || (j->ncomp != 1 && j->ncomp != 3) || n < 6 + 3 * j->ncomp)
        return 0;
    if (!(j->opts & OPT_FULLSCREEN) && (j->w >= IMG_WMAX || j->h >= 2048))
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
    /* Уменьшение ДКП (только вписывание): как Image.draft(size = вписанный размер) */
    j->m = 8;
    if (j->opts & OPT_FULLSCREEN) {
        uint32_t fw, fh, sc;
        fit_size(j->w, j->h, &fw, &fh);
        sc = j->w / fw < j->h / fh ? j->w / fw : j->h / fh;
        j->m = sc >= 8 ? 1 : sc >= 4 ? 2 : sc >= 2 ? 4 : 8;
    }
    j->ow = (j->w * (uint32_t)j->m + 7) / 8;
    j->oh = (j->h * (uint32_t)j->m + 7) / 8;
    if (j->ow >= IMG_WMAX)
        return 0;
    for (int i = 0; i < j->ncomp; i++) {
        jcomp_t *c = &j->comp[i];
        c->ss = comp_ss(j, c);
        c->bw = j->mcux * (uint32_t)(c->h * c->ss);
        c->rows = (uint32_t)(c->v * c->ss);
        c->dw = (j->w * (uint32_t)(c->h * c->ss) + 8u * j->hmax - 1) / (8u * j->hmax);
        c->dh = (j->h * (uint32_t)(c->v * c->ss) + 8u * j->vmax - 1) / (8u * j->vmax);
    }
    /* Восстановление цветности (jdsample.c, jinit_upsampler): «гладко» — только при стороне
       блока яркости больше 1; по x — ещё и при ширине цветности больше 2 (иначе повтор) */
    j->h2 = j->v2 = j->hrep = j->vfancy = 0;
    if (j->ncomp == 3) {
        const jcomp_t *c = &j->comp[1];
        int fancy = j->m > 1;
        j->h2 = c->h * c->ss < j->hmax * j->m;
        j->v2 = c->v * c->ss < j->vmax * j->m;
        j->hrep = j->h2 && !(fancy && c->dw > 2);
        j->vfancy = j->v2 && fancy && !(j->h2 && c->dw <= 2);
    }
    j->fit = NULL;
    if (!layout(j))
        return 0;
    return 1;
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

/* Стандартные таблицы Хаффмана (ITU T.81, K.3): 0 — яркость, 1 — цветность. Кадры MJPEG (AVI)
   обычно без DHT; libjpeg-turbo тогда берёт эти (jpeg_std_huff_table), как и FT812. Значения —
   из DHT JPEG, который пишет libjpeg Pillow без оптимизации (jcparam.c std_huff_tables). */
static const uint8_t std_dc_bits[2][16] = {
    {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0},
    {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0},
};
static const uint8_t std_dc_vals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const uint8_t std_ac_bits[2][16] = {
    {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125},
    {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 119},
};
static const uint8_t std_ac_vals[2][162] = {
    {0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
     0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
     0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
     0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
     0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
     0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
     0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
     0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
     0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
     0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
     0xf9, 0xfa},
    {0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
     0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
     0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
     0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
     0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
     0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
     0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
     0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
     0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
     0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
     0xf9, 0xfa},
};

static int seg_sos(jpeg_t *j, const uint8_t *s, int n)
{
    if (n < 1 || s[0] != j->ncomp || n < 4 + 2 * s[0])
        return 0;                                    /* одна развёртка, все каналы */
    for (int i = 0; i < j->ncomp; i++) {
        if (s[1 + 2 * i] != j->comp[i].id)
            return 0;                                /* порядок — как в SOF */
        int td = s[2 + 2 * i] >> 4, ta = s[2 + 2 * i] & 15;
        j->comp[i].td = td;
        j->comp[i].ta = ta;
        if (td > 1 || ta > 1)
            return 0;
        if (!j->hdc[td].ok && !build_huff(&j->hdc[td], std_dc_bits[td], std_dc_vals, 12))
            return 0;
        if (!j->hac[ta].ok && !build_huff(&j->hac[ta], std_ac_bits[ta], std_ac_vals[ta], 162))
            return 0;
        j->comp[i].pred = 0;
    }
    int p = 1 + 2 * j->ncomp;
    if (s[p] != 0 || s[p + 1] != 63 || s[p + 2] != 0)
        return 0;                                    /* baseline: Ss 0, Se 63, Ah/Al 0 */
    j->rst_left = j->restart;
    j->mcu_x = j->mcu_y = 0;
    j->held = 0;
    return 1;
}

/* ---- JPEG: вывод строк ------------------------------------------------------------------ */

static void put_row(jpeg_t *j, const uint8_t *yrow, const uint8_t *cb, const uint8_t *cr,
                    void (*emit)(void *, const uint8_t *, size_t), void *u)
{
    uint8_t *o = j->out;
    if (j->fit) {                                /* RGB — во вписывание */
        uint8_t *s = j->fit->src;
        for (uint32_t x = 0; x < j->ow; x++, s += 3) {
            int y = yrow[x];
            if (cb) {
                s[0] = clamp_tab[256 + y + cr_r[cr[x]]];
                s[1] = clamp_tab[256 + y + (int)((cb_g[cb[x]] + cr_g[cr[x]]) >> 16)];
                s[2] = clamp_tab[256 + y + cb_b[cb[x]]];
            } else {
                s[0] = s[1] = s[2] = (uint8_t)y;
            }
        }
        fit_row(j->fit, emit, u);
        return;
    }
    if (j->opts & OPT_MONO) {
        emit(u, yrow, j->ow);
        return;
    }
    /* две точки — одно слово (прошивка 32: побайтная запись была треть времени кадра);
       строка вывода выровнена (layout) */
    uint32_t *o32 = (uint32_t *)o, x = 0;
    if (cb) {
        for (; x + 1 < j->ow; x += 2)
            *o32++ = ycc565(yrow[x], cb[x], cr[x]) | ycc565(yrow[x + 1], cb[x + 1], cr[x + 1]) << 16;
        if (x < j->ow)
            *(uint16_t *)o32 = (uint16_t)ycc565(yrow[x], cb[x], cr[x]);
    } else {
        for (; x + 1 < j->ow; x += 2)
            *o32++ = gray565(yrow[x]) | gray565(yrow[x + 1]) << 16;
        if (x < j->ow)
            *(uint16_t *)o32 = (uint16_t)gray565(yrow[x]);
    }
    emit(u, o, 2 * j->ow);
}

/* 4:2:2 без «гладкости» по y: h2v1_fancy_upsample (как up_h) прямо при переводе цвета, без
   строки восстановления. У отсчёта цветности i — точки 2i и 2i+1: левая — (3·c[i] + c[i−1] + 1)
   / 4, правая — (3·c[i] + c[i+1] + 2) / 4; у краёв — сам отсчёт. Ширина n ≥ 2 (при 1 up_h берёт
   соседа из запаса блока). */
static void put_row_h2(jpeg_t *j, const uint8_t *yrow, const uint8_t *cb, const uint8_t *cr, uint32_t n,
                       void (*emit)(void *, const uint8_t *, size_t), void *u)
{
    uint32_t *o = (uint32_t *)j->out, ow = j->ow;
    for (uint32_t i = 0, x = 0; x < ow; i++, x += 2) {
        int b3 = cb[i] * 3, r3 = cr[i] * 3, b0, r0, b1, r1;
        if (i) {
            b0 = (b3 + cb[i - 1] + 1) >> 2;
            r0 = (r3 + cr[i - 1] + 1) >> 2;
        } else {
            b0 = cb[0];
            r0 = cr[0];
        }
        if (x + 1 == ow) {
            *(uint16_t *)o = (uint16_t)ycc565(yrow[x], b0, r0);
            break;
        }
        if (i + 1 < n) {
            b1 = (b3 + cb[i + 1] + 2) >> 2;
            r1 = (r3 + cr[i + 1] + 2) >> 2;
        } else {
            b1 = cb[i];
            r1 = cr[i];
        }
        *o++ = ycc565(yrow[x], b0, r0) | ycc565(yrow[x + 1], b1, r1) << 16;
    }
    emit(u, j->out, 2 * ow);
}

/* 4:2:0 (h2v2_fancy_upsample, как up_h2v2) прямо при переводе цвета. Сумма столбца s[i] =
   3·mid[i] + nb[i] (nb — сосед по y); у отсчёта i левая точка — (3·s[i] + s[i−1] + 8) / 16,
   правая — (3·s[i] + s[i+1] + 7) / 16, у краёв — (4·s + 8) / 16 и (4·s + 7) / 16. n ≥ 2. */
static void put_row_h2v2(jpeg_t *j, const uint8_t *yrow, const uint8_t *bm, const uint8_t *bn,
                         const uint8_t *rm, const uint8_t *rn, uint32_t n,
                         void (*emit)(void *, const uint8_t *, size_t), void *u)
{
    uint32_t *o = (uint32_t *)j->out, ow = j->ow;
    int bl = 0, rl = 0, bs = bm[0] * 3 + bn[0], rs = rm[0] * 3 + rn[0];
    for (uint32_t i = 0, x = 0; x < ow; i++, x += 2) {
        int b0, r0, b1, r1, bx = 0, rx = 0;
        if (i) {
            b0 = (bs * 3 + bl + 8) >> 4;
            r0 = (rs * 3 + rl + 8) >> 4;
        } else {
            b0 = (bs * 4 + 8) >> 4;
            r0 = (rs * 4 + 8) >> 4;
        }
        if (x + 1 == ow) {
            *(uint16_t *)o = (uint16_t)ycc565(yrow[x], b0, r0);
            break;
        }
        if (i + 1 < n) {
            bx = bm[i + 1] * 3 + bn[i + 1];
            rx = rm[i + 1] * 3 + rn[i + 1];
            b1 = (bs * 3 + bx + 7) >> 4;
            r1 = (rs * 3 + rx + 7) >> 4;
        } else {
            b1 = (bs * 4 + 7) >> 4;
            r1 = (rs * 4 + 7) >> 4;
        }
        *o++ = ycc565(yrow[x], b0, r0) | ycc565(yrow[x + 1], b1, r1) << 16;
        bl = bs;
        rl = rs;
        bs = bx;
        rs = rx;
    }
    emit(u, j->out, 2 * ow);
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

/* Строка ry строки MCU mr, яркость yrow. Соседи цветности по вертикали («гладко»): сверху —
   above (последняя строка прошлой строки MCU), у первой строки картинки — она сама; снизу —
   следующая строка, за последней настоящей строкой — она сама (libjpeg). held — отложенная
   последняя строка прошлой строки MCU: её цветность — above, сосед снизу — первая строка
   цветности нынешней (уже в плоскостях). */
static void put_line(jpeg_t *j, uint32_t mr, uint32_t ry, const uint8_t *yrow, int held,
                     void (*emit)(void *, const uint8_t *, size_t), void *u)
{
    if (j->ncomp == 1) {
        put_row(j, yrow, NULL, NULL, emit, u);
        return;
    }
    const jcomp_t *cc = &j->comp[1];
    const uint8_t *cv[2];
    if (!j->vfancy && j->h2 && !j->hrep && !j->fit && !(j->opts & OPT_MONO) && cc->dw >= 2) {
        uint32_t r = j->v2 ? ry >> 1 : ry;
        put_row_h2(j, yrow, plane(j, 1, r), plane(j, 2, r), cc->dw, emit, u);
        return;
    }
    /* 4:2:0: h2v2_fancy_upsample — тоже при переводе цвета (put_row_h2v2) */
    const int fast2 = j->vfancy && j->h2 && !j->fit && !(j->opts & OPT_MONO) && cc->dw >= 2;
    const uint8_t *mids[2], *nbs[2];
    for (int ci = 1; ci <= 2; ci++) {
        uint8_t *dst = j->cbuf[ci - 1];
        uint32_t r = j->v2 ? ry >> 1 : ry;
        if (!j->vfancy) {
            /* по y — как есть или повтор строки (int_upsample, h2v2_upsample) */
            const uint8_t *in = plane(j, ci, r);
            if (!j->h2) {
                cv[ci - 1] = in;                 /* цветность уже во всю ширину */
                continue;
            }
            if (j->hrep) {
                /* libjpeg: «гладко» по x — только при ширине цветности больше 2 и стороне
                   блока яркости больше 1, иначе повтор отсчёта */
                for (uint32_t i = 0; i < cc->dw; i++)
                    dst[2 * i] = dst[2 * i + 1] = in[i];
            } else {
                up_h(cc, 1, in, dst);
            }
            cv[ci - 1] = dst;
            continue;
        }
        uint32_t gr = mr * cc->rows + r;
        int lower = ry & 1;
        const uint8_t *mid = held ? j->above[ci] : plane(j, ci, r), *nb;
        if (!lower)
            nb = r ? plane(j, ci, r - 1) : (mr ? j->above[ci] : mid);
        else if (gr + 1 >= cc->dh)
            nb = mid;
        else if (held)
            nb = plane(j, ci, 0);
        else
            nb = plane(j, ci, r + 1);
        mids[ci - 1] = mid;
        nbs[ci - 1] = nb;
        if (fast2)
            continue;
        if (j->h2)
            up_h2v2(cc, mid, nb, dst);
        else
            up_h1v2(cc, mid, nb, lower, dst);
        cv[ci - 1] = dst;
    }
    if (fast2)
        put_row_h2v2(j, yrow, mids[0], nbs[0], mids[1], nbs[1], cc->dw, emit, u);
    else
        put_row(j, yrow, cv[0], cv[1], emit, u);
}

/* Строки картинки строки MCU mr — сразу после её декодирования. Сначала — отложенная строка
   прошлой; у «гладкой» по y цветности последняя строка яркости, чей сосед снизу — в следующей
   строке MCU, откладывается. */
static void emit_mcu_row(jpeg_t *j, uint32_t mr, void (*emit)(void *, const uint8_t *, size_t), void *u)
{
    uint32_t ys = (uint32_t)(j->vmax * j->m), y0 = mr * ys;
    if (j->held) {
        put_line(j, mr - 1, ys - 1, j->hold_y, 1, emit, u);
        j->held = 0;
    }
    for (uint32_t ry = 0; ry < ys && y0 + ry < j->oh; ry++) {
        if (j->vfancy && ry == ys - 1 && (mr + 1) * j->comp[1].rows < j->comp[1].dh) {
            memcpy(j->hold_y, plane(j, 0, ry), j->ow);
            j->held = 1;
            break;
        }
        put_line(j, mr, ry, plane(j, 0, ry), 0, emit, u);
    }
    if (j->vfancy)
        for (int ci = 1; ci < j->ncomp; ci++)
            memcpy(j->above[ci], plane(j, ci, j->comp[ci].rows - 1), j->comp[ci].bw);
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
                int any = 0;                     /* есть ли AC (у кода s > 0 значение не 0) */
                for (int k = 1; k < 64; k++) {
                    int rs = decode(j, ac), r = rs >> 4;
                    s = rs & 15;
                    if (s) {
                        k += r;
                        blk[zz[k]] = (int16_t)extend(getb(j, s), s);
                        any = 1;
                    } else {
                        if (r != 15)
                            break;
                        k += 15;
                    }
                }
                uint32_t x = (j->mcu_x * c->h + (uint32_t)bx) * (uint32_t)c->ss;
                uint8_t *o = plane(j, ci, (uint32_t)(by * c->ss)) + x;
                if (!any && c->ss == 8) {
                    /* только DC: idct даёт всем точкам одно значение — проход 1 кладёт
                       DC·q << PASS1_BITS в столбец 0, проход 2 берёт его у каждой строки */
                    int32_t dc = ((int32_t)blk[0] * j->qt[c->tq][0]) << PASS1_BITS;
                    uint32_t v = idct_lim[DESCALE(dc, PASS1_BITS + 3) & 1023] * 0x01010101u;
                    for (int r = 0; r < 8; r++, o += c->bw)
                        ((uint32_t *)o)[0] = ((uint32_t *)o)[1] = v;
                    continue;
                }
                switch (c->ss) {
                case 8:  idct(blk, j->qt[c->tq], o, c->bw); break;
                case 4:  idct4(blk, j->qt[c->tq], o, c->bw); break;
                case 2:  idct2(blk, j->qt[c->tq], o, c->bw); break;
                default: idct1(blk, j->qt[c->tq], o); break;
                }
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
            emit_mcu_row(j, j->mcu_y, emit, u);
            if (++j->mcu_y == j->mcuy)
                return 1;
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
                if (ok && fit_on(j->opts)) {
                    /* вписывание: веса и кольцо — в памяти inflate (JPEG её не занимает)
                       и в хвосте arena; сразу — верхнее поле холста */
                    span_t sp[2] = {{im->infl, im->infl + FTC_INFLATE_BYTES},
                                    {j->free_at, j->arena + ARENA}};
                    ok = im->infl && fit_init(&im->fit, j->w, j->h, j->ow, j->oh, sp, 2);
                    if (ok) {
                        j->fit = &im->fit;
                        j->state = J_SCAN;
                        fit_start(j->fit, emit, u);
                        im->info.fmt = FMT_RGB565;
                        im->info.w = FIT_W;
                        im->info.h = FIT_H;
                        im->info.stride = 2 * FIT_W;
                    }
                } else if (ok) {
                    j->state = J_SCAN;
                    im->info.fmt = (j->opts & OPT_MONO) ? FMT_L8 : FMT_RGB565;
                    im->info.w = j->ow;
                    im->info.h = j->oh;
                    im->info.stride = (j->opts & OPT_MONO) ? j->ow : 2 * j->ow;
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

/* DIV255 Pillow: v/255 с округлением, v ≤ 255·255 */
static inline uint8_t div255(uint32_t v)
{
    v += 128;
    return (uint8_t)((v + (v >> 8)) >> 8);
}

/* Вписывание: строка → RGB; прозрачность — наложение на чёрный (c·a/255) */
static void png_fit_row(png_t *p, const uint8_t *s)
{
    uint8_t *o = p->fit->src;
    for (uint32_t x = 0; x < p->w; x++, o += 3) {
        switch (p->ctype) {
        case 0:
            o[0] = o[1] = o[2] = s[x];
            break;
        case 2:
            o[0] = s[3 * x];
            o[1] = s[3 * x + 1];
            o[2] = s[3 * x + 2];
            break;
        case 6: {
            uint32_t a = s[4 * x + 3];
            o[0] = div255(s[4 * x] * a);
            o[1] = div255(s[4 * x + 1] * a);
            o[2] = div255(s[4 * x + 2] * a);
            break;
        }
        default: {                               /* палитра; tRNS — альфа, нет — 255 */
            uint32_t i = s[x];
            const uint8_t *e = p->plte + 3 * i;
            uint32_t a = i < p->ntrns ? p->trns[i] : 255;
            o[0] = div255(e[0] * a);
            o[1] = div255(e[1] * a);
            o[2] = div255(e[2] * a);
            break;
        }
        }
    }
    fit_row(p->fit, p->emit, p->eu);
}

/* Готовая строка (без байта фильтра) → формат FT812 */
static void png_row(png_t *p, const uint8_t *s)
{
    uint8_t *o = p->out;
    uint32_t n = 0;
    if (p->fit) {
        png_fit_row(p, s);
        return;
    }
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
    if (p->ctype != 3 || p->fit)                 /* у вписывания палитра — только для перевода */
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
                if (fit_on(p->opts)) {
                    /* вписывание: веса и кольцо — в хвосте памяти декодера (inflate занят);
                       сразу — верхнее поле холста */
                    span_t sp[1] = {{(uint8_t *)(((uintptr_t)(p + 1) + 3u) & ~(uintptr_t)3),
                                     (uint8_t *)im + FTC_IMG_BYTES}};
                    if (!fit_init(&im->fit, p->w, p->h, p->w, p->h, sp, 1)) {
                        p->state = P_ERR;
                        break;
                    }
                    p->fit = &im->fit;
                    fit_start(p->fit, emit, u);
                }
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
                if (p->fit) {
                    im->info.fmt = FMT_RGB565;
                    im->info.w = FIT_W;
                    im->info.h = FIT_H;
                    im->info.stride = 2 * FIT_W;
                    im->info.pal_bytes = 0;
                }
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
    im->infl = (uint8_t *)inflate_state;
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
