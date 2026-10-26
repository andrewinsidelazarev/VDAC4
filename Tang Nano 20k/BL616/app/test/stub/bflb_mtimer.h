/* Заглушка bouffalo_sdk для проверки fpga_link.c на ПК (link_test.c). */
#pragma once
#include <stdint.h>
uint64_t bflb_mtimer_get_time_ms(void);
uint64_t bflb_mtimer_get_time_us(void);
void bflb_mtimer_delay_ms(uint32_t ms);
void bflb_mtimer_delay_us(uint32_t us);
