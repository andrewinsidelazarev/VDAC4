/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Связь BL616 с ПЛИС Tang Nano 20K (rev 3923): SPI0, BL616 — ведущий, режим 0. Посылки —
 * fpga/vdac2/rtl/cp_spi.sv:
 *   C1 a2 a1 a0 слова…  запись слов подряд (в очередь ПЛИС);
 *   C2 a2 a1 a0 n       прочитать n + 1 слов в буфер чтения (через ту же очередь — по порядку);
 *   C3 xx s0…s15        снимок состояния (cp_link): метка, флаги, REG_CMD_*, место в очереди;
 *   C4 xx слова…        буфер чтения.
 * STATUS не стоит в очереди: состояние для сопроцессора берётся, когда очередь пуста, — иначе
 * CMD_DLSTART не увидел бы своей же записи REG_DLSWAP. Если метки STATUS не было (ПЛИС
 * перезагружается — например, внешнее ядро и обратно), состояние ПЛИС после неё — новое:
 * сопроцессору это сообщается как REG_CPURESET = 1, и он начинает заново.
 *
 * Частоты (прошивка 4). Делитель SDK — «половина такта SPI / частота − 1», так что запрос
 * 20 МГц — это 20 МГц (прежние «26,67» — ошибка SPI_CMD_GET_FREQ в SDK, она не прибавляет
 * единицу к фазам). Чтение и снимки — 20 МГц: MISO идёт через логику ПЛИС, на 40 МГц у него
 * полтакта — 12,5 нс. Запись — 40 МГц: MOSI ПЛИС берёт по фронту SCK. Первые CHECK_CHUNKS
 * посылок в RAM_G после включения проверяются чтением последнего слова; не сошлось — запись
 * навсегда на 20 МГц, посылка повторяется.
 *
 * Самопроверка (прошивка 9, fpga_speed_test): когда ядро FT812 загружено и сопроцессор
 * свободен, образец 64 КБ пишется в RAM_G с TEST_ADDR на 80 МГц (с паузами между кадрами SPI
 * и в непрерывном режиме — SPI_CMD_SET_CS_INTERVAL) и на 40 и читается на 20 МГц; запись — на
 * самой высокой частоте без ошибок (проверка первых посылок после неё — как была, неудача —
 * 20 МГц). Слова чтения (C4) — кадрами по 32 бита и на 40 МГц, если без ошибок на 40 и на
 * 53,3 МГц (период 3 такта, та же низкая фаза 12,5 нс, что у 40); снимки и запросы чтения —
 * всегда 20 МГц: по снимку решается, не перезагружалась ли ПЛИС. Отчёт — 32 слова в RAM_G с
 * FPGA_REPORT_ADDR (fpga_link.h).
 *
 * Запись от DMA_MIN слов — каналом DMA (dma0_ch0 → SPI0, ширина 32 бита: байты слова уходят
 * младшим вперёд, как в памяти), принятое в это время — каналом dma0_ch1 в одно слово-корзину
 * (иначе переполнилось бы FIFO приёма). Запись может идти без ожидания (wr_async): посылки по
 * до WRITE_CHUNK слов сменяют друг друга в fpga_pump, а процессор тем временем считает (3D рисует
 * следующую полосу, ft_3d.c). Любой другой обмен сначала дожидается её конца.
 *
 * Выводы BL616 (Tang Nano 20K rev 3923, как в FPGA Companion): CS GPIO0, SCK GPIO1,
 * MOSI GPIO27, MISO GPIO30, IRQ GPIO13 (низкий — есть работа). JTAG (GPIO 10, 12, 14, 16) —
 * FPGA Partner, на время перезагрузки ПЛИС — fpga_jtag.c.
 */
#include "bflb_dma.h"
#include "bflb_gpio.h"
#include "bflb_l1c.h"
#include "bflb_mtimer.h"
#include "bflb_spi.h"

#include "fpga_link.h"

#include <string.h>

#define PIN_CS   GPIO_PIN_0
#define PIN_SCK  GPIO_PIN_1
#define PIN_MOSI GPIO_PIN_27
#define PIN_MISO GPIO_PIN_30
#define PIN_IRQ  GPIO_PIN_13

#define OP_WRITE  0xC1
#define OP_RREQ   0xC2
#define OP_STATUS 0xC3
#define OP_RDATA  0xC4

#define ST_RD_READY 0x01
#define ST_IDLE     0x02
#define ST_OVERFLOW 0x08
#define ST_BOOT     0x80          /* ядро просит перезагрузку ПЛИС (ядро 1.13+, fpga_jtag.c) */

#define INFO_ADDR 0x3FFFFCu
#define READ_CHUNK  256u           /* READ_REQ carries an 8-bit word count minus one */
#define WRITE_CHUNK 511u           /* 512 queue events: address plus data words */

#define READ_HZ      20000000u
#define WRITE_HZ     40000000u
#define DMA_MIN      16u           /* слов: короче — опросом */
#define CHECK_CHUNKS 64u

static struct bflb_device_s *gpio, *spi, *dma_tx, *dma_rx;
#ifdef FPGA_LINK_HOST_TEST
extern const void *fpga_link_test_dma_src;
#endif
static struct bflb_dma_channel_lli_pool_s lli_tx[1], lli_rx[1];
static volatile uint32_t rx_sink;
static uint32_t free_words;        /* место в очереди ПЛИС по последнему снимку */
static int lost;                   /* метки STATUS не было — ПЛИС перезагружалась */
static int boot_req;               /* в последнем снимке — запрос перезагрузки ПЛИС */
static uint32_t cur_hz, write_hz = WRITE_HZ, checked;
static uint32_t rdata_hz = READ_HZ;  /* слова чтения (C4); снимки и запросы — всегда READ_HZ */
static uint32_t rdata_ph;            /* не 0 — слова чтения на фазах PRD_0 {ph1, ph0} (53,3 МГц) */
static int rd32_on;                  /* слова чтения — кадрами по 32 бита */
/* замеры самопроверки: время DMA, ожидания места в очереди ПЛИС, снимков в ожидании */
static int meas;
static uint32_t m_dma, m_wait, m_snaps;
/* простой шины для замера RENDER (прошивка 26): ожидание места в очереди ПЛИС и чтения, мкс */
static uint32_t link_room_us, link_read_us, link_read_chunks, link_read_snaps;
static uint64_t m_t0;
uint32_t fpga_xacts, fpga_waits;

/* запись без ожидания: что ещё не начато и посылка в работе */
static struct {
    uint32_t a, n;
    const uint32_t *w;
    int busy;                      /* посылка DMA идёт */
    uint32_t ca, cn;
    const uint32_t *cw;
} aw;

typedef struct {
    uint8_t flags, resets;
    uint16_t cmd_write, cmd_read, cmd_dl;
    uint8_t swap_cpur;
    uint16_t free;
} snap_t;

static void cs(int on)
{
    if (on)
        bflb_gpio_reset(gpio, PIN_CS);
    else
        bflb_gpio_set(gpio, PIN_CS);
}

static void speed(uint32_t hz)
{
    if (hz != cur_hz) {
        bflb_spi_feature_control(spi, SPI_CMD_SET_FREQ, hz);
        cur_hz = hz;
    }
}

/* частота слов чтения: rdata_hz или (самопроверка) фазы PRD_0 напрямую — несимметричный период */
static void rdata_speed(void)
{
#ifndef FPGA_LINK_HOST_TEST
    if (rdata_ph) {
        *(volatile uint32_t *)(uintptr_t)(spi->reg_base + 0x10u) = rdata_ph << 16;   /* SPI_PRD_0 */
        cur_hz = 0;
        return;
    }
#endif
    speed(rdata_hz);
}

static int snap(snap_t *s)
{
    uint8_t tx[18] = {OP_STATUS}, rx[18];
    speed(READ_HZ);
    cs(1);
    bflb_spi_poll_exchange(spi, tx, rx, sizeof rx);
    cs(0);
    fpga_xacts++;
    const uint8_t *b = rx + 2;
    if (b[0] != 0xC5 || b[1] != 0x01) {
        lost = 1;
        boot_req = 0;
        return -1;
    }
    s->flags = b[2];
    s->resets = b[3];
    s->cmd_write = (uint16_t)(b[4] | (b[5] << 8));
    s->cmd_read = (uint16_t)(b[6] | (b[7] << 8));
    s->cmd_dl = (uint16_t)(b[8] | (b[9] << 8));
    s->swap_cpur = b[10];
    s->free = (uint16_t)(b[12] | (b[13] << 8));
    free_words = s->free;
    boot_req = (s->flags & ST_BOOT) != 0;
    return 0;
}

/* снимок, когда очередь ПЛИС пуста: всё записанное до него уже в ft_core */
/* Годных снимков подряд без готовности чтения (места в очереди) — ядро не исполняет запрос:
   так отвечает ядро из кэша (next_cp ZX Next — только снимок STATUS, без чтения и ящика). Чтение
   — нули, запись — дальше без места: BL616 не встаёт навсегда (обработка BOOT, возврат). */
#define STUCK_SNAPS 20000u
static uint32_t stuck_reads, stuck_writes;

static void snap_idle(snap_t *s)
{
    for (uint32_t n = 0; n < STUCK_SNAPS; n++) {
        if (snap(s) == 0 && (s->flags & ST_IDLE))
            return;
        fpga_waits++;
    }
}

static void wait_room(uint32_t n);

/* Снимков подряд с чужим заголовком — связи с ПЛИС нет (перезагрузка): чтение — нули */
#define LOST_SNAPS 1000u

/* чтение n ≤ READ_CHUNK слов (без ожидания записи без ожидания — её здесь нет). Запрос чтения — тоже
   место в очереди ПЛИС (cp_link: событие = место); готовность — только по годному снимку */
static void rd_chunk(uint32_t a, uint32_t *w, uint32_t k)
{
    uint8_t req[5] = {OP_RREQ, (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a, (uint8_t)(k - 1)};
    wait_room(1);
    speed(READ_HZ);
    cs(1);
    bflb_spi_poll_exchange(spi, req, NULL, 5);
    cs(0);
    fpga_xacts++;
    free_words = free_words ? free_words - 1 : 0;
    snap_t s;
    uint32_t bad = 0, good = 0;
    link_read_chunks++;
    for (;;) {
        fpga_waits++;
        link_read_snaps++;
        if (snap(&s) == 0) {
            bad = 0;
            if (s.flags & ST_RD_READY)
                break;
            if (++good >= STUCK_SNAPS) {
                stuck_reads++;
                memset(w, 0, 4 * k);
                return;
            }
        } else if (++bad >= LOST_SNAPS) {
            memset(w, 0, 4 * k);
            return;
        }
    }
    uint8_t hdr[2] = {OP_RDATA, 0};
    speed(READ_HZ);
    cs(1);
    bflb_spi_poll_exchange(spi, hdr, NULL, 2);
    rdata_speed();
    if (rd32_on) {
        bflb_spi_feature_control(spi, SPI_CMD_SET_DATA_WIDTH, SPI_DATA_WIDTH_32BIT);
        bflb_spi_poll_exchange(spi, NULL, w, 4 * k);
        bflb_spi_feature_control(spi, SPI_CMD_SET_DATA_WIDTH, SPI_DATA_WIDTH_8BIT);
    } else {
        bflb_spi_poll_exchange(spi, NULL, w, 4 * k);
    }
    cs(0);
    fpga_xacts++;
}

static void chunk_poll(uint32_t a, const uint32_t *w, uint32_t n);

/* посылка дошла: учёт места (адрес посылки — тоже место в очереди ПЛИС, как и каждое слово) и
   проверка первых посылок на 40 МГц */
static void chunk_done(uint32_t a, const uint32_t *w, uint32_t n)
{
    fpga_xacts++;
    free_words = free_words >= n + 1 ? free_words - (n + 1) : 0;
    if (write_hz != READ_HZ && checked < CHECK_CHUNKS && a < 0x300000u && n >= DMA_MIN) {
        checked++;
        uint32_t back = 0;
        rd_chunk(a + 4 * (n - 1), &back, 1);
        if (back != w[n - 1]) {
            write_hz = READ_HZ;                /* 40 МГц не годится: дальше 20, посылку — снова */
            chunk_poll(a, w, n);
        }
    }
}

static void wait_room(uint32_t n)
{
    snap_t s;
    const int waiting = free_words < n;
    const uint64_t w0 = waiting ? bflb_mtimer_get_time_us() : 0;
    uint64_t t0 = meas ? bflb_mtimer_get_time_us() : 0;
    for (uint32_t tries = 0; free_words < n; tries++) {
        if (tries >= STUCK_SNAPS) {
            stuck_writes++;        /* места нет и не будет (ПЛИС пропала, чужое ядро) */
            free_words = n;
            break;
        }
        snap(&s);
        fpga_waits++;
        m_snaps += (uint32_t)meas;
    }
    if (waiting)
        link_room_us += (uint32_t)(bflb_mtimer_get_time_us() - w0);
    if (meas)
        m_wait += (uint32_t)(bflb_mtimer_get_time_us() - t0);
}

/* Заголовок — на READ_HZ (с прошивки 9): искажённый на высокой частоте адрес увёл бы запись в
   чужую память или регистры FT812; слова — на write_hz (CS держится, SCK меняется между ними). */
static void header(uint32_t a)
{
    uint8_t hdr[4] = {OP_WRITE, (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a};
    speed(READ_HZ);
    cs(1);
    bflb_spi_poll_exchange(spi, hdr, NULL, 4);
    speed(write_hz);
}

/* посылка опросом — целиком */
static void chunk_poll(uint32_t a, const uint32_t *w, uint32_t n)
{
    wait_room(n + 1);                                 /* адрес и n слов */
    header(a);
    bflb_spi_poll_exchange(spi, w, NULL, 4 * n);   /* слова — младшим байтом вперёд, как в памяти */
    cs(0);
    chunk_done(a, w, n);
}

/* посылка DMA — начать (кончится в fpga_pump) */
static void chunk_dma(uint32_t a, const uint32_t *w, uint32_t n)
{
    wait_room(n + 1);                                 /* адрес и n слов */
    header(a);
    bflb_spi_feature_control(spi, SPI_CMD_SET_DATA_WIDTH, SPI_DATA_WIDTH_32BIT);
    bflb_spi_link_txdma(spi, true);
    bflb_spi_link_rxdma(spi, true);
    bflb_l1c_dcache_clean_range((void *)w, 4 * n);
#ifdef FPGA_LINK_HOST_TEST
    fpga_link_test_dma_src = w;          /* проверка на ПК: адрес не помещается в 32 бита */
#endif
    struct bflb_dma_channel_lli_transfer_s tx = {(uint32_t)(uintptr_t)w, (uint32_t)DMA_ADDR_SPI0_TDR, 4 * n};
    struct bflb_dma_channel_lli_transfer_s rx = {(uint32_t)DMA_ADDR_SPI0_RDR, (uint32_t)(uintptr_t)&rx_sink, 4 * n};
    bflb_dma_channel_lli_reload(dma_rx, lli_rx, 1, &rx, 1);
    bflb_dma_channel_lli_reload(dma_tx, lli_tx, 1, &tx, 1);
    bflb_dma_channel_start(dma_rx);
    bflb_dma_channel_start(dma_tx);
    if (meas)
        m_t0 = bflb_mtimer_get_time_us();
    aw.busy = 1;
    aw.ca = a;
    aw.cw = w;
    aw.cn = n;
}

void fpga_pump(void)
{
    if (aw.busy) {
        if (bflb_dma_channel_isbusy(dma_rx) || bflb_dma_channel_isbusy(dma_tx))
            return;                            /* приём кончается с последним битом посылки */
        if (meas)
            m_dma += (uint32_t)(bflb_mtimer_get_time_us() - m_t0);
        bflb_dma_channel_stop(dma_tx);
        bflb_dma_channel_stop(dma_rx);
        bflb_spi_link_txdma(spi, false);
        bflb_spi_link_rxdma(spi, false);
        bflb_spi_feature_control(spi, SPI_CMD_SET_DATA_WIDTH, SPI_DATA_WIDTH_8BIT);
        bflb_spi_feature_control(spi, SPI_CMD_CLEAR_RX_FIFO, 0);
        cs(0);
        aw.busy = 0;
        chunk_done(aw.ca, aw.cw, aw.cn);
    }
    while (!aw.busy && aw.n) {
        /* Start once a useful packet fits, then consume available capacity.
           Do not wait for an entirely empty queue just to send a larger packet. */
        uint32_t minimum=aw.n<DMA_MIN?aw.n:DMA_MIN;
        wait_room(minimum+1);
        uint32_t k=aw.n<WRITE_CHUNK?aw.n:WRITE_CHUNK;
        if(k>=free_words)k=free_words-1;
        const uint32_t *w = aw.w;
        uint32_t a = aw.a;
        aw.a += 4 * k;
        aw.w += k;
        aw.n -= k;
        if (k >= DMA_MIN && dma_tx && dma_rx)
            chunk_dma(a, w, k);
        else
            chunk_poll(a, w, k);
    }
}

void fpga_wr_wait(void)
{
    while (aw.busy || aw.n)
        fpga_pump();
}

static void b_wr_async(void *ctx, uint32_t a, const uint32_t *w, uint32_t n)
{
    (void)ctx;
    fpga_wr_wait();
    aw.a = a;
    aw.w = w;
    aw.n = n;
    fpga_pump();
}

static void b_pump(void *ctx)
{
    (void)ctx;
    fpga_pump();
}

static void b_wr_wait(void *ctx)
{
    (void)ctx;
    fpga_wr_wait();
}

static void b_wr(void *ctx, uint32_t a, const uint32_t *w, uint32_t n)
{
    b_wr_async(ctx, a, w, n);
    fpga_wr_wait();
}

static void b_rd(void *ctx, uint32_t a, uint32_t *w, uint32_t n)
{
    (void)ctx;
    fpga_wr_wait();
    const uint64_t t0 = bflb_mtimer_get_time_us();
    while (n) {
        uint32_t k = n < READ_CHUNK ? n : READ_CHUNK;
        rd_chunk(a, w, k);
        a += 4 * k;
        w += k;
        n -= k;
    }
    link_read_us += (uint32_t)(bflb_mtimer_get_time_us() - t0);
}

static void b_status(void *ctx, ftc_status_t *st)
{
    (void)ctx;
    fpga_wr_wait();
    snap_t s;
    snap_idle(&s);
    st->cmd_write = s.cmd_write;
    st->cmd_read = s.cmd_read;
    st->cmd_dl = s.cmd_dl;
    st->dlswap = s.swap_cpur & 3u;
    st->cpureset = (s.swap_cpur >> 2) & 7u;
    st->resets = s.resets;
    if (lost) {                        /* ПЛИС перезагружалась: для сопроцессора — сброс */
        lost = 0;
        st->cpureset |= 1u;
    }
}

static void b_irq(void *ctx, uint32_t flags)
{
    b_wr(ctx, FTC_REG_INT_FLAGS, &flags, 1);
}

static uint32_t b_ms(void *ctx)
{
    (void)ctx;
    return (uint32_t)bflb_mtimer_get_time_ms();
}

static uint32_t b_us(void *ctx)
{
    (void)ctx;
    return (uint32_t)bflb_mtimer_get_time_us();
}

static void b_link_us(void *ctx, uint32_t link[5])
{
    (void)ctx;
    link[0] = link_room_us;
    link[1] = link_read_us;
    link[2] = link_read_chunks;
    link[3] = link_read_snaps;
    link[4] = rdata_hz;
}

static void dma_init(void)
{
    dma_tx = bflb_device_get_by_name("dma0_ch0");
    dma_rx = bflb_device_get_by_name("dma0_ch1");
    if (!dma_tx || !dma_rx) {
        dma_tx = dma_rx = NULL;
        return;
    }
    struct bflb_dma_channel_config_s tx = {
        .direction = DMA_MEMORY_TO_PERIPH,
        .src_req = DMA_REQUEST_NONE,
        .dst_req = DMA_REQUEST_SPI0_TX,
        .src_addr_inc = DMA_ADDR_INCREMENT_ENABLE,
        .dst_addr_inc = DMA_ADDR_INCREMENT_DISABLE,
        .src_burst_count = DMA_BURST_INCR1,
        .dst_burst_count = DMA_BURST_INCR1,
        .src_width = DMA_DATA_WIDTH_32BIT,
        .dst_width = DMA_DATA_WIDTH_32BIT,
    };
    struct bflb_dma_channel_config_s rx = {
        .direction = DMA_PERIPH_TO_MEMORY,
        .src_req = DMA_REQUEST_SPI0_RX,
        .dst_req = DMA_REQUEST_NONE,
        .src_addr_inc = DMA_ADDR_INCREMENT_DISABLE,
        .dst_addr_inc = DMA_ADDR_INCREMENT_DISABLE,    /* корзина: одно слово */
        .src_burst_count = DMA_BURST_INCR1,
        .dst_burst_count = DMA_BURST_INCR1,
        .src_width = DMA_DATA_WIDTH_32BIT,
        .dst_width = DMA_DATA_WIDTH_32BIT,
    };
    bflb_dma_channel_init(dma_tx, &tx);
    bflb_dma_channel_init(dma_rx, &rx);
}

int fpga_link_init(void)
{
    gpio = bflb_device_get_by_name("gpio");
    bflb_gpio_init(gpio, PIN_MISO, GPIO_FUNC_SPI0 | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_3);
    bflb_gpio_init(gpio, PIN_MOSI, GPIO_FUNC_SPI0 | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_3);
    bflb_gpio_init(gpio, PIN_SCK, GPIO_FUNC_SPI0 | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_3);
    bflb_gpio_init(gpio, PIN_CS, GPIO_OUTPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_3);
    bflb_gpio_set(gpio, PIN_CS);
    bflb_gpio_init(gpio, PIN_IRQ, GPIO_INPUT | GPIO_PULLUP | GPIO_SMT_EN);

    struct bflb_spi_config_s cfg = {
        .freq = READ_HZ,
        .role = SPI_ROLE_MASTER,
        .mode = SPI_MODE0,
        .data_width = SPI_DATA_WIDTH_8BIT,
        .bit_order = SPI_BIT_MSB,
        .byte_order = SPI_BYTE_LSB,
        .tx_fifo_threshold = 0,
        .rx_fifo_threshold = 0,
    };
    spi = bflb_device_get_by_name("spi0");
    bflb_spi_init(spi, &cfg);
    bflb_spi_feature_control(spi, SPI_CMD_SET_DATA_WIDTH, SPI_DATA_WIDTH_8BIT);
    cur_hz = READ_HZ;
    if (!dma_tx)
        dma_init();

    snap_t s;
    return snap(&s);
}

int fpga_irq(void)
{
    return !bflb_gpio_read(gpio, PIN_IRQ);
}

void fpga_bus(ftc_bus_t *bus)
{
    bus->ctx = NULL;
    bus->rd = b_rd;
    bus->wr = b_wr;
    bus->status = b_status;
    bus->irq = b_irq;
    bus->ms = b_ms;
    bus->wr_async = b_wr_async;
    bus->pump = b_pump;
    bus->wr_wait = b_wr_wait;
    bus->us = b_us;
    bus->link_us = b_link_us;
}

void fpga_info(uint32_t v)
{
    b_wr(NULL, INFO_ADDR, &v, 1);
}

int fpga_boot_requested(void)
{
    return boot_req;
}

int fpga_status_poll(void)
{
    snap_t s;
    return snap(&s);
}

uint32_t fpga_write_hz(void)
{
    return write_hz;
}

uint32_t fpga_stuck(void)
{
    return stuck_reads + stuck_writes;
}

/* --- самопроверка частот ------------------------------------------------------------------- */
/* Место образца: выше 1 МБ RAM_G FT812 и ниже шрифтов ПЗУ (0x1E0000) — программам FT812 не
   виден ни в 2 МБ, ни в 3 МБ (у FT812 RAM_G — 1 МБ). */
#define TEST_ADDR   0x1D0000u
#define TEST_WORDS  1024u              /* 4 КБ за проход */
#define TEST_PASSES 16u                /* 64 КБ */

static uint32_t t_pat[TEST_WORDS], t_back[TEST_WORDS];

/* образец прохода: случайные слова, 0x55…/0xAA…, 0/FFFFFFFF, бегущие единица и ноль */
static void pattern(uint32_t pass)
{
    uint32_t x = 0x9E3779B9u * (pass + 1);
    for (uint32_t i = 0; i < TEST_WORDS; i++) {
        uint32_t v;
        switch (pass % 4) {
        case 1: v = (i & 1) ? 0xAAAAAAAAu : 0x55555555u; break;
        case 2: v = (i & 1) ? 0xFFFFFFFFu : 0u; break;
        case 3: v = (i & 32) ? ~(1u << (i & 31)) : (1u << (i & 31)); break;
        default:
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            v = x;
            break;
        }
        t_pat[i] = v;
    }
}

static uint32_t mismatches(void)
{
    uint32_t bad = 0;
    for (uint32_t i = 0; i < TEST_WORDS; i++)
        bad += t_back[i] != t_pat[i];
    return bad;
}

/* записать образец на hz, прочитать на 20 МГц: ошибок; *us — время записи (мкс, все проходы) */
static uint32_t write_test(uint32_t hz, uint32_t passes, uint32_t *us)
{
    uint32_t bad = 0, t = 0, save_hz = write_hz;
    write_hz = hz;
    rdata_hz = READ_HZ;
    for (uint32_t p = 0; p < passes; p++) {
        pattern(p);
        uint64_t t0 = bflb_mtimer_get_time_us();
        b_wr(NULL, TEST_ADDR, t_pat, TEST_WORDS);
        t += (uint32_t)(bflb_mtimer_get_time_us() - t0);
        b_rd(NULL, TEST_ADDR, t_back, TEST_WORDS);
        bad += mismatches();
    }
    write_hz = save_hz;
    *us = t;
    return bad;
}

/* образец записан на 20 МГц, слова читаются на hz (ph — фазы PRD_0, 0 — по hz), кадрами по 32
   бита или по байту: ошибок; *us — время чтения */
static uint32_t read_test(uint32_t hz, uint32_t ph, int w32, uint32_t *us)
{
    uint32_t bad = 0, t = 0, save_hz = write_hz;
    for (uint32_t p = 0; p < TEST_PASSES; p++) {
        pattern(p);
        write_hz = READ_HZ;
        b_wr(NULL, TEST_ADDR, t_pat, TEST_WORDS);
        rdata_hz = hz;
        rdata_ph = ph;
        rd32_on = w32;
        uint64_t t0 = bflb_mtimer_get_time_us();
        b_rd(NULL, TEST_ADDR, t_back, TEST_WORDS);
        t += (uint32_t)(bflb_mtimer_get_time_us() - t0);
        rdata_hz = READ_HZ;
        rdata_ph = 0;
        rd32_on = 0;
        bad += mismatches();
    }
    write_hz = save_hz;
    *us = t;
    return bad;
}

static void cont(int on)
{
    bflb_spi_feature_control(spi, SPI_CMD_SET_CS_INTERVAL, (size_t)on);
}

/* запись с замерами: rep[0] ошибок, [1] мкс всего, [2] мкс DMA, [3] мкс ожидания места, [4] снимков */
static void write_meas(uint32_t hz, uint32_t *rep)
{
    meas = 1;
    m_dma = m_wait = m_snaps = 0;
    rep[0] = write_test(hz, TEST_PASSES, &rep[1]);
    rep[2] = m_dma;
    rep[3] = m_wait;
    rep[4] = m_snaps;
    meas = 0;
}

int fpga_speed_test(void)
{
    uint32_t us;
    uint32_t save_checked = checked;
    checked = CHECK_CHUNKS;                  /* свою проверку посылок на время теста — прочь */
    cont(0);
    if (write_test(READ_HZ, 1, &us) != 0) {  /* 20 МГц не сошлось: ядра FT812 нет (TS-Config) */
        checked = save_checked;
        return -1;
    }
    uint32_t rep[32] = {FPGA_REPORT_MAGIC};
    write_meas(80000000u, &rep[4]);          /* с паузами между кадрами SPI */
    cont(1);
    write_meas(80000000u, &rep[9]);          /* непрерывный режим */
    rep[14] = write_test(40000000u, TEST_PASSES, &rep[15]);
    cont(0);
    rep[16] = read_test(READ_HZ, 0, 0, &rep[17]);
    rep[18] = read_test(READ_HZ, 0, 1, &rep[19]);
    rep[20] = read_test(40000000u, 0, 1, &rep[21]);
    rep[22] = read_test(0, 0x0001u, 1, &rep[23]);   /* PH_0 2 такта, PH_1 1 */
    rep[24] = read_test(0, 0x0100u, 1, &rep[25]);   /* PH_0 1 такт, PH_1 2 */
    rep[26] = read_test(80000000u, 0, 1, &rep[27]);
    int use_cont = rep[9] == 0;
    uint32_t whz = (rep[9] == 0 || rep[4] == 0) ? 80000000u : 40000000u;
    int r32 = rep[18] == 0;
    uint32_t rhz = (r32 && rep[20] == 0 && (rep[22] == 0 || rep[24] == 0)) ? 40000000u : READ_HZ;
    rep[1] = whz;
    rep[2] = rhz;
    rep[3] = (uint32_t)use_cont | ((uint32_t)r32 << 1);
    write_hz = READ_HZ;
    b_wr(NULL, FPGA_REPORT_ADDR, rep, 32);
    cont(use_cont);
    write_hz = whz;
    rdata_hz = rhz;
    rd32_on = r32;
    checked = 0;                             /* первые посылки на новой частоте — снова с проверкой */
    return whz == 80000000u ? 3 : 2;
}
