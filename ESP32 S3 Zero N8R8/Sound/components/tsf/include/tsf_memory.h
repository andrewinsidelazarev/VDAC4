#pragma once

// Куда TinySoundFont и TinyMidiLoader берут память (tsf.c). По умолчанию —
// malloc/realloc/free libc; менять — до первой загрузки банка или песни.
// tsf_vdac4_rest — уступить ядро другим задачам в долгой работе (разбор песни и банка);
// NULL (по умолчанию) — не уступать.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void* (*tsf_vdac4_malloc)(size_t size);
extern void* (*tsf_vdac4_realloc)(void* pointer, size_t size);
extern void (*tsf_vdac4_free)(void* pointer);
extern void (*tsf_vdac4_rest)(void);

#ifdef __cplusplus
}
#endif
