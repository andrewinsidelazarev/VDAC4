/* SPDX-License-Identifier: Apache-2.0 */
/* Загрузка ядра в SRAM ПЛИС Tang по JTAG с BL616 (core_jtag.c, docs/next-core.md). */
#ifndef CORE_JTAG_H
#define CORE_JTAG_H

#include <stdint.h>

enum {
    CORE_JTAG_OK = 0,
    CORE_JTAG_NO_FPGA = 1,     /* IDCODE не GW2AR-18 */
    CORE_JTAG_NO_EDIT = 2,     /* ПЛИС не вошла в режим настройки (или не вышла) */
    CORE_JTAG_NO_ERASE = 3,    /* SRAM не стёрлась */
    CORE_JTAG_READ = 4,        /* источник образа не отдал кусок */
    CORE_JTAG_NOT_DONE = 5,    /* после записи нет DONE_FINAL (битый образ, CRC) */
    CORE_JTAG_BAD_SIZE = 6,
};

/* Кусок образа [off, off + n): 0 — прочитан. */
typedef int (*core_jtag_read_fn)(void *ctx, uint32_t off, uint8_t *buf, uint32_t n);

/* Двоичный битстрим Gowin (.bin) размером size — в SRAM ПЛИС; после загрузки ПЛИС сразу
   работает с новым ядром. usercode — UserCode загруженного ядра (может быть NULL). Проверку
   метки VDAC4 в образе делает вызывающий ДО загрузки (ядро для голой Tang Nano 20K может
   гнать выходы на линии Evo и ESP). */
int core_jtag_load(core_jtag_read_fn read, void *ctx, uint32_t size, uint32_t *usercode);
/* UserCode ядра, что работает в ПЛИС, — без вмешательства в его работу (сдвиг IR USERCODE и DR):
   CORE_JTAG_OK или CORE_JTAG_NO_FPGA. По метке VDAC4 (56 44 34 xx, xx ≥ 03) BL616 при запуске
   узнаёт ядро из кэша — у него нет ни ящика, ни сопроцессора. */
int core_jtag_usercode(uint32_t *usercode);

#endif
