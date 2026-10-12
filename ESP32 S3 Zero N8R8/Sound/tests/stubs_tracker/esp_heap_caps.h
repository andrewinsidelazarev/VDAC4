#pragma once

// PSRAM на ПК — обычная куча.

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_attr.h"

#define MALLOC_CAP_SPIRAM 0x0400
#define MALLOC_CAP_8BIT 0x0004
#define MALLOC_CAP_INTERNAL 0x0800

static inline void* heap_caps_malloc(size_t size, uint32_t caps) {
  (void)caps;
  return malloc(size);
}

static inline void* heap_caps_calloc(size_t count, size_t size, uint32_t caps) {
  (void)caps;
  return calloc(count, size);
}

static inline void heap_caps_free(void* pointer) { free(pointer); }
