/* Реализация TinySoundFont и TinyMidiLoader (заголовочные библиотеки) с
 * памятью через указатели tsf_memory.h. */

#include <stdlib.h>

#include "tsf_memory.h"

void* (*tsf_vdac4_malloc)(size_t size) = malloc;
void* (*tsf_vdac4_realloc)(void* pointer, size_t size) = realloc;
void (*tsf_vdac4_free)(void* pointer) = free;
void (*tsf_vdac4_rest)(void) = NULL;

#define TSF_MALLOC(size) tsf_vdac4_malloc(size)
#define TSF_REALLOC(pointer, size) tsf_vdac4_realloc(pointer, size)
#define TSF_FREE(pointer) tsf_vdac4_free(pointer)
#define TSF_REST() do { if (tsf_vdac4_rest) tsf_vdac4_rest(); } while (0)
#define TSF_NO_STDIO
#define TSF_IMPLEMENTATION
#include "tsf.h"

#define TML_MALLOC(size) tsf_vdac4_malloc(size)
#define TML_REALLOC(pointer, size) tsf_vdac4_realloc(pointer, size)
#define TML_FREE(pointer) tsf_vdac4_free(pointer)
#define TML_REST() do { if (tsf_vdac4_rest) tsf_vdac4_rest(); } while (0)
#define TML_NO_STDIO
#define TML_IMPLEMENTATION
#include "tml.h"
