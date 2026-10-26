/* Заглушка bouffalo_sdk для проверки core_cache.c на ПК (core_cache_test.c). */
#pragma once
#include <stdint.h>
uint32_t bflb_flash_get_size(void);
int bflb_flash_erase(uint32_t addr, uint32_t len);
int bflb_flash_write(uint32_t addr, uint8_t *data, uint32_t len);
int bflb_flash_read(uint32_t addr, uint8_t *data, uint32_t len);
