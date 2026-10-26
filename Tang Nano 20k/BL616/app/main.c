/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Прошивка BL616 модуля Tang Nano 20K для VDAC4: сопроцессор FT812 (bl616/copro). Кольцо
 * RAM_CMD, дисплей-лист, RAM_G и регистры — в ПЛИС (fpga/vdac2, ft_core); BL616 читает и
 * пишет их по SPI (fpga_link.c) и исполняет команды. Ставится вторичной прошивкой с 0x40000
 * за FPGA Partner (как FPGA Companion MiSTle) — USB-JTAG Tang при этом остаётся.
 *
 * Работа: опрос сопроцессора, пока у него есть дело; без дела — ждать IRQ_N от ПЛИС (кольцо
 * не пусто или был сброс сопроцессора), но не дольше 10 мс. Раз в полсекунды — слово
 * состояния для ESP (LINK 0x30, CP_INFO): {C0, версия, ошибка, состояние, счётчик}.
 * Раз в 50 мс — почтовый ящик ПЛИС: новая прошивка от ESP (fw_update.c); пока она идёт,
 * сопроцессор стоит, а CP_INFO — ответы обновления {C1, …}.
 * С прошивки 9 — самопроверка частот SPI (fpga_link.c, fpga_speed_test): пока ядро FT812
 * загружено и сопроцессор свободен; итог — в CP_INFO, разряды 11…10: 0 — ещё не было (запись
 * 40 МГц), 2 — запись 40 МГц, 3 — 80; отчёт — в RAM_G (FPGA_REPORT_ADDR).
 * С прошивки 4 — перезагрузка ПЛИС по просьбе ядра (бит 7 флагов STATUS, ядро 1.13+): JTAG
 * Reconfig с выводов BL616 (fpga_jtag.c) — так работает multiboot (слоты flash Tang).
 * С прошивки 14 — кэш ядер во flash BL616 (core_cache.c, docs/next-core.md): ESP кладёт ядро
 * (ZX Next и другие, которых нет во flash Tang) через тот же почтовый ящик, BL616 грузит его в
 * SRAM ПЛИС по JTAG (core_jtag.c); пока работает ядро из кэша, BL616 только ждёт его просьбу о
 * перезагрузке — назад в слот 0.
 */
#include <stdio.h>

#include "bflb_mtimer.h"
#include "board.h"

#ifndef FTC_BRIDGE
#include "core_cache.h"
#include "core_jtag.h"
#include "core_mailbox.h"
#endif
#include "fpga_jtag.h"
#include "fpga_link.h"
#include "ft_copro.h"
#include "fw_update.h"

/* 8 — номер отвергнутой сборки Codex (bl616/_rejected_codex_2026-10-19); 9 принимала по сети
   только образы меньше ~70 КБ (образ — в malloc, куча после декодера картинок мала); 10 — мост
   (FTC_BRIDGE): только обновление по сети, без сопроцессора — маленький образ, который 9
   принимает; с него ставится 11; 12 — учёт очереди ПЛИС (адрес посылки и запрос чтения —
   тоже места, разбор Codex 20.10.2026), готовность чтения — только по годному снимку; 13 —
   сглаживание 3D SSAA 2×2 (3D версии 2, формат CMD_3D_TARGET | 0x100); 14 — кэш ядер во flash
   BL616 и их загрузка в ПЛИС по JTAG; 15 — по разбору Codex 21.10.2026: при запуске с ядром из
   кэша в ПЛИС (UserCode) — ящик не трогать, вести ядро; ожидание чтения и места — с пределом;
   пустой каталог — во flash до первой записи; отказ JTAG до настройки — без перезагрузки ПЛИС;
   16 — метрики шрифтов CMD_TEXT с проверкой и кэшем ПЗУ-шрифтов: чтение ПЛИС под нагрузкой
   (нули по пределу ожидания) клало все знаки строки в одну точку (R-Type, 21.10.2026) */
#ifdef FTC_BRIDGE
#define FW_VERSION 0x0A
#else
#define FW_VERSION 0x18 /* v22 renderer and fast SPI transport restored after v23 comparison */
#endif

#ifndef FTC_BRIDGE
static ftc_t cop;
#endif

int main(void)
{
    board_init();
    printf("VDAC4: сопроцессор FT812, версия %d\r\n", FW_VERSION);

    while (fpga_link_init() != 0)
        bflb_mtimer_delay_ms(100);          /* ядро VDAC4 ещё не загружено */
    printf("связь с ПЛИС есть\r\n");

    ftc_bus_t bus;
    fpga_bus(&bus);
#ifndef FTC_BRIDGE
    /* В ПЛИС уже ядро из кэша (сброс одного BL616 при работающем ZX Next): у него нет ящика —
       чтение ящика ждало бы готовности вечно. Сперва вести его (возврат по BOOT или молчанию),
       ящик, обновление и сопроцессор — когда ПЛИС снова в ядре VDAC4. */
    uint32_t uc = 0;
    if (core_jtag_usercode(&uc) == CORE_JTAG_OK && (uc >> 8) == CC_MARK && (uc & 0xFFu) >= 0x03u) {
        printf("в ПЛИС ядро из кэша (UserCode %08X): ждать возврата в слот 0\r\n", (unsigned)uc);
        core_cache_adopt(&bus, (uint32_t)bflb_mtimer_get_time_ms());
        while (core_cache_poll((uint32_t)bflb_mtimer_get_time_ms()))
            ;
    }
#endif
    fw_update_init(&bus);
#ifndef FTC_BRIDGE
    core_cache_init(&bus);
    ftc_init(&cop, &bus);
    fw_update_set_buffer(FTC_SCRATCH(&cop), (uint32_t)FTC_SCRATCH_BYTES);   /* образ — в общую рабочую память */
#endif

    uint64_t t_info = 0, t_boot = 0, t_spd = 0;
    uint32_t beat = 0;
    int busy = 0, spd = 0;
    for (;;) {
        uint64_t now = bflb_mtimer_get_time_ms();
#ifndef FTC_BRIDGE
        if (core_cache_poll((uint32_t)now))
            continue;                       /* запись ядра в кэш или в ПЛИС — ядро из кэша */
#endif
        if (fw_update_poll((uint32_t)now))
            continue;                       /* идёт обновление прошивки: сопроцессор стоит */
#ifndef FTC_BRIDGE
        if (fw_update_clobbered())
            cop.scratch_dirty = 1;          /* обновление брошено: общая рабочая память испорчена */
        busy = ftc_poll(&cop);
#endif
        now = bflb_mtimer_get_time_ms();
        if (fpga_boot_requested() && (t_boot == 0 || now - t_boot >= 2000)) {
            /* ядро просит перезагрузку (BOOT от ESP, автопереход multiboot): свой RECONFIG_N
               ПЛИС не перезагружает — JTAG Reconfig; пока ПЛИС грузится, снимков STATUS нет */
            t_boot = now;
            printf("ПЛИС просит перезагрузку: JTAG Reconfig\r\n");
            fpga_jtag_reconfig();
            continue;
        }
#ifndef FTC_BRIDGE
        if (spd <= 0 && !busy && now - t_spd >= 1000) {
            t_spd = now;
            int r = fpga_speed_test();
            spd = r > 0 ? r : 0;
            if (r > 0)
                printf("SPI: запись %u МГц\r\n", (unsigned)(fpga_write_hz() / 1000000u));
        }
#endif
        if (now - t_info >= 500 && !fw_update_owns_info((uint32_t)now)
#ifndef FTC_BRIDGE
            && !core_cache_owns_info((uint32_t)now)
#endif
        ) {
            t_info = now;
#ifdef FTC_BRIDGE
            uint32_t state = 0, fault = 0;
#else
            uint32_t state = cop.held ? 2u : busy ? 1u : 0u, fault = (uint32_t)cop.fault & 15u;
#endif
            fpga_info(0xC0000000u | ((uint32_t)FW_VERSION << 16) | (fault << 12) |
                      ((uint32_t)spd << 10) | (state << 8) | (beat++ & 0xFFu));
        }
        if (!busy)
            while (!fpga_irq() && bflb_mtimer_get_time_ms() - now < 10)
                ;
    }
}
