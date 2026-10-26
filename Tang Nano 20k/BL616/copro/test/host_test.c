/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Проверка сопроцессора на ПК против bt8xxemu. Память FT812 — массив, хост — как Z80 с TSLib:
 * пишет поток команд в кольцо, сколько есть места (REG_CMDB_SPACE), и ждёт. RAM_DL — два
 * банка, как в FT812 и ft_core: запись и чтение — в тот, что не показывается. Смена листа
 * выполняется через несколько опросов (как кадр), чтобы CMD_DLSTART её ждал.
 *
 *   1. Weather 2: поток test/data/weather2_ring.bin → RAM_DL против weather2_dl.bin, RAM_G
 *      против ramg.bin заставки, REG_CMD_READ/WRITE/DL и INT_CMDEMPTY против weather2_regs.txt.
 *   2. CMD_GRADIENT: слова листа против замеров bt8xxemu (test/data/gradient.txt).
 *   3. Кольцо по кусочкам: тот же поток кусками по 4…60 байт.
 *   4. 3D (ft_3d.c, сборка с FTC3D_PROBE): заливка без щелей и наложений, кадр при разных полосах
 *      и с отбрасыванием нелицевых граней, глубина, отсечение, форматы, ошибки. FTC3D_PPM=файл —
 *      кадр сцены в PPM.
 *
 *   host_test DATA_DIR RAMG_BIN
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../ft_copro.h"

static uint8_t mem[0x400000];
static uint8_t dl_front[8192];   /* показываемый банк RAM_DL; задний — в mem */
static int swap_delay;            /* опросов до выполнения смены листа */
static uint32_t resets;           /* счётчик сбросов сопроцессора (как у ft_core) */
static int swap_left;
static unsigned long long bus_words;

static uint32_t peek32(uint32_t a)
{
    return (uint32_t)mem[a] | ((uint32_t)mem[a + 1] << 8) | ((uint32_t)mem[a + 2] << 16) |
           ((uint32_t)mem[a + 3] << 24);
}

static void poke32(uint32_t a, uint32_t v)
{
    mem[a] = (uint8_t)v;
    mem[a + 1] = (uint8_t)(v >> 8);
    mem[a + 2] = (uint8_t)(v >> 16);
    mem[a + 3] = (uint8_t)(v >> 24);
}

/* Чтение ПЛИС, на которое она не ответила (fpga_link: rd_chunk), — нули: столько следующих
   чтений ПЗУ (с 0x200000) отдают нули. Часы стенда — по мс на вызов (b_ms). */
static uint32_t rd_zero_rom;
static uint32_t fake_now;
static uint32_t b_ms(void *ctx)
{
    (void)ctx;
    return fake_now++;
}

static void b_rd(void *ctx, uint32_t a, uint32_t *w, uint32_t n)
{
    (void)ctx;
    if (a & 3u) {
        fprintf(stderr, "чтение с невыровненного адреса %06x\n", a);
        exit(2);
    }
    if (rd_zero_rom && a >= 0x200000u && a < 0x300000u) {
        rd_zero_rom--;
        memset(w, 0, 4 * n);
        bus_words += n;
        return;
    }
    for (uint32_t i = 0; i < n; i++)
        w[i] = peek32((a + 4 * i) & 0x3FFFFFu);
    bus_words += n;
}

static void b_wr(void *ctx, uint32_t a, const uint32_t *w, uint32_t n)
{
    (void)ctx;
    if (a & 3u) {
        fprintf(stderr, "запись по невыровненному адресу %06x\n", a);
        exit(2);
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ad = (a + 4 * i) & 0x3FFFFFu;
        if (ad == FTC_REG_DLSWAP) {
            poke32(ad, w[i] & 3u);
            swap_left = swap_delay;
        } else {
            poke32(ad, w[i]);
        }
    }
    bus_words += n;
}

static void b_status(void *ctx, ftc_status_t *st)
{
    (void)ctx;
    st->cmd_write = peek32(FTC_REG_CMD_WRITE);
    st->cmd_read = peek32(FTC_REG_CMD_READ);
    st->cmd_dl = peek32(FTC_REG_CMD_DL);
    st->dlswap = peek32(FTC_REG_DLSWAP);
    st->cpureset = peek32(FTC_REG_CPURESET);
    st->resets = resets;
}

static void b_irq(void *ctx, uint32_t f)
{
    (void)ctx;
    poke32(FTC_REG_INT_FLAGS, peek32(FTC_REG_INT_FLAGS) | f);
}

static void frame_tick(void)
{
    if (peek32(FTC_REG_DLSWAP) && --swap_left <= 0) {
        uint8_t t[8192];
        memcpy(t, dl_front, sizeof t);
        memcpy(dl_front, mem + FTC_RAM_DL, sizeof t);
        memcpy(mem + FTC_RAM_DL, t, sizeof t);
        poke32(FTC_REG_DLSWAP, 0);
    }
}

static ftc_t cop;

/* Запись без ожидания, отложенная до wr_wait (как DMA на BL616): буфер, который 3D тронет
   раньше wr_wait, испортит кадр — проверка t3_scene это увидит. */
static int async_bus;
static struct {
    uint32_t a, n;
    const uint32_t *w;
} pend;

static void b_wr_wait(void *ctx)
{
    if (pend.n) {
        uint32_t n = pend.n;
        pend.n = 0;
        b_wr(ctx, pend.a, pend.w, n);
    }
}

static void b_wr_async(void *ctx, uint32_t a, const uint32_t *w, uint32_t n)
{
    b_wr_wait(ctx);
    pend.a = a;
    pend.w = w;
    pend.n = n;
}

static void b_pump(void *ctx)
{
    (void)ctx;
}

static void chip_reset(void)
{
    memset(mem, 0, sizeof mem);
    memset(dl_front, 0, sizeof dl_front);
    ftc_bus_t bus = {NULL, b_rd, b_wr, b_status, b_irq, NULL, NULL, NULL, NULL};
    if (async_bus) {
        bus.wr_async = b_wr_async;
        bus.pump = b_pump;
        bus.wr_wait = b_wr_wait;
    }
    pend.n = 0;
    ftc_init(&cop, &bus);
}

/* Поток в кольцо кусками не больше chunk байт (0 — сколько влезет); 0 — всё выполнено. */
static int feed(const uint8_t *s, size_t n, size_t chunk)
{
    size_t pos = 0;
    long guard = 0;
    for (;;) {
        uint32_t wp = peek32(FTC_REG_CMD_WRITE), rp = peek32(FTC_REG_CMD_READ);
        if (rp == 0xFFF) {
            fprintf(stderr, "сопроцессор: ошибка %d (команда %08x)\n", cop.fault, cop.cmd);
            return 1;
        }
        uint32_t space = (4092 - ((wp - rp) & 0xFFF)) & 0xFFC;
        size_t k = n - pos < space ? n - pos : space;
        if (chunk && k > chunk)
            k = chunk & ~(size_t)3;
        for (size_t i = 0; i < k; i++)
            mem[FTC_RAM_CMD + ((wp + i) & 0xFFF)] = s[pos + i];
        pos += k;
        poke32(FTC_REG_CMD_WRITE, (wp + (uint32_t)k) & 0xFFF);
        int busy = ftc_poll(&cop);
        frame_tick();
        if (pos == n && !busy && peek32(FTC_REG_CMD_READ) == peek32(FTC_REG_CMD_WRITE))
            return 0;
        if (++guard > 10000000) {
            fprintf(stderr, "сопроцессор не закончил\n");
            return 1;
        }
    }
}

static uint8_t *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "нет файла %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n + 1);
    if (fread(b, 1, *n, f) != *n)
        exit(2);
    fclose(f);
    b[*n] = 0;
    return b;
}

static int weather2(const char *dir, const char *ramg_path, size_t chunk)
{
    char p[1024];
    size_t ring_n, dl_n, ramg_n, regs_n;
    snprintf(p, sizeof p, "%s/weather2_ring.bin", dir);
    uint8_t *ring = load(p, &ring_n);
    snprintf(p, sizeof p, "%s/weather2_dl.bin", dir);
    uint8_t *dl = load(p, &dl_n);
    snprintf(p, sizeof p, "%s/weather2_regs.txt", dir);
    char *regs = (char *)load(p, &regs_n);
    uint8_t *ramg = load(ramg_path, &ramg_n);
    unsigned g_rd, g_wr, g_dl, g_int;
    if (sscanf(regs, "%u %u %u %u", &g_rd, &g_wr, &g_dl, &g_int) != 4)
        return 1;

    chip_reset();
    swap_delay = 3;
    bus_words = 0;
    int err = feed(ring, ring_n, chunk);
    for (int i = 0; i < 10; i++)               /* последняя смена листа — кадром позже */
        frame_tick();
    size_t dl_bad = 0, g_bad = 0, first_dl = (size_t)-1, first_g = (size_t)-1;
    for (size_t i = 0; i < dl_n; i++)
        if (mem[FTC_RAM_DL + i] != dl[i] && dl_bad++ == 0)
            first_dl = i;
    for (size_t i = 0; i < ramg_n; i++)
        if (mem[i] != ramg[i] && g_bad++ == 0)
            first_g = i;
    uint32_t rd = peek32(FTC_REG_CMD_READ), wr = peek32(FTC_REG_CMD_WRITE), cdl = peek32(FTC_REG_CMD_DL);
    uint32_t intf = peek32(FTC_REG_INT_FLAGS);
    printf("Weather 2 (куски %zu): RAM_DL расхождений %zu из %zu, RAM_G %zu из %zu; "
           "CMD_READ %u/%u CMD_WRITE %u/%u CMD_DL %u/%u INT_CMDEMPTY %u; слов по шине %llu\n",
           chunk, dl_bad, dl_n, g_bad, ramg_n, rd, g_rd, wr, g_wr, cdl, g_dl, (intf >> 5) & 1, bus_words);
    if (dl_bad)
        printf("  первое расхождение RAM_DL: слово %zu: %08x, bt8xxemu %08x\n", first_dl / 4,
               peek32(FTC_RAM_DL + (uint32_t)(first_dl & ~(size_t)3)),
               (uint32_t)dl[first_dl & ~3u] | ((uint32_t)dl[(first_dl & ~3u) + 1] << 8) |
                   ((uint32_t)dl[(first_dl & ~3u) + 2] << 16) | ((uint32_t)dl[(first_dl & ~3u) + 3] << 24));
    if (g_bad)
        printf("  первое расхождение RAM_G: %06zx\n", first_g);
    free(ring);
    free(dl);
    free(regs);
    free(ramg);
    return err || dl_bad || g_bad || rd != g_rd || wr != g_wr || cdl != g_dl || ((intf ^ g_int) & 0x20);
}

static int gradients(const char *dir)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/gradient.txt", dir);
    FILE *f = fopen(p, "r");
    if (!f) {
        fprintf(stderr, "нет файла %s\n", p);
        return 1;
    }
    int cases = 0, bad = 0, bad_c = 0;
    long x0, y0, x1, y1;
    unsigned long rgb0, rgb1;
    unsigned n;
    while (fscanf(f, "%ld %ld %lu %ld %ld %lu %u", &x0, &y0, &rgb0, &x1, &y1, &rgb1, &n) == 7) {
        uint32_t want[64];
        for (unsigned i = 0; i < n; i++)
            if (fscanf(f, "%x", &want[i]) != 1)
                return 1;
        chip_reset();
        swap_delay = 0;
        uint32_t s[6] = {0xFFFFFF00u, 0xFFFFFF0Bu, ((uint32_t)x0 & 0xFFFF) | ((uint32_t)y0 << 16), (uint32_t)rgb0,
                         ((uint32_t)x1 & 0xFFFF) | ((uint32_t)y1 << 16), (uint32_t)rgb1};
        uint8_t sb[24];
        for (int i = 0; i < 6; i++)
            for (int k = 0; k < 4; k++)
                sb[4 * i + k] = (uint8_t)(s[i] >> (8 * k));
        if (feed(sb, sizeof sb, 0))
            return 1;
        cases++;
        int ok = 1, only_c = 1;
        for (unsigned i = 0; i < n; i++)
            if (peek32(FTC_RAM_DL + 4 * i) != want[i]) {
                ok = 0;
                if (i != 5)
                    only_c = 0;
            }
        if (n > 7 && peek32(FTC_REG_CMD_DL) != 4 * n)
            ok = only_c = 0;
        if (!ok) {
            bad++;
            bad_c += only_c;
            if (!only_c && bad - bad_c <= 5)
                printf("  градиент (%ld,%ld)-(%ld,%ld): слова не те\n", x0, y0, x1, y1);
        }
    }
    fclose(f);
    printf("CMD_GRADIENT: случаев %d, не совпало %d (из них только C: %d)\n", cases, bad, bad_c);
    return bad != bad_c;           /* C — известное расхождение (ft_copro.c, gradient) */
}

/* Ошибка и восстановление, как в PG §5.6: неизвестная команда → REG_CMD_READ 0xFFF; хост держит
   REG_CPURESET, обнуляет указатели, отпускает — сопроцессор работает с начала кольца.
   Второй раз — сброса сопроцессор не видит (только счётчик): так бывает, если BL616 не успел
   опросить ПЛИС, пока сброс держался. */
static int recovery(void)
{
    chip_reset();
    swap_delay = 0;
    const uint8_t bad[] = {0x55, 0xFF, 0xFF, 0xFF};
    int ok = 1;
    for (int pass = 0; pass < 2; pass++) {
        if (!feed(bad, sizeof bad, 0) || peek32(FTC_REG_CMD_READ) != 0xFFF)
            ok = 0;
        if (pass == 0) {
            poke32(FTC_REG_CPURESET, 1);
            ftc_poll(&cop);
        }
        poke32(FTC_REG_CMD_READ, 0);
        poke32(FTC_REG_CMD_WRITE, 0);
        poke32(FTC_REG_CPURESET, 0);
        resets++;
        /* CMD_DLSTART, слово листа, CMD_MEMSET(0x100, 0x5A, 8), CMD_GETPTR */
        const uint32_t s[] = {0xFFFFFF00u, 0x02123456u, 0xFFFFFF1Bu, 0x100, 0x5A, 8};
        uint8_t sb[sizeof s];
        memcpy(sb, s, sizeof s);
        if (feed(sb, sizeof sb, 0))
            ok = 0;
        if (peek32(FTC_RAM_DL) != 0x02123456u || peek32(FTC_REG_CMD_DL) != 4 || peek32(0x100) != 0x5A5A5A5Au ||
            peek32(0x104) != 0x5A5A5A5Au || peek32(0x108) != 0)
            ok = 0;
        memset(mem + FTC_RAM_DL, 0, 8192);
        memset(mem + 0x100, 0, 16);
    }
    printf("ошибка и восстановление через REG_CPURESET: %s\n", ok ? "да" : "НЕТ");
    return !ok;
}

/* --- 3D (ft_3d.c) ------------------------------------------------------------------------------
   Сборка с FTC3D_PROBE: ft_3d.c сообщает каждую закрашиваемую точку (до проверки глубины) —
   так видно, сколько треугольников накрыли точку, — и даёт задать высоту полосы. */
int ftc3d_strip_lines;
static uint8_t cover[1024][1024];
static int probe_on;

void ftc3d_probe(int x, int y)
{
    if (probe_on && x >= 0 && x < 1024 && y >= 0 && y < 1024 && cover[y][x] < 255)
        cover[y][x]++;
}

#define T3_TARGET 0x000000u
#define T3_VERTS  0x100000u
#define T3_FACES  0x110000u
#define T3_VERTS2 0x120000u
#define T3_FACES2 0x130000u

static uint32_t c3[8192];
static size_t n3;

static void C(uint32_t w)
{
    c3[n3++] = w;
}

static int run3(void)
{
    int err = feed((const uint8_t *)c3, n3 * 4, 0);
    n3 = 0;
    return err;
}

static uint32_t fix16(double v)
{
    return (uint32_t)(int32_t)(v * 65536.0 + (v < 0 ? -0.5 : 0.5));
}

typedef struct {
    int16_t v[FTC3D_MAX_VERTS][3];
    uint16_t f[FTC3D_MAX_FACES][4];
    int nv, nf;
} mesh_t;

/* грани — наружу: стандартное (b − a) × (c − a) смотрит внутрь (система левая, см. ft_3d.c) */
static void face(mesh_t *m, int a, int b, int c, uint16_t color, double ox, double oy, double oz)
{
    double ax = m->v[a][0], ay = m->v[a][1], az = m->v[a][2];
    double ux = m->v[b][0] - ax, uy = m->v[b][1] - ay, uz = m->v[b][2] - az;
    double wx = m->v[c][0] - ax, wy = m->v[c][1] - ay, wz = m->v[c][2] - az;
    double nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
    if (nx * ox + ny * oy + nz * oz > 0) {
        int t = b;
        b = c;
        c = t;
    }
    uint16_t *f = m->f[m->nf++];
    f[0] = (uint16_t)a;
    f[1] = (uint16_t)b;
    f[2] = (uint16_t)c;
    f[3] = color;
}

static void torus(mesh_t *m, double R, double r, int nu, int nv, uint16_t color)
{
    const double pi = 3.14159265358979;
    m->nv = m->nf = 0;
    for (int i = 0; i < nu; i++)
        for (int j = 0; j < nv; j++) {
            double u = 2 * pi * i / nu, v = 2 * pi * j / nv;
            m->v[m->nv][0] = (int16_t)lround((R + r * cos(v)) * cos(u));
            m->v[m->nv][1] = (int16_t)lround(r * sin(v));
            m->v[m->nv][2] = (int16_t)lround((R + r * cos(v)) * sin(u));
            m->nv++;
        }
    for (int i = 0; i < nu; i++)
        for (int j = 0; j < nv; j++) {
            int a = i * nv + j, b = ((i + 1) % nu) * nv + j, c = ((i + 1) % nu) * nv + (j + 1) % nv,
                d = i * nv + (j + 1) % nv;
            double u = 2 * pi * (i + 0.5) / nu, v = 2 * pi * (j + 0.5) / nv;
            double ox = cos(v) * cos(u), oy = sin(v), oz = cos(v) * sin(u);
            face(m, a, b, c, color, ox, oy, oz);
            face(m, a, c, d, color, ox, oy, oz);
        }
}

static void cube(mesh_t *m, int s)
{
    static const uint16_t col[6] = {0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F, 0x07FF};
    m->nv = m->nf = 0;
    for (int i = 0; i < 8; i++) {
        m->v[i][0] = (int16_t)((i & 1) ? s : -s);
        m->v[i][1] = (int16_t)((i & 2) ? s : -s);
        m->v[i][2] = (int16_t)((i & 4) ? s : -s);
    }
    m->nv = 8;
    static const int q[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
    static const int o[6][3] = {{0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {0, 1, 0}, {-1, 0, 0}, {1, 0, 0}};
    for (int k = 0; k < 6; k++) {
        face(m, q[k][0], q[k][1], q[k][2], col[k], o[k][0], o[k][1], o[k][2]);
        face(m, q[k][0], q[k][2], q[k][3], col[k], o[k][0], o[k][1], o[k][2]);
    }
}

static void put_mesh(const mesh_t *m, uint32_t va, uint32_t fa)
{
    for (int i = 0; i < m->nv; i++) {
        poke32(va + 8 * (uint32_t)i, (uint16_t)m->v[i][0] | ((uint32_t)(uint16_t)m->v[i][1] << 16));
        poke32(va + 8 * (uint32_t)i + 4, (uint16_t)m->v[i][2]);
    }
    for (int i = 0; i < m->nf; i++) {
        poke32(fa + 8 * (uint32_t)i, m->f[i][0] | ((uint32_t)m->f[i][1] << 16));
        poke32(fa + 8 * (uint32_t)i + 4, m->f[i][2] | ((uint32_t)m->f[i][3] << 16));
    }
}

static void mesh_cmd(const mesh_t *m, uint32_t va, uint32_t fa, uint32_t flags)
{
    C(0xFFFFFFAAu);
    C(va);
    C(fa);
    C((uint32_t)m->nv | ((uint32_t)m->nf << 16));
    C(flags);
}

static void begin3(uint32_t fmt, uint32_t w, uint32_t h, double focal, double znear, uint32_t clear)
{
    C(0xFFFFFFA0u); C(T3_TARGET); C(fmt); C(w | (h << 16));
    C(0xFFFFFFA1u); C(fix16(focal)); C(fix16(znear));
    C(0xFFFFFFA9u); C(clear);
    C(0xFFFFFFA2u);
}

static uint32_t rng = 12345;
static int rnd(int n)
{
    rng = rng * 1103515245u + 12345u;
    return (int)((rng >> 8) % (uint32_t)n);
}

/* Сетка 8×8 клеток в плоскости перед камерой, внутренние вершины сдвинуты на ±1¾ точки,
   диагонали клеток — наугад: каждая точка внутри квадрата — ровно один треугольник. */
static int t3_watertight(void)
{
    static mesh_t m;
    int bad = 0, cases = 40;
    for (int k = 0; k < cases; k++) {
        chip_reset();
        m.nv = m.nf = 0;
        for (int j = 0; j <= 8; j++)
            for (int i = 0; i <= 8; i++) {
                int x = -320 + 80 * i, y = -240 + 60 * j;
                if (i > 0 && i < 8 && j > 0 && j < 8) {
                    x += rnd(15) - 7;
                    y += rnd(15) - 7;
                }
                m.v[m.nv][0] = (int16_t)x;
                m.v[m.nv][1] = (int16_t)y;
                m.v[m.nv][2] = 1024;
                m.nv++;
            }
        for (int j = 0; j < 8; j++)
            for (int i = 0; i < 8; i++) {
                int a = j * 9 + i, b = a + 1, c = a + 10, d = a + 9;
                uint16_t col = (uint16_t)(rnd(65535) | 1);
                if (rnd(2)) {
                    face(&m, a, b, c, col, 0, 0, -1);
                    face(&m, a, c, d, col, 0, 0, -1);
                } else {
                    face(&m, a, b, d, col, 0, 0, -1);
                    face(&m, b, c, d, col, 0, 0, -1);
                }
            }
        put_mesh(&m, T3_VERTS, T3_FACES);
        memset(cover, 0, sizeof cover);
        probe_on = 1;
        begin3(7, 256, 192, 256, 1, 0);
        C(0xFFFFFFA5u); C(fix16(0.25)); C(fix16(0.25)); C(fix16(0.25));
        mesh_cmd(&m, T3_VERTS, T3_FACES, k & 1);         /* через раз — с отбрасыванием */
        C(0xFFFFFFABu);
        if (run3())
            return 1;
        probe_on = 0;
        /* квадрат на экране: x 48…208, y 36…156 — центры точек 48…207 и 36…155 */
        for (int y = 0; y < 192; y++)
            for (int x = 0; x < 256; x++) {
                int in = x >= 48 && x <= 207 && y >= 36 && y <= 155;
                if (cover[y][x] != in) {
                    if (bad < 5)
                        printf("  сетка %d: точка (%d,%d) закрашена %d раз\n", k, x, y, cover[y][x]);
                    bad++;
                }
            }
    }
    printf("3D, сетка без щелей и наложений: %d сеток, неверных точек %d\n", cases, bad);
    return bad != 0;
}

/* сцена: гладкий тор и куб со светом по граням; scene_flags — добавка к флагам сеток */
static uint32_t scene_flags;

static void scene(uint32_t fmt, uint32_t cull)
{
    begin3(fmt, 256, 192, 256, 4, 0x00203040u);          /* фон прозрачный (ARGB1555, ARGB2) */
    C(0xFFFFFFA6u);
    C(0xFFFFFFA3u); C(fix16(-20)); C(fix16(0)); C(fix16(300));
    C(0xFFFFFFA4u); C(0x2000); C(0x1400); C(0x0800);
    C(0xFFFFFFAAu); C(T3_VERTS); C(T3_FACES); C((uint32_t)(24 * 12) | ((uint32_t)(24 * 12 * 2) << 16)); C(4 | cull | scene_flags);
    C(0xFFFFFFA7u);
    C(0xFFFFFFA3u); C(fix16(70)); C(fix16(30)); C(fix16(260));
    C(0xFFFFFFA4u); C(0x1000); C(0x2600); C(0);
    C(0xFFFFFFAAu); C(T3_VERTS2); C(T3_FACES2); C(8 | (12u << 16)); C(2 | cull | scene_flags);
    C(0xFFFFFFABu);
}

static void put_scene(void)
{
    static mesh_t t, k;
    torus(&t, 60, 25, 24, 12, 0xFC60);
    cube(&k, 22);
    put_mesh(&t, T3_VERTS, T3_FACES);
    put_mesh(&k, T3_VERTS2, T3_FACES2);
}

/* CMD_3D_INFO: слово i (0 — версия, 1 — прошлый кадр) */
static uint32_t info3(int i)
{
    uint32_t at = peek32(FTC_REG_CMD_WRITE);
    C(0xFFFFFFACu); C(0); C(0);
    if (run3())
        return 0xFFFFFFFFu;
    return peek32(FTC_RAM_CMD + ((at + 4 + 4 * (uint32_t)i) & 0xFFF));
}

static void save_ppm(const char *path, uint32_t w, uint32_t h)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t i = 0; i < w * h; i++) {
        uint32_t c = (uint32_t)mem[T3_TARGET + 2 * i] | ((uint32_t)mem[T3_TARGET + 2 * i + 1] << 8);
        uint8_t rgb[3] = {(uint8_t)(((c >> 11) << 3) | (c >> 13)), (uint8_t)((((c >> 5) & 63) << 2) | ((c >> 9) & 3)),
                          (uint8_t)(((c & 31) << 3) | ((c >> 2) & 7))};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* Полосы, отбрасывание, форматы: один и тот же кадр. */
static int t3_scene(void)
{
    static uint8_t ref[256 * 192 * 2];
    int err = 0;
    const uint32_t fmts[] = {7, 0, 4, 5, 3};
    for (size_t fi = 0; fi < sizeof fmts / sizeof fmts[0]; fi++) {
        uint32_t fmt = fmts[fi], bytes = (fmt == 7 || fmt == 0) ? 2 : 1, size = 256 * 192 * bytes;
        const int strips[] = {0, 1, 7, 13, 0};       /* 0 — как в прошивке (32 строки); последний —
                                                        запись без ожидания */
        for (size_t si = 0; si < 5; si++)
            for (uint32_t cull = 0; cull <= 1; cull++) {
                async_bus = si == 4;
                chip_reset();
                put_scene();
                ftc3d_strip_lines = strips[si];
                bus_words = 0;
                scene(fmt, cull);
                if (run3())
                    return 1;
                unsigned long long words = bus_words;
                uint32_t st = info3(1);
                if (si == 0 && cull == 0) {
                    memcpy(ref, mem + T3_TARGET, size);
                    printf("3D, сцена (формат %u): треугольников %u, слов по шине %llu (~%.1f мс SPI 26,7 МГц)\n",
                           fmt, st & 0xFFFF, words, words * 4 * 8 / 26.67e6 * 1e3);
                    if (fmt == 7 && getenv("FTC3D_PPM"))
                        save_ppm(getenv("FTC3D_PPM"), 256, 192);
                } else {
                    uint32_t diff = 0;
                    for (uint32_t i = 0; i < size; i += bytes)
                        diff += memcmp(ref + i, mem + T3_TARGET + i, bytes) != 0;
                    if (diff) {
                        printf("  формат %u, полоса %d, отбрасывание %u: другие точки %u\n", fmt, strips[si], cull, diff);
                        err = 1;
                    }
                }
                if (cull && si == 0)
                    printf("  с отбрасыванием нелицевых: треугольников %u\n", st & 0xFFFF);
            }
        if (fmt == 0) {                          /* ARGB1555: фон прозрачный только там, где пусто */
            int opaque = 0;
            for (uint32_t i = 0; i < 256 * 192; i++)
                opaque += (ref[2 * i + 1] >> 7) & 1;
            if (opaque == 0 || opaque == 256 * 192) {
                printf("  ARGB1555: альфа не та (непрозрачных %d)\n", opaque);
                err = 1;
            }
        }
    }
    ftc3d_strip_lines = 0;
    async_bus = 0;
    printf("3D, кадр не зависит от полос, отбрасывания и записи без ожидания: %s\n", err ? "НЕТ" : "да");
    return err;
}

/* Кэш неизменных сеток (флаг 8): второй кадр не читает сетки, кадр тот же; сетка сменилась —
   с флагом кадр прежний (копия), без флага — новый, и копия выброшена. */
static int t3_cache(void)
{
    static uint8_t a[256 * 192 * 2], b[256 * 192 * 2];
    const uint32_t size = sizeof a;
    int err = 0;
    chip_reset();
    put_scene();
    scene_flags = 8;
    bus_words = 0;
    scene(7, 1);
    err |= run3();
    unsigned long long w1 = bus_words;
    memcpy(a, mem + T3_TARGET, size);
    bus_words = 0;
    scene(7, 1);
    err |= run3();
    unsigned long long w2 = bus_words;
    if (memcmp(a, mem + T3_TARGET, size))
        err = 1;
    /* тор шире: вершины ×1.25 */
    for (int i = 0; i < 24 * 12; i++) {
        uint32_t v0 = peek32(T3_VERTS + 8 * (uint32_t)i), v1 = peek32(T3_VERTS + 8 * (uint32_t)i + 4);
        int16_t x = (int16_t)v0, y = (int16_t)(v0 >> 16), z = (int16_t)v1;
        x = (int16_t)(x * 5 / 4);
        z = (int16_t)(z * 5 / 4);
        poke32(T3_VERTS + 8 * (uint32_t)i, (uint16_t)x | ((uint32_t)(uint16_t)y << 16));
        poke32(T3_VERTS + 8 * (uint32_t)i + 4, (uint16_t)z);
    }
    scene(7, 1);
    err |= run3();
    int kept = memcmp(a, mem + T3_TARGET, size) == 0;      /* копия: прежний тор */
    scene_flags = 0;
    scene(7, 1);
    err |= run3();
    memcpy(b, mem + T3_TARGET, size);
    int fresh = memcmp(a, b, size) != 0;                    /* без флага — новый */
    scene_flags = 8;
    scene(7, 1);
    err |= run3();
    int reread = memcmp(b, mem + T3_TARGET, size) == 0;    /* копия выброшена — тоже новый */
    scene_flags = 0;
    err |= !kept || !fresh || !reread || w1 - w2 != 1768;   /* сетки сцены: тор 576 + 576, куб 16 + 24 слова */
    printf("3D, кэш сеток: слов по шине %llu, со второго кадра %llu; смена сетки: копия %s, без флага %s, "
           "потом %s: %s\n", w1, w2, kept ? "держится" : "НЕТ", fresh ? "новая" : "НЕТ",
           reread ? "новая" : "НЕТ", err ? "НЕТ" : "да");
    return err;
}

/* Два квадрата друг за другом в обоих порядках: кадр тот же, в перекрытии — ближний. */
static int t3_depth(void)
{
    static mesh_t m;
    static uint8_t ref[64 * 64 * 2];
    int err = 0;
    for (int order = 0; order < 2; order++) {
        chip_reset();
        m.nv = m.nf = 0;
        const int z[2] = {200, 400};
        const uint16_t col[2] = {0xF800, 0x001F};
        for (int k = 0; k < 2; k++) {
            int q = order ? 1 - k : k, off = q ? 10 : -10;
            for (int i = 0; i < 4; i++) {
                m.v[m.nv][0] = (int16_t)(((i & 1) ? 30 : -30) * z[q] / 200 + off);
                m.v[m.nv][1] = (int16_t)(((i & 2) ? 30 : -30) * z[q] / 200);
                m.v[m.nv][2] = (int16_t)z[q];
                m.nv++;
            }
            int b = 4 * k;
            face(&m, b, b + 1, b + 3, col[q], 0, 0, -1);
            face(&m, b, b + 3, b + 2, col[q], 0, 0, -1);
        }
        put_mesh(&m, T3_VERTS, T3_FACES);
        begin3(7, 64, 64, 200, 1, 0);
        mesh_cmd(&m, T3_VERTS, T3_FACES, 0);
        C(0xFFFFFFABu);
        if (run3())
            return 1;
        if (order == 0)
            memcpy(ref, mem + T3_TARGET, sizeof ref);
        else if (memcmp(ref, mem + T3_TARGET, sizeof ref))
            err = 1;
    }
    /* перекрытие: x 32…41 — там ближний красный */
    uint32_t c = (uint32_t)mem[T3_TARGET + 2 * (32 * 64 + 36)] | ((uint32_t)mem[T3_TARGET + 2 * (32 * 64 + 36) + 1] << 8);
    if (c != 0xF800)
        err = 1;
    printf("3D, глубина: порядок граней не важен, ближний сверху: %s\n", err ? "НЕТ" : "да");
    return err;
}

/* Пол y = −50 от z = −500 (за камерой) до 3000, x ±2000: выше горизонта — ничего. */
static int t3_clip(void)
{
    static mesh_t m;
    chip_reset();
    m.nv = m.nf = 0;
    const int16_t pts[4][3] = {{-2000, -50, -500}, {2000, -50, -500}, {-2000, -50, 3000}, {2000, -50, 3000}};
    memcpy(m.v, pts, sizeof pts);
    m.nv = 4;
    face(&m, 0, 1, 3, 0x07E0, 0, 1, 0);
    face(&m, 0, 3, 2, 0x07E0, 0, 1, 0);
    put_mesh(&m, T3_VERTS, T3_FACES);
    memset(cover, 0, sizeof cover);
    probe_on = 1;
    begin3(7, 256, 192, 256, 1, 0);
    mesh_cmd(&m, T3_VERTS, T3_FACES, 1);
    C(0xFFFFFFABu);
    int err = run3();
    probe_on = 0;
    int above = 0, below = 0, twice = 0;
    for (int y = 0; y < 192; y++)
        for (int x = 0; x < 256; x++) {
            if (cover[y][x] > 1)
                twice++;
            if (cover[y][x] && y < 100)
                above++;
            if (cover[y][x] && y >= 100)
                below++;
        }
    /* дальний край z = 3000: y = 96 + 256·50/3000 = 100,3 — ниже строк 100… всё закрашено */
    err |= above != 0 || twice != 0 || below != 256 * 92;
    printf("3D, отсечение (пол за камерой): выше горизонта %d, дважды %d, низ %d из %d: %s\n", above, twice,
           below, 256 * 92, err ? "НЕТ" : "да");
    return err;
}

/* SSAA 2×2 (формат | 0x100). Сцена: внутри граней (у обычного кадра соседи 3×3 одного цвета) тон
   тот же ±1 шаг канала — среднее отсчётов в центре точки; края сглажены; полосы и запись без
   ожидания кадр не меняют; форматы; ARGB1555 — фон прозрачный только там, где пусто. Край
   полуплоскости: в каждой строке белое по площади слева от края ±0,35 точки, переход монотонный,
   не шире 2 точек. Цена — время счёта на ПК обычного и SSAA (канал тот же: слов по шине поровну). */
static void save_ppm_x4(const char *path, const uint8_t *rgb565, uint32_t w, uint32_t h)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%u %u\n255\n", 4 * w, 4 * h);
    for (uint32_t y = 0; y < 4 * h; y++)
        for (uint32_t x = 0; x < 4 * w; x++) {
            uint32_t i = (y / 4) * w + x / 4, c = (uint32_t)rgb565[2 * i] | ((uint32_t)rgb565[2 * i + 1] << 8);
            uint8_t rgb[3] = {(uint8_t)(((c >> 11) << 3) | (c >> 13)), (uint8_t)((((c >> 5) & 63) << 2) | ((c >> 9) & 3)),
                              (uint8_t)(((c & 31) << 3) | ((c >> 2) & 7))};
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

static int t3_aa(void)
{
    static uint8_t ref[256 * 192 * 2], aa[256 * 192 * 2];
    int err = 0;
    /* сцена RGB565: обычный кадр и SSAA */
    chip_reset();
    put_scene();
    bus_words = 0;
    scene(7, 0);
    if (run3())
        return 1;
    memcpy(ref, mem + T3_TARGET, sizeof ref);
    unsigned long long words_ref = bus_words;
    chip_reset();
    put_scene();
    bus_words = 0;
    scene(7 | 0x100, 0);
    if (run3())
        return 1;
    memcpy(aa, mem + T3_TARGET, sizeof aa);
    unsigned long long words_aa = bus_words;
    int inside = 0, inside_bad = 0, changed = 0;
    for (int y = 1; y < 191; y++)
        for (int x = 1; x < 255; x++) {
            const uint16_t *r = (const uint16_t *)ref, *a = (const uint16_t *)aa;
            uint16_t c = r[y * 256 + x];
            int same = 1;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    same &= r[(y + dy) * 256 + x + dx] == c;
            uint16_t v = a[y * 256 + x];
            changed += v != c;
            if (!same)
                continue;
            inside++;
            int dr = abs((int)(v >> 11) - (int)(c >> 11)), dg = abs((int)((v >> 5) & 63) - (int)((c >> 5) & 63)),
                db = abs((int)(v & 31) - (int)(c & 31));
            if (dr > 1 || dg > 1 || db > 1)
                inside_bad++;
        }
    if (inside_bad || changed == 0 || words_aa != words_ref) {
        printf("  SSAA: внутри граней не тот тон %d из %d, изменено точек %d, слов по шине %llu и %llu\n", inside_bad,
               inside, changed, words_ref, words_aa);
        err = 1;
    }
    const char *dir = getenv("FTC3D_AA_DIR");
    if (dir) {
        char path[512];
        snprintf(path, sizeof path, "%s/scene_x4.ppm", dir);
        save_ppm_x4(path, ref, 256, 192);
        snprintf(path, sizeof path, "%s/scene_aa_x4.ppm", dir);
        save_ppm_x4(path, aa, 256, 192);
    }
    /* полосы 1 и 3 строки, запись без ожидания — тот же кадр */
    const int strips[] = {1, 3, 0};
    for (size_t si = 0; si < 3; si++) {
        async_bus = si == 2;
        chip_reset();
        put_scene();
        ftc3d_strip_lines = strips[si];
        scene(7 | 0x100, 0);
        if (run3())
            return 1;
        if (memcmp(aa, mem + T3_TARGET, sizeof aa)) {
            printf("  SSAA: полоса %d строк%s — другой кадр\n", strips[si], async_bus ? ", без ожидания" : "");
            err = 1;
        }
    }
    ftc3d_strip_lines = 0;
    async_bus = 0;
    /* форматы: ARGB1555 и ARGB2 с прозрачным фоном, RGB332, L8 */
    const uint32_t fmts[] = {0, 4, 5, 3};
    for (size_t fi = 0; fi < 4; fi++) {
        chip_reset();
        put_scene();
        scene(fmts[fi] | 0x100, 0);
        if (run3()) {
            printf("  SSAA: формат %u — ошибка\n", fmts[fi]);
            err = 1;
            continue;
        }
        if (fmts[fi] == 0) {
            int opaque = 0, corner = 0;
            for (uint32_t i = 0; i < 256 * 192; i++)
                opaque += (mem[T3_TARGET + 2 * i + 1] >> 7) & 1;
            corner = (mem[T3_TARGET + 1] >> 7) & 1;                 /* угол цели — фон */
            if (opaque == 0 || opaque == 256 * 192 || corner) {
                printf("  SSAA ARGB1555: альфа не та (непрозрачных %d, угол %d)\n", opaque, corner);
                err = 1;
            }
        }
    }
    /* край полуплоскости: белый квадрат слева от края x = 32 + (−3,3 … 6,1) сверху вниз, фон чёрный, L8 */
    {
        mesh_t q;
        q.nv = 4;
        q.nf = 0;
        const int16_t vx[4] = {-800, -33, 61, -800}, vy[4] = {500, 500, -500, -500};
        for (int i = 0; i < 4; i++) {
            q.v[i][0] = vx[i];
            q.v[i][1] = vy[i];
            q.v[i][2] = 0;
        }
        face(&q, 0, 1, 2, 0xFFFF, 0, 0, -1);
        face(&q, 0, 2, 3, 0xFFFF, 0, 0, -1);
        for (int mode = 0; mode < 2; mode++) {
            chip_reset();
            put_mesh(&q, T3_VERTS, T3_FACES);
            begin3(3 | (mode ? 0x100u : 0u), 64, 64, 256, 4, 0xFF000000u);
            C(0xFFFFFFA3u); C(fix16(0)); C(fix16(0)); C(fix16(256));
            C(0xFFFFFFA5u); C(fix16(0.1)); C(fix16(0.1)); C(fix16(1));
            mesh_cmd(&q, T3_VERTS, T3_FACES, 0);
            C(0xFFFFFFABu);
            if (run3())
                return 1;
            double worst = 0;
            int ramp_bad = 0, wide = 0;
            for (int y = 0; y < 64; y++) {
                /* край на экране: x = 32 + xe(y'), y' = 32 − y (камера); от −3,3 при y = −18 до 6,1 при y = 82 */
                double area = 0;
                for (int k = 0; k < 64; k++) {
                    double sy = y + (k + 0.5) / 64, t = (sy - (32 - 50)) / 100;
                    double xe = 32 + (-3.3 + t * (6.1 - -3.3));
                    area += (xe < 0 ? 0 : xe > 64 ? 64 : xe) / 64;
                }
                double sum = 0;
                int mid = 0, prev = 255;
                for (int x = 0; x < 64; x++) {
                    int v = mem[T3_TARGET + (uint32_t)(y * 64 + x)];
                    sum += v / 255.0;
                    mid += v > 0 && v < 255;
                    if (v > prev)
                        ramp_bad++;
                    prev = v;
                }
                if (fabs(sum - area) > worst)
                    worst = fabs(sum - area);
                if (mid > 2)
                    wide++;
            }
            int mids = 0;
            for (uint32_t i = 0; i < 64 * 64; i++)
                mids += mem[T3_TARGET + i] > 0 && mem[T3_TARGET + i] < 255;
            printf("  край полуплоскости, %s: ошибка площади строки до %.2f точки, промежуточных точек %d\n",
                   mode ? "SSAA" : "обычный", worst, mids);
            if (mode && (worst > 0.35 || ramp_bad || wide || mids < 32)) {
                printf("  SSAA: край не тот (немонотонно %d, шире 2 точек %d строк)\n", ramp_bad, wide);
                err = 1;
            }
        }
    }
    /* цена счёта на ПК: сцена RGB565, обычная и SSAA */
    double t_ms[2];
    for (int mode = 0; mode < 2; mode++) {
        const int N = 30;
        clock_t c0 = clock();
        for (int i = 0; i < N; i++) {
            chip_reset();
            put_scene();
            scene(7 | (mode ? 0x100u : 0u), 0);
            if (run3())
                return 1;
        }
        t_ms[mode] = (double)(clock() - c0) * 1000.0 / CLOCKS_PER_SEC / N;
    }
    printf("3D, SSAA 2x2: изменено точек %d, внутри граней тон тот же (%d точек), слов по шине %llu (как без "
           "него); счёт кадра на ПК %.2f -> %.2f мс (x%.1f): %s\n",
           changed, inside, words_aa, t_ms[0], t_ms[1], t_ms[1] / (t_ms[0] > 0 ? t_ms[0] : 1), err ? "НЕТ" : "да");
    return err;
}

static int t3_errors(void)
{
    int ok = 1;
    chip_reset();
    uint32_t v = info3(0);
    if (v != (0x3D000000u | (FTC3D_VERSION << 16) | FTC3D_MAX_TRIS))
        ok = 0;
    const uint32_t bad[][4] = {
        {0xFFFFFFA0u, 0, 7, 6 | (8u << 16)},           /* ширина не кратна 4 */
        {0xFFFFFFA0u, 0, 2, 8 | (8u << 16)},           /* L4 — не поддерживается */
        {0xFFFFFFA0u, 2, 7, 8 | (8u << 16)},           /* адрес не кратен 4 */
        {0xFFFFFFA0u, 0x2FFF00u, 7, 64 | (64u << 16)}, /* за RAM_G */
        {0xFFFFFFA0u, 0, 7 | 0x200u, 8 | (8u << 16)},  /* неизвестный бит формата (0x100 — SSAA) */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        chip_reset();
        for (int k = 0; k < 4; k++)
            C(bad[i][k]);
        if (!run3() || peek32(FTC_REG_CMD_READ) != 0xFFF || cop.fault != FTC_FAULT_3D)
            ok = 0;
    }
    chip_reset();
    C(0xFFFFFFABu);                                    /* RENDER без цели */
    if (!run3() || cop.fault != FTC_FAULT_3D)
        ok = 0;
    printf("3D, CMD_3D_INFO и ошибки параметров: %s\n", ok ? "да" : "НЕТ");
    return !ok;
}

/* --- CMD_TEXT ----------------------------------------------------------------------------------
 * Случаи сняты с bt8xxemu (fpga/vdac2/scripts/ft_probe_text.py): образ ПЗУ FT812 с 0x1E0000 и
 * text_dl.txt — в FT812_ROM_DIR (по умолчанию E:/zx/ft812_rom; данные Bridgetek, не в репозитории).
 * Нет файлов — проверка пропускается. Каждый случай: CMD_DLSTART, CMD_TEXT, лист — слово в слово;
 * затем шрифт 3 через CMD_SETFONT с метриками шрифта 26 в RAM_G — тот же лист с ручкой 3. */
static size_t text_cmd(uint8_t *b, int x, int y, unsigned font, unsigned opt, const char *s, size_t n)
{
    size_t k = 0;
    uint32_t w[3] = {0xFFFFFF0Cu, ((uint32_t)(uint16_t)y << 16) | (uint16_t)x, ((uint32_t)opt << 16) | font};
    memcpy(b, w, 12);
    k = 12;
    memcpy(b + k, s, n);
    k += n;
    b[k++] = 0;
    while (k & 3)
        b[k++] = 0;
    return k;
}

static int t_text(void)
{
    const char *dir = getenv("FT812_ROM_DIR");
    char p[512];
    snprintf(p, sizeof p, "%s/ft812_rom_1E0000.bin", dir ? dir : "E:/zx/ft812_rom");
    FILE *fr = fopen(p, "rb");
    snprintf(p, sizeof p, "%s/text_dl.txt", dir ? dir : "E:/zx/ft812_rom");
    FILE *fc = fopen(p, "r");
    if (!fr || !fc) {
        printf("CMD_TEXT: нет образа ПЗУ или случаев bt8xxemu (FT812_ROM_DIR) — пропущено\n");
        if (fr) fclose(fr);
        if (fc) fclose(fc);
        return 0;
    }
    static uint8_t rom[0x120000];
    size_t rn = fread(rom, 1, sizeof rom, fr);
    fclose(fr);
    if (rn != sizeof rom) {
        fprintf(stderr, "CMD_TEXT: образ ПЗУ короче 1152 КБ\n");
        fclose(fc);
        return 1;
    }
    int cases = 0, bad = 0;
    static char line[16384];
    uint32_t first_dl[64];
    uint32_t first_n = 0;
    while (fgets(line, sizeof line, fc)) {
        int x, y;
        unsigned font, opt, n;
        char hx[512];
        int off = 0;
        if (sscanf(line, "%d %d %u %u %511s %u%n", &x, &y, &font, &opt, hx, &n, &off) != 6)
            continue;
        char s[256];
        size_t sl = 0;
        if (strcmp(hx, "-") != 0)
            for (size_t i = 0; hx[2 * i] && sl < sizeof s; i++) {
                unsigned v;
                sscanf(hx + 2 * i, "%2x", &v);
                s[sl++] = (char)v;
            }
        uint32_t want[4096];
        char *q = line + off;
        for (unsigned i = 0; i < n && i < 4096; i++)
            want[i] = (uint32_t)strtoul(q, &q, 16);
        chip_reset();
        memcpy(mem + 0x1E0000, rom, sizeof rom);
        uint8_t st[512];
        uint32_t dls = 0xFFFFFF00u;
        memcpy(st, &dls, 4);
        size_t k = 4 + text_cmd(st + 4, x, y, font, opt, s, sl);
        int err = feed(st, k, 0);
        uint32_t got_n = peek32(FTC_REG_CMD_DL) / 4;
        int ok = !err && got_n == n;
        for (unsigned i = 0; ok && i < n; i++)
            ok = peek32(FTC_RAM_DL + 4 * i) == want[i];
        if (!ok && bad < 4) {
            fprintf(stderr, "CMD_TEXT(%d,%d,%u,%#x,%.*s): слов %u, ждали %u\n", x, y, font, opt, (int)sl, s, got_n, n);
            for (unsigned i = 0; i < n && i < 12; i++)
                fprintf(stderr, "  %08X %08X\n", peek32(FTC_RAM_DL + 4 * i), want[i]);
        }
        if (cases == 0 && n <= 64) {
            memcpy(first_dl, want, 4 * n);
            first_n = n;
        }
        bad += !ok;
        cases++;
    }
    fclose(fc);
    /* шрифт 3 по CMD_SETFONT — метрики шрифта 26 (первый случай: 45, 712, «12:34:56») */
    int sf_ok = 0;
    if (first_n > 4) {
        chip_reset();
        memcpy(mem + 0x1E0000, rom, sizeof rom);
        uint32_t root = peek32(0x2FFFFC);
        memcpy(mem + 0x10000, mem + root + 148 * (26 - 16), 148);
        uint32_t sf[3] = {0xFFFFFF00u, 0, 0};
        uint8_t st[512];
        memcpy(st, sf, 4);
        uint32_t setfont[3] = {0xFFFFFF2Bu, 3, 0x10000};
        memcpy(st + 4, setfont, 12);
        size_t k = 16 + text_cmd(st + 16, 45, 712, 3, 0, "12:34:56", 8);
        sf_ok = !feed(st, k, 0) && peek32(FTC_REG_CMD_DL) / 4 == first_n;
        for (uint32_t i = 0; sf_ok && i < first_n; i++) {
            uint32_t w = first_dl[i];
            if (i == 2)
                w = 0x05000003u;                 /* BITMAP_HANDLE(3) */
            else if ((w >> 30) == 2)
                w = (w & ~(31u << 7)) | (3u << 7); /* VERTEX2II: ручка 3 */
            sf_ok = peek32(FTC_RAM_DL + 4 * i) == w;
        }
    }
    /* Чтение метрик под нагрузкой — нули (R-Type: «ширины знаков обнуляются, буквы накладываются
       друг на друга»): CMD_TEXT ждёт годных метрик и читает снова; годные метрики ПЗУ-шрифта — в кэше
       сопроцессора, и дальше нулевые чтения ПЗУ тексту не мешают. Сброс сопроцессора — давно
       (часы стенда — с 100 000 мс). */
    int zero_ok = 0, cache_ok = 0;
    if (first_n > 4) {
        chip_reset();
        cop.bus.ms = b_ms;
        fake_now = 100000;
        memcpy(mem + 0x1E0000, rom, sizeof rom);
        for (int pass = 0; pass < 2; pass++) {
            rd_zero_rom = pass == 0 ? 3 : 1000000;
            uint8_t st[512];
            uint32_t dls = 0xFFFFFF00u;
            memcpy(st, &dls, 4);
            size_t k = 4 + text_cmd(st + 4, 45, 712, 26, 0, "12:34:56", 8);
            int ok = !feed(st, k, 0) && peek32(FTC_REG_CMD_DL) / 4 == first_n;
            for (uint32_t i = 0; ok && i < first_n; i++)
                ok = peek32(FTC_RAM_DL + 4 * i) == first_dl[i];
            if (pass == 0)
                zero_ok = ok && rd_zero_rom == 0;
            else
                cache_ok = ok;
        }
        rd_zero_rom = 0;
    }
    printf("CMD_TEXT: случаев bt8xxemu %d, расходится %d; CMD_SETFONT (шрифт 3 = метрики 26): %s; "
           "нулевые чтения метрик: %s, метрики ПЗУ в кэше: %s\n",
           cases, bad, sf_ok ? "да" : "НЕТ", zero_ok ? "читаются снова" : "НЕТ",
           cache_ok ? "да" : "НЕТ");
    return bad != 0 || !sf_ok || cases == 0 || !zero_ok || !cache_ok;
}

/* CMD_NUMBER: случаи bt8xxemu (fpga/vdac2/scripts/ft_probe_number.py → FT812_ROM_DIR/number_dl.txt:
   x y шрифт опции база число, слова листа) — CMD_SETBASE, CMD_NUMBER, слово в слово */
static int t_number(void)
{
    const char *dir = getenv("FT812_ROM_DIR");
    char p[512];
    snprintf(p, sizeof p, "%s/ft812_rom_1E0000.bin", dir ? dir : "E:/zx/ft812_rom");
    FILE *fr = fopen(p, "rb");
    snprintf(p, sizeof p, "%s/number_dl.txt", dir ? dir : "E:/zx/ft812_rom");
    FILE *fc = fopen(p, "r");
    if (!fr || !fc) {
        printf("CMD_NUMBER: нет образа ПЗУ или случаев bt8xxemu (FT812_ROM_DIR) — пропущено\n");
        if (fr) fclose(fr);
        if (fc) fclose(fc);
        return 0;
    }
    static uint8_t rom[0x120000];
    size_t rn = fread(rom, 1, sizeof rom, fr);
    fclose(fr);
    if (rn != sizeof rom) {
        fclose(fc);
        return 1;
    }
    int cases = 0, bad = 0;
    static char line[16384];
    while (fgets(line, sizeof line, fc)) {
        int x, y, off = 0;
        unsigned font, opt, base, n;
        unsigned long num;
        if (sscanf(line, "%d %d %u %u %u %lu %u%n", &x, &y, &font, &opt, &base, &num, &n, &off) != 7)
            continue;
        uint32_t want[256];
        char *q = line + off;
        for (unsigned i = 0; i < n && i < 256; i++)
            want[i] = (uint32_t)strtoul(q, &q, 16);
        chip_reset();
        memcpy(mem + 0x1E0000, rom, sizeof rom);
        uint32_t st[6] = {0xFFFFFF00u, 0xFFFFFF38u, base, 0xFFFFFF2Eu,
                          ((uint32_t)(uint16_t)y << 16) | (uint16_t)x, ((uint32_t)opt << 16) | font};
        uint8_t b[32];
        memcpy(b, st, 24);
        uint32_t nv = (uint32_t)num;
        memcpy(b + 24, &nv, 4);
        int err = feed(b, 28, 0);
        uint32_t got_n = peek32(FTC_REG_CMD_DL) / 4;
        int ok = !err && got_n == n;
        for (unsigned i = 0; ok && i < n; i++)
            ok = peek32(FTC_RAM_DL + 4 * i) == want[i];
        if (!ok && bad < 4) {
            fprintf(stderr, "CMD_NUMBER(%d,%d,%u,%#x,база %u,%lu): слов %u, ждали %u\n", x, y, font, opt, base,
                    num, got_n, n);
            for (unsigned i = 0; i < n && i < 12; i++)
                fprintf(stderr, "  %08X %08X\n", peek32(FTC_RAM_DL + 4 * i), want[i]);
        }
        bad += !ok;
        cases++;
    }
    fclose(fc);
    printf("CMD_NUMBER: случаев bt8xxemu %d, расходится %d\n", cases, bad);
    return bad != 0 || cases == 0;
}

/* CMD_PROGRESS: случаи bt8xxemu (fpga/vdac2/scripts/ft_probe_progress.py → data/progress.txt:
   x y w h опции значение предел fg bg, слова листа) — CMD_FGCOLOR, CMD_BGCOLOR, CMD_PROGRESS */
static int t_progress(const char *dir)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/progress.txt", dir);
    FILE *fc = fopen(p, "r");
    if (!fc) {
        printf("CMD_PROGRESS: нет %s\n", p);
        return 1;
    }
    int cases = 0, bad = 0;
    static char line[8192];
    while (fgets(line, sizeof line, fc)) {
        int x, y, w, h, off = 0;
        unsigned opt, val, rng, fg, bg, n;
        if (sscanf(line, "%d %d %d %d %u %u %u %u %u %u%n", &x, &y, &w, &h, &opt, &val, &rng, &fg, &bg, &n,
                   &off) != 10)
            continue;
        uint32_t want[64];
        char *q = line + off;
        for (unsigned i = 0; i < n && i < 64; i++)
            want[i] = (uint32_t)strtoul(q, &q, 16);
        chip_reset();
        uint32_t st[10] = {0xFFFFFF00u, 0xFFFFFF0Au, fg, 0xFFFFFF09u, bg, 0xFFFFFF0Fu,
                           ((uint32_t)(uint16_t)y << 16) | (uint16_t)x, ((uint32_t)(uint16_t)h << 16) | (uint16_t)w,
                           (val << 16) | opt, rng};
        int err = feed((const uint8_t *)st, sizeof st, 0);
        uint32_t got_n = peek32(FTC_REG_CMD_DL) / 4;
        int ok = !err && got_n == n;
        for (unsigned i = 0; ok && i < n; i++)
            ok = peek32(FTC_RAM_DL + 4 * i) == want[i];
        if (!ok && bad < 4) {
            fprintf(stderr, "CMD_PROGRESS(%d,%d,%d,%d,%#x,%u,%u): слов %u, ждали %u\n", x, y, w, h, opt, val, rng,
                    got_n, n);
            for (unsigned i = 0; i < n && i < 24; i++)
                fprintf(stderr, "  %08X %08X\n", peek32(FTC_RAM_DL + 4 * i), want[i]);
        }
        bad += !ok;
        cases++;
    }
    fclose(fc);
    printf("CMD_PROGRESS: случаев bt8xxemu %d, расходится %d\n", cases, bad);
    return bad != 0 || cases == 0;
}

/* CMD_LOADIMAGE через кольцо: картинки data/image (make_image_data.py) — RAM_G с ptr, слова
   листа (как у bt8xxemu: [PALETTE_SOURCE], SOURCE, LAYOUT_H, LAYOUT, SIZE_H, SIZE) и ответ
   следующей CMD_GETPROPS (данные картинки сняты с кольца до слова) */
static int t_loadimage(const char *dir, size_t chunk)
{
    char path[1024], line[512];
    snprintf(path, sizeof path, "%s/image/list.txt", dir);
    FILE *lst = fopen(path, "r");
    if (!lst) {
        printf("LOADIMAGE: нет %s\n", path);
        return 1;
    }
    int bad = 0, n_ok = 0;
    const uint32_t ptr = 0x1000;
    while (fgets(line, sizeof line, lst)) {
        char file[256], expf[256];
        unsigned fmt, w, h, stride, pal;
        if (sscanf(line, "%255s %255s %u %u %u %u %u", file, expf, &fmt, &w, &h, &stride, &pal) != 7)
            continue;
        size_t n, en;
        snprintf(path, sizeof path, "%s/image/%s", dir, file);
        uint8_t *data = load(path, &n);
        snprintf(path, sizeof path, "%s/image/%s", dir, expf);
        uint8_t *exp = load(path, &en);
        if (!data || !exp) {
            bad++;
            continue;
        }
        size_t padded = (n + 3) & ~(size_t)3;
        uint8_t *s = malloc(padded + 64);
        uint32_t hdr[4] = {0xFFFFFF00u, 0xFFFFFF24u, ptr, 0};
        memcpy(s, hdr, 16);
        memset(s + 16, 0, padded);
        memcpy(s + 16, data, n);
        uint32_t tail[4] = {0xFFFFFF25u, 0, 0, 0};
        memcpy(s + 16 + padded, tail, 16);
        chip_reset();
        int err = feed(s, 16 + padded + 16, chunk);
        uint32_t rd = peek32(FTC_REG_CMD_READ);
        uint32_t gp[3];
        for (int i = 0; i < 3; i++)
            gp[i] = peek32(FTC_RAM_CMD + ((rd - 12 + 4 * i) & 0xFFF));
        int jpeg = strstr(file, ".jpg") != NULL;
        uint32_t src = ptr + pal, want_end = jpeg ? ptr : src + stride * h;
        uint32_t dl[8], nd = 0;
        if (pal)
            dl[nd++] = (0x2Au << 24) | ptr;
        dl[nd++] = (0x01u << 24) | src;
        dl[nd++] = (0x28u << 24) | ((stride >> 10) << 2) | (h >> 9);
        dl[nd++] = (0x07u << 24) | (fmt << 19) | ((stride & 0x3FF) << 9) | (h & 0x1FF);
        dl[nd++] = (0x29u << 24) | ((w >> 9) << 2) | (h >> 9);
        dl[nd++] = (0x08u << 24) | ((w & 0x1FF) << 9) | (h & 0x1FF);
        int dl_ok = peek32(FTC_REG_CMD_DL) == 4 * nd;
        for (uint32_t i = 0; i < nd && dl_ok; i++)
            dl_ok = peek32(FTC_RAM_DL + 4 * i) == dl[i];
        int mem_ok = !memcmp(mem + ptr, exp, en);
        int ok = !err && mem_ok && dl_ok && gp[0] == want_end && gp[1] == w && gp[2] == h;
        if (!ok) {
            printf("LOADIMAGE %-14s куски %u: ошибка %d, RAM_G %s, лист %s, GETPROPS %x %u %u (ждали %x %u %u)\n",
                   file, (unsigned)chunk, err, mem_ok ? "да" : "НЕТ", dl_ok ? "да" : "НЕТ", gp[0], gp[1],
                   gp[2], want_end, w, h);
            bad++;
        } else {
            n_ok++;
        }
        free(s);
        free(data);
        free(exp);
    }
    fclose(lst);
    printf("LOADIMAGE (куски %u): %d верно, %d нет\n", (unsigned)chunk, n_ok, bad);
    return bad != 0;
}

/* 3D после картинки: CMD_LOADIMAGE берёт рабочую память 3D (кэш сеток, треугольники), 3D
   при следующей команде начинает её заново — кадр тот же, сетки перечитываются из RAM_G */
static int t3_after_image(const char *dir)
{
    static uint8_t a[256 * 192 * 2];
    const uint32_t size = sizeof a;
    int err = 0;
    chip_reset();
    put_scene();
    scene_flags = 8;
    scene(7, 1);
    err |= run3();
    memcpy(a, mem + T3_TARGET, size);
    char path[1024];
    snprintf(path, sizeof path, "%s/image/prgbbig.png", dir);
    size_t n;
    uint8_t *data = load(path, &n);
    if (!data)
        return 1;
    size_t padded = (n + 3) & ~(size_t)3;
    uint8_t *st = calloc(1, padded + 16);
    uint32_t hdr[3] = {0xFFFFFF24u, 0x140000u, 0};
    memcpy(st, hdr, 12);
    memcpy(st + 12, data, n);
    err |= feed(st, 12 + padded, 0);
    free(st);
    free(data);
    unsigned long long w0 = bus_words;
    scene(7, 1);
    err |= run3();
    unsigned long long w1 = bus_words - w0;
    int same = memcmp(a, mem + T3_TARGET, size) == 0;
    w0 = bus_words;
    scene(7, 1);
    err |= run3();
    unsigned long long w2 = bus_words - w0;
    err |= !same || w1 <= w2;
    printf("3D после CMD_LOADIMAGE: кадр %s, слов по шине %llu (сетки перечитаны), дальше %llu (кэш): %s\n",
           same ? "тот же" : "ДРУГОЙ", w1, w2, err ? "НЕТ" : "да");
    return err;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "host_test DATA_DIR RAMG_BIN\n");
        return 2;
    }
    int err = 0;
    err |= weather2(argv[1], argv[2], 0);
    err |= weather2(argv[1], argv[2], 60);
    err |= weather2(argv[1], argv[2], 4);
    err |= gradients(argv[1]);
    err |= recovery();
    err |= t3_watertight();
    err |= t3_scene();
    err |= t3_aa();
    err |= t3_cache();
    err |= t3_depth();
    err |= t3_clip();
    err |= t3_errors();
    err |= t_text();
    err |= t_number();
    err |= t_progress(argv[1]);
    err |= t_loadimage(argv[1], 0);
    err |= t3_after_image(argv[1]);
    err |= t_loadimage(argv[1], 60);
    printf(err ? "ТЕСТ НЕ ПРОЙДЕН\n" : "ТЕСТ ПРОЙДЕН\n");
    return err;
}
