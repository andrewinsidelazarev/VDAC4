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
 *
 * Прошивка 39 (04.11.2026, задание task_bl616_images.md — браузер TSafari):
 * - CMD_LOADIMAGE_BOX (ft_copro.c): картинка в рамку max_w × max_h и в room байтов RAM_G с ptr.
 *   Влезает как есть — выход как у CMD_LOADIMAGE; иначе уменьшение по площади (см. «Уменьшение
 *   по площади») в RGB565, L8 (серый) или ARGB4 (есть прозрачность). Источник — до 65535 по
 *   стороне (предел — память строк).
 * - Причина ошибки (FTC_IMG_E_*): ft_copro.c кладёт её в RAM_CMD до сброса сопроцессора.
 * - Буферы — из всей рабочей памяти сопроцессора (FTC_IMG_BYTES и у JPEG ещё место inflate),
 *   по размерам картинки (пул pool_take); что не влезает (черновик) — в RAM_G за выходом в
 *   пределах room (draft_wr/draft_rd).
 * - OPT_FULLSCREEN с уменьшением больше 4 раз (ядро фильтра Pillow не помещается) — тем же
 *   уменьшением по площади на холст (раньше — ошибка).
 *
 * Прошивка 40: WebP (VP8, VP8 с альфой, VP8L, первый кадр анимации) — libwebp, см. раздел «WebP».
 */
#include <stddef.h>
#include <string.h>

#include "ft_copro.h"

/* WebP: декодер libwebp 1.5.0 (third_party/libwebp, правки — README.VDAC4.md) */
#include "src/dec/vp8i_dec.h"
#include "src/dec/vp8li_dec.h"
#include "src/dec/webpi_dec.h"
#include "src/dsp/dsp.h"

#define JB_BYTES  8192u           /* буфер сжатых данных JPEG */
#define JB_NEED   3072u           /* столько — на одну MCU с запасом */
#define IMG_WMAX  2048u           /* ширина картинки — до 2047 (поле BITMAP_SIZE_H) */
#define SRC_MAX   65535u          /* источник с уменьшением (рамка, вписывание): JPEG — до 65535
                                     (задание — не меньше 8192), предел — память строк */
#define FIT_W     1024u           /* OPT_FULLSCREEN: холст */
#define FIT_H     768u
#define FIT_KMAX  9               /* отсчётов фильтра на точку (уменьшение до 4 раз) */

enum { FMT_ARGB4 = 6, FMT_RGB565 = 7, FMT_L8 = 3, FMT_PAL565 = 14, FMT_PAL4444 = 15 };
enum { OPT_MONO = 1, OPT_NODL = 2, OPT_FULLSCREEN = 8, OPT_MEDIAFIFO = 16 };
/* Выход: как у FT812 (и рамка без уменьшения); вписывание Pillow (OPT_FULLSCREEN); по площади */
enum { OUT_RAW, OUT_FIT, OUT_AREA };
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

/* Уменьшение по площади (прошивка 39): см. раздел ниже */
typedef struct area {
    uint32_t sw, sh, ow, oh;      /* источник и выход */
    uint32_t cw, ch, x0, y0;      /* холст (OPT_FULLSCREEN): размер и место картинки; иначе cw = ow */
    int      nch;                 /* каналов входа: 1 — серый, 3 — RGB, 4 — RGBA (не предумноженный) */
    uint32_t fmt;                 /* выход: FMT_L8, FMT_RGB565, FMT_ARGB4 */
    int      black;               /* прозрачность — на чёрный (холст) */
    uint32_t sy, oy;              /* строк источника принято, строк выхода выдано */
    int      mode;                /* вход: AM_GRAY, AM_RGB, AM_RGBA, AM_GA, AM_PAL, AM_RGBX */
    const uint32_t *pal;          /* AM_PAL: 4 слова на индекс (у прозрачной — предумноженные) */
    float   *vacc;                /* суммы по y нынешней строки выхода: ow × nch */
    uint8_t *src;                 /* строка источника sw × nch (её заполняет декодер, если нужно) */
    uint8_t *row;                 /* строка выхода (холста) */
} area_t;

/* --------------------------------------------------------------------------------------- */
/* JPEG                                                                                    */
/* --------------------------------------------------------------------------------------- */

typedef struct {
    uint8_t  look_len[512];      /* 9 разрядов вперёд: длина кода (0 — длиннее 9) */
    uint8_t  look_val[512];
    int32_t  maxcode[18];        /* как в libjpeg (jdhuff.c) */
    int32_t  valoff[18];
    uint8_t  vals[256];
    uint8_t  counts[16];         /* исходные длины кодов (повтор прогрессивного из копии) */
    int      nvals;
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
    huff_t   hdc[4], hac[4];      /* таблицы 0…3 (у baseline обычно 0 и 1) */
    jcomp_t  comp[3];
    int      ncomp, hmax, vmax;
    uint32_t w, h, mcux, mcuy;   /* MCU по горизонтали и вертикали */
    int      m;                  /* сторона блока яркости после ДКП (libjpeg min_DCT_scaled_size) */
    uint32_t ow, oh;             /* размер на выходе ДКП (output_width/height) */
    int      omode;              /* выход: OUT_* */
    uint32_t tw, th;             /* OUT_AREA: размер выхода */
    int      started;            /* выход открыт (первый SOS) */
    int      h2, v2;             /* цветность вдвое уже / ниже яркости после ДКП */
    int      hrep, vfancy;       /* по x — повтор, а не «гладко»; по y — «гладко» (нужен сосед) */
    fit_t   *fit;                /* OPT_FULLSCREEN */
    area_t  *area;               /* уменьшение по площади */
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
    uint8_t *arena;              /* плоскости и строки (layout) — из пула, по размеру картинки */
    /* прогрессивный (SOF2, прошивка 39): см. раздел ниже */
    int      prog, pmode, pphase, replay, copying;
    int      ns, scomp[3], ss, se, ah, al;   /* развёртка: компоненты, полоса спектра, приближение */
    uint32_t eobrun;
    int      sdone, skip, scan_end;
    uint8_t  end_marker;
    uint32_t sbx, sby;           /* развёртка одной компоненты: следующий блок */
    uint32_t cbw[3], cbh[3];     /* блоков компоненты (по MCU с запасом) */
    uint32_t nbw[3], nbh[3];     /* без запаса — развёртка одной компоненты */
    int16_t *coef[3];            /* PM_FULL — все блоки, PM_STRIPE — блоки полосы, PM_DC — DC */
    struct jckpt *ckpt;          /* PM_STRIPE: закладки развёрток (где в копии начинается полоса) */
    int      nscan, cur;         /* развёрток в нынешнем разборе; номер нынешней */
    int      seek;               /* повтор: перейти в копии на seek_to */
    uint32_t seek_to, jb_base;   /* место в копии: куда; jb[0] */
    int64_t  in_base;            /* место в копии байта in[0] нынешнего вызова */
    uint32_t lo, hi, srows;      /* полоса — строки MCU [lo, hi); строк в полосе */
    uint32_t orow;               /* PP_OUT: следующая строка MCU */
    struct jtabs *tabs;          /* PM_STRIPE: таблицы на конец SOF */
    uint8_t *fcopy;              /* копия файла в пуле (NULL — в RAM_G с fbase) */
    uint32_t fbase, fcap, fsize, rp;
    size_t   copy_from;          /* копия: с какого байта нынешнего вызова */
    uint8_t *rbuf;               /* повтор из RAM_G: кусок копии */
    uint8_t  fstage[256];        /* копия в RAM_G: хвост до 256 байт */
    uint32_t fstage_n;
    uint8_t  jb[JB_BYTES];
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
    int      ctype, depth;       /* тип цвета (0, 2, 3, 4, 6) и глубина (1, 2, 4, 8, 16) */
    int      ch;                 /* каналов в файле: 1 серый или индекс, 2 серый+a, 3 RGB, 4 RGBA */
    int      bpp;                /* байтов на точку для фильтра (не меньше 1) */
    uint32_t bits;               /* разрядов на точку */
    int      interlace;          /* Adam7 */
    uint8_t  plte[768];
    uint32_t npal;
    uint8_t  trns[256];
    uint32_t ntrns;
    uint16_t key[3];             /* tRNS вида 0 и 2: прозрачный цвет во всю глубину */
    int      has_key;            /* tRNS вида 0 и 2 есть */
    int      keyed;              /* прозрачность по key учитывается (рамка, вписывание) */
    int      nch;                /* каналов нормированной строки (по 8 бит; key — +альфа) */
    int      started;            /* решение о выходе принято (первый IDAT), идут строки */
    int      out;                /* OUT_* */
    int      fmt;                /* формат выхода OUT_RAW */
    fit_t   *fit;                /* OPT_FULLSCREEN */
    area_t  *area;               /* уменьшение по площади */
    uint8_t *infl;               /* состояние tinfl (c->inflate) */
    int      infl_done;
    int      lcur;
    uint32_t lgot;               /* байтов нынешней строки фильтра */
    uint32_t y;                  /* строк выдано (без Adam7 — и принято) */
    /* строки фильтра нынешнего прохода (у Adam7 — прохода): ширина, строк, байтов строки */
    int      pass;               /* проход Adam7 0…6 (без Adam7 — 0); 7 — все приняты */
    uint32_t pw, ph, prow, prowb;
    /* Adam7: проходы — черновиком (нормированные строки) в пуле или в RAM_G с draft_base */
    uint32_t poff[7], pstride[7];
    uint8_t *draft;              /* NULL — черновик в RAM_G */
    uint32_t draft_base;
    int      assembling;         /* проходы приняты: собирать строки (FTC_IMG_BUSY) */
    uint8_t *line[2];            /* байт фильтра + строка: нынешняя и прошлая */
    uint8_t *norm;               /* нормированная строка картинки: w × nch */
    uint8_t *prow8;              /* нормированная строка прохода */
    uint8_t *out_row;            /* строка выхода (RAW: формат FT812) */
    int      err;
} png_t;

/* --------------------------------------------------------------------------------------- */
/* WebP (прошивка 40)                                                                      */
/* --------------------------------------------------------------------------------------- */

typedef struct {
    int      state;              /* W_* */
    uint8_t  hdr[12];            /* «RIFF», длина, «WEBP» */
    uint32_t hdr_got;
    uint8_t *file;               /* файл целиком — в пуле (не far) */
    uint32_t size, got;          /* длина файла (8 + длина RIFF), принято */
    int      far;                /* рамка, файл больше WP_NEAR_MAX: файл — в RAM_G в конце room */
    uint32_t base;               /* его место в RAM_G (от ptr); выход — до него */
    uint8_t  stage[256];         /* приём в RAM_G словами (draft_wr — кратно 4) */
    uint32_t staged, flushed;
    uint32_t img_off, alph_off;  /* места блоков в файле */
    uint32_t parts_off;          /* VP8 из RAM_G: таблица размеров разделов токенов — место в файле */
    uint8_t *win;                /* VP8 из RAM_G: окна разделов (MAX_NUM_PARTITIONS × WP_WIN) */
    uint32_t ppos[MAX_NUM_PARTITIONS], pend[MAX_NUM_PARTITIONS];   /* разделы: следующий байт, конец */
    uint8_t *pool0;              /* начало пула (сколько нужно памяти — от него) */
    int      lossless;           /* VP8L */
    int      alpha;              /* у выхода прозрачность: флаг VP8X, у простого VP8L — его подсказка */
    uint32_t cw, ch;             /* холст */
    uint32_t fx, fy, fw, fh;     /* кадр на холсте (у анимации — первый; иначе весь холст) */
    const uint8_t *img;          /* полезная нагрузка VP8 / VP8L */
    uint32_t img_size;
    const uint8_t *alph;         /* ALPH (у VP8) */
    uint32_t alph_size;
    uint32_t y;                  /* строк холста выдано */
    uint32_t fy_out;             /* строк кадра выдано */
    int      out;                /* OUT_* */
    fit_t   *fit;
    area_t  *area;
    uint8_t *canvas;             /* строка холста RGBA (кадр меньше холста) */
    uint8_t *frow;               /* VP8L: строка кадра RGBA из слов ARGB */
    uint8_t *out_row;            /* строка выхода (RAW) */
    uint8_t *pair;               /* VP8: две строки кадра RGBA */
    uint8_t *tmp_y, *tmp_u, *tmp_v;   /* VP8: строка, отложенная «гладким» восстановлением цветности */
    uint8_t *heap;               /* с этого места пул — память libwebp (стопкой, VDAC4WebPAlloc) */
    uint32_t want;               /* libwebp не хватило: нужно байтов пула (оценка — от pool0) */
    uint32_t lrow;               /* VP8L: строк разобрано */
    VP8Io    io;
    WebPDecParams params;        /* VP8L: io.opaque */
    WebPDecBuffer buf;           /* VP8L: только вид точек и высота (строки — в wp_lrows) */
    VP8Decoder  *vp8;
    VP8LDecoder *vp8l;
} webp_t;

typedef struct {
    int kind;                    /* 0 — не известно, 1 — JPEG, 2 — PNG, 3 — WebP */
    ftc_img_info_t info;
    /* запрос (ftc_img_begin_req) */
    uint32_t opts;
    int      boxed;
    uint32_t box_w, box_h, room;
    void   (*draft_wr)(void *u, uint32_t off, const uint8_t *p, size_t n);
    void   (*draft_rd)(void *u, uint32_t off, uint8_t *p, size_t n);
    /* выход нынешнего вызова ftc_img_run */
    void   (*emit)(void *u, const uint8_t *p, size_t n);
    void    *eu;
    uint8_t *infl;               /* память inflate (FTC_INFLATE_BYTES): у JPEG — пул */
    uint8_t *pool_at, *pool_end; /* пул буферов: остаток FTC_IMG_BYTES за img_t (у JPEG — и infl) */
    uint32_t out_bytes;          /* выход в RAM_G (рамка): черновик — за ним, до room */
    fit_t    fit;
    area_t   area;
    union {
        jpeg_t j;
        png_t  p;
        webp_t w;
    } u;
} img_t;

_Static_assert(sizeof(img_t) <= FTC_IMG_BYTES, "FTC_IMG_BYTES");

#define IMG_OF_J(j) ((img_t *)((uint8_t *)(j) - offsetof(img_t, u)))
#define IMG_OF_P(p) ((img_t *)((uint8_t *)(p) - offsetof(img_t, u)))
#define IMG_OF_W(w) ((img_t *)((uint8_t *)(w) - offsetof(img_t, u)))

/* Причина ошибки — первая (дальше обычно следствия) */
static void img_err(img_t *im, uint32_t code, uint32_t arg)
{
    if (!im->info.err) {
        im->info.err = code;
        im->info.err_arg = arg;
    }
}

/* Буфер из пула (по 4 байта); NULL — не хватило */
static void *pool_take(img_t *im, size_t bytes)
{
    bytes = (bytes + 3u) & ~(size_t)3;
    if ((size_t)(im->pool_end - im->pool_at) < bytes)
        return NULL;
    void *p = im->pool_at;
    im->pool_at += bytes;
    return p;
}

static uint32_t pool_free(const img_t *im)
{
    return (uint32_t)(im->pool_end - im->pool_at);
}

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

/* Вписывание из пула: веса и кольцо — в его остатке; 0 — не помещается или ядро больше FIT_KMAX */
static int fit_init_pool(img_t *im, fit_t *f, uint32_t w, uint32_t h, uint32_t iw, uint32_t ih)
{
    span_t sp[1] = {{im->pool_at, im->pool_end}};
    if (!fit_init(f, w, h, iw, ih, sp, 1))
        return 0;
    im->pool_at = sp[0].a;
    return 1;
}

/* ---- Уменьшение по площади (прошивка 39) ----------------------------------------------- */
/*
 * CMD_LOADIMAGE_BOX и вписывание с уменьшением больше 4 раз: каждая точка источника входит в
 * свои точки выхода с долей по площади пересечения (Pillow Image.resize BOX). Точка x источника
 * — отрезок [x·ow, (x+1)·ow), точка X выхода — [X·sw, (X+1)·sw) в единицах 1/(sw·ow) строки;
 * выход не больше источника, поэтому у точки выхода — первая точка источника с долей, внутренние
 * целиком (доля ow — складываются без умножения) и последняя с долей. Так же по y. Строки
 * источника идут по одной: суммы по x — целые (Σ значение·доля, Σ долей = sw; у прозрачных —
 * предумноженное c·a ≤ 65025, и 65025·65535 < 2^32), суммы по y — float (BL616: FPU одинарной
 * точности, ошибка — сотые доли ступени 8 бит); строка выхода готова, когда её отрезок по y
 * закрыт, и делится на площадь sw·sh. Прозрачность — в предумноженном виде (цвет × альфа), иначе
 * по краям темнеет; на холсте (OPT_FULLSCREEN) — на чёрный, как у вписывания. Каналы в формат
 * FT812 — округлением до 8 бит и усечением, как у CMD_LOADIMAGE. Вход — строка декодера как есть
 * (серый, RGB, RGBA, серый с альфой, индексы палитры — по предумноженной таблице).
 * Модель BL616 (rvsim, 04.11.2026): так втрое быстрее, чем float по каждой точке.
 * Прошивка 41: всё — один проход по строке источника. Сумма по x точки выхода сразу ложится в
 * суммы по y (vacc), без массива сумм строки и второго прохода; последняя строка источника у строки
 * выхода сразу же выдаёт точку выхода и кладёт в vacc свою долю следующей строки (без прохода по
 * vacc на выдачу и обнуления); границы точек по x — на ходу, без массива. Раньше массивы строки (у
 * 829 точек RGB ~35 КБ) не помещались в кэш данных BL616 (16 КБ), и каждая строка источника
 * выметала его. Порядок и точность сложений прежние — выход бит в бит тот же.
 */
enum { AM_GRAY, AM_RGB, AM_RGBA, AM_GA, AM_PAL, AM_RGBX };   /* вход: байтов на точку 1, 3, 4, 2, 1, 4 */

static int area_init(img_t *im, area_t *a, uint32_t sw, uint32_t sh, uint32_t ow, uint32_t oh, int nch,
                     uint32_t fmt, int canvas)
{
    memset(a, 0, sizeof *a);
    if (!ow || !oh || ow > sw || oh > sh) {
        img_err(im, FTC_IMG_E_TOO_BIG, 0);   /* увеличивать по площади нельзя */
        return 0;
    }
    a->sw = sw;
    a->sh = sh;
    a->ow = ow;
    a->oh = oh;
    a->nch = nch;
    a->mode = nch == 1 ? AM_GRAY : nch == 3 ? AM_RGB : AM_RGBA;
    a->fmt = fmt;
    a->black = canvas;
    a->cw = canvas ? FIT_W : ow;
    a->ch = canvas ? FIT_H : oh;
    a->x0 = canvas ? (FIT_W - ow) / 2 : 0;
    a->y0 = canvas ? (FIT_H - oh) / 2 : 0;
    uint32_t obpp = fmt == FMT_L8 ? 1u : 2u;
    size_t need = (size_t)ow * (size_t)nch * sizeof(float) + (size_t)sw * (size_t)nch + a->cw * obpp + 8u;
    if (pool_free(im) < need) {
        img_err(im, FTC_IMG_E_TOO_BIG, (uint32_t)need);
        return 0;
    }
    a->vacc = pool_take(im, (size_t)ow * (size_t)nch * sizeof(float));
    a->src = pool_take(im, (size_t)sw * (size_t)nch);
    a->row = pool_take(im, (size_t)a->cw * obpp + 4u);
    memset(a->vacc, 0, (size_t)ow * (size_t)nch * sizeof(float));
    return 1;
}

/* Поля холста: верхнее — сразу, нижнее — за последней строкой */
static void area_margin(const area_t *a, uint32_t rows, void (*emit)(void *, const uint8_t *, size_t), void *u)
{
    memset(a->row, 0, 2u * a->cw);
    for (uint32_t y = 0; y < rows; y++)
        emit(u, a->row, 2u * a->cw);
}

static void area_start(area_t *a, void (*emit)(void *, const uint8_t *, size_t), void *u)
{
    if (a->y0)
        area_margin(a, a->y0, emit, u);
}

static inline uint32_t to8(float v)
{
    return v <= 0.0f ? 0u : v >= 254.5f ? 255u : (uint32_t)(v + 0.5f);
}

/* Точка выхода из сумм по площади v (n каналов; k — 1 / площадь sw·sh) — в формат FT812 по o */
static inline uint8_t *area_px(const area_t *a, const float *v, int n, float k, uint8_t *o)
{
    uint32_t r, g, b, al = 255;
    if (n == 1) {
        r = g = b = to8(v[0] * k);
    } else if (n == 3) {
        r = to8(v[0] * k);
        g = to8(v[1] * k);
        b = to8(v[2] * k);
    } else if (a->black) {                   /* холст: предумноженный цвет и есть цвет на чёрном */
        r = to8(v[0] * k * (1.0f / 255.0f));
        g = to8(v[1] * k * (1.0f / 255.0f));
        b = to8(v[2] * k * (1.0f / 255.0f));
    } else {
        al = to8(v[3] * k);
        float ia = v[3] > 0.0f ? 1.0f / v[3] : 0.0f;
        r = to8(v[0] * ia);
        g = to8(v[1] * ia);
        b = to8(v[2] * ia);
    }
    if (a->fmt == FMT_L8) {
        *o++ = (uint8_t)r;
    } else {
        uint32_t w = a->fmt == FMT_ARGB4 ? ((al >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)
                                         : ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        *o++ = (uint8_t)w;
        *o++ = (uint8_t)(w >> 8);
    }
    return o;
}

/* Строка выхода (a->row) готова: в выход; за последней — нижнее поле холста */
static void area_emit(area_t *a, void (*emit)(void *, const uint8_t *, size_t), void *u)
{
    emit(u, a->row, (a->fmt == FMT_L8 ? 1u : 2u) * a->cw);
    if (++a->oy == a->oh && a->black && a->y0 + a->oh < a->ch)
        area_margin(a, a->ch - a->y0 - a->oh, emit, u);
}

/* Строка источника по точкам выхода (N каналов): сумма по x точек источника, задевающих точку
   выхода — первая и последняя с долей пересечения, внутренние с долей ow (границы ведутся на ходу,
   без делений и без массива), — сразу в суммы по y с долей f1. Строка источника последняя у
   строки выхода (END): точка выхода готова — сразу в формат FT812 (area_px, по o), а в v — доля f2
   этой строки источника в следующей строке выхода (0, если граница строк ровно на её конце). Вход —
   по режиму LOAD. Суммы по x — целые, те же, что у прошивки 39, по y — то же сложение float. */
#define AREA_ROW(LOAD, N, END)                                                                    \
    for (uint32_t X = 0, x = 0, sb = ow, lo = 0; X < ow; X++, v += (N), lo += sw) {               \
        /* точка выхода — [lo, hi); точка источника x — [sb − ow, sb), в ней lo */               \
        const uint32_t hi = lo + sw;                                                              \
        uint32_t c0, c1, c2, c3, t0, t1, t2, t3, w = sb - lo;    /* sb ≤ lo + ow ≤ hi */          \
        uint32_t i0 = 0, i1 = 0, i2 = 0, i3 = 0;                                                  \
        LOAD(x);                                                                                  \
        t0 = c0 * w; t1 = c1 * w; t2 = c2 * w; t3 = c3 * w;                                      \
        for (x++, sb += ow; sb <= hi; x++, sb += ow) {           /* внутренние — целиком */       \
            LOAD(x);                                                                              \
            i0 += c0; i1 += c1; i2 += c2; i3 += c3;                                               \
        }                                                                                         \
        t0 += i0 * ow; t1 += i1 * ow; t2 += i2 * ow; t3 += i3 * ow;                              \
        if (sb - ow < hi) {                                      /* последняя — с долей */        \
            LOAD(x);                                                                              \
            w = hi - (sb - ow);                                                                   \
            t0 += c0 * w; t1 += c1 * w; t2 += c2 * w; t3 += c3 * w;                              \
        }                                                                                         \
        if (END) {                                                                                \
            float e[4];                                                                           \
            e[0] = v[0] + (float)t0 * f1;                                                         \
            v[0] = (float)t0 * f2;                                                                \
            if ((N) >= 3) {                                                                       \
                e[1] = v[1] + (float)t1 * f1; e[2] = v[2] + (float)t2 * f1;                       \
                v[1] = (float)t1 * f2; v[2] = (float)t2 * f2;                                     \
            }                                                                                     \
            if ((N) == 4) { e[3] = v[3] + (float)t3 * f1; v[3] = (float)t3 * f2; }                \
            o = area_px(a, e, (N), k, o);                                                         \
        } else {                                                                                  \
            v[0] += (float)t0 * f1;                                                               \
            if ((N) >= 3) { v[1] += (float)t1 * f1; v[2] += (float)t2 * f1; }                     \
            if ((N) == 4) v[3] += (float)t3 * f1;                                                 \
        }                                                                                         \
    }
/* Все режимы входа (выбор — раз на строку, не на точку) */
#define AREA_MODES(END)                                                                           \
    switch (a->mode) {                                                                            \
    case AM_GRAY: AREA_ROW(LD_GRAY, 1, END); break;                                               \
    case AM_RGB:  AREA_ROW(LD_RGB, 3, END); break;                                                \
    case AM_RGBX: AREA_ROW(LD_RGBX, 3, END); break;                                               \
    case AM_GA:   AREA_ROW(LD_GA, 4, END); break;                                                 \
    case AM_PAL:                                                                                  \
        if (a->nch == 4) {                                                                        \
            AREA_ROW(LD_PAL, 4, END);                                                             \
        } else {                                                                                  \
            AREA_ROW(LD_PAL, 3, END);                                                             \
        }                                                                                         \
        break;                                                                                    \
    default:      AREA_ROW(LD_RGBA, 4, END); break;                                               \
    }
#define LD_GRAY(x) (c0 = s[x], c1 = c2 = c3 = 0)
#define LD_RGB(x)  (c0 = s[3 * (x)], c1 = s[3 * (x) + 1], c2 = s[3 * (x) + 2], c3 = 0)
#define LD_RGBA(x) (c3 = s[4 * (x) + 3], c0 = s[4 * (x)] * c3, c1 = s[4 * (x) + 1] * c3, c2 = s[4 * (x) + 2] * c3)
#define LD_GA(x)   (c3 = s[2 * (x) + 1], c0 = c1 = c2 = s[2 * (x)] * c3)
#define LD_PAL(x)  (pe = pal + 4u * s[x], c0 = pe[0], c1 = pe[1], c2 = pe[2], c3 = pe[3])
#define LD_RGBX(x) (c0 = s[4 * (x)], c1 = s[4 * (x) + 1], c2 = s[4 * (x) + 2], c3 = 0)   /* RGBA без альфы (WebP) */

/* Строка источника s (по a->mode; каналов у режима — a->nch: серый 1, RGB и RGBX 3, прочие 4,
   палитра — 3 или 4) */
static void area_row(area_t *a, const uint8_t *s, void (*emit)(void *, const uint8_t *, size_t), void *u)
{
    if (a->sy >= a->sh)
        return;
    /* по y: строка sy — отрезок [sy·oh, (sy+1)·oh), строка выхода oy — до (oy+1)·sh; oh ≤ sh, поэтому
       строка источника задевает не больше двух строк выхода */
    const uint32_t y0 = a->sy * a->oh, y1 = y0 + a->oh, ybound = (a->oy + 1) * a->sh;
    const uint32_t wy = y1 > ybound ? ybound - y0 : a->oh;
    const float f1 = (float)wy, f2 = (float)(a->oh - wy);
    const uint32_t ow = a->ow, sw = a->sw, *pal = a->pal, *pe;
    const float k = 1.0f / ((float)a->sw * (float)a->sh);
    float *v = a->vacc;
    uint8_t *o = a->row;
    a->sy++;
    if (y1 < ybound) {                           /* строка выхода ещё не закрыта */
        AREA_MODES(0)
    } else {
        if (a->black)
            memset(o, 0, 2u * a->cw);
        o += (a->fmt == FMT_L8 ? 1u : 2u) * a->x0;
        AREA_MODES(1)
        area_emit(a, emit, u);
    }
    (void)pal;
    (void)pe;
}

/* ---- Рамка: размер выхода -------------------------------------------------------------- */

/* 1 — как есть (выход как у CMD_LOADIMAGE, nat байтов); 0 — уменьшить до *tw × *th (bpp байтов
   на точку); -1 — не влезает даже точка (FTC_IMG_E_NO_ROOM) */
static int box_target(img_t *im, uint32_t w, uint32_t h, uint32_t nat, uint32_t bpp, uint32_t *tw, uint32_t *th)
{
    if (w <= im->box_w && h <= im->box_h && w < IMG_WMAX && h < IMG_WMAX && nat <= im->room)
        return 1;
    uint32_t bw = im->box_w < IMG_WMAX ? im->box_w : IMG_WMAX - 1;
    uint32_t bh = im->box_h < IMG_WMAX ? im->box_h : IMG_WMAX - 1;
    uint32_t ow = w, oh = h;
    if (ow > bw || oh > bh) {
        if ((uint64_t)w * bh >= (uint64_t)h * bw) {
            ow = bw;
            oh = (uint32_t)(((uint64_t)h * bw + w / 2) / w);
        } else {
            oh = bh;
            ow = (uint32_t)(((uint64_t)w * bh + h / 2) / h);
        }
    }
    if (!ow)
        ow = 1;
    if (!oh)
        oh = 1;
    /* не влезает в room — меньше рамки, с теми же пропорциями */
    while ((uint64_t)ow * oh * bpp > im->room && (ow > 1 || oh > 1)) {
        uint32_t nw = ow * 15u / 16u, nh = oh * 15u / 16u;
        ow = nw ? nw : 1;
        oh = nh ? nh : 1;
    }
    if ((uint64_t)ow * oh * bpp > im->room) {
        img_err(im, FTC_IMG_E_NO_ROOM, bpp);
        return -1;
    }
    *tw = ow;
    *th = oh;
    return 0;
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
    memmove(t->vals, vals, (size_t)nvals);
    memmove(t->counts, counts, 16);
    t->nvals = nvals;
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

enum { J_SOI0, J_SOI1, J_FF, J_MARK, J_LEN0, J_LEN1, J_BODY, J_SKIP, J_SCAN, J_DONE, J_ERR, J_PASS };

static uint8_t *plane(jpeg_t *j, int ci, uint32_t r)
{
    return j->pl[ci] + r * j->comp[ci].bw;
}

/* Плоскости строки MCU, соседи сверху, отложенная строка, строки восстановления и вывода — в
   arena; у вписывания строки вывода нет (своя строка RGB в fit_t). */
static int layout(jpeg_t *j)
{
    img_t *im = IMG_OF_J(j);
    uint8_t *a = (uint8_t *)(((uintptr_t)im->pool_at + 3u) & ~(uintptr_t)3), *end = im->pool_end;
    j->arena = a;
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
    if (a > end) {
        img_err(im, FTC_IMG_E_TOO_BIG, (uint32_t)(a - j->arena));
        return 0;
    }
    im->pool_at = a;
    return 1;
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

static void jpeg_scale(jpeg_t *j);

static int seg_sof(jpeg_t *j, const uint8_t *s, int n)
{
    img_t *im = IMG_OF_J(j);
    if (n < 6)
        return 0;
    if (s[0] != 8) {
        img_err(im, FTC_IMG_E_UNSUPPORTED, FTC_IMG_U_PRECISION);
        return 0;
    }
    j->h = ((uint32_t)s[1] << 8) | s[2];
    j->w = ((uint32_t)s[3] << 8) | s[4];
    j->ncomp = s[5];
    if (j->ncomp != 1 && j->ncomp != 3) {
        img_err(im, FTC_IMG_E_UNSUPPORTED, FTC_IMG_U_COMPONENTS);
        return 0;
    }
    if (!j->w || !j->h || n < 6 + 3 * j->ncomp)
        return 0;
    im->info.src_w = j->w;
    im->info.src_h = j->h;
    /* с уменьшением (рамка, вписывание) — до SRC_MAX, как есть — до 2047 (BITMAP_SIZE) */
    if ((im->boxed || (j->opts & OPT_FULLSCREEN)) ? (j->w > SRC_MAX || j->h > SRC_MAX)
                                                  : (j->w >= IMG_WMAX || j->h >= IMG_WMAX)) {
        img_err(im, FTC_IMG_E_SIZE, j->w | j->h << 16);
        return 0;
    }
    j->hmax = j->vmax = 1;
    for (int i = 0; i < j->ncomp; i++) {
        jcomp_t *c = &j->comp[i];
        c->id = s[6 + 3 * i];
        c->h = s[7 + 3 * i] >> 4;
        c->v = s[7 + 3 * i] & 15;
        c->tq = s[8 + 3 * i] & 3;
        if (c->h < 1 || c->h > 2 || c->v < 1 || c->v > 2) {
            img_err(im, FTC_IMG_E_UNSUPPORTED, FTC_IMG_U_COMPONENTS);
            return 0;
        }
        if (c->h > j->hmax) j->hmax = c->h;
        if (c->v > j->vmax) j->vmax = c->v;
    }
    if (j->ncomp == 1)
        j->comp[0].h = j->comp[0].v = j->hmax = j->vmax = 1;   /* один канал — блоками 8×8 */
    else if (j->comp[1].h != j->comp[2].h || j->comp[1].v != j->comp[2].v ||
             j->comp[0].h != j->hmax || j->comp[0].v != j->vmax ||
             (j->comp[1].h == j->hmax && j->comp[1].v == j->vmax && j->hmax > 1)) {
        img_err(im, FTC_IMG_E_UNSUPPORTED, FTC_IMG_U_COMPONENTS);
        return 0;      /* цветность — одинаковая и не больше яркости; 2×2 у всех — не нужно */
    }
    j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
    j->mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
    /* Выход и уменьшение ДКП: как Image.draft(size = размер выхода) — наибольшее s из 8, 4, 2, 1,
       не большее min(w / ow, h / oh) нацело. Вписывание — холст (или по площади, если ядро
       фильтра не помещается — решается в SOS); рамка — как есть или по площади. */
    j->m = 8;
    j->omode = OUT_RAW;
    uint32_t fw = 0, fh = 0;
    if (j->opts & OPT_FULLSCREEN) {
        fit_size(j->w, j->h, &fw, &fh);
        j->omode = OUT_FIT;
    } else if (im->boxed) {
        int gray = j->ncomp == 1 || (j->opts & OPT_MONO);
        int r = box_target(im, j->w, j->h, j->w * j->h * ((j->opts & OPT_MONO) ? 1u : 2u), gray ? 1u : 2u,
                           &fw, &fh);
        if (r < 0)
            return 0;
        if (r == 0)
            j->omode = OUT_AREA;
    }
    if (j->omode != OUT_RAW) {
        uint32_t sc = j->w / fw < j->h / fh ? j->w / fw : j->h / fh;
        j->m = sc >= 8 ? 1 : sc >= 4 ? 2 : sc >= 2 ? 4 : 8;
        j->tw = fw;
        j->th = fh;
    }
    jpeg_scale(j);
    j->fit = NULL;
    if (!j->prog && !layout(j))
        return 0;
    return 1;
}

/* Размер после ДКП (j->m), стороны блоков компонент и восстановление цветности */
static void jpeg_scale(jpeg_t *j)
{
    j->ow = (j->w * (uint32_t)j->m + 7) / 8;
    j->oh = (j->h * (uint32_t)j->m + 7) / 8;
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
}

static int seg_dht(jpeg_t *j, const uint8_t *s, int n)
{
    while (n > 0) {
        if (n < 17)
            return 0;
        int tc = s[0] >> 4, th = s[0] & 15, cnt = 0;
        for (int i = 0; i < 16; i++)
            cnt += s[1 + i];
        if (tc > 1 || th > 3 || cnt > 256 || n < 17 + cnt)
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
        if (td > 3 || ta > 3)
            return 0;
        if (!j->hdc[td].ok && (td > 1 || !build_huff(&j->hdc[td], std_dc_bits[td], std_dc_vals, 12)))
            return 0;
        if (!j->hac[ta].ok && (ta > 1 || !build_huff(&j->hac[ta], std_ac_bits[ta], std_ac_vals[ta], 162)))
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
    if (j->area && j->area->nch == 1) {          /* серый — в уменьшение по площади */
        area_row(j->area, yrow, emit, u);
        return;
    }
    if (j->fit || j->area) {                     /* RGB — во вписывание или по площади */
        uint8_t *s = j->fit ? j->fit->src : j->area->src;
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
        if (j->fit)
            fit_row(j->fit, emit, u);
        else
            area_row(j->area, j->area->src, emit, u);
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
    if (!j->vfancy && j->h2 && !j->hrep && !j->fit && !j->area && !(j->opts & OPT_MONO) && cc->dw >= 2) {
        uint32_t r = j->v2 ? ry >> 1 : ry;
        put_row_h2(j, yrow, plane(j, 1, r), plane(j, 2, r), cc->dw, emit, u);
        return;
    }
    /* 4:2:0: h2v2_fancy_upsample — тоже при переводе цвета (put_row_h2v2) */
    const int fast2 = j->vfancy && j->h2 && !j->fit && !j->area && !(j->opts & OPT_MONO) && cc->dw >= 2;
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
    j->eobrun = 0;
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

/* ---- JPEG: прогрессивный (SOF2, прошивка 39) --------------------------------------------- */
/*
 * Развёртки (SOS) идут одна за другой: DC (первая и уточняющие, обычно всех компонент сразу) и AC
 * (по одной компоненте: полоса спектра Ss…Se, последовательное приближение Ah/Al, серии EOB) —
 * как jdphuff.c libjpeg. Коэффициенты копятся по всей картинке; после EOI — обратное ДКП и вывод
 * теми же строками MCU, что у baseline (layout, emit_mcu_row): точки бит в бит с libjpeg (у
 * полной картинки libjpeg сглаживание блоков не включает — все коэффициенты точны).
 * Память:
 * - PM_FULL: коэффициенты всей картинки (64 × 2 байта на блок) — в пуле;
 * - PM_STRIPE: не помещаются — файл копится (в RAM_G за выходом у рамки или в пуле), картинка
 *   выводится полосами строк MCU (FTC_IMG_BUSY): коэффициенты — только у блоков полосы. На конце
 *   полосы у каждой развёртки ставится закладка — место в копии (байт, буфер разрядов, серия EOB,
 *   предсказания DC, счёт RST); следующая полоса идёт по копии заново (маркеры и таблицы — по
 *   порядку), но данные каждой развёртки берёт с закладки, а остаток развёртки перепрыгивает до
 *   её маркера. Так весь файл разбирается примерно один раз. Таблицы на конец SOF запоминаются —
 *   повтор начинается с них;
 * - PM_DC: (рамка, вписывание) и полосами не выходит — только DC: ДКП 1/8, развёртки AC
 *   пропускаются целиком, 2 байта на блок в пуле. Картинка мельче, но целая.
 * CMD_LOADIMAGE — только PM_FULL и PM_STRIPE с копией в пуле; не помещается — FTC_IMG_E_TOO_BIG.
 */
enum { PM_NONE, PM_FULL, PM_STRIPE, PM_DC };
enum { PP_LIVE, PP_OUT, PP_REPLAY };     /* приём, вывод полосы, повтор разбора из копии */
#define PROG_OUT_ROWS     2u             /* строк MCU за вызов при выводе */
#define PROG_REPLAY_BYTES 1024u          /* байтов копии за вызов при повторе (после перехода к закладке
                                            нужен лишь кусок развёртки: 4 КБ — втрое больше чтения SPI) */
#define PROG_PASSES_MAX   64u            /* полос больше — у рамки и вписывания лучше PM_DC */
#define PROG_COPY_EST     12u            /* копия файла: байтов на блок (оценка; сайты — 8…9) */
#define PROG_COPY_POOL    8u             /* то же для копии в пуле (там тесно) */

#define PROG_SCANS_MAX 64                /* развёрток в файле (у libjpeg — 10) */

/* Закладка развёртки: где в копии начинается следующая полоса, и её маркер конца */
typedef struct jckpt {
    uint32_t pos;                /* следующий байт после буфера разрядов */
    uint32_t bits, end;          /* буфер разрядов; место FF маркера конца развёртки */
    int      nbits, hit_marker, rst_left;
    uint32_t eobrun;
    int      pred[3];
    int      done;               /* блоки развёртки кончились раньше полосы */
} jckpt_t;

/* Таблицы на конец SOF: повтор разбора из копии начинается с них */
typedef struct {
    uint8_t  counts[16], vals[256];
    int      nvals, ok;
} jhsrc_t;
typedef struct jtabs {
    uint16_t qt[4][64];
    jhsrc_t  hdc[4], hac[4];     /* исходные байты: таблицы строятся заново (build_huff) */
    int      restart;
} jtabs_t;

static void tabs_save(jhsrc_t *s, const huff_t *t)
{
    s->ok = t->ok;
    s->nvals = t->nvals;
    memcpy(s->counts, t->counts, 16);
    memcpy(s->vals, t->vals, (size_t)t->nvals);
}

static void tabs_load(huff_t *t, const jhsrc_t *s)
{
    t->ok = 0;
    if (s->ok)
        build_huff(t, s->counts, s->vals, s->nvals);
}

/* Коэффициенты блока (ci, bx, by); NULL — блок вне полосы */
static int16_t *prog_coef(jpeg_t *j, int ci, uint32_t bx, uint32_t by)
{
    if (j->pmode == PM_FULL)
        return j->coef[ci] + ((size_t)by * j->cbw[ci] + bx) * 64u;
    uint32_t v = (uint32_t)j->comp[ci].v, r0 = j->lo * v;
    if (by < r0 || by >= j->hi * v)
        return NULL;
    return j->coef[ci] + ((size_t)(by - r0) * j->cbw[ci] + bx) * 64u;
}

/* AC, первый проход (decode_mcu_AC_first) */
static void prog_ac_first(jpeg_t *j, const jcomp_t *c, int16_t *blk)
{
    if (j->eobrun) {
        j->eobrun--;
        return;
    }
    const huff_t *t = &j->hac[c->ta];
    for (int k = j->ss; k <= j->se; k++) {
        int rs = decode(j, t), r = rs >> 4, s = rs & 15;
        if (s) {
            k += r;
            blk[zz[k]] = (int16_t)LSH(extend(getb(j, s), s), j->al);
        } else if (r == 15) {
            k += 15;                                 /* ZRL: 15 нулей полосы */
        } else {
            j->eobrun = 1u << r;                     /* EOBr: серия 2^r + разряды */
            if (r)
                j->eobrun += (uint32_t)getb(j, r);
            j->eobrun--;
            break;
        }
    }
}

/* AC, уточняющий проход (decode_mcu_AC_refine): у ненулевых — разряд поправки, новые — ±p1 */
static void prog_ac_refine(jpeg_t *j, const jcomp_t *c, int16_t *blk)
{
    const int p1 = 1 << j->al, m1 = -p1;
    const huff_t *t = &j->hac[c->ta];
    int k = j->ss;
    if (!j->eobrun) {
        for (; k <= j->se; k++) {
            int rs = decode(j, t), r = rs >> 4, s = rs & 15;
            if (s) {
                s = getb(j, 1) ? p1 : m1;
            } else if (r != 15) {
                j->eobrun = 1u << r;
                if (r)
                    j->eobrun += (uint32_t)getb(j, r);
                break;
            }
            do {
                int16_t *th = blk + zz[k];
                if (*th) {
                    if (getb(j, 1) && (*th & p1) == 0)
                        *th = (int16_t)(*th + (*th >= 0 ? p1 : m1));
                } else if (--r < 0) {
                    break;                           /* нужный нулевой коэффициент */
                }
                k++;
            } while (k <= j->se);
            if (s)
                blk[zz[k]] = (int16_t)s;
        }
    }
    if (j->eobrun) {
        for (; k <= j->se; k++) {
            int16_t *th = blk + zz[k];
            if (*th && getb(j, 1) && (*th & p1) == 0)
                *th = (int16_t)(*th + (*th >= 0 ? p1 : m1));
        }
        j->eobrun--;
    }
}

/* Блок развёртки */
static void prog_block(jpeg_t *j, int ci, uint32_t bx, uint32_t by)
{
    jcomp_t *c = &j->comp[ci];
    if (j->ss == 0) {                                /* DC */
        int16_t *d = j->pmode == PM_DC ? j->coef[ci] + (size_t)by * j->cbw[ci] + bx : prog_coef(j, ci, bx, by);
        if (!j->ah) {
            int s = decode(j, &j->hdc[c->td]);
            if (s)
                s = extend(getb(j, s), s);
            c->pred += s;
            if (d)
                d[0] = (int16_t)LSH(c->pred, j->al);
        } else if (getb(j, 1) && d) {
            d[0] = (int16_t)(d[0] | (1 << j->al));
        }
        return;
    }
    int16_t *blk = prog_coef(j, ci, bx, by), tmp[64];
    if (!blk) {                                      /* вне полосы (не бывает: данные — с закладки) */
        memset(tmp, 0, sizeof tmp);
        blk = tmp;
    }
    if (!j->ah)
        prog_ac_first(j, c, blk);
    else
        prog_ac_refine(j, c, blk);
}

/* Блоки развёртки кончились (не полоса) */
static int prog_scan_all(const jpeg_t *j)
{
    return j->ns == 1 ? j->sby >= j->nbh[j->scomp[0]] : j->mcu_y >= j->mcuy;
}

static void prog_ckpt_save(jpeg_t *j)
{
    jckpt_t *c = &j->ckpt[j->cur];
    c->pos = j->jb_base + j->jb_r;
    c->bits = j->bits;
    c->nbits = j->nbits;
    c->hit_marker = j->hit_marker;
    c->rst_left = j->rst_left;
    c->eobrun = j->eobrun;
    for (int i = 0; i < j->ncomp; i++)
        c->pred[i] = j->comp[i].pred;
}

/* Следующая MCU развёртки (блок одной компоненты или MCU всех); 1 — нужные блоки кончились */
static int prog_mcu(jpeg_t *j)
{
    if (j->ns == 1) {
        int ci = j->scomp[0];
        prog_block(j, ci, j->sbx, j->sby);
        if (++j->sbx >= j->nbw[ci]) {
            j->sbx = 0;
            j->sby++;
        }
        return j->sby >= j->nbh[ci] ||
               (j->pmode == PM_STRIPE && j->sby >= j->hi * (uint32_t)j->comp[ci].v);
    }
    for (int i = 0; i < j->ns; i++) {
        int ci = j->scomp[i];
        const jcomp_t *c = &j->comp[ci];
        for (int by = 0; by < c->v; by++)
            for (int bx = 0; bx < c->h; bx++)
                prog_block(j, ci, j->mcu_x * (uint32_t)c->h + (uint32_t)bx, j->mcu_y * (uint32_t)c->v + (uint32_t)by);
    }
    if (++j->mcu_x == j->mcux) {
        j->mcu_x = 0;
        j->mcu_y++;
    }
    return j->mcu_y >= j->mcuy || (j->pmode == PM_STRIPE && j->mcu_y >= j->hi);
}

static int seg_sos_prog(jpeg_t *j, const uint8_t *s, int n)
{
    int ns = n >= 1 ? s[0] : 0;
    if (ns < 1 || ns > j->ncomp || n < 4 + 2 * ns)
        return 0;
    int last = -1;
    for (int i = 0; i < ns; i++) {
        int ci = 0;
        while (ci < j->ncomp && j->comp[ci].id != s[1 + 2 * i])
            ci++;
        if (ci == j->ncomp || ci <= last)
            return 0;                                /* компонента — из SOF, по порядку */
        last = ci;
        int td = s[2 + 2 * i] >> 4, ta = s[2 + 2 * i] & 15;
        if (td > 3 || ta > 3)
            return 0;
        j->comp[ci].td = td;
        j->comp[ci].ta = ta;
        j->scomp[i] = ci;
    }
    int p = 1 + 2 * ns;
    j->ns = ns;
    j->ss = s[p];
    j->se = s[p + 1];
    j->ah = s[p + 2] >> 4;
    j->al = s[p + 2] & 15;
    if (j->ss == 0 ? j->se != 0 : (ns != 1 || j->se < j->ss || j->se > 63))
        return 0;
    if (j->al > 13)
        return 0;
    for (int i = 0; i < ns; i++) {
        jcomp_t *c = &j->comp[j->scomp[i]];
        if (j->ss == 0 && !j->ah && !j->hdc[c->td].ok)
            return 0;
        if (j->ss && !j->hac[c->ta].ok)
            return 0;
        if (j->ss == 0 && !j->ah)
            c->pred = 0;
    }
    j->eobrun = 0;
    j->rst_left = j->restart;
    j->mcu_x = j->mcu_y = 0;
    j->sbx = j->sby = 0;
    j->sdone = 0;
    j->scan_end = 0;
    j->bits = 0;
    j->nbits = 0;
    j->hit_marker = 0;
    j->skip = j->pmode == PM_DC && j->ss > 0;        /* только DC: развёртки AC — прочь */
    if (j->nscan >= PROG_SCANS_MAX)
        return 0;
    j->cur = j->nscan++;
    if (j->ckpt && !j->replay)
        memset(&j->ckpt[j->cur], 0, sizeof(jckpt_t));
    return 1;
}

/* Сжатые данные развёртки: до её маркера конца (не RST). 1 — развёртка разобрана и маркер взят
   (j->end_marker); 0 — нужны данные. */
static int prog_scan(jpeg_t *j, const uint8_t *in, size_t n, size_t *pi)
{
    size_t i = *pi;
    for (;;) {
        if (j->skip) {                               /* данные развёртки не нужны */
            while (i < n && !j->scan_end) {
                uint8_t b = in[i++];
                if (j->prev_ff && b && b != 0xFF && !(b >= 0xD0 && b <= 0xD7)) {
                    j->scan_end = 1;
                    j->end_marker = b;
                    if (j->ckpt)                     /* место FF маркера конца — в закладку */
                        j->ckpt[j->cur].end = (uint32_t)(j->in_base + (int64_t)i) - 2u;
                }
                j->prev_ff = b == 0xFF;
            }
            *pi = i;
            return j->scan_end;
        }
        if (j->jb_r > JB_BYTES / 2) {
            memmove(j->jb, j->jb + j->jb_r, j->jb_n - j->jb_r);
            j->jb_n -= j->jb_r;
            j->jb_base += j->jb_r;
            j->jb_r = 0;
        }
        while (i < n && !j->scan_end && j->jb_n < JB_BYTES) {
            uint8_t b = in[i++];
            if (j->prev_ff && b && b != 0xFF && !(b >= 0xD0 && b <= 0xD7)) {
                j->scan_end = 1;                     /* маркер конца: FF перед ним — прочь */
                j->end_marker = b;
                j->jb_n--;
                if (j->ckpt)
                    j->ckpt[j->cur].end = (uint32_t)(j->in_base + (int64_t)i) - 2u;
                break;
            }
            if (!j->jb_n) {
                j->jb_r = 0;
                j->jb_base = (uint32_t)(j->in_base + (int64_t)i) - 1u;
            }
            j->jb[j->jb_n++] = b;
            j->prev_ff = b == 0xFF;
        }
        while (!j->sdone && (j->scan_end || j->jb_n - j->jb_r >= JB_NEED)) {
            if (j->restart && j->rst_left == 0) {
                if (!restart(j)) {
                    if (j->jb_n >= JB_BYTES)
                        j->sdone = 1;                /* испорчено: RST нет и в полном буфере */
                    break;                           /* маркер RST ещё не пришёл */
                }
            }
            j->sdone = prog_mcu(j);
            if (j->restart)
                j->rst_left--;
        }
        if (j->sdone && j->ckpt && !prog_scan_all(j)) {
            /* полоса кончилась внутри развёртки: закладка — следующей полосе; в повторе — сразу
               к маркеру конца (его место знает закладка прошлой полосы), в приёме — до маркера */
            j->ckpt[j->cur].done = 0;
            prog_ckpt_save(j);
            if (j->replay) {
                *pi = i;
                return 2;
            }
        } else if (j->sdone && j->ckpt) {
            j->ckpt[j->cur].done = 1;                /* блоки развёртки кончились раньше полосы */
        }
        if (j->sdone && !j->scan_end) {              /* нужные блоки разобраны: остаток — прочь */
            j->skip = 1;
            j->jb_r = j->jb_n = 0;
            continue;
        }
        if (j->scan_end && !j->sdone && j->restart && j->rst_left == 0) {
            j->sdone = 1;                            /* испорчено: RST не пришёл до маркера */
        }
        *pi = i;
        return j->scan_end && j->sdone;
    }
}

/* Строка MCU mr из коэффициентов: обратное ДКП в плоскости и вывод (как у baseline) */
static void prog_emit_row(jpeg_t *j, uint32_t mr, void (*emit)(void *, const uint8_t *, size_t), void *u)
{
    for (uint32_t mx = 0; mx < j->mcux; mx++)
        for (int ci = 0; ci < j->ncomp; ci++) {
            jcomp_t *c = &j->comp[ci];
            const uint16_t *q = j->qt[c->tq];
            for (int by = 0; by < c->v; by++)
                for (int bx = 0; bx < c->h; bx++) {
                    uint32_t gx = mx * (uint32_t)c->h + (uint32_t)bx, gy = mr * (uint32_t)c->v + (uint32_t)by;
                    uint8_t *o = plane(j, ci, (uint32_t)(by * c->ss)) + gx * (uint32_t)c->ss;
                    if (j->pmode == PM_DC) {         /* только DC: блок одного тона */
                        int32_t dc = j->coef[ci][(size_t)gy * j->cbw[ci] + gx] * q[0];
                        uint8_t v = idct_lim[DESCALE(dc, 3) & 1023];
                        for (int r = 0; r < c->ss; r++, o += c->bw)
                            memset(o, v, (size_t)c->ss);
                        continue;
                    }
                    const int16_t *blk = prog_coef(j, ci, gx, gy);
                    int any = 0;
                    for (int k = 1; k < 64 && !any; k++)
                        any = blk[k] != 0;
                    if (!any && c->ss == 8) {        /* только DC — заливка, как decode_mcu */
                        int32_t dc = ((int32_t)blk[0] * q[0]) << PASS1_BITS;
                        uint32_t v = idct_lim[DESCALE(dc, PASS1_BITS + 3) & 1023] * 0x01010101u;
                        for (int r = 0; r < 8; r++, o += c->bw)
                            ((uint32_t *)o)[0] = ((uint32_t *)o)[1] = v;
                        continue;
                    }
                    switch (c->ss) {
                    case 8:  idct(blk, q, o, c->bw); break;
                    case 4:  idct4(blk, q, o, c->bw); break;
                    case 2:  idct2(blk, q, o, c->bw); break;
                    default: idct1(blk, q, o); break;
                    }
                }
        }
    emit_mcu_row(j, mr, emit, u);
}

/* Копия файла (PM_STRIPE): байты приёма с начала развёрток */
static int prog_copy(img_t *im, jpeg_t *j, const uint8_t *p, size_t n)
{
    if (j->fsize + n > j->fcap) {
        img_err(im, FTC_IMG_E_TOO_BIG, j->fsize + (uint32_t)n + (j->fcopy ? pool_free(im) : 0));
        return 0;
    }
    if (j->fcopy) {
        memcpy(j->fcopy + j->fsize, p, n);
        j->fsize += (uint32_t)n;
        return 1;
    }
    while (n) {                                      /* RAM_G — кусками по 256 байт */
        size_t k = sizeof j->fstage - j->fstage_n;
        if (k > n)
            k = n;
        memcpy(j->fstage + j->fstage_n, p, k);
        j->fstage_n += (uint32_t)k;
        j->fsize += (uint32_t)k;
        p += k;
        n -= k;
        if (j->fstage_n == sizeof j->fstage) {
            im->draft_wr(im->eu, j->fbase + j->fsize - (uint32_t)sizeof j->fstage, j->fstage, sizeof j->fstage);
            j->fstage_n = 0;
        }
    }
    return 1;
}

static void prog_copy_flush(img_t *im, jpeg_t *j)
{
    if (!j->fcopy && j->fstage_n) {
        uint32_t k = (j->fstage_n + 3u) & ~3u;
        memset(j->fstage + j->fstage_n, 0, k - j->fstage_n);
        im->draft_wr(im->eu, j->fbase + j->fsize - j->fstage_n, j->fstage, k);
        j->fstage_n = 0;
    }
}

/* Разбор с начала копии: таблицы — как на конец SOF, разряды и предсказания — с нуля */
static void prog_rewind(jpeg_t *j)
{
    const jtabs_t *t = j->tabs;
    memcpy(j->qt, t->qt, sizeof j->qt);
    for (int k = 0; k < 4; k++) {
        tabs_load(&j->hdc[k], &t->hdc[k]);
        tabs_load(&j->hac[k], &t->hac[k]);
    }
    j->restart = t->restart;
    j->state = J_FF;
    j->prev_ff = 0;
    j->jb_r = j->jb_n = 0;
    j->bits = 0;
    j->nbits = 0;
    j->hit_marker = 0;
    j->scan_end = j->sdone = j->skip = 0;
    j->eobrun = 0;
    for (int i = 0; i < j->ncomp; i++)
        j->comp[i].pred = 0;
    j->rp = 0;
    j->nscan = 0;
    j->seek = 0;
}

static int jpeg_open_out(img_t *im, jpeg_t *j, void (*emit)(void *, const uint8_t *, size_t), void *u);
static int layout(jpeg_t *j);
static void jpeg_scale(jpeg_t *j);

/* Память под выход (оценка сверху): вписывание или по площади */
static uint32_t prog_out_need(const jpeg_t *j)
{
    uint32_t nch = (j->ncomp == 1 || (j->opts & OPT_MONO)) ? 1u : 3u;
    uint32_t area = 2u * j->tw * nch * 4u + j->ow * nch + 2u * FIT_W + 64u;
    if (j->omode == OUT_AREA)
        return area;
    if (j->omode == OUT_FIT && fit_on(j->opts)) {
        uint32_t ow, oh;
        fit_size(j->w, j->h, &ow, &oh);
        int hk = j->ow != ow ? fit_ksize(j->ow, ow) : 1, vk = j->oh != oh ? fit_ksize(j->oh, oh) : 1;
        if (!hk || !vk)
            return area;
        uint32_t iw = j->ow, wide = iw > ow ? iw : ow;
        return ow * (uint32_t)hk * 4u + 3u * ow + (uint32_t)vk * wide * 3u + iw * 3u + wide * 3u + 2u * FIT_W + 64u;
    }
    return 0;
}

/* Сколько байтов пула займут плоскости (как layout) */
static uint32_t prog_layout_need(const jpeg_t *j)
{
    uint32_t a = 0;
    for (int ci = 0; ci < j->ncomp; ci++)
        a = ((a + 3u) & ~3u) + j->comp[ci].bw * j->comp[ci].rows;
    for (int ci = 1; ci < j->ncomp; ci++)
        a += j->comp[ci].bw;
    a += 2u * (j->mcux * (uint32_t)j->m * (uint32_t)j->hmax + 16u);
    if (j->vfancy)
        a += j->ow;
    a = ((a + 3u) & ~3u) + 2u * j->ow + 8u;
    return a + 64u;
}

/* Прогрессивный: где держать коэффициенты; плоскости, выход, память коэффициентов. 0 — ошибка. */
static int prog_plan(img_t *im, jpeg_t *j, void (*emit)(void *, const uint8_t *, size_t), void *u)
{
    uint32_t blocks = 0, rowbytes = 0;
    for (int ci = 0; ci < j->ncomp; ci++) {
        jcomp_t *c = &j->comp[ci];
        j->cbw[ci] = j->mcux * (uint32_t)c->h;
        j->cbh[ci] = j->mcuy * (uint32_t)c->v;
        uint32_t cw = (j->w * (uint32_t)c->h + (uint32_t)j->hmax - 1) / (uint32_t)j->hmax;
        uint32_t ch = (j->h * (uint32_t)c->v + (uint32_t)j->vmax - 1) / (uint32_t)j->vmax;
        j->nbw[ci] = (cw + 7) / 8;
        j->nbh[ci] = (ch + 7) / 8;
        blocks += j->cbw[ci] * j->cbh[ci];
        rowbytes += j->cbw[ci] * (uint32_t)c->v * 128u;
    }
    const uint32_t full = blocks * 128u, nzb = (uint32_t)(PROG_SCANS_MAX * sizeof(jckpt_t)) + 8u;
    const uint32_t tabs = (uint32_t)sizeof(jtabs_t) + 8u;
    uint32_t need = prog_layout_need(j) + prog_out_need(j) + 256u;
    uint32_t avail = pool_free(im) > need ? pool_free(im) - need : 0;
    uint32_t srows = 0, fcap = 0, ram = 0;
    if (full <= avail) {
        j->pmode = PM_FULL;
    } else {
        /* полосы: «уже не ноль» всех блоков, полоса строк MCU, копия файла (RAM_G у рамки или пул).
           Рамка: копии — место в RAM_G по оценке PROG_COPY_EST байтов на блок; выход как есть его
           не оставляет — выход меньше рамки (точный), а не «только DC». */
        const int gray = j->ncomp == 1 || (j->opts & OPT_MONO);
        uint32_t est = blocks * PROG_COPY_EST + 4096u;
        if (im->boxed && im->draft_rd && im->room > est + 1024u && nzb + tabs + PROG_REPLAY_BYTES + rowbytes < avail) {
            uint32_t ow = j->omode == OUT_AREA ? j->tw : j->ow, oh = j->omode == OUT_AREA ? j->th : j->oh;
            uint32_t bpp = j->omode == OUT_AREA ? (gray ? 1u : 2u) : ((j->opts & OPT_MONO) ? 1u : 2u);
            uint32_t maxout = im->room - est;
            if ((uint64_t)ow * oh * bpp > maxout) {
                while ((uint64_t)ow * oh * bpp > maxout && (ow > 1 || oh > 1)) {
                    ow = ow * 15u / 16u ? ow * 15u / 16u : 1u;
                    oh = oh * 15u / 16u ? oh * 15u / 16u : 1u;
                }
                if (j->omode == OUT_RAW)
                    bpp = gray ? 1u : 2u;
                j->omode = OUT_AREA;                 /* меньше рамки — по площади */
                j->tw = ow;
                j->th = oh;
                uint32_t sc = j->w / ow < j->h / oh ? j->w / ow : j->h / oh;
                j->m = sc >= 8 ? 1 : sc >= 4 ? 2 : sc >= 2 ? 4 : 8;
                jpeg_scale(j);
                need = prog_layout_need(j) + prog_out_need(j) + 256u;
                avail = pool_free(im) > need ? pool_free(im) - need : 0;
            }
            uint32_t base = ((uint32_t)((uint64_t)ow * oh * bpp) + 3u) & ~3u;
            ram = 1;
            fcap = (im->room - base) & ~3u;
            fcap = fcap > 8u ? fcap - 8u : 0;
            if (avail > nzb + tabs + PROG_REPLAY_BYTES + 16u)
                srows = (avail - nzb - tabs - PROG_REPLAY_BYTES - 16u) / rowbytes;
        } else if (avail > nzb + tabs + rowbytes) {
            /* копия в пуле: полосе — столько строк, чтобы полос было не больше PROG_PASSES_MAX */
            uint32_t smin = (j->mcuy + PROG_PASSES_MAX - 1) / PROG_PASSES_MAX;
            srows = (avail - nzb - tabs) / 3u / rowbytes;
            if (srows < smin)
                srows = smin;
            if ((uint64_t)srows * rowbytes + nzb + tabs < avail)
                fcap = avail - nzb - tabs - srows * rowbytes;
            /* рамка и вписывание: копия по оценке не влезет — лучше «только DC», чем ошибка
               посреди файла (у CMD_LOADIMAGE — ошибка «велика») */
            if (j->omode != OUT_RAW && fcap < blocks * PROG_COPY_POOL)
                fcap = 0;
        }
        if (srows > j->mcuy)
            srows = j->mcuy;
        uint32_t passes = srows ? (j->mcuy + srows - 1) / srows : 0;
        if (srows && fcap >= 4096u && (j->omode == OUT_RAW || passes <= PROG_PASSES_MAX)) {
            j->pmode = PM_STRIPE;
            j->srows = srows;
        } else if (j->omode != OUT_RAW) {
            /* только DC: ДКП 1/8 (выход не больше картинки 1/8) */
            j->pmode = PM_DC;
            j->m = 1;
            jpeg_scale(j);
            if (j->omode == OUT_AREA && (j->tw > j->ow || j->th > j->oh)) {
                j->tw = j->ow;
                j->th = j->oh;
            }
            need = prog_layout_need(j) + prog_out_need(j) + 256u;
            avail = pool_free(im) > need ? pool_free(im) - need : 0;
            if (blocks * 2u > avail) {
                img_err(im, FTC_IMG_E_TOO_BIG, blocks * 2u + need);
                return 0;
            }
        } else {
            img_err(im, FTC_IMG_E_TOO_BIG, full + need);
            return 0;
        }
    }
    if (!layout(j))
        return 0;
    j->started = 1;
    if (!jpeg_open_out(im, j, emit, u))
        return 0;
    for (int ci = 0; ci < j->ncomp; ci++) {
        size_t n = j->pmode == PM_FULL ? (size_t)j->cbw[ci] * j->cbh[ci] * 64u
                 : j->pmode == PM_DC ? (size_t)j->cbw[ci] * j->cbh[ci]
                 : (size_t)j->cbw[ci] * (size_t)j->comp[ci].v * j->srows * 64u;
        j->coef[ci] = pool_take(im, n * 2u);
        if (!j->coef[ci]) {
            img_err(im, FTC_IMG_E_TOO_BIG, j->pmode == PM_FULL ? full : nzb + rowbytes);
            return 0;
        }
        memset(j->coef[ci], 0, n * 2u);
    }
    if (j->pmode == PM_STRIPE) {
        j->ckpt = pool_take(im, PROG_SCANS_MAX * sizeof(jckpt_t));
        if (!j->ckpt)
            return 0;
    }
    j->lo = 0;
    j->hi = j->pmode == PM_STRIPE ? j->srows : j->mcuy;
    if (j->pmode == PM_STRIPE) {
        j->tabs = pool_take(im, sizeof(jtabs_t));
        if (!j->tabs)
            return 0;
        memcpy(j->tabs->qt, j->qt, sizeof j->qt);
        for (int k = 0; k < 4; k++) {
            tabs_save(&j->tabs->hdc[k], &j->hdc[k]);
            tabs_save(&j->tabs->hac[k], &j->hac[k]);
        }
        j->tabs->restart = j->restart;
        if (ram) {
            j->fbase = (im->out_bytes + 3u) & ~3u;
            fcap = im->room > j->fbase + 8u ? (im->room - j->fbase - 8u) & ~3u : 0;
            j->rbuf = pool_take(im, PROG_REPLAY_BYTES + 8u);
            if (!j->rbuf)
                return 0;
        } else {
            fcap = pool_free(im) > 64u ? pool_free(im) - 64u : 0;
            j->fcopy = pool_take(im, fcap);
        }
        j->fcap = fcap;
        j->copying = 1;
    }
    return 1;
}

/* Конец разбора полосы (EOI): вывод её строк */
static void prog_pass_end(img_t *im, jpeg_t *j)
{
    if (j->pmode == PM_STRIPE && !j->replay)
        prog_copy_flush(im, j);
    j->copying = 0;
    j->pphase = PP_OUT;
    j->orow = j->lo;
}

/* Работа без новых данных: вывод полосы, повтор разбора из копии */
static int prog_work(img_t *im, jpeg_t *j, void (*emit)(void *, const uint8_t *, size_t), void *u);

/* Выход картинки (первый SOS): как есть, вписывание (Pillow) или по площади. Вписывание, у
   которого ядро фильтра не помещается (уменьшение больше 4 раз после ДКП), — по площади на холст. */
static int jpeg_open_out(img_t *im, jpeg_t *j, void (*emit)(void *, const uint8_t *, size_t), void *u)
{
    int gray = j->ncomp == 1 || (j->opts & OPT_MONO);
    if (j->omode == OUT_FIT && fit_on(j->opts)) {
        if (fit_init_pool(im, &im->fit, j->w, j->h, j->ow, j->oh)) {
            j->fit = &im->fit;
            fit_start(j->fit, emit, u);
        } else {
            if (!area_init(im, &im->area, j->ow, j->oh, j->tw, j->th, gray ? 1 : 3, FMT_RGB565, 1))
                return 0;
            j->area = &im->area;
            area_start(j->area, emit, u);
        }
        im->info.fmt = FMT_RGB565;
        im->info.w = FIT_W;
        im->info.h = FIT_H;
        im->info.stride = 2 * FIT_W;
        return 1;
    }
    if (j->omode == OUT_AREA) {
        uint32_t fmt = gray ? FMT_L8 : FMT_RGB565;
        if (!area_init(im, &im->area, j->ow, j->oh, j->tw, j->th, gray ? 1 : 3, fmt, 0))
            return 0;
        j->area = &im->area;
        im->info.fmt = fmt;
        im->info.w = j->tw;
        im->info.h = j->th;
        im->info.stride = (gray ? 1u : 2u) * j->tw;
        im->out_bytes = im->info.stride * j->th;
        return 1;
    }
    im->info.fmt = (j->opts & OPT_MONO) ? FMT_L8 : FMT_RGB565;
    im->info.w = j->ow;
    im->info.h = j->oh;
    im->info.stride = (j->opts & OPT_MONO) ? j->ow : 2 * j->ow;
    im->out_bytes = im->info.stride * j->oh;
    return 1;
}

/* Копия файла (PM_STRIPE): байты нынешнего вызова до i */
static int prog_copy_upto(img_t *im, jpeg_t *j, const uint8_t *in, size_t i)
{
    if (!j->copying || j->replay || i <= j->copy_from)
        return 1;
    int ok = prog_copy(im, j, in + j->copy_from, i - j->copy_from);
    j->copy_from = i;
    return ok;
}

static int jpeg_parse(img_t *im, const uint8_t *in, size_t n, size_t *used,
                      void (*emit)(void *, const uint8_t *, size_t), void *u)
{
    jpeg_t *j = &im->u.j;
    size_t i = 0;
    while (j->state != J_DONE && j->state != J_ERR && j->state != J_PASS && !j->seek) {
        if (j->state == J_SCAN && j->prog) {
            int ps = prog_scan(j, in, n, &i);
            if (ps == 2) {                           /* повтор: полоса развёртки — к её маркеру */
                j->seek_to = j->ckpt[j->cur].end;
                j->seek = 1;
                j->state = J_FF;
                break;
            }
            if (!ps) {
                if (i >= n)
                    break;
                continue;
            }
            uint8_t mk = j->end_marker;              /* развёртка кончилась маркером mk */
            j->scan_end = j->sdone = j->skip = 0;
            j->prev_ff = 0;
            j->jb_r = j->jb_n = 0;
            if (mk == 0xD9) {                        /* EOI: полоса (или вся картинка) разобрана */
                if (!prog_copy_upto(im, j, in, i)) {
                    j->state = J_ERR;
                    break;
                }
                prog_pass_end(im, j);
                j->state = J_PASS;
                break;
            }
            j->marker = mk;
            j->state = (mk == 0xD8 || (mk >= 0xD0 && mk <= 0xD7) || mk == 0x01) ? J_FF : J_LEN0;
            continue;
        }
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
            if (b == 0xD9 && j->prog && j->started) {
                prog_pass_end(im, j);              /* повтор: EOI сразу за переходом к маркеру */
                j->state = J_PASS;
            } else if (b == 0xD9)
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
            int keep = m == 0xC0 || m == 0xC1 || m == 0xC2 || m == 0xC4 || m == 0xDB || m == 0xDA || m == 0xDD;
            if (m >= 0xC3 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                /* C3, C5…C7 — lossless и иерархический; C9…CB, CD…CF — арифметика */
                img_err(im, FTC_IMG_E_UNSUPPORTED,
                        (m == 0xC3 || (m >= 0xC5 && m <= 0xC7)) ? FTC_IMG_U_LOSSLESS : FTC_IMG_U_ARITH);
                j->state = J_ERR;
                break;
            }
            if (j->seg_len < 0 || (keep && j->seg_len > (int)sizeof j->seg)) {
                j->state = J_ERR;
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
            if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
                if (j->replay) {
                    ok = 1;                          /* повтор из копии: SOF уже разобран */
                } else {
                    j->prog = m == 0xC2;
                    ok = !j->w && seg_sof(j, j->seg, j->seg_len);
                    if (ok && j->prog) {             /* память коэффициентов, выход, копия с i */
                        ok = prog_plan(im, j, emit, u);
                        j->copy_from = i;
                        j->in_base = -(int64_t)i;
                    }
                }
            }
            else if (m == 0xC4)
                ok = seg_dht(j, j->seg, j->seg_len);
            else if (m == 0xDB)
                ok = seg_dqt(j, j->seg, j->seg_len);
            else if (m == 0xDD)
                j->restart = j->seg_len >= 2 ? (j->seg[0] << 8) | j->seg[1] : 0;
            else if (m == 0xDA && j->prog) {
                ok = j->w && seg_sos_prog(j, j->seg, j->seg_len);
                if (ok)
                    j->state = J_SCAN;
                if (ok && j->ckpt && j->replay) {
                    /* повтор полосы: данные развёртки — с закладки, или вся развёртка — мимо */
                    jckpt_t *c = &j->ckpt[j->cur];
                    if (c->done) {
                        j->seek_to = c->end;
                        j->state = J_FF;
                    } else {
                        j->seek_to = c->pos;
                        j->bits = c->bits;
                        j->nbits = c->nbits;
                        j->hit_marker = c->hit_marker;
                        j->rst_left = c->rst_left;
                        j->eobrun = c->eobrun;
                        for (int k = 0; k < j->ncomp; k++)
                            j->comp[k].pred = c->pred[k];
                        if (j->ns == 1)
                            j->sby = j->lo * (uint32_t)j->comp[j->scomp[0]].v;
                        else
                            j->mcu_y = j->lo;
                    }
                    j->seek = 1;
                }
                if (j->seek)
                    break;
            } else if (m == 0xDA) {
                ok = j->w && seg_sos(j, j->seg, j->seg_len);
                if (ok && !j->started) {
                    j->started = 1;
                    ok = jpeg_open_out(im, j, emit, u);
                }
                if (ok)
                    j->state = J_SCAN;
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
    if (j->state == J_PASS)
        return FTC_IMG_BUSY;
    return j->state == J_DONE ? FTC_IMG_DONE : FTC_IMG_NEED_INPUT;
}

static int prog_work(img_t *im, jpeg_t *j, void (*emit)(void *, const uint8_t *, size_t), void *u)
{
    if (j->pphase == PP_OUT) {
        for (uint32_t k = 0; k < PROG_OUT_ROWS && j->orow < j->hi; k++)
            prog_emit_row(j, j->orow++, emit, u);
        if (j->orow < j->hi)
            return FTC_IMG_BUSY;
        if (j->hi >= j->mcuy) {
            j->state = J_DONE;
            return FTC_IMG_DONE;
        }
        j->lo = j->hi;                               /* следующая полоса — заново из копии */
        j->hi = j->lo + j->srows > j->mcuy ? j->mcuy : j->lo + j->srows;
        for (int ci = 0; ci < j->ncomp; ci++)
            memset(j->coef[ci], 0, (size_t)j->cbw[ci] * (size_t)j->comp[ci].v * j->srows * 64u * 2u);
        prog_rewind(j);
        j->replay = 1;
        j->pphase = PP_REPLAY;
        return FTC_IMG_BUSY;
    }
    if (j->rp >= j->fsize) {                         /* копия кончилась раньше EOI */
        img_err(im, FTC_IMG_E_CORRUPT, 1);
        return FTC_IMG_ERROR;
    }
    uint32_t k = j->fsize - j->rp;
    const uint8_t *p;
    if (j->fcopy) {
        if (k > PROG_REPLAY_BYTES)
            k = PROG_REPLAY_BYTES;
        p = j->fcopy + j->rp;
    } else {
        uint32_t off = j->rp & ~3u, lead = j->rp - off;
        if (k > PROG_REPLAY_BYTES - lead)
            k = PROG_REPLAY_BYTES - lead;
        im->draft_rd(im->eu, j->fbase + off, j->rbuf, (lead + k + 3u) & ~3u);
        p = j->rbuf + lead;
    }
    size_t used = 0;
    j->in_base = j->rp;
    int r = jpeg_parse(im, p, k, &used, emit, u);
    j->rp += (uint32_t)used;
    if (r == FTC_IMG_ERROR)
        return r;
    if (j->seek) {                                   /* переход по копии: закладка или маркер */
        j->seek = 0;
        j->rp = j->seek_to;
        j->jb_r = j->jb_n = 0;
        j->jb_base = j->rp;
        j->prev_ff = 0;
        return FTC_IMG_BUSY;
    }
    if (j->pphase == PP_OUT || used)
        return FTC_IMG_BUSY;
    img_err(im, FTC_IMG_E_CORRUPT, 1);               /* разбор встал */
    return FTC_IMG_ERROR;
}

/* JPEG: приём (и у прогрессивного — работа без данных: вывод, повтор из копии) */
static int jpeg_run(img_t *im, const uint8_t *in, size_t n, size_t *used,
                    void (*emit)(void *, const uint8_t *, size_t), void *u)
{
    jpeg_t *j = &im->u.j;
    *used = 0;
    if (j->pphase != PP_LIVE)
        return prog_work(im, j, emit, u);
    j->copy_from = 0;
    j->in_base = j->fsize;
    int r = jpeg_parse(im, in, n, used, emit, u);
    if (r != FTC_IMG_ERROR && !prog_copy_upto(im, j, in, *used))
        return FTC_IMG_ERROR;
    return r;
}

/* --------------------------------------------------------------------------------------- */
/* PNG                                                                                     */
/* --------------------------------------------------------------------------------------- */
/*
 * Прошивка 39: все виды и глубины стандарта — серый 1/2/4/8/16, RGB 8/16, палитра 1/2/4/8, серый
 * с альфой 8/16, RGBA 8/16, tRNS у серого, RGB и палитры, «через строку» (Adam7). Строка после
 * фильтров переводится в нормированную — по 8 бит на канал: 16 бит — старший байт, серый 1/2/4
 * бита растягивается на 0…255 (·255, ·85, ·17), индекс палитры — как есть; tRNS у серого и RGB в
 * рамке и вписывании — ещё канал альфы (цвет совпал с tRNS во всю глубину — 0, иначе 255). Выход:
 * - как у FT812 (CMD_LOADIMAGE, рамка без уменьшения): серый — L8, RGB — RGB565, палитра —
 *   PALETTED565 (с tRNS — PALETTED4444, индексы по байту), серый с альфой и RGBA — ARGB4. tRNS у
 *   серого и RGB в CMD_LOADIMAGE не учитывается, как у FT812 (bt8xxemu 04.11.2026: те же байты,
 *   что без tRNS; 1 и 4 бита FT812 не разбирает — у VDAC4 это расширение); в рамке —
 *   учитывается: выход ARGB4;
 * - вписывание (Pillow) и по площади — RGB или RGBA из нормированной строки.
 * Adam7: строки проходов (нормированные) — в черновик: в пул, не влезает — в RAM_G за выходом в
 * пределах room (рамка). Когда приняты все проходы, строки картинки собираются по одной из
 * строк проходов (FTC_IMG_BUSY — кусками по PNG_ASM_ROWS, между ними — главный цикл BL616).
 */

enum { P_SIG, P_LEN, P_TYPE, P_DATA, P_CRC, P_DONE, P_ERR };
#define PNG_ASM_ROWS 64u

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

/* Adam7: начало и шаг проходов по x и y */
static const uint8_t a7_x0[7] = {0, 4, 0, 2, 0, 1, 0}, a7_y0[7] = {0, 0, 4, 0, 2, 0, 1};
static const uint8_t a7_dx[7] = {8, 8, 4, 4, 2, 2, 1}, a7_dy[7] = {8, 8, 8, 4, 4, 2, 2};

static void png_pass_dims(const png_t *p, int pass, uint32_t *pw, uint32_t *ph)
{
    if (!p->interlace) {
        *pw = p->w;
        *ph = p->h;
        return;
    }
    *pw = p->w > a7_x0[pass] ? (p->w - a7_x0[pass] + a7_dx[pass] - 1) / a7_dx[pass] : 0;
    *ph = p->h > a7_y0[pass] ? (p->h - a7_y0[pass] + a7_dy[pass] - 1) / a7_dy[pass] : 0;
}

static uint32_t png_rowbytes(const png_t *p, uint32_t w)
{
    return (uint32_t)(((uint64_t)w * p->bits + 7u) / 8u);
}

/* Первый непустой проход с номера pass; 7 — проходов больше нет */
static void png_pass_begin(png_t *p, int pass)
{
    int last = p->interlace ? 7 : 1;
    for (; pass < last; pass++) {
        png_pass_dims(p, pass, &p->pw, &p->ph);
        if (p->pw && p->ph)
            break;
    }
    p->pass = pass < last ? pass : 7;
    p->prow = 0;
    p->lgot = 0;
    p->prowb = p->pass < 7 ? png_rowbytes(p, p->pw) : 0;
}

/* Строка после фильтров (n точек) → нормированная: по 8 бит на канал, у keyed — и альфа */
static void png_unpack(const png_t *p, const uint8_t *s, uint32_t n, uint8_t *o)
{
    const int d = p->depth, ch = p->ch;
    if (d < 8) {                                 /* серый или индекс: по d бит, старшие — первые */
        const uint32_t mask = (1u << d) - 1u, mul = p->ctype == 0 ? 255u / mask : 1u;
        int sh = 8 - d;
        for (uint32_t x = 0; x < n; x++) {
            uint32_t v = ((uint32_t)*s >> sh) & mask;
            sh -= d;
            if (sh < 0) {
                sh = 8 - d;
                s++;
            }
            *o++ = (uint8_t)(v * mul);
            if (p->keyed)
                *o++ = v == p->key[0] ? 0 : 255;
        }
        return;
    }
    if (d == 8) {
        if (!p->keyed) {
            memcpy(o, s, (size_t)n * (size_t)ch);
            return;
        }
        for (uint32_t x = 0; x < n; x++, s += ch) {
            int hit = s[0] == p->key[0];
            o[0] = s[0];
            if (ch == 3) {
                hit = hit && s[1] == p->key[1] && s[2] == p->key[2];
                o[1] = s[1];
                o[2] = s[2];
            }
            o[ch] = hit ? 0 : 255;
            o += ch + 1;
        }
        return;
    }
    for (uint32_t x = 0; x < n; x++, s += 2 * ch) {   /* 16 бит: старший байт; tRNS — во всю глубину */
        int hit = 1;
        for (int c = 0; c < ch; c++) {
            o[c] = s[2 * c];
            hit = hit && (uint32_t)(s[2 * c] << 8 | s[2 * c + 1]) == p->key[c];
        }
        o += ch;
        if (p->keyed)
            *o++ = hit ? 0 : 255;
    }
}

/* Вписывание (Pillow): нормированная строка → RGB; прозрачность — наложение на чёрный (c·a/255) */
static void png_fit_row(img_t *im, png_t *p, const uint8_t *s)
{
    uint8_t *o = p->fit->src;
    for (uint32_t x = 0; x < p->w; x++, o += 3) {
        if (p->ctype == 3) {                     /* палитра; tRNS — альфа, нет — 255 */
            uint32_t i = s[x];
            const uint8_t *e = p->plte + 3 * i;
            uint32_t a = i < p->ntrns ? p->trns[i] : 255;
            o[0] = div255(e[0] * a);
            o[1] = div255(e[1] * a);
            o[2] = div255(e[2] * a);
        } else if (p->nch == 1) {
            o[0] = o[1] = o[2] = s[x];
        } else if (p->nch == 2) {
            uint32_t a = s[2 * x + 1];
            o[0] = o[1] = o[2] = div255(s[2 * x] * a);
        } else if (p->nch == 3) {
            o[0] = s[3 * x];
            o[1] = s[3 * x + 1];
            o[2] = s[3 * x + 2];
        } else {
            uint32_t a = s[4 * x + 3];
            o[0] = div255(s[4 * x] * a);
            o[1] = div255(s[4 * x + 1] * a);
            o[2] = div255(s[4 * x + 2] * a);
        }
    }
    fit_row(p->fit, im->emit, im->eu);
}

/* Нормированная строка картинки → выход */
static void png_out_row(img_t *im, png_t *p, const uint8_t *s)
{
    if (p->out == OUT_AREA) {                    /* по площади — строка как есть (режим — в png_plan) */
        area_row(p->area, s, im->emit, im->eu);
        return;
    }
    if (p->out == OUT_FIT) {
        png_fit_row(im, p, s);
        return;
    }
    uint8_t *o = p->out_row;
    uint32_t n = 0;
    switch (p->fmt) {
    case FMT_L8:                                 /* серый */
    case FMT_PAL565:                             /* индексы */
    case FMT_PAL4444:
        im->emit(im->eu, s, p->w);
        return;
    case FMT_RGB565:
        for (uint32_t x = 0; x < p->w; x++, s += 3) {
            uint32_t v = ((uint32_t)(s[0] >> 3) << 11) | ((uint32_t)(s[1] >> 2) << 5) | (s[2] >> 3);
            o[n++] = (uint8_t)v;
            o[n++] = (uint8_t)(v >> 8);
        }
        break;
    default:                                     /* ARGB4: серый с альфой или RGBA */
        for (uint32_t x = 0; x < p->w; x++) {
            uint32_t r, g, b, a;
            if (p->nch == 2) {
                r = g = b = s[0];
                a = s[1];
                s += 2;
            } else {
                r = s[0];
                g = s[1];
                b = s[2];
                a = s[3];
                s += 4;
            }
            uint32_t v = ((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
            o[n++] = (uint8_t)v;
            o[n++] = (uint8_t)(v >> 8);
        }
        break;
    }
    im->emit(im->eu, o, n);
}

/* По площади: режим входа по виду строки; у палитры — таблица (с прозрачностью — предумноженная) */
static int png_area_mode(img_t *im, png_t *p, area_t *a)
{
    if (p->ctype == 3) {
        uint32_t *t = pool_take(im, 256u * 4u * 4u);
        if (!t) {
            img_err(im, FTC_IMG_E_TOO_BIG, 4096u);
            return 0;
        }
        for (uint32_t i = 0; i < 256; i++) {
            const uint8_t *e = p->plte + 3 * (i < p->npal ? i : 0);
            uint32_t al = i < p->ntrns ? p->trns[i] : 255;
            if (a->nch == 4) {
                t[4 * i] = e[0] * al;
                t[4 * i + 1] = e[1] * al;
                t[4 * i + 2] = e[2] * al;
                t[4 * i + 3] = al;
            } else {
                t[4 * i] = e[0];
                t[4 * i + 1] = e[1];
                t[4 * i + 2] = e[2];
                t[4 * i + 3] = 0;
            }
        }
        a->pal = t;
        a->mode = AM_PAL;
    } else if (p->nch == 2) {
        a->mode = AM_GA;                         /* серый с альфой и серый с tRNS */
    }
    return 1;
}

/* Решение о выходе (первый IDAT: палитра и tRNS уже известны) и буферы строк. 0 — ошибка. */
static int png_plan(img_t *im, png_t *p)
{
    const uint32_t w = p->w, h = p->h;
    p->keyed = p->has_key && (im->boxed || fit_on(p->opts));
    p->nch = (p->ctype == 2 ? 3 : p->ctype == 4 ? 2 : p->ctype == 6 ? 4 : 1) + (p->keyed ? 1 : 0);
    const int gray = p->ctype == 0 || p->ctype == 4;
    const int alpha = p->ctype == 4 || p->ctype == 6 || (p->ctype == 3 && p->ntrns) || p->keyed;
    /* выход как у FT812 */
    uint32_t pal = 0;
    p->fmt = p->ctype == 3 ? (p->ntrns ? FMT_PAL4444 : FMT_PAL565)
           : (p->ctype == 4 || p->ctype == 6 || p->keyed) ? FMT_ARGB4 : p->ctype == 0 ? FMT_L8 : FMT_RGB565;
    if (p->ctype == 3)
        pal = 2 * p->npal;
    uint32_t stride = (p->fmt == FMT_L8 || p->ctype == 3) ? w : 2 * w;
    uint32_t tw = 0, th = 0;
    p->out = OUT_RAW;
    if (fit_on(p->opts)) {
        p->out = OUT_FIT;
    } else if (im->boxed) {
        int r = box_target(im, w, h, pal + stride * h, alpha ? 2u : gray ? 1u : 2u, &tw, &th);
        if (r < 0)
            return 0;
        if (r == 0)
            p->out = OUT_AREA;
    }
    /* строки фильтра, нормированная, выхода */
    uint32_t rowb = png_rowbytes(p, w);
    p->line[0] = pool_take(im, rowb + 1u);
    p->line[1] = pool_take(im, rowb + 1u);
    p->norm = pool_take(im, (size_t)w * (size_t)p->nch + 4u);
    if (p->interlace)
        p->prow8 = pool_take(im, (size_t)w * (size_t)p->nch + 4u);
    if (p->out == OUT_RAW)
        p->out_row = pool_take(im, 2u * w + 4u);
    if (!p->line[0] || !p->line[1] || !p->norm || (p->interlace && !p->prow8) ||
        (p->out == OUT_RAW && !p->out_row)) {
        img_err(im, FTC_IMG_E_TOO_BIG, 2u * (rowb + 1u) + 3u * w * (uint32_t)p->nch);
        return 0;
    }
    /* выход */
    im->info.alpha = (uint32_t)alpha;
    if (p->out == OUT_FIT) {
        im->info.alpha = 0;
        if (fit_init_pool(im, &im->fit, w, h, w, h)) {
            p->fit = &im->fit;
            fit_start(p->fit, im->emit, im->eu);
        } else {
            uint32_t fw, fh;
            fit_size(w, h, &fw, &fh);
            if (!area_init(im, &im->area, w, h, fw, fh, alpha ? 4 : gray ? 1 : 3, FMT_RGB565, 1) ||
                !png_area_mode(im, p, &im->area))
                return 0;
            p->area = &im->area;
            p->out = OUT_AREA;
            area_start(p->area, im->emit, im->eu);
        }
        im->info.fmt = FMT_RGB565;
        im->info.w = FIT_W;
        im->info.h = FIT_H;
        im->info.stride = 2 * FIT_W;
    } else if (p->out == OUT_AREA) {
        uint32_t fmt = alpha ? FMT_ARGB4 : gray ? FMT_L8 : FMT_RGB565;
        if (!area_init(im, &im->area, w, h, tw, th, alpha ? 4 : gray ? 1 : 3, fmt, 0) ||
            !png_area_mode(im, p, &im->area))
            return 0;
        p->area = &im->area;
        im->info.fmt = fmt;
        im->info.w = tw;
        im->info.h = th;
        im->info.stride = (fmt == FMT_L8 ? 1u : 2u) * tw;
        im->out_bytes = im->info.stride * th;
    } else {
        im->info.fmt = (uint32_t)p->fmt;
        im->info.w = w;
        im->info.h = h;
        im->info.stride = stride;
        im->info.pal_bytes = pal;
        im->out_bytes = pal + stride * h;
        if (p->ctype == 3) {                     /* палитра — первой, с ptr: 565 или 4444 */
            uint8_t buf[512];
            for (uint32_t i = 0; i < p->npal; i++) {
                const uint8_t *e = p->plte + 3 * i;
                uint32_t v;
                if (p->ntrns) {
                    uint32_t a = i < p->ntrns ? p->trns[i] : 255;
                    v = ((a >> 4) << 12) | ((uint32_t)(e[0] >> 4) << 8) | ((uint32_t)(e[1] >> 4) << 4) |
                        (e[2] >> 4);
                } else {
                    v = ((uint32_t)(e[0] >> 3) << 11) | ((uint32_t)(e[1] >> 2) << 5) | (e[2] >> 3);
                }
                buf[2 * i] = (uint8_t)v;
                buf[2 * i + 1] = (uint8_t)(v >> 8);
            }
            im->emit(im->eu, buf, 2 * p->npal);
        }
    }
    /* Adam7: черновик проходов — в пуле или в RAM_G за выходом (рамка) */
    if (p->interlace) {
        uint32_t total = 0;
        for (int ps = 0; ps < 7; ps++) {
            uint32_t pw, ph;
            png_pass_dims(p, ps, &pw, &ph);
            p->poff[ps] = total;
            p->pstride[ps] = (pw * (uint32_t)p->nch + 3u) & ~3u;
            total += p->pstride[ps] * ph;
        }
        p->draft = pool_take(im, total);
        if (!p->draft) {
            uint32_t base = (im->out_bytes + 3u) & ~3u;
            if (!im->boxed || !im->draft_rd || (uint64_t)base + total > im->room) {
                img_err(im, FTC_IMG_E_TOO_BIG, total);
                return 0;
            }
            p->draft_base = base;
        }
    }
    png_pass_begin(p, 0);
    return 1;
}

/* Строка прохода Adam7 (нормированная) — в черновик */
static void png_draft_row(img_t *im, png_t *p)
{
    uint32_t off = p->poff[p->pass] + p->prow * p->pstride[p->pass];
    if (p->draft)
        memcpy(p->draft + off, p->prow8, p->pstride[p->pass]);
    else
        im->draft_wr(im->eu, p->draft_base + off, p->prow8, p->pstride[p->pass]);
}

/* Сборка строк Adam7 из проходов: до rows строк; 1 — собраны все */
static int png_assemble(img_t *im, png_t *p, uint32_t rows)
{
    const uint32_t nch = (uint32_t)p->nch;
    while (rows-- && p->y < p->h) {
        uint32_t y = p->y;
        for (int ps = 0; ps < 7; ps++) {
            uint32_t pw, ph;
            png_pass_dims(p, ps, &pw, &ph);
            if (!pw || !ph || y < a7_y0[ps] || (y - a7_y0[ps]) % a7_dy[ps])
                continue;
            uint32_t off = p->poff[ps] + (y - a7_y0[ps]) / a7_dy[ps] * p->pstride[ps];
            const uint8_t *s = p->draft + off;
            if (!p->draft) {
                im->draft_rd(im->eu, p->draft_base + off, p->prow8, p->pstride[ps]);
                s = p->prow8;
            }
            uint8_t *o = p->norm + a7_x0[ps] * nch;
            const uint32_t step = a7_dx[ps] * nch;
            for (uint32_t i = 0; i < pw; i++, s += nch, o += step)
                for (uint32_t c = 0; c < nch; c++)
                    o[c] = s[c];
        }
        png_out_row(im, p, p->norm);
        p->y++;
    }
    return p->y >= p->h;
}

/* Выход tinfl: байты строк с фильтром */
static void png_inflated(void *u, const uint8_t *d, size_t n)
{
    png_t *p = (png_t *)u;
    img_t *im = IMG_OF_P(p);
    while (n && !p->err && p->pass < 7) {
        uint8_t *cur = p->line[p->lcur];
        size_t k = (size_t)(p->prowb + 1) - p->lgot;
        if (k > n)
            k = n;
        memcpy(cur + p->lgot, d, k);
        p->lgot += (uint32_t)k;
        d += k;
        n -= k;
        if (p->lgot < p->prowb + 1)
            continue;
        const uint8_t *prev = p->prow ? p->line[p->lcur ^ 1] + 1 : NULL;
        uint8_t *s = cur + 1;
        const uint32_t nb = p->prowb, bpp = (uint32_t)p->bpp;
        /* фильтры строки — вид выбран один раз на строку (модель BL616: втрое быстрее, чем по байту) */
        switch (cur[0]) {
        case 0:
            break;
        case 1:
            for (uint32_t i = bpp; i < nb; i++)
                s[i] = (uint8_t)(s[i] + s[i - bpp]);
            break;
        case 2:
            if (prev)
                for (uint32_t i = 0; i < nb; i++)
                    s[i] = (uint8_t)(s[i] + prev[i]);
            break;
        case 3:
            if (prev) {
                for (uint32_t i = 0; i < bpp && i < nb; i++)
                    s[i] = (uint8_t)(s[i] + (prev[i] >> 1));
                for (uint32_t i = bpp; i < nb; i++)
                    s[i] = (uint8_t)(s[i] + ((s[i - bpp] + prev[i]) >> 1));
            } else {
                for (uint32_t i = bpp; i < nb; i++)
                    s[i] = (uint8_t)(s[i] + (s[i - bpp] >> 1));
            }
            break;
        case 4:                                  /* Paeth: без строки сверху — как «Sub» */
            if (prev) {
                for (uint32_t i = 0; i < bpp && i < nb; i++)
                    s[i] = (uint8_t)(s[i] + prev[i]);
                for (uint32_t i = bpp; i < nb; i++)
                    s[i] = (uint8_t)(s[i] + paeth(s[i - bpp], prev[i], prev[i - bpp]));
            } else {
                for (uint32_t i = bpp; i < nb; i++)
                    s[i] = (uint8_t)(s[i] + s[i - bpp]);
            }
            break;
        default:
            p->err = 1;
            return;
        }
        if (p->interlace) {
            png_unpack(p, s, p->pw, p->prow8);
            png_draft_row(im, p);
        } else if (p->depth == 8 && !p->keyed) {
            png_out_row(im, p, s);               /* 8 бит без tRNS — строка уже нормированная */
            p->y++;
        } else {
            png_unpack(p, s, p->w, p->norm);
            png_out_row(im, p, p->norm);
            p->y++;
        }
        p->lcur ^= 1;
        p->lgot = 0;
        if (++p->prow == p->ph) {
            png_pass_begin(p, p->pass + 1);
            if (p->pass == 7 && p->interlace)
                p->assembling = 1;
        }
    }
}

/* IHDR: глубина и вид — по стандарту; размер — по пределу команды */
static int png_ihdr(img_t *im, png_t *p)
{
    p->w = be32(p->ihdr);
    p->h = be32(p->ihdr + 4);
    p->depth = p->ihdr[8];
    p->ctype = p->ihdr[9];
    p->interlace = p->ihdr[12];
    int d = p->depth, ok;
    switch (p->ctype) {
    case 0: ok = d == 1 || d == 2 || d == 4 || d == 8 || d == 16; p->ch = 1; break;
    case 3: ok = d == 1 || d == 2 || d == 4 || d == 8; p->ch = 1; break;
    case 2: ok = d == 8 || d == 16; p->ch = 3; break;
    case 4: ok = d == 8 || d == 16; p->ch = 2; break;
    case 6: ok = d == 8 || d == 16; p->ch = 4; break;
    default: ok = 0; break;
    }
    if (!ok || p->ihdr[10] || p->ihdr[11] || p->interlace > 1) {
        img_err(im, FTC_IMG_E_UNSUPPORTED, FTC_IMG_U_PNG_TYPE);
        return 0;
    }
    im->info.src_w = p->w;
    im->info.src_h = p->h;
    if (!p->w || !p->h)
        return 0;
    if ((im->boxed || fit_on(p->opts)) ? (p->w > SRC_MAX || p->h > SRC_MAX)
                                       : (p->w >= IMG_WMAX || p->h >= IMG_WMAX)) {
        img_err(im, FTC_IMG_E_SIZE, (p->w > 0xFFFFu ? 0xFFFFu : p->w) | (p->h > 0xFFFFu ? 0xFFFFu : p->h) << 16);
        return 0;
    }
    p->bits = (uint32_t)(p->ch * d);
    p->bpp = d < 8 ? 1 : p->ch * d / 8;
    return 1;
}

static int png_run(img_t *im, const uint8_t *in, size_t n, size_t *used)
{
    png_t *p = &im->u.p;
    size_t i = 0;
    for (;;) {
        if (p->assembling) {                     /* Adam7: проходы приняты — сборка кусками */
            if (!png_assemble(im, p, PNG_ASM_ROWS)) {
                *used = i;
                return FTC_IMG_BUSY;
            }
            p->assembling = 0;
        }
        if (i >= n || p->state == P_DONE || p->state == P_ERR)
            break;
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
                    if (p->type == 0x49444154u && p->state != P_ERR && !p->started) {
                        p->started = 1;                           /* первый IDAT: выход */
                        if (!png_plan(im, p))
                            p->state = P_ERR;
                    }
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
                if (!png_ihdr(im, p))
                    p->state = P_ERR;
            } else if (p->type == 0x504C5445u) {
                p->npal = p->len / 3 > 256 ? 256 : p->len / 3;
            } else if (p->type == 0x74524E53u && !p->started) {
                p->ntrns = p->len > 256 ? 256 : p->len;
                if (p->ctype == 0 && p->len >= 2) {               /* tRNS серого: одно значение */
                    p->key[0] = (uint16_t)(p->trns[0] << 8 | p->trns[1]);
                    p->has_key = 1;
                    p->ntrns = 0;
                } else if (p->ctype == 2 && p->len >= 6) {        /* tRNS RGB: цвет */
                    for (int c = 0; c < 3; c++)
                        p->key[c] = (uint16_t)(p->trns[2 * c] << 8 | p->trns[2 * c + 1]);
                    p->has_key = 1;
                    p->ntrns = 0;
                } else if (p->ctype != 3) {
                    p->ntrns = 0;
                }
            } else if (p->type == 0x49454E44u) {
                p->state = P_DONE;
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

/* ---- WebP (прошивка 40) ---------------------------------------------------------------- */
/*
 * Разбор — libwebp 1.5.0 (third_party/libwebp, правки — README.VDAC4.md): VP8 (с потерями,
 * прозрачность — блок ALPH), VP8L (без потерь), у анимации — первый кадр на прозрачном холсте.
 * Эталон — Pillow 12 (Image.open — WebPAnimDecoder: точки RGBA без предумножения, у VP8 —
 * «гладкое» восстановление цветности).
 *
 * Файл копится в пул целиком (длина — из заголовка RIFF): VP8 читает свои разделы вперемешку,
 * VP8L ссылается назад по всей картинке. Потом — разбор без новых данных, кусками
 * (FTC_IMG_BUSY): VP8 — строками макроблоков (16 строк точек), VP8L — по 16 строк. Память
 * libwebp — тоже из пула, стопкой (VDAC4WebPAlloc вместо malloc, правка utils.c): VP8 — ~2 КБ на
 * столбец макроблоков (кэш строк для фильтра, коэффициенты строки), с прозрачностью — ещё 2 байта
 * на точку (плоскость альфы и её VP8L); VP8L — вся картинка, 4 байта на точку. Не влезает —
 * «велика» (FTC_IMG_E_TOO_BIG) с оценкой, сколько нужно.
 *
 * У рамки файл больше WP_NEAR_MAX кладётся не в пул, а в RAM_G — в конец room (выход — до него,
 * поэтому, если room тесен, он мельче рамки). В пул из него читаются только заголовок VP8 с
 * разделом 0 (режимы макроблоков), ALPH и у VP8L — весь блок, а разделы токенов VP8 — окнами
 * по WP_WIN байтов (правки vp8_dec.c и bit_reader_utils.c: VP8PartitionsHookFn,
 * VP8BitReaderRefillFn). Так VP8 1920×1080 (файл 400 КБ) помещается: пулу остаётся кэш строк.
 *
 * Выход — как у PNG RGBA: с прозрачностью (флаг VP8X, у простого VP8L — его подсказка; у Pillow
 * это режим RGBA) — ARGB4, без — RGB565; рамка — уменьшение по площади, вписывание — на чёрный.
 */
enum { W_HDR, W_FILE, W_RUN, W_DONE, W_ERR };
#define WP_VP8_MBS 128u          /* VP8: макроблоков за кусок (строка — целиком) */
#define WP_VP8L_PX 32768u        /* VP8L: точек за кусок (не меньше 16 строк) */
#define WP_NEAR_MAX 49152u       /* рамка: файл больше — в RAM_G */
#define WP_WIN     512u          /* окно раздела токенов VP8 из RAM_G */

static webp_t *wp_cur;           /* чья память у libwebp (VDAC4WebPAlloc) */

#define WP_OF_IO(io) ((webp_t *)((uint8_t *)(io) - offsetof(webp_t, io)))

static uint32_t wp_le24(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
}

static uint32_t wp_le32(const uint8_t *p)
{
    return wp_le24(p) | (uint32_t)p[3] << 24;
}

/* Память libwebp (правка utils.c): из пула картинки стопкой. Блок — 8 байтов заголовка (длина,
   освобождён ли), данные, хвост с длиной; free снимает с верха пула освобождённые блоки, а
   освобождённые в середине ждут конца картинки (тогда пул — заново). */
void *VDAC4WebPAlloc(size_t size, int zero)
{
    webp_t *w = wp_cur;
    if (!w || !w->heap)
        return NULL;
    img_t *im = IMG_OF_W(w);
    size_t need = ((size + 7u) & ~(size_t)7) + 16u;
    if (size > 0x7FFFFFF0u || pool_free(im) < need) {
        uint64_t want = (uint64_t)(im->pool_at - w->pool0) + need;
        if (want > w->want)
            w->want = want > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)want;
        return NULL;
    }
    uint32_t *h = (uint32_t *)im->pool_at;
    im->pool_at += need;
    h[0] = (uint32_t)need;
    h[1] = 0;
    ((uint32_t *)im->pool_at)[-1] = (uint32_t)need;
    if (zero)
        memset(h + 2, 0, size);
    return h + 2;
}

void VDAC4WebPFree(void *ptr)
{
    webp_t *w = wp_cur;
    if (!ptr || !w || !w->heap)
        return;
    img_t *im = IMG_OF_W(w);
    ((uint32_t *)ptr)[-1] = 1;
    while (im->pool_at > w->heap) {
        uint32_t *h = (uint32_t *)(im->pool_at - ((const uint32_t *)im->pool_at)[-1]);
        if (!h[1])
            break;
        im->pool_at = (uint8_t *)h;
    }
}

/* Ошибка libwebp: не хватило памяти — «велика», иначе — «повреждена» */
static int wp_fail(img_t *im, webp_t *w, int status)
{
    if (status == VP8_STATUS_OUT_OF_MEMORY || w->want)
        img_err(im, FTC_IMG_E_TOO_BIG, w->want);
    else
        img_err(im, FTC_IMG_E_CORRUPT, 3);
    return 0;
}

/* Строка холста RGBA → выход */
static void wp_row(img_t *im, webp_t *w, const uint8_t *s)
{
    if (w->y >= w->ch)
        return;
    w->y++;
    if (w->out == OUT_AREA) {                    /* по площади: RGBA или RGBX (AM_RGBX) */
        area_row(w->area, s, im->emit, im->eu);
        return;
    }
    if (w->out == OUT_FIT) {                     /* вписывание: прозрачное — на чёрный */
        uint8_t *o = w->fit->src;
        for (uint32_t x = 0; x < w->cw; x++, s += 4, o += 3) {
            uint32_t a = w->alpha ? s[3] : 255u;
            o[0] = a == 255u ? s[0] : div255(s[0] * a);
            o[1] = a == 255u ? s[1] : div255(s[1] * a);
            o[2] = a == 255u ? s[2] : div255(s[2] * a);
        }
        fit_row(w->fit, im->emit, im->eu);
        return;
    }
    uint8_t *o = w->out_row;
    for (uint32_t x = 0; x < w->cw; x++, s += 4) {
        uint32_t v = w->alpha ? ((uint32_t)(s[3] >> 4) << 12) | ((uint32_t)(s[0] >> 4) << 8) |
                                ((uint32_t)(s[1] >> 4) << 4) | (s[2] >> 4u)
                              : ((uint32_t)(s[0] >> 3) << 11) | ((uint32_t)(s[1] >> 2) << 5) | (s[2] >> 3u);
        *o++ = (uint8_t)v;
        *o++ = (uint8_t)(v >> 8);
    }
    im->emit(im->eu, w->out_row, 2u * w->cw);
}

/* Строки холста вне кадра — прозрачные (холст WebPAnimDecoder — нули) */
static void wp_blank(img_t *im, webp_t *w, uint32_t rows)
{
    memset(w->canvas, 0, 4u * w->cw);
    while (rows--)
        wp_row(im, w, w->canvas);
}

/* Строка кадра RGBA → строка холста */
static void wp_frame_row(img_t *im, webp_t *w, const uint8_t *s)
{
    if (w->fy_out >= w->fh)
        return;
    if (w->canvas) {
        memset(w->canvas, 0, 4u * w->cw);
        memcpy(w->canvas + 4u * w->fx, s, 4u * w->fw);
        s = w->canvas;
    }
    wp_row(im, w, s);
    if (++w->fy_out == w->fh && w->canvas)
        wp_blank(im, w, w->ch - w->fy - w->fh);
}

/* VP8L: строки ARGB (слово: A — старший байт) после обратных преобразований → RGBA */
static void wp_lrows(const VP8Io *io, const uint32_t *argb, int width, int num_rows)
{
    webp_t *w = WP_OF_IO(io);
    img_t *im = IMG_OF_W(w);
    for (int r = 0; r < num_rows; r++, argb += io->width) {
        uint8_t *o = w->frow;
        for (int x = 0; x < width; x++, o += 4) {
            uint32_t v = argb[x];
            o[0] = (uint8_t)(v >> 16);
            o[1] = (uint8_t)(v >> 8);
            o[2] = (uint8_t)v;
            o[3] = (uint8_t)(v >> 24);
        }
        wp_frame_row(im, w, w->frow);
    }
}

/* VP8: строка кадра RGBA + альфа из плоскости ALPH (она у libwebp — во весь кадр) → холст */
static void wp_vrow(img_t *im, webp_t *w, uint8_t *s)
{
    const uint8_t *a = w->vp8->alpha_plane_;
    if (a) {
        a += (size_t)w->fy_out * w->fw;
        for (uint32_t x = 0; x < w->fw; x++)
            s[4 * x + 3] = a[x];
    }
    wp_frame_row(im, w, s);
}

static int wp_setup(VP8Io *io)
{
    WebPInitUpsamplers();
    return WebPIoInitFromOptions(NULL, io, MODE_RGBA);   /* без обрезки и масштаба, «гладко» */
}

/* VP8: строки YUV 4:2:0 → RGBA, как EmitFancyRGB libwebp (io_dec.c): цветность — «гладко»,
   парами строк; последняя строка куска ждёт первую строку следующего (tmp_*). Строки выходят
   по порядку — хватает буфера на две. */
static int wp_put(const VP8Io *io)
{
    webp_t *w = WP_OF_IO(io);
    img_t *im = IMG_OF_W(w);
    const WebPUpsampleLinePairFunc up = WebPUpsamplers[MODE_RGBA];
    const int mb_w = io->mb_w, uv_w = (mb_w + 1) / 2, y_end = io->mb_y + io->mb_h;
    const uint8_t *cy = io->y, *cu = io->u, *cv = io->v, *tu = w->tmp_u, *tv = w->tmp_v;
    uint8_t *r0 = w->pair, *r1 = w->pair + 4 * mb_w;
    int y = io->mb_y;
    if (y == 0) {                                /* первая строка: цветность — своя же сверху */
        up(cy, NULL, cu, cv, cu, cv, r0, NULL, mb_w);
        wp_vrow(im, w, r0);
    } else {                                     /* отложенная строка прошлого куска и первая */
        up(w->tmp_y, cy, tu, tv, cu, cv, r0, r1, mb_w);
        wp_vrow(im, w, r0);
        wp_vrow(im, w, r1);
    }
    for (; y + 2 < y_end; y += 2) {
        tu = cu;
        tv = cv;
        cu += io->uv_stride;
        cv += io->uv_stride;
        cy += 2 * io->y_stride;
        up(cy - io->y_stride, cy, tu, tv, cu, cv, r0, r1, mb_w);
        wp_vrow(im, w, r0);
        wp_vrow(im, w, r1);
    }
    cy += io->y_stride;
    if (io->crop_top + y_end < io->crop_bottom) {
        memcpy(w->tmp_y, cy, (size_t)mb_w);
        memcpy(w->tmp_u, cu, (size_t)uv_w);
        memcpy(w->tmp_v, cv, (size_t)uv_w);
    } else if (!(y_end & 1)) {                   /* последняя строка чётной высоты */
        up(cy, NULL, cu, cv, cu, cv, r0, NULL, mb_w);
        wp_vrow(im, w, r0);
    }
    return 1;
}

/* Файл в RAM_G: байты [off, off + n) — в dst (draft_rd — словами; до 3 байтов за концом файла —
   ещё в room: base кратно 4, base + size ≤ room) */
static void wp_read(img_t *im, webp_t *w, uint32_t off, uint8_t *dst, uint32_t n)
{
    uint8_t tmp[256];
    while (n) {
        uint32_t a = (w->base + off) & ~3u, skip = w->base + off - a;
        uint32_t k = n + skip > sizeof tmp ? (uint32_t)sizeof tmp : (n + skip + 3u) & ~3u;
        im->draft_rd(im->eu, a, tmp, k);
        uint32_t m = k - skip < n ? k - skip : n;
        memcpy(dst, tmp + skip, m);
        dst += m;
        off += m;
        n -= m;
    }
}

/* Байты файла [off, off + n): в пуле — на месте, в RAM_G — читаются в buf */
static const uint8_t *wp_at(img_t *im, webp_t *w, uint32_t off, uint32_t n, uint8_t *buf)
{
    if (!w->far)
        return w->file + off;
    wp_read(im, w, off, buf, n);
    return buf;
}

/* VP8 из RAM_G: начало раздела токенов p (место — от таблицы размеров разделов) — окном */
static void wp_part(VP8Decoder *dec, int p, size_t off, size_t size)
{
    webp_t *w = wp_cur;
    w->ppos[p] = w->parts_off + (uint32_t)off;
    w->pend[p] = w->ppos[p] + (uint32_t)size;
    VP8InitBitReader(dec->parts_ + p, w->win + (uint32_t)p * WP_WIN, 0);   /* пусто — сразу wp_refill */
}

/* VP8 из RAM_G: окно раздела кончилось — следующее (у раздела 0 и чужих — нет: конец данных) */
static void wp_refill(VP8BitReader *br)
{
    webp_t *w = wp_cur;
    if (!w || !w->vp8 || !w->win)
        return;
    VP8Decoder *dec = w->vp8;
    for (int p = 0; p <= dec->num_parts_minus_one_; p++) {
        if (br != dec->parts_ + p)
            continue;
        uint32_t pos = w->ppos[p], end = w->pend[p];
        if (pos >= end)
            return;
        img_t *im = IMG_OF_W(w);
        uint8_t *win = w->win + (uint32_t)p * WP_WIN;
        uint32_t a = (w->base + pos) & ~3u, skip = w->base + pos - a, k = end - pos + skip;
        if (k > WP_WIN)
            k = WP_WIN;
        im->draft_rd(im->eu, a, win, (k + 3u) & ~3u);
        VP8BitReaderSetBuffer(br, win + skip, k - skip);
        w->ppos[p] = pos + (k - skip);
        return;
    }
}

/* Блоки RIFF: холст (VP8X), кадр (у анимации — первый ANMF), ALPH и VP8 / VP8L — как
   WebPDemux: у VP8 данные — ровно полезная нагрузка блока. 0 — повреждён. */
static int wp_chunks(img_t *im, webp_t *w)
{
    uint8_t b[16];
    uint32_t at = 12, end = w->size;
    int vp8x = 0, anim = 0, found = 0;           /* anim: 1 — анимация, 2 — внутри первого кадра */
    while (end - at >= 8) {
        uint8_t tag[4];
        const uint8_t *h = wp_at(im, w, at, 8, b);
        uint32_t len = wp_le32(h + 4), d = at + 8;
        memcpy(tag, h, 4);
        if (len > end - d)
            return 0;
        uint32_t next = d + len + (len & 1u);
        if (at == 12 && !memcmp(tag, "VP8X", 4)) {
            if (len < 10)
                return 0;
            const uint8_t *x = wp_at(im, w, d, 10, b);
            vp8x = 1;
            anim = (x[0] & 0x02) != 0;
            w->alpha = (x[0] & 0x10) != 0;
            w->cw = wp_le24(x + 4) + 1;
            w->ch = wp_le24(x + 7) + 1;
        } else if (anim == 1 && !memcmp(tag, "ANMF", 4)) {
            if (len < 16)
                return 0;
            const uint8_t *x = wp_at(im, w, d, 16, b);
            w->fx = 2 * wp_le24(x);
            w->fy = 2 * wp_le24(x + 3);
            w->fw = wp_le24(x + 6) + 1;
            w->fh = wp_le24(x + 9) + 1;
            anim = 2;
            end = d + len;                       /* дальше — блоки кадра */
            next = d + 16;
        } else if (anim != 1 && vp8x && !memcmp(tag, "ALPH", 4)) {
            w->alph_off = d;
            w->alph_size = len;
        } else if (anim != 1 && (!memcmp(tag, "VP8 ", 4) || !memcmp(tag, "VP8L", 4))) {
            w->lossless = tag[3] == 'L';
            w->img_off = d;
            w->img_size = len;
            found = 1;
            break;
        }
        at = next < end ? next : end;
    }
    if (!found)
        return 0;
    int iw, ih, ha = 0;
    uint32_t hn = w->img_size < 16u ? w->img_size : 16u;
    const uint8_t *g = wp_at(im, w, w->img_off, hn, b);
    if (w->lossless ? !VP8LGetInfo(g, hn, &iw, &ih, &ha) : !VP8GetInfo(g, hn, w->img_size, &iw, &ih))
        return 0;
    if (!vp8x) {                                 /* простой: холст — картинка; альфа — подсказка VP8L */
        w->cw = (uint32_t)iw;
        w->ch = (uint32_t)ih;
        w->alpha = w->lossless && ha;
    }
    if (anim != 2) {
        w->fx = w->fy = 0;
        w->fw = w->cw;
        w->fh = w->ch;
    }
    if (w->lossless)
        w->alph_size = 0;                        /* у VP8L ALPH не нужен (libwebp его не берёт) */
    return (uint32_t)iw == w->fw && (uint32_t)ih == w->fh && w->fx + w->fw <= w->cw && w->fy + w->fh <= w->ch;
}

/* Решение о выходе и строки. 0 — ошибка. */
static int wp_plan(img_t *im, webp_t *w)
{
    const uint32_t W = w->cw, H = w->ch;
    im->info.src_w = W;
    im->info.src_h = H;
    if ((im->boxed || fit_on(im->opts)) ? (W > SRC_MAX || H > SRC_MAX) : (W >= IMG_WMAX || H >= IMG_WMAX)) {
        img_err(im, FTC_IMG_E_SIZE, (W > 0xFFFFu ? 0xFFFFu : W) | (H > 0xFFFFu ? 0xFFFFu : H) << 16);
        return 0;
    }
    uint32_t tw = 0, th = 0;
    w->out = OUT_RAW;
    if (fit_on(im->opts)) {
        w->out = OUT_FIT;
    } else if (im->boxed) {
        int r = box_target(im, W, H, 2u * W * H, 2u, &tw, &th);
        if (r < 0)
            return 0;
        if (r == 0)
            w->out = OUT_AREA;
    }
    if (w->fw != W || w->fh != H)
        w->canvas = pool_take(im, 4u * W);
    if (w->lossless) {
        w->frow = pool_take(im, 4u * w->fw);
    } else {
        w->pair = pool_take(im, 8u * w->fw);
        w->tmp_y = pool_take(im, w->fw);
        w->tmp_u = pool_take(im, (w->fw + 1u) / 2u);
        w->tmp_v = pool_take(im, (w->fw + 1u) / 2u);
    }
    if (w->out == OUT_RAW)
        w->out_row = pool_take(im, 2u * W);
    if ((w->fw != W || w->fh != H) && !w->canvas) {
        img_err(im, FTC_IMG_E_TOO_BIG, (uint32_t)(im->pool_at - w->pool0) + 4u * W);
        return 0;
    }
    if ((w->lossless ? !w->frow : !w->pair || !w->tmp_y || !w->tmp_u || !w->tmp_v) || (w->out == OUT_RAW && !w->out_row)) {
        img_err(im, FTC_IMG_E_TOO_BIG, (uint32_t)(im->pool_at - w->pool0) + 12u * W);
        return 0;
    }
    im->info.alpha = (uint32_t)w->alpha;
    if (w->out == OUT_FIT) {
        im->info.alpha = 0;
        if (fit_init_pool(im, &im->fit, W, H, W, H)) {
            w->fit = &im->fit;
            fit_start(w->fit, im->emit, im->eu);
        } else {
            uint32_t ow, oh;
            fit_size(W, H, &ow, &oh);
            if (!area_init(im, &im->area, W, H, ow, oh, w->alpha ? 4 : 3, FMT_RGB565, 1))
                return 0;
            w->area = &im->area;
            w->out = OUT_AREA;
            area_start(w->area, im->emit, im->eu);
        }
        im->info.fmt = FMT_RGB565;
        im->info.w = FIT_W;
        im->info.h = FIT_H;
        im->info.stride = 2 * FIT_W;
    } else if (w->out == OUT_AREA) {
        uint32_t fmt = w->alpha ? FMT_ARGB4 : FMT_RGB565;
        if (!area_init(im, &im->area, W, H, tw, th, w->alpha ? 4 : 3, fmt, 0))
            return 0;
        w->area = &im->area;
        im->info.fmt = fmt;
        im->info.w = tw;
        im->info.h = th;
        im->info.stride = 2u * tw;
        im->out_bytes = im->info.stride * th;
    } else {
        im->info.fmt = w->alpha ? FMT_ARGB4 : FMT_RGB565;
        im->info.w = W;
        im->info.h = H;
        im->info.stride = 2u * W;
        im->out_bytes = 2u * W * H;
    }
    if (w->area && !w->alpha)
        w->area->mode = AM_RGBX;                 /* строка RGBA, альфа не нужна */
    return 1;
}

/* Данные для libwebp: в пуле — на месте; файл в RAM_G — в пул ALPH, у VP8L — весь блок, у VP8 —
   заголовок кадра, раздел 0 и таблица размеров разделов (разделы токенов — окнами). 0 — ошибка. */
static int wp_load(img_t *im, webp_t *w)
{
    if (!w->far) {
        w->img = w->file + w->img_off;
        w->alph = w->alph_size ? w->file + w->alph_off : NULL;
        return 1;
    }
    uint32_t head = w->img_size;
    if (!w->lossless) {
        uint8_t h[10];
        wp_read(im, w, w->img_off, h, 10);       /* VP8GetInfo: блок не короче 10 байтов */
        uint32_t p0 = (h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16) >> 5;   /* длина раздела 0 */
        w->parts_off = w->img_off + 10u + p0;
        if (head > 10u + p0 + 3u * (MAX_NUM_PARTITIONS - 1))
            head = 10u + p0 + 3u * (MAX_NUM_PARTITIONS - 1);
        w->win = pool_take(im, MAX_NUM_PARTITIONS * WP_WIN);
    }
    uint8_t *a = w->alph_size ? pool_take(im, w->alph_size) : NULL;
    uint8_t *g = pool_take(im, head);
    if (!g || (w->alph_size && !a) || (!w->lossless && !w->win)) {
        img_err(im, FTC_IMG_E_TOO_BIG, (uint32_t)(im->pool_at - w->pool0) + head + w->alph_size);
        return 0;
    }
    wp_read(im, w, w->img_off, g, head);
    if (a)
        wp_read(im, w, w->alph_off, a, w->alph_size);
    w->img = g;
    w->alph = a;
    return 1;
}

/* Файл принят: блоки, выход, декодер libwebp. 0 — ошибка. */
static int wp_start(img_t *im, webp_t *w)
{
    if (!wp_chunks(im, w)) {
        img_err(im, FTC_IMG_E_CORRUPT, 3);
        return 0;
    }
    if (!wp_plan(im, w) || !wp_load(im, w))
        return 0;
    if (w->canvas && w->fy)
        wp_blank(im, w, w->fy);
    im->pool_at = (uint8_t *)(((uintptr_t)im->pool_at + 7u) & ~(uintptr_t)7);
    w->heap = im->pool_at;                       /* дальше пул — libwebp */
    VP8LRowsHookFn = wp_lrows;
    VP8PartitionsHookFn = w->win ? wp_part : NULL;
    VP8BitReaderRefillFn = w->win ? wp_refill : NULL;
    if (!VP8InitIo(&w->io))
        return wp_fail(im, w, VP8_STATUS_INVALID_PARAM);
    w->io.data = w->img;
    w->io.data_size = w->img_size;               /* у VP8 из RAM_G в пуле — только начало */
    if (w->lossless) {
        w->buf.colorspace = MODE_RGBA;
        w->buf.width = (int)w->fw;
        w->buf.height = (int)w->fh;
        w->params.output = &w->buf;
        w->io.opaque = &w->params;
        w->vp8l = VP8LNew();
        if (!w->vp8l)
            return wp_fail(im, w, VP8_STATUS_OUT_OF_MEMORY);
        if (!VP8LDecodeHeader(w->vp8l, &w->io))
            return wp_fail(im, w, w->vp8l->status_);
    } else {
        w->vp8 = VP8New();
        if (!w->vp8)
            return wp_fail(im, w, VP8_STATUS_OUT_OF_MEMORY);
        w->vp8->alpha_data_ = w->alph;
        w->vp8->alpha_data_size_ = w->alph_size;
        w->io.setup = wp_setup;
        w->io.put = wp_put;
        if (!VP8GetHeaders(w->vp8, &w->io) || VP8EnterCritical(w->vp8, &w->io) != VP8_STATUS_OK ||
            !VP8InitFrame(w->vp8, &w->io))
            return wp_fail(im, w, w->vp8->status_);
        w->vp8->mb_y_ = 0;
    }
    if ((uint32_t)w->io.width != w->fw || (uint32_t)w->io.height != w->fh)
        return wp_fail(im, w, VP8_STATUS_BITSTREAM_ERROR);
    return 1;
}

/* Кусок разбора: 1 — дальше, 2 — всё, 0 — ошибка */
static int wp_step(img_t *im, webp_t *w)
{
    if (w->lossless) {
        uint32_t rows = WP_VP8L_PX / w->fw;
        rows = rows < 16u ? 16u : rows & ~15u;
        w->lrow = w->fh - w->lrow > rows ? w->lrow + rows : w->fh;
        if (!VP8LDecodeImageRows(w->vp8l, (int)w->lrow))
            return wp_fail(im, w, w->vp8l->status_);
        if (w->lrow < w->fh)
            return 1;
    } else {
        VP8Decoder *dec = w->vp8;
        uint32_t mbs = 0;
        /* тело ParseFrame (vp8_dec.c) — по строке макроблоков */
        for (; dec->mb_y_ < dec->br_mb_y_ && mbs < WP_VP8_MBS; dec->mb_y_++, mbs += (uint32_t)dec->mb_w_) {
            VP8BitReader *tbr = &dec->parts_[dec->mb_y_ & dec->num_parts_minus_one_];
            if (!VP8ParseIntraModeRow(&dec->br_, dec))
                return wp_fail(im, w, VP8_STATUS_NOT_ENOUGH_DATA);
            for (; dec->mb_x_ < dec->mb_w_; ++dec->mb_x_)
                if (!VP8DecodeMB(dec, tbr))
                    return wp_fail(im, w, VP8_STATUS_NOT_ENOUGH_DATA);
            VP8InitScanline(dec);
            if (!VP8ProcessRow(dec, &w->io))
                return wp_fail(im, w, dec->status_);
        }
        if (dec->mb_y_ < dec->br_mb_y_)
            return 1;
        if (!VP8ExitCritical(dec, &w->io))
            return wp_fail(im, w, dec->status_);
    }
    if (w->y != w->ch)
        return wp_fail(im, w, VP8_STATUS_BITSTREAM_ERROR);
    return 2;
}

static int webp_run(img_t *im, const uint8_t *in, size_t n, size_t *used)
{
    webp_t *w = &im->u.w;
    size_t i = 0;
    wp_cur = w;
    while (w->state == W_HDR && i < n) {
        w->hdr[w->hdr_got++] = in[i++];
        if (w->hdr_got < 12)
            continue;
        uint32_t len = wp_le32(w->hdr + 4);
        if (memcmp(w->hdr, "RIFF", 4) || memcmp(w->hdr + 8, "WEBP", 4)) {
            img_err(im, FTC_IMG_E_UNSUPPORTED, FTC_IMG_U_FORMAT);
            w->state = W_ERR;
        } else if (len < 4u + 8u || len > 0x7FFFFFF0u) {
            w->state = W_ERR;
        } else {
            uint32_t room = im->room & ~3u;
            w->size = len + 8u;
            if (im->boxed && im->draft_wr && im->draft_rd && w->size > WP_NEAR_MAX && w->size + 4u <= room) {
                w->far = 1;                      /* файл — в конец room, выход — до него */
                w->base = (room - w->size) & ~3u;
                im->room = w->base;
                memcpy(w->stage, w->hdr, 12);
                w->staged = 12;
            } else if ((w->file = pool_take(im, w->size)) != NULL) {
                memcpy(w->file, w->hdr, 12);
            } else {
                img_err(im, FTC_IMG_E_TOO_BIG, w->size);
                w->state = W_ERR;
            }
            if (w->state != W_ERR) {
                w->got = 12;
                w->state = W_FILE;
            }
        }
    }
    if (w->state == W_FILE && i < n) {
        size_t k = n - i;
        if (k > w->size - w->got)
            k = w->size - w->got;
        if (w->far) {                            /* в RAM_G — кусками по 256 байтов */
            for (size_t q = 0; q < k;) {
                uint32_t m = (uint32_t)sizeof w->stage - w->staged;
                if (m > k - q)
                    m = (uint32_t)(k - q);
                memcpy(w->stage + w->staged, in + i + q, m);
                w->staged += m;
                q += m;
                if (w->staged == sizeof w->stage) {
                    im->draft_wr(im->eu, w->base + w->flushed, w->stage, sizeof w->stage);
                    w->flushed += (uint32_t)sizeof w->stage;
                    w->staged = 0;
                }
            }
        } else {
            memcpy(w->file + w->got, in + i, k);
        }
        w->got += (uint32_t)k;
        i += k;
    }
    if (w->state == W_FILE && w->got == w->size) {
        if (w->far && w->staged) {               /* хвост — до кратного 4 (в room помещается) */
            uint32_t m = (w->staged + 3u) & ~3u;
            memset(w->stage + w->staged, 0, m - w->staged);
            im->draft_wr(im->eu, w->base + w->flushed, w->stage, m);
        }
        w->state = wp_start(im, w) ? W_RUN : W_ERR;
    } else if (w->state == W_RUN) {
        int r = wp_step(im, w);
        w->state = r == 2 ? W_DONE : r ? W_RUN : W_ERR;
    }
    *used = i;
    if (w->state == W_ERR)
        return FTC_IMG_ERROR;
    if (w->state == W_DONE)
        return FTC_IMG_DONE;
    return w->state == W_RUN ? FTC_IMG_BUSY : FTC_IMG_NEED_INPUT;
}

/* --------------------------------------------------------------------------------------- */

void ftc_img_begin_req(void *st, const ftc_img_req_t *rq, void *inflate_state)
{
    img_t *im = (img_t *)st;
    tables();
    im->kind = 0;
    memset(&im->info, 0, sizeof im->info);
    memset(&im->u.j, 0, offsetof(jpeg_t, jb));
    memset(&im->u.p, 0, sizeof im->u.p);
    im->opts = rq->opts;
    im->boxed = rq->boxed;
    im->box_w = rq->box_w;
    im->box_h = rq->box_h;
    im->room = rq->room;
    im->draft_wr = rq->draft_wr;
    im->draft_rd = rq->draft_rd;
    if (im->boxed)                               /* у рамки холста нет */
        im->opts &= ~(uint32_t)OPT_FULLSCREEN;
    im->u.j.opts = im->opts;
    im->u.p.opts = im->opts;
    im->u.p.infl = (uint8_t *)inflate_state;
    im->infl = (uint8_t *)inflate_state;
    im->pool_at = (uint8_t *)(((uintptr_t)(im + 1) + 7u) & ~(uintptr_t)7);
    im->pool_end = (uint8_t *)im + FTC_IMG_BYTES;
    im->out_bytes = 0;
    if (im->boxed) {
        if (rq->opts & OPT_MEDIAFIFO)
            img_err(im, FTC_IMG_E_UNSUPPORTED, FTC_IMG_U_OPTION);
        else if (!im->box_w || !im->box_h)
            img_err(im, FTC_IMG_E_SIZE, 0);
    }
}

void ftc_img_begin(void *st, uint32_t opts, void *inflate_state)
{
    ftc_img_req_t rq = {opts, 0, 0, 0, 0, NULL, NULL};
    ftc_img_begin_req(st, &rq, inflate_state);
}

int ftc_img_run(void *st, const uint8_t *in, size_t n, size_t *used,
                void (*emit)(void *u, const uint8_t *p, size_t n), void *u)
{
    img_t *im = (img_t *)st;
    *used = 0;
    im->emit = emit;
    im->eu = u;
    if (im->info.err)
        return FTC_IMG_ERROR;
    if (!im->kind) {
        if (!n)
            return FTC_IMG_NEED_INPUT;
        if (in[0] == 0xFF) {
            jpeg_t *j = &im->u.j;
            memset(j, 0, offsetof(jpeg_t, jb));
            j->opts = im->opts;
            /* JPEG не нужен inflate: его место (у сопроцессора — сразу за состоянием) — в пул */
            if (im->infl == im->pool_end)
                im->pool_end = im->infl + FTC_INFLATE_BYTES;
            j->state = J_SOI0;
            im->kind = 1;
            im->info.jpeg = 1;
        } else if (in[0] == 0x89) {
            png_t *p = &im->u.p;
            memset(p, 0, sizeof *p);
            p->opts = im->opts;
            p->infl = im->infl;
            p->state = P_SIG;
            ftc_inflate_begin(p->infl);
            im->kind = 2;
        } else if (in[0] == 'R') {               /* RIFF: WebP */
            webp_t *w = &im->u.w;
            memset(w, 0, sizeof *w);
            if (im->infl == im->pool_end)        /* inflate не нужен — его место в пул, как у JPEG */
                im->pool_end = im->infl + FTC_INFLATE_BYTES;
            w->pool0 = im->pool_at;
            w->state = W_HDR;
            im->kind = 3;
        } else {
            img_err(im, FTC_IMG_E_UNSUPPORTED, FTC_IMG_U_FORMAT);
            return FTC_IMG_ERROR;
        }
    }
    int r = im->kind == 1 ? jpeg_run(im, in, n, used, emit, u)
          : im->kind == 2 ? png_run(im, in, n, used) : webp_run(im, in, n, used);
    if (r == FTC_IMG_ERROR)
        img_err(im, FTC_IMG_E_CORRUPT, (uint32_t)im->kind);
    return r;
}

void ftc_img_get_info(const void *st, ftc_img_info_t *info)
{
    *info = ((const img_t *)st)->info;
}
