/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Загрузка ядра в SRAM ПЛИС Tang Nano 20K (GW2AR-18) по JTAG с BL616 — для ядер, которых нет
 * во flash Tang (ZX Next и другие, docs/next-core.md): multiboot ведёт только по адресу из
 * заголовка битстрима (слот 0 → 1), третье ядро так не загрузить.
 *
 * Порядок команд Gowin — как у FPGA-Companion MiSTle (src/gowin.c, src/jtag.c, src/bl616/
 * mcu_hw.c, Apache-2.0, Till Harbaum и др.): у GW2AR-18 сброс ошибки CRC, CONFIG_ENABLE (ждать
 * SYSTEM_EDIT_MODE), ERASE_SRAM, NOOP, ждать MEMORY_ERASE, XFER_DONE, NOOP, CONFIG_DISABLE (ждать
 * снятия EDIT_MODE); затем CONFIG_ENABLE, INIT_ADDR, XFER_WRITE и битстрим одним сдвигом DR —
 * каждый байт старшим битом вперёд; CONFIG_DISABLE, NOOP, DONE_FINAL в STATUS, USERCODE.
 *
 * Выводы — как у fpga_jtag.c (TCK GPIO10, TDI GPIO12, TDO GPIO14, TMS GPIO16): ими же пользуется
 * FPGA Partner (USB-JTAG), их настройка сохраняется и возвращается. Такт — без задержек, как у
 * FPGA-Companion (bflb_gpio_set/reset подряд): у fpga_jtag.c задержки по 1 мкс — ядро 886 КБ шло
 * бы ~15 с.
 */
#include <stdint.h>

#include "bflb_gpio.h"
#include "bflb_mtimer.h"

#include "core_jtag.h"

#define PIN_TCK GPIO_PIN_10
#define PIN_TDI GPIO_PIN_12
#define PIN_TDO GPIO_PIN_14
#define PIN_TMS GPIO_PIN_16

#define GW_NOOP           0x02u
#define GW_ERASE_SRAM     0x05u
#define GW_XFER_DONE      0x09u
#define GW_IDCODE         0x11u
#define GW_INIT_ADDR      0x12u
#define GW_USERCODE       0x13u
#define GW_CONFIG_ENABLE  0x15u
#define GW_XFER_WRITE     0x17u
#define GW_CONFIG_DISABLE 0x3Au
#define GW_STATUS         0x41u

#define ST_CRC_ERROR      (1u << 0)
#define ST_MEMORY_ERASE   (1u << 5)
#define ST_EDIT_MODE      (1u << 7)
#define ST_DONE_FINAL     (1u << 13)

#define IDCODE_GW2AR18    0x0000081Bu

#ifdef CORE_JTAG_HOST_TEST
extern volatile uint32_t core_jtag_test_cfg[32];
#define GPIO_CFG(n) (&core_jtag_test_cfg[(n)])
#else
#define GPIO_CFG(n) ((volatile uint32_t *)(uintptr_t)(0x200008C4u + 4u * (n)))   /* gpio_cfgN */
#endif

static struct bflb_device_s *gpio;

/* len тактов, TMS — биты tms младшим вперёд, TDI постоянный; TDO — в тех же битах */
static uint32_t tms_seq(int tdi, uint32_t tms, int len)
{
    uint32_t rx = 0;
    if (tdi)
        bflb_gpio_set(gpio, PIN_TDI);
    else
        bflb_gpio_reset(gpio, PIN_TDI);
    for (int i = 0; i < len; i++) {
        if ((tms >> i) & 1u)
            bflb_gpio_set(gpio, PIN_TMS);
        else
            bflb_gpio_reset(gpio, PIN_TMS);
        bflb_gpio_set(gpio, PIN_TCK);
        if (bflb_gpio_read(gpio, PIN_TDO))
            rx |= 1u << i;
        bflb_gpio_reset(gpio, PIN_TCK);
    }
    return rx;
}

/* сдвиг n бит (TMS = 0) младшим вперёд; tx == NULL — единицы; rx — может быть NULL */
static void shift_bits(const uint8_t *tx, uint8_t *rx, int n)
{
    bflb_gpio_reset(gpio, PIN_TMS);
    for (int i = 0; i < n; i++) {
        int b = tx ? (tx[i >> 3] >> (i & 7)) & 1 : 1;
        if (b)
            bflb_gpio_set(gpio, PIN_TDI);
        else
            bflb_gpio_reset(gpio, PIN_TDI);
        bflb_gpio_set(gpio, PIN_TCK);
        if (rx) {
            if (bflb_gpio_read(gpio, PIN_TDO))
                rx[i >> 3] |= (uint8_t)(1u << (i & 7));
            else
                rx[i >> 3] &= (uint8_t)~(1u << (i & 7));
        }
        bflb_gpio_reset(gpio, PIN_TCK);
    }
}

/* такты в Run-Test/Idle: ПЛИС работает над командой (стирание, проверка) */
static void idle_us(uint32_t us)
{
    uint64_t end = bflb_mtimer_get_time_us() + us;
    bflb_gpio_reset(gpio, PIN_TMS);
    while (bflb_mtimer_get_time_us() < end) {
        bflb_gpio_set(gpio, PIN_TCK);
        bflb_gpio_reset(gpio, PIN_TCK);
    }
}

/* команда в IR из Run-Test/Idle и назад (как jtag_command_u08 FPGA-Companion) */
static void ir(uint8_t cmd)
{
    tms_seq(1, 0x00u, 6);              /* постоять в Run-Test/Idle */
    tms_seq(1, 0x03u, 4);              /* 1, 1, 0, 0 — Shift-IR */
    shift_bits(&cmd, NULL, 7);
    tms_seq((cmd & 0x80u) ? 1 : 0, 0x01u, 1);   /* последний бит — с выходом в Exit1-IR */
    tms_seq(1, 0x01u, 2);              /* Update-IR, Run-Test/Idle */
}

static void enter_shift_dr(void)
{
    tms_seq(1, 0x00u, 6);
    tms_seq(1, 0x01u, 3);              /* 1, 0, 0 — Shift-DR */
}

static uint32_t ir_read32(uint8_t cmd)
{
    uint8_t rx[4] = {0};
    ir(cmd);
    enter_shift_dr();
    shift_bits(NULL, rx, 31);
    uint32_t last = tms_seq(1, 0x01u, 1);   /* последний бит — с выходом в Exit1-DR */
    tms_seq(1, 0x01u, 2);
    if (last & 1u)
        rx[3] |= 0x80u;
    else
        rx[3] &= 0x7Fu;
    return (uint32_t)rx[0] | (uint32_t)rx[1] << 8 | (uint32_t)rx[2] << 16 | (uint32_t)rx[3] << 24;
}

static int poll_status(uint32_t mask, uint32_t value)
{
    for (int i = 0; i < 1000; i++)
        if ((ir_read32(GW_STATUS) & mask) == value)
            return 1;
    return 0;
}

static uint32_t identify(void)
{
    uint8_t rx[4] = {0};
    tms_seq(1, 0x1Fu, 5);              /* Test-Logic-Reset */
    tms_seq(1, 0x02u, 4);              /* 0, 1, 0, 0 — Shift-DR (IDCODE после сброса) */
    shift_bits(NULL, rx, 32);
    tms_seq(1, 0x03u, 3);              /* 1, 1, 0 — Run-Test/Idle */
    return (uint32_t)rx[0] | (uint32_t)rx[1] << 8 | (uint32_t)rx[2] << 16 | (uint32_t)rx[3] << 24;
}

static uint8_t reverse(uint8_t b)
{
    b = (uint8_t)(((b & 0x55u) << 1) | ((b & 0xAAu) >> 1));
    b = (uint8_t)(((b & 0x33u) << 2) | ((b & 0xCCu) >> 2));
    return (uint8_t)(((b & 0x0Fu) << 4) | ((b & 0xF0u) >> 4));
}

static int erase_sram(void)
{
    (void)ir_read32(GW_USERCODE);
    if (ir_read32(GW_STATUS) & ST_CRC_ERROR) {
        /* GW2A после неудачной загрузки: порядок FPGA-Companion (gowin_gw2a_force_state) */
        ir(GW_CONFIG_DISABLE); ir(0);
        (void)ir_read32(GW_IDCODE); (void)ir_read32(GW_STATUS);
        ir(GW_CONFIG_DISABLE); ir(0); (void)ir_read32(GW_STATUS);
        (void)ir_read32(GW_IDCODE);
        ir(GW_CONFIG_ENABLE); ir(GW_CONFIG_DISABLE); ir(GW_NOOP);
        (void)ir_read32(GW_IDCODE); ir(GW_NOOP); (void)ir_read32(GW_IDCODE);
    }
    ir(GW_CONFIG_ENABLE);
    if (!poll_status(ST_EDIT_MODE, ST_EDIT_MODE))
        return CORE_JTAG_NO_EDIT;
    ir(GW_ERASE_SRAM);
    ir(GW_NOOP);
    idle_us(10000);
    if (!poll_status(ST_MEMORY_ERASE, ST_MEMORY_ERASE))
        return CORE_JTAG_NO_ERASE;
    ir(GW_XFER_DONE);
    ir(GW_NOOP);
    ir(GW_CONFIG_DISABLE);
    ir(GW_NOOP);
    if (!poll_status(ST_EDIT_MODE, 0))
        return CORE_JTAG_NO_EDIT;
    return CORE_JTAG_OK;
}

/* Выводы JTAG — у FPGA Partner; на время обмена — свои, потом — как были */
static uint32_t saved_cfg[4];

static void pins_take(void)
{
    gpio = bflb_device_get_by_name("gpio");
    saved_cfg[0] = *GPIO_CFG(10);
    saved_cfg[1] = *GPIO_CFG(12);
    saved_cfg[2] = *GPIO_CFG(14);
    saved_cfg[3] = *GPIO_CFG(16);
    bflb_gpio_init(gpio, PIN_TCK, GPIO_OUTPUT | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_3);
    bflb_gpio_init(gpio, PIN_TDI, GPIO_OUTPUT | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_3);
    bflb_gpio_init(gpio, PIN_TMS, GPIO_OUTPUT | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_3);
    bflb_gpio_init(gpio, PIN_TDO, GPIO_INPUT | GPIO_FLOAT | GPIO_SMT_EN);
    bflb_gpio_reset(gpio, PIN_TCK);
}

static void pins_give(void)
{
    tms_seq(1, 0x1Fu, 5);              /* Test-Logic-Reset */
    *GPIO_CFG(10) = saved_cfg[0];      /* выводы — снова как у FPGA Partner */
    *GPIO_CFG(12) = saved_cfg[1];
    *GPIO_CFG(14) = saved_cfg[2];
    *GPIO_CFG(16) = saved_cfg[3];
}

int core_jtag_usercode(uint32_t *usercode)
{
    pins_take();
    int result = CORE_JTAG_NO_FPGA;
    if (identify() == IDCODE_GW2AR18) {
        tms_seq(1, 0x1Fu, 5);
        tms_seq(1, 0x00u, 1);          /* Run-Test/Idle */
        if (usercode)
            *usercode = ir_read32(GW_USERCODE);
        result = CORE_JTAG_OK;
    }
    pins_give();
    return result;
}

int core_jtag_load(core_jtag_read_fn read, void *ctx, uint32_t size, uint32_t *usercode)
{
    static uint8_t buf[512];
    int result = CORE_JTAG_OK;

    pins_take();

    if (size == 0) {
        result = CORE_JTAG_BAD_SIZE;
        goto out;
    }
    if (identify() != IDCODE_GW2AR18) {
        result = CORE_JTAG_NO_FPGA;
        goto out;
    }
    tms_seq(1, 0x1Fu, 5);
    tms_seq(1, 0x00u, 1);              /* Run-Test/Idle */
    if ((result = erase_sram()) != CORE_JTAG_OK)
        goto out;

    ir(GW_CONFIG_ENABLE);
    ir(GW_INIT_ADDR);
    ir(GW_XFER_WRITE);
    enter_shift_dr();
    for (uint32_t at = 0; at < size;) {
        uint32_t n = size - at > sizeof buf ? (uint32_t)sizeof buf : size - at;
        if (read(ctx, at, buf, n) != 0) {
            result = CORE_JTAG_READ;
            break;
        }
        for (uint32_t i = 0; i < n; i++)
            buf[i] = reverse(buf[i]);
        at += n;
        if (at < size) {
            shift_bits(buf, NULL, (int)(8 * n));
        } else {
            shift_bits(buf, NULL, (int)(8 * n - 1));
            tms_seq((buf[n - 1] >> 7) & 1, 0x01u, 1);   /* последний бит — с выходом в Exit1-DR */
            tms_seq(1, 0x01u, 2);
        }
    }
    if (result != CORE_JTAG_OK) {
        tms_seq(1, 0x03u, 3);
        ir(GW_CONFIG_DISABLE);
        ir(GW_NOOP);
        goto out;
    }
    ir(GW_CONFIG_DISABLE);
    ir(GW_NOOP);
    (void)ir_read32(GW_STATUS);
    if (!(ir_read32(GW_STATUS) & ST_DONE_FINAL)) {
        result = CORE_JTAG_NOT_DONE;
        goto out;
    }
    if (usercode)
        *usercode = ir_read32(GW_USERCODE);
out:
    pins_give();
    return result;
}
