#pragma once

// Заглушка OPL для трекера TS-Labs. Каналы AdLib модулей S3M исполнял бы
// эмулятор YMF262 прошивки TS-Labs (opl.cpp); здесь его нет: OPL «включён»,
// но opl_is_enabled() отвечает нулём, и трекер в него не пишет.

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

typedef enum
{
  OPL_MODE_OFF = 0,
  OPL_MODE_YMF262,
  OPL_MODE_YMF278
} OPL_MODE;

typedef enum
{
  OPL_FM_RENDER_FULL = 0,
  OPL_FM_RENDER_OPL2_MELODY,
  OPL_FM_RENDER_2OP_ACTIVE
} OPL_FM_RENDER_MODE;

typedef struct
{
  u16 reg;
  u8 value;
} OPL_FM_REG_WRITE;

typedef struct
{
  OPL_MODE mode;
  u8 enabled;
  size_t rom_size;
  u32 rom_crc32;
  size_t ram_size;
  u8 fm_latch_bank;
  u8 fm_latch[2];
  u8 wave_latch;
  u8 status;
  u32 queued_writes;
} OPL_INFO;

static inline esp_err_t opl_enable(OPL_MODE mode, size_t ram_size)
{
  (void)mode;
  (void)ram_size;
  return ESP_OK;
}

static inline void opl_reset() {}

static inline int opl_is_enabled() { return 0; }

static inline esp_err_t opl_get_info(OPL_INFO *info)
{
  if (info)
  {
    *info = OPL_INFO();
    info->mode = OPL_MODE_YMF262;
    info->enabled = 1;
  }
  return ESP_OK;
}

static inline u8 opl_read_fm_reg(u16 reg)
{
  (void)reg;
  return 0;
}

static inline void opl_write_fm_reg(u16 reg, u8 value)
{
  (void)reg;
  (void)value;
}

static inline esp_err_t opl_write_fm_regs(const OPL_FM_REG_WRITE *writes, size_t count)
{
  (void)writes;
  (void)count;
  return ESP_OK;
}

static inline esp_err_t opl_set_fm_render_mode(OPL_FM_RENDER_MODE mode)
{
  (void)mode;
  return ESP_OK;
}
