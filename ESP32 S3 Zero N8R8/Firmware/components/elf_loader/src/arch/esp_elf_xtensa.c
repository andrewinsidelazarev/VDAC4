/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <sys/errno.h>
#include "esp_elf.h"
#include "esp_log.h"
#include "private/elf_platform.h"

/** @brief Xtensa relocations defined by the ABIs */

#define R_XTENSA_NONE           0
#define R_XTENSA_32             1
#define R_XTENSA_RTLD           2
#define R_XTENSA_GLOB_DAT       3
#define R_XTENSA_JMP_SLOT       4
#define R_XTENSA_RELATIVE       5
#define R_XTENSA_PLT            6
#define R_XTENSA_OP0            8
#define R_XTENSA_OP1            9
#define R_XTENSA_OP2            10
#define R_XTENSA_ASM_EXPAND     11
#define R_XTENSA_ASM_SIMPLIFY   12
#define R_XTENSA_GNU_VTINHERIT  15
#define R_XTENSA_GNU_VTENTRY    16
#define R_XTENSA_DIFF8          17
#define R_XTENSA_DIFF16         18
#define R_XTENSA_DIFF32         19
#define R_XTENSA_SLOT0_OP       20
#define R_XTENSA_SLOT1_OP       21
#define R_XTENSA_SLOT2_OP       22
#define R_XTENSA_SLOT3_OP       23
#define R_XTENSA_SLOT4_OP       24
#define R_XTENSA_SLOT5_OP       25
#define R_XTENSA_SLOT6_OP       26
#define R_XTENSA_SLOT7_OP       27
#define R_XTENSA_SLOT8_OP       28
#define R_XTENSA_SLOT9_OP       29
#define R_XTENSA_SLOT10_OP      30
#define R_XTENSA_SLOT11_OP      31
#define R_XTENSA_SLOT12_OP      32
#define R_XTENSA_SLOT13_OP      33
#define R_XTENSA_SLOT14_OP      34
#define R_XTENSA_SLOT0_ALT      35
#define R_XTENSA_SLOT1_ALT      36
#define R_XTENSA_SLOT2_ALT      37
#define R_XTENSA_SLOT3_ALT      38
#define R_XTENSA_SLOT4_ALT      39
#define R_XTENSA_SLOT5_ALT      40
#define R_XTENSA_SLOT6_ALT      41
#define R_XTENSA_SLOT7_ALT      42
#define R_XTENSA_SLOT8_ALT      43
#define R_XTENSA_SLOT9_ALT      44
#define R_XTENSA_SLOT10_ALT     45
#define R_XTENSA_SLOT11_ALT     46
#define R_XTENSA_SLOT12_ALT     47
#define R_XTENSA_SLOT13_ALT     48
#define R_XTENSA_SLOT14_ALT     49

static const char *TAG = "elf_arch";

/**
 * @brief Relocates target architecture symbol of ELF
 *
 * @param elf  - ELF object pointer
 * @param rela - Relocated symbol data
 * @param sym  - ELF symbol table
 * @param addr - Jumping target address
 *
 * @return ESP_OK if success or other if failed.
 */
int esp_elf_arch_relocate(esp_elf_t *elf, const elf32_rela_t *rela,
                          const elf32_sym_t *sym, uint32_t addr)
{
    uint32_t val;
    uint32_t past;
    uint32_t *where;

    assert(elf && rela);

    where = (uint32_t *)esp_elf_map_sym(elf, rela->offset);

    ESP_LOGD(TAG, "type: %d, where=%p addr=0x%x offset=0x%x\n",
             ELF_R_TYPE(rela->info), where, (int)addr, (int)rela->offset);

    switch (ELF_R_TYPE(rela->info)) {
    case R_XTENSA_RELATIVE:
        /* VDAC4: указатель «за последний элемент» массива в конце секции не попадает ни
           в одну секцию (esp_elf_map_sym берёт полуоткрытый интервал), и раньше он
           становился 0. Компилятор делает такой указатель концом цикла по таблице: в
           MIDI.ELF 1.7 таблица модуляторов лежала последней в .rodata, цикл бежал по
           памяти до ошибки кэша, и ESP сбрасывалась при загрузке банка. Конец секции
           переводим по её последнему байту и прибавляем 1 (так и для .text: его адреса
           переносятся на шину команд). Значение, равное концу одной секции и началу
           другой, остаётся началом: так — указатели на первый объект секции. */
        past = 0;
        val = esp_elf_map_sym(elf, *where);
        if (!val && *where) {
            val = esp_elf_map_sym(elf, *where - 1);
            past = val ? 1 : 0;
        }
#ifdef CONFIG_ELF_LOADER_CACHE_OFFSET
        *where = elf_remap_text(elf, val) + past;
#else
        *where  = val + past;
#endif
        break;
    case R_XTENSA_RTLD:
        break;
    case R_XTENSA_GLOB_DAT:
    case R_XTENSA_JMP_SLOT:
#ifdef CONFIG_ELF_LOADER_CACHE_OFFSET
        *where = elf_remap_text(elf, addr);
#else
        *where = addr;
#endif
        break;
    default:
        ESP_LOGE(TAG, "info=%d is not supported", ELF_R_TYPE(rela->info));
        return -EINVAL;
    }

    return 0;
}
