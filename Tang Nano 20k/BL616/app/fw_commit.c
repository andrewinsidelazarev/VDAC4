/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Запись нового образа во flash BL616 с 0x40000 и перезагрузка. Вся функция — в TCM и
 * вызывает только то, что тоже в TCM (bflb_flash_*, bflb_irq_save, GLB_SW_System_Reset):
 * после стирания на месте старой прошивки во flash уже ничего нашего нет, и вернуться туда
 * нельзя. Прерывания запрещены до самой перезагрузки. Сверка — чтением flash; не сошлось —
 * ещё две попытки. FPGA Partner с 0x0 не трогается; если образ всё же испорчен, BL616
 * восстанавливается по USB (кнопка UPDATE, bl616/README.md).
 */
#include "bflb_flash.h"
#include "bflb_irq.h"
#include "bl616_glb.h"

#include "fw_mailbox.h"
#include "fw_update.h"

static uint8_t chk[256];

/* без memcpy/memcmp — библиотека лежит во flash */
ATTR_TCM_SECTION __attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))
void fw_update_commit(const uint8_t *img, uint32_t size)
{
    (void)bflb_irq_save();
    uint32_t erase = (size + 4095u) & ~4095u;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (bflb_flash_erase(FWU_FLASH_ADDR, erase) != 0)
            continue;
        if (bflb_flash_write(FWU_FLASH_ADDR, (uint8_t *)img, size) != 0)
            continue;
        int ok = 1;
        for (uint32_t off = 0; off < size && ok; off += sizeof chk) {
            uint32_t n = size - off < sizeof chk ? size - off : (uint32_t)sizeof chk;
            if (bflb_flash_read(FWU_FLASH_ADDR + off, chk, n) != 0)
                ok = 0;
            for (uint32_t i = 0; i < n && ok; i++)
                if (chk[i] != img[off + i])
                    ok = 0;
        }
        if (ok)
            break;
    }
    GLB_SW_System_Reset();
    for (;;)
        ;
}
