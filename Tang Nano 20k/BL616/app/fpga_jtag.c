/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Перезагрузка ПЛИС Tang Nano 20K по JTAG с BL616: инструкция Gowin Reconfig (IR 0x3C), затем
 * NOOP (0x02) — как кнопка «Reprogram» программатора Gowin (UG290, команды JTAG; так же —
 * openFPGALoader). ПЛИС грузится из flash по адресу перехода multiboot своего битстрима: из
 * слота 0 — в слот 1, из слота 1 — в 0. Свой RECONFIG_N ПЛИС (выведенный как GPIO) перезагрузку
 * не запускает — проверено на столе 18.10.2026, — поэтому ядро только просит (бит 7 флагов
 * STATUS, cp_link.sv), а перезагружает BL616.
 *
 * Выводы JTAG BL616 (Tang Nano 20K, как у FPGA Companion MiSTle): TCK GPIO10, TDI GPIO12,
 * TDO GPIO14, TMS GPIO16. Ими же пользуется FPGA Partner (USB-JTAG программатора): настройку
 * выводов (регистры GLB gpio_cfgN) команда сохраняет и после неё возвращает как была. Пока
 * программатор по USB шлёт свои команды, перезагрузка с ними столкнулась бы — в плате Evo USB
 * не подключён.
 */
#include <stdint.h>

#include "bflb_gpio.h"
#include "bflb_mtimer.h"

#include "fpga_jtag.h"

#define PIN_TCK GPIO_PIN_10
#define PIN_TDI GPIO_PIN_12
#define PIN_TMS GPIO_PIN_16

#define IR_RECONFIG 0x3Cu
#define IR_NOOP     0x02u

#ifdef FPGA_JTAG_HOST_TEST
extern volatile uint32_t fpga_jtag_test_cfg[32];          /* проверка на ПК: регистры — массив */
#define GPIO_CFG(n) (&fpga_jtag_test_cfg[(n)])
#else
#define GPIO_CFG(n) ((volatile uint32_t *)(uintptr_t)(0x200008C4u + 4u * (n)))   /* gpio_cfgN */
#endif

static struct bflb_device_s *gpio;

/* такт TCK: TMS и TDI выставлены до фронта, ПЛИС берёт их по фронту */
static void clock(int tms, int tdi)
{
    if (tms)
        bflb_gpio_set(gpio, PIN_TMS);
    else
        bflb_gpio_reset(gpio, PIN_TMS);
    if (tdi)
        bflb_gpio_set(gpio, PIN_TDI);
    else
        bflb_gpio_reset(gpio, PIN_TDI);
    bflb_mtimer_delay_us(1);
    bflb_gpio_set(gpio, PIN_TCK);
    bflb_mtimer_delay_us(1);
    bflb_gpio_reset(gpio, PIN_TCK);
}

/* из Run-Test/Idle: Shift-IR, 8 бит младшим вперёд, Update-IR и снова Run-Test/Idle */
static void shift_ir(uint32_t ir)
{
    clock(1, 0);                       /* Select-DR-Scan */
    clock(1, 0);                       /* Select-IR-Scan */
    clock(0, 0);                       /* Capture-IR */
    clock(0, 0);                       /* Shift-IR */
    for (int i = 0; i < 8; i++)
        clock(i == 7, (int)((ir >> i) & 1u));   /* последний бит — с выходом в Exit1-IR */
    clock(1, 0);                       /* Update-IR */
    clock(0, 0);                       /* Run-Test/Idle */
}

void fpga_jtag_reconfig(void)
{
    gpio = bflb_device_get_by_name("gpio");
    const uint32_t tck = *GPIO_CFG(10), tdi = *GPIO_CFG(12), tms = *GPIO_CFG(16);
    bflb_gpio_init(gpio, PIN_TCK, GPIO_OUTPUT | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_3);
    bflb_gpio_init(gpio, PIN_TDI, GPIO_OUTPUT | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_3);
    bflb_gpio_init(gpio, PIN_TMS, GPIO_OUTPUT | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_3);
    bflb_gpio_reset(gpio, PIN_TCK);

    for (int i = 0; i < 6; i++)
        clock(1, 0);                   /* Test-Logic-Reset из любого состояния */
    clock(0, 0);                       /* Run-Test/Idle */
    shift_ir(IR_RECONFIG);
    for (int i = 0; i < 8; i++)
        clock(0, 0);
    shift_ir(IR_NOOP);
    for (int i = 0; i < 8; i++)
        clock(0, 0);

    *GPIO_CFG(10) = tck;               /* выводы — снова как у FPGA Partner */
    *GPIO_CFG(12) = tdi;
    *GPIO_CFG(16) = tms;
}
