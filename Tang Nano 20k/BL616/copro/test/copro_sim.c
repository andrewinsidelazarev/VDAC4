/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Сопроцессор BL616 (ft_copro.c) для стенда хоста на ПК — DLL с моделью памяти и регистров FT812,
 * как их видит хост через ядро FT VDAC4 (fpga/vdac2/rtl/ft_core.sv): RAM_G, RAM_DL, кольцо и
 * REG_CMDB_WRITE (каждые 4 байта посылки — слово в кольцо), REG_CMDB_SPACE (формула ядра),
 * REG_CPURESET (1 → 0 — счётчик сбросов), REG_DLSWAP (смена листа через несколько опросов),
 * REG_MEDIAFIFO_READ/WRITE (память). Рисования нет — кадры проверяются по RAM_G.
 *
 * Стенд плагина ftview (tests/test_vdac4.py, VDAC4_COPRO_DLL) подставляет её вместо bt8xxemu:
 * модель Z80 пишет и читает через sim_write/sim_read, а сопроцессор идёт вперёд на каждом
 * обращении (как отдельный процессор BL616 рядом с Z80).
 *
 *   clang -shared -O2 -I.. copro_sim.c ../ft_copro.c ../ft_image.c ../ft_inflate.c ../ft_3d.c -o copro_sim.dll
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ft_copro.h"

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

#define REG_CMDB_SPACE 0x302574u
#define REG_CMDB_WRITE 0x302578u

static uint8_t mem[0x400000];
static uint8_t dl_front[8192];
static ftc_t cop;
static uint32_t resets, ticks, swap_left, cmdb_word, cmdb_n;
static int polls_per_access = 40;

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

static void b_rd(void *ctx, uint32_t a, uint32_t *w, uint32_t n)
{
    (void)ctx;
    for (uint32_t i = 0; i < n; i++)
        w[i] = peek32((a + 4 * i) & 0x3FFFFCu);
}

static void b_wr(void *ctx, uint32_t a, const uint32_t *w, uint32_t n)
{
    (void)ctx;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ad = (a + 4 * i) & 0x3FFFFCu;
        poke32(ad, w[i]);
        if (ad == FTC_REG_DLSWAP)
            swap_left = 3;
    }
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

static uint32_t b_ms(void *ctx)
{
    (void)ctx;
    return ticks / 16;
}

static void tick(void)
{
    ticks++;
    if (peek32(FTC_REG_DLSWAP) && swap_left && --swap_left == 0) {
        memcpy(dl_front, mem + FTC_RAM_DL, sizeof dl_front);
        poke32(FTC_REG_DLSWAP, 0);
    }
}

EXPORT int sim_poll(int n)
{
    int busy = 0;
    while (n-- > 0) {
        busy = ftc_poll(&cop);
        tick();
    }
    return busy;
}

EXPORT void sim_init(int per_access)
{
    memset(mem, 0, sizeof mem);
    memset(dl_front, 0, sizeof dl_front);
    resets = ticks = swap_left = cmdb_word = cmdb_n = 0;
    polls_per_access = per_access > 0 ? per_access : 40;
    ftc_bus_t bus = {0};
    bus.rd = b_rd;
    bus.wr = b_wr;
    bus.status = b_status;
    bus.irq = b_irq;
    bus.ms = b_ms;
    ftc_init(&cop, &bus);
}

/* Посылка хоста: адрес растёт, кроме REG_CMDB_WRITE (порт кольца). */
EXPORT void sim_write(uint32_t a, const uint8_t *d, uint32_t n)
{
    a &= 0x3FFFFFu;
    if (a == REG_CMDB_WRITE) {
        for (uint32_t i = 0; i < n; i++) {
            cmdb_word |= (uint32_t)d[i] << (8 * cmdb_n);
            if (++cmdb_n == 4) {
                uint32_t wp = peek32(FTC_REG_CMD_WRITE) & 0xFFCu;
                poke32(FTC_RAM_CMD + wp, cmdb_word);
                poke32(FTC_REG_CMD_WRITE, (wp + 4) & 0xFFCu);
                cmdb_word = cmdb_n = 0;
            }
        }
        cmdb_word = cmdb_n = 0;                 /* неполное слово в конце посылки — прочь */
    } else {
        uint32_t was = peek32(FTC_REG_CPURESET) & 1u;
        for (uint32_t i = 0; i < n; i++)
            mem[(a + i) & 0x3FFFFFu] = d[i];
        if (was && !(peek32(FTC_REG_CPURESET) & 1u))
            resets++;                           /* сброс сопроцессора кончился */
        if (a <= FTC_REG_DLSWAP && FTC_REG_DLSWAP < a + n && peek32(FTC_REG_DLSWAP))
            swap_left = 3;                      /* смену листа заказал хост */
    }
    sim_poll(polls_per_access);
}

EXPORT void sim_read(uint32_t a, uint8_t *out, uint32_t n)
{
    sim_poll(polls_per_access);
    a &= 0x3FFFFFu;
    if (a == REG_CMDB_SPACE && n <= 4) {
        uint32_t r = peek32(FTC_REG_CMD_READ), w = peek32(FTC_REG_CMD_WRITE);
        uint32_t v = (0xFFCu - ((w - r) & 0xFFFu)) & 0xFFCu;
        for (uint32_t i = 0; i < n; i++)
            out[i] = (uint8_t)(v >> (8 * i));
        return;
    }
    for (uint32_t i = 0; i < n; i++)
        out[i] = mem[(a + i) & 0x3FFFFFu];
}

/* Образ ПЗУ FT812 0x1E0000…0x2FFFFF (шрифты и ROM_FONTROOT, снят с bt8xxemu): без него
   CMD_TEXT и CMD_NUMBER ждут шрифтов, как сопроцессор на плате до заливки образа с ESP. */
EXPORT int sim_rom(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t n = fread(mem + 0x1E0000u, 1, 0x120000u, f);
    fclose(f);
    return n == 0x120000u;
}

EXPORT int sim_fault(void)
{
    return cop.fault;
}

EXPORT void sim_dl_front(uint8_t *out)
{
    memcpy(out, dl_front, sizeof dl_front);
}
