/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Проверка прошивки BL616 на ПК: fpga_link.c (шина к ПЛИС) и ядро сопроцессора — поверх
 * побайтовой модели ПЛИС: cp_spi/cp_link (посылки C1…C4, очередь 512 слов, STATUS, буфер
 * чтения, IRQ_N) и ft_core (память FT812, два банка RAM_DL, DLSWAP, REG_INT_FLAGS — ИЛИ).
 * Хост — как Z80 с TSLib: поток Weather 2 в кольцо, сколько есть места. Очередь ПЛИС нарочно
 * медленная (EXEC слов за посылку), чтобы прошивка ждала места в ней и пустой очереди перед
 * снимком для сопроцессора. Итог сверяется с bt8xxemu, как в copro/test/host_test.c.
 *
 *   link_test DATA_DIR RAMG_BIN [EXEC]
 *
 * С -DLINK_MODEL_ONLY — только модель ПЛИС (без main): её же берёт update_test.cpp, для него
 * здесь почтовый ящик cp_link и регистры LINK со стороны ESP (model_link_*).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bflb_dma.h"
#include "bflb_gpio.h"
#include "bflb_l1c.h"
#include "bflb_mtimer.h"
#include "bflb_spi.h"
#include "fpga_link.h"
#include "ft_copro.h"

/* --- память FT812 (ft_core) ------------------------------------------------------------- */
static uint8_t mem[0x400000];
static uint8_t dl_front[8192];
static int swap_left, swap_delay = 3;
static uint32_t resets, info;

static uint32_t peek32(uint32_t a)
{
    return (uint32_t)mem[a] | ((uint32_t)mem[a + 1] << 8) | ((uint32_t)mem[a + 2] << 16) |
           ((uint32_t)mem[a + 3] << 24);
}

static void poke32(uint32_t a, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        mem[a + i] = (uint8_t)(v >> (8 * i));
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

/* запись сопроцессора через порт слов */
static void port_write(uint32_t a, uint32_t v)
{
    if (a == FTC_REG_INT_FLAGS)
        poke32(a, peek32(a) | (v & 0xFF));
    else if (a == FTC_REG_DLSWAP) {
        poke32(a, v & 3);
        swap_left = swap_delay;
    } else if (a < 0x400000)
        poke32(a, v);
}

/* --- cp_link: очередь, буфер чтения, снимок ---------------------------------------------- */
enum { K_ADDR, K_WORD, K_RREQ };
static uint8_t q_kind[512];
static uint32_t q_data[512];
static unsigned q_rd, q_cnt, rd_pend, ovf, exec_per_xact = 8, max_cnt;
static uint32_t waddr, rbuf[512], resets_seen;
static uint32_t mbox[512], mb_page, core_version = 0x01010108u;
static uint8_t snapv[16];
static int model_no_read;         /* ядро без чтения (next_cp ZX Next): запрос чтения не исполняется */

static void push(int kind, uint32_t d)
{
    if (q_cnt == 512) {
        ovf = 1;
        return;
    }
    unsigned i = (q_rd + q_cnt) & 511;
    q_kind[i] = (uint8_t)kind;
    q_data[i] = d;
    q_cnt++;
    if (q_cnt > max_cnt)
        max_cnt = q_cnt;
    if (kind == K_RREQ)
        rd_pend++;
}

static void exec_one(void)
{
    unsigned i = q_rd;
    q_rd = (q_rd + 1) & 511;
    q_cnt--;
    uint32_t d = q_data[i];
    switch (q_kind[i]) {
    case K_ADDR:
        waddr = d & 0x3FFFFCu;
        break;
    case K_WORD:
        if (waddr == 0x3FFFFCu)
            info = d;
        else
            port_write(waddr, d);
        waddr = (waddr + 4) & 0x3FFFFCu;
        break;
    default:
        if (model_no_read)
            break;                 /* запрос остаётся невыполненным: готовности чтения нет */
        for (uint32_t k = 0; k <= (d >> 24); k++) {
            uint32_t a = ((d & 0x3FFFFCu) + 4 * k) & 0x3FFFFCu;
            rbuf[k] = ((d & 0x3FFFFCu) >> 11) == 0x7E0 ? mbox[(((d & 0x7FCu) >> 2) + k) & 511] : peek32(a);
        }
        rd_pend--;
        break;
    }
}

static int ring_pending(void)
{
    uint32_t rd = peek32(FTC_REG_CMD_READ), wr = peek32(FTC_REG_CMD_WRITE);
    return rd != wr && rd != 0xFFF && !(peek32(FTC_REG_CPURESET) & 1);
}

static int model_boot;            /* ядро просит перезагрузку ПЛИС (бит 7 флагов, ядро 1.13) */

static void snapshot(void)
{
    uint32_t wr = peek32(FTC_REG_CMD_WRITE) & 0xFFF, rd = peek32(FTC_REG_CMD_READ) & 0xFFF;
    uint32_t dl = peek32(FTC_REG_CMD_DL) & 0x1FFF, sw = peek32(FTC_REG_DLSWAP) & 3, cr = peek32(FTC_REG_CPURESET) & 7;
    uint32_t fr = 512 - q_cnt;
    uint8_t f = (uint8_t)((rd_pend == 0) | ((q_cnt == 0) << 1) | ((cr & 1) << 2) | (ovf << 3) | (ring_pending() << 4) |
                          ((sw != 0) << 5) | (model_boot << 7));
    uint8_t s[16] = {0xC5, 0x01, f, (uint8_t)resets, (uint8_t)wr, (uint8_t)(wr >> 8), (uint8_t)rd, (uint8_t)(rd >> 8),
                     (uint8_t)dl, (uint8_t)(dl >> 8), (uint8_t)((cr << 2) | sw), 0, (uint8_t)fr, (uint8_t)(fr >> 8), 0, 0};
    memcpy(snapv, s, sizeof s);
    resets_seen = resets;
}

/* --- cp_spi: посылка по байтам ------------------------------------------------------------ */
static int cs_low;
static unsigned bi;
static uint8_t op;
static uint32_t a24, w32;
static unsigned long long xacts, bytes;

static uint8_t spi_byte(uint8_t b)
{
    unsigned k = bi++;
    uint8_t r = 0;
    bytes++;
    if (k == 0) {
        op = b;
        if (op == 0xC3)
            snapshot();
        return 0;
    }
    if (op == 0xC3 && k >= 2 && k < 18)
        r = snapv[k - 2];
    if (op == 0xC4 && k >= 2)
        r = (uint8_t)(rbuf[((k - 2) / 4) & 511] >> (8 * ((k - 2) % 4)));
    if ((op == 0xC1 || op == 0xC2) && k <= 3)
        a24 = (a24 << 8) | b;
    if (op == 0xC1 && k == 3)
        push(K_ADDR, a24 & 0xFFFFFF);
    if (op == 0xC1 && k >= 4) {
        unsigned lane = (k - 4) % 4;
        w32 = (w32 & ~(0xFFu << (8 * lane))) | ((uint32_t)b << (8 * lane));
        if (lane == 3)
            push(K_WORD, w32);
    }
    if (op == 0xC2 && k == 4)
        push(K_RREQ, ((uint32_t)b << 24) | (a24 & 0xFFFFFF));
    return r;
}

/* --- заглушки bouffalo_sdk ------------------------------------------------------------------ */
static struct bflb_device_s dev_gpio, dev_spi;
static uint64_t now_ms;

static struct bflb_device_s dev_dma_tx, dev_dma_rx;
const void *fpga_link_test_dma_src;      /* fpga_link.c: адрес слов DMA (на ПК не помещается в 32 бита) */
static uint32_t dma_bytes, dma_n;
unsigned long long dma_total;

struct bflb_device_s *bflb_device_get_by_name(const char *name)
{
    if (strcmp(name, "dma0_ch0") == 0)
        return &dev_dma_tx;
    if (strcmp(name, "dma0_ch1") == 0)
        return &dev_dma_rx;
    return strcmp(name, "gpio") == 0 ? &dev_gpio : &dev_spi;
}

/* DMA: запуск канала передачи сразу прогоняет байты через модель ПЛИС */
void bflb_dma_channel_init(struct bflb_device_s *d, const struct bflb_dma_channel_config_s *c) { (void)d; (void)c; }
int bflb_dma_channel_lli_reload(struct bflb_device_s *d, struct bflb_dma_channel_lli_pool_s *p, uint32_t ps,
                                struct bflb_dma_channel_lli_transfer_s *t, uint32_t n)
{
    (void)p;
    (void)ps;
    (void)n;
    if (d == &dev_dma_tx)
        dma_bytes = t->nbytes;
    return 0;
}
void bflb_dma_channel_start(struct bflb_device_s *d)
{
    if (d != &dev_dma_tx)
        return;
    if (!cs_low) {
        fprintf(stderr, "DMA без CS\n");
        exit(2);
    }
    const uint8_t *b = (const uint8_t *)fpga_link_test_dma_src;
    for (uint32_t i = 0; i < dma_bytes; i++)
        spi_byte(b[i]);
    dma_total += dma_bytes;
    dma_n++;
}
void bflb_dma_channel_stop(struct bflb_device_s *d) { (void)d; }
bool bflb_dma_channel_isbusy(struct bflb_device_s *d) { (void)d; return false; }
void bflb_spi_link_txdma(struct bflb_device_s *d, bool e) { (void)d; (void)e; }
void bflb_spi_link_rxdma(struct bflb_device_s *d, bool e) { (void)d; (void)e; }
void bflb_l1c_dcache_clean_range(void *a, uint32_t n) { (void)a; (void)n; }
void bflb_gpio_init(struct bflb_device_s *d, uint8_t pin, uint32_t cfg) { (void)d; (void)pin; (void)cfg; }
void bflb_gpio_reset(struct bflb_device_s *d, uint8_t pin)
{
    (void)d;
    if (pin == GPIO_PIN_0) {
        if (cs_low) {
            fprintf(stderr, "CS уже низкий\n");
            exit(2);
        }
        cs_low = 1;
        bi = 0;
    }
}
void bflb_gpio_set(struct bflb_device_s *d, uint8_t pin)
{
    (void)d;
    if (pin == GPIO_PIN_0 && cs_low) {
        cs_low = 0;
        xacts++;
        for (unsigned i = 0; i < exec_per_xact && q_cnt; i++)
            exec_one();
        frame_tick();
    }
}
bool bflb_gpio_read(struct bflb_device_s *d, uint8_t pin)
{
    (void)d;
    if (pin == GPIO_PIN_13)
        return !(ring_pending() || resets != resets_seen);
    return 1;
}
void bflb_spi_init(struct bflb_device_s *d, const struct bflb_spi_config_s *c) { (void)d; (void)c; }
int bflb_spi_feature_control(struct bflb_device_s *d, int cmd, size_t arg) { (void)d; (void)cmd; (void)arg; return 0; }
int bflb_spi_poll_exchange(struct bflb_device_s *d, const void *tx, void *rx, size_t n)
{
    (void)d;
    if (!cs_low) {
        fprintf(stderr, "обмен SPI без CS\n");
        exit(2);
    }
    for (size_t i = 0; i < n; i++) {
        uint8_t r = spi_byte(tx ? ((const uint8_t *)tx)[i] : 0xFF);
        if (rx)
            ((uint8_t *)rx)[i] = r;
    }
    return 0;
}
uint64_t bflb_mtimer_get_time_ms(void) { return now_ms++; }
uint64_t bflb_mtimer_get_time_us(void) { return 1000 * now_ms++; }
void bflb_mtimer_delay_ms(uint32_t ms) { now_ms += ms; }

/* --- регистры LINK со стороны ESP (ts_fb_core) ---------------------------------------------- */
uint32_t model_link_read(uint32_t a)
{
    switch (a) {
    case 0x04: return core_version;
    case 0x30: return info;
    case 0x34: return mb_page;
    default: return 0;
    }
}

void model_link_write(uint32_t a, uint32_t v)
{
    if (a == 0x34)
        mb_page = v & 3;
    else if (a >= 0x200 && a <= 0x3FC)
        mbox[mb_page * 128 + (a - 0x200) / 4] = v;
}

void model_set_version(uint32_t v)
{
    core_version = v;
}

uint32_t model_mbox(uint32_t i)
{
    return mbox[i & 511];
}

/* ядро просит перезагрузку ПЛИС (бит 7 флагов STATUS): для cache_test.cpp */
void model_set_boot(int on)
{
    model_boot = on != 0;
}

#ifndef LINK_MODEL_ONLY
/* --- Weather 2 ------------------------------------------------------------------------------ */
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

static ftc_t cop;

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "link_test DATA_DIR RAMG_BIN [EXEC]\n");
        return 2;
    }
    if (argc > 3)
        exec_per_xact = (unsigned)atoi(argv[3]);
    char p[1024];
    size_t ring_n, dl_n, ramg_n, regs_n;
    snprintf(p, sizeof p, "%s/weather2_ring.bin", argv[1]);
    uint8_t *ring = load(p, &ring_n);
    snprintf(p, sizeof p, "%s/weather2_dl.bin", argv[1]);
    uint8_t *dl = load(p, &dl_n);
    snprintf(p, sizeof p, "%s/weather2_regs.txt", argv[1]);
    char *regs = (char *)load(p, &regs_n);
    uint8_t *ramg = load(argv[2], &ramg_n);
    unsigned g_rd, g_wr, g_dl, g_int;
    if (sscanf(regs, "%u %u %u %u", &g_rd, &g_wr, &g_dl, &g_int) != 4)
        return 2;

    if (fpga_link_init() != 0) {
        printf("ПЛИС не ответила меткой STATUS\n");
        return 1;
    }
    ftc_bus_t bus;
    fpga_bus(&bus);
    ftc_init(&cop, &bus);

    /* хост пишет кольцо (у него свой путь — SPI Evo), прошивка крутит свой цикл */
    size_t pos = 0;
    long iter = 0;
    int err = 0, busy = 1;
    uint32_t beat = 0;
    for (;;) {
        uint32_t wp = peek32(FTC_REG_CMD_WRITE), rp = peek32(FTC_REG_CMD_READ);
        if (rp == 0xFFF) {
            printf("сопроцессор: ошибка %d\n", cop.fault);
            err = 1;
            break;
        }
        uint32_t space = (4092 - ((wp - rp) & 0xFFF)) & 0xFFC;
        size_t k = ring_n - pos < space ? ring_n - pos : space;
        for (size_t i = 0; i < k; i++)
            mem[FTC_RAM_CMD + ((wp + i) & 0xFFF)] = ring[pos + i];
        pos += k;
        poke32(FTC_REG_CMD_WRITE, (wp + (uint32_t)k) & 0xFFF);

        /* то же, что цикл main.c */
        busy = ftc_poll(&cop);
        if ((iter & 63) == 0)
            fpga_info(0xC0010000u | (beat++ & 0xFF));
        if (!busy)
            for (int w = 0; w < 10 && !fpga_irq(); w++)
                frame_tick();
        if (pos == ring_n && !busy && !fpga_irq() && q_cnt == 0 &&
            peek32(FTC_REG_CMD_READ) == peek32(FTC_REG_CMD_WRITE))
            break;
        if (++iter > 5000000) {
            printf("сопроцессор не закончил\n");
            err = 1;
            break;
        }
    }
    for (int i = 0; i < 10; i++)
        frame_tick();

    size_t dl_bad = 0, g_bad = 0;
    for (size_t i = 0; i < dl_n; i++)
        dl_bad += mem[FTC_RAM_DL + i] != dl[i];
    for (size_t i = 0; i < ramg_n; i++)
        g_bad += mem[i] != ramg[i];
    uint32_t rd = peek32(FTC_REG_CMD_READ), wr = peek32(FTC_REG_CMD_WRITE), cdl = peek32(FTC_REG_CMD_DL);
    uint32_t intf = peek32(FTC_REG_INT_FLAGS);
    printf("Weather 2 через fpga_link (очередь ПЛИС — %u слов за посылку): RAM_DL расхождений %zu, "
           "RAM_G %zu; CMD_READ %u/%u CMD_WRITE %u/%u CMD_DL %u/%u INT_CMDEMPTY %u\n",
           exec_per_xact, dl_bad, g_bad, rd, g_rd, wr, g_wr, cdl, g_dl, (intf >> 5) & 1);
    printf("посылок SPI %llu, байтов %llu (%.2f с на 26,67 МГц), ожиданий %u; очередь до %u слов, "
           "переполнение %u; слово состояния %08x\n",
           xacts, bytes, bytes * 8 / 26.67e6, fpga_waits, max_cnt, ovf, info);
    err |= dl_bad || g_bad || rd != g_rd || wr != g_wr || cdl != g_dl || !((intf >> 5) & 1) || ovf ||
           (info >> 24) != 0xC0;

    /* запрос перезагрузки ПЛИС: бит 7 флагов STATUS — fpga_boot_requested() */
    model_boot = 1;
    int boot_on = fpga_link_init() == 0 && fpga_boot_requested();
    model_boot = 0;
    int boot_off = fpga_link_init() == 0 && !fpga_boot_requested();
    printf("запрос перезагрузки ПЛИС в STATUS: %s\n", boot_on && boot_off ? "виден и снимается" : "НЕТ");
    printf("запись: DMA %u посылок, %llu байтов; частота записи %u МГц (проверка первых посылок)\n", dma_n,
           dma_total, fpga_write_hz() / 1000000u);
    err |= dma_n == 0 || fpga_write_hz() != 40000000u;
    err |= !(boot_on && boot_off);
    /* самопроверка частот (прошивка 9): у модели ошибок нет — запись 80 МГц, отчёт в RAM_G */
    int spd = fpga_speed_test();
    uint32_t rep[16];
    for (int i = 0; i < 16; i++)
        rep[i] = peek32(FPGA_REPORT_ADDR + 4u * (uint32_t)i);
    uint32_t rep2[32];
    for (int i = 0; i < 32; i++)
        rep2[i] = peek32(FPGA_REPORT_ADDR + 4u * (uint32_t)i);
    int rep_ok = rep2[0] == FPGA_REPORT_MAGIC && rep2[1] == 80000000u && rep2[2] == 40000000u && rep2[3] == 3 &&
                 !rep2[4] && !rep2[9] && !rep2[14] && !rep2[16] && !rep2[18] && !rep2[20] && !rep2[22] && !rep2[24];
    (void)rep;
    printf("самопроверка частот: код %d, запись %u МГц, отчёт %s\n", spd, fpga_write_hz() / 1000000u,
           rep_ok ? "да" : "НЕТ");
    err |= spd != 3 || fpga_write_hz() != 80000000u || !rep_ok;
    /* ядро без чтения (ZX Next из кэша: снимок есть, готовности чтения нет никогда) — чтение не
       встаёт навсегда: нули и счёт брошенных (прошивка 15; разбор Codex 21.10.2026, № 2) */
    model_no_read = 1;
    uint32_t got[4] = {1, 2, 3, 4};
    const uint32_t stuck_before = fpga_stuck();
    bus.rd(bus.ctx, FTC_RAM_DL, got, 4);
    const int stuck_ok = (got[0] | got[1] | got[2] | got[3]) == 0 && fpga_stuck() == stuck_before + 1;
    printf("чтение у ядра без чтения: %s\n", stuck_ok ? "брошено, нули" : "НЕ ВЕРНУЛОСЬ ВЕРНО");
    err |= !stuck_ok;
    printf(err ? "ТЕСТ НЕ ПРОЙДЕН\n" : "ТЕСТ ПРОЙДЕН\n");
    return err;
}
#endif /* LINK_MODEL_ONLY */
