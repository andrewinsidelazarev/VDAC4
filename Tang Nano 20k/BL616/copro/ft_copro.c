/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Сопроцессор FT812: кольцо, команды, дисплей-лист, запись в память. Описание — ft_copro.h.
 *
 * Что сделано (первая версия — то, что нужно Weather 2, и простое из PG §5):
 *   CMD_DLSTART, SWAP, INTERRUPT, BGCOLOR, FGCOLOR, GRADCOLOR, COLDSTART, SETBASE, SETSCRATCH,
 *   SETROTATE, GETPROPS; MEMWRITE, MEMSET, MEMZERO, MEMCPY, MEMCRC, REGREAD, APPEND, INFLATE,
 *   GETPTR; LOADIDENTITY, TRANSLATE, SCALE, ROTATE, SETMATRIX, GETMATRIX; SETBITMAP, GRADIENT;
 *   CALIBRATE, TRACK, TOUCH_TRANSFORM и недокументированные коды без действия (как BT8XX);
 *   TEXT (ПЗУ-шрифты 16…31 и шрифты CMD_SETFONT), SETFONT.
 *   LOADIMAGE (JPEG baseline и PNG 8 бит из кольца; ft_image.c; JPEG — как libjpeg), NUMBER,
 *   PROGRESS; MEDIAFIFO, VIDEOSTART, VIDEOFRAME (MJPEG AVI из медиа-FIFO, см. «Видео»).
 * Остальное (SETFONT2, ROMFONT, прочие виджеты, PLAYVIDEO) — пока ошибка «нет команды».
 * LOADIMAGE с OPT_MEDIAFIFO — тоже (данные только из кольца).
 * Свои команды VDAC4 — 3D (0xFFFFFFA0…0xFFFFFFAC): ft_3d.c. Свой флаг LOADIMAGE —
 * OPT_FULLSCREEN (8): картинка вписана в холст RGB565 1024×768 (ft_image.c, «Вписывание»).
 *
 * Матрицы — по eve-emu (eve-copro-matrix.cpp, MIT, Copyright alfishe): порядок, шаги угла и
 * округления там сверены с BT8XX. CMD_GRADIENT — замер bt8xxemu (fpga/vdac2/scripts/
 * ft_probe_gradient.py): eve-emu не повторяет, как ПЗУ считает C при L² > 2²⁰.
 */
#include <string.h>

#include "ft_copro.h"

_Static_assert(FTC_SCRATCH_BYTES >= FTC_IMG_BYTES + FTC_INFLATE_BYTES,
               "рабочая память меньше декодера картинок и inflate");

#define RING_MASK 0xFFFu

enum {
    CMD_DLSTART = 0xFFFFFF00u, CMD_SWAP = 0xFFFFFF01u, CMD_INTERRUPT = 0xFFFFFF02u,
    CMD_BGCOLOR = 0xFFFFFF09u, CMD_FGCOLOR = 0xFFFFFF0Au, CMD_GRADIENT = 0xFFFFFF0Bu,
    CMD_PROGRESS = 0xFFFFFF0Fu,
    CMD_TEXT = 0xFFFFFF0Cu, CMD_SETFONT = 0xFFFFFF2Bu, CMD_NUMBER = 0xFFFFFF2Eu,
    CMD_CALIBRATE = 0xFFFFFF15u, CMD_MEMCRC = 0xFFFFFF18u, CMD_REGREAD = 0xFFFFFF19u,
    CMD_MEMWRITE = 0xFFFFFF1Au, CMD_MEMSET = 0xFFFFFF1Bu, CMD_MEMZERO = 0xFFFFFF1Cu,
    CMD_MEMCPY = 0xFFFFFF1Du, CMD_APPEND = 0xFFFFFF1Eu, CMD_TOUCH_TRANSFORM = 0xFFFFFF20u,
    CMD_INFLATE = 0xFFFFFF22u, CMD_GETPTR = 0xFFFFFF23u, CMD_LOADIMAGE = 0xFFFFFF24u,
    CMD_GETPROPS = 0xFFFFFF25u,
    CMD_LOADIDENTITY = 0xFFFFFF26u, CMD_TRANSLATE = 0xFFFFFF27u, CMD_SCALE = 0xFFFFFF28u,
    CMD_ROTATE = 0xFFFFFF29u, CMD_SETMATRIX = 0xFFFFFF2Au, CMD_TRACK = 0xFFFFFF2Cu,
    CMD_COLDSTART = 0xFFFFFF32u, CMD_GETMATRIX = 0xFFFFFF33u, CMD_GRADCOLOR = 0xFFFFFF34u,
    CMD_SETROTATE = 0xFFFFFF36u, CMD_SETBASE = 0xFFFFFF38u, CMD_SETSCRATCH = 0xFFFFFF3Cu,
    CMD_SETBITMAP = 0xFFFFFF43u, CMD_MEDIAFIFO = 0xFFFFFF39u, CMD_VIDEOSTART = 0xFFFFFF40u,
    CMD_VIDEOFRAME = 0xFFFFFF41u,
};

/* Слов параметров после кода; -1 — команды нет. */
static int cmd_params(uint32_t code)
{
    switch (code) {
    case CMD_DLSTART: case CMD_SWAP: case CMD_COLDSTART: case CMD_LOADIDENTITY: case CMD_SETMATRIX:
    case CMD_VIDEOSTART:
        return 0;
    case CMD_INTERRUPT: case CMD_BGCOLOR: case CMD_FGCOLOR: case CMD_GRADCOLOR: case CMD_SETBASE:
    case CMD_SETSCRATCH: case CMD_SETROTATE: case CMD_INFLATE: case CMD_GETPTR: case CMD_CALIBRATE:
    case CMD_ROTATE:
        return 1;
    case CMD_REGREAD: case CMD_MEMWRITE: case CMD_MEMZERO: case CMD_APPEND: case CMD_TRANSLATE:
    case CMD_SCALE: case CMD_TEXT: case CMD_SETFONT: case CMD_LOADIMAGE: case CMD_MEDIAFIFO:
    case CMD_VIDEOFRAME:
        return 2;
    case CMD_GETPROPS: case CMD_MEMCRC: case CMD_MEMSET: case CMD_MEMCPY: case CMD_TRACK:
    case CMD_SETBITMAP: case CMD_NUMBER:
        return 3;
    case CMD_GRADIENT: case CMD_PROGRESS:
        return 4;
    case CMD_GETMATRIX:
        return 6;
    case CMD_TOUCH_TRANSFORM:
        return 13;
    /* недокументированные: BT8XX выполняет их без действия (eve-emu, golden cmd-undocumented-*) */
    case 0xFFFFFF03u: case 0xFFFFFF35u: case 0xFFFFFF3Du: case 0xFFFFFF42u: case 0xFFFFFF44u:
    case 0xFFFFFF47u: case 0xFFFFFF48u: case 0xFFFFFF49u: case 0xFFFFFF4Au: case 0xFFFFFF4Bu:
    case 0xFFFFFF4Cu: case 0xFFFFFF4Du: case 0xFFFFFF4Fu: case 0xFFFFFF60u: case 0xFFFFFFFFu:
        return 0;
    default:
        return ftc3d_params(code);         /* 3D VDAC4 (ft_3d.c) или -1 */
    }
}

#define DL(op, v) (((uint32_t)(op) << 24) | (uint32_t)(v))
enum {
    OP_BITMAP_SOURCE = 0x01, OP_COLOR_RGB = 0x04, OP_BITMAP_HANDLE = 0x05, OP_BITMAP_LAYOUT = 0x07,
    OP_BITMAP_SIZE = 0x08, OP_BLEND_FUNC = 0x0B, OP_COLOR_A = 0x10, OP_TRANSFORM_A = 0x15,
    OP_BEGIN = 0x1F, OP_COLOR_MASK = 0x20, OP_SAVE_CONTEXT = 0x22, OP_RESTORE_CONTEXT = 0x23,
    OP_LAYOUT_H = 0x28, OP_SIZE_H = 0x29, OP_PALETTE_SOURCE = 0x2A,
};

/* --- шина ------------------------------------------------------------------------------- */
static uint32_t rd32(ftc_t *c, uint32_t a)
{
    uint32_t w;
    c->bus.rd(c->bus.ctx, a, &w, 1);
    return w;
}

static void wr32(ftc_t *c, uint32_t a, uint32_t w)
{
    c->bus.wr(c->bus.ctx, a, &w, 1);
}

/* n байтов памяти FT812 с адреса a */
static void rd_bytes(ftc_t *c, uint32_t a, uint8_t *out, uint32_t n)
{
    uint32_t tmp[64];
    while (n) {
        uint32_t a0 = a & ~3u, skip = a & 3u;
        uint32_t words = (skip + n + 3) >> 2;
        if (words > 64)
            words = 64;
        c->bus.rd(c->bus.ctx, a0, tmp, words);
        uint32_t take = words * 4 - skip;
        if (take > n)
            take = n;
        for (uint32_t i = 0; i < take; i++)
            out[i] = (uint8_t)(tmp[(skip + i) >> 2] >> (8 * ((skip + i) & 3)));
        out += take;
        a += take;
        n -= take;
    }
}

/* --- кольцо ------------------------------------------------------------------------------ */
static uint32_t ring_words(const ftc_t *c)
{
    return ((c->wp - c->rp) & RING_MASK) >> 2;
}

static uint32_t ring_word(const ftc_t *c, uint32_t i)
{
    return c->ring[((c->rp + 4 * i) & RING_MASK) >> 2];
}

static const uint8_t *ring_bytes(const ftc_t *c)
{
    return (const uint8_t *)c->ring;           /* слова — в порядке байтов FT812 (little endian) */
}

static uint32_t data_avail(const ftc_t *c)
{
    return ring_words(c) * 4 - c->roff;
}

static void data_consume(ftc_t *c, uint32_t n)
{
    uint32_t t = c->roff + n;
    c->rp = (c->rp + (t & ~3u)) & RING_MASK;
    c->roff = t & 3u;
}

/* новые слова кольца — в копию */
static void fetch(ftc_t *c)
{
    while (c->fetched != c->wp) {
        uint32_t end = c->wp > c->fetched ? c->wp : 4096;
        c->bus.rd(c->bus.ctx, FTC_RAM_CMD + c->fetched, &c->ring[c->fetched >> 2], (end - c->fetched) >> 2);
        c->fetched = end & RING_MASK;
    }
}

/* выходной параметр — в кольцо на место параметра i */
static void result(ftc_t *c, uint32_t i, uint32_t v)
{
    uint32_t o = (c->cmd_at + 4 * (1 + i)) & RING_MASK;
    c->ring[o >> 2] = v;
    wr32(c, FTC_RAM_CMD + o, v);
}

void ftc_result(ftc_t *c, uint32_t i, uint32_t v)
{
    result(c, i, v);
}

/* --- дисплей-лист ------------------------------------------------------------------------- */
static void dl_flush(ftc_t *c)
{
    if (c->dlq_n) {
        c->bus.wr(c->bus.ctx, FTC_RAM_DL + c->dlq_at, c->dlq, c->dlq_n);
        c->dlq_n = 0;
    }
}

static void fault(ftc_t *c, int why);

static int dl_put(ftc_t *c, uint32_t w)
{
    /* REG_CMD_DL — 13 бит: после слова 2047 он 0, следующее слово — ошибка [PG §5.6] */
    if (c->dl_full) {
        fault(c, FTC_FAULT_DL_FULL);
        return 0;
    }
    if (c->dlq_n && (c->dlq_at + 4 * c->dlq_n != c->dl || c->dlq_n == FTC_DLQ_WORDS))
        dl_flush(c);
    if (!c->dlq_n)
        c->dlq_at = c->dl;
    c->dlq[c->dlq_n++] = w;
    c->dl = (c->dl + 4) & 0x1FFFu;
    if (c->dl == 0)
        c->dl_full = 1;
    return 1;
}

static int dl_words(ftc_t *c, const uint32_t *w, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (!dl_put(c, w[i]))
            return 0;
    return 1;
}

/* --- запись байтов подряд: края слов — чтением памяти ------------------------------------- */
static void mw_flush(ftc_t *c)
{
    if (c->mw_n) {
        if (c->bus.wr_async) {
            /* с прошивки 32 — без ожидания: этот буфер уходит DMA, пока копится другой (у
               wr_async прошлая посылка сперва кончается; чтение, снимок и обычная запись шины
               тоже ждут её — порядок записей в ПЛИС тот же) */
            c->bus.wr_async(c->bus.ctx, c->mw_at, c->mw_buf, c->mw_n);
            c->mw_buf = c->mw_bufs[c->mw_buf == c->mw_bufs[0]];
        } else {
            c->bus.wr(c->bus.ctx, c->mw_at, c->mw_buf, c->mw_n);
        }
        c->mw_n = 0;
    }
}

static void mw_out(ftc_t *c, uint32_t wa, uint32_t w)
{
    if (c->mw_n && (c->mw_at + 4 * c->mw_n != wa || c->mw_n == FTC_MW_WORDS))
        mw_flush(c);
    if (!c->mw_n)
        c->mw_at = wa;
    c->mw_buf[c->mw_n++] = w;
}

static void mw_begin(ftc_t *c, uint32_t a)
{
    c->mw_addr = a;
    c->mw_word = 0;
    c->mw_mask = 0;
    if (a & 3u) {
        c->mw_word = rd32(c, a & ~3u);
        c->mw_mask = (1u << (a & 3u)) - 1;
    }
}

static void mw_byte(ftc_t *c, uint8_t b)
{
    uint32_t lane = c->mw_addr & 3u;
    c->mw_word = (c->mw_word & ~(0xFFu << (8 * lane))) | ((uint32_t)b << (8 * lane));
    c->mw_mask |= 1u << lane;
    c->mw_addr++;
    if (lane == 3) {
        mw_out(c, c->mw_addr - 4, c->mw_word);
        c->mw_word = 0;
        c->mw_mask = 0;
    }
}

static void mw_put(ftc_t *c, const uint8_t *p, size_t n)
{
    while (n) {
        if (!(c->mw_addr & 3u) && n >= 4) {
            /* слова — пачкой (строки картинок, прошивка 32), сброс — как у mw_out */
            if (c->mw_n && (c->mw_at + 4 * c->mw_n != c->mw_addr || c->mw_n == FTC_MW_WORDS))
                mw_flush(c);
            if (!c->mw_n)
                c->mw_at = c->mw_addr;
            uint32_t k = (uint32_t)(n >> 2), room = FTC_MW_WORDS - c->mw_n;
            if (k > room)
                k = room;
            memcpy(c->mw_buf + c->mw_n, p, 4 * k);
            c->mw_n += k;
            c->mw_addr += 4 * k;
            p += 4 * k;
            n -= 4 * k;
        } else {
            mw_byte(c, *p++);
            n--;
        }
    }
}

static void mw_fill(ftc_t *c, uint8_t v, uint32_t n)
{
    uint32_t w = v * 0x01010101u;
    while (n) {
        if (!(c->mw_addr & 3u) && n >= 4) {
            mw_out(c, c->mw_addr, w);
            c->mw_addr += 4;
            n -= 4;
        } else {
            mw_byte(c, v);
            n--;
        }
    }
}

static void mw_end(ftc_t *c)
{
    mw_flush(c);
    if (c->mw_mask) {
        uint32_t a = c->mw_addr & ~3u, w = rd32(c, a), m = 0;
        for (uint32_t l = 0; l < 4; l++)
            if (c->mw_mask & (1u << l))
                m |= 0xFFu << (8 * l);
        wr32(c, a, (w & ~m) | (c->mw_word & m));
        c->mw_mask = 0;
    }
}

/* --- ошибка, завершение команды, состояние по умолчанию ------------------------------------ */
static void fault(ftc_t *c, int why)
{
    dl_flush(c);
    mw_flush(c);
    c->fault = why;
    c->cmd = 0;
    c->roff = 0;
    /* BT8XX оставляет в начале RAM_CMD слово, зависящее от ошибки (eve-emu, golden fault-ring-*) */
    uint32_t mark = why == FTC_FAULT_UNKNOWN ? 0x04F2u : why == FTC_FAULT_INFLATE ? 0x2074u : 0;
    if (mark) {
        c->ring[0] = mark;
        wr32(c, FTC_RAM_CMD, mark);
    }
    if (c->bus.irq)
        c->bus.irq(c->bus.ctx, FTC_INT_CMDEMPTY);
}

static void complete(ftc_t *c)
{
    if (c->roff)
        data_consume(c, 4 - c->roff);
    c->cmd = 0;
    c->done = c->total = 0;
}

static void defaults(ftc_t *c)
{
    memset(c->matrix, 0, sizeof c->matrix);
    c->matrix[0] = c->matrix[4] = 0x10000;
    c->bg = 0x002040;
    c->fg = 0x003870;
    c->grad = 0xFFFFFF;
    c->scratch = 15;
    c->base = 10;
    c->inflate_end = 0;
    c->img_addr = c->img_w = c->img_h = 0;
    memset(c->font_ptr, 0, sizeof c->font_ptr);
    c->rom_t0 = c->bus.ms ? c->bus.ms(c->bus.ctx) : 0;
    c->rom_ok = 0;
    c->txt_wait = 0;
    ftc3d_reset(&c->g3d);
    c->scratch_dirty = 0;
}

void ftc_init(ftc_t *c, const ftc_bus_t *bus)
{
    memset(c, 0, sizeof *c);
    c->bus = *bus;
    c->mw_buf = c->mw_bufs[0];
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t r = i;
        for (int k = 0; k < 8; k++)
            r = (r >> 1) ^ (0xEDB88320u & (0u - (r & 1u)));
        c->crc_table[i] = r;
    }
    defaults(c);
    c->held = 1;                        /* состояние ПЛИС — с первого опроса, как после сброса */
}

/* --- матрицы (eve-emu, eve-copro-matrix.cpp) ----------------------------------------------- */
static int32_t sat32(int64_t v)
{
    return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
}

static int64_t fmul(int64_t a, int64_t b)
{
    return (a * b) >> 16;                 /* вниз (BT8XX, matrix-combined) */
}

static int64_t fdiv(int64_t a, int64_t b)
{
    return b == 0 ? 0 : (a * 65536) / b;  /* к нулю */
}

static double sine(double x)
{
    double term = x, sum = x, x2 = x * x;
    for (int n = 1; n < 12; n++) {
        term = -term * x2 / (double)((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

/* sin и cos угла CMD_ROTATE в 16.16, как в ПЗУ: шаг 1/1024 оборота, 2·trunc(32767·sin) */
static void sincos16(uint32_t angle, int64_t *s, int64_t *k)
{
    const double pi = 3.14159265358979323846;
    uint32_t a = (angle & ~0x3Fu) % 65536u, q = a / 16384u;
    double r = (double)(a % 16384u) * (2 * pi) / 65536.0;
    double sv = sine(r), cv = sine(pi / 2 - r), t;
    switch (q) {
    case 1: t = sv; sv = cv; cv = -t; break;
    case 2: sv = -sv; cv = -cv; break;
    case 3: t = sv; sv = -cv; cv = t; break;
    default: break;
    }
    *s = 2 * (int64_t)(sv * 32767);
    *k = 2 * (int64_t)(cv * 32767);
}

/* m = n × m */
static void premul(int32_t *m, int64_t na, int64_t nb, int64_t nc, int64_t nd, int64_t ne, int64_t nf)
{
    int64_t a = m[0], b = m[1], cc = m[2], d = m[3], e = m[4], f = m[5];
    m[0] = sat32(fmul(na, a) + fmul(nb, d));
    m[1] = sat32(fmul(na, b) + fmul(nb, e));
    m[2] = sat32(fmul(na, cc) + fmul(nb, f) + nc);
    m[3] = sat32(fmul(nd, a) + fmul(ne, d));
    m[4] = sat32(fmul(nd, b) + fmul(ne, e));
    m[5] = sat32(fmul(nd, cc) + fmul(ne, f) + nf);
}

/* --- слова битмапов ------------------------------------------------------------------------- */
static uint32_t bpp(uint32_t fmt)
{
    switch (fmt) {
    case 1: return 1;                      /* L1 */
    case 17: return 2;                     /* L2 */
    case 2: return 4;                      /* L4 */
    case 0: case 6: case 7: case 10: return 16;
    default: return 8;
    }
}

static uint32_t w_layout(uint32_t fmt, uint32_t stride, uint32_t h)
{
    return DL(OP_BITMAP_LAYOUT, (fmt << 19) | ((stride & 0x3FFu) << 9) | (h & 0x1FFu));
}

static uint32_t w_layout_h(uint32_t stride, uint32_t h)
{
    return DL(OP_LAYOUT_H, ((stride >> 10) << 2) | (h >> 9));
}

static uint32_t w_size(uint32_t w, uint32_t h)
{
    return DL(OP_BITMAP_SIZE, ((w & 0x1FFu) << 9) | (h & 0x1FFu));
}

static uint32_t w_size_h(uint32_t w, uint32_t h)
{
    return DL(OP_SIZE_H, ((w >> 9) << 2) | (h >> 9));
}

static int64_t tdiv(int64_t n, int64_t d)
{
    return n / d;                          /* C: к нулю */
}

/* CMD_GRADIENT. Рампа L8 512×1 в ПЗУ (0x200065: 0 до текселя 128, 255 с 383) на весь экран
   ручкой scratch: сначала поканальный минимум цветов с рампой в альфе, затем растущие каналы
   с DST_ALPHA и падающие с ONE_MINUS_DST_ALPHA (eve-emu, eve-copro-text.cpp). Преобразование
   t = 0 → тексель 128, t = 1 → 384; A, B — 65536·d/L², C — замер bt8xxemu: при L² > 2²⁰ ПЗУ
   считает C по концам, округлённым до кратного 2^s (s — на сколько бит L² - 1 длиннее 20):
   по X — оба конца вниз, по Y — длина вверх, начало как есть (при dy < 0 — от конца).
   Сходится с bt8xxemu в 1228 замерах из 1299 (test/data/gradient.txt); в 64 из остальных C
   отличается на 1…16, в 7 — на 64…92 (вертикали с нечётным x0, обратная диагональ). */
static int gradient(ftc_t *c)
{
    int64_t x0 = (int16_t)c->p[0], y0 = (int16_t)(c->p[0] >> 16);
    int64_t x1 = (int16_t)c->p[2], y1 = (int16_t)(c->p[2] >> 16);
    uint32_t rgb0 = c->p[1] & 0xFFFFFFu, rgb1 = c->p[3] & 0xFFFFFFu;
    int64_t dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
    int64_t a = -1, b = -1, cc = -1, d = -1, e = -1, f = -1;
    if (l2) {
        int s = 0;
        while (((l2 - 1) >> (20 + s)) != 0)
            s++;
        int64_t m = ~(((int64_t)1 << s) - 1);
        int64_t x0r = x0 & m, dxr = (x1 & m) - x0r;
        int64_t dyc = -((-dy) & m);
        int64_t y0e = dy >= 0 ? y0 : y1 - dyc;
        a = tdiv(65536 * dx, l2);
        b = tdiv(65536 * dy, l2);
        cc = tdiv(32768 * (dx * dxr + dy * dyc) - 65536 * (dx * x0r + dy * y0e), l2);
        d = e = f = 0;
    }
    uint32_t base = 0, rise = 0, fall = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
        uint32_t v0 = (rgb0 >> sh) & 0xFF, v1 = (rgb1 >> sh) & 0xFF;
        base |= (v0 < v1 ? v0 : v1) << sh;
        rise |= (v1 > v0 ? v1 - v0 : 0) << sh;
        fall |= (v0 > v1 ? v0 - v1 : 0) << sh;
    }
    uint32_t h = c->scratch & 31;
    uint32_t vtx = (2u << 30) | (h << 7);
    const uint32_t head[] = {
        DL(OP_SAVE_CONTEXT, 0),
        DL(OP_TRANSFORM_A + 0, (uint32_t)a & 0x1FFFFu), DL(OP_TRANSFORM_A + 1, (uint32_t)b & 0x1FFFFu),
        DL(OP_TRANSFORM_A + 3, (uint32_t)d & 0x1FFFFu), DL(OP_TRANSFORM_A + 4, (uint32_t)e & 0x1FFFFu),
        DL(OP_TRANSFORM_A + 2, (uint32_t)cc & 0xFFFFFFu), DL(OP_TRANSFORM_A + 5, (uint32_t)f & 0xFFFFFFu),
        DL(OP_BITMAP_HANDLE, h), DL(OP_BITMAP_SOURCE, 0x200065u), w_layout_h(512, 1), w_layout(3, 512, 1),
        w_size_h(0, 0), DL(OP_BITMAP_SIZE, 1u << 18),
        DL(OP_COLOR_RGB, base), DL(OP_COLOR_A, 0xFF), DL(OP_BLEND_FUNC, (1u << 3) | 0),
        DL(OP_COLOR_MASK, 0xF), DL(OP_BEGIN, 1), vtx, DL(OP_COLOR_MASK, 0xE),
    };
    if (!dl_words(c, head, sizeof head / sizeof head[0]))
        return 0;
    if (rise) {
        const uint32_t pass[] = {DL(OP_COLOR_RGB, rise), DL(OP_BLEND_FUNC, (3u << 3) | 1), vtx};
        if (!dl_words(c, pass, 3))
            return 0;
    }
    if (fall) {
        const uint32_t pass[] = {DL(OP_COLOR_RGB, fall), DL(OP_BLEND_FUNC, (5u << 3) | 1), vtx};
        if (!dl_words(c, pass, 3))
            return 0;
    }
    return dl_put(c, DL(OP_RESTORE_CONTEXT, 0));
}

/* --- память ---------------------------------------------------------------------------------- */
static void mem_copy(ftc_t *c, uint32_t dst, uint32_t src, uint32_t n)
{
    /* вперёд по байтам (eve-emu, V16): при перекрытии копия читает то, что сама записала —
       кусок не длиннее расстояния, каждый кусок записан до чтения следующего */
    uint8_t buf[256];
    uint32_t gap = dst - src, lim = sizeof buf;
    int overlap = dst > src && gap < n;
    if (overlap && gap < lim)
        lim = gap;
    mw_begin(c, dst);
    while (n) {
        uint32_t k = n < lim ? n : lim;
        if (overlap) {
            mw_end(c);
            mw_begin(c, dst);
        }
        rd_bytes(c, src, buf, k);
        mw_put(c, buf, k);
        src += k;
        dst += k;
        n -= k;
    }
    mw_end(c);
}

static uint32_t mem_crc(ftc_t *c, uint32_t a, uint32_t n)
{
    uint8_t buf[256];
    uint32_t crc = 0xFFFFFFFFu;
    while (n) {
        uint32_t k = n < sizeof buf ? n : sizeof buf;
        rd_bytes(c, a, buf, k);
        for (uint32_t i = 0; i < k; i++)
            crc = c->crc_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
        a += k;
        n -= k;
    }
    return ~crc;
}

static int append(ftc_t *c, uint32_t a, uint32_t n)
{
    uint32_t tmp[64];
    n &= ~3u;                              /* кратно 4 [PG §5.15] */
    while (n) {
        uint32_t k = n / 4 < 64 ? n / 4 : 64;
        c->bus.rd(c->bus.ctx, a, tmp, k);
        if (!dl_words(c, tmp, k))
            return 0;
        a += 4 * k;
        n -= 4 * k;
    }
    return 1;
}

static void inflate_out(void *u, const uint8_t *p, size_t n)
{
    ftc_t *c = (ftc_t *)u;
    mw_put(c, p, n);
    c->done += (uint32_t)n;
}

/* CMD_LOADIMAGE выполнена: лист и CMD_GETPROPS — как у FT812 (bt8xxemu,
   fpga/vdac2/scripts/ft_probe_loadimage.py): [PALETTE_SOURCE ptr], SOURCE (за палитрой),
   LAYOUT_H, LAYOUT, SIZE_H, SIZE (NEAREST, BORDER); GETPROPS — у JPEG начало, у PNG конец. */
static void loadimage_done(ftc_t *c)
{
    ftc_img_info_t in;
    ftc_img_get_info(FTC_SCRATCH(c), &in);
    uint32_t ptr = c->p[0] & 0x3FFFFFu, src = ptr + in.pal_bytes;
    c->img_addr = in.jpeg ? ptr : src + in.stride * in.h;
    c->img_w = in.w;
    c->img_h = in.h;
    if (c->p[1] & 2u)                    /* OPT_NODL */
        return;
    if (in.pal_bytes)
        dl_put(c, DL(OP_PALETTE_SOURCE, ptr));
    const uint32_t words[] = {DL(OP_BITMAP_SOURCE, src), w_layout_h(in.stride, in.h),
                              w_layout(in.fmt, in.stride, in.h), w_size_h(in.w, in.h), w_size(in.w, in.h)};
    dl_words(c, words, 5);
}

/* --- CMD_TEXT --------------------------------------------------------------------------------
 * Лист — как у bt8xxemu (fpga/vdac2/scripts/ft_probe_text.py: 312 случаев слово в слово):
 * SAVE_CONTEXT, VERTEX_FORMAT(2), BITMAP_HANDLE(шрифт), BEGIN(BITMAPS); каждый символ (и пробел) —
 * VERTEX2II(x, y, шрифт, код), если 0 ≤ x, y ≤ 511, иначе CELL(код) и VERTEX2F(4x, 4y); шаг — ширина
 * символа из метрик шрифта; RESTORE_CONTEXT. Ширина строки для OPT_RIGHTX / OPT_CENTERX — сумма
 * ширин − 1 (CENTERX — её половина вниз), OPT_CENTERY — половина высоты шрифта. Метрики (148
 * байтов: 128 ширин, формат, шаг строки, ширина, высота, адрес) — ПЗУ-шрифты 16…31 по таблице
 * ROM_FONTROOT, шрифты 0…15 — по адресу из CMD_SETFONT. */
enum { OPT_SIGNED = 256, OPT_CENTERX = 512, OPT_CENTERY = 1024, OPT_RIGHTX = 2048 };

static int rom_root_ok(uint32_t root)
{
    return root >= 0x200000u && root + 148u * 16u <= FTC_ROM_FONTROOT && !(root & 3u);
}

/* Метрики годны: формат, шаг строки, ширина и высота есть, у ПЗУ-шрифта — глифы в ПЗУ, и не все
   ширины печатных знаков нулевые. Чтение ПЛИС, на которое она не ответила (fpga_link: rd_chunk),
   — нули: с ними все знаки строки встают в одну точку. */
static int metrics_ok(const uint32_t *m, int rom)
{
    const uint8_t *wd = (const uint8_t *)m;
    int widths = 0;
    for (uint32_t ch = 32; ch < 127 && !widths; ch++)
        widths = wd[ch] != 0;
    if (!widths || m[32] > 31u || m[33] == 0 || m[34] == 0 || m[35] == 0)
        return 0;
    return !rom || (m[36] >= 0x200000u && m[36] < 0x300000u);
}

/* Метрики шрифта команды — в c->txt_m. 1 — годны (или шрифта нет — нули, как у FT812). */
static int text_metrics(ftc_t *c, uint32_t font)
{
    memset(c->txt_m, 0, sizeof c->txt_m);
    if (font >= 16 && font <= 31) {
        uint32_t bit = 1u << (font - 16u);
        if (c->rom_ok & bit) {
            memcpy(c->txt_m, c->rom_met[font - 16u], sizeof c->txt_m);
            return 1;
        }
        uint32_t root = rd32(c, FTC_ROM_FONTROOT);
        if (!rom_root_ok(root))
            return 0;
        c->bus.rd(c->bus.ctx, root + 148u * (font - 16u), c->txt_m, 37);
        if (!metrics_ok(c->txt_m, 1))
            return 0;
        memcpy(c->rom_met[font - 16u], c->txt_m, sizeof c->txt_m);
        c->rom_ok |= bit;
        return 1;
    }
    if (font < 16 && c->font_ptr[font]) {
        c->bus.rd(c->bus.ctx, c->font_ptr[font] & ~3u, c->txt_m, 37);
        return metrics_ok(c->txt_m, 0);
    }
    return 1;
}

/* Можно рисовать: метрики годны — или ждать их уже поздно: образ ПЗУ-шрифтов заливает ESP после
   загрузки ядра (до FTC_ROM_WAIT_MS от сброса сопроцессора), а негодное чтение повторяется до
   FTC_TEXT_RETRY_MS (без часов — FTC_TEXT_TRIES раз подряд); дальше — с тем, что есть. */
static int text_ready(ftc_t *c)
{
    uint32_t font = c->p[1] & 0xFFFFu;
    if (!c->bus.ms) {
        for (uint32_t i = 0; i < FTC_TEXT_TRIES; i++)
            if (text_metrics(c, font))
                return 1;
        return 1;
    }
    uint32_t now = c->bus.ms(c->bus.ctx);
    if (!c->txt_wait) {
        c->txt_wait = 1;
        c->txt_t0 = now;
    }
    if (text_metrics(c, font) ||
        (now - c->rom_t0 >= FTC_ROM_WAIT_MS && now - c->txt_t0 >= FTC_TEXT_RETRY_MS)) {
        c->txt_wait = 0;
        return 1;
    }
    return 0;
}

static void text_emit(ftc_t *c)
{
    int32_t x = (int16_t)(c->p[0] & 0xFFFFu), y = (int16_t)(c->p[0] >> 16);
    uint32_t font = c->p[1] & 0xFFFFu, opt = c->p[1] >> 16;
    const uint32_t *m = c->txt_m;                  /* text_ready */
    const uint8_t *wd = (const uint8_t *)m;        /* ширины — младшим байтом вперёд */
    int32_t h = (int32_t)m[35];
    int32_t tw = 0;
    for (uint32_t i = 0; i < c->txt_n; i++)
        tw += wd[c->txt[i] & 127u];
    if (c->txt_n) {
        if (opt & OPT_RIGHTX)
            x -= tw - 1;
        else if (opt & OPT_CENTERX)
            x -= (tw - 1) >> 1;
    }
    if (opt & OPT_CENTERY)
        y -= h >> 1;
    uint32_t hd = font & 31u;
    const uint32_t head[4] = {DL(0x22, 0), DL(0x27, 2), DL(0x05, hd), DL(0x1F, 1)};
    if (!dl_words(c, head, 4))
        return;
    for (uint32_t i = 0; i < c->txt_n; i++) {
        uint32_t ch = c->txt[i] & 127u;
        if (x >= 0 && x <= 511 && y >= 0 && y <= 511) {
            if (!dl_put(c, 0x80000000u | ((uint32_t)x << 21) | ((uint32_t)y << 12) | (hd << 7) | ch))
                return;
        } else {
            const uint32_t v[2] = {DL(0x06, ch), 0x40000000u | (((uint32_t)(x * 4) & 0x7FFFu) << 15) |
                                                 ((uint32_t)(y * 4) & 0x7FFFu)};
            if (!dl_words(c, v, 2))
                return;
        }
        x += wd[ch];
    }
    dl_put(c, DL(0x23, 0));
}

/* CMD_NUMBER: строка числа p[2] — как у bt8xxemu (fpga/vdac2/scripts/ft_probe_number.py, 190
   случаев): база — CMD_SETBASE (цифры больше 9 — A…Z), OPT_SIGNED — отрицательное как «-» и
   модуль; младшие 5 разрядов опций — ровно столько цифр (ведущие нули, старшие лишние цифры
   отбрасываются; знак — сверх них), 0 — сколько нужно. Дальше — как CMD_TEXT. */
static void number_text(ftc_t *c)
{
    uint32_t n = c->p[2], opt = c->p[1] >> 16, base = c->base;
    if (base < 2 || base > 36)
        base = 10;
    int neg = (opt & OPT_SIGNED) && (int32_t)n < 0;
    uint32_t v = neg ? 0u - n : n, want = opt & 31u, k = 0;
    char d[32];
    do {
        uint32_t r = v % base;
        d[k++] = (char)(r < 10 ? '0' + r : 'A' + r - 10);
        v /= base;
    } while (want ? k < want : v != 0);
    c->txt_n = 0;
    if (neg)
        c->txt[c->txt_n++] = '-';
    while (k)
        c->txt[c->txt_n++] = (uint8_t)d[--k];
}

/* CMD_PROGRESS — лист как у bt8xxemu (fpga/vdac2/scripts/ft_probe_progress.py): полоса — линия
   (LINES, VERTEX_FORMAT 2, т. е. 1/4 точки) по оси полосы: горизонтальная при w > h, иначе
   вертикальная (заполняется сверху); толщина t = h или w, LINE_WIDTH 8t (по маске 12 разрядов).
   Фон — BGCOLOR, у объёмной (без OPT_FLAT) под ним белая (+2, +2) и чёрная (−3, −3) линии.
   Значение — текущим цветом, LINE_WIDTH 7t, до начала + длина·val/range (вниз; range 0 — начало
   − 1), без предела сверху. */
static uint32_t v2f(int32_t x, int32_t y)
{
    return 0x40000000u | (((uint32_t)x & 0x7FFFu) << 15) | ((uint32_t)y & 0x7FFFu);
}

static void progress(ftc_t *c)
{
    int32_t x = (int16_t)(c->p[0] & 0xFFFFu), y = (int16_t)(c->p[0] >> 16);
    int32_t w = (int16_t)(c->p[1] & 0xFFFFu), h = (int16_t)(c->p[1] >> 16);
    uint32_t opt = c->p[2] & 0xFFFFu, val = c->p[2] >> 16, range = c->p[3] & 0xFFFFu;
    int vert = !(w > h);
    int32_t t = vert ? w : h, a = 4 * (vert ? y : x), len = 4 * (vert ? h : w);
    int32_t cc = vert ? 4 * x + 2 * w : 4 * y + 2 * h;
    int32_t vb = range ? a + (int32_t)(((int64_t)len * val) / range) : a - 1;
#define PV(u, v) (vert ? v2f((v), (u)) : v2f((u), (v)))
    uint32_t dl[24];
    uint32_t n = 0;
    dl[n++] = DL(0x22, 0);
    dl[n++] = DL(0x27, 2);
    dl[n++] = DL(0x0E, (uint32_t)(8 * t) & 0xFFFu);
    dl[n++] = DL(0x1F, 3);
    if (!(opt & 256u)) {
        dl[n++] = DL(OP_COLOR_RGB, 0xFFFFFFu);
        dl[n++] = v2f(vert ? cc + 2 : a + 2, vert ? a + 2 : cc + 2);
        dl[n++] = v2f(vert ? cc + 2 : a + len + 2, vert ? a + len + 2 : cc + 2);
        dl[n++] = DL(OP_COLOR_RGB, 0);
        dl[n++] = v2f(vert ? cc - 3 : a - 3, vert ? a - 3 : cc - 3);
        dl[n++] = v2f(vert ? cc - 3 : a + len - 3, vert ? a + len - 3 : cc - 3);
    }
    dl[n++] = DL(OP_COLOR_RGB, c->bg & 0xFFFFFFu);
    dl[n++] = PV(a, cc);
    dl[n++] = PV(a + len, cc);
    dl[n++] = DL(0x23, 0);
    dl[n++] = DL(0x22, 0);
    dl[n++] = DL(0x27, 2);
    dl[n++] = DL(0x0E, (uint32_t)(7 * t) & 0xFFFu);
    dl[n++] = PV(a, cc);
    dl[n++] = PV(vb, cc);
    dl[n++] = DL(0x23, 0);
#undef PV
    dl_words(c, dl, n);
}

/* --- команды ---------------------------------------------------------------------------------- */
enum { B_DONE, B_DATA, B_WAIT };     /* выполнена; ждёт данных кольца; ждёт события */

/* --- Видео: медиа-FIFO, CMD_VIDEOSTART, CMD_VIDEOFRAME --------------------------------------
 * Как у FT812 (замер bt8xxemu: fpga/vdac2/scripts/ft_probe_video.py, docs/ft812-mjpeg-video.md):
 * - CMD_MEDIAFIFO(ptr, size) — кольцо в RAM_G; REG_MEDIAFIFO_READ и _WRITE — в 0. Данные и
 *   _WRITE пишет хост; сопроцессор двигает _READ: позиция, округлённая вверх до 4 (FT812
 *   читает словами).
 * - CMD_VIDEOSTART — заголовок RIFF 'AVI ' (из него — длина файла), затем чанки по 'avih'
 *   включительно (у обычного AVI — 88 байтов).
 * - CMD_VIDEOFRAME(dst, ptr) — чанки дальше: в LIST (strl, movi, rec) заходит, прочие (strh,
 *   strf, JUNK, звук, idx1) пропускает с байтом выравнивания нечётной длины; чанк '..dc' или
 *   '..db' — кадр JPEG в dst (RGB565; ft_image.c, JPEG — как libjpeg, без DHT — стандартные
 *   таблицы). В слово RAM_G по ptr — длина файла минус позиция за кадром (0 — файл кончился).
 *   Дошёл до конца файла на пропуске — 0, кадра нет, dst не тронут. Данных нет — ждёт: так
 *   ftview узнаёт конец потока, если idx1 нет.
 * Чанк кадра здесь кончается по длине из заголовка, а не по маркеру EOI с заглядыванием в
 * следующий чанк (у bt8xxemu чанк нечётной длины с байтом за EOI сбивал разбор), и последнему
 * кадру не нужен запас данных за ним. */
#define REG_MF_READ  0x309014u
#define REG_MF_WRITE 0x309018u
#define FOURCC(a, b, c_, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c_) << 16) | ((uint32_t)(d) << 24))
enum { VS_RIFF, VS_HDR, VS_SKIP, VF_HDR, VF_SKIP, VF_DEC, VF_TAIL };

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Байты потока, записанные хостом (REG_MEDIAFIFO_WRITE), — в mf_buf, сколько влезет. */
static void mf_fill(ftc_t *c)
{
    if (c->mf_bi) {
        memmove(c->mf_buf, c->mf_buf + c->mf_bi, c->mf_bn - c->mf_bi);
        c->mf_bn -= c->mf_bi;
        c->mf_bi = 0;
    }
    if (!c->mf_size || FTC_MF_BUF - c->mf_bn < 256)
        return;
    uint32_t wr = rd32(c, REG_MF_WRITE) % c->mf_size;
    for (int part = 0; part < 2; part++) {
        uint32_t at = (c->mf_pos + c->mf_bn) % c->mf_size;
        uint32_t n = (wr + c->mf_size - at) % c->mf_size, room = FTC_MF_BUF - c->mf_bn;
        if (n > c->mf_size - at)
            n = c->mf_size - at;                  /* до конца кольца; остальное — со второго раза */
        if (n > room)
            n = room;
        if (!n)
            break;
        rd_bytes(c, c->mf_base + at, c->mf_buf + c->mf_bn, n);
        c->mf_bn += n;
    }
}

static void mf_take(ftc_t *c, uint32_t n)
{
    c->mf_bi += n;
    c->mf_pos += n;
}

static void mf_publish(ftc_t *c)
{
    if (!c->mf_size)
        return;
    uint32_t r = ((c->mf_pos + 3u) & ~3u) % c->mf_size;
    if (r != c->mf_read_pub) {
        wr32(c, REG_MF_READ, r);
        c->mf_read_pub = r;
    }
}

/* Кадр кончился (или файл): слово по ptr, _READ, команда выполнена. */
static void vf_finish(ftc_t *c, int frame)
{
    if (frame)
        mw_end(c);
    wr32(c, c->p[1] & 0x3FFFFCu, frame && c->mf_pos < c->avi_end ? c->avi_end - c->mf_pos : 0);
    mf_publish(c);
    complete(c);
}

/* CMD_VIDEOSTART и CMD_VIDEOFRAME: B_DONE — выполнена, B_WAIT — ждёт данных медиа-FIFO. */
static int video(ftc_t *c)
{
    while (!c->fault) {
        mf_fill(c);
        uint32_t have = c->mf_bn - c->mf_bi;
        const uint8_t *b = c->mf_buf + c->mf_bi;
        switch (c->vf_state) {
        case VS_RIFF:                                /* 'RIFF', длина, 'AVI ' */
            if (have < 12)
                goto wait;
            c->avi_end = le32(b + 4) + 8;
            mf_take(c, 12);
            c->vf_state = VS_HDR;
            break;
        case VS_HDR:
        case VF_HDR: {
            if (have < 8)
                goto wait;
            uint32_t id = le32(b), size = le32(b + 4);
            if (id == FOURCC('L', 'I', 'S', 'T')) {  /* внутрь списка: его тип и дальше чанки */
                if (have < 12)
                    goto wait;
                mf_take(c, 12);
                break;
            }
            mf_take(c, 8);
            c->vf_left = size + (size & 1u);
            if (c->vf_state == VS_HDR) {
                /* чанки заголовка пропускаются (размеры кадра берутся у JPEG); за avih
                   VIDEOSTART выполнена — vf_jpeg здесь признак avih */
                c->vf_state = VS_SKIP;
                c->vf_jpeg = id == FOURCC('a', 'v', 'i', 'h');
                break;
            }
            uint32_t kind = id >> 16;                /* '..dc' — сжатый кадр, '..db' — тоже */
            /* кадр длины 0 (кодировщики так повторяют прошлый) — пропуск, как у bt8xxemu: этот
               же вызов берёт следующий кадр (прошивка 33; 32 давала ошибку IMAGE — Reason 04) */
            if ((kind == ('d' | ('c' << 8)) || kind == ('d' | ('b' << 8))) && size) {
                c->scratch_dirty = 1;                /* декодер — в рабочей памяти 3D */
                ftc_img_begin(FTC_SCRATCH(c), 0, FTC_INFLATE_MEM(c));
                mw_begin(c, c->p[0] & 0x3FFFFCu);
                c->vf_jpeg = size;
                c->vf_state = VF_DEC;
            } else {
                c->vf_state = VF_SKIP;
            }
            break;
        }
        case VS_SKIP:
        case VF_SKIP:
        case VF_TAIL: {
            uint32_t n = have < c->vf_left ? have : c->vf_left;
            mf_take(c, n);
            c->vf_left -= n;
            if (c->vf_left)
                goto wait;
            if (c->vf_state == VS_SKIP) {
                if (c->vf_jpeg) {                    /* avih пройден: VIDEOSTART выполнена */
                    mf_publish(c);
                    complete(c);
                    return B_DONE;
                }
                c->vf_state = VS_HDR;
            } else if (c->vf_state == VF_TAIL) {
                vf_finish(c, 1);
                return B_DONE;
            } else if (c->mf_pos >= c->avi_end) {
                vf_finish(c, 0);                     /* файл кончился, кадра нет */
                return B_DONE;
            } else {
                c->vf_state = VF_HDR;
            }
            break;
        }
        case VF_DEC: {
            uint32_t n = have < c->vf_jpeg ? have : c->vf_jpeg;
            size_t used = 0;
            int r = n ? ftc_img_run(FTC_SCRATCH(c), b, n, &used, inflate_out, c) : FTC_IMG_NEED_INPUT;
            mf_take(c, (uint32_t)used);
            c->vf_jpeg -= (uint32_t)used;
            c->vf_left -= (uint32_t)used;
            if (r == FTC_IMG_ERROR || (r == FTC_IMG_NEED_INPUT && !c->vf_jpeg)) {
                fault(c, FTC_FAULT_IMAGE);           /* испорченный или оборванный кадр */
                return B_DONE;
            }
            if (r == FTC_IMG_DONE) {
                c->vf_state = VF_TAIL;               /* хвост чанка за EOI и выравнивание */
                break;
            }
            if (!used)
                goto wait;
            break;
        }
        default:
            fault(c, FTC_FAULT_UNKNOWN);
            return B_DONE;
        }
        continue;
    wait:
        mf_publish(c);
        return B_WAIT;
    }
    return B_DONE;
}

/* Начало команды (параметры сняты). 1 — команда выполнена целиком. */
static int begin(ftc_t *c)
{
    uint32_t *p = c->p;
    switch (c->cmd) {
    case CMD_SWAP:
        dl_flush(c);
        wr32(c, FTC_REG_DLSWAP, 2);
        return 1;
    case CMD_INTERRUPT:
        c->t0 = c->bus.ms ? c->bus.ms(c->bus.ctx) : 0;
        return 0;
    case CMD_NUMBER:                  /* как CMD_TEXT: может ждать ПЗУ-шрифтов — в body() */
        return 0;
    case CMD_TEXT:
        c->txt_n = 0;                 /* строка — в body() */
        return 0;
    case CMD_SETFONT:
        if (p[0] < 16)
            c->font_ptr[p[0]] = p[1];
        return 1;
    case CMD_BGCOLOR: c->bg = p[0] & 0xFFFFFFu; return 1;
    case CMD_FGCOLOR: c->fg = p[0] & 0xFFFFFFu; return 1;
    case CMD_GRADCOLOR: c->grad = p[0] & 0xFFFFFFu; return 1;
    case CMD_COLDSTART: defaults(c); return 1;
    case CMD_SETBASE: c->base = p[0]; return 1;
    case CMD_SETSCRATCH: c->scratch = p[0]; return 1;
    case CMD_SETROTATE:
        c->rotate = p[0];
        wr32(c, FTC_REG_ROTATE, p[0]);
        return 1;
    case CMD_GETPROPS:
        result(c, 0, c->img_addr);
        result(c, 1, c->img_w);
        result(c, 2, c->img_h);
        return 1;
    case CMD_MEMCRC:
        result(c, 2, p[1] ? mem_crc(c, p[0], p[1]) : 0);
        return 1;
    case CMD_REGREAD:
        result(c, 1, rd32(c, p[0] & ~3u));
        return 1;
    case CMD_MEMWRITE:
        c->total = p[1];
        mw_begin(c, p[0]);
        if (!c->total) {
            mw_end(c);
            return 1;
        }
        return 0;
    case CMD_MEMSET:
    case CMD_MEMZERO:
        mw_begin(c, p[0]);
        if (c->cmd == (uint32_t)CMD_MEMSET)
            mw_fill(c, (uint8_t)p[1], p[2]);
        else
            mw_fill(c, 0, p[1]);
        mw_end(c);
        return 1;
    case CMD_MEMCPY:
        mem_copy(c, p[0], p[1], p[2]);
        return 1;
    case CMD_APPEND:
        append(c, p[0], p[1]);
        return !c->fault;
    case CMD_INFLATE:
        c->scratch_dirty = 1;            /* inflate — в общей рабочей памяти */
        ftc_inflate_begin(FTC_INFLATE_MEM(c));
        mw_begin(c, p[0]);
        c->done = 0;
        return 0;
    case CMD_LOADIMAGE:
        if (p[1] & 16u) {                /* OPT_MEDIAFIFO: медиа-FIFO здесь ещё нет */
            fault(c, FTC_FAULT_UNKNOWN);
            return 1;
        }
        c->scratch_dirty = 1;            /* состояние декодера — в рабочей памяти 3D */
        ftc_img_begin(FTC_SCRATCH(c), p[1], FTC_INFLATE_MEM(c));
        mw_begin(c, p[0] & 0x3FFFFCu);
        c->done = 0;
        return 0;
    case CMD_GETPTR:
        result(c, 0, c->inflate_end);
        return 1;
    case CMD_MEDIAFIFO:
        c->mf_base = p[0] & 0x3FFFFCu;
        c->mf_size = p[1] & 0x3FFFFCu;
        c->mf_pos = c->mf_bi = c->mf_bn = 0;
        c->mf_read_pub = 0;
        wr32(c, REG_MF_READ, 0);
        wr32(c, REG_MF_WRITE, 0);
        return 1;
    case CMD_VIDEOSTART:
        c->vf_state = VS_RIFF;
        c->vf_jpeg = 0;
        return 0;
    case CMD_VIDEOFRAME:
        c->vf_state = VF_HDR;
        return 0;
    case CMD_LOADIDENTITY:
        memset(c->matrix, 0, sizeof c->matrix);
        c->matrix[0] = c->matrix[4] = 0x10000;
        return 1;
    case CMD_TRANSLATE:              /* обратное к сдвигу на (tx, ty) */
        premul(c->matrix, 65536, 0, -(int64_t)(int32_t)p[0], 0, 65536, -(int64_t)(int32_t)p[1]);
        return 1;
    case CMD_SCALE:
        premul(c->matrix, fdiv(65536, (int32_t)p[0]), 0, 0, 0, fdiv(65536, (int32_t)p[1]), 0);
        return 1;
    case CMD_ROTATE: {               /* по часовой на экране; обратное — назад */
        int64_t s, k;
        sincos16(p[0], &s, &k);
        premul(c->matrix, k, s, 0, -s, k, 0);
        return 1;
    }
    case CMD_SETMATRIX:              /* F первым, A последним (golden cmd-matrix) */
        for (int i = 5; i >= 0; i--) {
            uint32_t mask = (i == 2 || i == 5) ? 0xFFFFFFu : 0x1FFFFu;
            if (!dl_put(c, DL(OP_TRANSFORM_A + i, (uint32_t)(c->matrix[i] >> 8) & mask)))
                return 0;
        }
        return 1;
    case CMD_GETMATRIX:
        for (uint32_t i = 0; i < 6; i++)
            result(c, i, (uint32_t)c->matrix[i]);
        return 1;
    case CMD_SETBITMAP: {            /* SOURCE, SIZE_H, SIZE, LAYOUT_H, LAYOUT (golden cmd-fonts) */
        uint32_t fmt = p[1] & 0xFFFF, w = p[1] >> 16, h = p[2] & 0xFFFF;
        uint32_t stride = (w * bpp(fmt) + 7) / 8;
        const uint32_t words[] = {DL(OP_BITMAP_SOURCE, p[0] & 0x3FFFFFu), w_size_h(w, h), w_size(w, h),
                                  w_layout_h(stride, h), w_layout(fmt, stride, h)};
        dl_words(c, words, 5);
        return !c->fault;
    }
    case CMD_GRADIENT:
        gradient(c);
        return !c->fault;
    case CMD_PROGRESS:
        progress(c);
        return !c->fault;
    case CMD_DLSTART:
        return 0;
    default:
        if ((c->cmd & 0xFFFFFFF0u) == 0xFFFFFFA0u) {   /* 3D VDAC4 (ft_3d.c) */
            dl_flush(c);
            mw_flush(c);
            if (c->scratch_dirty) {          /* рабочую память занимала картинка или обновление */
                ftc3d_scratch_lost(&c->g3d);
                c->scratch_dirty = 0;
            }
            if (!ftc3d_exec(c))
                fault(c, FTC_FAULT_3D);
            return !c->fault;
        }
        return 1;                    /* без действия */
    }
}

/* Продолжение команды. */
static int body(ftc_t *c)
{
    switch (c->cmd) {
    case CMD_VIDEOSTART:
    case CMD_VIDEOFRAME:
        return video(c);
    case CMD_DLSTART: {              /* ждёт, пока прошлый лист не сменится [PG §5.11] */
        ftc_status_t st;
        c->bus.status(c->bus.ctx, &st);
        if ((st.dlswap & 3u) || (st.cpureset & 1u))
            return B_WAIT;                 /* сброс — разберёт следующий опрос */
        dl_flush(c);
        c->dl = 0;
        c->dl_full = 0;
        complete(c);
        return B_DONE;
    }
    case CMD_INTERRUPT:
        if (c->bus.ms && c->bus.ms(c->bus.ctx) - c->t0 < c->p[0])
            return B_WAIT;
        if (c->bus.irq)
            c->bus.irq(c->bus.ctx, FTC_INT_CMDFLAG);
        complete(c);
        return B_DONE;
    case CMD_MEMWRITE: {
        uint32_t n = data_avail(c), left = c->total - c->done;
        if (n > left)
            n = left;
        while (n) {
            uint32_t at = (c->rp + c->roff) & RING_MASK, k = 4096 - at;
            if (k > n)
                k = n;
            mw_put(c, ring_bytes(c) + at, k);
            data_consume(c, k);
            c->done += k;
            n -= k;
        }
        if (c->done < c->total)
            return B_DATA;
        mw_end(c);
        complete(c);
        return B_DONE;
    }
    case CMD_NUMBER:
        if (!text_ready(c))
            return B_WAIT;
        number_text(c);
        text_emit(c);
        complete(c);
        return B_DONE;
    case CMD_TEXT:                  /* строка до 0, затем выравнивание до слова (complete) */
        for (;;) {
            if (!data_avail(c))
                return B_DATA;
            uint8_t ch = ring_bytes(c)[(c->rp + c->roff) & RING_MASK];
            if (ch) {
                if (c->txt_n < FTC_TEXT_MAX)
                    c->txt[c->txt_n++] = ch;
                data_consume(c, 1);
                continue;
            }
            if (!text_ready(c))
                return B_WAIT;          /* образ ПЗУ-шрифтов ещё не залит */
            data_consume(c, 1);
            text_emit(c);
            complete(c);
            return B_DONE;
        }
    case CMD_LOADIMAGE:
        for (;;) {
            uint32_t n = data_avail(c);
            if (!n)
                return B_DATA;
            uint32_t at = (c->rp + c->roff) & RING_MASK, k = 4096 - at;
            if (k > n)
                k = n;
            size_t used = 0;
            int r = ftc_img_run(FTC_SCRATCH(c), ring_bytes(c) + at, k, &used, inflate_out, c);
            data_consume(c, (uint32_t)used);
            if (r == FTC_IMG_ERROR) {
                fault(c, FTC_FAULT_IMAGE);
                return B_DONE;
            }
            if (r == FTC_IMG_DONE) {
                mw_end(c);
                loadimage_done(c);
                complete(c);
                return B_DONE;
            }
            if (used < k)
                return B_DATA;
        }
    case CMD_INFLATE:
        for (;;) {
            uint32_t n = data_avail(c);
            if (!n)
                return B_DATA;
            uint32_t at = (c->rp + c->roff) & RING_MASK, k = 4096 - at;
            if (k > n)
                k = n;
            size_t used = 0;
            int r = ftc_inflate_run(FTC_INFLATE_MEM(c), ring_bytes(c) + at, k, &used, inflate_out, c);
            data_consume(c, (uint32_t)used);
            if (r == FTC_INF_ERROR) {
                fault(c, FTC_FAULT_INFLATE);
                return B_DONE;
            }
            if (r == FTC_INF_DONE) {
                mw_end(c);
                c->inflate_end = c->p[0] + c->done;
                complete(c);
                return B_DONE;
            }
            if (used < k)
                return B_DATA;
        }
    default:
        complete(c);
        return B_DONE;
    }
}

/* Всё, что можно выполнить сейчас. 1 — ждёт события (смена листа, время). */
static int run(ftc_t *c)
{
    while (!c->fault) {
        if (c->cmd) {
            int r = body(c);
            if (r == B_DATA)
                return 0;
            if (r == B_WAIT)
                return 1;
            continue;
        }
        uint32_t n = ring_words(c);
        if (!n)
            return 0;
        uint32_t w = ring_word(c, 0);
        if ((w & 0xFFFFFF00u) != 0xFFFFFF00u) {   /* слово дисплей-листа [PG §5.3] */
            if (!dl_put(c, w))
                return 0;
            c->rp = (c->rp + 4) & RING_MASK;
            continue;
        }
        int np = cmd_params(w);
        if (np < 0) {
            c->cmd = w;
            fault(c, FTC_FAULT_UNKNOWN);
            return 0;
        }
        if (n < 1u + (uint32_t)np)
            return 0;                              /* параметры ещё не пришли */
        c->cmd = w;
        c->cmd_at = c->rp;
        for (int i = 0; i < np; i++)
            c->p[i] = ring_word(c, 1 + (uint32_t)i);
        c->rp = (c->rp + 4 * (1 + (uint32_t)np)) & RING_MASK;
        c->roff = 0;
        c->done = c->total = 0;
        if (begin(c) && !c->fault)
            complete(c);
    }
    return 0;
}

/* Сделанное — в ПЛИС: слова листа и памяти, REG_CMD_DL, затем REG_CMD_READ. */
static void publish(ftc_t *c)
{
    dl_flush(c);
    mw_flush(c);
    if (c->dl != c->dl_pub) {
        wr32(c, FTC_REG_CMD_DL, c->dl);
        c->dl_pub = c->dl;
    }
    uint32_t rp = c->fault ? 0xFFFu : c->rp;
    if (rp != c->rp_pub) {
        wr32(c, FTC_REG_CMD_READ, rp);
        c->rp_pub = rp;
    }
}

int ftc_poll(ftc_t *c)
{
    ftc_status_t st;
    c->bus.status(c->bus.ctx, &st);
    if (st.cpureset & 1u) {                       /* в сбросе: команда в работе брошена */
        c->held = 1;
        c->cmd = 0;
        c->roff = 0;
        c->dlq_n = 0;
        c->mw_n = 0;
        c->mw_mask = 0;
        return 0;
    }
    if (c->held || st.resets != c->resets) {      /* из сброса — с REG_CMD_READ [PG §5.6] */
        c->held = 0;
        c->resets = st.resets;
        c->cmd = 0;
        c->roff = 0;
        c->dlq_n = 0;
        c->mw_n = 0;
        c->mw_mask = 0;
        c->fault = 0;
        defaults(c);
        c->rp = c->fetched = st.cmd_read & 0xFFCu;
        c->rp_pub = st.cmd_read;
        c->dl = c->dl_pub = st.cmd_dl & 0x1FFFu;
        c->dl_full = 0;
        c->empty_raised = 0;
    }
    if (c->fault)
        return 0;
    if (!c->cmd && c->rp == c->wp && (st.cmd_dl & 0x1FFFu) != c->dl_pub) {
        c->dl = c->dl_pub = st.cmd_dl & 0x1FFFu;  /* стоял — хост переписал REG_CMD_DL */
        c->dl_full = 0;
    }
    c->wp = st.cmd_write & 0xFFCu;
    fetch(c);
    int wait = run(c);
    publish(c);
    if (c->fault)
        return 0;
    if (!c->cmd && c->rp == c->wp) {
        if (!c->empty_raised && c->bus.irq)
            c->bus.irq(c->bus.ctx, FTC_INT_CMDEMPTY);
        c->empty_raised = 1;
        return 0;
    }
    c->empty_raised = 0;
    return wait || c->rp != c->wp || c->cmd;
}
