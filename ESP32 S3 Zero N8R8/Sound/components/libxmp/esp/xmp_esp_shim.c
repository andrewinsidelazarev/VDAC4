/* VDAC4: libxmp в модуле ESP (TRACKER.ELF). Ядро прошивки модулям не даёт ни
 * файлового ввода-вывода, ни time(), ни таблицы ctype picolibc (_ctype_b), а
 * libxmp на них ссылается: здесь — «файлы» в памяти и своя таблица (только
 * ASCII, как у picolibc: смещение 128, бит UPPER 1, LOWER 2, DIGIT 4, SPACE 8,
 * PUNCT 16, CNTRL 32, HEX 64, BLANK 128).
 * Память libxmp — в PSRAM (xmp_malloc и прочие): обычный malloc берёт блоки до
 * 4 КБ из внутренней ОЗУ, а дорожек узоров у IT — сотни, и внутренняя ОЗУ (её
 * ждут Wi-Fi и обмен с Z80) кончилась бы. realloc ядро модулям не даёт —
 * длина блока лежит перед ним.
 * Модуль libxmp берёт из памяти или потоком (xmp_load_module_from_callbacks —
 * IT у TRACKER). Файл в памяти нужен ProWizard: упакованный модуль Amiga он
 * переводит в MOD во временный файл (make_temp_file) и читает его обратно —
 * буфер растёт по записи, fclose его освобождает. Открыть файл по имени нельзя
 * (fopen — NULL).
 * Сжатие отсчётов (xmp_esp_reduce, xmp_esp.h) — флаги для src/loaders/sample.c.
 * Имена — с приставкой xmp_ (ключи -D в CMakeLists): у обычной сборки
 * приложения модуля есть и libc, и одинаковые имена там столкнулись бы.
 * errno направлен на __errno ключом __PICOLIBC_ERRNO_FUNCTION, зерно случайных
 * чисел — без time() (src/rng.c, «VDAC4»). */

/* Здесь имена libc — настоящие (ключи -D переименовывают их только для
 * исходников libxmp, а заголовки ESP-IDF тянут stdio.h): xmp_* ниже определены
 * явно. */
#undef malloc
#undef calloc
#undef realloc
#undef free
#undef fread
#undef fseek
#undef ftell
#undef fclose
#undef feof
#undef ferror
#undef fgetc
#undef fwrite
#undef fputc
#undef fflush
#undef fopen
#undef _ctype_b
#undef getenv

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <esp_heap_caps.h>

#include "xmp_esp.h"

#ifdef XMP_ESP_HOST_BUDGET
/* Стенд ПК (tests/player_host.cpp): память libxmp — в счёт его PSRAM. */
void* xmp_host_alloc(size_t size);
void xmp_host_free(void* pointer);
#define heap_caps_malloc(size, caps) xmp_host_alloc(size)
#define heap_caps_free(pointer) xmp_host_free(pointer)
#endif

int libxmp_esp_reduce_flags;

void xmp_esp_reduce(int flags)
{
  libxmp_esp_reduce_flags = flags;
}

/* Блок: [длина, 8 байтов][данные]. Сначала PSRAM, нет места — любая память. */
#define XMP_BLOCK_HEAD 8

void* xmp_malloc(size_t size)
{
  unsigned char* block = (unsigned char*)heap_caps_malloc(size + XMP_BLOCK_HEAD,
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (block == NULL) {
    block = (unsigned char*)heap_caps_malloc(size + XMP_BLOCK_HEAD, MALLOC_CAP_8BIT);
  }
  if (block == NULL) {
    return NULL;
  }
  memcpy(block, &size, sizeof(size));
  return block + XMP_BLOCK_HEAD;
}

void xmp_free(void* pointer)
{
  if (pointer != NULL) {
    heap_caps_free((unsigned char*)pointer - XMP_BLOCK_HEAD);
  }
}

void* xmp_calloc(size_t count, size_t size)
{
  if (size != 0 && count > (size_t)-1 / size) {
    return NULL;
  }
  void* pointer = xmp_malloc(count * size);
  if (pointer != NULL) {
    memset(pointer, 0, count * size);
  }
  return pointer;
}

void* xmp_realloc(void* pointer, size_t size)
{
  if (pointer == NULL) {
    return xmp_malloc(size);
  }
  size_t old;
  memcpy(&old, (unsigned char*)pointer - XMP_BLOCK_HEAD, sizeof(old));
  void* moved = xmp_malloc(size);
  if (moved == NULL) {
    return NULL;
  }
  memcpy(moved, pointer, old < size ? old : size);
  xmp_free(pointer);
  return moved;
}

typedef struct {
  unsigned char* data;
  size_t size;      /* записано байтов */
  size_t capacity;
  size_t pos;
} xmp_memfile_t;

void* make_temp_file(char** name)
{
  if (name != NULL) {
    *name = NULL;
  }
  return xmp_calloc(1, sizeof(xmp_memfile_t));
}

void unlink_temp_file(char* name)
{
  (void)name;                       /* память освобождает fclose */
}

/* Окружения у модуля нет (XMP_INSTRUMENT_PATH — внешние инструменты). */
char* xmp_getenv(const char* name)
{
  (void)name;
  return NULL;
}

void* xmp_fopen(const char* path, const char* mode)
{
  (void)path; (void)mode;
  return NULL;
}

size_t xmp_fwrite(const void* buffer, size_t size, size_t count, void* file)
{
  xmp_memfile_t* f = (xmp_memfile_t*)file;
  const size_t bytes = size * count;
  if (f == NULL || bytes == 0) {
    return 0;
  }
  if (f->pos + bytes > f->capacity) {
    size_t capacity = f->capacity != 0 ? f->capacity : 65536;
    while (capacity < f->pos + bytes) {
      capacity *= 2;
    }
    unsigned char* data = (unsigned char*)xmp_realloc(f->data, capacity);
    if (data == NULL) {
      return 0;
    }
    f->data = data;
    f->capacity = capacity;
  }
  if (f->pos > f->size) {
    memset(f->data + f->size, 0, f->pos - f->size);
  }
  memcpy(f->data + f->pos, buffer, bytes);
  f->pos += bytes;
  if (f->pos > f->size) {
    f->size = f->pos;
  }
  return count;
}

int xmp_fputc(int c, void* file)
{
  const unsigned char byte = (unsigned char)c;
  return xmp_fwrite(&byte, 1, 1, file) == 1 ? byte : -1;
}

size_t xmp_fread(void* buffer, size_t size, size_t count, void* file)
{
  xmp_memfile_t* f = (xmp_memfile_t*)file;
  if (f == NULL || size == 0 || f->pos >= f->size) {
    return 0;
  }
  size_t items = (f->size - f->pos) / size;
  if (items > count) {
    items = count;
  }
  memcpy(buffer, f->data + f->pos, items * size);
  f->pos += items * size;
  return items;
}

int xmp_fgetc(void* file)
{
  unsigned char byte;
  return xmp_fread(&byte, 1, 1, file) == 1 ? byte : -1;
}

int xmp_fseek(void* file, long offset, int whence)
{
  xmp_memfile_t* f = (xmp_memfile_t*)file;
  if (f == NULL) {
    return -1;
  }
  long base = whence == 1 ? (long)f->pos : (whence == 2 ? (long)f->size : 0);   /* SEEK_CUR, SEEK_END */
  if (base + offset < 0) {
    return -1;
  }
  f->pos = (size_t)(base + offset);
  return 0;
}

long xmp_ftell(void* file)
{
  xmp_memfile_t* f = (xmp_memfile_t*)file;
  return f != NULL ? (long)f->pos : -1;
}

int xmp_feof(void* file)
{
  xmp_memfile_t* f = (xmp_memfile_t*)file;
  return f == NULL || f->pos >= f->size;
}

int xmp_ferror(void* file)
{
  return file == NULL;
}

int xmp_fflush(void* file)
{
  (void)file;
  return 0;
}

int xmp_fclose(void* file)
{
  xmp_memfile_t* f = (xmp_memfile_t*)file;
  if (f != NULL) {
    xmp_free(f->data);
    xmp_free(f);
  }
  return 0;
}

const char xmp_ctype_b[384] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x28, 0x28, 0x28, 0x28, 0x28, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x88, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    0x10, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x10, 0x10, 0x10, 0x10, 0x10,
    0x10, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x10, 0x10, 0x10, 0x10, 0x20,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
