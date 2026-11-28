#pragma once
// Заглушка кучи ESP-IDF: память — обычный malloc, свободного — постоянные числа стенда.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#define MALLOC_CAP_INTERNAL (1u << 11)
#define MALLOC_CAP_SPIRAM (1u << 10)
#define MALLOC_CAP_8BIT (1u << 2)
inline void* heap_caps_malloc(size_t size, uint32_t) { return malloc(size); }
inline void heap_caps_free(void* memory) { free(memory); }
inline size_t heap_caps_get_free_size(uint32_t caps) { return (caps & MALLOC_CAP_SPIRAM) ? 7000000 : 40000; }
inline size_t heap_caps_get_minimum_free_size(uint32_t caps) { return (caps & MALLOC_CAP_SPIRAM) ? 6000000 : 30000; }
