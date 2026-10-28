/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Проверка core_jtag.c на ПК: выводы BL616 — заглушки bouffalo_sdk, за ними — автомат TAP JTAG
 * (IEEE 1149.1) и модель ПЛИС Gowin GW2AR-18: IR 8 бит младшим вперёд; DR по инструкции —
 * IDCODE (0x081B), STATUS (EDIT_MODE после CONFIG_ENABLE до CONFIG_DISABLE, MEMORY_ERASE после
 * ERASE_SRAM, DONE_FINAL — когда записан весь образ), USERCODE (из хвоста образа), XFER_WRITE —
 * байты образа старшим битом вперёд. TDO меняется по спаду TCK, как у ПЛИС.
 * Ожидается: образ (настоящий NEXT.BIN, если передан, иначе свой) записан байт в байт, UserCode
 * прочитан, TAP в конце — Test-Logic-Reset, настройка выводов — та же, что до загрузки; ошибка
 * источника — CORE_JTAG_READ.
 * Переход в слот (core_jtag_boot, прошивка 35): модель запоминает адрес перехода из команды D2
 * принятого потока, Reconfig (0x3C) грузит «из flash» по нему. Ожидается: стирание, только
 * заголовок (64 байта: синхрослово, IDCODE, D2 с нужным адресом, INIT_ADDR), Reconfig — после
 * CONFIG_DISABLE, адрес загрузки — заданный, выводы вернулись, TAP — Run-Test/Idle.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bflb_gpio.h"
#include "bflb_mtimer.h"
#include "core_jtag.h"

volatile uint32_t core_jtag_test_cfg[32];

enum { TLR, RTI, SEL_DR, CAP_DR, SH_DR, EX1_DR, PAU_DR, EX2_DR, UPD_DR,
       SEL_IR, CAP_IR, SH_IR, EX1_IR, PAU_IR, EX2_IR, UPD_IR };
static const unsigned char next_state[16][2] = {
    {RTI, TLR}, {RTI, SEL_DR}, {CAP_DR, SEL_IR}, {SH_DR, EX1_DR}, {SH_DR, EX1_DR}, {PAU_DR, UPD_DR},
    {PAU_DR, EX2_DR}, {SH_DR, UPD_DR}, {RTI, SEL_DR}, {CAP_IR, TLR}, {SH_IR, EX1_IR}, {SH_IR, EX1_IR},
    {PAU_IR, UPD_IR}, {PAU_IR, EX2_IR}, {SH_IR, UPD_IR}, {RTI, SEL_DR},
};

static int pin[32];
static int tap = TLR;
static unsigned ir_sr, ir = 0x11;          /* после Test-Logic-Reset — IDCODE */
static uint64_t dr_sr;
static int tdo;
static int edit_mode, erased, done, writing;
static uint8_t *got;                       /* принятый образ */
static size_t got_n, got_cap;
static unsigned got_bit, got_byte;
static uint32_t usercode_img;
static unsigned bad_cmds;
static uint64_t now_us;
static uint32_t jump_reg;                  /* адрес перехода multiboot (команда D2 потока) */
static unsigned reconfigs;
static uint32_t boot_from;                 /* откуда ПЛИС грузилась по последнему Reconfig */
static const uint8_t *img;
static size_t img_n;

struct bflb_device_s *bflb_device_get_by_name(const char *name) { static struct bflb_device_s d; (void)name; return &d; }
void bflb_gpio_init(struct bflb_device_s *d, uint8_t p, uint32_t cfg) { (void)d; core_jtag_test_cfg[p] = 0xA5000000u | cfg; }
uint64_t bflb_mtimer_get_time_us(void) { return now_us += 1; }
uint64_t bflb_mtimer_get_time_ms(void) { return now_us / 1000; }
void bflb_mtimer_delay_us(uint32_t us) { now_us += us; }
void bflb_mtimer_delay_ms(uint32_t ms) { now_us += 1000ull * ms; }
bool bflb_gpio_read(struct bflb_device_s *d, uint8_t p) { (void)d; return p == GPIO_PIN_14 ? tdo : pin[p]; }

static uint32_t status(void)
{
    return (edit_mode ? 1u << 7 : 0) | (erased ? 1u << 5 : 0) | (done ? 1u << 13 : 0);
}

static void update_ir(void)
{
    ir = ir_sr & 0xFF;
    switch (ir) {
    case 0x15: edit_mode = 1; break;                       /* CONFIG_ENABLE */
    case 0x3A:                                             /* CONFIG_DISABLE */
        edit_mode = 0;
        /* DONE — только за целым битстримом (у ПЛИС его закрывают CRC и команды в конце) */
        if (writing) {
            done = got_n == img_n && memcmp(got, img, img_n) == 0;
            writing = 0;
            /* D2 00 FF FF и адрес (старшим вперёд) — в заголовке до первого кадра */
            for (size_t i = 0; i + 8 <= got_n && i < 128; i++)
                if (got[i] == 0xD2 && got[i + 1] == 0x00 && got[i + 2] == 0xFF && got[i + 3] == 0xFF) {
                    jump_reg = (uint32_t)got[i + 4] << 24 | (uint32_t)got[i + 5] << 16 |
                               (uint32_t)got[i + 6] << 8 | got[i + 7];
                    break;
                }
        }
        break;
    case 0x3C:                                             /* RECONFIG: загрузка из flash */
        if (edit_mode) bad_cmds++;
        reconfigs++;
        boot_from = jump_reg;
        break;
    case 0x05: if (edit_mode) erased = 1; else bad_cmds++; break;   /* ERASE_SRAM */
    case 0x12: if (!edit_mode) bad_cmds++; break;          /* INIT_ADDR */
    case 0x17: if (edit_mode && erased) { writing = 1; got_n = 0; got_bit = 0; done = 0; } else bad_cmds++; break;
    default: break;
    }
}

static void rising(void)
{
    int tms = pin[GPIO_PIN_16], tdi = pin[GPIO_PIN_12];
    if (tap == CAP_DR)
        dr_sr = ir == 0x41 ? status() : ir == 0x13 ? (done ? usercode_img : 0) : ir == 0x11 ? 0x0000081Bu : 0;
    if (tap == CAP_IR)
        ir_sr = 0x01;
    if (tap == SH_IR)
        ir_sr = (ir_sr >> 1) | ((unsigned)tdi << 7);
    if (tap == SH_DR) {
        if (ir == 0x17 && writing) {                         /* образ: первый бит — старший */
            got_byte = (got_byte << 1) | (unsigned)tdi;
            if (++got_bit == 8) {
                if (got_n == got_cap) { got_cap = got_cap ? 2 * got_cap : 1 << 20; got = realloc(got, got_cap); }
                got[got_n++] = (uint8_t)got_byte;
                got_bit = 0; got_byte = 0;
            }
        }
        dr_sr = (dr_sr >> 1) | ((uint64_t)tdi << 31);
    }
    int next = next_state[tap][tms];
    if (next == TLR) ir = 0x11;
    if (next == UPD_IR) update_ir();
    tap = next;
}

static void falling(void)
{
    tdo = tap == SH_DR ? (int)(dr_sr & 1u) : tap == SH_IR ? (int)(ir_sr & 1u) : 0;
}

void bflb_gpio_set(struct bflb_device_s *d, uint8_t p)
{
    (void)d;
    if (p == GPIO_PIN_10 && !pin[p]) { pin[p] = 1; rising(); return; }
    pin[p] = 1;
}

void bflb_gpio_reset(struct bflb_device_s *d, uint8_t p)
{
    (void)d;
    if (p == GPIO_PIN_10 && pin[p]) { pin[p] = 0; falling(); return; }
    pin[p] = 0;
}

static size_t fail_at = (size_t)-1;
static int src(void *ctx, uint32_t off, uint8_t *buf, uint32_t n)
{
    (void)ctx;
    if (off + n > fail_at) return -1;
    memcpy(buf, img + off, n);
    return 0;
}

static void reset_model(void)
{
    tap = TLR; ir = 0x11; edit_mode = erased = done = writing = 0; got_n = 0; got_bit = 0; bad_cmds = 0;
    for (int i = 0; i < 32; i++) core_jtag_test_cfg[i] = 0x1000u + (unsigned)i;
}

int main(int argc, char **argv)
{
    static uint8_t own[300000];
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 2; }
        static uint8_t file[2u << 20];
        img_n = fread(file, 1, sizeof file, f);
        fclose(f);
        img = file;
    } else {
        for (size_t i = 0; i < sizeof own; i++) own[i] = (uint8_t)(i * 37u + 11u);
        img = own; img_n = sizeof own;
    }
    usercode_img = 0x56443403u;
    int bad = 0;

    reset_model();
    uint32_t uc = 0;
    int r = core_jtag_load(src, NULL, (uint32_t)img_n, &uc);
    bool same = got_n == img_n && memcmp(got, img, img_n) == 0;
    bool pins = core_jtag_test_cfg[10] == 0x100Au && core_jtag_test_cfg[12] == 0x100Cu &&
                core_jtag_test_cfg[14] == 0x100Eu && core_jtag_test_cfg[16] == 0x1010u;
    printf("загрузка: итог %d, принято %zu из %zu байт, совпали %s, UserCode %08X, TAP в конце %s, "
           "выводы вернулись %s, неверных команд %u\n", r, got_n, img_n, same ? "да" : "НЕТ", uc,
           tap == TLR ? "Test-Logic-Reset" : "НЕ ТОТ", pins ? "да" : "НЕТ", bad_cmds);
    if (r != CORE_JTAG_OK || !same || uc != 0x56443403u || tap != TLR || !pins || bad_cmds) bad = 1;

    /* UserCode ядра, что уже работает (прошивка 15): без настройки и без вмешательства */
    {
        uint32_t running = 0;
        const unsigned cmds_before = bad_cmds;
        const int ur = core_jtag_usercode(&running);
        const bool still = done && !edit_mode && bad_cmds == cmds_before && tap == TLR &&
                           core_jtag_test_cfg[10] == 0x100Au && core_jtag_test_cfg[16] == 0x1010u;
        printf("UserCode работающей ПЛИС: итог %d, %08X, ядро не тронуто %s\n", ur, running,
               still ? "да" : "НЕТ");
        if (ur != CORE_JTAG_OK || running != 0x56443403u || !still) bad = 1;
    }

    reset_model();
    fail_at = img_n / 2;
    r = core_jtag_load(src, NULL, (uint32_t)img_n, &uc);
    printf("отказ источника посреди образа: итог %d (ждали %d), DONE %d\n", r, CORE_JTAG_READ, done);
    if (r != CORE_JTAG_READ || done) bad = 1;

    /* Переход в слот 3 из TS-Config (адрес перехода её заголовка — слот 1), затем назад в 0 */
    fail_at = (size_t)-1;
    const uint8_t *file_img = argc > 1 && img_n >= 64 ? img : NULL;   /* настоящий битстрим ядра */
    img_n = 0;
    static const uint8_t sync_id[] = {0xA5, 0xC3, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x1B};
    for (int k = 0; k < 2; k++) {
        const uint32_t want = k == 0 ? 0x300000u : 0u;
        reset_model();
        jump_reg = 0x100000u;
        reconfigs = 0;
        boot_from = 0xFFFFFFFFu;
        r = core_jtag_boot(want);
        bool head = got_n == 64 && memcmp(got + 0x16, sync_id, sizeof sync_id) == 0 &&
                    got[0x34] == 0xD2 && got[0x35] == 0x00 && got[0x36] == 0xFF &&
                    got[0x37] == 0xFF && got[0x3C] == 0x12;
        /* с битстримом ядра VDAC4: заголовок — байт в байт его, кроме адреса перехода */
        if (file_img && head)
            head = memcmp(got, file_img, 0x38) == 0 && memcmp(got + 0x3C, file_img + 0x3C, 4) == 0;
        const bool pins = core_jtag_test_cfg[10] == 0x100Au && core_jtag_test_cfg[12] == 0x100Cu &&
                          core_jtag_test_cfg[14] == 0x100Eu && core_jtag_test_cfg[16] == 0x1010u;
        printf("переход в %06X: итог %d, стёрта %s, заголовок %zu байт %s, Reconfig %u, ПЛИС грузится "
               "с %06X, TAP в конце %s, выводы вернулись %s, неверных команд %u\n", (unsigned)want, r,
               erased ? "да" : "НЕТ", got_n, head ? "верен" : "НЕВЕРЕН", reconfigs, (unsigned)boot_from,
               tap == RTI ? "Run-Test/Idle" : "НЕ ТОТ", pins ? "да" : "НЕТ", bad_cmds);
        if (r != CORE_JTAG_OK || !erased || !head || reconfigs != 1 || boot_from != want || tap != RTI ||
            !pins || bad_cmds || edit_mode)
            bad = 1;
    }

    printf("%s\n", bad ? "ТЕСТ НЕ ПРОЙДЕН" : "ТЕСТ ПРОЙДЕН");
    return bad;
}
