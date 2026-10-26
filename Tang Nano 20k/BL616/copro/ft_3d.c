/* SPDX-License-Identifier: Apache-2.0 */
/*
 * 3D сопроцессора — свои команды VDAC4, которых нет у FT812/BT81x (коды 0xFFFFFFA0…0xFFFFFFAF).
 * Универсальные команды версии 3 (AD/AE/AF): см. vdac3d.h и docs/3d-api-v3.md.
 *
 * Растеризатор Tang рисует дисплей-лист заново на каждой строке и косых краёв не умеет, поэтому
 * сцену считает BL616 целиком: преобразование, отсечение, свет, растеризацию с z-буфером. Итог —
 * обычный битмап в RAM_G (цель), который хост показывает сам: CMD_SETBITMAP, BITMAPS, при нужде
 * с увеличением (CMD_SCALE + CMD_SETMATRIX). Цель считается полосами по FTC3D_STRIP байтов, и
 * каждая полоса сразу уходит в RAM_G: память BL616 — только на полосу, а размер цели — любой до
 * 1024 точек в ширину. Предел скорости — SPI к ПЛИС (~3 МБ/с): 256×192 по байту на точку —
 * ~15 мс, по два байта — ~30 мс. Пока BL616 пишет цель, её не надо показывать: две цели
 * (A, B) и смена в листе вместе с CMD_SWAP.
 *
 * Команды (параметры — слова кольца; 16.16 — число с фиксированной точкой, угол — доля оборота
 * в 1/65536, как у CMD_ROTATE):
 *   A0 CMD_3D_TARGET(addr, format, w | h << 16)   цель: RAM_G, адрес кратен 4; форматы — RGB565,
 *        ARGB1555 (фон — с альфой CMD_3D_CLEAR), RGB332, ARGB2 (с упорядоченным дизерингом), L8;
 *        w 4…1024 кратно 4, h 1…2048. Центр проекции — середина цели. format | 0x100 (версия 2)
 *        — сглаживание SSAA 2×2: 4 отсчёта на точку по повёрнутой сетке, среднее — в цель; цель
 *        и канал к ПЛИС те же, счёт точек — вчетверо. Прошивка версии 1 такой формат отвергает.
 *   A1 CMD_3D_PROJECT(focal, znear)       перспектива, 16.16: x' = w/2 + focal·x/z,
 *        y' = h/2 − focal·y/z; видно то, что дальше znear. Камера в начале координат, x вправо,
 *        y вверх, z — от зрителя.
 *   A2 CMD_3D_IDENTITY  A3 CMD_3D_TRANSLATE(x, y, z)  A4 CMD_3D_ROTATE(ax, ay, az)
 *   A5 CMD_3D_SCALE(sx, sy, sz)  A6 CMD_3D_PUSH  A7 CMD_3D_POP
 *        матрица модель-вид, как у OpenGL: каждая команда умножает её справа, т. е. последняя
 *        действует на вершины первой. ROTATE — вокруг X, затем Y, затем Z (M = M·Rz·Ry·Rx);
 *        положительный угол поворачивает X к Y, Y к Z, Z к X. Стек — FTC3D_STACK матриц.
 *   A8 CMD_3D_LIGHT(x | y << 16, z | ambient << 16 | diffuse << 24)   направление на свет
 *        в пространстве камеры (int16, любой длины), фон и рассеянный свет 0…255.
 *   A9 CMD_3D_CLEAR(argb)                 начать кадр: список треугольников пуст, фон — цвет.
 *   AA CMD_3D_MESH(vaddr, faddr, nv | nf << 16, flags)   сетка из RAM_G в кадр:
 *        вершина — int16 x, y, z, 0 (8 байтов); грань — uint16 a, b, c, цвет RGB565 (8 байтов);
 *        лицевая сторона — обход против часовой, если смотреть на неё снаружи.
 *        flags: 1 — отбросить нелицевые грани, 2 — свет по граням, 4 — гладкий свет (Гуро:
 *        нормали вершин из граней вокруг; 2 тогда не нужен); без 2 и 4 — цвет граней как есть;
 *        8 — сетка не меняется: BL616 читает её из RAM_G один раз и держит копию (FTC3D_CACHE_WORDS
 *        слов на все такие сетки) до сброса сопроцессора; без 8 сетка читается заново, копия
 *        выбрасывается. До FTC3D_MAX_VERTS вершин, FTC3D_MAX_FACES граней.
 *   AB CMD_3D_RENDER                      нарисовать кадр в цель. Команда кончается, когда
 *        цель записана: REG_CMD_READ уходит в ПЛИС после всех слов цели.
 *   AC CMD_3D_INFO(out version, out stats)   0x3D000000 | версия << 16 | FTC3D_MAX_TRIS;
 *        прошлый кадр: треугольников | мс << 16 (старшие 16 — 0xFFFF, если больше).
 * Ошибка параметров — FTC_FAULT_3D (REG_CMD_READ = 0xFFF, как любая ошибка сопроцессора).
 * У настоящего FT812 этих команд нет (ошибка): так программа и узнаёт, есть ли 3D.
 *
 * Растеризация — по центрам точек, правило «верх-лево» (общий край двух треугольников
 * закрашивается ровно один раз); глубина — znear/z по плоскости треугольника (линейна на
 * экране), z-буфер 24 бита, ближе — больше; свет Гуро — линейно по экрану. Треугольники
 * полностью текстурированного кадра сортируются от ближних к дальним; смешанный кадр —
 * в исходном порядке. В каждой точке побеждает ближний, при равной глубине — первый
 * в исходном порядке подачи.
 *
 * SSAA 2×2: те же правила, но вместо центра точки — 4 отсчёта (AA_X, AA_Y; повёрнутая сетка: у
 * почти горизонтальных и почти вертикальных краёв — 4 ступени, а не 2), у каждого свой z и цвет
 * RGB565. Среднее отсчётов — ровно центр точки, поэтому внутри граней тон тот же. Память та же:
 * полоса вчетверо ниже, z отсчётов — в zb, цвета — в полосе pix[cur], свёртка пишет итог туда же
 * (точка i читает отсчёты 4i…4i+3 раньше, чем запись доходит до них).
 */
#include <math.h>
#include <float.h>
#include <string.h>

#include "ft_copro.h"

#define CMD_3D_TARGET    0xFFFFFFA0u
#define CMD_3D_PROJECT   0xFFFFFFA1u
#define CMD_3D_IDENTITY  0xFFFFFFA2u
#define CMD_3D_TRANSLATE 0xFFFFFFA3u
#define CMD_3D_ROTATE    0xFFFFFFA4u
#define CMD_3D_SCALE     0xFFFFFFA5u
#define CMD_3D_PUSH      0xFFFFFFA6u
#define CMD_3D_POP       0xFFFFFFA7u
#define CMD_3D_LIGHT     0xFFFFFFA8u
#define CMD_3D_CLEAR     0xFFFFFFA9u
#define CMD_3D_MESH      0xFFFFFFAAu
#define CMD_3D_RENDER    0xFFFFFFABu
#define CMD_3D_INFO      0xFFFFFFACu
#define CMD_3D_TEXTURE   0xFFFFFFADu
#define CMD_3D_TEXMESH   0xFFFFFFAEu
#define CMD_3D_MATERIAL  0xFFFFFFAFu

#define MESH_CULL   1u
#define MESH_LIGHT  2u
#define MESH_SMOOTH 4u
#define MESH_STATIC 8u           /* сетка не меняется: из RAM_G — один раз, дальше из кэша */

#define GUARD 8192.0f           /* отсечение по сторонам: ±GUARD точек от центра цели */

/* SSAA 2×2: отсчёты в точке, 1/16 точки (повёрнутая сетка RGSS; среднее — центр 8, 8) */
static const int32_t AA_X[4] = {6, 14, 2, 10}, AA_Y[4] = {2, 6, 10, 14};
#define AA_MIN 2
#define AA_MAX 14

#ifdef FTC3D_PROBE              /* проверка на ПК: каждая закрашиваемая точка, высота полосы */
void ftc3d_probe(int x, int y);
extern int ftc3d_strip_lines;
int ftc3d_disable_order;
uint64_t ftc3d_hidden_spans, ftc3d_hidden_samples;
uint64_t ftc3d_vertex_transforms, ftc3d_vertex_projections;
#ifdef FTC3D_DIFFERENTIAL
int ftc3d_reference;
int ftc3d_exec_reference(ftc_t *c);
#endif
#endif

int ftc3d_params(uint32_t code)
{
    switch (code) {
    case CMD_3D_IDENTITY: case CMD_3D_PUSH: case CMD_3D_POP: case CMD_3D_RENDER:
        return 0;
    case CMD_3D_CLEAR:
        return 1;
    case CMD_3D_PROJECT: case CMD_3D_LIGHT: case CMD_3D_INFO:
        return 2;
    case CMD_3D_TARGET: case CMD_3D_TRANSLATE: case CMD_3D_ROTATE: case CMD_3D_SCALE:
        return 3;
    case CMD_3D_MESH: case CMD_3D_TEXTURE: case CMD_3D_TEXMESH:
        return 4;
    case CMD_3D_MATERIAL:
        return 5;
    default:
        return -1;
    }
}

static float fx(uint32_t v)
{
    return (float)(int32_t)v * (1.0f / 65536.0f);
}

static void identity(float *m)
{
    memset(m, 0, 12 * sizeof *m);
    m[0] = m[5] = m[10] = 1.0f;
}

static void set_light(ftc3d_t *g, float x, float y, float z)
{
    float l = sqrtf(x * x + y * y + z * z);
    if (l < 1e-6f) {
        x = 0.0f;
        y = 0.0f;
        z = -1.0f;
        l = 1.0f;
    }
    g->lx = x / l;
    g->ly = y / l;
    g->lz = z / l;
}

void ftc3d_scratch_lost(ftc3d_t *g)
{
    g->ntris = g->dropped = 0;
    g->ncached = g->cache_used = 0;
    g->tex_valid = 0;
}

void ftc3d_reset(ftc3d_t *g)
{
    for (int i=0;i<256;i++) g->unorm[i]=(float)i/255.0f;
    identity(g->m);
    g->sp = 0;
    g->addr = g->fmt = g->w = g->h = g->aa = 0;
    g->focal = 256.0f;
    g->znear = 1.0f;
    set_light(g, -1.0f, 1.0f, -2.0f);       /* слева сверху от зрителя */
    g->ambient = 64;
    g->diffuse = 191;
    g->clear = 0;
    g->fog_near = g->fog_far = g->color_steps = 0;
    g->material = 128u << 8;
    g->fog_color = 0;
    g->ntris = g->dropped = 0;
    g->last_tris = g->last_ms = 0;
    g->ncached = g->cache_used = 0;
    g->tex_valid = 0;
}

/* m = m × n (обе — аффинные 3×4) */
static void mat_mul(float *m, const float *n)
{
    float r[12];
    for (int i = 0; i < 3; i++) {
        const float *a = m + 4 * i;
        for (int j = 0; j < 4; j++)
            r[4 * i + j] = a[0] * n[j] + a[1] * n[4 + j] + a[2] * n[8 + j];
        r[4 * i + 3] += a[3];
    }
    memcpy(m, r, sizeof r);
}

static void rotate(float *m, uint32_t ax, uint32_t ay, uint32_t az)
{
    const float k = 6.28318530718f / 65536.0f;
    float n[12];
    float s = sinf(k * (float)(az & 0xFFFFu)), c = cosf(k * (float)(az & 0xFFFFu));
    identity(n);
    n[0] = c; n[1] = -s; n[4] = s; n[5] = c;
    mat_mul(m, n);
    s = sinf(k * (float)(ay & 0xFFFFu));
    c = cosf(k * (float)(ay & 0xFFFFu));
    identity(n);
    n[0] = c; n[2] = s; n[8] = -s; n[10] = c;
    mat_mul(m, n);
    s = sinf(k * (float)(ax & 0xFFFFu));
    c = cosf(k * (float)(ax & 0xFFFFu));
    identity(n);
    n[5] = c; n[6] = -s; n[9] = s; n[10] = c;
    mat_mul(m, n);
}

static uint32_t fmt_bytes(uint32_t fmt)
{
    return (fmt == 0 || fmt == 7) ? 2u : 1u;    /* ARGB1555, RGB565 — по два */
}

static int fmt_ok(uint32_t fmt)
{
    return fmt == 0 || fmt == 3 || fmt == 4 || fmt == 5 || fmt == 7;
}

/* --- треугольники кадра ----------------------------------------------------------------------- */
typedef struct cv {
    float x, y, z, s;           /* камера и освещённость */
    float attr[5];             /* u, v in texels; linear RGB multipliers */
    int textured;
    uint32_t origin;           /* TEXMESH vertex index + 1; zero after clipping */
} cv_t;

static float plane_d(const ftc3d_t *g, int k, const cv_t *v)
{
    switch (k) {
    case 0: return v->z - g->znear;
    case 1: return GUARD * v->z - g->focal * v->x;
    case 2: return GUARD * v->z + g->focal * v->x;
    case 3: return GUARD * v->z - g->focal * v->y;
    default: return GUARD * v->z + g->focal * v->y;
    }
}

static int32_t to_fix(float v)
{
    float f=v*16.0f+0.5f;
    int32_t n=(int32_t)f;
    return n-((float)n>f);     /* exact floor; guarded screen coordinates fit int32 */
}

static void add_tri(ftc3d_t *g, const cv_t *a, const cv_t *b, const cv_t *c, uint16_t color)
{
    const cv_t *p[3] = {a, b, c};
    float cx = 0.5f * (float)g->w, cy = 0.5f * (float)g->h;
    int32_t X[3], Y[3];
    float iz[3];
    uint8_t sh[3];
    for (int i = 0; i < 3; i++) {
        uint32_t origin=p[i]->origin;
        ftc3d_vtx_t *cached=origin?&g->v[origin-1]:NULL;
        if(cached && (g->vsh[origin-1]&2)) {
            X[i]=(int32_t)cached->nx;Y[i]=(int32_t)cached->ny;iz[i]=cached->nz;
        } else {
            float inv = 1.0f / p[i]->z;
            X[i] = to_fix(cx + g->focal * p[i]->x * inv);
            Y[i] = to_fix(cy - g->focal * p[i]->y * inv);
            iz[i] = g->znear * inv;
            if (iz[i] > 1.0f) iz[i] = 1.0f;
            if(cached) {
                /* Legacy normals are scratch during TEXMESH. Guarded screen
                   coordinates fit exactly in float, so caching is lossless. */
                cached->nx=(float)X[i];cached->ny=(float)Y[i];cached->nz=iz[i];
                g->vsh[origin-1]|=2;
            }
#ifdef FTC3D_PROBE
            ftc3d_vertex_projections++;
#endif
        }
        float s = p[i]->s;
        sh[i] = (uint8_t)(s <= 0.0f ? 0 : s >= 255.0f ? 255 : (int)(s + 0.5f));
    }
    int64_t area = (int64_t)(X[1] - X[0]) * (Y[2] - Y[0]) - (int64_t)(X[2] - X[0]) * (Y[1] - Y[0]);
    if (area == 0)
        return;
    if (area < 0) {                          /* обход — по часовой на экране (y вниз) */
        int32_t t = X[1]; X[1] = X[2]; X[2] = t;
        t = Y[1]; Y[1] = Y[2]; Y[2] = t;
        float f = iz[1]; iz[1] = iz[2]; iz[2] = f;
        uint8_t u = sh[1]; sh[1] = sh[2]; sh[2] = u;
        const cv_t *pv = p[1]; p[1] = p[2]; p[2] = pv;
    }
    int32_t x0 = X[0], x1 = X[0], y0 = Y[0], y1 = Y[0];
    for (int i = 1; i < 3; i++) {
        if (X[i] < x0) x0 = X[i];
        if (X[i] > x1) x1 = X[i];
        if (Y[i] < y0) y0 = Y[i];
        if (Y[i] > y1) y1 = Y[i];
    }
    /* строки и столбцы, чьи центры (16·n + 8) внутри рамки; у SSAA — отсчёты 16·n + 2…14 */
    const int32_t olo = g->aa ? AA_MIN : 8, ohi = g->aa ? AA_MAX : 8;
    int32_t r0 = (y0 - ohi + 15) >> 4, r1 = (y1 - olo) >> 4;
    if (r0 < 0) r0 = 0;
    if (r1 > (int32_t)g->h - 1) r1 = (int32_t)g->h - 1;
    if (r0 > r1 || x1 < olo || x0 > 16 * (int32_t)g->w - (16 - ohi))
        return;
    if (g->ntris >= FTC3D_MAX_TRIS) {
        g->dropped++;
        return;
    }
    ftc3d_tri_t *t = &g->tris[g->ntris++];
    for (int i = 0; i < 3; i++) {
        t->x[i] = X[i];
        t->y[i] = Y[i];
        t->iz[i] = iz[i];
        t->sh[i] = sh[i];
    }
    t->pad = (uint8_t)a->textured;
    if (a->textured)
        for (int k = 0; k < 5; k++)
            for (int i = 0; i < 3; i++)
                t->attr[k][i] = p[i]->attr[k] * iz[i];
    t->color = color;
    t->y0 = (int16_t)r0;
    t->y1 = (int16_t)r1;
}

/* Треугольник камеры → отсечение (ближняя плоскость и края охранной зоны) → в кадр. */
static void clip_tri(ftc3d_t *g, const cv_t *a, const cv_t *b, const cv_t *c, uint16_t color)
{
    int out = 0;
    const cv_t *p3[3] = {a, b, c};
    for (int k = 0; k < 5; k++)
        for (int i = 0; i < 3; i++)
            if (plane_d(g, k, p3[i]) < 0.0f)
                out |= 1 << k;
    if (!out) {
        add_tri(g, a, b, c, color);
        return;
    }
    cv_t buf[2][9];
    int n = 3, cur = 0;
    buf[0][0] = *a;
    buf[0][1] = *b;
    buf[0][2] = *c;
    for (int k = 0; k < 5 && n; k++) {
        if (!(out & (1 << k)))
            continue;
        const cv_t *in = buf[cur];
        cv_t *o = buf[cur ^ 1];
        int m = 0;
        for (int i = 0; i < n; i++) {
            const cv_t *p = &in[i], *q = &in[(i + 1) % n];
            float dp = plane_d(g, k, p), dq = plane_d(g, k, q);
            if (dp >= 0.0f)
                o[m++] = *p;
            if ((dp >= 0.0f) != (dq >= 0.0f)) {
                float t = dp / (dp - dq);
                o[m].x = p->x + t * (q->x - p->x);
                o[m].y = p->y + t * (q->y - p->y);
                o[m].z = p->z + t * (q->z - p->z);
                o[m].s = p->s + t * (q->s - p->s);
                o[m].textured = p->textured;
                o[m].origin = 0;
                for (int a = 0; a < 5; a++)
                    o[m].attr[a] = p->attr[a] + t * (q->attr[a] - p->attr[a]);
                if (k == 0)
                    o[m].z = g->znear;           /* точно на плоскости: znear/z = 1 */
                m++;
            }
        }
        n = m;
        cur ^= 1;
    }
    for (int i = 1; i + 1 < n; i++)
        add_tri(g, &buf[cur][0], &buf[cur][i], &buf[cur][i + 1], color);
}

static float shade(const ftc3d_t *g, float nx, float ny, float nz)
{
    float l = sqrtf(nx * nx + ny * ny + nz * nz);
    float d = l > 0.0f ? (nx * g->lx + ny * g->ly + nz * g->lz) / l : 0.0f;
    float s = (float)g->ambient + (d > 0.0f ? d * (float)g->diffuse : 0.0f);
    return s > 255.0f ? 255.0f : s;
}

/* Слова RAM_G [addr, addr + 4·words): из кэша (флаг MESH_STATIC), иначе чтением в tmp. Под
   нагрузкой растеризатора Tang (SDRAM занята кадром FT) чтение сетки на каждом кадре стоило
   десятки миллисекунд. Без флага кусок читается заново, а его копия в кэше выбрасывается. */
static const uint32_t *mesh_words(ftc_t *c, uint32_t addr, uint32_t words, uint32_t *tmp, int keep)
{
    ftc3d_t *g = &c->g3d;
    for (uint32_t i = 0; i < g->ncached; i++)
        if (g->cached[i].addr == addr && g->cached[i].words == words) {
            if (keep)
                return &g->cache[g->cached[i].at];
            g->cached[i].addr = 0xFFFFFFFFu;      /* сетка могла смениться */
        }
    if (keep && g->ncached < FTC3D_CACHE_ITEMS && g->cache_used + words <= FTC3D_CACHE_WORDS) {
        ftc3d_cached_t *e = &g->cached[g->ncached++];
        e->addr = addr;
        e->words = words;
        e->at = g->cache_used;
        g->cache_used += words;
        c->bus.rd(c->bus.ctx, addr, &g->cache[e->at], words);
        return &g->cache[e->at];
    }
    c->bus.rd(c->bus.ctx, addr, tmp, words);
    return tmp;
}

static int mesh(ftc_t *c)
{
    ftc3d_t *g = &c->g3d;
    uint32_t vaddr = c->p[0], faddr = c->p[1], nv = c->p[2] & 0xFFFFu, nf = c->p[2] >> 16, flags = c->p[3];
    if (nv > FTC3D_MAX_VERTS || nf > FTC3D_MAX_FACES || (vaddr & 3u) || (faddr & 3u))
        return 0;
    if (!nv || !nf || !g->w)
        return 1;
    /* вершины: слова RAM_G — в полосу цвета (её место свободно до CMD_3D_RENDER) */
    const int keep = (flags & MESH_STATIC) != 0;
    const uint32_t *raw = mesh_words(c, vaddr & 0x3FFFFCu, 2 * nv, (uint32_t *)g->pix[0], keep);
    const float *m = g->m;
    for (uint32_t i = 0; i < nv; i++) {
        float x = (float)(int16_t)raw[2 * i], y = (float)(int16_t)(raw[2 * i] >> 16);
        float z = (float)(int16_t)raw[2 * i + 1];
        ftc3d_vtx_t *v = &g->v[i];
        v->x = m[0] * x + m[1] * y + m[2] * z + m[3];
        v->y = m[4] * x + m[5] * y + m[6] * z + m[7];
        v->z = m[8] * x + m[9] * y + m[10] * z + m[11];
        v->nx = v->ny = v->nz = 0.0f;
    }
    const uint32_t *fw = mesh_words(c, faddr & 0x3FFFFCu, 2 * nf, g->faces, keep);
    int smooth = (flags & MESH_SMOOTH) != 0;            /* гладкий свет — тоже свет */
    if (smooth) {
        for (uint32_t f = 0; f < nf; f++) {
            uint32_t ia = fw[2 * f] & 0xFFFFu, ib = fw[2 * f] >> 16, ic = fw[2 * f + 1] & 0xFFFFu;
            if (ia >= nv || ib >= nv || ic >= nv)
                continue;
            ftc3d_vtx_t *a = &g->v[ia], *b = &g->v[ib], *cc = &g->v[ic];
            float ux = cc->x - a->x, uy = cc->y - a->y, uz = cc->z - a->z;
            float wx = b->x - a->x, wy = b->y - a->y, wz = b->z - a->z;
            float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
            a->nx += nx; a->ny += ny; a->nz += nz;
            b->nx += nx; b->ny += ny; b->nz += nz;
            cc->nx += nx; cc->ny += ny; cc->nz += nz;
        }
        for (uint32_t i = 0; i < nv; i++)
            g->vsh[i] = (uint8_t)(shade(g, g->v[i].nx, g->v[i].ny, g->v[i].nz) + 0.5f);
    }
    for (uint32_t f = 0; f < nf; f++) {
        uint32_t ia = fw[2 * f] & 0xFFFFu, ib = fw[2 * f] >> 16, ic = fw[2 * f + 1] & 0xFFFFu;
        uint16_t color = (uint16_t)(fw[2 * f + 1] >> 16);
        if (ia >= nv || ib >= nv || ic >= nv)
            continue;
        const ftc3d_vtx_t *a = &g->v[ia], *b = &g->v[ib], *cc = &g->v[ic];
        /* нормаль наружу: (c − a) × (b − a) — система левая (z от зрителя) */
        float ux = cc->x - a->x, uy = cc->y - a->y, uz = cc->z - a->z;
        float wx = b->x - a->x, wy = b->y - a->y, wz = b->z - a->z;
        float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
        int front = nx * a->x + ny * a->y + nz * a->z < 0.0f;
        if ((flags & MESH_CULL) && !front)
            continue;
        cv_t p[3] = {{a->x, a->y, a->z, 255.0f, {0}, 0, 0}, {b->x, b->y, b->z, 255.0f, {0}, 0, 0}, {cc->x, cc->y, cc->z, 255.0f, {0}, 0, 0}};
        if (smooth) {
            p[0].s = g->vsh[ia];
            p[1].s = g->vsh[ib];
            p[2].s = g->vsh[ic];
        } else if (flags & MESH_LIGHT) {
            float s = front ? shade(g, nx, ny, nz) : shade(g, -nx, -ny, -nz);
            p[0].s = p[1].s = p[2].s = s;
        }
        clip_tri(g, &p[0], &p[1], &p[2], color);
    }
    return 1;
}

/* All new sources must lie wholly inside RAM_G, never in registers or the ring. */
static int ram_range(uint32_t addr, uint32_t bytes)
{
    return !(addr & 3u) && addr < 0x300000u && bytes <= 0x300000u - addr;
}

static int texture(ftc_t *c)
{
    ftc3d_t *g = &c->g3d;
    uint32_t w = c->p[1] & 65535u, h = c->p[1] >> 16, fmt = c->p[2];
    uint32_t bytes = w * h * ((fmt == 1 || fmt == 2) ? 2u : 1u);
    if (g->ntris || !w || !h || w > 4095 || h > 4095 || fmt > 3 ||
        bytes > sizeof g->texture || !ram_range(c->p[0], (bytes + 3u) & ~3u)) return 0;
    if ((fmt == 0 || fmt == 3) && !ram_range(c->p[3], fmt == 3 ? 2048 : 1024)) return 0;
    c->bus.rd(c->bus.ctx, c->p[0], (uint32_t *)g->texture, (bytes + 3u) / 4u);
    if (fmt == 0 || fmt == 3)
        c->bus.rd(c->bus.ctx, c->p[3], g->palette, fmt == 3 ? 512 : 256);
    g->tex_w = w; g->tex_h = h; g->tex_format = fmt; g->tex_valid = 1;
    return 1;
}

/* Q8.8 XYZ, unsigned Q12.4 UV, unsigned Q4.12 RGB: 16 bytes per vertex.
   Faces are three uint16 indices and a reserved zero uint16. */
static int texmesh(ftc_t *c)
{
    ftc3d_t *g = &c->g3d;
    uint32_t nv = c->p[2] & 65535u, nf = c->p[2] >> 16, flags = c->p[3];
    if (!g->tex_valid || !g->w || nv > FTC3D_MAX_VERTS || nf > FTC3D_MAX_FACES ||
        flags & ~1u || !ram_range(c->p[0], nv * 16) || !ram_range(c->p[1], nf * 8)) return 0;
    uint32_t *raw = (uint32_t *)g->pix[0];
    c->bus.rd(c->bus.ctx, c->p[1], g->faces, nf * 2);
    uint32_t first=nv,last=0;
    for(uint32_t f=0;f<nf;f++) {
        uint32_t indices[3]={g->faces[f*2]&65535u,g->faces[f*2]>>16,g->faces[f*2+1]&65535u};
        if(g->faces[f*2+1]>>16)return 0;
        for(int i=0;i<3;i++) {
            if(indices[i]>=nv)return 0;
            if(indices[i]<first)first=indices[i];
            if(indices[i]>last)last=indices[i];
        }
    }
    if(!nf)return 1;
    /* Fetch only the referenced interval, preserving the dynamic-source rules.
       The entire declared source range was still validated above. */
    c->bus.rd(c->bus.ctx,c->p[0]+first*16,raw+first*4,(last-first+1)*4);
    memset(g->vsh+first,0,last-first+1);
    for (uint32_t f = 0; f < nf; f++) {
        uint32_t ids[3] = {g->faces[f*2] & 65535u, g->faces[f*2] >> 16, g->faces[f*2+1] & 65535u};
        if (g->faces[f*2+1] >> 16) return 0;
        cv_t v[3];
        for (int i = 0; i < 3; i++) {
            if (ids[i] >= nv) return 0;
            const uint32_t *r = raw + ids[i] * 4;
            ftc3d_vtx_t *cached=&g->v[ids[i]];
            if(!g->vsh[ids[i]]) {
                float x=(int16_t)r[0]/256.0f,y=(int16_t)(r[0]>>16)/256.0f,z=(int16_t)r[1]/256.0f;
                const float *m=g->m;
                cached->x=m[0]*x+m[1]*y+m[2]*z+m[3];
                cached->y=m[4]*x+m[5]*y+m[6]*z+m[7];
                cached->z=m[8]*x+m[9]*y+m[10]*z+m[11];
                g->vsh[ids[i]]=1;
#ifdef FTC3D_PROBE
                ftc3d_vertex_transforms++;
#endif
            }
            v[i].x=cached->x;v[i].y=cached->y;v[i].z=cached->z;
            v[i].s=255;v[i].textured=1;v[i].origin=ids[i]+1;
            v[i].attr[0] = (r[1] >> 16) / 16.0f;
            v[i].attr[1] = (r[2] & 65535u) / 16.0f;
            v[i].attr[2] = (r[2] >> 16) / 4096.0f;
            v[i].attr[3] = (r[3] & 65535u) / 4096.0f;
            v[i].attr[4] = (r[3] >> 16) / 4096.0f;
        }
        if (flags & 1) {
            float ax=v[2].x-v[0].x, ay=v[2].y-v[0].y, az=v[2].z-v[0].z;
            float bx=v[1].x-v[0].x, by=v[1].y-v[0].y, bz=v[1].z-v[0].z;
            if ((ay*bz-az*by)*v[0].x+(az*bx-ax*bz)*v[0].y+(ax*by-ay*bx)*v[0].z >= 0) continue;
        }
        clip_tri(g, &v[0], &v[1], &v[2], 0xffff);
    }
    return 1;
}

/* --- растеризация ------------------------------------------------------------------------------ */
static const uint8_t bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

/* v 0…255 в L + 1 уровней с порогом d 0…15 */
static uint32_t q(uint32_t v, uint32_t L, uint32_t d)
{
    return (v * L * 16u + d * 255u) / 4080u;
}

static uint32_t pack(uint32_t fmt, uint32_t r, uint32_t gg, uint32_t b, uint32_t d)
{
    switch (fmt) {
    case 7: return ((r >> 3) << 11) | ((gg >> 2) << 5) | (b >> 3);
    case 0: return 0x8000u | ((r >> 3) << 10) | ((gg >> 3) << 5) | (b >> 3);
    case 4: return (q(r, 7, d) << 5) | (q(gg, 7, d) << 2) | q(b, 3, d);
    case 5: return 0xC0u | (q(r, 3, d) << 4) | (q(gg, 3, d) << 2) | q(b, 3, d);
    default: return (77u * r + 150u * gg + 29u * b + 128u) >> 8;    /* L8 */
    }
}

static uint32_t pack_clear(uint32_t fmt, uint32_t argb)
{
    uint32_t a = argb >> 24, r = (argb >> 16) & 0xFF, gg = (argb >> 8) & 0xFF, b = argb & 0xFF;
    switch (fmt) {
    case 0: return ((a >= 128) ? 0x8000u : 0u) | ((r >> 3) << 10) | ((gg >> 3) << 5) | (b >> 3);
    case 4: return (((r * 7 + 127) / 255) << 5) | (((gg * 7 + 127) / 255) << 2) | ((b * 3 + 127) / 255);
    case 5: return ((a >> 6) << 6) | (((r * 3 + 127) / 255) << 4) | (((gg * 3 + 127) / 255) << 2) |
                   ((b * 3 + 127) / 255);
    default: return pack(fmt, r, gg, b, 8);
    }
}

/* float → целое в [lo, hi]; NaN — lo */
static int32_t clampi(float v, int32_t lo, int32_t hi)
{
    if (!(v > (float)lo))
        return lo;
    if (v >= (float)hi)
        return hi;
    return (int32_t)v;
}

static int64_t ceil_div(int64_t a, int64_t b)        /* b > 0 */
{
    return a >= 0 ? (a + b - 1) / b : -((-a) / b);
}

static int64_t floor_div(int64_t a, int64_t b)       /* b > 0 */
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

/* Треугольник t в строки [ys, ys + rows) полосы. */
static void raster(ftc3d_t *g, const ftc3d_tri_t *t, int ys, int rows, uint8_t *pix, const ftc_bus_t *bus)
{
    const int w = (int)g->w;
    const uint32_t fmt = g->fmt, bytes = fmt_bytes(fmt);
    int64_t sx[3], sy[3], c0[3];
    for (int e = 0; e < 3; e++) {
        int a = e, b = (e + 1) % 3;
        int64_t dx = (int64_t)t->x[b] - t->x[a], dy = (int64_t)t->y[b] - t->y[a];
        sx[e] = -16 * dy;
        sy[e] = 16 * dx;
        c0[e] = dx * (8 - (int64_t)t->y[a]) - dy * (8 - (int64_t)t->x[a]);
        if (!(dy < 0 || (dy == 0 && dx > 0)))
            c0[e] -= 1;                          /* не верхний и не левый край: строго внутри */
    }
    /* плоскости глубины и света: значение у центра точки (x, y) = a0 + gx·(16x+8−X0) + gy·(16y+8−Y0).
       Площадь — точная целая (у иглы во float она могла бы стать 0) */
    float X10 = (float)(t->x[1] - t->x[0]), X20 = (float)(t->x[2] - t->x[0]);
    float Y10 = (float)(t->y[1] - t->y[0]), Y20 = (float)(t->y[2] - t->y[0]);
    int64_t area = (int64_t)(t->x[1] - t->x[0]) * (t->y[2] - t->y[0]) -
                   (int64_t)(t->x[2] - t->x[0]) * (t->y[1] - t->y[0]);
    float inv = 1.0f / (float)area;                  /* > 0: обход выровнен в add_tri */
    float z1 = t->iz[1] - t->iz[0], z2 = t->iz[2] - t->iz[0];
    float zgx = (z1 * Y20 - z2 * Y10) * inv, zgy = (z2 * X10 - z1 * X20) * inv;
    int flat = t->sh[0] == t->sh[1] && t->sh[1] == t->sh[2];
    float s1 = (float)t->sh[1] - t->sh[0], s2 = (float)t->sh[2] - t->sh[0];
    float sgx = (s1 * Y20 - s2 * Y10) * inv, sgy = (s2 * X10 - s1 * X20) * inv;
    uint32_t c = t->color;
    uint32_t r8 = ((c >> 11) << 3) | (c >> 13), g8 = (((c >> 5) & 63) << 2) | ((c >> 9) & 3),
             b8 = ((c & 31) << 3) | ((c >> 2) & 7);
    int dither = fmt == 4 || fmt == 5;
    uint32_t solid = 0;
    if (flat) {
        uint32_t s = t->sh[0];
        r8 = (r8 * s * 257u + 32896u) >> 16;
        g8 = (g8 * s * 257u + 32896u) >> 16;
        b8 = (b8 * s * 257u + 32896u) >> 16;
        solid = pack(fmt, r8, g8, b8, 8);
    }
    int ya = t->y0 > ys ? t->y0 : ys, yb = t->y1 < ys + rows - 1 ? t->y1 : ys + rows - 1;
    for (int y = ya; y <= yb; y++) {
        int64_t lo = 0, hi = w - 1;
        int empty = 0;
        for (int e = 0; e < 3 && !empty; e++) {
            int64_t ce = c0[e] + sy[e] * y;
            if (sx[e] > 0) {
                int64_t v = ceil_div(-ce, sx[e]);
                if (v > lo) lo = v;
            } else if (sx[e] < 0) {
                int64_t v = floor_div(ce, -sx[e]);
                if (v < hi) hi = v;
            } else if (ce < 0) {
                empty = 1;
            }
        }
        if (empty || lo > hi)
            continue;
        /* глубина и свет — по концам отрезка, зажатым в свои пределы: у иглы градиенты огромны,
           а шаг по отрезку из концов не выводит ни за пределы, ни за int32 */
        float pl = (float)(16 * lo + 8 - t->x[0]), ph = (float)(16 * hi + 8 - t->x[0]);
        float py = (float)(16 * y + 8 - t->y[0]);
        int32_t span = (int32_t)(hi - lo);
        float zr = t->iz[0] + zgy * py;
        int32_t z = clampi((zr + zgx * pl) * 16777216.0f, 1, 1 << 24);
        int32_t ze = clampi((zr + zgx * ph) * 16777216.0f, 1, 1 << 24);
        int32_t dz = span ? (int32_t)(((int64_t)ze - z) / span) : 0;
        int32_t s = 0, ds = 0;
        if (!flat) {
            float sr = (float)t->sh[0] + sgy * py;
            s = clampi((sr + sgx * pl) * 256.0f, 0, 255 * 256);
            int32_t se = clampi((sr + sgx * ph) * 256.0f, 0, 255 * 256);
            ds = span ? (se - s) / span : 0;
        }
        uint32_t *zp = g->zb + (y - ys) * w;
        if (bus->pump)
            bus->pump(bus->ctx);                 /* прошлая полоса уходит, пока считается эта */
        uint8_t *row = pix + (size_t)(y - ys) * (size_t)w * bytes;
        const uint8_t *bay = bayer + 4 * (y & 3);
        for (int x = (int)lo; x <= (int)hi; x++, z += dz, s += ds) {
#ifdef FTC3D_PROBE
            ftc3d_probe(x, y);
#endif
            uint32_t zq = (uint32_t)z;                 /* 1…2^24 */
            if (zq <= zp[x])
                continue;
            zp[x] = zq;
            uint32_t v = solid;
            if (!flat) {
                int32_t sv = s >> 8;
                uint32_t k = sv < 0 ? 0u : sv > 255 ? 255u : (uint32_t)sv;
                v = pack(fmt, (r8 * k * 257u + 32896u) >> 16, (g8 * k * 257u + 32896u) >> 16,
                         (b8 * k * 257u + 32896u) >> 16, bay[x & 3]);
            } else if (dither) {
                v = pack(fmt, r8, g8, b8, bay[x & 3]);
            }
            if (bytes == 2)
                ((uint16_t *)row)[x] = (uint16_t)v;
            else
                row[x] = (uint8_t)v;
        }
    }
}

static uint16_t rgb565(uint32_t r, uint32_t gg, uint32_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((gg >> 2) << 5) | (b >> 3));
}

/* SSAA: треугольник t в отсчёты строк [ys, ys + rows) полосы. z и цвет RGB565 — по 4 на точку,
   отсчёт s точки i — 4·i + s. Те же правила, что у raster, только вместо центра (8, 8) — отсчёт
   (AA_X[s], AA_Y[s]). */
static void raster_aa(ftc3d_t *g, const ftc3d_tri_t *t, int ys, int rows, uint16_t *col, const ftc_bus_t *bus)
{
    const int w = (int)g->w;
    float X10 = (float)(t->x[1] - t->x[0]), X20 = (float)(t->x[2] - t->x[0]);
    float Y10 = (float)(t->y[1] - t->y[0]), Y20 = (float)(t->y[2] - t->y[0]);
    int64_t area = (int64_t)(t->x[1] - t->x[0]) * (t->y[2] - t->y[0]) -
                   (int64_t)(t->x[2] - t->x[0]) * (t->y[1] - t->y[0]);
    float inv = 1.0f / (float)area;
    float z1 = t->iz[1] - t->iz[0], z2 = t->iz[2] - t->iz[0];
    float zgx = (z1 * Y20 - z2 * Y10) * inv, zgy = (z2 * X10 - z1 * X20) * inv;
    int flat = t->sh[0] == t->sh[1] && t->sh[1] == t->sh[2];
    float s1 = (float)t->sh[1] - t->sh[0], s2 = (float)t->sh[2] - t->sh[0];
    float sgx = (s1 * Y20 - s2 * Y10) * inv, sgy = (s2 * X10 - s1 * X20) * inv;
    uint32_t c = t->color;
    uint32_t r8 = ((c >> 11) << 3) | (c >> 13), g8 = (((c >> 5) & 63) << 2) | ((c >> 9) & 3),
             b8 = ((c & 31) << 3) | ((c >> 2) & 7);
    uint16_t solid = 0;
    if (flat) {
        uint32_t k = t->sh[0];
        solid = rgb565((r8 * k * 257u + 32896u) >> 16, (g8 * k * 257u + 32896u) >> 16,
                       (b8 * k * 257u + 32896u) >> 16);
    }
    int32_t ymin = t->y[0], ymax = t->y[0];
    for (int i = 1; i < 3; i++) {
        if (t->y[i] < ymin) ymin = t->y[i];
        if (t->y[i] > ymax) ymax = t->y[i];
    }
    for (int s = 0; s < 4; s++) {
        const int64_t ox = AA_X[s], oy = AA_Y[s];
        int64_t sx[3], sy[3], c0[3];
        for (int e = 0; e < 3; e++) {
            int a = e, b = (e + 1) % 3;
            int64_t dx = (int64_t)t->x[b] - t->x[a], dy = (int64_t)t->y[b] - t->y[a];
            sx[e] = -16 * dy;
            sy[e] = 16 * dx;
            c0[e] = dx * (oy - (int64_t)t->y[a]) - dy * (ox - (int64_t)t->x[a]);
            if (!(dy < 0 || (dy == 0 && dx > 0)))
                c0[e] -= 1;
        }
        int ya = (int)ceil_div((int64_t)ymin - oy, 16), yb = (int)floor_div((int64_t)ymax - oy, 16);
        if (ya < ys) ya = ys;
        if (yb > ys + rows - 1) yb = ys + rows - 1;
        for (int y = ya; y <= yb; y++) {
            int64_t lo = 0, hi = w - 1;
            int empty = 0;
            for (int e = 0; e < 3 && !empty; e++) {
                int64_t ce = c0[e] + sy[e] * y;
                if (sx[e] > 0) {
                    int64_t v = ceil_div(-ce, sx[e]);
                    if (v > lo) lo = v;
                } else if (sx[e] < 0) {
                    int64_t v = floor_div(ce, -sx[e]);
                    if (v < hi) hi = v;
                } else if (ce < 0) {
                    empty = 1;
                }
            }
            if (empty || lo > hi)
                continue;
            float pl = (float)(16 * lo + ox - t->x[0]), ph = (float)(16 * hi + ox - t->x[0]);
            float py = (float)(16 * y + oy - t->y[0]);
            int32_t span = (int32_t)(hi - lo);
            float zr = t->iz[0] + zgy * py;
            int32_t z = clampi((zr + zgx * pl) * 16777216.0f, 1, 1 << 24);
            int32_t ze = clampi((zr + zgx * ph) * 16777216.0f, 1, 1 << 24);
            int32_t dz = span ? (int32_t)(((int64_t)ze - z) / span) : 0;
            int32_t sh = 0, ds = 0;
            if (!flat) {
                float sr = (float)t->sh[0] + sgy * py;
                sh = clampi((sr + sgx * pl) * 256.0f, 0, 255 * 256);
                int32_t se = clampi((sr + sgx * ph) * 256.0f, 0, 255 * 256);
                ds = span ? (se - sh) / span : 0;
            }
            uint32_t *zp = g->zb + (size_t)(y - ys) * (size_t)w * 4u + (size_t)s;
            uint16_t *cp = col + (size_t)(y - ys) * (size_t)w * 4u + (size_t)s;
            if (bus->pump)
                bus->pump(bus->ctx);
            for (int x = (int)lo; x <= (int)hi; x++, z += dz, sh += ds) {
                uint32_t zq = (uint32_t)z;
                if (zq <= zp[4 * x])
                    continue;
                zp[4 * x] = zq;
                uint16_t v = solid;
                if (!flat) {
                    int32_t sv = sh >> 8;
                    uint32_t k = sv < 0 ? 0u : sv > 255 ? 255u : (uint32_t)sv;
                    v = rgb565((r8 * k * 257u + 32896u) >> 16, (g8 * k * 257u + 32896u) >> 16,
                               (b8 * k * 257u + 32896u) >> 16);
                }
                cp[4 * x] = v;
            }
        }
    }
}

/* SSAA: 4 отсчёта каждой из rows·w точек полосы → точка цели, в начало той же полосы. Покрытие
   отсчёта — его z (0 — фон). У прозрачного фона (ARGB1555, ARGB2 с альфой < 3) цвет — среднее
   покрытых отсчётов, альфа — по их числу; иначе — среднее всех четырёх. */
static void resolve_aa(const ftc3d_t *g, uint8_t *pix, int ys, int rows)
{
    const uint32_t w = g->w, fmt = g->fmt, n = (uint32_t)rows * w, bytes = fmt_bytes(fmt);
    const uint16_t *col = (const uint16_t *)pix;
    const uint32_t a = g->clear >> 24;
    const int clear_bg = (fmt == 0 && a < 128) || (fmt == 5 && (a >> 6) < 3);
    const uint32_t bg = pack_clear(fmt, g->clear);
    for (uint32_t i = 0; i < n; i++) {
        const uint16_t *sp = col + 4 * i;
        const uint32_t *zp = g->zb + 4 * i;
        uint32_t sr = 0, sg = 0, sb = 0, cr = 0, cg = 0, cb = 0, k = 0;
        for (int s = 0; s < 4; s++) {
            uint32_t v = sp[s], r = v >> 11, gg = (v >> 5) & 63u, b = v & 31u;
            sr += r; sg += gg; sb += b;
            if (zp[s]) {
                cr += r; cg += gg; cb += b;
                k++;
            }
        }
        uint32_t x = i % w, y = (uint32_t)ys + i / w, d = bayer[4 * (y & 3u) + (x & 3u)];
        uint32_t out;
        if (clear_bg) {
            if (!k) {
                out = bg;
            } else if (fmt == 0) {
                out = (k >= 2 ? 0x8000u : 0u) | (((cr + k / 2) / k) << 10) | (((cg + k) / (2 * k)) << 5) |
                      ((cb + k / 2) / k);
            } else {                                        /* ARGB2 */
                uint32_t r = (cr * 255u + 31u * k / 2) / (31u * k), gg = (cg * 255u + 63u * k / 2) / (63u * k),
                         b = (cb * 255u + 31u * k / 2) / (31u * k), a2 = (3u * k + (a >> 6) * (4u - k) + 2u) / 4u;
                out = (a2 << 6) | (q(r, 3, d) << 4) | (q(gg, 3, d) << 2) | q(b, 3, d);
            }
        } else if (fmt == 7) {
            out = (((sr + 2) >> 2) << 11) | (((sg + 2) >> 2) << 5) | ((sb + 2) >> 2);
        } else if (fmt == 0) {
            out = 0x8000u | (((sr + 2) >> 2) << 10) | (((sg + 4) >> 3) << 5) | ((sb + 2) >> 2);
        } else {
            out = pack(fmt, (sr * 255u + 62u) / 124u, (sg * 255u + 126u) / 252u, (sb * 255u + 62u) / 124u, d);
        }
        if (bytes == 2)
            ((uint16_t *)pix)[i] = (uint16_t)out;
        else
            pix[i] = (uint8_t)out;
    }
}

/* Exact edge intersections. Divide once when starting a strip, then advance
   the quotient and nonnegative remainder. No 64-bit division per scanline.
   GUARD bounds edge slopes/divisors well inside signed 32 bits. */
typedef struct tex_edge {
    int64_t q;
    uint32_t rem, divisor, delta_rem;
    int32_t delta;
    int side;
} tex_edge_t;

static void tex_edges(const ftc3d_tri_t *t, int ox, int oy, int y, tex_edge_t edge[3])
{
    for (int e=0;e<3;e++) {
        int b=(e+1)%3;
        int64_t dx=(int64_t)t->x[b]-t->x[e], dy=(int64_t)t->y[b]-t->y[e];
        int64_t sx=-16*dy, sy=16*dx;
        int64_t ce=dx*(16*(int64_t)y+oy-t->y[e])-dy*(ox-(int64_t)t->x[e]);
        if (!(dy<0 || (dy==0 && dx>0))) ce--;
        tex_edge_t *p=&edge[e];
        p->side=sx>0?1:sx<0?-1:0;
        if (!sx) {p->q=ce;p->delta=(int32_t)sy;continue;}
        int64_t numerator=sx>0?-ce:ce;
        int32_t step=(int32_t)(sx>0?-sy:sy), divisor=(int32_t)(sx>0?sx:-sx);
        p->q=floor_div(numerator,divisor);
        p->rem=(uint32_t)(numerator-p->q*divisor);p->divisor=(uint32_t)divisor;
        p->delta=step/divisor;
        int32_t remainder=step-p->delta*divisor;
        if (remainder<0) {remainder+=divisor;p->delta--;}
        p->delta_rem=(uint32_t)remainder;
    }
}

static int tex_span(tex_edge_t edge[3], int w, int *left, int *right)
{
    int64_t lo=0,hi=w-1;
    for (int e=0;e<3;e++) {
        tex_edge_t *p=&edge[e];
        if (!p->side) {if(p->q<0)hi=-1;p->q+=p->delta;continue;}
        int64_t x=p->q+(p->side>0 && p->rem!=0);
        if (p->side>0) {if(x>lo)lo=x;} else if(x<hi)hi=x;
        p->q+=p->delta;p->rem+=p->delta_rem;
        if(p->rem>=p->divisor){p->rem-=p->divisor;p->q++;}
    }
    if(lo>hi)return 0;
    *left=(int)lo;*right=(int)hi;return 1;
}

static inline __attribute__((always_inline)) uint32_t tex_channel(float value, float light, float fog,
    uint32_t fog_color, int black_fog, float steps, float inv_steps)
{
    value*=light;
    if(black_fog)value=value*fog;
    else value=value*fog+(1-fog)*fog_color/255.0f;
    if(steps>0) {
        float q=value*steps;
        int whole=(int)q;
        if((float)whole>q)whole--;
        value=(float)whole*inv_steps;
    }
    return (uint32_t)clampi(value*255+0.5f,0,255);
}

/* Conservative upper bound for the depth produced by repeated float adds.
   n <= 1023. The error allowance exceeds gamma_n*(|z|+n*|step|), including
   rounding of the endpoint/bound itself. One depth unit covers subnormals.
   Strict comparison preserves original primitive ownership at equal Z. */
static uint32_t tex_depth_upper(float z,float step,int n)
{
    float end=z+step*(float)n;
    /* Explicit builtins retain native FPU absolute value with -fno-builtin. */
    float magnitude=__builtin_fabsf(z)+__builtin_fabsf(step)*(float)n;
    float margin=magnitude*(4.0f*FLT_EPSILON*(float)(n+1))+1.0f/16777216.0f;
    float upper=(z>end?z:end)+margin;
    if(!(upper>0 && upper<1.0f))return 1u<<24;
    return (uint32_t)clampi(upper*16777216.0f,1,1<<24);
}

static int tex_hidden(const uint32_t *depth,int stride,int count,float z,float step)
{
    if(count<8 || !depth[0] || !depth[(count-1)*stride])return 0;
    uint32_t upper=tex_depth_upper(z,step,count-1);
    if(depth[0]<=upper || depth[(count-1)*stride]<=upper)return 0;
    for(int i=1;i<count-1;i++)if(depth[i*stride]<=upper)return 0;
#ifdef FTC3D_PROBE
    ftc3d_hidden_spans++;ftc3d_hidden_samples+=(unsigned)count;
#endif
    return 1;
}

#ifdef FTC3D_PROBE
uint32_t ftc3d_depth_upper_test(float z,float step,int n){return tex_depth_upper(z,step,n);}
#endif

/* Generic textured path. Attribute planes contain attribute/z, so UV and RGB
   remain perspective correct after clipping. Transparent texels never write Z. */
static __attribute__((noinline)) void raster_tex(ftc3d_t *g, const ftc3d_tri_t *t, int ys, int rows,
                       uint8_t *pix, const ftc_bus_t *bus, uint32_t primitive, int ordered)
{
    float dx1=(float)(t->x[1]-t->x[0]), dx2=(float)(t->x[2]-t->x[0]);
    float dy1=(float)(t->y[1]-t->y[0]), dy2=(float)(t->y[2]-t->y[0]);
    int64_t area=(int64_t)(t->x[1]-t->x[0])*(t->y[2]-t->y[0])-
                 (int64_t)(t->x[2]-t->x[0])*(t->y[1]-t->y[0]);
    float inv=1.0f/(float)area, gx[6], gy[6], base[6];
    for (int k=0;k<6;k++) {
        const float *a=k ? t->attr[k-1] : t->iz;
        base[k]=a[0];
        gx[k]=((a[1]-a[0])*dy2-(a[2]-a[0])*dy1)*inv;
        gy[k]=((a[2]-a[0])*dx1-(a[1]-a[0])*dx2)*inv;
    }
    const int ns=g->aa ? 4 : 1, w=(int)g->w;
    const int bytes=(int)fmt_bytes(g->fmt);
    const uint32_t fmt=g->aa?7:g->fmt;
    const uint32_t fog_color=g->fog_color, material=g->material;
    const int black_fog=!(fog_color&0xffffffu);
    const uint32_t tex_format=g->tex_format,tex_width=g->tex_w;
    const int tex_right=(int)g->tex_w-1,tex_bottom=(int)g->tex_h-1;
    const float steps=g->color_steps,znear=g->znear,fog_far=g->fog_far;
    const float inv_fog=(g->material&1)?1.0f/(g->fog_far-g->fog_near):0;
    const float inv_steps=g->color_steps>0?1.0f/g->color_steps:0;
    int ya=t->y0>ys?t->y0:ys, yb=t->y1<ys+rows-1?t->y1:ys+rows-1;
    for (int sample=0;sample<ns;sample++) {
        int ox=g->aa?AA_X[sample]:8, oy=g->aa?AA_Y[sample]:8;
        tex_edge_t edge[3];tex_edges(t,ox,oy,ya,edge);
        for (int y=ya;y<=yb;y++) {
            int lo,hi;if(!tex_span(edge,w,&lo,&hi))continue;
            if (bus->pump) bus->pump(bus->ctx);
            float px=(float)(16*lo+ox-t->x[0]),py=(float)(16*y+oy-t->y[0]);
            float first_z=base[0]+gx[0]*px+gy[0]*py;
            if(tex_hidden(g->zb+((y-ys)*w+lo)*ns+sample,ns,hi-lo+1,first_z,gx[0]*16))continue;
            float a[6], step[6];
            a[0]=first_z;step[0]=gx[0]*16;
            for (int k=1;k<6;k++) {
                a[k]=base[k]+gx[k]*px+gy[k]*py;
                step[k]=gx[k]*16;
            }
            /* Scalar induction variables stay in FPU registers on RV32.
               The arrays forced six load/add/store iterations per pixel. */
            float az=a[0],au=a[1],av=a[2],ar=a[3],ag=a[4],ab=a[5];
            const float dz=step[0],du=step[1],dv=step[2],dr=step[3],dg=step[4],db=step[5];
            for (int x=(int)lo;x<=(int)hi;x++) {
                uint32_t at=(uint32_t)(((y-ys)*w+x)*ns+sample);
                uint32_t z=(uint32_t)clampi(az*16777216.0f,1,1<<24);
                if (az>0 && (z>g->zb[at] || (ordered && z==g->zb[at] && primitive<g->owner[at]))) {
                    float iz=1.0f/az;
                    int u=clampi(au*iz,0,tex_right);
                    int v=clampi(av*iz,0,tex_bottom);
                    uint32_t ti=(uint32_t)v*tex_width+(uint32_t)u, rgba, flags=0;
                    if (tex_format==0 || tex_format==3) {
                        uint32_t index=g->texture[ti];
                        if (tex_format==3) { rgba=g->palette[index*2]; flags=g->palette[index*2+1]; }
                        else rgba=g->palette[index];
                    } else {
                        uint32_t c=((const uint16_t *)g->texture)[ti];
                        uint32_t r,gg,b=((c&31)*255+15)/31;
                        if (tex_format==1) {
                            r=((c>>11)*255+15)/31; gg=(((c>>5)&63)*255+31)/63;
                            rgba=0xff000000u;
                        } else {
                            r=(((c>>10)&31)*255+15)/31; gg=(((c>>5)&31)*255+15)/31;
                            rgba=(c&0x8000)?0xff000000u:0;
                        }
                        rgba|=(r<<16)|(gg<<8)|b;
                    }
                    if ((rgba>>24)>=((material>>8)&255)) {
                        float fog=1;
                        if ((material&1) && !(flags&2)) {
                            fog=(fog_far-znear*iz)*inv_fog;
                            if (fog<0) fog=0; if (fog>1) fog=1;
                            if (material&2) fog=fog*fog*(3-2*fog);
                        }
                        float lr=1,lg=1,lb=1;
                        if(!(flags&1)){lr=ar*iz;lg=ag*iz;lb=ab*iz;}
                        uint32_t red=tex_channel(g->unorm[(rgba>>16)&255],lr,fog,(fog_color>>16)&255,black_fog,steps,inv_steps);
                        uint32_t green=tex_channel(g->unorm[(rgba>>8)&255],lg,fog,(fog_color>>8)&255,black_fog,steps,inv_steps);
                        uint32_t blue=tex_channel(g->unorm[rgba&255],lb,fog,fog_color&255,black_fog,steps,inv_steps);
                        uint32_t out=fmt==7?rgb565(red,green,blue):pack(fmt,red,green,blue,bayer[(y&3)*4+(x&3)]);
                        g->zb[at]=z;
                        if (ordered) g->owner[at]=(uint16_t)primitive;
                        if (g->aa || bytes==2) ((uint16_t *)pix)[at]=(uint16_t)out;
                        else pix[at]=(uint8_t)out;
                    }
                }
                az+=dz;au+=du;av+=dv;ar+=dr;ag+=dg;ab+=db;
            }
        }
    }
}

/* Front-to-back ordering reduces expensive shading of hidden texels. Stable
   radix sorting is bounded; original primitive IDs preserve first-wins Z ties.
   Mixed legacy/textured frames retain their original submission order. */
static int order_textured(ftc3d_t *g)
{
#ifdef FTC3D_PROBE
    if (ftc3d_disable_order) return 0;
#endif
    for (uint32_t i=0;i<g->ntris;i++) if (!g->tris[i].pad) return 0;
    for (uint32_t i=0;i<g->ntris;i++) {
        float z=g->tris[i].iz[0]+g->tris[i].iz[1]+g->tris[i].iz[2];
        uint32_t bits;memcpy(&bits,&z,4);g->sort_key[i]=~bits;g->order[i]=(uint16_t)i;
    }
    for (unsigned shift=0;shift<32;shift+=8) {
        uint32_t counts[256]={0},total=0;
        for (uint32_t i=0;i<g->ntris;i++) counts[(g->sort_key[g->order[i]]>>shift)&255]++;
        for (unsigned i=0;i<256;i++) {uint32_t n=counts[i];counts[i]=total;total+=n;}
        for (uint32_t i=0;i<g->ntris;i++) {uint16_t id=g->order[i];g->sort_tmp[counts[(g->sort_key[id]>>shift)&255]++]=id;}
        memcpy(g->order,g->sort_tmp,g->ntris*sizeof g->order[0]);
    }
    return 1;
}

static int render(ftc_t *c)
{
    ftc3d_t *g = &c->g3d;
    if (!g->w)
        return 0;
    uint32_t t0 = c->bus.ms ? c->bus.ms(c->bus.ctx) : 0;
    const int ordered=order_textured(g);
    const uint32_t w = g->w, h = g->h, bytes = fmt_bytes(g->fmt), row = w * bytes;
    uint32_t lines = (FTC3D_STRIP / 2) / (g->aa ? 4 * w : w);      /* у SSAA — по 4 отсчёта */
#ifdef FTC3D_PROBE
    if (ftc3d_strip_lines > 0 && (uint32_t)ftc3d_strip_lines < lines)
        lines = (uint32_t)ftc3d_strip_lines;
#endif
    if (lines > h)
        lines = h;
    uint32_t bg = pack_clear(g->fmt, g->clear);
    const uint16_t bg565 = rgb565((g->clear >> 16) & 0xFFu, (g->clear >> 8) & 0xFFu, g->clear & 0xFFu);
    /* Полосы — в два буфера: пока одна уходит в RAM_G (запись без ожидания, DMA), следующая
       считается; без wr_async — по очереди */
    const int async = c->bus.wr_async && c->bus.wr_wait;
    int cur = 0, pending = 0;
    for (uint32_t ys = 0; ys < h; ys += lines) {
        uint32_t rows = h - ys < lines ? h - ys : lines;
        uint8_t *pix = g->pix[cur];
        if (g->aa) {
            uint16_t *p = (uint16_t *)pix;
            for (uint32_t i = 0; i < 4 * rows * w; i++)
                p[i] = bg565;
            memset(g->zb, 0, 4 * rows * w * sizeof g->zb[0]);
        } else {
            if (bytes == 2) {
                uint16_t *p = (uint16_t *)pix;
                for (uint32_t i = 0; i < rows * w; i++)
                    p[i] = (uint16_t)bg;
            } else {
                memset(pix, (int)bg, rows * w);
            }
            memset(g->zb, 0, rows * w * sizeof g->zb[0]);
        }
        if (ordered) memset(g->owner,255,rows*w*(g->aa?4:1)*sizeof g->owner[0]);
        for (uint32_t i = 0; i < g->ntris; i++) {
            uint32_t id=ordered?g->order[i]:i;
            const ftc3d_tri_t *t = &g->tris[id];
            if (t->y1 >= (int)ys && t->y0 < (int)(ys + rows)) {
                if (t->pad)
                    raster_tex(g, t, (int)ys, (int)rows, pix, &c->bus,id,ordered);
                else if (g->aa)
                    raster_aa(g, t, (int)ys, (int)rows, (uint16_t *)pix, &c->bus);
                else
                    raster(g, t, (int)ys, (int)rows, pix, &c->bus);
            }
        }
        if (g->aa)
            resolve_aa(g, pix, (int)ys, (int)rows);
        if (async) {
            if (pending)
                c->bus.wr_wait(c->bus.ctx);          /* другой буфер свободен */
            c->bus.wr_async(c->bus.ctx, g->addr + ys * row, (const uint32_t *)pix, rows * row / 4);
            pending = 1;
            cur ^= 1;
        } else {
            c->bus.wr(c->bus.ctx, g->addr + ys * row, (const uint32_t *)pix, rows * row / 4);
        }
    }
    if (pending)
        c->bus.wr_wait(c->bus.ctx);
    uint32_t ms = (c->bus.ms ? c->bus.ms(c->bus.ctx) : 0) - t0;
    g->last_tris = g->ntris;
    g->last_ms = ms > 0xFFFFu ? 0xFFFFu : ms;
    return 1;
}

/* --- команды ----------------------------------------------------------------------------------- */
int ftc3d_exec(ftc_t *c)
{
#ifdef FTC3D_DIFFERENTIAL
    if(ftc3d_reference)return ftc3d_exec_reference(c);
#endif
    ftc3d_t *g = &c->g3d;
    const uint32_t *p = c->p;
    float n[12];
    switch (c->cmd) {
    case CMD_3D_TARGET: {
        uint32_t w = p[2] & 0xFFFFu, h = p[2] >> 16, fmt = p[1] & 0xFFu;
        if ((p[1] & ~0x1FFu) || !fmt_ok(fmt) || w < 4 || w > 1024 || (w & 3u) || h < 1 || h > 2048 ||
            (p[0] & 3u) || p[0] >= 0x300000u || p[0] + w * h * fmt_bytes(fmt) > 0x300000u)
            return 0;
        g->addr = p[0];
        g->fmt = fmt;
        g->aa = (p[1] >> 8) & 1u;
        g->w = w;
        g->h = h;
        g->ntris = g->dropped = 0;
        return 1;
    }
    case CMD_3D_PROJECT:
        if ((int32_t)p[0] <= 0 || (int32_t)p[1] <= 0)
            return 0;
        g->focal = fx(p[0]);
        g->znear = fx(p[1]);
        return 1;
    case CMD_3D_IDENTITY:
        identity(g->m);
        return 1;
    case CMD_3D_TRANSLATE:
        identity(n);
        n[3] = fx(p[0]);
        n[7] = fx(p[1]);
        n[11] = fx(p[2]);
        mat_mul(g->m, n);
        return 1;
    case CMD_3D_ROTATE:
        rotate(g->m, p[0], p[1], p[2]);
        return 1;
    case CMD_3D_SCALE:
        identity(n);
        n[0] = fx(p[0]);
        n[5] = fx(p[1]);
        n[10] = fx(p[2]);
        mat_mul(g->m, n);
        return 1;
    case CMD_3D_PUSH:
        if (g->sp < FTC3D_STACK)
            memcpy(g->stack[g->sp++], g->m, sizeof g->m);
        return 1;
    case CMD_3D_POP:
        if (g->sp > 0)
            memcpy(g->m, g->stack[--g->sp], sizeof g->m);
        return 1;
    case CMD_3D_LIGHT:
        set_light(g, (float)(int16_t)p[0], (float)(int16_t)(p[0] >> 16), (float)(int16_t)p[1]);
        g->ambient = (p[1] >> 16) & 0xFFu;
        g->diffuse = p[1] >> 24;
        return 1;
    case CMD_3D_CLEAR:
        g->clear = p[0];
        g->ntris = g->dropped = 0;
        return 1;
    case CMD_3D_MESH:
        return mesh(c);
    case CMD_3D_TEXTURE:
        return texture(c);
    case CMD_3D_TEXMESH:
        return texmesh(c);
    case CMD_3D_MATERIAL:
        if (g->ntris || (p[4] & ~0xff03u) || (int32_t)p[3] < 0 ||
            ((p[4] & 1u) && ((int32_t)p[0] < 0 || (int32_t)p[1] <= (int32_t)p[0]))) return 0;
        g->fog_near = fx(p[0]); g->fog_far = fx(p[1]);
        g->fog_color = p[2]; g->color_steps = fx(p[3]); g->material = p[4];
        return 1;
    case CMD_3D_RENDER:
        return render(c);
    case CMD_3D_INFO:
        ftc_result(c, 0, 0x3D000000u | ((uint32_t)FTC3D_VERSION << 16) | FTC3D_MAX_TRIS);
        ftc_result(c, 1, (g->last_tris & 0xFFFFu) | (g->last_ms << 16));
        return 1;
    default:
        return 0;
    }
}
