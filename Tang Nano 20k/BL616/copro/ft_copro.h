/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Сопроцессор FT812 — то, что в FT812 делает микропрограмма J1: читает кольцо RAM_CMD,
 * пишет дисплей-лист (RAM_DL с REG_CMD_DL) и RAM_G. Регистры, кольцо, RAM_DL и RAM_G живут
 * в ПЛИС Tang (ft_core); здесь — только исполнение команд.
 *
 * Ядро переносимое: к памяти FT812 оно ходит через шину платформы (ftc_bus_t). На BL616 это
 * SPI к ПЛИС, на ПК — массив (test/host_test.c, проверка против bt8xxemu).
 *
 * Поведение — как у FT812 (bt8xxemu) и eve-emu (alfishe/unreal-ng, MIT): команда снимается
 * с кольца вместе с параметрами, слова не 0xFFFFFFxx — слова дисплей-листа, CMD_DLSTART ждёт
 * выполненной смены листа, ошибка — REG_CMD_READ = 0xFFF до сброса REG_CPURESET.
 * REG_CMD_READ уходит в ПЛИС, когда всё, что команды до него сделали, уже записано.
 */
#ifndef FT_COPRO_H
#define FT_COPRO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Адреса FT812 */
#define FTC_RAM_DL      0x300000u
#define FTC_RAM_CMD     0x308000u
#define FTC_REG         0x302000u
#define FTC_REG_INT_FLAGS (FTC_REG + 0x0A8u)
#define FTC_REG_DLSWAP  (FTC_REG + 0x054u)
#define FTC_REG_ROTATE  (FTC_REG + 0x058u)
#define FTC_REG_CMD_READ  (FTC_REG + 0x0F8u)
#define FTC_REG_CMD_WRITE (FTC_REG + 0x0FCu)
#define FTC_REG_CMD_DL  (FTC_REG + 0x100u)
#define FTC_REG_CPURESET (FTC_REG + 0x020u)

/* Флаги прерывания, которые поднимает сопроцессор */
#define FTC_INT_CMDEMPTY 0x20u
#define FTC_INT_CMDFLAG  0x40u

/* Состояние ПЛИС одним запросом. */
typedef struct ftc_status {
    uint32_t cmd_write;   /* REG_CMD_WRITE */
    uint32_t cmd_read;    /* REG_CMD_READ (хост пишет его при восстановлении после ошибки) */
    uint32_t cmd_dl;      /* REG_CMD_DL (хост может писать его, пока сопроцессор стоит) */
    uint32_t dlswap;      /* REG_DLSWAP: не 0 — смена листа ещё не выполнена */
    uint32_t cpureset;    /* REG_CPURESET: бит 0 — сопроцессор держится в сбросе */
    uint32_t resets;      /* счётчик сбросов (REG_CPURESET 1 → 0, сброс ядра FT812): сменился —
                             сопроцессор начинает заново, даже если сам сброса не застал */
} ftc_status_t;

/* Шина к памяти FT812. Адреса кратны 4, n слов подряд с растущими адресами. */
typedef struct ftc_bus {
    void *ctx;
    void (*rd)(void *ctx, uint32_t addr, uint32_t *w, uint32_t n);
    void (*wr)(void *ctx, uint32_t addr, const uint32_t *w, uint32_t n);
    void (*status)(void *ctx, ftc_status_t *st);
    void (*irq)(void *ctx, uint32_t flags);        /* поднять флаги REG_INT_FLAGS */
    uint32_t (*ms)(void *ctx);                     /* время в мс (CMD_INTERRUPT); может быть NULL */
    /* Необязательно (NULL — обычная wr): запись без ожидания — слова w не трогать до wr_wait;
       pump — продвинуть её (звать почаще, пока идёт счёт), wr_wait — дождаться конца. Прочие
       обмены шины сами ждут её конца. */
    void (*wr_async)(void *ctx, uint32_t addr, const uint32_t *w, uint32_t n);
    void (*pump)(void *ctx);
    void (*wr_wait)(void *ctx);
    /* Необязательно (для замера, прошивка 26): время в мкс и сколько мкс с запуска шина простояла,
       ожидая места в очереди ПЛИС (room) и читая из ПЛИС (read). */
    uint32_t (*us)(void *ctx);
    void (*link_us)(void *ctx, uint32_t link[5]);   /* room, read мкс; кусков; опросов; частота данных */
} ftc_bus_t;

/* Ошибки (REG_CMD_READ = 0xFFF) */
enum {
    FTC_FAULT_NONE = 0,
    FTC_FAULT_UNKNOWN,      /* команда, которой здесь нет */
    FTC_FAULT_DL_FULL,      /* дисплей-лист длиннее 2048 слов */
    FTC_FAULT_INFLATE,      /* испорченный поток CMD_INFLATE */
    FTC_FAULT_3D,           /* команда 3D с негодными параметрами (ft_3d.c) */
    FTC_FAULT_IMAGE,        /* CMD_LOADIMAGE: не JPEG baseline / PNG 8 бит или испорчена */
};

#define FTC_INFLATE_BYTES (32768 + 11264)  /* tinfl: словарь 32 КБ и его состояние */
#define FTC_IMG_BYTES_DEF (100u * 1024u)   /* CMD_LOADIMAGE: состояние ft_image.c */
#define FTC_DLQ_WORDS 128
#define FTC_MW_WORDS  256         /* слов в буфере записи байтов (прошивка 32: два, строка 512 точек RGB565) */
#define FTC_MF_BUF    2048             /* медиа-FIFO: прочитанные из RAM_G байты потока */
#define FTC_TEXT_MAX  256              /* строка CMD_TEXT; длиннее — хвост не рисуется */
#define FTC_ROM_FONTROOT 0x2FFFFCu     /* адрес таблицы ПЗУ-шрифтов (у FT812 — 0x201EE0) */
#define FTC_ROM_WAIT_MS  6000u         /* ждать образ ПЗУ-шрифтов после сброса сопроцессора */
#define FTC_TEXT_RETRY_MS 1000u        /* метрики шрифта прочитались негодными — читать снова столько */
#define FTC_TEXT_TRIES   8u            /* ... или (нет часов) столько раз подряд */

/* --- 3D: расширение VDAC4 (ft_3d.c) ------------------------------------------------------- */
#define FTC3D_VERSION    3         /* 3 — indexed textures, perspective UV/RGB, material */
#define FTC3D_MAX_TRIS   1280      /* треугольников в кадре (после отсечения) */
#define FTC3D_MAX_VERTS  512       /* вершин в одной CMD_3D_MESH */
#define FTC3D_MAX_FACES  1024      /* граней в одной CMD_3D_MESH */
#define FTC3D_STRIP      16384     /* байтов полосы цвета (точек — вдвое меньше); z — по 4 байта */
#define FTC3D_STACK      8         /* глубина CMD_3D_PUSH */
#define FTC3D_CACHE_WORDS 6144    /* кэш неизменных сеток (флаг 8 CMD_3D_MESH): слов */
#define FTC3D_CACHE_ITEMS 16
#define FTC3D_DIAG_ADDR  0x1DFE00u  /* блок замера RENDER: выше 1 МБ RAM_G FT812, у самопроверки SPI */
#define FTC3D_DIAG_RESET 0x3D1A6000u
#define FTC3D_DIAG_RUN   0x3D1A6001u
/* С прошивки 28: слово просьбы перебора вариантов (render_variant в ft_3d.c) — 0x3D1A60nn, где
   nn — сколько вариантов перебирать по кадрам (1…8); читается при FTC3D_DIAG_RESET. Иначе —
   обычный вариант. Суммы по вариантам — в блоке за меткой, слова 16…47 (по 4 на вариант). */
#define FTC3D_DIAG_MODE  (FTC3D_DIAG_ADDR + 0xC0u)
#define FTC3D_DIAG_WORDS 48u
#define FTC3D_VARIANTS   8u

typedef struct ftc3d_tri {
    int32_t  x[3], y[3];           /* экран цели, 1/16 точки; обход — по часовой (y вниз) */
    float    iz[3];                /* znear / z: 1 — у ближней плоскости, к 0 — вдаль */
    uint8_t  sh[3];                /* освещённость 0…255 */
    uint8_t  pad;
    uint16_t color;                /* RGB565 */
    int16_t  y0, y1;               /* строки цели, которые он задевает */
    float attr[5][3];             /* u, v, r, g, b multiplied by znear/z; pad=1 */
} ftc3d_tri_t;

typedef struct ftc3d_vtx {
    float x, y, z;                 /* в пространстве камеры */
    float nx, ny, nz;              /* нормаль вершины (CMD_3D_MESH, гладкое освещение) */
} ftc3d_vtx_t;

typedef struct ftc3d_cached {
    uint32_t addr, words, at;      /* кусок RAM_G и его место в cache */
} ftc3d_cached_t;

typedef struct ftc3d {
    float    m[12];                /* модель-вид 3×4 по строкам */
    float    stack[FTC3D_STACK][12];
    int      sp;
    uint32_t addr, fmt, w, h;      /* цель в RAM_G; w = 0 — не задана */
    uint32_t aa;                   /* 1 — сглаживание SSAA 2×2: 4 отсчёта на точку */
    float    focal, znear;
    float    lx, ly, lz;           /* направление на свет, единичное */
    uint32_t ambient, diffuse;
    uint32_t clear;                /* ARGB8888 */
    uint32_t ntris, dropped;
    uint32_t last_tris, last_ms;
    float fog_near, fog_far, color_steps;
    uint32_t material, fog_color, tex_w, tex_h, tex_valid, tex_format;
    float unorm[256];              /* exact byte/255 conversion, outside the pixel loop */
    /* Быстрый путь RENDER (прошивка 25, ft_3d.c raster_tex_fast): палитра одним словом на
       цвет — r, g, b, бит 24 — проходит порог alpha, 25 — UNLIT, 26 — NO_FOG (строится в начале
       каждого RENDER); таблицы квантования: номер ступени floor(значение · color_steps) (без
       квантования — round(значение · 255)) → итоговый байт, сразу на своём месте в RGB565 (по
       таблице на канал r, g, b), до fast_wmax (дальше байт уже 255); строятся заново, когда
       меняется color_steps (−1 — ещё не строились). */
    /* Замер на плате (прошивка 26): программа пишет FTC3D_DIAG_RESET в RAM_G по
       FTC3D_DIAG_ADDR, и после каждого RENDER сопроцессор кладёт за меткой суммы по фазам с
       последнего сброса (ftc3d_diag в ft_3d.c). Без метки ничего не пишется. */
    uint32_t diag[FTC3D_DIAG_WORDS];
    uint32_t diag_texmesh_us, diag_texmesh_n, diag_room0, diag_read0, diag_chunks0, diag_snaps0;
    uint32_t var_n, var_i;         /* перебор вариантов по кадрам (прошивка 28): сколько и счётчик */
    uint32_t fast_pump_mask;       /* быстрый путь зовёт pump на строках y, где (y & маска) == 0 */
    uint32_t fast_pal[256];
    uint16_t fast_q565[3][512];
    int32_t  fast_wmax;
    float    fast_lut_steps;
    /* С tris — общая рабочая память сопроцессора (FTC_SCRATCH): у 3D — треугольники кадра,
       сетка команды, кэш сеток, полосы, z-буфер; у CMD_LOADIMAGE — состояние ft_image.c и с
       FTC_IMG_BYTES — inflate (PNG); у CMD_INFLATE — inflate; при обновлении прошивки — образ.
       Задачи не пересекаются; после чужой 3D сбрасывает кэш сеток и незаконченный кадр
       (ftc3d_scratch_lost), параметры выше не трогаются. */
    _Alignas(8) ftc3d_tri_t tris[FTC3D_MAX_TRIS];
    ftc3d_vtx_t v[FTC3D_MAX_VERTS];
    uint8_t  vsh[FTC3D_MAX_VERTS];
    uint32_t faces[2 * FTC3D_MAX_FACES];
    uint32_t cache[FTC3D_CACHE_WORDS];
    ftc3d_cached_t cached[FTC3D_CACHE_ITEMS];
    uint32_t ncached, cache_used;
    uint32_t palette[512];        /* ARGB8888 + optional generic per-entry material flags */
    uint8_t texture[16384];       /* indexed atlas, loaded explicitly by CMD_3D_TEXTURE */
    _Alignas(32) uint8_t pix[2][FTC3D_STRIP];   /* полосы: одна считается, другая уходит */
    uint32_t zb[FTC3D_STRIP / 2];  /* z-буфер полосы: znear/z · 2^24 */
    uint16_t owner[FTC3D_STRIP / 2]; /* original primitive order for exact depth ties */
    uint16_t order[FTC3D_MAX_TRIS], sort_tmp[FTC3D_MAX_TRIS];
    uint32_t sort_key[FTC3D_MAX_TRIS];
} ftc3d_t;

typedef struct ftc {
    ftc_bus_t bus;

    /* кольцо */
    uint32_t ring[1024];       /* копия RAM_CMD */
    uint32_t rp;               /* REG_CMD_READ сопроцессора (слова) */
    uint32_t roff;             /* байтов прочитано в слове rp (данные CMD_INFLATE, MEMWRITE) */
    uint32_t wp;               /* REG_CMD_WRITE последнего опроса */
    uint32_t fetched;          /* кольцо скопировано до этого смещения */
    uint32_t rp_pub;           /* REG_CMD_READ, записанный в ПЛИС */
    int      held;             /* REG_CPURESET держит сопроцессор */
    uint32_t resets;           /* счётчик сбросов, с которым работаем */
    int      fault;
    int      empty_raised;     /* INT_CMDEMPTY уже поднят для пустого кольца */

    /* команда в работе */
    uint32_t cmd;              /* 0 — нет */
    uint32_t cmd_at;           /* её смещение в кольце (результаты — в слова параметров) */
    uint32_t p[13];
    uint32_t done, total, crc;
    uint32_t t0;               /* CMD_INTERRUPT: время начала */

    /* дисплей-лист */
    uint32_t dl;               /* REG_CMD_DL */
    uint32_t dl_pub;           /* записанный в ПЛИС */
    int      dl_full;
    uint32_t dlq[FTC_DLQ_WORDS];
    uint32_t dlq_n, dlq_at;

    /* запись байтов в память */
    uint32_t mw_addr;          /* адрес следующего байта */
    uint32_t mw_word, mw_mask; /* собираемое слово и его байты */
    uint32_t mw_bufs[2][FTC_MW_WORDS]; /* с прошивки 32 два: один уходит DMA (wr_async), другой копится */
    uint32_t *mw_buf;          /* копящийся */
    uint32_t mw_n, mw_at;

    /* состояние сопроцессора */
    int32_t  matrix[6];        /* преобразование битмапа, 16.16 */
    uint32_t bg, fg, grad, scratch, base, rotate;
    uint32_t inflate_end;      /* CMD_GETPTR */
    uint32_t img_addr, img_w, img_h;   /* CMD_GETPROPS */

    /* текст: строка CMD_TEXT из кольца, шрифты 0…15 (CMD_SETFONT); ПЗУ-шрифты 16…31 — по
       таблице ПЗУ (ROM_FONTROOT): образ шрифтов в SDRAM Tang заливает ESP, до того CMD_TEXT
       ждёт его до FTC_ROM_WAIT_MS от сброса сопроцессора */
    uint8_t  txt[FTC_TEXT_MAX];
    uint32_t txt_n;
    uint32_t font_ptr[16];
    uint32_t rom_t0;
    uint32_t txt_m[37];        /* метрики шрифта команды (text_ready → text_emit) */
    uint32_t txt_t0;           /* с какого мс команда ждёт годных метрик */
    int      txt_wait;
    /* метрики ПЗУ-шрифтов 16…31 — проверенные, до сброса сопроцессора (ПЗУ, пока ядро работает, не
       меняется): чтение ПЛИС под нагрузкой бывает нулями, а нулевые ширины кладут знаки в одну точку */
    uint32_t rom_met[16][37];
    uint32_t rom_ok;           /* биты шрифтов 16…31 с метриками в rom_met */

    uint32_t crc_table[256];
    int      scratch_dirty;    /* общую рабочую память занимали картинка, inflate или обновление */

    /* медиа-FIFO и видео (CMD_MEDIAFIFO, CMD_VIDEOSTART, CMD_VIDEOFRAME; ft_copro.c) */
    uint32_t mf_base, mf_size; /* кольцо в RAM_G; size 0 — CMD_MEDIAFIFO не было */
    uint32_t mf_pos;           /* позиция потока: байтов взято с CMD_MEDIAFIFO */
    uint32_t mf_read_pub;      /* REG_MEDIAFIFO_READ, записанный в ПЛИС */
    uint32_t mf_bi, mf_bn;     /* mf_buf[mf_bi…mf_bn) — байты потока с mf_pos */
    uint8_t  mf_buf[FTC_MF_BUF];
    uint32_t avi_end;          /* длина файла из заголовка RIFF + 8 (CMD_VIDEOSTART) */
    uint32_t vf_state;         /* разбор AVI: что ждём */
    uint32_t vf_left;          /* байтов до конца чанка (с выравниванием) */
    uint32_t vf_jpeg;          /* байтов JPEG кадра, ещё не отданных декодеру */

    ftc3d_t  g3d;
} ftc_t;

void ftc_init(ftc_t *c, const ftc_bus_t *bus);

/* Один опрос: состояние ПЛИС, кольцо, исполнение всего, что можно. Возвращает 0, если
   сопроцессору делать нечего (кольцо пусто, команд в работе нет, ошибки нет), иначе 1. */
int ftc_poll(ftc_t *c);

/* --- ft_inflate.c ------------------------------------------------------------------------ */
enum { FTC_INF_NEED_INPUT = 0, FTC_INF_DONE = 1, FTC_INF_ERROR = 2 };
void ftc_inflate_begin(void *st);
/* Распаковать из in[0..n): *used — сколько взято; выход отдаётся кусками через out(). */
int ftc_inflate_run(void *st, const uint8_t *in, size_t n, size_t *used,
                    void (*out)(void *u, const uint8_t *p, size_t n), void *u);
size_t ftc_inflate_state_size(void);

/* --- ft_image.c: CMD_LOADIMAGE (JPEG baseline, PNG 8 бит) -------------------------------- */
#define FTC_IMG_BYTES FTC_IMG_BYTES_DEF
enum { FTC_IMG_NEED_INPUT = 0, FTC_IMG_DONE = 1, FTC_IMG_ERROR = 2 };
typedef struct ftc_img_info {
    uint32_t fmt, w, h, stride;    /* формат BITMAP_LAYOUT, размер, шаг строки */
    uint32_t pal_bytes;            /* палитра с ptr (PALETTED565/4444), точки — за ней */
    int      jpeg;                 /* CMD_GETPROPS: у JPEG — начало, у PNG — конец данных */
} ftc_img_info_t;
size_t ftc_img_state_size(void);
/* opts — параметр CMD_LOADIMAGE (OPT_MONO 1, OPT_NODL 2, OPT_FULLSCREEN 8 — VDAC4: вписать в
   1024×768); inflate_state — для PNG (у JPEG — память вписывания) */
void ftc_img_begin(void *st, uint32_t opts, void *inflate_state);
/* Данные из in[0..n): *used — сколько взято; выход — байты подряд с ptr через emit(). */
int  ftc_img_run(void *st, const uint8_t *in, size_t n, size_t *used,
                 void (*emit)(void *u, const uint8_t *p, size_t n), void *u);
void ftc_img_get_info(const void *st, ftc_img_info_t *info);

/* --- ft_3d.c ------------------------------------------------------------------------------- */
void ftc3d_reset(ftc3d_t *g);
void ftc3d_scratch_lost(ftc3d_t *g);       /* рабочая память занята другим: кэш и кадр — с нуля */
/* Рабочая память 3D — общая (см. ftc3d_t.tris) */
#define FTC_SCRATCH(c)    ((void *)(c)->g3d.tris)
#define FTC_SCRATCH_BYTES (sizeof(ftc3d_t) - offsetof(ftc3d_t, tris))
#define FTC_INFLATE_MEM(c) ((void *)((uint8_t *)FTC_SCRATCH(c) + FTC_IMG_BYTES))
int  ftc3d_params(uint32_t code);          /* слов параметров; -1 — не команда 3D */
int  ftc3d_exec(ftc_t *c);                 /* 1 — выполнена, 0 — ошибка (FTC_FAULT_3D) */
void ftc_result(ftc_t *c, uint32_t i, uint32_t v);   /* выходной параметр команды (ft_copro.c) */

#ifdef __cplusplus
}
#endif
#endif
