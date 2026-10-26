/* Заглушка bouffalo_sdk для проверки fpga_link.c на ПК (link_test.c). */
#pragma once
#include "bflb_gpio.h"
struct bflb_spi_config_s {
    uint32_t freq;
    uint8_t role, mode, data_width, bit_order, byte_order, tx_fifo_threshold, rx_fifo_threshold;
};
#define SPI_ROLE_MASTER 0
#define SPI_MODE0 0
#define SPI_DATA_WIDTH_8BIT 0
#define SPI_DATA_WIDTH_32BIT 3
#define SPI_BIT_MSB 0
#define SPI_BYTE_LSB 0
#define SPI_CMD_SET_DATA_WIDTH 1
#define SPI_CMD_CLEAR_RX_FIFO 4
#define SPI_CMD_SET_FREQ 9
#define SPI_CMD_SET_CS_INTERVAL 5
void bflb_spi_init(struct bflb_device_s *dev, const struct bflb_spi_config_s *cfg);
int bflb_spi_feature_control(struct bflb_device_s *dev, int cmd, size_t arg);
int bflb_spi_poll_exchange(struct bflb_device_s *dev, const void *tx, void *rx, size_t n);
void bflb_spi_link_txdma(struct bflb_device_s *dev, bool enable);
void bflb_spi_link_rxdma(struct bflb_device_s *dev, bool enable);
