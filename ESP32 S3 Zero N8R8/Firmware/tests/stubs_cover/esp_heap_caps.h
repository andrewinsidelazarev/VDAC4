#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_SPIRAM 0x0400
#define MALLOC_CAP_8BIT 0x0004
#define MALLOC_CAP_INTERNAL 0x0800
void* heap_caps_malloc(size_t, uint32_t);
void heap_caps_free(void*);
