/* Заглушка bouffalo_sdk для проверки fpga_link.c на ПК (link_test.c). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct bflb_device_s { int id; };
struct bflb_device_s *bflb_device_get_by_name(const char *name);
#define GPIO_PIN_0  0
#define GPIO_PIN_1  1
#define GPIO_PIN_10 10
#define GPIO_PIN_12 12
#define GPIO_PIN_13 13
#define GPIO_PIN_14 14
#define GPIO_PIN_16 16
#define GPIO_PIN_27 27
#define GPIO_PIN_30 30
#define GPIO_FUNC_SPI0 0
#define GPIO_ALTERNATE 0
#define GPIO_PULLUP    0
#define GPIO_FLOAT     0
#define GPIO_SMT_EN    0
#define GPIO_DRV_3     0
#define GPIO_OUTPUT    0
#define GPIO_INPUT     0
void bflb_gpio_init(struct bflb_device_s *dev, uint8_t pin, uint32_t cfg);
void bflb_gpio_set(struct bflb_device_s *dev, uint8_t pin);
void bflb_gpio_reset(struct bflb_device_s *dev, uint8_t pin);
bool bflb_gpio_read(struct bflb_device_s *dev, uint8_t pin);
