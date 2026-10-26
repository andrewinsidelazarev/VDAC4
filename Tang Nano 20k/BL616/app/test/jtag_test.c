/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Проверка fpga_jtag.c на ПК: выводы TCK, TDI, TMS — заглушки bouffalo_sdk, за ними — автомат
 * TAP JTAG (16 состояний, IEEE 1149.1) и регистр инструкций ПЛИС Gowin (8 бит, младшим вперёд).
 * Ожидается: инструкции Reconfig (0x3C), затем NOOP (0x02), TAP в конце — Run-Test/Idle,
 * настройка выводов (gpio_cfg10/12/16) после команды — та же, что до неё (FPGA Partner).
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "bflb_gpio.h"
#include "bflb_mtimer.h"
#include "fpga_jtag.h"

volatile uint32_t fpga_jtag_test_cfg[32];

enum { TLR, RTI, SEL_DR, CAP_DR, SH_DR, EX1_DR, PAU_DR, EX2_DR, UPD_DR,
       SEL_IR, CAP_IR, SH_IR, EX1_IR, PAU_IR, EX2_IR, UPD_IR };
static const unsigned char next_state[16][2] = {
    {RTI, TLR}, {RTI, SEL_DR}, {CAP_DR, SEL_IR}, {SH_DR, EX1_DR}, {SH_DR, EX1_DR}, {PAU_DR, UPD_DR},
    {PAU_DR, EX2_DR}, {SH_DR, UPD_DR}, {RTI, SEL_DR}, {CAP_IR, TLR}, {SH_IR, EX1_IR}, {SH_IR, EX1_IR},
    {PAU_IR, UPD_IR}, {PAU_IR, EX2_IR}, {SH_IR, UPD_IR}, {RTI, SEL_DR},
};

static int pin[32];
static int tap = TLR;
static unsigned ir_shift, ir_bits;
static unsigned irs[8], nir;
static unsigned edges;
static bool cfg_touched;

struct bflb_device_s *bflb_device_get_by_name(const char *name)
{
    static struct bflb_device_s d;
    (void)name;
    return &d;
}

void bflb_gpio_init(struct bflb_device_s *d, uint8_t p, uint32_t cfg)
{
    (void)d;
    (void)cfg;
    fpga_jtag_test_cfg[p] = 0xDEADBEEFu;   /* драйвер переписывает настройку вывода */
    cfg_touched = true;
}

static void tck_rise(void)
{
    edges++;
    int tms = pin[GPIO_PIN_16], tdi = pin[GPIO_PIN_12];
    if (tap == SH_IR) {
        ir_shift |= (unsigned)tdi << ir_bits;
        ir_bits++;
    }
    tap = next_state[tap][tms];
    if (tap == TLR)
        nir = 0;               /* считаются инструкции после сброса TAP: из Shift-IR путь в
                                  Test-Logic-Reset по стандарту идёт через Update-IR */
    if (tap == CAP_IR) {
        ir_shift = 0;
        ir_bits = 0;
    }
    if (tap == UPD_IR && nir < 8)
        irs[nir++] = ir_bits == 8 ? ir_shift : 0x100u | ir_bits;   /* не 8 бит — ошибка */
}

void bflb_gpio_set(struct bflb_device_s *d, uint8_t p)
{
    (void)d;
    if (p == GPIO_PIN_10 && !pin[p])
        tck_rise();
    pin[p] = 1;
}

void bflb_gpio_reset(struct bflb_device_s *d, uint8_t p)
{
    (void)d;
    pin[p] = 0;
}

bool bflb_gpio_read(struct bflb_device_s *d, uint8_t p)
{
    (void)d;
    return pin[p];
}

void bflb_mtimer_delay_us(uint32_t us) { (void)us; }
void bflb_mtimer_delay_ms(uint32_t ms) { (void)ms; }
uint64_t bflb_mtimer_get_time_ms(void) { return 0; }

int main(void)
{
    int err = 0;
    for (int start = 0; start < 16; start++) {     /* из любого состояния TAP */
        for (int i = 0; i < 32; i++)
            fpga_jtag_test_cfg[i] = 0x1000u + (uint32_t)i;
        tap = start;
        nir = 0;
        edges = 0;
        cfg_touched = false;
        fpga_jtag_reconfig();
        int ok = nir == 2 && irs[0] == 0x3Cu && irs[1] == 0x02u && tap == RTI && cfg_touched &&
                 fpga_jtag_test_cfg[10] == 0x100Au && fpga_jtag_test_cfg[12] == 0x100Cu &&
                 fpga_jtag_test_cfg[16] == 0x1010u && fpga_jtag_test_cfg[14] == 0x100Eu;
        if (!ok) {
            printf("  из состояния %d: инструкций %u (%02X %02X), TAP %d, выводы %08X %08X %08X\n", start, nir,
                   irs[0], irs[1], tap, (unsigned)fpga_jtag_test_cfg[10], (unsigned)fpga_jtag_test_cfg[12],
                   (unsigned)fpga_jtag_test_cfg[16]);
            err = 1;
        }
    }
    printf("JTAG Reconfig: IR 3C, затем 02, из любого состояния TAP; %u тактов TCK; выводы — как были: %s\n",
           edges, err ? "НЕТ" : "да");
    printf(err ? "ТЕСТ НЕ ПРОЙДЕН\n" : "ТЕСТ ПРОЙДЕН\n");
    return err;
}
