/* Заглушка bouffalo_sdk для проверки fpga_link.c на ПК (link_test.c): DMA сразу «передаёт». */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "bflb_gpio.h"
struct bflb_dma_channel_config_s {
    uint8_t direction;
    uint32_t src_req, dst_req;
    uint8_t src_addr_inc, dst_addr_inc, src_burst_count, dst_burst_count, src_width, dst_width;
};
struct bflb_dma_channel_lli_pool_s { uint32_t a, b, c, d; };
struct bflb_dma_channel_lli_transfer_s { uint32_t src_addr, dst_addr, nbytes; };
#define DMA_MEMORY_TO_PERIPH 1
#define DMA_PERIPH_TO_MEMORY 2
#define DMA_REQUEST_NONE 0
#define DMA_REQUEST_SPI0_TX 0x0B
#define DMA_REQUEST_SPI0_RX 0x0A
#define DMA_ADDR_INCREMENT_ENABLE 1
#define DMA_ADDR_INCREMENT_DISABLE 0
#define DMA_BURST_INCR1 0
#define DMA_DATA_WIDTH_32BIT 2
#define DMA_ADDR_SPI0_TDR 0x2000A288u
#define DMA_ADDR_SPI0_RDR 0x2000A28Cu
void bflb_dma_channel_init(struct bflb_device_s *dev, const struct bflb_dma_channel_config_s *cfg);
int bflb_dma_channel_lli_reload(struct bflb_device_s *dev, struct bflb_dma_channel_lli_pool_s *pool, uint32_t pool_size,
                                struct bflb_dma_channel_lli_transfer_s *transfer, uint32_t count);
void bflb_dma_channel_start(struct bflb_device_s *dev);
void bflb_dma_channel_stop(struct bflb_device_s *dev);
bool bflb_dma_channel_isbusy(struct bflb_device_s *dev);
