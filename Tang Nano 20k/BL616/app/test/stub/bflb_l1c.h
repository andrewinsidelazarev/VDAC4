/* Заглушка bouffalo_sdk для проверки fpga_link.c на ПК (link_test.c). */
#pragma once
#include <stdint.h>
void bflb_l1c_dcache_clean_range(void *addr, uint32_t size);
