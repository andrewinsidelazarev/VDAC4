// ============================================================================
// Includes
// ============================================================================

// -------------------- Primary project header --------------------
#include "tracker.h"

// -------------------- C / POSIX headers --------------------
#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

// -------------------- ESP-IDF headers --------------------
#include "esp_heap_caps.h"
#include "esp_log.h"

// -------------------- Заглушки вместо модулей TS-Labs --------------------
// OPL (каналы AdLib в S3M): в этой прошивке FM-синтеза нет, такие каналы
// молчат. Память контекста — в PSRAM, как у TS-Labs.
#include "opl_stub.h"

// Модуль ESP TRACKER.ELF (modules/tracker): загрузчик elf_loader кладёт только
// .text, .rodata, .data, .data.rel.ro и .bss, секций .iram1 он не загружает —
// ссылки на них вели в никуда, и ESP падала при связывании модуля. В модуле
// весь код — в .text.
#ifdef TRACKER_ELF_MODULE
#undef IRAM_ATTR
#define IRAM_ATTR
#endif

static void *malloc_spiram(size_t size)
{
  void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(size);
}

// ============================================================================
// Common tracker helpers
// ============================================================================


// -------------------- Common byte order helpers --------------------

uint16_t tracker_rd_be16(const uint8_t *p)
{
  return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

uint16_t tracker_rd_le16(const uint8_t *p)
{
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t tracker_rd_le24(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

uint32_t tracker_rd_le32(const uint8_t *p)
{
  return tracker_rd_le16(p) | ((uint32_t)tracker_rd_le16(p + 2) << 16);
}

// -------------------- Common size helpers --------------------

int tracker_size_add(size_t *dst, size_t add)
{
  if (!dst) return 0;
  if (SIZE_MAX - *dst < add) return 0;
  *dst += add;
  return 1;
}

int tracker_size_mul(size_t a, size_t b, size_t *out)
{
  if (!out) return 0;
  if (a && b > SIZE_MAX / a) return 0;
  *out = a * b;
  return 1;
}

// -------------------- Common string/memory helpers --------------------

void tracker_copy_trimmed(char *dst, size_t dst_size, const uint8_t *src, size_t src_size)
{
  size_t n;

  if (!dst || !dst_size) return;

  n = src_size;
  while (n && (src[n - 1] == 0 || src[n - 1] == ' ')) n--;
  if (n >= dst_size) n = dst_size - 1;

  if (n) memcpy(dst, src, n);
  dst[n] = 0;
}

void tracker_memcpy_pad(void *dst, size_t dst_len, const void *src, size_t src_len, size_t offset)
{
  uint8_t *dst_c = (uint8_t*)dst;
  const uint8_t *src_c = (const uint8_t*)src;
  size_t copy_bytes;

  if (!dst || !dst_len) return;

  copy_bytes = (src_len >= offset) ? (src_len - offset) : 0;
  if (copy_bytes > dst_len) copy_bytes = dst_len;

  if (copy_bytes) memcpy(dst_c, src_c + offset, copy_bytes);
  memset(dst_c + copy_bytes, 0, dst_len - copy_bytes);
}

// ============================================================================
// XM context / public API
// ============================================================================

#define OFFSET(ptr) do {										\
		(ptr) = reinterpret_cast<decltype(ptr)>((intptr_t)(ptr) + (intptr_t)(*ctxp));	\
	} while(0)


int xm_group_sizes_add(xm_context_group_sizes_t *sizes, size_t *field, size_t add);
int xm_group_sizes_add_mul(xm_context_group_sizes_t *sizes, size_t *field, size_t a, size_t b);
int xm_measure_group_sizes_from_memory(const char *moddata, size_t moddata_length, xm_context_group_sizes_t *sizes);
void *xm_group_cursor_alloc(char **cursor, char *end, size_t size, bool clear);
void *xm_alloc_and_register_group(xm_context_t *ctx, size_t size, uint8_t type, MFUNC mfunc, bool clear);
int xm_allocate_context_groups(xm_context_t **ctxp, xm_context_group_cursor_t *cursor, const xm_context_group_sizes_t *sizes, MFUNC mfunc);
int xm_setup_runtime_group(xm_context_t *ctx, xm_context_group_cursor_t *cursor, uint32_t rate);

/* VDAC4: отсчёты XM — прямо в буфере файла (xm_create_context_in_place). Пока
 * разбирается модуль, здесь копятся места отсчётов; после разбора они
 * сдвигаются и распаковываются (xm_place_samples). */
typedef struct {
  xm_sample_t* sample;
  size_t source;          /* смещение данных в файле */
  size_t target;          /* куда они встанут (кратно 4 от начала буфера) */
} xm_inplace_sample_t;

static uint8_t* xm_inplace_base = NULL;
static size_t xm_inplace_capacity = 0;
static xm_inplace_sample_t* xm_inplace_list = NULL;
static size_t xm_inplace_count = 0;
static size_t xm_inplace_limit = 0;

static int xm_create_context_common(xm_context_t** ctxp, void* moddata, size_t moddata_length,
                                    uint32_t rate, MFUNC mfunc);

int xm_create_context_in_place(xm_context_t** ctxp, void* moddata, size_t moddata_length,
                               size_t capacity, uint32_t rate, MFUNC mfunc)
{
  xm_inplace_base = (uint8_t*)moddata;
  xm_inplace_capacity = capacity;
  xm_inplace_count = 0;
  /* Отсчётов не больше, чем байтов файла на заголовок отсчёта (40). */
  xm_inplace_limit = moddata_length / 40 + 1;
  xm_inplace_list = (xm_inplace_sample_t*)malloc(xm_inplace_limit * sizeof(xm_inplace_sample_t));
  int result = -2;
  if (xm_inplace_list) result = xm_create_context_common(ctxp, moddata, moddata_length, rate, mfunc);
  free(xm_inplace_list);
  xm_inplace_list = NULL;
  xm_inplace_base = NULL;
  return result;
}

int xm_create_context_safe(xm_context_t** ctxp, void* moddata, size_t moddata_length, uint32_t rate, MFUNC mfunc)
{
  return xm_create_context_common(ctxp, moddata, moddata_length, rate, mfunc);
}

static int xm_create_context_common(xm_context_t** ctxp, void* moddata, size_t moddata_length,
                                    uint32_t rate, MFUNC mfunc)
{
  xm_context_group_sizes_t sizes;
  xm_context_group_cursor_t cursor;
  xm_context_t *ctx;

  if (ctxp) *ctxp = NULL;
  if (!ctxp || !moddata || !mfunc) return -1;

  if (XM_DEFENSIVE)
  {
    int ret = xm_check_sanity_preload((const char*)moddata, moddata_length);
    if (ret)
    {
      DEBUG("xm_check_sanity_preload() returned %i, module is not safe to load", ret);
      return -1;
    }
  }

  if (!xm_measure_group_sizes_from_memory((const char*)moddata, moddata_length, &sizes)) return -2;
  if (xm_inplace_base)
  {
    /* Отсчёты остаются в буфере файла: группа под них не нужна. */
    sizes.total_size -= sizes.samples_size;
    sizes.samples_size = 0;
  }

  if (!xm_allocate_context_groups(ctxp, &cursor, &sizes, mfunc)) return -2;

  ctx = *ctxp;
  ctx->rate = rate;

  if (!xm_load_module(ctx, (const char*)moddata, moddata_length, &cursor))
  {
    xm_free_context(ctx);
    *ctxp = NULL;
    return -2;
  }

  if (!xm_setup_runtime_group(ctx, &cursor, rate))
  {
    xm_free_context(ctx);
    *ctxp = NULL;
    return -2;
  }

  if (XM_DEFENSIVE)
  {
    int ret = xm_check_sanity_postload(ctx);
    if (ret)
    {
      DEBUG("xm_check_sanity_postload() returned %i, module is not safe to play", ret);
      xm_free_context(ctx);
      *ctxp = NULL;
      return -1;
    }
  }

  return (int)sizes.total_size;
}

void xm_create_context_from_libxmize(xm_context_t** ctxp, char* libxmized, uint32_t rate) {
	size_t i, j;

	*ctxp = (xm_context_t*)libxmized;

	/* Reverse steps of libxmize.c */
	OFFSET((*ctxp)->module.patterns);
	OFFSET((*ctxp)->module.instruments);
	OFFSET((*ctxp)->row_loop_count);
	OFFSET((*ctxp)->channels);

	for(i = 0; i < (*ctxp)->module.num_patterns; ++i) {
		OFFSET((*ctxp)->module.patterns[i].slots);
	}

	for(i = 0; i < (*ctxp)->module.num_instruments; ++i) {
		OFFSET((*ctxp)->module.instruments[i].samples);

		for(j = 0; j < (*ctxp)->module.instruments[i].num_samples; ++j) {
			OFFSET((*ctxp)->module.instruments[i].samples[j].data8);

			if(XM_LIBXMIZE_DELTA_SAMPLES) {
				if((*ctxp)->module.instruments[i].samples[j].length > 1) {
					if((*ctxp)->module.instruments[i].samples[j].bits == 8) {
						for(size_t k = 1; k < (*ctxp)->module.instruments[i].samples[j].length; ++k) {
							(*ctxp)->module.instruments[i].samples[j].data8[k] += (*ctxp)->module.instruments[i].samples[j].data8[k-1];
						}
					} else {
						for(size_t k = 1; k < (*ctxp)->module.instruments[i].samples[j].length; ++k) {
							(*ctxp)->module.instruments[i].samples[j].data16[k] += (*ctxp)->module.instruments[i].samples[j].data16[k-1];
						}
					}
				}
			}
		}
	}
}

struct tracker_context_segment_page_s
{
  tracker_context_segment_page_t *next;
  uint16_t used;
  tracker_context_segment_t segments[TRACKER_CONTEXT_SEGMENT_PAGE_CAPACITY];
};

int tracker_context_has_registered_segments(const xm_context_t *ctx)
{
  if (!ctx) return 0;
  if (ctx->ctx_size < offsetof(xm_context_t, tracker_segment_pages) + sizeof(ctx->tracker_segment_pages)) return 0;
  if (ctx->tracker_segments_magic != TRACKER_CONTEXT_SEGMENTS_MAGIC) return 0;
  if (ctx->tracker_segments_magic_check != TRACKER_CONTEXT_SEGMENTS_MAGIC_CHECK) return 0;
  if (!ctx->tracker_segment_pages) return 0;

  return ctx->tracker_segment_count > 0;
}

uint16_t tracker_context_get_segment_count(const xm_context_t *ctx)
{
  if (!tracker_context_has_registered_segments(ctx)) return 0;
  return ctx->tracker_segment_count;
}

int tracker_context_get_segment(const xm_context_t *ctx, uint16_t index, tracker_context_segment_t *segment)
{
  uint16_t base = 0;

  if (!segment) return 0;
  memset(segment, 0, sizeof(*segment));
  if (!tracker_context_has_registered_segments(ctx)) return 0;
  if (index >= ctx->tracker_segment_count) return 0;

  for (tracker_context_segment_page_t *page = ctx->tracker_segment_pages; page; page = page->next)
  {
    if (index < base + page->used)
    {
      *segment = page->segments[index - base];
      return 1;
    }

    base += page->used;
  }

  return 0;
}

int tracker_context_register_segment(xm_context_t *ctx, void *addr, size_t size, uint8_t type)
{
  tracker_context_segment_page_t *page;
  tracker_context_segment_page_t *last_page = NULL;

  if (!ctx || !addr || !size) return 0;
  if (ctx->ctx_size < offsetof(xm_context_t, tracker_segment_pages) + sizeof(ctx->tracker_segment_pages)) return 0;

  if (ctx->tracker_segments_magic != TRACKER_CONTEXT_SEGMENTS_MAGIC || ctx->tracker_segments_magic_check != TRACKER_CONTEXT_SEGMENTS_MAGIC_CHECK)
  {
    if (ctx->tracker_segments_magic || ctx->tracker_segments_magic_check || ctx->tracker_segment_count || ctx->tracker_segment_pages) return 0;

    ctx->tracker_segments_magic = TRACKER_CONTEXT_SEGMENTS_MAGIC;
    ctx->tracker_segments_magic_check = TRACKER_CONTEXT_SEGMENTS_MAGIC_CHECK;
  }

  for (page = ctx->tracker_segment_pages; page; page = page->next)
  {
    last_page = page;

    for (uint16_t i = 0; i < page->used; i++)
    {
      if (page->segments[i].addr == addr)
      {
        page->segments[i].size = size;
        page->segments[i].type = type;
        page->segments[i].flags = 0;
        page->segments[i].index = 0xffff;
        return 1;
      }
    }
  }

  if (ctx->tracker_segment_count == UINT16_MAX) return 0;

  if (!last_page || last_page->used >= TRACKER_CONTEXT_SEGMENT_PAGE_CAPACITY)
  {
    page = (tracker_context_segment_page_t*)malloc_spiram(sizeof(tracker_context_segment_page_t));
    if (!page) return 0;
    memset(page, 0, sizeof(*page));

    if (last_page)
      last_page->next = page;
    else
      ctx->tracker_segment_pages = page;

    last_page = page;
  }

  tracker_context_segment_t *seg = last_page->segments + last_page->used;
  seg->addr = addr;
  seg->size = size;
  seg->type = type;
  seg->flags = 0;
  seg->index = 0xffff;

  last_page->used++;
  ctx->tracker_segment_count++;

  return 1;
}

void tracker_context_free_segments(xm_context_t *ctx)
{
  tracker_context_segment_page_t *page;
  tracker_context_segment_page_t *next_page;
  if (!ctx) return;

  if (!tracker_context_has_registered_segments(ctx))
  {
    free(ctx);
    return;
  }

  page = ctx->tracker_segment_pages;

  for (tracker_context_segment_page_t *p = page; p; p = p->next)
  {
    for (uint16_t i = 0; i < p->used; i++)
    {
      void *addr = p->segments[i].addr;
      if (!addr) continue;
      if (addr == ctx) continue;

      free(addr);
      p->segments[i].addr = NULL;
    }
  }

  free(ctx);

  while (page)
  {
    next_page = page->next;
    free(page);
    page = next_page;
  }
}

void xm_free_context(xm_context_t* context)
{
  tracker_context_free_segments(context);
}

void xm_set_max_loop_count(xm_context_t* context, uint8_t loopcnt) {
	context->max_loop_count = loopcnt;
}

uint8_t xm_get_loop_count(xm_context_t* context) {
	return context->loop_count;
}

static void xm_tick(xm_context_t*);

/* VDAC4: идёт прогон xm_scan_song — звук не нужен: частоты не считаются, а
 * отсчёты не трогаются (EFy переворачивает петлю в самих данных). */
static bool xm_scanning = false;

uint32_t xm_scan_song(xm_context_t* ctx, uint32_t* order_ms, uint32_t limit_ms) {
	const size_t channels_size = (size_t)ctx->module.num_channels * sizeof(xm_channel_context_t);
	const size_t loops_size = (size_t)ctx->module.length * MAX_NUM_ROWS * sizeof(uint8_t);
	uint8_t* saved = (uint8_t*)malloc(sizeof(xm_context_t) + channels_size + loops_size);
	if(saved == NULL) return 0;
	memcpy(saved, ctx, sizeof(xm_context_t));
	memcpy(saved + sizeof(xm_context_t), ctx->channels, channels_size);
	memcpy(saved + sizeof(xm_context_t) + channels_size, ctx->row_loop_count, loops_size);
	if(order_ms != NULL) {
		for(int i = 0; i < PATTERN_ORDER_TABLE_LENGTH; ++i) order_ms[i] = UINT32_MAX;
	}

	const double limit = (double)limit_ms * ctx->rate / 1000.0;
	double samples = 0;
	xm_scanning = true;
	while(ctx->loop_count == 0 && samples < limit) {
		/* Номер позиции libxm меняет за несколько тиков до конца строки: начало
		 * позиции — первый тик 0 с её номером. */
		if(order_ms != NULL && ctx->current_tick == 0 && order_ms[ctx->current_table_index] == UINT32_MAX) {
			order_ms[ctx->current_table_index] = (uint32_t)(samples * 1000.0 / ctx->rate);
		}
		xm_tick(ctx);
		samples += ctx->remaining_samples_in_tick;
		ctx->remaining_samples_in_tick = 0;
	}
	xm_scanning = false;
	const bool ended = ctx->loop_count > 0;

	memcpy(ctx, saved, sizeof(xm_context_t));
	memcpy(ctx->channels, saved + sizeof(xm_context_t), channels_size);
	memcpy(ctx->row_loop_count, saved + sizeof(xm_context_t) + channels_size, loops_size);
	free(saved);
	return ended ? (uint32_t)(samples * 1000.0 / ctx->rate) : 0;
}

void xm_seek(xm_context_t* ctx, uint8_t pot, uint8_t row, uint16_t tick) {
	ctx->current_table_index = pot;
	ctx->current_row = row;
	ctx->current_tick = tick;
	ctx->remaining_samples_in_tick = 0;
}

bool xm_mute_channel(xm_context_t* ctx, uint16_t channel, bool mute) {
	bool old = ctx->channels[channel - 1].muted;
	ctx->channels[channel - 1].muted = mute;
	return old;
}

bool xm_mute_instrument(xm_context_t* ctx, uint16_t instr, bool mute) {
	bool old = ctx->module.instruments[instr - 1].muted;
	ctx->module.instruments[instr - 1].muted = mute;
	return old;
}

#if XM_STRINGS
const char* xm_get_module_name(xm_context_t* ctx) {
	return ctx->module.name;
}

const char* xm_get_tracker_name(xm_context_t* ctx) {
	return ctx->module.trackername;
}
#else
const char* xm_get_module_name(xm_context_t* ctx) {
	return NULL;
}

const char* xm_get_tracker_name(xm_context_t* ctx) {
	return NULL;
}
#endif

uint16_t xm_get_number_of_channels(xm_context_t* ctx) {
	return ctx->module.num_channels;
}

uint16_t xm_get_module_length(xm_context_t* ctx) {
	return ctx->module.length;
}

uint16_t xm_get_number_of_patterns(xm_context_t* ctx) {
	return ctx->module.num_patterns;
}

uint16_t xm_get_number_of_rows(xm_context_t* ctx, uint16_t pattern) {
	return ctx->module.patterns[pattern].num_rows;
}

uint16_t xm_get_number_of_instruments(xm_context_t* ctx) {
	return ctx->module.num_instruments;
}

uint16_t xm_get_number_of_samples(xm_context_t* ctx, uint16_t instrument) {
	return ctx->module.instruments[instrument - 1].num_samples;
}

void* xm_get_sample_waveform(xm_context_t* ctx, uint16_t i, uint16_t s, size_t* size, uint8_t* bits) {
	*size = ctx->module.instruments[i - 1].samples[s].length;
	*bits = ctx->module.instruments[i - 1].samples[s].bits;
	return ctx->module.instruments[i - 1].samples[s].data8;
}

void xm_get_playing_speed(xm_context_t* ctx, uint16_t* bpm, uint16_t* tempo) {
	if(bpm) *bpm = ctx->bpm;
	if(tempo) *tempo = ctx->tempo;
}

void xm_get_position(xm_context_t* ctx, uint8_t* pattern_index, uint8_t* pattern, uint8_t* row, uint64_t* samples) {
	if(pattern_index) *pattern_index = ctx->current_table_index;
	if(pattern) *pattern = ctx->module.pattern_table[ctx->current_table_index];
	if(row) *row = ctx->current_row;
	if(samples) *samples = ctx->generated_samples;
}

uint64_t xm_get_latest_trigger_of_instrument(xm_context_t* ctx, uint16_t instr) {
	return ctx->module.instruments[instr - 1].latest_trigger;
}

uint64_t xm_get_latest_trigger_of_sample(xm_context_t* ctx, uint16_t instr, uint16_t sample) {
	return ctx->module.instruments[instr - 1].samples[sample].latest_trigger;
}

uint64_t xm_get_latest_trigger_of_channel(xm_context_t* ctx, uint16_t chn) {
	return ctx->channels[chn - 1].latest_trigger;
}

bool xm_is_channel_active(xm_context_t* ctx, uint16_t chn) {
	xm_channel_context_t* ch = ctx->channels + (chn - 1);
	return ch->instrument != NULL && ch->sample != NULL && ch->sample_position >= 0;
}

float xm_get_frequency_of_channel(xm_context_t* ctx, uint16_t chn) {
	return ctx->channels[chn - 1].frequency;
}

float xm_get_volume_of_channel(xm_context_t* ctx, uint16_t chn) {
	return ctx->channels[chn - 1].actual_volume * ctx->global_volume;
}

float xm_get_panning_of_channel(xm_context_t* ctx, uint16_t chn) {
	return ctx->channels[chn - 1].actual_panning;
}

uint16_t xm_get_instrument_of_channel(xm_context_t* ctx, uint16_t chn) {
	xm_channel_context_t* ch = ctx->channels + (chn - 1);
	if(ch->instrument == NULL) return 0;
	return 1 + (ch->instrument - ctx->module.instruments);
}

// ============================================================================
// XM module loader
// ============================================================================

/* Bounded reader macros.
 * If we attempt to read the buffer out-of-bounds, pretend that the buffer is
 * infinitely padded with zeroes.
 */
#define READ_U8(offset) (((offset) < moddata_length) ? (*(uint8_t*)(moddata + (offset))) : 0)
#define READ_U16(offset) ((uint16_t)READ_U8(offset) | ((uint16_t)READ_U8((offset) + 1) << 8))
#define READ_U32(offset) ((uint32_t)READ_U16(offset) | ((uint32_t)READ_U16((offset) + 2) << 16))
#define READ_MEMCPY(ptr, offset, length) tracker_memcpy_pad(ptr, length, moddata, moddata_length, offset)

int xm_group_sizes_add(xm_context_group_sizes_t *sizes, size_t *field, size_t add)
{
  if (!sizes || !field) return 0;
  if (!tracker_size_add(field, add)) return 0;
  if (!tracker_size_add(&sizes->total_size, add)) return 0;
  return 1;
}

int xm_group_sizes_add_mul(xm_context_group_sizes_t *sizes, size_t *field, size_t a, size_t b)
{
  size_t bytes;

  if (!tracker_size_mul(a, b, &bytes)) return 0;
  return xm_group_sizes_add(sizes, field, bytes);
}

/* VDAC4: сколько узоров таблица порядка (в пределах длины песни) называет
 * сверх записанных в файле. FT2 играет такие узоры пустыми на 64 строки, а
 * libxm такой модуль отвергал (в коллекции XM — 48 файлов из 3073). */
static uint16_t xm_missing_patterns(const char *moddata, size_t moddata_length)
{
  const uint16_t song_length = READ_U16(60 + 4);
  const uint16_t num_patterns = READ_U16(60 + 10);
  uint16_t needed = num_patterns;
  for (uint16_t i = 0; i < song_length && i < PATTERN_ORDER_TABLE_LENGTH; i++)
  {
    const uint8_t pattern = READ_U8(60 + 20 + i);
    if (pattern >= needed)
      needed = (uint16_t)(pattern + 1);
  }
  return (uint16_t)(needed - num_patterns);
}

int xm_measure_group_sizes_from_memory(const char *moddata, size_t moddata_length, xm_context_group_sizes_t *sizes)
{
  size_t offset;
  uint16_t num_channels;
  uint16_t num_patterns;
  uint16_t num_instruments;

  if (!moddata || !sizes) return 0;
  memset(sizes, 0, sizeof(*sizes));

  if (!xm_group_sizes_add(sizes, &sizes->context_size, sizeof(xm_context_t))) return 0;

  offset = 60;
  num_channels = READ_U16(offset + 8);
  num_patterns = READ_U16(offset + 10);
  num_instruments = READ_U16(offset + 12);

  /* VDAC4: и недостающие узоры — заголовки и одна общая пустая ячейка-сетка на 64 строки. */
  const uint16_t missing = xm_missing_patterns(moddata, moddata_length);
  if (!xm_group_sizes_add_mul(sizes, &sizes->patterns_size, (size_t)num_patterns + missing, sizeof(xm_pattern_t))) return 0;
  if (missing && !xm_group_sizes_add_mul(sizes, &sizes->patterns_size, (size_t)64 * num_channels, sizeof(xm_pattern_slot_t))) return 0;
  if (!xm_group_sizes_add_mul(sizes, &sizes->instruments_size, num_instruments, sizeof(xm_instrument_t))) return 0;
  if (!xm_group_sizes_add_mul(sizes, &sizes->runtime_size, num_channels, sizeof(xm_channel_context_t))) return 0;
  if (!xm_group_sizes_add_mul(sizes, &sizes->runtime_size, READ_U16(offset + 4), MAX_NUM_ROWS * sizeof(uint8_t))) return 0;

  offset += READ_U32(offset);

  for (uint16_t i = 0; i < num_patterns; i++)
  {
    uint16_t num_rows = READ_U16(offset + 5);
    uint16_t packed_pattern_size = READ_U16(offset + 7);
    size_t slot_count;

    if (!tracker_size_mul(num_rows, num_channels, &slot_count)) return 0;
    if (!xm_group_sizes_add_mul(sizes, &sizes->patterns_size, slot_count, sizeof(xm_pattern_slot_t))) return 0;
    if (!tracker_size_add(&offset, READ_U32(offset))) return 0;
    if (!tracker_size_add(&offset, packed_pattern_size)) return 0;
  }

  for (uint16_t i = 0; i < num_instruments; i++)
  {
    uint16_t num_samples = READ_U16(offset + 27);
    uint32_t sample_header_size = 0;
    size_t sample_size_aggregate = 0;

    if (!xm_group_sizes_add_mul(sizes, &sizes->instruments_size, num_samples, sizeof(xm_sample_t))) return 0;

    if (num_samples > 0) sample_header_size = READ_U32(offset + 29);
    if (!tracker_size_add(&offset, READ_U32(offset))) return 0;

    for (uint16_t j = 0; j < num_samples; j++)
    {
      uint32_t sample_size = READ_U32(offset);
      if (!tracker_size_add(&sample_size_aggregate, sample_size)) return 0;
      /* VDAC4: +3 — каждый отсчёт кладётся с адреса, кратного 4 (xm_load_module). */
      if (!xm_group_sizes_add(sizes, &sizes->samples_size, (size_t)sample_size + 3)) return 0;
      if (!tracker_size_add(&offset, sample_header_size)) return 0;
    }

    if (!tracker_size_add(&offset, sample_size_aggregate)) return 0;
  }

  return sizes->total_size <= INT_MAX;
}

void *xm_group_cursor_alloc(char **cursor, char *end, size_t size, bool clear)
{
  void *ptr;

  if (!cursor || !*cursor || !end) return NULL;
  if (*cursor > end) return NULL;
  if (size > (size_t)(end - *cursor)) return NULL;

  ptr = *cursor;
  *cursor += size;
  if (clear && size) memset(ptr, 0, size);
  return ptr;
}

void *xm_alloc_and_register_group(xm_context_t *ctx, size_t size, uint8_t type, MFUNC mfunc, bool clear)
{
  void *ptr;

  if (!ctx || !mfunc) return NULL;
  if (!size) return NULL;

  ptr = mfunc(size);
  if (!ptr) return NULL;
  if (clear) memset(ptr, 0, size);

  if (!tracker_context_register_segment(ctx, ptr, size, type))
  {
    free(ptr);
    return NULL;
  }

  return ptr;
}

int xm_allocate_context_groups(xm_context_t **ctxp, xm_context_group_cursor_t *cursor, const xm_context_group_sizes_t *sizes, MFUNC mfunc)
{
  xm_context_t *ctx;
  void *patterns = NULL;
  void *instruments = NULL;
  void *samples = NULL;
  void *runtime = NULL;
  void *format_extra = NULL;

  if (ctxp) *ctxp = NULL;
  if (!ctxp || !cursor || !sizes || !mfunc) return 0;
  memset(cursor, 0, sizeof(*cursor));

  ctx = (xm_context_t*)mfunc(sizeof(xm_context_t));
  if (!ctx) return 0;
  memset(ctx, 0, sizeof(*ctx));
  ctx->ctx_size = sizes->total_size;

  if (!tracker_context_register_segment(ctx, ctx, sizeof(xm_context_t), TRACKER_CONTEXT_SEG_CONTEXT))
  {
    free(ctx);
    return 0;
  }

  *ctxp = ctx;

  patterns = xm_alloc_and_register_group(ctx, sizes->patterns_size, TRACKER_CONTEXT_SEG_PATTERNS, mfunc, true);
  if (sizes->patterns_size && !patterns) goto fail;

  instruments = xm_alloc_and_register_group(ctx, sizes->instruments_size, TRACKER_CONTEXT_SEG_INSTRUMENTS, mfunc, true);
  if (sizes->instruments_size && !instruments) goto fail;

  samples = xm_alloc_and_register_group(ctx, sizes->samples_size, TRACKER_CONTEXT_SEG_SAMPLES, mfunc, false);
  if (sizes->samples_size && !samples) goto fail;

  runtime = xm_alloc_and_register_group(ctx, sizes->runtime_size, TRACKER_CONTEXT_SEG_RUNTIME, mfunc, true);
  if (sizes->runtime_size && !runtime) goto fail;

  format_extra = xm_alloc_and_register_group(ctx, sizes->format_extra_size, TRACKER_CONTEXT_SEG_FORMAT_EXTRA, mfunc, true);
  if (sizes->format_extra_size && !format_extra) goto fail;

  cursor->patterns = (char*)patterns;
  cursor->patterns_end = cursor->patterns ? cursor->patterns + sizes->patterns_size : NULL;
  cursor->instruments = (char*)instruments;
  cursor->instruments_end = cursor->instruments ? cursor->instruments + sizes->instruments_size : NULL;
  cursor->samples = (char*)samples;
  cursor->samples_end = cursor->samples ? cursor->samples + sizes->samples_size : NULL;
  cursor->runtime = (char*)runtime;
  cursor->runtime_end = cursor->runtime ? cursor->runtime + sizes->runtime_size : NULL;
  cursor->format_extra = (char*)format_extra;
  cursor->format_extra_end = cursor->format_extra ? cursor->format_extra + sizes->format_extra_size : NULL;

  return 1;

fail:
  xm_free_context(ctx);
  *ctxp = NULL;
  memset(cursor, 0, sizeof(*cursor));
  return 0;
}

int xm_setup_runtime_group(xm_context_t *ctx, xm_context_group_cursor_t *cursor, uint32_t rate)
{
  if (!ctx || !cursor) return 0;

  ctx->channels = (xm_channel_context_t*)xm_group_cursor_alloc(&cursor->runtime, cursor->runtime_end, ctx->module.num_channels * sizeof(xm_channel_context_t), true);
  if (!ctx->channels) return 0;

  ctx->rate = rate;
  ctx->global_volume = 1.f;
  ctx->amplification = .25f;

#if XM_RAMPING
  ctx->volume_ramp = (1.f / 128.f);
  ctx->panning_ramp = (1.f / 128.f);
#endif

  for (uint8_t i = 0; i < ctx->module.num_channels; i++)
  {
    xm_channel_context_t *ch = ctx->channels + i;
    ch->ping = true;
    ch->vibrato_waveform = XM_SINE_WAVEFORM;
    ch->vibrato_waveform_retrigger = true;
    ch->tremolo_waveform = XM_SINE_WAVEFORM;
    ch->tremolo_waveform_retrigger = true;
    ch->panbrello_waveform = XM_SINE_WAVEFORM;
    ch->panbrello_waveform_retrigger = true;
    ch->filter_cutoff = 127;   /* VDAC4: фильтра нет */
    ch->volume = ch->volume_envelope_volume = ch->fadeout_volume = 1.0f;
    ch->channel_volume = 1.0f;
    ch->panning = ch->panning_envelope_panning = .5f;
    ch->default_panning = -1.f;
    ch->tracker_format = TRACKER_FORMAT_XM;
    ch->period_note_offset = .0f;
    ch->actual_volume = .0f;
    ch->actual_panning = .5f;
  }

  ctx->row_loop_count = (uint8_t*)xm_group_cursor_alloc(&cursor->runtime, cursor->runtime_end, ctx->module.length * MAX_NUM_ROWS * sizeof(uint8_t), true);
  if (!ctx->row_loop_count) return 0;

  return 1;
}

int xm_check_sanity_preload(const char* module, size_t module_length) {
	if(module_length < 60) {
		return 4;
	}

	if(memcmp("Extended Module: ", module, 17) != 0) {
		return 1;
	}

	if(module[37] != 0x1A) {
		return 2;
	}

	if(module[59] != 0x01 || module[58] != 0x04) {
		/* Not XM 1.04 */
		return 3;
	}

	return 0;
}

int xm_check_sanity_postload(xm_context_t* ctx) {
	/* @todo: plenty of stuff to do here… */

	/* Check the POT */
	/* VDAC4: счётчик 16-битный — при длине 256 восьмибитный оборачивался, и проверка
	 * не кончалась (сторожевой таймер). */
	for(uint16_t i = 0; i < ctx->module.length; ++i) {
		if(ctx->module.pattern_table[i] >= ctx->module.num_patterns) {
			if(i+1 == ctx->module.length && ctx->module.length > 1) {
				/* Cheap fix */
				--ctx->module.length;
				DEBUG("trimming invalid POT at pos %X", i);
			} else {
				DEBUG("module has invalid POT, pos %X references nonexistent pattern %X",
				      i,
				      ctx->module.pattern_table[i]);
				return 1;
			}
		}
	}

	return 0;
}

size_t xm_get_memory_needed_for_context(const char* moddata, size_t moddata_length)
{
  xm_context_group_sizes_t sizes;

  if (!xm_measure_group_sizes_from_memory(moddata, moddata_length, &sizes)) return 0;
  return sizes.total_size;
}

static int xm_place_samples(size_t moddata_length);

int xm_load_module(xm_context_t* ctx, const char* moddata, size_t moddata_length, xm_context_group_cursor_t *cursor) {
	size_t offset = 0;
	xm_module_t* mod = &(ctx->module);

	ctx->tracker_format = TRACKER_FORMAT_XM;

	/* Read XM header */
#if XM_STRINGS
	READ_MEMCPY(mod->name, offset + 17, MODULE_NAME_LENGTH);
	READ_MEMCPY(mod->trackername, offset + 38, TRACKER_NAME_LENGTH);
#endif
	offset += 60;

	/* Read module header */
	uint32_t header_size = READ_U32(offset);

	mod->length = READ_U16(offset + 4);
	mod->restart_position = READ_U16(offset + 6);
	/* VDAC4: таблица порядка — PATTERN_ORDER_TABLE_LENGTH (256) позиций, а длина песни в
	 * заголовке 16-битная: больше 256 — чтение и проигрывание шли бы за таблицу. */
	if(mod->length > PATTERN_ORDER_TABLE_LENGTH) mod->length = PATTERN_ORDER_TABLE_LENGTH;
	if(mod->restart_position >= mod->length) mod->restart_position = 0;
	mod->num_channels = READ_U16(offset + 8);
	mod->num_patterns = READ_U16(offset + 10);
	mod->num_instruments = READ_U16(offset + 12);

	const uint16_t missing_patterns = xm_missing_patterns(moddata, moddata_length); /* VDAC4 */
	size_t patterns_size = ((size_t)mod->num_patterns + missing_patterns) * sizeof(xm_pattern_t);
	mod->patterns = patterns_size ? (xm_pattern_t*)xm_group_cursor_alloc(&cursor->patterns, cursor->patterns_end, patterns_size, true) : NULL;
	if(patterns_size && !mod->patterns) return 0;

	size_t instruments_size = mod->num_instruments * sizeof(xm_instrument_t);
	mod->instruments = instruments_size ? (xm_instrument_t*)xm_group_cursor_alloc(&cursor->instruments, cursor->instruments_end, instruments_size, true) : NULL;
	if(instruments_size && !mod->instruments) return 0;

	uint16_t flags = READ_U32(offset + 14);
	mod->frequency_type = (flags & (1 << 0)) ? XM_LINEAR_FREQUENCIES : XM_AMIGA_FREQUENCIES;

	ctx->tempo = READ_U16(offset + 16);
	ctx->bpm = READ_U16(offset + 18);

	READ_MEMCPY(mod->pattern_table, offset + 20, PATTERN_ORDER_TABLE_LENGTH);
	offset += header_size;

	/* Read patterns */
	for(uint16_t i = 0; i < mod->num_patterns; ++i) {
		uint16_t packed_patterndata_size = READ_U16(offset + 7);
		xm_pattern_t* pat = mod->patterns + i;

		pat->num_rows = READ_U16(offset + 5);

		size_t slots_size = mod->num_channels * pat->num_rows * sizeof(xm_pattern_slot_t);
		pat->slots = slots_size ? (xm_pattern_slot_t*)xm_group_cursor_alloc(&cursor->patterns, cursor->patterns_end, slots_size, true) : NULL;
		if(slots_size && !pat->slots) return 0;

		/* Pattern header length */
		offset += READ_U32(offset);

		if(packed_patterndata_size == 0) {
			/* No pattern data is present */
		} else {
			/* This isn't your typical for loop */
			const size_t slot_count = (size_t)mod->num_channels * pat->num_rows;
			for(uint16_t j = 0, k = 0; j < packed_patterndata_size; ++k) {
				/* VDAC4: данных узора больше, чем ячеек, — испорченный файл: дальше не писать. */
				if(k >= slot_count) break;
				uint8_t note = READ_U8(offset + j);
				xm_pattern_slot_t* slot = pat->slots + k;

				if(note & (1 << 7)) {
					/* MSB is set, this is a compressed packet */
					++j;

					if(note & (1 << 0)) {
						/* Note follows */
						slot->note = READ_U8(offset + j);
						++j;
					} else {
						slot->note = 0;
					}

					if(note & (1 << 1)) {
						/* Instrument follows */
						slot->instrument = READ_U8(offset + j);
						++j;
					} else {
						slot->instrument = 0;
					}

					if(note & (1 << 2)) {
						/* Volume column follows */
						slot->volume_column = READ_U8(offset + j);
						++j;
					} else {
						slot->volume_column = 0;
					}

					if(note & (1 << 3)) {
						/* Effect follows */
						slot->effect_type = READ_U8(offset + j);
						++j;
					} else {
						slot->effect_type = 0;
					}

					if(note & (1 << 4)) {
						/* Effect parameter follows */
						slot->effect_param = READ_U8(offset + j);
						++j;
					} else {
						slot->effect_param = 0;
					}
				} else {
					/* Uncompressed packet */
					slot->note = note;
					slot->instrument = READ_U8(offset + j + 1);
					slot->volume_column = READ_U8(offset + j + 2);
					slot->effect_type = READ_U8(offset + j + 3);
					slot->effect_param = READ_U8(offset + j + 4);
					j += 5;
				}
				/* VDAC4: эффекты за X (33) — расширения OpenMPT, а коды 34 и
				 * дальше внутри трекера заняты командами S3M (34 — скорость,
				 * 35 — темп): Y (34) — панбрелло, Z (35) и \ (36, плавный
				 * макрос — здесь сразу) — макрос MIDI (фильтр), прочее — без
				 * эффекта. */
				if(slot->effect_type == 34) {
					slot->effect_type = XM_EFFECT_S3M_PANBRELLO;
				} else if(slot->effect_type == 35 || slot->effect_type == 36) {
					slot->effect_type = XM_EFFECT_S3M_MIDI_MACRO;
				} else if(slot->effect_type > 36) {
					slot->effect_type = 0;
					slot->effect_param = 0;
				}
			}
		}

		offset += packed_patterndata_size;
	}

	/* VDAC4: недостающие узоры — пустые на 64 строки, сетка ячеек у всех одна. */
	if(missing_patterns) {
		const size_t empty_size = (size_t)64 * mod->num_channels * sizeof(xm_pattern_slot_t);
		xm_pattern_slot_t* empty = empty_size ? (xm_pattern_slot_t*)xm_group_cursor_alloc(&cursor->patterns, cursor->patterns_end, empty_size, true) : NULL;
		if(empty_size && !empty) return 0;
		for(uint16_t i = mod->num_patterns; i < mod->num_patterns + missing_patterns; ++i) {
			mod->patterns[i].num_rows = 64;
			mod->patterns[i].slots = empty;
		}
		mod->num_patterns += missing_patterns;
	}

	/* Read instruments */
	for(uint16_t i = 0; i < ctx->module.num_instruments; ++i) {
		uint32_t sample_header_size = 0;
		xm_instrument_t* instr = mod->instruments + i;

#if XM_STRINGS
		READ_MEMCPY(instr->name, offset + 4, INSTRUMENT_NAME_LENGTH);
#endif
	    instr->num_samples = READ_U16(offset + 27);

		if(instr->num_samples > 0) {
			/* Read extra header properties */
			sample_header_size = READ_U32(offset + 29);
			READ_MEMCPY(instr->sample_of_notes, offset + 33, NUM_NOTES);

			instr->volume_envelope.num_points = READ_U8(offset + 225);
			instr->panning_envelope.num_points = READ_U8(offset + 226);
			/* VDAC4: точек у FT2 не больше NUM_ENVELOPE_POINTS (12); больше — файл испорчен,
			 * и запись шла бы за массив points. Такая огибающая выключается ниже. */
			const bool volume_points_bad = instr->volume_envelope.num_points > NUM_ENVELOPE_POINTS;
			const bool panning_points_bad = instr->panning_envelope.num_points > NUM_ENVELOPE_POINTS;
			if(volume_points_bad) instr->volume_envelope.num_points = 0;
			if(panning_points_bad) instr->panning_envelope.num_points = 0;

			for(uint8_t j = 0; j < instr->volume_envelope.num_points; ++j) {
				instr->volume_envelope.points[j].frame = READ_U16(offset + 129 + 4 * j);
				instr->volume_envelope.points[j].value = READ_U16(offset + 129 + 4 * j + 2);
			}

			for(uint8_t j = 0; j < instr->panning_envelope.num_points; ++j) {
				instr->panning_envelope.points[j].frame = READ_U16(offset + 177 + 4 * j);
				instr->panning_envelope.points[j].value = READ_U16(offset + 177 + 4 * j + 2);
			}

			instr->volume_envelope.sustain_point = READ_U8(offset + 227);
			instr->volume_envelope.loop_start_point = READ_U8(offset + 228);
			instr->volume_envelope.loop_end_point = READ_U8(offset + 229);

			instr->panning_envelope.sustain_point = READ_U8(offset + 230);
			instr->panning_envelope.loop_start_point = READ_U8(offset + 231);
			instr->panning_envelope.loop_end_point = READ_U8(offset + 232);

			uint8_t flags = READ_U8(offset + 233);
			instr->volume_envelope.enabled = flags & (1 << 0);
			instr->volume_envelope.sustain_enabled = flags & (1 << 1);
			instr->volume_envelope.loop_enabled = flags & (1 << 2);

			flags = READ_U8(offset + 234);
			instr->panning_envelope.enabled = flags & (1 << 0);
			instr->panning_envelope.sustain_enabled = flags & (1 << 1);
			instr->panning_envelope.loop_enabled = flags & (1 << 2);

			/* VDAC4: номера точек sustain и петли — индексы points при проигрывании; вне
			 * огибающей — выключены, а огибающая с лишними точками — целиком. */
			xm_envelope_t* const envelopes[2] = {&instr->volume_envelope, &instr->panning_envelope};
			const bool points_bad[2] = {volume_points_bad, panning_points_bad};
			for(int e = 0; e < 2; ++e) {
				xm_envelope_t* env = envelopes[e];
				if(points_bad[e]) env->enabled = false;
				if(env->sustain_point >= env->num_points) env->sustain_enabled = false;
				if(env->loop_start_point >= env->num_points || env->loop_end_point >= env->num_points)
					env->loop_enabled = false;
			}

			instr->vibrato_type = (xm_waveform_type_t)READ_U8(offset + 235);
			if(instr->vibrato_type == 2) {
				instr->vibrato_type = XM_RAMP_DOWN_WAVEFORM;
			} else if(instr->vibrato_type == 1) {
				instr->vibrato_type = XM_SQUARE_WAVEFORM;
			}
			instr->vibrato_sweep = READ_U8(offset + 236);
			instr->vibrato_depth = READ_U8(offset + 237);
			instr->vibrato_rate = READ_U8(offset + 238);
			instr->volume_fadeout = READ_U16(offset + 239);

			instr->samples = (xm_sample_t*)xm_group_cursor_alloc(&cursor->instruments, cursor->instruments_end, instr->num_samples * sizeof(xm_sample_t), true);
			if(!instr->samples) return 0;
		} else {
			instr->samples = NULL;
		}

		/* Instrument header size */
		offset += READ_U32(offset);
		const size_t sample_headers = offset; /* VDAC4: длины отсчётов в байтах — отсюда */

		for(uint16_t j = 0; j < instr->num_samples; ++j) {
			/* Read sample header */
			xm_sample_t* sample = instr->samples + j;

			sample->length = READ_U32(offset);
			sample->loop_start = READ_U32(offset + 4);
			sample->loop_length = READ_U32(offset + 8);
			sample->loop_end = sample->loop_start + sample->loop_length;
			sample->volume = (float)READ_U8(offset + 12) / (float)0x40;
			sample->finetune = (int8_t)READ_U8(offset + 13);

			uint8_t flags = READ_U8(offset + 14);
			if((flags & 3) == 0) {
				sample->loop_type = XM_NO_LOOP;
			} else if((flags & 3) == 1) {
				sample->loop_type = XM_FORWARD_LOOP;
			} else {
				sample->loop_type = XM_PING_PONG_LOOP;
			}

			sample->bits = (flags & (1 << 4)) ? 16 : 8;

			sample->panning = (float)READ_U8(offset + 15) / (float)0xFF;
			sample->relative_note = (int8_t)READ_U8(offset + 16);
#if XM_STRINGS
			READ_MEMCPY(sample->name, offset + 18, SAMPLE_NAME_LENGTH);
#endif
			if(sample->length > 0 && xm_inplace_base) {
				sample->data8 = NULL;    /* место — после разбора (xm_place_samples) */
			} else if(sample->length > 0) {
				/* VDAC4: отсчёты лежат подряд в одной группе, и 16-битный после
				 * 8-битного нечётной длины начинался с нечётного адреса. На x86
				 * это проходит, а ESP32-S3 на записи data16[k] падает
				 * (LoadStoreAlignment): XM вешал прошивку. Каждый — с адреса,
				 * кратного 4; запас учтён в xm_measure_group_sizes_from_memory. */
				cursor->samples = (char*)(((uintptr_t)cursor->samples + 3) & ~(uintptr_t)3);
				sample->data8 = (int8_t*)xm_group_cursor_alloc(&cursor->samples, cursor->samples_end, sample->length, false);
				if(!sample->data8) return 0;
			} else {
				sample->data8 = NULL;
			}

			if(sample->bits == 16) {
				sample->loop_start >>= 1;
				sample->loop_length >>= 1;
				sample->loop_end >>= 1;
				sample->length >>= 1;
			}

			/* VDAC4: петля из заголовка — в пределах отсчёта. Петля нулевой
			 * длины (такие XM встречаются) вешала звук: при проигрывании
			 * «while (pos >= loop_end) pos -= loop_length» не кончается. */
			if(sample->loop_type != XM_NO_LOOP) {
				if(sample->loop_start >= sample->length) {
					sample->loop_length = 0;
				} else if(sample->loop_length > sample->length - sample->loop_start) {
					sample->loop_length = sample->length - sample->loop_start;
				}
				if(sample->loop_length == 0) {
					sample->loop_type = XM_NO_LOOP;
				}
				sample->loop_end = sample->loop_start + sample->loop_length;
			}

			offset += sample_header_size;
		}

		for(uint16_t j = 0; j < instr->num_samples; ++j) {
			/* Read sample data */
			xm_sample_t* sample = instr->samples + j;
			uint32_t length = sample->length;
			/* VDAC4: дальше — на длину отсчёта в байтах из его заголовка. У
			 * 16-битного нечётной длины (FT2 такое пишет) length << 1 на байт
			 * короче, и всё после него читалось со сдвигом: модуль отвергался. */
			uint32_t bytes = READ_U32(sample_headers + (size_t)j * sample_header_size);

			if(xm_inplace_base) {
				if(length > 0) {
					if(xm_inplace_count >= xm_inplace_limit) return 0;
					xm_inplace_list[xm_inplace_count].sample = sample;
					xm_inplace_list[xm_inplace_count].source = offset;
					++xm_inplace_count;
				}
			} else if(sample->bits == 16) {
				int16_t v = 0;
				for(uint32_t k = 0; k < length; ++k) {
					v = v + (int16_t)READ_U16(offset + (k << 1));
					sample->data16[k] = v;
				}
			} else {
				int8_t v = 0;
				for(uint32_t k = 0; k < length; ++k) {
					v = v + (int8_t)READ_U8(offset + k);
					sample->data8[k] = v;
				}
			}
			offset += bytes;
		}
	}

	if(xm_inplace_base && !xm_place_samples(moddata_length)) return 0;
	return 1;
}

/* VDAC4: отсчёты — на своих местах в буфере файла. Каждый встаёт с адреса,
 * кратного 4 (16-битный с нечётного ESP32-S3 не читает), поэтому данные
 * сдвигаются вверх, в запас за концом файла: с последнего отсчёта к первому,
 * так что ни один ещё не сдвинутый не затирается (заголовки уже разобраны).
 * Потом дельты распаковываются на месте, вперёд. */
static int xm_place_samples(size_t moddata_length)
{
	size_t shift = 0;
	for(size_t i = 0; i < xm_inplace_count; ++i) {
		xm_inplace_sample_t* e = &xm_inplace_list[i];
		xm_sample_t* sample = e->sample;
		size_t size = sample->bits == 16 ? (size_t)sample->length * 2 : sample->length;
		if(e->source >= moddata_length) {
			sample->length = 0;
			size = 0;
		} else if(size > moddata_length - e->source) {
			/* Файл обрезан: отсчёт — сколько есть. */
			size = moddata_length - e->source;
			sample->length = sample->bits == 16 ? (uint32_t)(size / 2) : (uint32_t)size;
			size = sample->bits == 16 ? (size_t)sample->length * 2 : sample->length;
			if(sample->loop_end > sample->length) {
				sample->loop_type = XM_NO_LOOP;
				sample->loop_start = sample->loop_length = sample->loop_end = 0;
			}
		}
		size_t target = e->source + shift;
		const size_t pad = (4 - (((uintptr_t)xm_inplace_base + target) & 3)) & 3;
		shift += pad;
		e->target = target + pad;
		if(e->target + size > xm_inplace_capacity) return 0;
	}
	for(size_t i = xm_inplace_count; i-- > 0;) {
		xm_inplace_sample_t* e = &xm_inplace_list[i];
		size_t size = e->sample->bits == 16 ? (size_t)e->sample->length * 2 : e->sample->length;
		if(size && e->target != e->source) memmove(xm_inplace_base + e->target, xm_inplace_base + e->source, size);
	}
	for(size_t i = 0; i < xm_inplace_count; ++i) {
		xm_inplace_sample_t* e = &xm_inplace_list[i];
		xm_sample_t* sample = e->sample;
		uint8_t* data = xm_inplace_base + e->target;
		if(sample->length == 0) {
			sample->data8 = NULL;
		} else if(sample->bits == 16) {
			int16_t v = 0;
			int16_t* out = (int16_t*)data;
			for(uint32_t k = 0; k < sample->length; ++k) {
				v = (int16_t)(v + (int16_t)(data[2 * k] | (data[2 * k + 1] << 8)));
				out[k] = v;
			}
			sample->data16 = out;
		} else {
			int8_t v = 0;
			for(uint32_t k = 0; k < sample->length; ++k) {
				v = (int8_t)(v + (int8_t)data[k]);
				data[k] = (uint8_t)v;
			}
			sample->data8 = (int8_t*)data;
		}
	}
	return 1;
}

// ============================================================================
// MOD module loader
// ============================================================================

#define MOD_STREAM_READ_OK 0

int mod_read_layout(const void *moddata, size_t moddata_length, mod_layout_t *layout);

uint16_t mod_channels_from_magic(const uint8_t *magic)
{
  if (!magic) return 0;

  if (!memcmp(magic, "M.K.", 4)) return 4;
  if (!memcmp(magic, "M!K!", 4)) return 4;
  if (!memcmp(magic, "M&K!", 4)) return 4;
  if (!memcmp(magic, "N.T.", 4)) return 4;
  if (!memcmp(magic, "FLT4", 4)) return 4;
  if (!memcmp(magic, "EXO4", 4)) return 4;

  if (magic[0] >= '1' && magic[0] <= '9' && magic[1] == 'C' && magic[2] == 'H' && magic[3] == 'N')
    return magic[0] - '0';

  if (magic[0] >= '0' && magic[0] <= '9' && magic[1] >= '0' && magic[1] <= '9')
  {
    uint16_t channels = (uint16_t)((magic[0] - '0') * 10 + (magic[1] - '0'));
    if ((magic[2] == 'C' && magic[3] == 'H') || (magic[2] == 'C' && magic[3] == 'N'))
      return channels;
  }

  return 0;
}

uint16_t mod_get_channel_count(const void *moddata, size_t moddata_length)
{
  mod_layout_t layout;

  if (!mod_read_layout(moddata, moddata_length, &layout)) return 0;
  return layout.channels;
}

int mod_finish_layout(const uint8_t *header, size_t header_length, size_t moddata_length, mod_layout_t *layout)
{
  uint8_t max_pattern = 0;
  uint8_t max_table_pattern = 0;
  uint16_t min_patterns;
  uint16_t table_patterns;
  size_t pattern_end;
  size_t sample_avail;
  size_t order_offset;
  size_t pattern_block_bytes;

  if (!header || !layout) return 0;
  if (!layout->sample_count || layout->sample_count > MOD_SAMPLE_COUNT) return 0;
  if (!layout->header_size || header_length < layout->header_size) return 0;
  if (moddata_length < layout->header_size) return 0;
  if (!layout->channels || layout->channels > MOD_MAX_CHANNELS) return 0;
  if (!layout->song_length || layout->song_length > MOD_ORDER_COUNT) return 0;

  if (layout->restart_position >= layout->song_length) layout->restart_position = 0;

  order_offset = layout->sample_count == MOD_SAMPLE_COUNT ? MOD_ORDER_OFFSET : MOD_15_ORDER_OFFSET;
  for (uint16_t i = 0; i < layout->song_length; i++)
  {
    uint8_t pat = header[order_offset + i];
    if (pat >= MOD_ORDER_COUNT) return 0;
    if (pat > max_pattern) max_pattern = pat;
  }

  for (uint16_t i = 0; i < MOD_ORDER_COUNT; i++)
  {
    uint8_t pat = header[order_offset + i];
    if (pat < MOD_ORDER_COUNT && pat > max_table_pattern) max_table_pattern = pat;
  }

  min_patterns = (uint16_t)max_pattern + 1;
  table_patterns = (uint16_t)max_table_pattern + 1;
  if (table_patterns < min_patterns) table_patterns = min_patterns;
  if (!min_patterns) return 0;

  for (uint16_t i = 0; i < layout->sample_count; i++)
  {
    size_t off = MOD_TITLE_SIZE + (size_t)i * MOD_SAMPLE_HEADER_SIZE;
    uint32_t length = (uint32_t)tracker_rd_be16(header + off + 22) * 2u;
    uint32_t loop_start = (uint32_t)tracker_rd_be16(header + off + 26) * 2u;
    uint32_t loop_length = (uint32_t)tracker_rd_be16(header + off + 28) * 2u;

    layout->sample_length[i] = length;
    layout->sample_loop_start[i] = loop_start;
    layout->sample_loop_length[i] = loop_length;

    if (!tracker_size_add(&layout->sample_bytes, length)) return 0;
  }

  if (!tracker_size_mul(MOD_ROWS_PER_PATTERN, layout->channels, &pattern_block_bytes)) return 0;
  if (!tracker_size_mul(pattern_block_bytes, 4, &pattern_block_bytes)) return 0;

  layout->patterns = min_patterns;
  if (moddata_length >= layout->header_size && moddata_length - layout->header_size >= layout->sample_bytes)
  {
    size_t pattern_area = moddata_length - layout->header_size - layout->sample_bytes;
    size_t file_patterns = pattern_area / pattern_block_bytes;
    size_t pattern_tail = pattern_area % pattern_block_bytes;

    if (pattern_block_bytes && !pattern_tail && file_patterns >= min_patterns && file_patterns <= MOD_ORDER_COUNT)
      layout->patterns = (uint16_t)file_patterns;
    else if (pattern_block_bytes && table_patterns > min_patterns && file_patterns >= table_patterns && table_patterns <= MOD_ORDER_COUNT)
      layout->patterns = table_patterns;
  }

  if (!tracker_size_mul(layout->patterns, pattern_block_bytes, &layout->pattern_bytes)) return 0;

  layout->sample_data_offset = layout->pattern_data_offset;
  if (!tracker_size_add(&layout->sample_data_offset, layout->pattern_bytes)) return 0;
  pattern_end = layout->sample_data_offset;
  if (moddata_length < pattern_end) return 0;

  sample_avail = moddata_length - pattern_end;
  for (uint16_t i = 0; i < layout->sample_count; i++)
  {
    uint32_t length = layout->sample_length[i];
    uint32_t file_length = length;

    if ((size_t)file_length > sample_avail) file_length = (uint32_t)sample_avail;
    layout->sample_file_length[i] = file_length;
    sample_avail -= file_length;
  }

  layout->expected_size = layout->sample_data_offset;
  if (!tracker_size_add(&layout->expected_size, layout->sample_bytes)) return 0;

  return 1;
}

int mod_read_layout_from_header(const uint8_t *header, size_t header_length, size_t moddata_length, mod_layout_t *layout)
{
  uint16_t channels;

  if (!header || !layout) return 0;
  if (header_length < MOD_15_HEADER_SIZE) return 0;

  memset(layout, 0, sizeof(*layout));

  if (header_length >= MOD_HEADER_SIZE)
  {
    channels = mod_channels_from_magic(header + MOD_MAGIC_OFFSET);
    if (channels)
    {
      layout->channels = channels;
      layout->sample_count = MOD_SAMPLE_COUNT;
      layout->header_size = MOD_HEADER_SIZE;
      layout->pattern_data_offset = MOD_HEADER_SIZE;
      layout->song_length = header[MOD_SONG_LENGTH_OFFSET];
      layout->restart_position = header[MOD_RESTART_OFFSET];
      if (mod_finish_layout(header, header_length, moddata_length, layout)) return 1;
    }
  }

  memset(layout, 0, sizeof(*layout));
  layout->channels = 4;
  layout->sample_count = MOD_15_SAMPLE_COUNT;
  layout->header_size = MOD_15_HEADER_SIZE;
  layout->pattern_data_offset = MOD_15_HEADER_SIZE;
  layout->song_length = header[MOD_15_SONG_LENGTH_OFFSET];
  layout->restart_position = header[MOD_15_RESTART_OFFSET];
  if (mod_finish_layout(header, header_length, moddata_length, layout)) return 1;

  memset(layout, 0, sizeof(*layout));
  return 0;
}

int mod_read_layout(const void *moddata, size_t moddata_length, mod_layout_t *layout)
{
  const uint8_t *data = (const uint8_t*)moddata;

  if (!data || moddata_length < MOD_15_HEADER_SIZE) return 0;
  return mod_read_layout_from_header(data, moddata_length, moddata_length, layout);
}

int mod_check_sanity_preload(const void *moddata, size_t moddata_length)
{
  mod_layout_t layout;

  return mod_read_layout(moddata, moddata_length, &layout) ? 0 : 1;
}
uint32_t mod_effective_loop_length(const mod_layout_t *layout, uint16_t sample_index, uint32_t *out_loop_start)
{
  uint32_t length;
  uint32_t loop_start;
  uint32_t loop_length;

  if (!layout || sample_index >= layout->sample_count) return 0;

  length = layout->sample_length[sample_index];
  loop_start = layout->sample_loop_start[sample_index];
  loop_length = layout->sample_loop_length[sample_index];

  if (loop_start >= length || loop_length <= 2) return 0;
  if (loop_length > length - loop_start) loop_length = length - loop_start;
  if (loop_length <= 2) return 0;

  if (out_loop_start) *out_loop_start = loop_start;
  return loop_length;
}

int mod_layout_set_efx_backup_mask(mod_layout_t *layout, uint32_t sample_mask)
{
  size_t bytes = 0;
  uint16_t count = 0;
  uint32_t mask = 0;

  if (!layout) return 0;

  for (uint16_t i = 0; i < layout->sample_count; i++)
  {
    uint32_t loop_length;

    if (!(sample_mask & (1u << i))) continue;

    loop_length = mod_effective_loop_length(layout, i, NULL);
    if (!loop_length) continue;

    if (!tracker_size_add(&bytes, loop_length)) return 0;
    count++;
    mask |= 1u << i;
  }

  layout->efx_sample_mask = mask;
  layout->efx_backup_count = count;
  layout->efx_backup_bytes = bytes;
  return 1;
}

int mod_layout_set_efx_backup_all_looped(mod_layout_t *layout)
{
  uint32_t sample_mask = 0;

  if (!layout) return 0;

  for (uint16_t i = 0; i < layout->sample_count; i++)
  {
    if (mod_effective_loop_length(layout, i, NULL)) sample_mask |= 1u << i;
  }

  return mod_layout_set_efx_backup_mask(layout, sample_mask);
}

void mod_scan_efx_apply_slot(const mod_layout_t *layout, uint8_t *current_sample, uint8_t *efx_speed, uint16_t channel, const xm_pattern_slot_t *slot, uint32_t *sample_mask)
{
  uint8_t sample_index;

  if (!layout || !current_sample || !efx_speed || !slot || !sample_mask) return;
  if (channel >= layout->channels || channel >= MOD_MAX_CHANNELS) return;

  if (slot->instrument > 0 && slot->instrument <= layout->sample_count)
    current_sample[channel] = slot->instrument;

  if (slot->effect_type == 0xE && (slot->effect_param & 0xF0) == 0xF0)
    efx_speed[channel] = slot->effect_param & 0x0F;

  if (!efx_speed[channel] || !current_sample[channel]) return;

  sample_index = current_sample[channel] - 1;
  if (sample_index >= layout->sample_count) return;
  if (!mod_effective_loop_length(layout, sample_index, NULL)) return;

  *sample_mask |= 1u << sample_index;
}

uint32_t mod_scan_efx_sample_mask_from_context(const xm_context_t *ctx, const mod_layout_t *layout)
{
  uint8_t current_sample[MOD_MAX_CHANNELS] = {0};
  uint8_t efx_speed[MOD_MAX_CHANNELS] = {0};
  uint32_t sample_mask = 0;

  if (!ctx || !layout || !ctx->module.patterns) return 0;
  if (!layout->channels || layout->channels > MOD_MAX_CHANNELS) return 0;

  for (uint16_t order = 0; order < ctx->module.length; order++)
  {
    uint8_t pat_index = ctx->module.pattern_table[order];
    xm_pattern_t *pat;

    if (pat_index >= ctx->module.num_patterns) continue;
    pat = ctx->module.patterns + pat_index;
    if (!pat->slots) continue;

    for (uint16_t row = 0; row < pat->num_rows; row++)
    {
      for (uint16_t ch = 0; ch < ctx->module.num_channels; ch++)
      {
        xm_pattern_slot_t *slot = pat->slots + (size_t)row * ctx->module.num_channels + ch;
        mod_scan_efx_apply_slot(layout, current_sample, efx_speed, ch, slot, &sample_mask);
      }
    }
  }

  return sample_mask;
}

uint32_t mod_scan_efx_sample_mask_from_data(const void *moddata, const mod_layout_t *layout)
{
  const uint8_t *data = (const uint8_t*)moddata;
  uint8_t current_sample[MOD_MAX_CHANNELS] = {0};
  uint8_t efx_speed[MOD_MAX_CHANNELS] = {0};
  uint32_t sample_mask = 0;
  size_t order_offset;

  if (!data || !layout) return 0;
  if (!layout->channels || layout->channels > MOD_MAX_CHANNELS) return 0;

  order_offset = layout->sample_count == MOD_SAMPLE_COUNT ? MOD_ORDER_OFFSET : MOD_15_ORDER_OFFSET;
  for (uint16_t order = 0; order < layout->song_length; order++)
  {
    uint8_t pat_index = data[order_offset + order];

    if (pat_index >= layout->patterns) continue;

    for (uint16_t row = 0; row < MOD_ROWS_PER_PATTERN; row++)
    {
      for (uint16_t ch = 0; ch < layout->channels; ch++)
      {
        xm_pattern_slot_t slot;
        size_t off = layout->pattern_data_offset + (((size_t)pat_index * MOD_ROWS_PER_PATTERN + row) * layout->channels + ch) * 4;

        memset(&slot, 0, sizeof(slot));
        mod_decode_pattern_slot(&slot, data + off);
        mod_scan_efx_apply_slot(layout, current_sample, efx_speed, ch, &slot, &sample_mask);
      }
    }
  }

  return sample_mask;
}

int mod_get_group_sizes_for_layout(const mod_layout_t *layout, xm_context_group_sizes_t *sizes)
{
  size_t v;

  if (!layout || !sizes) return 0;
  memset(sizes, 0, sizeof(*sizes));

  if (!xm_group_sizes_add(sizes, &sizes->context_size, sizeof(xm_context_t))) return 0;

  if (!xm_group_sizes_add_mul(sizes, &sizes->patterns_size, layout->patterns, sizeof(xm_pattern_t))) return 0;

  if (!tracker_size_mul(layout->patterns, MOD_ROWS_PER_PATTERN, &v)) return 0;
  if (!tracker_size_mul(v, layout->channels, &v)) return 0;
  if (!xm_group_sizes_add_mul(sizes, &sizes->patterns_size, v, sizeof(xm_pattern_slot_t))) return 0;

  if (!xm_group_sizes_add_mul(sizes, &sizes->instruments_size, layout->sample_count, sizeof(xm_instrument_t))) return 0;
  if (!xm_group_sizes_add_mul(sizes, &sizes->instruments_size, layout->sample_count, sizeof(xm_sample_t))) return 0;

  if (!tracker_size_add(&sizes->total_size, layout->sample_bytes)) return 0;

  if (!xm_group_sizes_add_mul(sizes, &sizes->runtime_size, layout->channels, sizeof(xm_channel_context_t))) return 0;
  if (!tracker_size_mul(layout->song_length, MAX_NUM_ROWS, &v)) return 0;
  if (!xm_group_sizes_add_mul(sizes, &sizes->runtime_size, v, sizeof(uint8_t))) return 0;

  if (layout->efx_backup_count)
  {
    if (!xm_group_sizes_add_mul(sizes, &sizes->format_extra_size, layout->efx_backup_count, sizeof(xm_mod_efx_backup_t))) return 0;
    if (!xm_group_sizes_add(sizes, &sizes->format_extra_size, layout->efx_backup_bytes)) return 0;
  }

  return sizes->total_size <= INT_MAX;
}

size_t mod_get_memory_needed_for_layout(const mod_layout_t *layout)
{
  xm_context_group_sizes_t sizes;

  if (!mod_get_group_sizes_for_layout(layout, &sizes)) return 0;
  return sizes.total_size;
}

size_t mod_get_memory_needed_for_context(const void *moddata, size_t moddata_length)
{
  mod_layout_t layout;

  if (!mod_read_layout(moddata, moddata_length, &layout)) return 0;
  if (!mod_layout_set_efx_backup_mask(&layout, mod_scan_efx_sample_mask_from_data(moddata, &layout))) return 0;
  return mod_get_memory_needed_for_layout(&layout);
}

void mod_init_channel(xm_channel_context_t *ch, uint16_t channel)
{
  float panning;

  if (!ch) return;

  panning = ((channel & 3) == 1 || (channel & 3) == 2) ? 0.75f : 0.25f;

  ch->ping = true;
  ch->vibrato_waveform = XM_SINE_WAVEFORM;
  ch->vibrato_waveform_retrigger = true;
  ch->tremolo_waveform = XM_SINE_WAVEFORM;
  ch->tremolo_waveform_retrigger = true;
  ch->panbrello_waveform = XM_SINE_WAVEFORM;
  ch->panbrello_waveform_retrigger = true;
  ch->filter_cutoff = 127;   /* VDAC4: фильтра нет */

  ch->volume = ch->volume_envelope_volume = ch->fadeout_volume = 1.0f;
  ch->channel_volume = 1.0f;
  ch->panning = panning;
  ch->default_panning = panning;
  ch->tracker_format = TRACKER_FORMAT_MOD;
  ch->period_note_offset = .0f;
  ch->panning_envelope_panning = .5f;
  ch->actual_volume = .0f;
  ch->actual_panning = panning;
#if XM_RAMPING
  ch->target_volume = .0f;
  ch->target_panning = panning;
#endif
}

void mod_decode_pattern_slot(xm_pattern_slot_t *slot, const uint8_t *entry)
{
  uint8_t b0;
  uint8_t b1;
  uint8_t b2;
  uint8_t b3;

  if (!slot || !entry) return;

  b0 = entry[0];
  b1 = entry[1];
  b2 = entry[2];
  b3 = entry[3];

  slot->period = (uint16_t)(((uint16_t)(b0 & 0x0F) << 8) | b1);
  slot->note = 0;
  slot->instrument = (uint8_t)((b0 & 0xF0) | (b2 >> 4));
  slot->volume_column = 0;
  slot->effect_type = b2 & 0x0F;
  slot->effect_param = b3;
}

int mod_setup_module_header(xm_context_t *ctx, const uint8_t *header, const mod_layout_t *layout, xm_context_group_cursor_t *cursor)
{
  xm_module_t *mod;

  if (!ctx || !header || !layout || !cursor) return 0;

  mod = &ctx->module;
  ctx->tracker_format = TRACKER_FORMAT_MOD;

#if XM_STRINGS
  tracker_copy_trimmed(mod->name, sizeof(mod->name), header, MOD_TITLE_SIZE);
  strncpy(mod->trackername, "ProTracker MOD", sizeof(mod->trackername) - 1);
#endif

  mod->length = layout->song_length;
  mod->restart_position = layout->restart_position;
  mod->num_channels = layout->channels;
  mod->num_patterns = layout->patterns;
  mod->num_instruments = layout->sample_count;
  mod->frequency_type = XM_AMIGA_FREQUENCIES;

  memcpy(mod->pattern_table, header + (layout->sample_count == MOD_SAMPLE_COUNT ? MOD_ORDER_OFFSET : MOD_15_ORDER_OFFSET), layout->song_length);

  mod->patterns = (xm_pattern_t*)xm_group_cursor_alloc(&cursor->patterns, cursor->patterns_end, mod->num_patterns * sizeof(xm_pattern_t), true);
  if (!mod->patterns) return 0;

  mod->instruments = (xm_instrument_t*)xm_group_cursor_alloc(&cursor->instruments, cursor->instruments_end, mod->num_instruments * sizeof(xm_instrument_t), true);
  if (!mod->instruments) return 0;

  return 1;
}

int mod_setup_sample_metadata(xm_context_t *ctx, const uint8_t *header, const mod_layout_t *layout, uint16_t i, xm_context_group_cursor_t *cursor, MFUNC mfunc)
{
  xm_module_t *mod;
  size_t header_off;
  xm_instrument_t *instr;
  xm_sample_t *sample;
  uint8_t volume;
  uint8_t finetune;
  uint32_t loop_start;
  uint32_t loop_length;

  if (!ctx || !header || !layout || !cursor || !mfunc || i >= layout->sample_count) return 0;

  mod = &ctx->module;
  header_off = MOD_TITLE_SIZE + (size_t)i * MOD_SAMPLE_HEADER_SIZE;
  instr = mod->instruments + i;
  volume = header[header_off + 25];
  finetune = header[header_off + 24] & 0x0F;
  loop_start = layout->sample_loop_start[i];
  loop_length = layout->sample_loop_length[i];

#if XM_STRINGS
  tracker_copy_trimmed(instr->name, sizeof(instr->name), header + header_off, 22);
#endif
  instr->num_samples = 1;
  instr->samples = (xm_sample_t*)xm_group_cursor_alloc(&cursor->instruments, cursor->instruments_end, sizeof(xm_sample_t), true);
  if (!instr->samples) return 0;

  sample = instr->samples;
#if XM_STRINGS
  tracker_copy_trimmed(sample->name, sizeof(sample->name), header + header_off, 22);
#endif
  sample->bits = 8;
  sample->length = layout->sample_length[i];
  sample->volume = (float)(volume > 64 ? 64 : volume) / 64.f;
  sample->finetune = (int8_t)((finetune < 8 ? finetune : finetune - 16) << 4);
  sample->panning = .5f;
  sample->relative_note = 0;
  sample->data8 = sample->length ? (int8_t*)xm_alloc_and_register_group(ctx, sample->length, TRACKER_CONTEXT_SEG_SAMPLES, mfunc, false) : NULL;
  if (sample->length && !sample->data8) return 0;

  if (loop_start >= sample->length || loop_length <= 2)
  {
    sample->loop_start = 0;
    sample->loop_length = 0;
    sample->loop_end = 0;
    sample->loop_type = XM_NO_LOOP;
  }
  else
  {
    if (loop_length > sample->length - loop_start)
      loop_length = sample->length - loop_start;

    if (loop_length <= 2)
    {
      sample->loop_start = 0;
      sample->loop_length = 0;
      sample->loop_end = 0;
      sample->loop_type = XM_NO_LOOP;
    }
    else
    {
      sample->loop_start = loop_start;
      sample->loop_length = loop_length;
      sample->loop_end = loop_start + loop_length;
      sample->loop_type = XM_FORWARD_LOOP;
    }
  }

  return 1;
}

int mod_load_module(xm_context_t *ctx, const void *moddata, const mod_layout_t *layout, xm_context_group_cursor_t *cursor, MFUNC mfunc)
{
  const uint8_t *data = (const uint8_t*)moddata;
  xm_module_t *mod;
  size_t pattern_offset;
  size_t sample_offset;

  if (!ctx || !data || !layout || !cursor || !mfunc) return 0;

  mod = &ctx->module;
  if (!mod_setup_module_header(ctx, data, layout, cursor)) return 0;

  pattern_offset = layout->pattern_data_offset;
  for (uint16_t pat_i = 0; pat_i < mod->num_patterns; pat_i++)
  {
    xm_pattern_t *pat = mod->patterns + pat_i;

    pat->num_rows = MOD_ROWS_PER_PATTERN;
    pat->slots = (xm_pattern_slot_t*)xm_group_cursor_alloc(&cursor->patterns, cursor->patterns_end, MOD_ROWS_PER_PATTERN * mod->num_channels * sizeof(xm_pattern_slot_t), true);
    if (!pat->slots) return 0;

    for (uint16_t row = 0; row < MOD_ROWS_PER_PATTERN; row++)
    {
      for (uint16_t ch = 0; ch < mod->num_channels; ch++)
      {
        size_t off = pattern_offset + ((size_t)row * mod->num_channels + ch) * 4;
        xm_pattern_slot_t *slot = pat->slots + (size_t)row * mod->num_channels + ch;

        mod_decode_pattern_slot(slot, data + off);
      }
    }

    pattern_offset += (size_t)MOD_ROWS_PER_PATTERN * mod->num_channels * 4;
  }

  sample_offset = layout->sample_data_offset;
  for (uint16_t i = 0; i < layout->sample_count; i++)
  {
    xm_sample_t *sample;

    if (!mod_setup_sample_metadata(ctx, data, layout, i, cursor, mfunc)) return 0;

    sample = mod->instruments[i].samples;
    if (layout->sample_file_length[i])
      memcpy(sample->data8, data + sample_offset, layout->sample_file_length[i]);
    if (layout->sample_file_length[i] < sample->length)
      memset(sample->data8 + layout->sample_file_length[i], 0, sample->length - layout->sample_file_length[i]);
    sample_offset += layout->sample_file_length[i];
  }

  return 1;
}

int mod_setup_context_runtime(xm_context_t *ctx, xm_context_group_cursor_t *cursor, uint32_t rate)
{
  if (!ctx || !cursor) return 0;

  ctx->channels = (xm_channel_context_t*)xm_group_cursor_alloc(&cursor->runtime, cursor->runtime_end, ctx->module.num_channels * sizeof(xm_channel_context_t), true);
  if (!ctx->channels) return 0;

  ctx->row_loop_count = (uint8_t*)xm_group_cursor_alloc(&cursor->runtime, cursor->runtime_end, ctx->module.length * MAX_NUM_ROWS * sizeof(uint8_t), true);
  if (!ctx->row_loop_count) return 0;

  ctx->tempo = MOD_DEFAULT_TEMPO;
  ctx->bpm = MOD_DEFAULT_BPM;
  ctx->global_volume = 1.f;
  ctx->amplification = .25f;
  ctx->rate = rate;
#if XM_RAMPING
  ctx->volume_ramp = (1.f / 128.f);
  ctx->panning_ramp = (1.f / 128.f);
#endif

  for (uint16_t i = 0; i < ctx->module.num_channels; i++)
    mod_init_channel(ctx->channels + i, i);

  return 1;
}

int mod_setup_efx_backups(xm_context_t *ctx, const mod_layout_t *layout, xm_context_group_cursor_t *cursor)
{
  xm_mod_efx_backup_t *backup;
  int8_t *backup_data;

  if (!ctx || !layout || !cursor) return 0;

  ctx->mod_efx_backup_count = 0;
  ctx->mod_efx_backups = NULL;
  ctx->mod_efx_backup_data = NULL;

  if (!layout->efx_backup_count) return 1;

  backup = (xm_mod_efx_backup_t*)xm_group_cursor_alloc(&cursor->format_extra, cursor->format_extra_end, (size_t)layout->efx_backup_count * sizeof(xm_mod_efx_backup_t), true);
  if (!backup) return 0;

  backup_data = (int8_t*)xm_group_cursor_alloc(&cursor->format_extra, cursor->format_extra_end, layout->efx_backup_bytes, false);
  if (layout->efx_backup_bytes && !backup_data) return 0;

  ctx->mod_efx_backups = backup;
  ctx->mod_efx_backup_data = backup_data;

  for (uint16_t i = 0; i < layout->sample_count; i++)
  {
    xm_instrument_t *instr;
    xm_sample_t *sample;
    uint32_t loop_length;

    if (!(layout->efx_sample_mask & (1u << i))) continue;
    if (i >= ctx->module.num_instruments) continue;

    instr = ctx->module.instruments + i;
    if (!instr->num_samples || !instr->samples) continue;

    sample = instr->samples;
    if (sample->bits != 8 || sample->loop_type == XM_NO_LOOP || !sample->data8) continue;

    loop_length = sample->loop_length;
    if (sample->loop_start >= sample->length || loop_length <= 2) continue;
    if (loop_length > sample->length - sample->loop_start) loop_length = sample->length - sample->loop_start;
    if (loop_length <= 2) continue;

    backup->sample = sample;
    backup->backup8 = backup_data;
    backup->loop_start = sample->loop_start;
    backup->loop_length = loop_length;
    memcpy(backup_data, sample->data8 + sample->loop_start, loop_length);

    backup_data += loop_length;
    backup++;
    ctx->mod_efx_backup_count++;
  }

  return 1;
}

void mod_restore_efx_backups(xm_context_t *ctx)
{
  if (!ctx || !ctx->mod_efx_backup_count || !ctx->mod_efx_backups) return;

  for (uint16_t i = 0; i < ctx->mod_efx_backup_count; i++)
  {
    xm_mod_efx_backup_t *backup = ctx->mod_efx_backups + i;

    if (!backup->sample || !backup->sample->data8 || !backup->backup8 || !backup->loop_length) continue;
    if (backup->loop_start >= backup->sample->length) continue;
    if (backup->loop_length > backup->sample->length - backup->loop_start) continue;

    memcpy(backup->sample->data8 + backup->loop_start, backup->backup8, backup->loop_length);
  }
}

int mod_finish_context(xm_context_t **ctxp, xm_context_t *ctx)
{
  if (XM_DEFENSIVE && xm_check_sanity_postload(ctx))
  {
    xm_free_context(ctx);
    if (ctxp) *ctxp = NULL;
    return -1;
  }

  return (int)ctx->ctx_size;
}

int mod_allocate_format_extra_group(xm_context_t *ctx, xm_context_group_cursor_t *cursor, const xm_context_group_sizes_t *sizes, MFUNC mfunc)
{
  void *format_extra;

  if (!ctx || !cursor || !sizes || !mfunc) return 0;
  if (!sizes->format_extra_size) return 1;

  format_extra = xm_alloc_and_register_group(ctx, sizes->format_extra_size, TRACKER_CONTEXT_SEG_FORMAT_EXTRA, mfunc, true);
  if (!format_extra) return 0;

  cursor->format_extra = (char*)format_extra;
  cursor->format_extra_end = cursor->format_extra + sizes->format_extra_size;
  return 1;
}

int mod_create_context_safe(xm_context_t **ctxp, void *moddata, size_t moddata_length, uint32_t rate, MFUNC mfunc, size_t *out_bytes_needed)
{
  mod_layout_t layout;
  xm_context_group_sizes_t sizes;
  xm_context_group_cursor_t cursor;
  xm_context_t *ctx;

  if (out_bytes_needed) *out_bytes_needed = 0;
  if (ctxp) *ctxp = NULL;
  if (!ctxp || !moddata || !mfunc) return -1;

  if (!mod_read_layout(moddata, moddata_length, &layout)) return -1;
  if (!mod_layout_set_efx_backup_mask(&layout, mod_scan_efx_sample_mask_from_data(moddata, &layout))) return -1;
  if (!mod_get_group_sizes_for_layout(&layout, &sizes)) return -1;

  if (out_bytes_needed) *out_bytes_needed = sizes.total_size;
  if (!sizes.total_size) return -1;
  if (sizes.total_size > INT_MAX) return -2;

  if (!xm_allocate_context_groups(&ctx, &cursor, &sizes, mfunc)) return -2;
  *ctxp = ctx;

  if (!mod_load_module(ctx, moddata, &layout, &cursor, mfunc))
  {
    xm_free_context(ctx);
    *ctxp = NULL;
    return -1;
  }

  if (!mod_setup_context_runtime(ctx, &cursor, rate))
  {
    xm_free_context(ctx);
    *ctxp = NULL;
    return -1;
  }

  if (!mod_setup_efx_backups(ctx, &layout, &cursor))
  {
    xm_free_context(ctx);
    *ctxp = NULL;
    return -1;
  }

  return mod_finish_context(ctxp, ctx);
}

// ============================================================================
// S3M module loader
// ============================================================================

#define S3M_FILE_TYPE_OFFSET 29
#define S3M_ORDERS_OFFSET 32
#define S3M_SAMPLES_OFFSET 34
#define S3M_PATTERNS_OFFSET 36
#define S3M_FLAGS_OFFSET 38
#define S3M_CWTV_OFFSET 40
#define S3M_FORMAT_VERSION_OFFSET 42
#define S3M_GLOBAL_VOLUME_OFFSET 48
#define S3M_SPEED_OFFSET 49
#define S3M_TEMPO_OFFSET 50
#define S3M_MASTER_VOLUME_OFFSET 51
#define S3M_PANNING_TABLE_FLAG_OFFSET 53
#define S3M_CHANNEL_SETTINGS_OFFSET 64
#define S3M_FILE_TYPE_MODULE 0x10
#define S3M_PANNING_TABLE_PRESENT 0xFC
#define S3M_FLAG_FAST_VOLUME_SLIDES 0x40
#define S3M_CWTV_ST3_00 0x1300
#define S3M_CWTV_ST3_20 0x1320
#define S3M_FORMAT_SIGNED 0x01
#define S3M_FORMAT_UNSIGNED 0x02
#define S3M_SAMPLE_TYPE_NONE 0
#define S3M_SAMPLE_TYPE_PCM 1
#define S3M_SAMPLE_TYPE_ADLIB 2
#define S3M_SAMPLE_PACK_NONE 0
#define S3M_SAMPLE_PACK_ADPCM4 4   /* VDAC4: ModPlug ADPCM, 4 бита на отсчёт */
#define S3M_ADLIB_REG_COUNT 12
#define S3M_ADLIB_CHANNEL_NONE 0xFF
#define S3M_ADLIB_CHANNEL_FIRST 16
#define S3M_ADLIB_CHANNEL_COUNT 16
#define S3M_SAMPLE_FLAG_LOOP 0x01
#define S3M_SAMPLE_FLAG_STEREO 0x02
#define S3M_SAMPLE_FLAG_16BIT 0x04
#define S3M_PATTERN_NOTE_PRESENT 0x20
#define S3M_PATTERN_VOLUME_PRESENT 0x40
#define S3M_PATTERN_EFFECT_PRESENT 0x80
#define S3M_PATTERN_CHANNEL_MASK 0x1F
#define S3M_NOTE_NONE 0xFF
#define S3M_NOTE_KEY_OFF 0xFE

const char *g_s3m_last_error = "S3M loader was not called";
char g_s3m_last_error_buf[160];

struct s3m_sample_layout_t
{
  uint8_t name[28];
  uint32_t header_offset;
  uint32_t data_offset;
  uint32_t length;
  uint32_t file_length;
  uint32_t right_file_length;
  uint32_t loop_start;
  uint32_t loop_end;
  uint32_t c4speed;
  uint8_t type;
  uint8_t volume;
  uint8_t pack;
  uint8_t flags;
  uint8_t channels;
};

struct s3m_layout_t
{
  uint8_t name[28];
  uint16_t order_count;
  uint16_t sample_count;
  uint16_t pattern_count;
  uint16_t song_length;
  uint16_t channels;
  uint16_t flags;
  uint16_t cwtv;
  uint16_t format_version;
  uint8_t global_volume;
  uint8_t speed;
  uint8_t tempo;
  uint8_t master_volume;
  uint8_t has_panning_table;
  uint8_t signed_samples;
  uint8_t stereo;
  uint8_t channel_map[S3M_MAX_CHANNELS];
  uint8_t orders[S3M_MAX_ORDERS];
  uint8_t pattern_table[S3M_MAX_ORDERS];
  uint8_t channel_settings[S3M_MAX_CHANNELS];
  uint8_t channel_panning[S3M_MAX_CHANNELS];
  uint8_t channel_opl[S3M_MAX_CHANNELS];
  uint16_t sample_para[S3M_MAX_SAMPLES];
  uint16_t pattern_para[S3M_MAX_PATTERNS];
  s3m_sample_layout_t samples[S3M_MAX_SAMPLES];
  s3m_adlib_instrument_t adlib_instruments[S3M_MAX_SAMPLES];
  uint8_t has_adlib_instruments;
  uint8_t has_opl_channels;
  size_t pattern_slots_bytes;
  size_t sample_bytes;
};

typedef int (*s3m_read_at_fn)(void *user, size_t offset, void *dst, size_t size);

struct s3m_read_source_t
{
  void *user;
  size_t size;
  s3m_read_at_fn read_at;
};

const char *s3m_get_last_error()
{
  return g_s3m_last_error;
}

void s3m_set_error(const char *msg)
{
  g_s3m_last_error = msg ? msg : "S3M unknown error";
}

void s3m_set_error_fmt(const char *fmt, ...)
{
  va_list ap;

  if (!fmt)
  {
    s3m_set_error("S3M unknown error");
    return;
  }

  va_start(ap, fmt);
  vsnprintf(g_s3m_last_error_buf, sizeof(g_s3m_last_error_buf), fmt, ap);
  va_end(ap);
  g_s3m_last_error_buf[sizeof(g_s3m_last_error_buf) - 1] = 0;
  g_s3m_last_error = g_s3m_last_error_buf;
}

size_t s3m_para_offset(uint16_t para)
{
  return (size_t)para << 4;
}

float s3m_pan_from_nibble(uint8_t pan)
{
  if (pan > 15) pan = 15;
  return (float)pan / 15.f;
}

void s3m_set_default_panning(s3m_layout_t *layout)
{
  if (!layout) return;

  for (uint16_t i = 0; i < S3M_MAX_CHANNELS; i++)
  {
    uint8_t setting = layout->channel_settings[i];

    if (!layout->stereo || setting == 0xFF)
      layout->channel_panning[i] = 8;
    else
    {
      setting &= 0x7F;
      if (setting < 8)
        layout->channel_panning[i] = 3;
      else if (setting < 16)
        layout->channel_panning[i] = 12;
      else
        layout->channel_panning[i] = 8;
    }
  }
}

void s3m_compact_channel_panning(s3m_layout_t *layout)
{
  if (!layout) return;

  for (uint16_t i = 0; i < S3M_MAX_CHANNELS; i++)
  {
    uint8_t mapped = layout->channel_map[i];

    if (mapped != 0xFF)
      layout->channel_panning[mapped] = layout->channel_panning[i];
  }

  for (uint16_t i = layout->channels; i < S3M_MAX_CHANNELS; i++)
    layout->channel_panning[i] = 8;
}

int s3m_source_read_at(const s3m_read_source_t *source, size_t offset, void *dst, size_t size)
{
  if (!source || !source->read_at || (!dst && size)) return 0;
  if (!size) return 1;
  if (offset > source->size || source->size - offset < size) return 0;
  return source->read_at(source->user, offset, dst, size);
}

int s3m_memory_read_at(void *user, size_t offset, void *dst, size_t size)
{
  const uint8_t *data = (const uint8_t*)user;

  if ((!data && size) || (!dst && size)) return 0;
  if (size) memcpy(dst, data + offset, size);
  return 1;
}

int s3m_read_layout_from_source(const s3m_read_source_t *source, s3m_layout_t *layout)
{
  uint8_t header[S3M_HEADER_SIZE];
  uint8_t sample_header[S3M_SAMPLE_HEADER_SIZE];
  uint8_t table_entry[2];
  size_t tables_offset;
  size_t panning_offset;
  size_t tables_size = 0;
  size_t v;
  uint8_t max_pattern = 0;

  if (!source || !layout) return 0;
  memset(layout, 0, sizeof(*layout));

  if (source->size < S3M_HEADER_SIZE)
  {
    s3m_set_error("S3M header is shorter than 96 bytes");
    return 0;
  }

  if (!s3m_source_read_at(source, 0, header, sizeof(header)))
  {
    s3m_set_error("S3M header read failed");
    return 0;
  }

  if (memcmp(header + S3M_MAGIC_OFFSET, "SCRM", 4) != 0)
  {
    s3m_set_error("S3M missing SCRM signature");
    return 0;
  }

  if (header[S3M_FILE_TYPE_OFFSET] != S3M_FILE_TYPE_MODULE)
  {
    s3m_set_error_fmt("S3M invalid file type: 0x%02X", header[S3M_FILE_TYPE_OFFSET]);
    return 0;
  }

  memcpy(layout->name, header, sizeof(layout->name));
  layout->order_count = tracker_rd_le16(header + S3M_ORDERS_OFFSET);
  layout->sample_count = tracker_rd_le16(header + S3M_SAMPLES_OFFSET);
  layout->pattern_count = tracker_rd_le16(header + S3M_PATTERNS_OFFSET);
  layout->flags = tracker_rd_le16(header + S3M_FLAGS_OFFSET);
  layout->cwtv = tracker_rd_le16(header + S3M_CWTV_OFFSET);
  layout->format_version = tracker_rd_le16(header + S3M_FORMAT_VERSION_OFFSET);
  layout->global_volume = header[S3M_GLOBAL_VOLUME_OFFSET];
  layout->speed = header[S3M_SPEED_OFFSET];
  layout->tempo = header[S3M_TEMPO_OFFSET];
  layout->master_volume = header[S3M_MASTER_VOLUME_OFFSET];
  layout->stereo = (layout->master_volume & 0x80) ? 1 : 0;
  layout->has_panning_table = (header[S3M_PANNING_TABLE_FLAG_OFFSET] == S3M_PANNING_TABLE_PRESENT);
  memcpy(layout->channel_settings, header + S3M_CHANNEL_SETTINGS_OFFSET, S3M_MAX_CHANNELS);
  memset(layout->channel_map, 0xFF, sizeof(layout->channel_map));
  memset(layout->channel_opl, S3M_ADLIB_CHANNEL_NONE, sizeof(layout->channel_opl));

  if (layout->format_version != S3M_FORMAT_SIGNED && layout->format_version != S3M_FORMAT_UNSIGNED)
  {
    s3m_set_error_fmt("S3M unsupported sample format version: 0x%04X", layout->format_version);
    return 0;
  }
  layout->signed_samples = (layout->format_version == S3M_FORMAT_SIGNED);

  if (!layout->order_count || layout->order_count > S3M_MAX_ORDERS)
  {
    s3m_set_error_fmt("S3M unsupported order count: %u", layout->order_count);
    return 0;
  }

  if (layout->sample_count > S3M_MAX_SAMPLES)
  {
    s3m_set_error_fmt("S3M unsupported sample count: %u", layout->sample_count);
    return 0;
  }

  if (!layout->pattern_count || layout->pattern_count > S3M_MAX_PATTERNS)
  {
    s3m_set_error_fmt("S3M unsupported pattern count: %u", layout->pattern_count);
    return 0;
  }

  for (uint16_t i = 0; i < S3M_MAX_CHANNELS; i++)
  {
    uint8_t setting = layout->channel_settings[i];

    if (setting != 0xFF)
    {
      uint8_t mapped = (uint8_t)layout->channels++;

      layout->channel_map[i] = mapped;
      setting &= 0x7F;
      if (setting >= S3M_ADLIB_CHANNEL_FIRST && setting < S3M_ADLIB_CHANNEL_FIRST + S3M_ADLIB_CHANNEL_COUNT)
      {
        layout->channel_opl[mapped] = (uint8_t)(setting - S3M_ADLIB_CHANNEL_FIRST);
        layout->has_opl_channels = 1;
      }
    }
  }

  if (layout->channels < S3M_MIN_CHANNELS || layout->channels > S3M_MAX_CHANNELS)
  {
    s3m_set_error_fmt("S3M unsupported channel count: %u", layout->channels);
    return 0;
  }

  if (!tracker_size_add(&tables_size, layout->order_count)) return 0;
  if (!tracker_size_mul(layout->sample_count, 2, &v)) return 0;
  if (!tracker_size_add(&tables_size, v)) return 0;
  if (!tracker_size_mul(layout->pattern_count, 2, &v)) return 0;
  if (!tracker_size_add(&tables_size, v)) return 0;

  tables_offset = S3M_HEADER_SIZE;
  if (source->size < tables_offset || source->size - tables_offset < tables_size)
  {
    s3m_set_error("S3M truncated order/pointer tables");
    return 0;
  }

  if (!s3m_source_read_at(source, tables_offset, layout->orders, layout->order_count))
  {
    s3m_set_error("S3M order table read failed");
    return 0;
  }
  tables_offset += layout->order_count;

  for (uint16_t i = 0; i < layout->sample_count; i++)
  {
    if (!s3m_source_read_at(source, tables_offset + (size_t)i * 2, table_entry, sizeof(table_entry)))
    {
      s3m_set_error("S3M sample pointer table read failed");
      return 0;
    }
    layout->sample_para[i] = tracker_rd_le16(table_entry);
  }
  tables_offset += (size_t)layout->sample_count * 2;

  for (uint16_t i = 0; i < layout->pattern_count; i++)
  {
    if (!s3m_source_read_at(source, tables_offset + (size_t)i * 2, table_entry, sizeof(table_entry)))
    {
      s3m_set_error("S3M pattern pointer table read failed");
      return 0;
    }
    layout->pattern_para[i] = tracker_rd_le16(table_entry);
  }
  tables_offset += (size_t)layout->pattern_count * 2;

  s3m_set_default_panning(layout);
  panning_offset = tables_offset;
  if (layout->has_panning_table)
  {
    uint8_t panning[S3M_MAX_CHANNELS];

    if (source->size < panning_offset || source->size - panning_offset < S3M_MAX_CHANNELS)
    {
      s3m_set_error("S3M truncated panning table");
      return 0;
    }

    if (!s3m_source_read_at(source, panning_offset, panning, sizeof(panning)))
    {
      s3m_set_error("S3M panning table read failed");
      return 0;
    }

    for (uint16_t i = 0; i < S3M_MAX_CHANNELS; i++)
    {
      uint8_t pan = panning[i];
      if (pan & 0x20) layout->channel_panning[i] = pan & 0x0F;
    }
  }
  s3m_compact_channel_panning(layout);

  for (uint16_t i = 0; i < layout->order_count; i++)
  {
    uint8_t order = layout->orders[i];

    if (order == 0xFF) break;
    if (order == 0xFE) continue;
    if (order >= layout->pattern_count)
    {
      s3m_set_error_fmt("S3M order %u references invalid pattern %u", i, order);
      return 0;
    }

    layout->pattern_table[layout->song_length++] = order;
    if (order > max_pattern) max_pattern = order;
  }

  if (!layout->song_length)
  {
    s3m_set_error("S3M has no playable orders");
    return 0;
  }

  if ((uint16_t)max_pattern + 1 < layout->pattern_count)
    layout->pattern_count = (uint16_t)max_pattern + 1;

  if (!tracker_size_mul(layout->pattern_count, S3M_ROWS_PER_PATTERN, &layout->pattern_slots_bytes)) return 0;
  if (!tracker_size_mul(layout->pattern_slots_bytes, layout->channels, &layout->pattern_slots_bytes)) return 0;
  if (!tracker_size_mul(layout->pattern_slots_bytes, sizeof(xm_pattern_slot_t), &layout->pattern_slots_bytes)) return 0;

  for (uint16_t i = 0; i < layout->sample_count; i++)
  {
    s3m_sample_layout_t *sample = layout->samples + i;
    size_t header_offset = s3m_para_offset(layout->sample_para[i]);
    size_t data_offset;
    size_t sample_bytes;

    sample->header_offset = (uint32_t)header_offset;
    if (!layout->sample_para[i]) continue;

    if (header_offset > source->size || source->size - header_offset < S3M_SAMPLE_HEADER_SIZE)
    {
      s3m_set_error_fmt("S3M sample %u header outside file", i + 1);
      return 0;
    }

    if (!s3m_source_read_at(source, header_offset, sample_header, sizeof(sample_header)))
    {
      s3m_set_error_fmt("S3M sample %u header read failed", i + 1);
      return 0;
    }

    const uint8_t *h = sample_header;
    memcpy(sample->name, h + 48, sizeof(sample->name));
    sample->type = h[0];
    sample->data_offset = (((uint32_t)h[13] << 16) | tracker_rd_le16(h + 14)) << 4;
    sample->length = tracker_rd_le32(h + 16);
    sample->loop_start = tracker_rd_le32(h + 20);
    sample->loop_end = tracker_rd_le32(h + 24);
    sample->volume = h[28];
    sample->pack = h[30];
    sample->flags = h[31];
    sample->c4speed = tracker_rd_le32(h + 32);
    if (!sample->c4speed) sample->c4speed = S3M_DEFAULT_C4SPEED;

    sample->channels = (sample->flags & S3M_SAMPLE_FLAG_STEREO) ? 2 : 1;

    if (sample->type == S3M_SAMPLE_TYPE_NONE)
    {
      sample->length = 0;
      sample->file_length = 0;
      sample->right_file_length = 0;
      sample->channels = 1;
      continue;
    }

    if (sample->type == S3M_SAMPLE_TYPE_ADLIB)
    {
      s3m_adlib_instrument_t *adlib = layout->adlib_instruments + i;

      if (memcmp(h + 76, "SCRI", 4) != 0)
      {
        s3m_set_error_fmt("S3M AdLib instrument %u missing SCRI signature", i + 1);
        return 0;
      }

      adlib->type = sample->type;
      adlib->volume = sample->volume;
      adlib->c4speed = sample->c4speed;
      memcpy(adlib->regs, h + 16, S3M_ADLIB_REG_COUNT);
      layout->has_adlib_instruments = 1;
      sample->length = 0;
      sample->file_length = 0;
      sample->right_file_length = 0;
      sample->loop_start = 0;
      sample->loop_end = 0;
      sample->flags = 0;
      sample->channels = 1;
      continue;
    }

    if (sample->type != S3M_SAMPLE_TYPE_PCM)
    {
      s3m_set_error_fmt("S3M unsupported sample %u type: %u", i + 1, sample->type);
      return 0;
    }

    if (memcmp(h + 76, "SCRS", 4) != 0)
    {
      s3m_set_error_fmt("S3M sample %u missing SCRS signature", i + 1);
      return 0;
    }

    if (sample->pack != S3M_SAMPLE_PACK_NONE &&
        !(sample->pack == S3M_SAMPLE_PACK_ADPCM4 && !(sample->flags & S3M_SAMPLE_FLAG_16BIT) &&
          sample->channels == 1))
    {
      s3m_set_error_fmt("S3M unsupported packed sample %u: pack=%u", i + 1, sample->pack);
      return 0;
    }

    if (sample->length > INT_MAX / 2)
    {
      s3m_set_error_fmt("S3M sample %u is too large", i + 1);
      return 0;
    }

    sample_bytes = sample->length;
    if (sample->flags & S3M_SAMPLE_FLAG_16BIT)
    {
      if (!tracker_size_mul(sample_bytes, 2, &sample_bytes)) return 0;
    }
    if (sample->channels == 2 && !tracker_size_mul(sample_bytes, 2, &sample_bytes)) return 0;

    data_offset = sample->data_offset;
    sample->file_length = sample->length;
    sample->right_file_length = 0;
    if (sample_bytes)
    {
      size_t bytes_per_sample = (sample->flags & S3M_SAMPLE_FLAG_16BIT) ? 2 : 1;
      size_t left_bytes = (size_t)sample->length * bytes_per_sample;

      if (data_offset >= source->size)
        sample->file_length = 0;
      else if (sample->pack == S3M_SAMPLE_PACK_ADPCM4)
      {
        /* VDAC4: ADPCM — 16 байт таблицы, затем отсчёты по два в байте. */
        size_t available = source->size - data_offset;
        size_t packed_samples = available > 16 ? (available - 16) * 2 : 0;
        sample->file_length = (uint32_t)(packed_samples < sample->length ? packed_samples : sample->length);
      }
      else
      {
        size_t available = source->size - data_offset;
        size_t left_available = available < left_bytes ? available : left_bytes;
        sample->file_length = (uint32_t)(left_available / bytes_per_sample);

        if (sample->channels == 2 && available > left_bytes)
        {
          size_t right_available = available - left_bytes;
          if (right_available > left_bytes) right_available = left_bytes;
          sample->right_file_length = (uint32_t)(right_available / bytes_per_sample);
        }
      }
    }

    if (!tracker_size_add(&layout->sample_bytes, sample_bytes)) return 0;
  }

  return 1;
}

uint8_t s3m_note_to_xm(uint8_t note)
{
  uint8_t octave;
  uint8_t semi;
  uint16_t xm_note;

  if (note == S3M_NOTE_NONE) return 0;
  if (note == S3M_NOTE_KEY_OFF) return 97;

  octave = note >> 4;
  semi = note & 0x0F;
  if (semi > 11) return 0;

  xm_note = (uint16_t)octave * 12 + semi + 1;
  if (xm_note > 96) return 0;
  return (uint8_t)xm_note;
}

int s3m_convert_effect(xm_pattern_slot_t *slot, uint8_t command, uint8_t param, uint16_t pattern, uint16_t row, uint16_t channel)
{
  if (!slot) return 0;
  if (!command)
  {
    if (param)
    {
      slot->effect_type = XM_EFFECT_S3M_NONE;
      slot->effect_param = param;
      return 1;
    }

    slot->effect_type = 0;
    slot->effect_param = 0;
    return 1;
  }

  switch (command | 0x40)
  {
    case 'A':
      slot->effect_type = XM_EFFECT_S3M_SPEED;
      slot->effect_param = param;
      return 1;

    case 'B':
      slot->effect_type = XM_EFFECT_S3M_POSITION_JUMP;
      slot->effect_param = param;
      return 1;

    case 'C':
      slot->effect_type = XM_EFFECT_S3M_PATTERN_BREAK;
      slot->effect_param = param;
      return 1;

    case 'D':
      slot->effect_type = XM_EFFECT_S3M_VOLUME_SLIDE;
      slot->effect_param = param;
      return 1;

    case 'E':
      slot->effect_type = XM_EFFECT_S3M_PORTAMENTO_DOWN;
      slot->effect_param = param;
      return 1;

    case 'F':
      slot->effect_type = XM_EFFECT_S3M_PORTAMENTO_UP;
      slot->effect_param = param;
      return 1;

    case 'G':
      slot->effect_type = XM_EFFECT_S3M_TONE_PORTAMENTO;
      slot->effect_param = param;
      return 1;

    case 'H':
      slot->effect_type = XM_EFFECT_S3M_VIBRATO;
      slot->effect_param = param;
      return 1;

    case 'I':
      slot->effect_type = XM_EFFECT_S3M_TREMOR;
      slot->effect_param = param;
      return 1;

    case 'J':
      slot->effect_type = XM_EFFECT_S3M_ARPEGGIO;
      slot->effect_param = param;
      return 1;

    case 'K':
      slot->effect_type = XM_EFFECT_S3M_VIBRATO_VOLUME_SLIDE;
      slot->effect_param = param;
      return 1;

    case 'L':
      slot->effect_type = XM_EFFECT_S3M_TONE_PORTAMENTO_VOLUME_SLIDE;
      slot->effect_param = param;
      return 1;

    case 'M':
      slot->effect_type = XM_EFFECT_S3M_CHANNEL_VOLUME;
      slot->effect_param = param;
      return 1;

    case 'N':
      slot->effect_type = XM_EFFECT_S3M_CHANNEL_VOLUME_SLIDE;
      slot->effect_param = param;
      return 1;

    case 'O':
      slot->effect_type = XM_EFFECT_S3M_SAMPLE_OFFSET;
      slot->effect_param = param;
      return 1;

    case 'P':
      slot->effect_type = XM_EFFECT_S3M_PANNING_SLIDE;
      slot->effect_param = param;
      return 1;

    case 'Q':
      slot->effect_type = XM_EFFECT_S3M_RETRIG;
      slot->effect_param = param;
      return 1;

    case 'R':
      slot->effect_type = XM_EFFECT_S3M_TREMOLO;
      slot->effect_param = param;
      return 1;

    case 'S':
      slot->effect_type = XM_EFFECT_S3M_EXTENDED;
      slot->effect_param = param;
      return 1;

    case 'T':
      slot->effect_type = XM_EFFECT_S3M_TEMPO;
      slot->effect_param = param;
      return 1;

    case 'U':
      slot->effect_type = XM_EFFECT_S3M_FINE_VIBRATO;
      slot->effect_param = param;
      return 1;

    case 'V':
      slot->effect_type = XM_EFFECT_S3M_GLOBAL_VOLUME;
      slot->effect_param = param;
      return 1;

    case 'W':
      slot->effect_type = XM_EFFECT_S3M_GLOBAL_VOLUME_SLIDE;
      slot->effect_param = param;
      return 1;

    case 'X':
      slot->effect_type = XM_EFFECT_S3M_PANNING;
      slot->effect_param = param;
      return 1;

    case 'Y':
      slot->effect_type = XM_EFFECT_S3M_PANBRELLO;
      slot->effect_param = param;
      return 1;

    case 'Z':
      slot->effect_type = XM_EFFECT_S3M_MIDI_MACRO;
      slot->effect_param = param;
      return 1;

    default:
      break;
  }

  /* VDAC4: команда за Z (их пишут OpenMPT и другие трекеры) — без эффекта, как
   * у ST3; раньше из-за неё отвергался весь модуль. */
  (void)pattern;
  (void)row;
  (void)channel;
  slot->effect_type = 0;
  slot->effect_param = 0;
  return 1;
}

int s3m_source_read_u8(const s3m_read_source_t *source, size_t *offset, uint8_t *out)
{
  if (!source || !offset || !out) return 0;
  if (!s3m_source_read_at(source, *offset, out, 1)) return 0;
  (*offset)++;
  return 1;
}

int s3m_decode_pattern_from_source(xm_pattern_t *pat, const s3m_read_source_t *source, const s3m_layout_t *layout, uint16_t pattern_index)
{
  size_t base;
  size_t offset;
  size_t soft_end_offset;
  size_t hard_end_offset;
  uint16_t packed_len;
  uint8_t len_buf[2];
  uint16_t row = 0;

  if (!pat || !source || !layout) return 0;
  if (!layout->pattern_para[pattern_index]) return 1;

  base = s3m_para_offset(layout->pattern_para[pattern_index]);
  if (base > source->size || source->size - base < 2)
  {
    s3m_set_error_fmt("S3M pattern %u offset outside file", pattern_index);
    return 0;
  }

  if (!s3m_source_read_at(source, base, len_buf, sizeof(len_buf)))
  {
    s3m_set_error_fmt("S3M pattern %u length read failed", pattern_index);
    return 0;
  }

  packed_len = tracker_rd_le16(len_buf);
  if (packed_len < 2) return 1;

  soft_end_offset = base + packed_len;
  hard_end_offset = base + 2 + packed_len;
  if (soft_end_offset > source->size) soft_end_offset = source->size;
  if (hard_end_offset > source->size) hard_end_offset = source->size;
  for (uint16_t i = 0; i < layout->pattern_count; i++)
  {
    size_t next_offset;

    if (i == pattern_index || !layout->pattern_para[i]) continue;
    next_offset = s3m_para_offset(layout->pattern_para[i]);
    if (next_offset > base && next_offset < hard_end_offset) hard_end_offset = next_offset;
  }
  if (soft_end_offset > hard_end_offset) soft_end_offset = hard_end_offset;
  offset = base + 2;

  while (row < S3M_ROWS_PER_PATTERN)
  {
    uint8_t info;
    uint8_t channel;
    uint8_t note = S3M_NOTE_NONE;
    uint8_t instr = 0;
    uint8_t volume = 0xFF;
    uint8_t command = 0;
    uint8_t param = 0;
    xm_pattern_slot_t *slot = NULL;

    size_t event_offset = offset;

    if (offset >= hard_end_offset) return 1;

    if (!s3m_source_read_u8(source, &offset, &info))
    {
      s3m_set_error_fmt("S3M pattern %u read failed at row %u", pattern_index, row);
      return 0;
    }

    if (info == 0)
    {
      row++;
      continue;
    }

    channel = info & S3M_PATTERN_CHANNEL_MASK;
    if (info & S3M_PATTERN_NOTE_PRESENT)
    {
      if (hard_end_offset - offset < 2)
      {
        if (event_offset >= soft_end_offset) return 1;
        s3m_set_error_fmt("S3M pattern %u truncated note at row %u", pattern_index, row);
        return 0;
      }
      if (!s3m_source_read_u8(source, &offset, &note)) return 0;
      if (!s3m_source_read_u8(source, &offset, &instr)) return 0;
    }

    if (info & S3M_PATTERN_VOLUME_PRESENT)
    {
      if (offset >= hard_end_offset)
      {
        if (event_offset >= soft_end_offset) return 1;
        s3m_set_error_fmt("S3M pattern %u truncated volume at row %u", pattern_index, row);
        return 0;
      }
      if (!s3m_source_read_u8(source, &offset, &volume)) return 0;
    }

    if (info & S3M_PATTERN_EFFECT_PRESENT)
    {
      if (hard_end_offset - offset < 2)
      {
        if (event_offset >= soft_end_offset) return 1;
        s3m_set_error_fmt("S3M pattern %u truncated effect at row %u", pattern_index, row);
        return 0;
      }
      if (!s3m_source_read_u8(source, &offset, &command)) return 0;
      if (!s3m_source_read_u8(source, &offset, &param)) return 0;
    }

    channel = layout->channel_map[channel];
    if (channel == 0xFF) continue;

    slot = pat->slots + (size_t)row * layout->channels + channel;
    slot->note = s3m_note_to_xm(note);
    slot->instrument = instr;
    if (volume != 0xFF)
    {
      if (volume >= 128 && volume <= 192)
        slot->volume_column = volume;
      else
      {
        if (volume > 64) volume = 64;
        slot->volume_column = (uint8_t)(0x10 + volume);
      }
    }

    if (!s3m_convert_effect(slot, command, param, pattern_index, row, channel))
    {
      if (event_offset >= soft_end_offset) return 1;
      return 0;
    }
  }

  return 1;
}

void s3m_apply_post_pattern_compat(xm_context_t *ctx, const s3m_layout_t *layout)
{
  int zxx_count_left = 0;
  int zxx_count_right = 0;
  bool pixplay_panning;

  if (!ctx || !layout) return;

  pixplay_panning = layout->cwtv < S3M_CWTV_ST3_20;
  if (!pixplay_panning) return;

  for (uint16_t pat_i = 0; pat_i < ctx->module.num_patterns; pat_i++)
  {
    xm_pattern_t *pat = ctx->module.patterns + pat_i;
    if (!pat->slots) continue;

    for (uint16_t row = 0; row < pat->num_rows; row++)
    {
      for (uint16_t chn = 0; chn < ctx->module.num_channels; chn++)
      {
        xm_pattern_slot_t *slot = pat->slots + (size_t)row * ctx->module.num_channels + chn;
        if (slot->effect_type != XM_EFFECT_S3M_MIDI_MACRO) continue;

        if (slot->effect_param > 0x0F)
        {
          pixplay_panning = false;
          break;
        }

        if (slot->effect_param < 0x08)
          zxx_count_left++;
        else if (slot->effect_param > 0x08)
          zxx_count_right++;
      }

      if (!pixplay_panning) break;
    }

    if (!pixplay_panning) break;
  }

  if (!pixplay_panning) return;
  if (zxx_count_left + zxx_count_right < ctx->module.num_channels) return;
  if (-zxx_count_left + zxx_count_right >= ctx->module.num_channels) return;

  for (uint16_t pat_i = 0; pat_i < ctx->module.num_patterns; pat_i++)
  {
    xm_pattern_t *pat = ctx->module.patterns + pat_i;
    if (!pat->slots) continue;

    for (uint16_t row = 0; row < pat->num_rows; row++)
    {
      for (uint16_t chn = 0; chn < ctx->module.num_channels; chn++)
      {
        xm_pattern_slot_t *slot = pat->slots + (size_t)row * ctx->module.num_channels + chn;
        if (slot->effect_type != XM_EFFECT_S3M_MIDI_MACRO) continue;

        slot->effect_type = XM_EFFECT_S3M_EXTENDED;
        slot->effect_param |= 0x80;
      }
    }
  }
}

int s3m_load_sample_data_from_source(xm_sample_t *sample, const s3m_read_source_t *source, const s3m_sample_layout_t *sl, bool signed_samples)
{
  uint32_t left_file_length;
  uint32_t right_file_length;
  size_t bytes_per_sample;
  size_t left_file_bytes;
  size_t right_file_bytes;
  size_t right_offset;

  if (!sample || !source || !sl || !sample->data8 || !sample->length) return 1;

  if (sl->pack == S3M_SAMPLE_PACK_ADPCM4)
  {
    /* VDAC4: ADPCM ModPlug — 16 байт таблицы разностей, затем по два отсчёта в
     * байте (младшая тетрада — первым), отсчёт — сумма разностей. Распаковка на
     * месте: сжатое ложится в конец буфера, вывод его не догоняет. */
    const uint32_t count = sl->file_length;
    const size_t packed = 16 + ((size_t)count + 1) / 2;
    uint8_t *buffer = (uint8_t*)sample->data8;
    uint8_t *temp = NULL;
    uint8_t *in = buffer + sample->length - packed;
    int8_t table[16];
    int8_t delta = 0;

    if (packed > sample->length)
    {
      temp = (uint8_t*)malloc(packed);
      if (!temp)
      {
        s3m_set_error("S3M ADPCM sample: no memory");
        return 0;
      }
      in = temp;
    }
    if (!s3m_source_read_at(source, sl->data_offset, in, packed))
    {
      free(temp);
      s3m_set_error("S3M sample data read failed");
      return 0;
    }
    memcpy(table, in, sizeof(table));
    for (uint32_t i = 0; i < count; i += 2)
    {
      const uint8_t pair = in[16 + i / 2];
      delta = (int8_t)(delta + table[pair & 0x0F]);
      sample->data8[i] = delta;
      if (i + 1 < count)
      {
        delta = (int8_t)(delta + table[pair >> 4]);
        sample->data8[i + 1] = delta;
      }
    }
    for (uint32_t i = count; i < sample->length; i++)
      sample->data8[i] = 0;
    free(temp);
    return 1;
  }

  bytes_per_sample = (sample->bits == 16) ? 2 : 1;
  left_file_length = sl->file_length;
  if (left_file_length > sample->length) left_file_length = sample->length;
  right_file_length = sl->right_file_length;
  if (right_file_length > sample->length) right_file_length = sample->length;

  left_file_bytes = (size_t)left_file_length * bytes_per_sample;
  right_file_bytes = (size_t)right_file_length * bytes_per_sample;
  right_offset = (size_t)sample->length * bytes_per_sample;

  if (left_file_bytes && !s3m_source_read_at(source, sl->data_offset, sample->data8, left_file_bytes))
  {
    s3m_set_error("S3M sample data read failed");
    return 0;
  }

  if (sl->channels == 2 && right_file_bytes)
  {
    if (!s3m_source_read_at(source, sl->data_offset + right_offset, sample->data8 + right_offset, right_file_bytes))
    {
      s3m_set_error("S3M stereo sample data read failed");
      return 0;
    }
  }

  if (sample->bits == 16)
  {
    int16_t *left = sample->data16;
    int16_t *right = (int16_t*)(sample->data8 + right_offset);

    for (uint32_t i = 0; i < sample->length; i++)
    {
      int32_t left_val = 0;
      int32_t right_val = 0;

      if (i < left_file_length)
      {
        uint16_t raw = tracker_rd_le16((const uint8_t*)left + (size_t)i * 2);
        if (!signed_samples) raw ^= 0x8000;
        left_val = (int16_t)raw;
      }

      if (sl->channels == 2)
      {
        if (i < right_file_length)
        {
          uint16_t raw = tracker_rd_le16((const uint8_t*)right + (size_t)i * 2);
          if (!signed_samples) raw ^= 0x8000;
          right_val = (int16_t)raw;
        }
        left[i] = (int16_t)((left_val + right_val) / 2);
      }
      else
        left[i] = (int16_t)left_val;
    }
  }
  else
  {
    int8_t *left = sample->data8;
    int8_t *right = sample->data8 + right_offset;

    for (uint32_t i = 0; i < sample->length; i++)
    {
      int16_t left_val = 0;
      int16_t right_val = 0;

      if (i < left_file_length)
        left_val = signed_samples ? left[i] : (int8_t)((uint8_t)left[i] ^ 0x80);

      if (sl->channels == 2)
      {
        if (i < right_file_length)
          right_val = signed_samples ? right[i] : (int8_t)((uint8_t)right[i] ^ 0x80);
        left[i] = (int8_t)((left_val + right_val) / 2);
      }
      else
        left[i] = (int8_t)left_val;
    }
  }

  return 1;
}

int s3m_get_group_sizes_for_layout(const s3m_layout_t *layout, xm_context_group_sizes_t *sizes)
{
  size_t v;

  if (!layout || !sizes) return 0;
  memset(sizes, 0, sizeof(*sizes));

  if (!xm_group_sizes_add(sizes, &sizes->context_size, sizeof(xm_context_t))) return 0;

  if (!tracker_size_mul(layout->pattern_count, sizeof(xm_pattern_t), &v)) return 0;
  if (!xm_group_sizes_add(sizes, &sizes->patterns_size, v)) return 0;
  if (!xm_group_sizes_add(sizes, &sizes->patterns_size, layout->pattern_slots_bytes)) return 0;

  if (!tracker_size_mul(layout->sample_count, sizeof(xm_instrument_t), &v)) return 0;
  if (!xm_group_sizes_add(sizes, &sizes->instruments_size, v)) return 0;
  if (!tracker_size_mul(layout->sample_count, sizeof(xm_sample_t), &v)) return 0;
  if (!xm_group_sizes_add(sizes, &sizes->instruments_size, v)) return 0;

  if (!tracker_size_add(&sizes->total_size, layout->sample_bytes)) return 0;

  if (layout->has_adlib_instruments)
  {
    if (!tracker_size_mul(layout->sample_count, sizeof(s3m_adlib_instrument_t), &v)) return 0;
    if (!xm_group_sizes_add(sizes, &sizes->format_extra_size, v)) return 0;
  }

  if (layout->has_opl_channels)
  {
    if (!tracker_size_mul(layout->channels, sizeof(uint8_t), &v)) return 0;
    if (!xm_group_sizes_add(sizes, &sizes->format_extra_size, v)) return 0;
  }

  if (!tracker_size_mul(layout->channels, sizeof(xm_channel_context_t), &v)) return 0;
  if (!xm_group_sizes_add(sizes, &sizes->runtime_size, v)) return 0;
  if (!tracker_size_mul(layout->song_length, MAX_NUM_ROWS, &v)) return 0;
  if (!tracker_size_mul(v, sizeof(uint8_t), &v)) return 0;
  if (!xm_group_sizes_add(sizes, &sizes->runtime_size, v)) return 0;

  return sizes->total_size <= INT_MAX;
}

int s3m_setup_module_header_grouped(xm_context_t *ctx, const s3m_layout_t *layout, xm_context_group_cursor_t *cursor)
{
  xm_module_t *mod;
  size_t patterns_size;
  size_t instruments_size;

  if (!ctx || !layout || !cursor) return 0;

  mod = &ctx->module;
  ctx->tracker_format = TRACKER_FORMAT_S3M;

#if XM_STRINGS
  tracker_copy_trimmed(mod->name, sizeof(mod->name), layout->name, sizeof(layout->name));
  tracker_copy_trimmed(mod->trackername, sizeof(mod->trackername), (const uint8_t*)"Scream Tracker 3", 16);
#endif

  mod->length = layout->song_length;
  mod->restart_position = 0;
  mod->num_channels = layout->channels;
  mod->num_patterns = layout->pattern_count;
  mod->num_instruments = layout->sample_count;
  mod->frequency_type = XM_S3M_FREQUENCIES;
  memcpy(mod->pattern_table, layout->pattern_table, layout->song_length);

  patterns_size = (size_t)mod->num_patterns * sizeof(xm_pattern_t);
  mod->patterns = patterns_size ? (xm_pattern_t*)xm_group_cursor_alloc(&cursor->patterns, cursor->patterns_end, patterns_size, true) : NULL;
  if (patterns_size && !mod->patterns) return 0;

  instruments_size = (size_t)mod->num_instruments * sizeof(xm_instrument_t);
  mod->instruments = instruments_size ? (xm_instrument_t*)xm_group_cursor_alloc(&cursor->instruments, cursor->instruments_end, instruments_size, true) : NULL;
  if (instruments_size && !mod->instruments) return 0;

  if (layout->has_adlib_instruments)
  {
    size_t adlib_size = (size_t)layout->sample_count * sizeof(s3m_adlib_instrument_t);

    ctx->s3m_adlib_instrument_count = layout->sample_count;
    ctx->s3m_adlib_instruments = (s3m_adlib_instrument_t*)xm_group_cursor_alloc(&cursor->format_extra, cursor->format_extra_end, adlib_size, false);
    if (!ctx->s3m_adlib_instruments) return 0;
    memcpy(ctx->s3m_adlib_instruments, layout->adlib_instruments, adlib_size);
  }

  if (layout->has_opl_channels)
  {
    ctx->s3m_channel_opl = (uint8_t*)xm_group_cursor_alloc(&cursor->format_extra, cursor->format_extra_end, layout->channels, false);
    if (!ctx->s3m_channel_opl) return 0;
    memcpy(ctx->s3m_channel_opl, layout->channel_opl, layout->channels);
  }

  return 1;
}

int s3m_setup_sample_metadata_grouped(xm_context_t *ctx, const s3m_layout_t *layout, uint16_t i, xm_context_group_cursor_t *cursor, MFUNC mfunc)
{
  xm_module_t *mod;
  const s3m_sample_layout_t *sl;
  xm_instrument_t *instr;
  xm_sample_t *sample;
  uint32_t loop_start;
  uint32_t loop_end;
  size_t sample_bytes;

  if (!ctx || !layout || !cursor || !mfunc || i >= layout->sample_count) return 0;

  mod = &ctx->module;
  sl = layout->samples + i;
  instr = mod->instruments + i;

#if XM_STRINGS
  if (layout->sample_para[i])
    tracker_copy_trimmed(instr->name, sizeof(instr->name), sl->name, sizeof(sl->name));
#endif

  instr->num_samples = 1;
  instr->samples = (xm_sample_t*)xm_group_cursor_alloc(&cursor->instruments, cursor->instruments_end, sizeof(xm_sample_t), true);
  if (!instr->samples) return 0;

  sample = instr->samples;
#if XM_STRINGS
  if (layout->sample_para[i])
    tracker_copy_trimmed(sample->name, sizeof(sample->name), sl->name, sizeof(sl->name));
#endif

  sample->bits = (sl->flags & S3M_SAMPLE_FLAG_16BIT) ? 16 : 8;
  sample->length = sl->length;
  sample->volume = (float)(sl->volume > 64 ? 64 : sl->volume) / 64.f;
  sample->finetune = 0;
  sample->panning = .5f;
  sample->relative_note = 0;
  sample->c4speed = sl->c4speed;

  sample_bytes = sample->length;
  if (sample->bits == 16 && !tracker_size_mul(sample_bytes, 2, &sample_bytes)) return 0;
  if (sl->channels == 2 && !tracker_size_mul(sample_bytes, 2, &sample_bytes)) return 0;
  sample->data8 = sample_bytes ? (int8_t*)xm_alloc_and_register_group(ctx, sample_bytes, TRACKER_CONTEXT_SEG_SAMPLES, mfunc, false) : NULL;
  if (sample_bytes && !sample->data8) return 0;

  loop_start = sl->loop_start;
  loop_end = sl->loop_end;
  if (!(sl->flags & S3M_SAMPLE_FLAG_LOOP) || loop_start >= sample->length || loop_end <= loop_start + 2)
  {
    sample->loop_start = 0;
    sample->loop_length = 0;
    sample->loop_end = 0;
    sample->loop_type = XM_NO_LOOP;
  }
  else
  {
    if (loop_end > sample->length) loop_end = sample->length;
    if (loop_end <= loop_start + 2)
    {
      sample->loop_start = 0;
      sample->loop_length = 0;
      sample->loop_end = 0;
      sample->loop_type = XM_NO_LOOP;
    }
    else
    {
      sample->loop_start = loop_start;
      sample->loop_end = loop_end;
      sample->loop_length = loop_end - loop_start;
      sample->loop_type = XM_FORWARD_LOOP;
    }
  }

  sample->latest_trigger = 0;
  return 1;
}

int s3m_load_module_from_source_grouped(xm_context_t *ctx, const s3m_read_source_t *source, const s3m_layout_t *layout, xm_context_group_cursor_t *cursor, MFUNC mfunc)
{
  xm_module_t *mod;

  if (!ctx || !source || !layout || !cursor || !mfunc) return 0;
  if (!s3m_setup_module_header_grouped(ctx, layout, cursor)) return 0;

  mod = &ctx->module;
  for (uint16_t pat_i = 0; pat_i < mod->num_patterns; pat_i++)
  {
    xm_pattern_t *pat = mod->patterns + pat_i;
    size_t slots_size = (size_t)S3M_ROWS_PER_PATTERN * mod->num_channels * sizeof(xm_pattern_slot_t);

    pat->num_rows = S3M_ROWS_PER_PATTERN;
    pat->slots = slots_size ? (xm_pattern_slot_t*)xm_group_cursor_alloc(&cursor->patterns, cursor->patterns_end, slots_size, true) : NULL;
    if (slots_size && !pat->slots) return 0;
    if (!s3m_decode_pattern_from_source(pat, source, layout, pat_i)) return 0;
  }

  s3m_apply_post_pattern_compat(ctx, layout);

  for (uint16_t i = 0; i < layout->sample_count; i++)
  {
    const s3m_sample_layout_t *sl = layout->samples + i;
    xm_sample_t *sample;

    if (!s3m_setup_sample_metadata_grouped(ctx, layout, i, cursor, mfunc)) return 0;

    sample = mod->instruments[i].samples;
    if (sl->file_length && !s3m_load_sample_data_from_source(sample, source, sl, layout->signed_samples != 0)) return 0;
  }

  return 1;
}

int s3m_setup_context_runtime_grouped(xm_context_t *ctx, const s3m_layout_t *layout, xm_context_group_cursor_t *cursor, uint32_t rate)
{
  if (!ctx || !layout || !cursor) return 0;

  ctx->channels = (xm_channel_context_t*)xm_group_cursor_alloc(&cursor->runtime, cursor->runtime_end, (size_t)ctx->module.num_channels * sizeof(xm_channel_context_t), true);
  if (!ctx->channels) return 0;

  ctx->row_loop_count = (uint8_t*)xm_group_cursor_alloc(&cursor->runtime, cursor->runtime_end, (size_t)ctx->module.length * MAX_NUM_ROWS * sizeof(uint8_t), true);
  if (!ctx->row_loop_count) return 0;

  ctx->tempo = layout->speed ? layout->speed : 6;
  ctx->bpm = layout->tempo ? layout->tempo : 125;
  ctx->global_volume = (float)(layout->global_volume > 64 ? 64 : layout->global_volume) / 64.f;
  ctx->s3m_fast_volume_slides =
    (layout->cwtv == S3M_CWTV_ST3_00 ||
     (layout->flags & S3M_FLAG_FAST_VOLUME_SLIDES) != 0);
  ctx->amplification = .25f;
  ctx->rate = rate;
#if XM_RAMPING
  ctx->volume_ramp = (1.f / 128.f);
  ctx->panning_ramp = (1.f / 128.f);
#endif

  for (uint16_t i = 0; i < ctx->module.num_channels; i++)
  {
    xm_channel_context_t *ch = ctx->channels + i;
    float panning = s3m_pan_from_nibble(layout->channel_panning[i]);

    ch->ping = true;
    ch->vibrato_waveform = XM_SINE_WAVEFORM;
    ch->vibrato_waveform_retrigger = true;
    ch->tremolo_waveform = XM_SINE_WAVEFORM;
    ch->tremolo_waveform_retrigger = true;
    ch->panbrello_waveform = XM_SINE_WAVEFORM;
    ch->panbrello_waveform_retrigger = true;
    ch->filter_cutoff = 127;   /* VDAC4: фильтра нет */
    ch->volume = ch->volume_envelope_volume = ch->fadeout_volume = 1.0f;
    ch->channel_volume = 1.0f;
    ch->panning = panning;
    ch->default_panning = panning;
    ch->tracker_format = TRACKER_FORMAT_S3M;
    ch->period_note_offset = .0f;
    ch->panning_envelope_panning = .5f;
    ch->actual_volume = .0f;
    ch->actual_panning = panning;
#if XM_RAMPING
    ch->target_volume = ch->volume;
    ch->target_panning = panning;
#endif
  }

  return 1;
}

int s3m_finish_context(xm_context_t **ctxp, xm_context_t *ctx)
{
  if (XM_DEFENSIVE && xm_check_sanity_postload(ctx))
  {
    xm_free_context(ctx);
    if (ctxp) *ctxp = NULL;
    s3m_set_error("S3M context failed postload sanity check");
    return -1;
  }

  return (int)ctx->ctx_size;
}

int s3m_create_context_safe_source(xm_context_t **ctxp, const s3m_read_source_t *source, uint32_t rate, MFUNC mfunc, size_t *out_bytes_needed)
{
  s3m_layout_t *layout;
  xm_context_group_sizes_t sizes;
  xm_context_group_cursor_t cursor;
  xm_context_t *ctx;

  if (ctxp) *ctxp = NULL;
  if (out_bytes_needed) *out_bytes_needed = 0;
  s3m_set_error("S3M loader was not called");

  if (!ctxp || !source || !mfunc || !rate)
  {
    s3m_set_error("S3M invalid loader parameter");
    return -1;
  }

  layout = (s3m_layout_t*)mfunc(sizeof(s3m_layout_t));
  if (!layout)
  {
    s3m_set_error("S3M layout allocation failed");
    return -2;
  }

  if (!s3m_read_layout_from_source(source, layout))
  {
    free(layout);
    return -1;
  }

  if (!s3m_get_group_sizes_for_layout(layout, &sizes))
  {
    s3m_set_error("S3M context size calculation failed");
    free(layout);
    return -1;
  }

  if (out_bytes_needed) *out_bytes_needed = sizes.total_size;
  if (!sizes.total_size)
  {
    s3m_set_error("S3M context size calculation failed");
    free(layout);
    return -1;
  }
  if (sizes.total_size > INT_MAX)
  {
    s3m_set_error_fmt("S3M context too large: %u", (unsigned)sizes.total_size);
    free(layout);
    return -2;
  }

  if (!xm_allocate_context_groups(&ctx, &cursor, &sizes, mfunc))
  {
    s3m_set_error_fmt("S3M context allocation failed: need=%u", (unsigned)sizes.total_size);
    free(layout);
    return -2;
  }
  *ctxp = ctx;

  if (!s3m_load_module_from_source_grouped(ctx, source, layout, &cursor, mfunc))
  {
    xm_free_context(ctx);
    *ctxp = NULL;
    free(layout);
    return -1;
  }

  if (!s3m_setup_context_runtime_grouped(ctx, layout, &cursor, rate))
  {
    s3m_set_error("S3M runtime context setup failed");
    xm_free_context(ctx);
    *ctxp = NULL;
    free(layout);
    return -1;
  }

  ctx->ctx_size = sizes.total_size;
  if (out_bytes_needed) *out_bytes_needed = ctx->ctx_size;

  free(layout);
  s3m_set_error("S3M OK");
  return s3m_finish_context(ctxp, ctx);
}

int s3m_create_context_safe(xm_context_t **ctxp, void *s3mdata, size_t s3mdata_length, uint32_t rate, MFUNC mfunc, size_t *out_bytes_needed)
{
  s3m_read_source_t source;

  if (!s3mdata)
  {
    if (ctxp) *ctxp = NULL;
    if (out_bytes_needed) *out_bytes_needed = 0;
    s3m_set_error("S3M invalid loader parameter");
    return -1;
  }

  source.user = s3mdata;
  source.size = s3mdata_length;
  source.read_at = s3m_memory_read_at;
  return s3m_create_context_safe_source(ctxp, &source, rate, mfunc, out_bytes_needed);
}

// ============================================================================
// Common tracker playback engine
// ============================================================================


/* ----- Static functions ----- */

static float xm_waveform(xm_waveform_type_t, uint8_t);
static void xm_autovibrato(xm_context_t*, xm_channel_context_t*);
static void xm_vibrato(xm_context_t*, xm_channel_context_t*, uint8_t, uint16_t);
static void xm_tremolo(xm_context_t*, xm_channel_context_t*, uint8_t, uint16_t);
static void xm_panbrello(xm_channel_context_t*, uint8_t, uint16_t);
static void xm_arpeggio(xm_context_t*, xm_channel_context_t*, uint8_t, uint16_t);
static void xm_tone_portamento(xm_context_t*, xm_channel_context_t*);
static void xm_pitch_slide(xm_context_t*, xm_channel_context_t*, float);
static void xm_panning_slide(xm_channel_context_t*, uint8_t);
static void xm_volume_slide(xm_context_t*, xm_channel_context_t*, uint8_t);
static void xm_invert_loop(xm_channel_context_t*);

static float xm_envelope_lerp(xm_envelope_point_t*, xm_envelope_point_t*, uint16_t);
static void xm_envelope_tick(xm_channel_context_t*, xm_envelope_t*, uint16_t*, float*);
static void xm_envelopes(xm_channel_context_t*);

static float xm_linear_period(float);
static float xm_linear_frequency(float);
static float xm_amiga_period(float);
static float xm_amiga_frequency(float);
static float xm_s3m_period(float);
static float xm_s3m_frequency(xm_channel_context_t*, float, float);
static float xm_period(xm_context_t*, float);
static float xm_frequency(xm_context_t*, xm_channel_context_t*, float, float);
static void xm_update_frequency(xm_context_t*, xm_channel_context_t*);
static void xm_filter_update(xm_context_t*, xm_channel_context_t*);

static void xm_handle_note_and_instrument(xm_context_t*, xm_channel_context_t*, xm_pattern_slot_t*);
static void xm_trigger_note(xm_context_t*, xm_channel_context_t*, unsigned int flags);
static void xm_cut_note(xm_channel_context_t*);
static void xm_key_off(xm_channel_context_t*);

static void xm_post_pattern_change(xm_context_t*);
static void xm_row(xm_context_t*);
static void xm_tick(xm_context_t*);

static float xm_sample_at(xm_sample_t*, size_t);
static float xm_next_of_sample(xm_channel_context_t*);
static void xm_sample_no_tick(xm_context_t*, float*, float*);
size_t xm_pingpong_forward_subrun(float, float, float, size_t);
size_t xm_pingpong_backward_subrun(float, float, float, size_t);
void xm_advance_no_loop(xm_channel_context_t*, size_t);
void xm_advance_forward_loop(xm_channel_context_t*, size_t);
void xm_advance_pingpong_loop(xm_channel_context_t*, size_t);
void xm_advance_gain_ramp_no_loop(xm_channel_context_t*, size_t, float, float);
void xm_advance_gain_ramp_forward_loop(xm_channel_context_t*, size_t, float, float);
void xm_advance_gain_ramp_pingpong_loop(xm_channel_context_t*, size_t, float, float);
void xm_render_no_loop_8(xm_channel_context_t*, float*, size_t);
void xm_render_no_loop_16(xm_channel_context_t*, float*, size_t);
void xm_render_forward_loop_8(xm_channel_context_t*, float*, size_t);
void xm_render_forward_loop_16(xm_channel_context_t*, float*, size_t);
void xm_render_pingpong_loop_8(xm_channel_context_t*, float*, size_t);
void xm_render_pingpong_loop_16(xm_channel_context_t*, float*, size_t);
void xm_render_sample_ramp_no_loop_8(xm_channel_context_t*, float*, size_t, float, float);
void xm_render_sample_ramp_no_loop_16(xm_channel_context_t*, float*, size_t, float, float);
void xm_render_sample_ramp_forward_loop_8(xm_channel_context_t*, float*, size_t, float, float);
void xm_render_sample_ramp_forward_loop_16(xm_channel_context_t*, float*, size_t, float, float);
void xm_render_sample_ramp_pingpong_loop_8(xm_channel_context_t*, float*, size_t, float, float);
void xm_render_sample_ramp_pingpong_loop_16(xm_channel_context_t*, float*, size_t, float, float);
void xm_render_gain_ramp_no_loop_8(xm_channel_context_t*, float*, size_t, float, float);
void xm_render_gain_ramp_no_loop_16(xm_channel_context_t*, float*, size_t, float, float);
void xm_render_gain_ramp_forward_loop_8(xm_channel_context_t*, float*, size_t, float, float);
void xm_render_gain_ramp_forward_loop_16(xm_channel_context_t*, float*, size_t, float, float);
void xm_render_gain_ramp_pingpong_loop_8(xm_channel_context_t*, float*, size_t, float, float);
void xm_render_gain_ramp_pingpong_loop_16(xm_channel_context_t*, float*, size_t, float, float);
void xm_sample(xm_context_t*, float*, float*);
void xm_render(xm_context_t*, float*, size_t);

/* ----- Other oddities ----- */

#define XM_TRIGGER_KEEP_VOLUME             (1 << 0)
#define XM_TRIGGER_KEEP_PERIOD             (1 << 1)
#define XM_TRIGGER_KEEP_SAMPLE_POSITION    (1 << 2)

static const uint16_t amiga_frequencies[] =
{
  1712, 1616, 1525, 1440, /* C-2, C#2, D-2, D#2 */
  1357, 1281, 1209, 1141, /* E-2, F-2, F#2, G-2 */
  1077, 1017, 961, 907,   /* G#2, A-2, A#2, B-2 */
  856,                    /* C-3 */
};

#define XM_S3M_C4_PERIOD      1712.f
#define XM_S3M_C4_FREQUENCY   261.625565f
#define XM_S3M_DEFAULT_C4SPEED 8363.f

static const float    multi_retrig_add[] =
{
  0.f,  -1.f,  -2.f, -4.f, /* 0, 1, 2, 3 */
  -8.f, -16.f, 0.f, 0.f,   /* 4, 5, 6, 7 */
  0.f,  1.f,   2.f, 4.f,   /* 8, 9, A, B */
  8.f,  16.f,  0.f, 0.f    /* C, D, E, F */
};

static const float    multi_retrig_multiply[] =
{
  1.f, 1.f, 1.f, 1.f,       /* 0, 1, 2, 3 */
  1.f, 1.f, .6666667f, .5f, /* 4, 5, 6, 7 */
  1.f, 1.f, 1.f, 1.f,       /* 8, 9, A, B */
  1.f, 1.f, 1.5f, 2.f       /* C, D, E, F */
};

#define XM_CLAMP_UP1F(vol, limit)            do {                  \
    if ((vol) > (limit)) (vol) = (limit);    \
} while(0)
#define XM_CLAMP_UP(vol)                     XM_CLAMP_UP1F((vol), 1.f)

#define XM_CLAMP_DOWN1F(vol, limit)          do {                \
    if ((vol) < (limit)) (vol) = (limit);    \
} while(0)
#define XM_CLAMP_DOWN(vol)                   XM_CLAMP_DOWN1F((vol), .0f)

#define XM_CLAMP2F(vol, up, down)            do {                  \
    if ((vol) > (up)) (vol)        = (up);                  \
    else if ((vol) < (down)) (vol) = (down); \
} while(0)
#define XM_CLAMP(vol)                        XM_CLAMP2F((vol), 1.f, .0f)

#define XM_SLIDE_TOWARDS(val, goal, incr)    do {          \
    if ((val) > (goal)) {                                            \
      (val) -= (incr);                                                \
      XM_CLAMP_DOWN1F((val), (goal));                 \
    } else if ((val) < (goal)) {                                     \
      (val) += (incr);                                                \
      XM_CLAMP_UP1F((val), (goal));                   \
    }                                                                                       \
} while(0)

#define XM_LERP(u, v, t)                     ((u) + (t) * ((v) - (u)))
#define XM_INVERSE_LERP(u, v, lerp)          (((lerp) - (u)) / ((v) - (u)))

#define HAS_TONE_PORTAMENTO(s)               ((s)->effect_type == 3 \
                                              || (s)->effect_type == 5 \
                                              || (s)->effect_type == XM_EFFECT_S3M_TONE_PORTAMENTO \
                                              || (s)->effect_type == XM_EFFECT_S3M_TONE_PORTAMENTO_VOLUME_SLIDE \
                                              || ((s)->volume_column >> 4) == 0xF)
#define HAS_ARPEGGIO(s)                      (((s)->effect_type == 0 \
                                               || (s)->effect_type == XM_EFFECT_S3M_ARPEGGIO) \
                                              && (s)->effect_param != 0)
#define HAS_VIBRATO(s)                       ((s)->effect_type == 4 \
                                              || (s)->effect_type == 6 \
                                              || (s)->effect_type == XM_EFFECT_S3M_VIBRATO \
                                              || (s)->effect_type == XM_EFFECT_S3M_VIBRATO_VOLUME_SLIDE \
                                              || (s)->effect_type == XM_EFFECT_S3M_FINE_VIBRATO \
                                              || ((s)->volume_column >> 4) == 0xB)
#define NOTE_IS_VALID(n)                     ((n) > 0 && (n) < 97)
#define SLOT_HAS_NOTE(s)                     ((s)->period != 0 || NOTE_IS_VALID((s)->note))
#define CHANNEL_IS_MOD(ch)                   ((ch)->tracker_format == TRACKER_FORMAT_MOD)
#define CHANNEL_IS_S3M(ch)                   ((ch)->tracker_format == TRACKER_FORMAT_S3M)
#define CHANNEL_HAS_DEFAULT_PANNING(ch)      ((ch)->default_panning >= .0f)
#define CHANNEL_USES_PERIOD_EFFECTS(ch)      (CHANNEL_IS_MOD(ch) || CHANNEL_IS_S3M(ch))

/* ----- Function definitions ----- */

static const uint8_t mod_efx_table[16] = {
  0, 5, 6, 7, 8, 10, 11, 13,
  16, 19, 22, 26, 32, 43, 64, 128
};

static float xm_waveform(xm_waveform_type_t waveform, uint8_t step)
{
  static unsigned int next_rand = 24492;

  step %= 0x40;

  switch (waveform)
  {
    case XM_SINE_WAVEFORM:
      /* Why not use a table? For saving space, and because there's very very little actual performance gain. */
      return -sinf(2.f * 3.141592f * (float)step / (float)0x40);

    case XM_RAMP_DOWN_WAVEFORM:
      /* Ramp down: 1.0f when step = 0; -1.0f when step = 0x40 */
      return (float)(0x20 - step) / 0x20;

    case XM_SQUARE_WAVEFORM:
      /* Square with a 50% duty */
      return (step >= 0x20) ? 1.f : -1.f;

    case XM_RANDOM_WAVEFORM:
      /* Use the POSIX.1-2001 example, just to be deterministic across different machines */
      next_rand = next_rand * 1103515245 + 12345;
      return (float)((next_rand >> 16) & 0x7FFF) / (float)0x4000 - 1.f;

    case XM_RAMP_UP_WAVEFORM:
      /* Ramp up: -1.f when step = 0; 1.f when step = 0x40 */
      return (float)(step - 0x20) / 0x20;

    default:
      break;
  }

  return .0f;
}

static void xm_autovibrato(xm_context_t* ctx, xm_channel_context_t* ch)
{
  if (ch->instrument == NULL || ch->instrument->vibrato_depth == 0)
    return;

  xm_instrument_t* instr = ch->instrument;
  float          sweep   = 1.f;

  if (ch->autovibrato_ticks < instr->vibrato_sweep)
  {
    /* No idea if this is correct, but it sounds close enough… */
    sweep = XM_LERP(0.f, 1.f, (float)ch->autovibrato_ticks / (float)instr->vibrato_sweep);
  }

  unsigned int step = ((ch->autovibrato_ticks++) * instr->vibrato_rate) >> 2;
  ch->autovibrato_note_offset = .25f * xm_waveform(instr->vibrato_type, step) * (float)instr->vibrato_depth / (float)0xF * sweep;
  xm_update_frequency(ctx, ch);
}

static void xm_vibrato(xm_context_t* ctx, xm_channel_context_t* ch, uint8_t param, uint16_t pos)
{
  unsigned int step = pos * (param >> 4);

  if (CHANNEL_USES_PERIOD_EFFECTS(ch))
  {
    ch->vibrato_note_offset = .0f;
    ch->vibrato_period_offset = -xm_waveform(ch->vibrato_waveform, step) * (float)((param & 0x0F) << 1);
  }
  else
  {
    ch->vibrato_period_offset = .0f;
    ch->vibrato_note_offset = 2.f * xm_waveform(ch->vibrato_waveform, step) * (float)(param & 0x0F) / (float)0xF;
  }

  xm_update_frequency(ctx, ch);
}

static void xm_tremolo(xm_context_t* ctx, xm_channel_context_t* ch, uint8_t param, uint16_t pos)
{
  unsigned int step = pos * (param >> 4);

  /* Not so sure about this, it sounds correct by ear compared with MilkyTracker, but it could come from other bugs */
  if (CHANNEL_USES_PERIOD_EFFECTS(ch))
    ch->tremolo_volume = -1.f * xm_waveform(ch->tremolo_waveform, step) * (float)(param & 0x0F) / (float)0x10;
  else
    ch->tremolo_volume = -1.f * xm_waveform(ch->tremolo_waveform, step) * (float)(param & 0x0F) / (float)0xF;
}

static void xm_panbrello(xm_channel_context_t* ch, uint8_t param, uint16_t pos)
{
  unsigned int step = pos * (param >> 4);

  ch->panbrello_panning_offset = xm_waveform(ch->panbrello_waveform, step) * (float)(param & 0x0F) / 64.f;
}

static void xm_arpeggio(xm_context_t* ctx, xm_channel_context_t* ch, uint8_t param, uint16_t tick)
{
  switch (tick % 3)
  {
    case 0:
      ch->arp_in_progress = false;
      ch->arp_note_offset = 0;
      break;

    case 1:
      ch->arp_in_progress = true;
      if (ch->current != NULL && ch->current->period != 0)
        ch->arp_note_offset = param >> 4;
      else
        ch->arp_note_offset = param & 0x0F;
      break;

    case 2:
      ch->arp_in_progress = true;
      if (ch->current != NULL && ch->current->period != 0)
        ch->arp_note_offset = param & 0x0F;
      else
        ch->arp_note_offset = param >> 4;
      break;
  }

  xm_update_frequency(ctx, ch);
}

static void xm_tone_portamento(xm_context_t* ctx, xm_channel_context_t* ch)
{
  /* 3xx called without a note, wait until we get an actual target note. */
  if (ch->tone_portamento_target_period == 0.f)
    return;

  if (ch->period != ch->tone_portamento_target_period)
  {
    float slide_scale = (ctx->module.frequency_type == XM_LINEAR_FREQUENCIES) ? 4.f : 1.f;

    XM_SLIDE_TOWARDS(ch->period,
                     ch->tone_portamento_target_period,
                     slide_scale * ch->tone_portamento_param
                     );
    xm_update_frequency(ctx, ch);
  }
}

static void xm_pitch_slide(xm_context_t* ctx, xm_channel_context_t* ch, float period_offset)
{
  if (ctx->module.frequency_type == XM_LINEAR_FREQUENCIES)
    period_offset *= 4.f;

  ch->period += period_offset;
  XM_CLAMP_DOWN(ch->period);
  /* XXX: upper bound of period ? */

  xm_update_frequency(ctx, ch);
}

static void xm_panning_slide(xm_channel_context_t* ch, uint8_t rawval)
{
  float f;

  if ((rawval & 0xF0) && (rawval & 0x0F))  /* Illegal state */
    return;

  if (rawval & 0xF0)
  {
    /* Slide right */
    f = (float)(rawval >> 4) / (float)0xFF;
    ch->panning += f;
    XM_CLAMP_UP(ch->panning);
  }
  else
  {
    /* Slide left */
    f = (float)(rawval & 0x0F) / (float)0xFF;
    ch->panning -= f;
    XM_CLAMP_DOWN(ch->panning);
  }
}

static void xm_volume_slide(xm_context_t* ctx, xm_channel_context_t* ch, uint8_t rawval)
{
  float f;

  if (CHANNEL_IS_S3M(ch))
  {
    if ((rawval & 0x0F) == 0x0F && (rawval & 0xF0))
    {
      if (ctx->current_tick == 0)
      {
        f = (float)(rawval >> 4) / (float)0x40;
        ch->volume += f;
        XM_CLAMP_UP(ch->volume);
      }
      return;
    }

    if ((rawval & 0xF0) == 0xF0 && (rawval & 0x0F))
    {
      if (ctx->current_tick == 0)
      {
        f = (float)(rawval & 0x0F) / (float)0x40;
        ch->volume -= f;
        XM_CLAMP_DOWN(ch->volume);
      }
      return;
    }

    if (ctx->current_tick == 0 && !ctx->s3m_fast_volume_slides) return;

    if (rawval & 0x0F)
    {
      f = (float)(rawval & 0x0F) / (float)0x40;
      ch->volume -= f;
      XM_CLAMP_DOWN(ch->volume);
      return;
    }

    f = (float)(rawval >> 4) / (float)0x40;
    ch->volume += f;
    XM_CLAMP_UP(ch->volume);
    return;
  }

  if ((rawval & 0xF0) && (rawval & 0x0F))
  {
    /* Illegal state */
    return;
  }

  if (rawval & 0xF0)
  {
    /* Slide up */
    f = (float)(rawval >> 4) / (float)0x40;
    ch->volume += f;
    XM_CLAMP_UP(ch->volume);
  }
  else
  {
    /* Slide down */
    f = (float)(rawval & 0x0F) / (float)0x40;
    ch->volume -= f;
    XM_CLAMP_DOWN(ch->volume);
  }
}

static void xm_invert_loop(xm_channel_context_t* ch)
{
  if (xm_scanning)
    return;
  uint32_t loop_length;
  uint32_t pos;

  if (!ch || !CHANNEL_IS_MOD(ch) || !ch->invert_loop_speed) return;
  if (!ch->sample || ch->sample->bits != 8 || !ch->sample->data8) return;
  if (ch->sample->loop_type == XM_NO_LOOP) return;
  if (ch->sample->loop_start >= ch->sample->length || ch->sample->loop_length <= 2) return;

  loop_length = ch->sample->loop_length;
  if (loop_length > ch->sample->length - ch->sample->loop_start)
    loop_length = ch->sample->length - ch->sample->loop_start;
  if (loop_length <= 2) return;

  ch->invert_loop_delay += mod_efx_table[ch->invert_loop_speed & 0x0F];
  if (ch->invert_loop_delay < 128) return;

  ch->invert_loop_delay = 0;
  ch->invert_loop_offset++;
  if (ch->invert_loop_offset >= loop_length) ch->invert_loop_offset = 0;

  pos = ch->sample->loop_start + ch->invert_loop_offset;
  ch->sample->data8[pos] = ~ch->sample->data8[pos];
}

static float xm_envelope_lerp(xm_envelope_point_t* a, xm_envelope_point_t* b, uint16_t pos)
{
  /* Linear interpolation between two envelope points */
  if (pos <= a->frame)
    return a->value;
  else if (pos >= b->frame)
    return b->value;
  else
  {
    float p = (float)(pos - a->frame) / (float)(b->frame - a->frame);
    return a->value * (1 - p) + b->value * p;
  }
}

static void xm_post_pattern_change(xm_context_t* ctx)
{
  /* Loop if necessary */
  if (ctx->module.length == 0)
  {
    ctx->current_table_index = 0;
    ctx->current_row = 0;
    return;
  }

  if (ctx->current_table_index >= ctx->module.length)
  {
    if (ctx->module.restart_position < ctx->module.length)
      ctx->current_table_index = ctx->module.restart_position;
    else
      ctx->current_table_index = 0;
  }
}

static float xm_linear_period(float note)
{
  return 7680.f - note * 64.f;
}

static float xm_linear_frequency(float period)
{
  return 8363.f * powf(2.f, (4608.f - period) / 768.f);
}

static float xm_amiga_period(float note)
{
  unsigned int intnote = note;
  uint8_t      a = intnote % 12;
  int8_t       octave = note / 12.f - 2;
  uint16_t     p1 = amiga_frequencies[a], p2 = amiga_frequencies[a + 1];

  if (octave > 0)
  {
    p1 >>= octave;
    p2 >>= octave;
  }
  else if (octave < 0)
  {
    p1 <<= (-octave);
    p2 <<= (-octave);
  }

  return XM_LERP(p1, p2, note - intnote);
}

static float xm_amiga_frequency(float period)
{
  if (period == .0f)
    return .0f;

  /* This is the PAL value. No reason to choose this one over the NTSC value. */
  return 7093789.2f / (period * 2.f);
}

static float xm_s3m_period(float note)
{
  return XM_S3M_C4_PERIOD * powf(2.f, (48.f - note) / 12.f);
}

static float xm_s3m_frequency(xm_channel_context_t* ch, float period, float note_offset)
{
  float c4speed = XM_S3M_DEFAULT_C4SPEED;

  if (period <= .0f) return .0f;
  if (ch != NULL && ch->sample != NULL && ch->sample->c4speed != 0)
    c4speed = (float)ch->sample->c4speed;

  return c4speed * XM_S3M_C4_PERIOD * powf(2.f, note_offset / 12.f) / period;
}

static float xm_period(xm_context_t* ctx, float note)
{
  switch (ctx->module.frequency_type)
  {
    case XM_LINEAR_FREQUENCIES:
      return xm_linear_period(note);

    case XM_AMIGA_FREQUENCIES:
      return xm_amiga_period(note);

    case XM_S3M_FREQUENCIES:
      return xm_s3m_period(note);
  }

  return .0f;
}

static float xm_frequency(xm_context_t* ctx, xm_channel_context_t* ch, float period, float note_offset)
{
  uint8_t  a;
  int8_t   octave;
  float    note;
  uint16_t p1, p2;

  switch (ctx->module.frequency_type)
  {
    case XM_LINEAR_FREQUENCIES:
      return xm_linear_frequency(period - 64.f * note_offset);

    case XM_AMIGA_FREQUENCIES:
      if (note_offset == 0)
      {
        /* A chance to escape from insanity */
        return xm_amiga_frequency(period);
      }

      /* FIXME: this is very crappy at best */
      a = octave = 0;

      /* Find the octave of the current period */
      if (period > amiga_frequencies[0])
      {
        --octave;
        while (period > (amiga_frequencies[0] << (-octave)))
          --octave;
      }
      else if (period < amiga_frequencies[12])
      {
        ++octave;
        while (period < (amiga_frequencies[12] >> octave))
          ++octave;
      }

      /* Find the smallest note closest to the current period */
      for (uint8_t i = 0; i < 12; ++i)
      {
        p1 = amiga_frequencies[i], p2 = amiga_frequencies[i + 1];

        if (octave > 0)
        {
          p1 >>= octave;
          p2 >>= octave;
        }
        else if (octave < 0)
        {
          p1 <<= (-octave);
          p2 <<= (-octave);
        }

        if (p2 <= period && period <= p1)
        {
          a = i;
          break;
        }
      }

      if (XM_DEBUG && (p1 < period || p2 > period))
      {
        DEBUG("%i <= %f <= %i should hold but doesn't, this is a bug", p2, period, p1);
      }

      note = 12.f * (octave + 2) + a + XM_INVERSE_LERP(p1, p2, period);

      return xm_amiga_frequency(xm_amiga_period(note + note_offset));

    case XM_S3M_FREQUENCIES:
      return xm_s3m_frequency(ch, period, note_offset);
  }

  return .0f;
}

/* VDAC4: резонансный фильтр IT (двухполюсный, формулы Olivier Lapicque —
 * как filter.c libxmp): коэффициенты по срезу и резонансу канала. */
static void xm_filter_update(xm_context_t* ctx, xm_channel_context_t* ch)
{
  ch->filter_active = ch->filter_cutoff < 127 || ch->filter_resonance > 0;
  if (!ch->filter_active)
    return;
  const float rate = (float)ctx->rate;
  float fc = 110.f * powf(2.f, (float)ch->filter_cutoff / 24.f + .25f);
  if (fc > rate / 2.f)
    fc = rate / 2.f;
  const float r = rate / (2.f * 3.14159265f * fc);
  const float d = powf(10.f, -3.f * (float)ch->filter_resonance / 320.f) * (r + 1.f) - 1.f;
  const float e = r * r;
  ch->filter_a0 = 1.f / (1.f + d + e);
  ch->filter_b0 = (d + e + e) / (1.f + d + e);
  ch->filter_b1 = -e / (1.f + d + e);
}

static void xm_update_frequency(xm_context_t* ctx, xm_channel_context_t* ch)
{
  if (xm_scanning)
    return;
  float note_offset = ch->period_note_offset;
  float period = ch->period;

  if (ch->arp_note_offset > 0)
    note_offset += ch->arp_note_offset;
  else
  {
    note_offset += ch->vibrato_note_offset + ch->autovibrato_note_offset;
    period += ch->vibrato_period_offset;
    XM_CLAMP_DOWN(period);
  }

  if (ch->s3m_glissando_control && ch->tone_portamento_target_period != .0f && period > .0f)
  {
    if (CHANNEL_IS_S3M(ch))
    {
      float note = 48.f - 12.f * log2f(period / XM_S3M_C4_PERIOD);
      period = xm_s3m_period(floorf(note + .5f));
    }
    else
    {
      /* VDAC4: глиссандо XM и MOD (E3y): период тон-портаменто скользит как
       * обычно, а звучит ближайший полутон с точной подстройкой сэмпла. */
      const float fine = ch->sample != NULL ? ch->sample->finetune / 128.f : 0.f;
      const float note = ctx->module.frequency_type == XM_LINEAR_FREQUENCIES
                           ? (7680.f - period) / 64.f - fine
                           : 12.f * log2f(xm_amiga_period(fine) / period);
      period = xm_period(ctx, floorf(note + .5f) + fine);
    }
  }

  ch->frequency = xm_frequency(ctx, ch, period, note_offset);
  ch->step = ch->frequency / ctx->rate;
}


int s3m_context_uses_adlib(const xm_context_t *ctx)
{
  return ctx && ctx->tracker_format == TRACKER_FORMAT_S3M &&
         ctx->s3m_adlib_instruments && ctx->s3m_channel_opl;
}

int s3m_adlib_channel_index(const xm_context_t *ctx, const xm_channel_context_t *ch)
{
  if (!ctx || !ch || !ctx->channels) return -1;
  ptrdiff_t index = ch - ctx->channels;
  if (index < 0 || index >= ctx->module.num_channels) return -1;
  return (int)index;
}

uint8_t s3m_adlib_opl_channel(const xm_context_t *ctx, const xm_channel_context_t *ch)
{
  int index = s3m_adlib_channel_index(ctx, ch);
  if (index < 0 || !ctx->s3m_channel_opl) return S3M_ADLIB_CHANNEL_NONE;
  return ctx->s3m_channel_opl[index];
}

const s3m_adlib_instrument_t *s3m_adlib_current_instrument(const xm_context_t *ctx, const xm_channel_context_t *ch, uint16_t *out_index)
{
  if (out_index) *out_index = 0;
  if (!s3m_context_uses_adlib(ctx) || !ch || !ch->instrument) return NULL;

  ptrdiff_t instrument_index = ch->instrument - ctx->module.instruments;
  if (instrument_index < 0 || instrument_index >= ctx->module.num_instruments) return NULL;
  if ((uint16_t)instrument_index >= ctx->s3m_adlib_instrument_count) return NULL;

  const s3m_adlib_instrument_t *adlib = ctx->s3m_adlib_instruments + instrument_index;
  if (adlib->type != S3M_SAMPLE_TYPE_ADLIB) return NULL;

  if (out_index) *out_index = (uint16_t)instrument_index;
  return adlib;
}

uint8_t s3m_adlib_operator_offset(uint8_t opl_channel, int carrier)
{
  if (opl_channel >= 9) return 0;

  if (carrier)
  {
    switch (opl_channel)
    {
      case 0: return 3;
      case 1: return 4;
      case 2: return 5;
      case 3: return 11;
      case 4: return 12;
      case 5: return 13;
      case 6: return 19;
      case 7: return 20;
      case 8: return 21;
    }
  }

  switch (opl_channel)
  {
    case 0: return 0;
    case 1: return 1;
    case 2: return 2;
    case 3: return 8;
    case 4: return 9;
    case 5: return 10;
    case 6: return 16;
    case 7: return 17;
    case 8: return 18;
  }

  return 0;
}

uint8_t s3m_adlib_pan_bits(const xm_channel_context_t *ch)
{
  if (!ch) return 0x30;
  if (ch->panning < .25f) return 0x10;
  if (ch->panning > .75f) return 0x20;
  return 0x30;
}

uint8_t s3m_adlib_scaled_total_level(uint8_t base, float volume)
{
  if (volume < .0f) volume = .0f;
  if (volume > 1.f) volume = 1.f;

  uint8_t level = base & 0x3F;
  uint8_t attenuation = (uint8_t)((1.f - volume) * 63.f + .5f);
  uint16_t scaled = (uint16_t)level + attenuation;
  if (scaled > 63) scaled = 63;
  return (base & 0xC0) | (uint8_t)scaled;
}

void s3m_adlib_frequency_to_opl(float frequency, uint16_t *out_fnum, uint8_t *out_block)
{
  uint16_t fnum = 0;
  uint8_t block = 0;

  if (frequency > .0f)
  {
    for (block = 0; block < 7; block++)
    {
      float f = frequency * ldexpf(1.f, 20 - block) / 49716.f;
      if (f <= 1023.f)
      {
        fnum = (uint16_t)(f + .5f);
        break;
      }
    }

    if (block == 7)
    {
      float f = frequency * ldexpf(1.f, 13) / 49716.f;
      fnum = f > 1023.f ? 1023 : (uint16_t)(f + .5f);
    }
  }

  if (out_fnum) *out_fnum = fnum;
  if (out_block) *out_block = block;
}

float s3m_adlib_channel_frequency(xm_channel_context_t *ch)
{
  if (!ch || ch->period <= .0f) return .0f;

  float period = ch->period;
  float note_offset = ch->period_note_offset;

  if (ch->arp_note_offset > 0)
    note_offset += ch->arp_note_offset;
  else
  {
    note_offset += ch->vibrato_note_offset + ch->autovibrato_note_offset;
    period += ch->vibrato_period_offset;
    XM_CLAMP_DOWN(period);
  }

  if (ch->s3m_glissando_control && ch->tone_portamento_target_period != .0f && period > .0f)
  {
    float note = 48.f - 12.f * log2f(period / XM_S3M_C4_PERIOD);
    period = xm_s3m_period(floorf(note + .5f));
  }

  if (period <= .0f) return .0f;
  return XM_S3M_C4_FREQUENCY * XM_S3M_C4_PERIOD * powf(2.f, note_offset / 12.f) / period;
}

float s3m_adlib_channel_volume(const xm_context_t *ctx, const xm_channel_context_t *ch)
{
  if (!ctx || !ch) return .0f;

#if XM_RAMPING
  float volume = ch->target_volume;
#else
  float volume = ch->actual_volume;
#endif

  if (ch->muted || (ch->instrument && ch->instrument->muted)) volume = .0f;
  volume *= ctx->global_volume;
  XM_CLAMP(volume);
  return volume;
}

void s3m_adlib_write_key_off(uint8_t opl_channel)
{
  if (opl_channel >= 9) return;
  uint8_t reg = (uint8_t)(0xB0 + opl_channel);
  opl_write_fm_reg(reg, opl_read_fm_reg(reg) & 0xDF);
}

void s3m_adlib_write_frequency(uint8_t opl_channel, float frequency, int key_on)
{
  if (opl_channel >= 9) return;

  uint16_t fnum;
  uint8_t block;
  s3m_adlib_frequency_to_opl(frequency, &fnum, &block);

  OPL_FM_REG_WRITE writes[2];
  writes[0].reg = (uint8_t)(0xA0 + opl_channel);
  writes[0].value = (uint8_t)(fnum & 0xFF);
  writes[1].reg = (uint8_t)(0xB0 + opl_channel);
  writes[1].value = (uint8_t)(((fnum >> 8) & 0x03) | ((block & 0x07) << 2) | (key_on ? 0x20 : 0x00));
  opl_write_fm_regs(writes, 2);
}

void s3m_adlib_write_instrument(const xm_context_t *ctx, const xm_channel_context_t *ch, uint8_t opl_channel, const s3m_adlib_instrument_t *adlib)
{
  if (!ctx || !ch || !adlib || opl_channel >= 9) return;

  uint8_t mod_op = s3m_adlib_operator_offset(opl_channel, 0);
  uint8_t car_op = s3m_adlib_operator_offset(opl_channel, 1);
  float volume = s3m_adlib_channel_volume(ctx, ch);

  OPL_FM_REG_WRITE writes[11];
  writes[0].reg = (uint8_t)(0x20 + mod_op);
  writes[0].value = adlib->regs[0];
  writes[1].reg = (uint8_t)(0x20 + car_op);
  writes[1].value = adlib->regs[1];
  writes[2].reg = (uint8_t)(0x40 + mod_op);
  writes[2].value = (adlib->regs[10] & 0x01) ? s3m_adlib_scaled_total_level(adlib->regs[2], volume) : adlib->regs[2];
  writes[3].reg = (uint8_t)(0x40 + car_op);
  writes[3].value = s3m_adlib_scaled_total_level(adlib->regs[3], volume);
  writes[4].reg = (uint8_t)(0x60 + mod_op);
  writes[4].value = adlib->regs[4];
  writes[5].reg = (uint8_t)(0x60 + car_op);
  writes[5].value = adlib->regs[5];
  writes[6].reg = (uint8_t)(0x80 + mod_op);
  writes[6].value = adlib->regs[6];
  writes[7].reg = (uint8_t)(0x80 + car_op);
  writes[7].value = adlib->regs[7];
  writes[8].reg = (uint8_t)(0xE0 + mod_op);
  writes[8].value = adlib->regs[8] & 0x07;
  writes[9].reg = (uint8_t)(0xE0 + car_op);
  writes[9].value = adlib->regs[9] & 0x07;
  writes[10].reg = (uint8_t)(0xC0 + opl_channel);
  writes[10].value = (adlib->regs[10] & 0x0F) | s3m_adlib_pan_bits(ch);
  opl_write_fm_regs(writes, 11);
}

void s3m_adlib_write_volume(const xm_context_t *ctx, const xm_channel_context_t *ch, uint8_t opl_channel, const s3m_adlib_instrument_t *adlib)
{
  if (!ctx || !ch || !adlib || opl_channel >= 9) return;

  uint8_t mod_op = s3m_adlib_operator_offset(opl_channel, 0);
  uint8_t car_op = s3m_adlib_operator_offset(opl_channel, 1);
  float volume = s3m_adlib_channel_volume(ctx, ch);

  OPL_FM_REG_WRITE writes[2];
  size_t count = 0;

  if (adlib->regs[10] & 0x01)
  {
    writes[count].reg = (uint8_t)(0x40 + mod_op);
    writes[count].value = s3m_adlib_scaled_total_level(adlib->regs[2], volume);
    count++;
  }

  writes[count].reg = (uint8_t)(0x40 + car_op);
  writes[count].value = s3m_adlib_scaled_total_level(adlib->regs[3], volume);
  count++;

  opl_write_fm_regs(writes, count);
}

void s3m_adlib_sync_channel(xm_context_t *ctx, xm_channel_context_t *ch)
{
  if (!s3m_context_uses_adlib(ctx) || !ch) return;

  uint8_t opl_channel = s3m_adlib_opl_channel(ctx, ch);
  if (opl_channel == S3M_ADLIB_CHANNEL_NONE) return;

  if (opl_channel >= 9)
  {
    ch->s3m_opl_key_on = 0;
    return;
  }

  uint16_t instrument_index;
  const s3m_adlib_instrument_t *adlib = s3m_adlib_current_instrument(ctx, ch, &instrument_index);
  if (!adlib || ch->sample_position < 0 || ch->s3m_note_cut)
  {
    if (ch->s3m_opl_key_on)
    {
      s3m_adlib_write_key_off(opl_channel);
      ch->s3m_opl_key_on = 0;
    }
    return;
  }

  if (!opl_is_enabled()) return;

  int new_trigger = ch->s3m_opl_trigger_id != ch->s3m_opl_synced_trigger_id;
  int new_instrument = ch->s3m_opl_instrument != (uint8_t)(instrument_index + 1);

  if (!ch->s3m_opl_key_on || new_trigger || new_instrument)
  {
    s3m_adlib_write_key_off(opl_channel);
    if (!ch->s3m_opl_key_on || new_instrument)
      s3m_adlib_write_instrument(ctx, ch, opl_channel, adlib);
    else
      s3m_adlib_write_volume(ctx, ch, opl_channel, adlib);
    ch->s3m_opl_key_on = 1;
    ch->s3m_opl_instrument = (uint8_t)(instrument_index + 1);
    ch->s3m_opl_synced_trigger_id = ch->s3m_opl_trigger_id;
  }
  else
  {
    s3m_adlib_write_volume(ctx, ch, opl_channel, adlib);
    opl_write_fm_reg((uint8_t)(0xC0 + opl_channel), (adlib->regs[10] & 0x0F) | s3m_adlib_pan_bits(ch));
  }

  s3m_adlib_write_frequency(opl_channel, s3m_adlib_channel_frequency(ch), 1);
}

void s3m_adlib_all_notes_off(xm_context_t *ctx)
{
  if (!s3m_context_uses_adlib(ctx)) return;

  for (uint16_t i = 0; i < ctx->module.num_channels; i++)
  {
    uint8_t opl_channel = ctx->s3m_channel_opl[i];
    if (opl_channel < 9)
      s3m_adlib_write_key_off(opl_channel);

    ctx->channels[i].s3m_opl_key_on = 0;
    ctx->channels[i].s3m_opl_instrument = 0;
    ctx->channels[i].s3m_opl_trigger_id = 0;
    ctx->channels[i].s3m_opl_synced_trigger_id = 0;
  }
}

void s3m_adlib_force_opl2_fm_mode()
{
  OPL_FM_REG_WRITE writes[3];
  writes[0].reg = 0x0105;
  writes[0].value = 0x01;
  writes[1].reg = 0x0104;
  writes[1].value = 0x00;
  writes[2].reg = 0x0105;
  writes[2].value = 0x00;
  opl_write_fm_regs(writes, 3);
}

esp_err_t s3m_adlib_prepare_opl(xm_context_t *ctx)
{
  if (!s3m_context_uses_adlib(ctx)) return ESP_OK;

  OPL_INFO info;
  esp_err_t err = opl_get_info(&info);
  if (err != ESP_OK) return err;

  int use_opl2_melody_fast_path = 0;

  if (!info.enabled)
  {
    err = opl_enable(OPL_MODE_YMF262, 0);
    if (err != ESP_OK) return err;
    use_opl2_melody_fast_path = 1;
  }
  else if (info.mode == OPL_MODE_YMF262)
    use_opl2_melody_fast_path = 1;

  if (use_opl2_melody_fast_path)
    s3m_adlib_force_opl2_fm_mode();

  opl_reset();

  if (use_opl2_melody_fast_path)
  {
    err = opl_set_fm_render_mode(OPL_FM_RENDER_OPL2_MELODY);
    if (err != ESP_OK) return err;
  }

  return ESP_OK;
}

static void xm_handle_note_and_instrument(xm_context_t* ctx, xm_channel_context_t* ch, xm_pattern_slot_t* s)
{
  xm_pattern_slot_t s3m_memory_slot;

  if (CHANNEL_IS_S3M(ch))
  {
    bool s3m_uses_shared_memory = false;

    switch (s->effect_type)
    {
      case XM_EFFECT_S3M_VOLUME_SLIDE:
      case XM_EFFECT_S3M_PORTAMENTO_DOWN:
      case XM_EFFECT_S3M_PORTAMENTO_UP:
      case XM_EFFECT_S3M_TREMOR:
      case XM_EFFECT_S3M_ARPEGGIO:
      case XM_EFFECT_S3M_VIBRATO_VOLUME_SLIDE:
      case XM_EFFECT_S3M_TONE_PORTAMENTO_VOLUME_SLIDE:
      case XM_EFFECT_S3M_RETRIG:
      case XM_EFFECT_S3M_TREMOLO:
      case XM_EFFECT_S3M_EXTENDED:
        s3m_uses_shared_memory = true;
        break;

      default:
        break;
    }

    if (s->effect_type == XM_EFFECT_S3M_NONE)
    {
      if (s->effect_param)
        ch->s3m_effect_memory = s->effect_param;
    }
    else if (s3m_uses_shared_memory)
    {
      if (s->effect_param)
        ch->s3m_effect_memory = s->effect_param;
      else if (ch->s3m_effect_memory)
      {
        s3m_memory_slot = *s;
        s3m_memory_slot.effect_param = ch->s3m_effect_memory;
        s = &s3m_memory_slot;
      }
    }
  }

  if (s->instrument > 0)
  {
    if (CHANNEL_IS_MOD(ch) && NOTE_IS_VALID(s->note)) ch->invert_loop_offset = 0;

    if (HAS_TONE_PORTAMENTO(ch->current) && ch->instrument != NULL && ch->sample != NULL)
    {
      if (CHANNEL_USES_PERIOD_EFFECTS(ch))
      {
        if (s->instrument > ctx->module.num_instruments)
        {
          xm_cut_note(ch);
          ch->instrument = NULL;
          ch->sample     = NULL;
        }
        else
        {
          /* MOD tone-portamento row with instrument: update instrument params, but do not retrigger. */
          ch->instrument = ctx->module.instruments + (s->instrument - 1);
          if (ch->instrument->num_samples > 0 && ch->instrument->samples != NULL)
            ch->volume = ch->instrument->samples[0].volume;
        }
      }
      else
      {
        /* Tone portamento in effect, unclear stuff happens */
        xm_trigger_note(ctx, ch, XM_TRIGGER_KEEP_PERIOD | XM_TRIGGER_KEEP_SAMPLE_POSITION);
      }
    }
    else if (s->instrument > ctx->module.num_instruments)
    {
      /* Invalid instrument, Cut current note */
      xm_cut_note(ch);
      ch->instrument = NULL;
      ch->sample     = NULL;
    }
    else
    {
      ch->instrument = ctx->module.instruments + (s->instrument - 1);
      if (s->note == 0 && s->period == 0 && ch->sample != NULL)
      {
        if (CHANNEL_USES_PERIOD_EFFECTS(ch))
        {
          /* MOD/S3M instrument-only row: update instrument parameters, but do not retrigger. */
          if (ch->instrument->num_samples > 0 && ch->instrument->samples != NULL)
            ch->volume = ch->instrument->samples[0].volume;
        }
        else
        {
          /* Ghost instrument, trigger note */
          /* Sample position is kept, but envelopes are reset */
          xm_trigger_note(ctx, ch, XM_TRIGGER_KEEP_SAMPLE_POSITION);
        }
      }
    }
  }

  if (SLOT_HAS_NOTE(s))
  {
    /* Yes, the real XM note number is s->note -1. Try finding THAT in any of the specs! :-) */

    xm_instrument_t* instr = ch->instrument;

    if (HAS_TONE_PORTAMENTO(ch->current) && instr != NULL && ch->sample != NULL)
    {
      /* Tone portamento in effect */
      if (s->period != 0)
      {
        ch->period_note_offset = ch->sample->finetune / 128.f;
        ch->tone_portamento_target_period = s->period;
      }
      else
      {
        ch->period_note_offset = .0f;
        ch->note = s->note + ch->sample->relative_note + ch->sample->finetune / 128.f - 1.f;
        ch->tone_portamento_target_period = xm_period(ctx, ch->note);
      }
    }
    else if (instr == NULL || ch->instrument->num_samples == 0)  /* Bad instrument */
      xm_cut_note(ch);
    else
    {
      uint8_t sample_index = 0;
      if (s->period == 0)
        sample_index = instr->sample_of_notes[s->note - 1];

      if (sample_index < instr->num_samples)
      {
#if XM_RAMPING
        for (unsigned int z = 0; z < XM_SAMPLE_RAMPING_POINTS; ++z)
          ch->end_of_previous_sample[z] = xm_next_of_sample(ch);

        ch->frame_count = 0;
#endif

        ch->sample = instr->samples + sample_index;
        if (s->period != 0)
        {
          ch->orig_note = ch->note = .0f;
          ch->period_note_offset = ch->sample->finetune / 128.f;
        }
        else
        {
          ch->orig_note = ch->note = s->note + ch->sample->relative_note
                                     + ch->sample->finetune / 128.f - 1.f;
          ch->period_note_offset = .0f;
        }

        if (s->instrument > 0)
          xm_trigger_note(ctx, ch, 0);
        else  /* Ghost note: keep old volume */
          xm_trigger_note(ctx, ch, XM_TRIGGER_KEEP_VOLUME);
      }
      else  /* Bad sample */
        xm_cut_note(ch);
    }
  }
  else if (s->note == 97)  /* Key Off */
    xm_key_off(ch);

  if (CHANNEL_IS_S3M(ch) && s->volume_column >= 128 && s->volume_column <= 192)
  {
    ch->surround = false;
    ch->panning = (float)(s->volume_column - 128) / 64.f;
  }
  else switch (s->volume_column >> 4)
  {
    case 0x5:
      if (s->volume_column > 0x50)
        break;

    case 0x1:
    case 0x2:
    case 0x3:
    case 0x4: /* Set volume */
      ch->volume = (float)(s->volume_column - 0x10) / (float)0x40;
      break;

    case 0x8: /* Fine volume slide down */
      xm_volume_slide(ctx, ch, s->volume_column & 0x0F);
      break;

    case 0x9: /* Fine volume slide up */
      xm_volume_slide(ctx, ch, s->volume_column << 4);
      break;

    case 0xA: /* Set vibrato speed */
      ch->vibrato_param = (ch->vibrato_param & 0x0F) | ((s->volume_column & 0x0F) << 4);
      break;

    case 0xC: /* Set panning */
      ch->panning = (float)(
        ((s->volume_column & 0x0F) << 4) | (s->volume_column & 0x0F)) / (float)0xFF;
      break;

    case 0xF: /* Tone portamento */
      if (s->volume_column & 0x0F)
        ch->tone_portamento_param = ((s->volume_column & 0x0F) << 4) | (s->volume_column & 0x0F);
      break;

    default:
      break;
  }

  switch (s->effect_type)
  {
    case 0: /* 0xy: XM/MOD arpeggio */
    case XM_EFFECT_S3M_ARPEGGIO: /* S3M Jxy: Arpeggio */
      if (CHANNEL_IS_S3M(ch) && s->effect_param > 0)
        ch->arpeggio_param = s->effect_param;
      break;

    case 1: /* 1xx: Portamento up */
    case XM_EFFECT_S3M_PORTAMENTO_UP: /* S3M Fxx: Portamento up */
      if (s->effect_param > 0)
        ch->portamento_up_param = s->effect_param;
      break;

    case 2: /* 2xx: Portamento down */
    case XM_EFFECT_S3M_PORTAMENTO_DOWN: /* S3M Exx: Portamento down */
      if (s->effect_param > 0)
        ch->portamento_down_param = s->effect_param;
      break;

    case 3: /* 3xx: Tone portamento */
    case XM_EFFECT_S3M_TONE_PORTAMENTO: /* S3M Gxx: Tone portamento */
      if (s->effect_param > 0)
        ch->tone_portamento_param = s->effect_param;
      break;

    case 4: /* 4xy: Vibrato */
    case XM_EFFECT_S3M_VIBRATO: /* S3M Hxy: Vibrato */
      if (s->effect_param & 0x0F) /* Set vibrato depth */
        ch->vibrato_param = (ch->vibrato_param & 0xF0) | (s->effect_param & 0x0F);

      if (s->effect_param >> 4)  /* Set vibrato speed */
        ch->vibrato_param = (s->effect_param & 0xF0) | (ch->vibrato_param & 0x0F);
      break;

    case 5: /* 5xy: Tone portamento + Volume slide */
    case XM_EFFECT_S3M_TONE_PORTAMENTO_VOLUME_SLIDE: /* S3M Lxy: Tone portamento + volume slide */
      if (s->effect_param > 0)
        ch->volume_slide_param = s->effect_param;
      break;

    case 6: /* 6xy: Vibrato + Volume slide */
    case XM_EFFECT_S3M_VIBRATO_VOLUME_SLIDE: /* S3M Kxy: Vibrato + volume slide */
      if (s->effect_param > 0)
        ch->volume_slide_param = s->effect_param;
      break;

    case 7: /* 7xy: Tremolo */
    case XM_EFFECT_S3M_TREMOLO: /* S3M Rxy: Tremolo */
      if (s->effect_param & 0x0F)  /* Set tremolo depth */
        ch->tremolo_param = (ch->tremolo_param & 0xF0) | (s->effect_param & 0x0F);

      if (s->effect_param >> 4)  /* Set tremolo speed */
        ch->tremolo_param = (s->effect_param & 0xF0) | (ch->tremolo_param & 0x0F);
      break;

    case 8: /* 8xx: Set panning */
      ch->panning = (float)s->effect_param / (float)0xFF;
      break;

    case XM_EFFECT_S3M_PANNING: /* S3M Xxx: Set panning */
      if (s->effect_param <= 0x80)
      {
        ch->surround = false;
        ch->panning = (float)s->effect_param / 128.f;
      }
      else if (s->effect_param == 0xA4)
      {
        ch->surround = true;
        ch->panning = .5f;
      }
      break;

    case 9: /* 9xx: Sample offset */
    case XM_EFFECT_S3M_SAMPLE_OFFSET: /* S3M Oxx: Sample offset */
      if (ch->sample != NULL && SLOT_HAS_NOTE(s))
      {
        uint8_t sample_offset_param = s->effect_param;
        uint32_t final_offset;

        if (sample_offset_param)
          ch->sample_offset_param = sample_offset_param;
        else
          sample_offset_param = ch->sample_offset_param;

        final_offset = sample_offset_param << (ch->sample->bits == 16 ? 7 : 8);
        if (CHANNEL_IS_S3M(ch))
          final_offset += (uint32_t)ch->s3m_high_sample_offset_param << (ch->sample->bits == 16 ? 15 : 16);

        if (final_offset >= ch->sample->length ||
            (CHANNEL_IS_S3M(ch) && s->period != 0 && ch->sample->loop_type != XM_NO_LOOP && final_offset >= ch->sample->loop_end))
        {
          if (CHANNEL_IS_S3M(ch) && s->period != 0 && ch->sample->loop_type != XM_NO_LOOP)
            ch->sample_position = ch->sample->loop_start;
          else
            ch->sample_position = -1;
          break;
        }

        ch->sample_position = final_offset;
      }
      break;

    case 0xA: /* Axy: Volume slide */
    case XM_EFFECT_S3M_VOLUME_SLIDE: /* S3M Dxy: Volume slide */
      if (s->effect_param > 0)
        ch->volume_slide_param = s->effect_param;
      break;

    case 0xB: /* Bxx: Position jump */
      if (s->effect_param < ctx->module.length)
      {
        ctx->position_jump = true;
        ctx->pattern_break = false;
        ctx->jump_dest     = s->effect_param;
        ctx->jump_row      = 0;
      }
      break;

    case XM_EFFECT_S3M_POSITION_JUMP: /* S3M Bxx: handled once per row in xm_row() */
      break;

    case 0xC: /* Cxx: Set volume */
      ch->volume = (float)((s->effect_param > 0x40)
                                                         ? 0x40 : s->effect_param) / (float)0x40;
      break;

    case 0xD: /* Dxx: Pattern break */
      /* Jump after playing this line */
      ctx->pattern_break = true;
      ctx->jump_row      = (s->effect_param >> 4) * 10 + (s->effect_param & 0x0F);
      break;

    case XM_EFFECT_S3M_PATTERN_BREAK: /* S3M Cxx: handled once per row in xm_row() */
      break;

    case 0xE: /* EXy: Extended command */
      switch (s->effect_param >> 4)
      {
        case 1: /* E1y: Fine portamento up */
          if ((s->effect_param & 0x0F) == 0 && CHANNEL_IS_MOD(ch))
            break;

          if (s->effect_param & 0x0F)
            ch->fine_portamento_up_param = s->effect_param & 0x0F;

          xm_pitch_slide(ctx, ch, -ch->fine_portamento_up_param);
          break;

        case 2: /* E2y: Fine portamento down */
          if ((s->effect_param & 0x0F) == 0 && CHANNEL_IS_MOD(ch))
            break;

          if (s->effect_param & 0x0F)
            ch->fine_portamento_down_param = s->effect_param & 0x0F;

          xm_pitch_slide(ctx, ch, ch->fine_portamento_down_param);
          break;

        case 3: /* E3y: Glissando control. VDAC4: у XM и MOD тоже (FT2, ProTracker) */
          ch->s3m_glissando_control = (s->effect_param & 0x0F) != 0;
          break;

        case 4: /* E4y: Set vibrato control */
          ch->vibrato_waveform = (xm_waveform_type_t)(s->effect_param & 3);
          ch->vibrato_waveform_retrigger = !((s->effect_param >> 2) & 1);
          break;

        case 5: /* E5y: Set finetune */
          if (ch->sample != NULL)
          {
            uint8_t finetune = s->effect_param & 0x0F;
            int8_t signed_finetune = finetune < 8 ? finetune : finetune - 16;

            if (ch->current->period != 0)
            {
              ch->period_note_offset = (float)(signed_finetune * 16) / 128.f;
              xm_update_frequency(ctx, ch);
            }
            else if (NOTE_IS_VALID(ch->current->note))
            {
              ch->note = ch->current->note + ch->sample->relative_note + (float)(((s->effect_param & 0x0F) - 8) << 4) / 128.f - 1.f;
              ch->period = xm_period(ctx, ch->note);
              xm_update_frequency(ctx, ch);
            }
          }
          break;

        case 6: /* E6y: Pattern loop */
          if (s->effect_param & 0x0F)
          {
            if ((s->effect_param & 0x0F) == ch->pattern_loop_count)
            {
              /* Loop is over */
              ch->pattern_loop_count = 0;
              break;
            }

            /* Jump to the beginning of the loop */
            ch->pattern_loop_count++;
            ctx->position_jump = true;
            ctx->jump_row      = ch->pattern_loop_origin;
            ctx->jump_dest     = ctx->current_table_index;
          }
          else
          {
            /* Set loop start point */
            ch->pattern_loop_origin = ctx->current_row;
            /* Replicate FT2 E60 bug */
            ctx->jump_row = ch->pattern_loop_origin;
          }
          break;

        case 7: /* E7y: Set tremolo control */
          ch->tremolo_waveform           = (xm_waveform_type_t)(s->effect_param & 3);
          ch->tremolo_waveform_retrigger = !((s->effect_param >> 2) & 1);
          break;

        case 8: /* E8y: Set 4-bit panning */
          if (CHANNEL_IS_MOD(ch))
            ch->panning = (float)(s->effect_param & 0x0F) / (float)0x0F;
          break;

        case 0xA: /* EAy: Fine volume slide up */
          if ((s->effect_param & 0x0F) == 0 && CHANNEL_IS_MOD(ch))
            break;

          if (s->effect_param & 0x0F)
            ch->fine_volume_slide_param = s->effect_param & 0x0F;

          xm_volume_slide(ctx, ch, ch->fine_volume_slide_param << 4);
          break;

        case 0xB: /* EBy: Fine volume slide down */
          if ((s->effect_param & 0x0F) == 0 && CHANNEL_IS_MOD(ch))
            break;

          if (s->effect_param & 0x0F)
            ch->fine_volume_slide_param = s->effect_param & 0x0F;

          xm_volume_slide(ctx, ch, ch->fine_volume_slide_param);
          break;

        case 0xD: /* EDy: Note delay */
          /* XXX: figure this out better. EDx triggers
           * the note even when there no note and no
           * instrument. But ED0 acts like a ghost
           * note, EDx (x ≠ 0) does not. */
          if (s->note == 0 && s->instrument == 0)
          {
            unsigned int flags = XM_TRIGGER_KEEP_VOLUME;

            if (ch->current->effect_param & 0x0F)
            {
              ch->note = ch->orig_note;
              xm_trigger_note(ctx, ch, flags);
            }
            else
            {
              xm_trigger_note(
                ctx, ch,
                flags
                | XM_TRIGGER_KEEP_PERIOD
                | XM_TRIGGER_KEEP_SAMPLE_POSITION
                );
            }
          }
          break;

        case 0xE: /* EEy: Pattern delay */
          ctx->extra_ticks = (ch->current->effect_param & 0x0F) * ctx->tempo;
          break;

        case 0xF: /* EFy: Invert loop (MOD only) */
          if (CHANNEL_IS_MOD(ch))
            ch->invert_loop_speed = ch->current->effect_param & 0x0F;
          break;

        default:
          break;
      }
      break;

    case XM_EFFECT_S3M_EXTENDED: /* S3M Sxy: Extended command */
      switch (s->effect_param >> 4)
      {
        case 0x1: /* S1x: Glissando control */
          ch->s3m_glissando_control = (s->effect_param & 0x0F) != 0;
          break;

        case 0x2: /* S2x: Set finetune */
          if (ch->sample != NULL)
          {
            uint8_t finetune = s->effect_param & 0x0F;
            int8_t signed_finetune = finetune < 8 ? finetune : finetune - 16;

            ch->period_note_offset = (float)signed_finetune / 8.f;
            xm_update_frequency(ctx, ch);
          }
          break;

        case 0x3: /* S3x: Set vibrato control */
          ch->vibrato_waveform = (xm_waveform_type_t)(s->effect_param & 3);
          ch->vibrato_waveform_retrigger = !((s->effect_param >> 2) & 1);
          break;

        case 0x4: /* S4x: Set tremolo control */
          ch->tremolo_waveform = (xm_waveform_type_t)(s->effect_param & 3);
          ch->tremolo_waveform_retrigger = !((s->effect_param >> 2) & 1);
          break;

        case 0x5: /* S5x: Set panbrello control */
          ch->panbrello_waveform = (xm_waveform_type_t)(s->effect_param & 3);
          ch->panbrello_waveform_retrigger = !((s->effect_param >> 2) & 1);
          break;

        case 0x6: /* S6x: Fine pattern delay, handled once per row in xm_row() */
          break;

        case 0x8: /* S8x: Set 4-bit panning */
          ch->surround = false;
          ch->panning = (float)(s->effect_param & 0x0F) / (float)0x0F;
          break;

        case 0x9: /* S9x: Sound control, only surround on/off is represented in the current mixer */
          if ((s->effect_param & 0x0F) == 0)
            ch->surround = false;
          else if ((s->effect_param & 0x0F) == 1)
          {
            ch->surround = true;
            ch->panning = .5f;
          }
          break;

        case 0xA: /* SAx: High sample offset */
          ch->s3m_high_sample_offset_param = s->effect_param & 0x0F;
          break;

        case 0xB: /* SBx: Pattern loop */
          if (s->effect_param & 0x0F)
          {
            if ((s->effect_param & 0x0F) == ch->pattern_loop_count)
            {
              ch->pattern_loop_count = 0;
              break;
            }

            ch->pattern_loop_count++;
            ctx->position_jump = true;
            ctx->jump_row      = ch->pattern_loop_origin;
            ctx->jump_dest     = ctx->current_table_index;
          }
          else
          {
            ch->pattern_loop_origin = ctx->current_row;
            ctx->jump_row = ch->pattern_loop_origin;
          }
          break;

        case 0xD: /* SDx: Note delay, delayed note is triggered from xm_tick() */
          break;

        case 0xE: /* SEx: Pattern delay, handled once per row in xm_row() */
          break;

        default:
          break;
      }
      break;

    case 0xF: /* Fxx: Set tempo/BPM */
      if (s->effect_param > 0)
      {
        if (s->effect_param <= 0x1F)
          ctx->tempo = s->effect_param;
        else
          ctx->bpm = s->effect_param;
      }
      break;

    case 16: /* Gxx: Set global volume */
    case XM_EFFECT_S3M_GLOBAL_VOLUME: /* S3M Vxx: Set global volume */
      ctx->global_volume = (float)((s->effect_param > 0x40)
                                                                         ? 0x40 : s->effect_param) / (float)0x40;
      break;

    case 17: /* Hxy: Global volume slide */
    case XM_EFFECT_S3M_GLOBAL_VOLUME_SLIDE: /* S3M Wxy: Global volume slide */
      if (s->effect_param > 0)
        ch->global_volume_slide_param = s->effect_param;
      break;

    case 21: /* Lxx: Set envelope position */
      ch->volume_envelope_frame_count  = s->effect_param;
      ch->panning_envelope_frame_count = s->effect_param;
      break;

    case 25: /* Pxy: Panning slide */
    case XM_EFFECT_S3M_PANNING_SLIDE: /* S3M Pxy: Panning slide */
      if (s->effect_param > 0)
        ch->panning_slide_param = s->effect_param;
      break;

    case XM_EFFECT_S3M_PANBRELLO: /* S3M Yxy: Panbrello */
      if (s->effect_param & 0x0F)
        ch->panbrello_param = (ch->panbrello_param & 0xF0) | (s->effect_param & 0x0F);
      if (s->effect_param >> 4)
        ch->panbrello_param = (s->effect_param & 0xF0) | (ch->panbrello_param & 0x0F);
      break;

    case 27: /* Rxy/Qxy: Multi retrig note */
    case XM_EFFECT_S3M_RETRIG: /* S3M Qxy: Retrigger */
      if (s->effect_param > 0)
      {
        if (CHANNEL_IS_S3M(ch))
          ch->multi_retrig_param = s->effect_param;
        else if ((s->effect_param >> 4) == 0)  /* Keep previous x value */
          ch->multi_retrig_param = (ch->multi_retrig_param & 0xF0) | (s->effect_param & 0x0F);
        else
          ch->multi_retrig_param = s->effect_param;
      }
      break;

    case 29: /* Txy: Tremor */
    case XM_EFFECT_S3M_TREMOR: /* S3M Ixy: Tremor */
      if (s->effect_param > 0)  /* Tremor x and y params do not appear to be separately kept in memory, unlike Rxy */
        ch->tremor_param = s->effect_param;
      break;

    case XM_EFFECT_S3M_NONE: /* S3M no-op with parameter: only updates shared memory */
      break;

    case XM_EFFECT_S3M_SPEED: /* S3M Axx: handled once per row in xm_row() */
      break;

    case XM_EFFECT_S3M_TEMPO: /* S3M Txx: handled once per row in xm_row() */
      break;

    case XM_EFFECT_S3M_FINE_VIBRATO: /* S3M Uxy: Fine vibrato */
      if (s->effect_param & 0x0F)
        ch->vibrato_param = (ch->vibrato_param & 0xF0) | (s->effect_param & 0x0F);
      if (s->effect_param >> 4)
        ch->vibrato_param = (s->effect_param & 0xF0) | (ch->vibrato_param & 0x0F);
      break;

    case XM_EFFECT_S3M_CHANNEL_VOLUME: /* S3M Mxx: Set channel volume */
      if (s->effect_param <= 0x40)
        ch->channel_volume = (float)s->effect_param / (float)0x40;
      break;

    case XM_EFFECT_S3M_MIDI_MACRO: /* Zxx. VDAC4: макросы по умолчанию IT и OpenMPT —
                                    Z00..Z7F срез фильтра, Z80..Z8F резонанс */
      if (s->effect_param < 0x80)
        ch->filter_cutoff = s->effect_param;
      else if (s->effect_param < 0x90)
        ch->filter_resonance = (uint8_t)((s->effect_param & 0x0F) * 8);
      xm_filter_update(ctx, ch);
      break;

    case XM_EFFECT_S3M_CHANNEL_VOLUME_SLIDE: /* S3M Nxy: Channel volume slide */
      if (s->effect_param > 0)
        ch->channel_volume_slide_param = s->effect_param;
      break;

    case 33: /* Xxy: Extra stuff */
      switch (s->effect_param >> 4)
      {
        case 1: /* X1y: Extra fine portamento up */
          if (s->effect_param & 0x0F)
            ch->extra_fine_portamento_up_param = s->effect_param & 0x0F;

          xm_pitch_slide(ctx, ch, -1.0f * ch->extra_fine_portamento_up_param);
          break;

        case 2: /* X2y: Extra fine portamento down */
          if (s->effect_param & 0x0F)
            ch->extra_fine_portamento_down_param = s->effect_param & 0x0F;

          xm_pitch_slide(ctx, ch, ch->extra_fine_portamento_down_param);
          break;

        case 5: /* X5y: Panbrello waveform (OpenMPT). VDAC4 */
          ch->panbrello_waveform = (xm_waveform_type_t)(s->effect_param & 3);
          ch->panbrello_waveform_retrigger = !((s->effect_param >> 2) & 1);
          break;

        default:
          break;
      }
      break;

    default:
      break;
  }
}

static void xm_trigger_note(xm_context_t* ctx, xm_channel_context_t* ch, unsigned int flags)
{
  if (!(flags & XM_TRIGGER_KEEP_SAMPLE_POSITION))
  {
    ch->sample_position = 0.f;
    ch->ping            = true;
  }

  if (ch->sample != NULL)
  {
    if (!(flags & XM_TRIGGER_KEEP_VOLUME))
      ch->volume = ch->sample->volume;

    if (CHANNEL_HAS_DEFAULT_PANNING(ch))
      ch->panning = ch->default_panning;
    else
      ch->panning = ch->sample->panning;
  }

  ch->sustained                   = true;
  ch->fadeout_volume              = ch->volume_envelope_volume = 1.0f;
  ch->panning_envelope_panning    = .5f;
  ch->volume_envelope_frame_count = ch->panning_envelope_frame_count = 0;
  ch->vibrato_note_offset         = 0.f;
  ch->vibrato_period_offset       = .0f;
  ch->tremolo_volume              = 0.f;
  ch->panbrello_panning_offset    = .0f;
  ch->panbrello_in_progress       = false;
  ch->s3m_note_cut              = false;
  ch->tremor_on                   = false;

  ch->autovibrato_ticks = 0;

  if (ch->vibrato_waveform_retrigger)
    ch->vibrato_ticks = 0;             /* XXX: should the waveform itself also be reset to sine? */

  if (ch->tremolo_waveform_retrigger)
    ch->tremolo_ticks = 0;

  if (ch->panbrello_waveform_retrigger)
    ch->panbrello_ticks = 0;

  if (!(flags & XM_TRIGGER_KEEP_PERIOD))
  {
    if (ch->current != NULL && ch->current->period != 0)
      ch->period = ch->current->period;
    else
      ch->period = xm_period(ctx, ch->note);
    xm_update_frequency(ctx, ch);
  }

  ch->s3m_opl_trigger_id++;
  ch->latest_trigger = ctx->generated_samples;
  if (ch->instrument != NULL)
    ch->instrument->latest_trigger = ctx->generated_samples;

  if (ch->sample != NULL)
    ch->sample->latest_trigger = ctx->generated_samples;
}

static void xm_cut_note(xm_channel_context_t* ch)
{
  /* NB: this is not the same as Key Off */
  ch->volume = .0f;
  if (CHANNEL_IS_S3M(ch))
  {
    ch->sample_position = -1.f;
    ch->s3m_note_cut = true;
  }
}

static void xm_key_off(xm_channel_context_t* ch)
{
  /* Key Off */
  ch->sustained = false;

  /* If no volume envelope is used, also cut the note */
  if (ch->instrument == NULL || !ch->instrument->volume_envelope.enabled)
    xm_cut_note(ch);
}

static void xm_row(xm_context_t* ctx)
{
  if (ctx->module.length == 0 || ctx->module.num_channels == 0 ||
      ctx->module.num_patterns == 0 || ctx->module.patterns == NULL ||
      ctx->channels == NULL || ctx->row_loop_count == NULL)
    return;

  if (ctx->position_jump)
  {
    ctx->current_table_index = ctx->jump_dest;
    ctx->current_row         = ctx->jump_row;
    ctx->position_jump       = false;
    ctx->pattern_break       = false;
    ctx->jump_row            = 0;
    xm_post_pattern_change(ctx);
  }
  else if (ctx->pattern_break)
  {
    ctx->current_table_index++;
    ctx->current_row   = ctx->jump_row;
    ctx->pattern_break = false;
    ctx->jump_row      = 0;
    xm_post_pattern_change(ctx);
  }

  if (ctx->current_table_index >= ctx->module.length)
    xm_post_pattern_change(ctx);

  if (ctx->current_table_index >= ctx->module.length)
    return;

  uint8_t pattern_index = ctx->module.pattern_table[ctx->current_table_index];

  if (pattern_index >= ctx->module.num_patterns)
  {
    ctx->current_table_index = 0;
    ctx->current_row = 0;
    return;
  }

  xm_pattern_t* cur = ctx->module.patterns + pattern_index;

  if (cur->slots == NULL || cur->num_rows == 0)
  {
    ctx->current_table_index++;
    ctx->current_row = 0;
    xm_post_pattern_change(ctx);
    return;
  }

  if (ctx->current_row >= cur->num_rows)
    ctx->current_row = 0;

  if (ctx->tracker_format == TRACKER_FORMAT_S3M)
  {
    bool s3m_position_jump = false;
    bool s3m_pattern_break = false;
    bool s3m_pattern_delay = false;
    uint8_t s3m_jump_dest = 0;
    uint8_t s3m_jump_row = 0;
    uint8_t s3m_pattern_delay_param = 0;
    uint8_t s3m_fine_pattern_delay_ticks = 0;

    ctx->s3m_tempo_slide_param = 0;

    for (uint8_t i = 0; i < ctx->module.num_channels; ++i)
    {
      xm_pattern_slot_t* s = cur->slots + ctx->current_row * ctx->module.num_channels + i;

      switch (s->effect_type)
      {
        case XM_EFFECT_S3M_SPEED: /* S3M Axx: Set speed */
          if (s->effect_param > 0)
            ctx->tempo = s->effect_param;
          break;

        case XM_EFFECT_S3M_POSITION_JUMP: /* S3M Bxx: Position jump */
          if (s->effect_param < ctx->module.length)
          {
            s3m_position_jump = true;
            s3m_jump_dest = s->effect_param;
          }
          break;

        case XM_EFFECT_S3M_PATTERN_BREAK: /* S3M Cxx: Pattern break */
        {
          uint8_t row = (s->effect_param >> 4) * 10 + (s->effect_param & 0x0F);
          if (row < 64)
          {
            s3m_pattern_break = true;
            s3m_jump_row = row;
          }
          break;
        }

        case XM_EFFECT_S3M_TEMPO: /* S3M Txx: Set tempo / tempo slide */
          if (s->effect_param >= 0x20)
            ctx->bpm = s->effect_param;
          else if (s->effect_param > 0)
            ctx->s3m_tempo_slide_param = s->effect_param;
          break;

        case XM_EFFECT_S3M_EXTENDED: /* S3M Sxy: Extended command */
        {
          uint8_t param = s->effect_param;
          if (!param)
          {
            xm_channel_context_t* ch = ctx->channels + i;
            param = ch->s3m_effect_memory;
          }

          switch (param >> 4)
          {
            case 0x6: /* S6x: Fine pattern delay */
              s3m_fine_pattern_delay_ticks += param & 0x0F;
              break;

            case 0xE: /* SEx: Pattern delay */
              if ((param & 0x0F) && !s3m_pattern_delay)
              {
                s3m_pattern_delay_param = param & 0x0F;
                s3m_pattern_delay = true;
              }
              break;

            default:
              break;
          }
          break;
        }

        default:
          break;
      }
    }

    if (s3m_pattern_delay)
      ctx->extra_ticks = s3m_pattern_delay_param * ctx->tempo;
    ctx->extra_ticks += s3m_fine_pattern_delay_ticks;

    if (s3m_position_jump)
    {
      ctx->position_jump = true;
      ctx->jump_dest = s3m_jump_dest;
      ctx->jump_row = s3m_pattern_break ? s3m_jump_row : 0;
      ctx->pattern_break = false;
    }
    else if (s3m_pattern_break)
    {
      ctx->pattern_break = true;
      ctx->jump_row = s3m_jump_row;
    }
  }

  bool in_a_loop = false;

  /* Read notes… */
  for (uint8_t i = 0; i < ctx->module.num_channels; ++i)
  {
    xm_pattern_slot_t   * s  = cur->slots + ctx->current_row * ctx->module.num_channels + i;
    xm_channel_context_t* ch = ctx->channels + i;

    ch->current = s;

    uint8_t effect_param = s->effect_param;
    if (s->effect_type == XM_EFFECT_S3M_EXTENDED)
    {
      if (effect_param)
        ch->s3m_effect_memory = effect_param;
      else
        effect_param = ch->s3m_effect_memory;
    }

    if (!((s->effect_type == 0xE && (s->effect_param >> 4) == 0xD) ||
          (s->effect_type == XM_EFFECT_S3M_EXTENDED && (effect_param >> 4) == 0xD)))
      xm_handle_note_and_instrument(ctx, ch, s);
    else if (CHANNEL_IS_S3M(ch))
    {
      uint8_t delay = effect_param & 0x0F;
      ch->note_delay_param = (delay > 0 && delay < ctx->tempo) ? delay : 0xFF;
    }
    else
      ch->note_delay_param = effect_param & 0x0F;

    if (!in_a_loop && ch->pattern_loop_count > 0)
      in_a_loop = true;
  }

  if (!in_a_loop)
  {
    /* No E6y loop is in effect (or we are in the first pass) */
    ctx->loop_count = (ctx->row_loop_count[MAX_NUM_ROWS * ctx->current_table_index + ctx->current_row]++);
  }

  ctx->current_row++;       /* Since this is an uint8, this line can increment from 255 to 0, in which case it is still necessary to go the next pattern. */

  if (!ctx->position_jump && !ctx->pattern_break &&
      (ctx->current_row >= cur->num_rows || ctx->current_row == 0))
  {
    ctx->current_table_index++;
    ctx->current_row = ctx->jump_row;             /* This will be 0 most of the time, except when E60 is used */
    ctx->jump_row    = 0;
    xm_post_pattern_change(ctx);
  }
}

static void xm_envelope_tick(xm_channel_context_t* ch, xm_envelope_t* env, uint16_t* counter, float* outval)
{
  if (env->num_points < 2)
  {
    /* Don't really know what to do… */
    if (env->num_points == 1)
    {
      /* XXX I am pulling this out of my ass */
      *outval = (float)env->points[0].value / (float)0x40;

      if (*outval > 1)
        *outval = 1;
    }

    return;
  }
  else
  {
    uint8_t j;

    if (env->loop_enabled)
    {
      uint16_t loop_start  = env->points[env->loop_start_point].frame;
      uint16_t loop_end    = env->points[env->loop_end_point].frame;
      uint16_t loop_length = loop_end - loop_start;

      if (*counter >= loop_end)
        *counter -= loop_length;
    }

    for (j = 0; j < (env->num_points - 2); ++j)
    {
      if (env->points[j].frame <= *counter && env->points[j + 1].frame >= *counter)
        break;
    }

    *outval = xm_envelope_lerp(env->points + j, env->points + j + 1, *counter) / (float)0x40;

    /* Make sure it is safe to increment frame count */
    if (!ch->sustained || !env->sustain_enabled || *counter != env->points[env->sustain_point].frame)
      (*counter)++;
  }
}

static void xm_envelopes(xm_channel_context_t* ch)
{
  if (ch->instrument != NULL)
  {
    if (ch->instrument->volume_envelope.enabled)
    {
      if (!ch->sustained)
      {
        ch->fadeout_volume -= (float)ch->instrument->volume_fadeout / 65536.f;
        XM_CLAMP_DOWN(ch->fadeout_volume);
      }

      xm_envelope_tick(ch,
                       &(ch->instrument->volume_envelope),
                       &(ch->volume_envelope_frame_count),
                       &(ch->volume_envelope_volume));
    }

    if (ch->instrument->panning_envelope.enabled)
    {
      xm_envelope_tick(ch,
                       &(ch->instrument->panning_envelope),
                       &(ch->panning_envelope_frame_count),
                       &(ch->panning_envelope_panning));
    }
  }
}

static void xm_tick(xm_context_t* ctx)
{
  if (ctx->current_tick == 0)
    xm_row(ctx);

  for (uint8_t i = 0; i < ctx->module.num_channels; ++i)
  {
    xm_channel_context_t* ch = ctx->channels + i;

    xm_envelopes(ch);
    xm_autovibrato(ctx, ch);

    if (ch->arp_in_progress && !HAS_ARPEGGIO(ch->current))
    {
      ch->arp_in_progress = false;
      ch->arp_note_offset = 0;
      xm_update_frequency(ctx, ch);
    }

    if (ch->vibrato_in_progress && !HAS_VIBRATO(ch->current))
    {
      ch->vibrato_in_progress = false;
      ch->vibrato_note_offset = 0.f;
      ch->vibrato_period_offset = .0f;
      xm_update_frequency(ctx, ch);
    }

    if (ch->panbrello_in_progress && ch->current->effect_type != XM_EFFECT_S3M_PANBRELLO)
    {
      ch->panbrello_in_progress = false;
      ch->panbrello_panning_offset = .0f;
    }

    if (!(CHANNEL_IS_S3M(ch) && ch->current->volume_column >= 128 && ch->current->volume_column <= 192))
    {
      switch (ch->current->volume_column >> 4)
      {
      case 0x6: /* Volume slide down */
        if (ctx->current_tick == 0)
          break;

        xm_volume_slide(ctx, ch, ch->current->volume_column & 0x0F);
        break;

      case 0x7: /* Volume slide up */
        if (ctx->current_tick == 0)
          break;

        xm_volume_slide(ctx, ch, ch->current->volume_column << 4);
        break;

      case 0xB: /* Vibrato */
        if (ctx->current_tick == 0)
          break;

        ch->vibrato_in_progress = false;
        xm_vibrato(ctx, ch, ch->vibrato_param, ch->vibrato_ticks++);
        break;

      case 0xD: /* Panning slide left */
        if (ctx->current_tick == 0)
          break;

        xm_panning_slide(ch, ch->current->volume_column & 0x0F);
        break;

      case 0xE: /* Panning slide right */
        if (ctx->current_tick == 0)
          break;

        xm_panning_slide(ch, ch->current->volume_column << 4);
        break;

      case 0xF: /* Tone portamento */
        if (ctx->current_tick == 0)
          break;

        xm_tone_portamento(ctx, ch);
        break;

        default:
          break;
      }
    }

    switch (ch->current->effect_type)
    {
      case 0: /* 0xy: XM/MOD arpeggio or no-op */
      case XM_EFFECT_S3M_ARPEGGIO: /* S3M Jxy: Arpeggio */
      {
        uint8_t param = ch->current->effect_param;
        if (ch->current->effect_type == 0 && CHANNEL_IS_S3M(ch))
          break;
        if (ch->current->effect_type == XM_EFFECT_S3M_ARPEGGIO && param == 0)
          param = ch->arpeggio_param;

        if (param > 0)
        {
          if (CHANNEL_USES_PERIOD_EFFECTS(ch))
          {
            xm_arpeggio(ctx, ch, param, ctx->current_tick);
            break;
          }

          char arp_offset = ctx->tempo % 3;

          switch (arp_offset)
          {
            case 2: /* 0 -> x -> 0 -> y -> x -> … */
              if (ctx->current_tick == 1)
              {
                ch->arp_in_progress = true;
                ch->arp_note_offset = param >> 4;
                xm_update_frequency(ctx, ch);
                break;
              }

            /* No break here, this is intended */
            case 1: /* 0 -> 0 -> y -> x -> … */
              if (ctx->current_tick == 0)
              {
                ch->arp_in_progress = false;
                ch->arp_note_offset = 0;
                xm_update_frequency(ctx, ch);
                break;
              }

            /* No break here, this is intended */
            case 0: /* 0 -> y -> x -> … */
              xm_arpeggio(ctx, ch, param, ctx->current_tick - arp_offset);

            default:
              break;
          }
        }
        break;
      }

      case 1: /* 1xx: Portamento up */
      case XM_EFFECT_S3M_PORTAMENTO_UP: /* S3M Fxx: Portamento up */
        if (CHANNEL_IS_S3M(ch))
        {
          uint8_t param = ch->portamento_up_param;
          if (!param) break;

          if (ctx->current_tick == 0)
          {
            if ((param & 0xF0) == 0xE0)
              xm_pitch_slide(ctx, ch, -0.25f * (float)(param & 0x0F));
            else if ((param & 0xF0) == 0xF0)
              xm_pitch_slide(ctx, ch, -(float)(param & 0x0F));
          }
          else if ((param & 0xF0) < 0xE0)
            xm_pitch_slide(ctx, ch, -param);
          break;
        }

        if (ctx->current_tick == 0 || (CHANNEL_IS_MOD(ch) && ch->current->effect_param == 0))
          break;

        xm_pitch_slide(ctx, ch, -ch->portamento_up_param);
        break;

      case 2: /* 2xx: Portamento down */
      case XM_EFFECT_S3M_PORTAMENTO_DOWN: /* S3M Exx: Portamento down */
        if (CHANNEL_IS_S3M(ch))
        {
          uint8_t param = ch->portamento_down_param;
          if (!param) break;

          if (ctx->current_tick == 0)
          {
            if ((param & 0xF0) == 0xE0)
              xm_pitch_slide(ctx, ch, 0.25f * (float)(param & 0x0F));
            else if ((param & 0xF0) == 0xF0)
              xm_pitch_slide(ctx, ch, (float)(param & 0x0F));
          }
          else if ((param & 0xF0) < 0xE0)
            xm_pitch_slide(ctx, ch, param);
          break;
        }

        if (ctx->current_tick == 0 || (CHANNEL_IS_MOD(ch) && ch->current->effect_param == 0))
          break;

        xm_pitch_slide(ctx, ch, ch->portamento_down_param);
        break;

      case 3: /* 3xx: Tone portamento */
      case XM_EFFECT_S3M_TONE_PORTAMENTO: /* S3M Gxx: Tone portamento */
        if (ctx->current_tick == 0)
          break;

        xm_tone_portamento(ctx, ch);
        break;

      case 4: /* 4xy: Vibrato */
      case XM_EFFECT_S3M_VIBRATO: /* S3M Hxy: Vibrato */
        if (ctx->current_tick == 0)
          break;

        ch->vibrato_in_progress = true;
        xm_vibrato(ctx, ch, ch->vibrato_param, ch->vibrato_ticks++);
        break;

      case 5: /* 5xy: Tone portamento + Volume slide */
      case XM_EFFECT_S3M_TONE_PORTAMENTO_VOLUME_SLIDE: /* S3M Lxy: Tone portamento + volume slide */
        if (ctx->current_tick == 0)
          break;

        xm_tone_portamento(ctx, ch);
        if (ch->current->effect_param != 0 || !CHANNEL_IS_MOD(ch))
          xm_volume_slide(ctx, ch, ch->volume_slide_param);
        break;

      case 6: /* 6xy: Vibrato + Volume slide */
      case XM_EFFECT_S3M_VIBRATO_VOLUME_SLIDE: /* S3M Kxy: Vibrato + volume slide */
        if (ctx->current_tick == 0)
          break;

        ch->vibrato_in_progress = true;
        xm_vibrato(ctx, ch, ch->vibrato_param, ch->vibrato_ticks++);
        if (ch->current->effect_param != 0 || !CHANNEL_IS_MOD(ch))
          xm_volume_slide(ctx, ch, ch->volume_slide_param);
        break;

      case 7: /* 7xy: Tremolo */
      case XM_EFFECT_S3M_TREMOLO: /* S3M Rxy: Tremolo */
        if (ctx->current_tick == 0)
          break;

        xm_tremolo(ctx, ch, ch->tremolo_param, ch->tremolo_ticks++);
        break;

      case 0xA: /* Axy: Volume slide */
      case XM_EFFECT_S3M_VOLUME_SLIDE: /* S3M Dxy: Volume slide */
        if (!CHANNEL_IS_S3M(ch) && ctx->current_tick == 0)
          break;
        if (ch->current->effect_param == 0 && CHANNEL_IS_MOD(ch))
          break;

        xm_volume_slide(ctx, ch, ch->volume_slide_param);
        break;

      case 0xE: /* EXy: Extended command */
        switch (ch->current->effect_param >> 4)
        {
          case 0x9: /* E9y: Retrigger note */
            if (ctx->current_tick != 0 && ch->current->effect_param & 0x0F)
            {
              if (!(ctx->current_tick % (ch->current->effect_param & 0x0F)))
              {
                xm_trigger_note(ctx, ch, 0);
                xm_envelopes(ch);
              }
            }
            break;

          case 0xC: /* ECy: Note cut */
            if ((ch->current->effect_param & 0x0F) == ctx->current_tick)
              xm_cut_note(ch);
            break;

          case 0xD: /* EDy: Note delay */
            if (ch->note_delay_param == ctx->current_tick)
            {
              xm_handle_note_and_instrument(ctx, ch, ch->current);
              xm_envelopes(ch);
            }
            break;

          default:
            break;
        }
        break;

      case XM_EFFECT_S3M_EXTENDED: /* S3M Sxy: Extended command */
      {
        uint8_t param = ch->current->effect_param;
        if (!param)
          param = ch->s3m_effect_memory;

        switch (param >> 4)
        {
          case 0xC: /* SCx: Note cut */
          {
            uint8_t tick = param & 0x0F;
            if (tick > 0 && tick < ctx->tempo && tick == ctx->current_tick)
              xm_cut_note(ch);
            break;
          }

          case 0xD: /* SDx: Note delay */
            if (ch->note_delay_param == ctx->current_tick)
            {
              xm_handle_note_and_instrument(ctx, ch, ch->current);
              xm_envelopes(ch);
            }
            break;

          default:
            break;
        }
        break;
      }

      case 17: /* Hxy: Global volume slide */
      case XM_EFFECT_S3M_GLOBAL_VOLUME_SLIDE: /* S3M Wxy: Global volume slide */
      {
        uint8_t param = ch->global_volume_slide_param;
        if (CHANNEL_IS_S3M(ch))
        {
          if ((param & 0x0F) == 0x0F && (param & 0xF0))
          {
            if (ctx->current_tick == 0)
            {
              float f = (float)(param >> 4) / (float)0x40;
              ctx->global_volume += f;
              XM_CLAMP_UP(ctx->global_volume);
            }
            break;
          }

          if ((param & 0xF0) == 0xF0 && (param & 0x0F))
          {
            if (ctx->current_tick == 0)
            {
              float f = (float)(param & 0x0F) / (float)0x40;
              ctx->global_volume -= f;
              XM_CLAMP_DOWN(ctx->global_volume);
            }
            break;
          }

          if (ctx->current_tick == 0 && !ctx->s3m_fast_volume_slides)
            break;

          if (param & 0x0F)
          {
            float f = (float)(param & 0x0F) / (float)0x40;
            ctx->global_volume -= f;
            XM_CLAMP_DOWN(ctx->global_volume);
          }
          else
          {
            float f = (float)(param >> 4) / (float)0x40;
            ctx->global_volume += f;
            XM_CLAMP_UP(ctx->global_volume);
          }
          break;
        }

        if (ctx->current_tick == 0)
          break;

        if ((param & 0xF0) && (param & 0x0F))  /* Illegal state */
          break;

        if (param & 0xF0)
        {
          /* Global slide up */
          float f = (float)(param >> 4) / (float)0x40;
          ctx->global_volume += f;
          XM_CLAMP_UP(ctx->global_volume);
        }
        else
        {
          /* Global slide down */
          float f = (float)(param & 0x0F) / (float)0x40;
          ctx->global_volume -= f;
          XM_CLAMP_DOWN(ctx->global_volume);
        }
        break;
      }

      case 20: /* Kxx: Key off */
        /* Most documentations will tell you the parameter has no
         * use. Don't be fooled. */
        if (ctx->current_tick == ch->current->effect_param)
          xm_key_off(ch);
        break;

      case 25: /* Pxy: Panning slide */
        if (ctx->current_tick == 0)
          break;

        xm_panning_slide(ch, ch->panning_slide_param);
        break;

      case XM_EFFECT_S3M_PANNING_SLIDE: /* S3M Pxy: Panning slide */
      {
        uint8_t param = ch->panning_slide_param;

        if (!param)
          break;

        if ((param & 0x0F) == 0x0F && (param & 0xF0))
        {
          if (ctx->current_tick == 0)
          {
            ch->panning -= (float)(param >> 4) / 64.f;
            XM_CLAMP_DOWN(ch->panning);
          }
          break;
        }

        if ((param & 0xF0) == 0xF0 && (param & 0x0F))
        {
          if (ctx->current_tick == 0)
          {
            ch->panning += (float)(param & 0x0F) / 64.f;
            XM_CLAMP_UP(ch->panning);
          }
          break;
        }

        if (ctx->current_tick == 0 || ((param & 0xF0) && (param & 0x0F)))
          break;

        if (param & 0x0F)
        {
          ch->panning += (float)(param & 0x0F) / 64.f;
          XM_CLAMP_UP(ch->panning);
        }
        else
        {
          ch->panning -= (float)(param >> 4) / 64.f;
          XM_CLAMP_DOWN(ch->panning);
        }
        break;
      }

      case XM_EFFECT_S3M_PANBRELLO: /* S3M Yxy: Panbrello */
        if (ctx->current_tick == 0)
          break;

        ch->panbrello_in_progress = true;
        xm_panbrello(ch, ch->panbrello_param, ch->panbrello_ticks++);
        break;

      case 27: /* Rxy: Multi retrig note */
      case XM_EFFECT_S3M_RETRIG: /* S3M Qxy: Retrigger */
      {
        uint8_t retrig_ticks;

        if (ctx->current_tick == 0)
          break;

        if (CHANNEL_IS_S3M(ch) && ch->s3m_note_cut)
          break;

        retrig_ticks = ch->multi_retrig_param & 0x0F;
        if (!retrig_ticks)
        {
          if (!CHANNEL_IS_S3M(ch))
            break;
          retrig_ticks = 1;
        }

        if ((ctx->current_tick % retrig_ticks) == 0)
        {
          float v = ch->volume * multi_retrig_multiply[ch->multi_retrig_param >> 4] + multi_retrig_add[ch->multi_retrig_param >> 4];
          XM_CLAMP(v);
          xm_trigger_note(ctx, ch, 0);
          ch->volume = v;
        }
        break;
      }

      case 29: /* Txy: Tremor */
      case XM_EFFECT_S3M_TREMOR: /* S3M Ixy: Tremor */
        if (ctx->current_tick == 0)
          break;

        if (CHANNEL_IS_S3M(ch))
        {
          uint8_t tremor_on_ticks = ch->tremor_param >> 4;
          uint8_t tremor_off_ticks = ch->tremor_param & 0x0F;
          uint8_t tremor_cycle = tremor_on_ticks + tremor_off_ticks;

          if (tremor_cycle)
            ch->tremor_on = ((ctx->current_tick - 1) % tremor_cycle) >= tremor_on_ticks;
        }
        else
        {
          ch->tremor_on = (
            (ctx->current_tick - 1) % ((ch->tremor_param >> 4) + (ch->tremor_param & 0x0F) + 2)
            >
            (ch->tremor_param >> 4)
            );
        }
        break;

      case XM_EFFECT_S3M_FINE_VIBRATO: /* S3M Uxy: Fine vibrato */
        if (ctx->current_tick == 0)
          break;

        ch->vibrato_in_progress = true;
        ch->vibrato_note_offset = .0f;
        ch->vibrato_period_offset = -xm_waveform(ch->vibrato_waveform, ch->vibrato_ticks++ * (ch->vibrato_param >> 4)) * (float)(ch->vibrato_param & 0x0F) / 2.f;
        xm_update_frequency(ctx, ch);
        break;

      case XM_EFFECT_S3M_CHANNEL_VOLUME_SLIDE: /* S3M Nxy: Channel volume slide */
      {
        uint8_t param = ch->channel_volume_slide_param;
        if (!param)
          break;

        if ((param & 0x0F) == 0x0F && (param & 0xF0))
        {
          if (ctx->current_tick == 0)
          {
            ch->channel_volume += (float)(param >> 4) / (float)0x40;
            XM_CLAMP_UP(ch->channel_volume);
          }
          break;
        }

        if ((param & 0xF0) == 0xF0 && (param & 0x0F))
        {
          if (ctx->current_tick == 0)
          {
            ch->channel_volume -= (float)(param & 0x0F) / (float)0x40;
            XM_CLAMP_DOWN(ch->channel_volume);
          }
          break;
        }

        if (ctx->current_tick == 0 && !ctx->s3m_fast_volume_slides)
          break;

        if (param & 0x0F)
        {
          ch->channel_volume -= (float)(param & 0x0F) / (float)0x40;
          XM_CLAMP_DOWN(ch->channel_volume);
        }
        else
        {
          ch->channel_volume += (float)(param >> 4) / (float)0x40;
          XM_CLAMP_UP(ch->channel_volume);
        }
        break;
      }

      default:
        break;
    }

    if (CHANNEL_IS_MOD(ch) && ch->invert_loop_speed &&
        (ctx->current_tick != 0 ||
         (ch->current->effect_type == 0xE && (ch->current->effect_param >> 4) == 0xF)))
      xm_invert_loop(ch);

    float panning, volume;

    panning = ch->panning + ch->panbrello_panning_offset;
    XM_CLAMP(panning);
    panning += (ch->panning_envelope_panning - .5f) * (.5f - fabsf(panning - .5f)) * 2.0f;
    XM_CLAMP(panning);

    if (ch->tremor_on)
      volume = .0f;
    else
    {
      volume = ch->volume + ch->tremolo_volume;
      XM_CLAMP(volume);
      volume *= ch->fadeout_volume * ch->volume_envelope_volume * ch->channel_volume;
    }

#if XM_RAMPING
    ch->target_panning = panning;
    ch->target_volume  = volume;
#else
    ch->actual_panning = panning;
    ch->actual_volume  = volume;
#endif

    s3m_adlib_sync_channel(ctx, ch);
  }

  if (ctx->tracker_format == TRACKER_FORMAT_S3M && ctx->current_tick > 0)
  {
    uint8_t param = ctx->s3m_tempo_slide_param;
    if ((param & 0xF0) == 0x00 && (param & 0x0F))
    {
      if (ctx->bpm > (param & 0x0F))
        ctx->bpm -= param & 0x0F;
      else
        ctx->bpm = 1;
    }
    else if ((param & 0xF0) == 0x10)
      ctx->bpm += param & 0x0F;
  }

  ctx->current_tick++;
  if (ctx->current_tick >= ctx->tempo + ctx->extra_ticks)
  {
    ctx->current_tick = 0;
    ctx->extra_ticks  = 0;
  }

  /* FT2 manual says number of ticks / second = BPM * 0.4 */
  ctx->remaining_samples_in_tick += (float)ctx->rate / ((float)ctx->bpm * 0.4f);
}

static float IRAM_ATTR xm_sample_at(xm_sample_t* sample, size_t k)
{
  return sample->bits == 8 ? (sample->data8[k] / 128.f) : (sample->data16[k] / 32768.f);
}

static float IRAM_ATTR xm_next_of_sample(xm_channel_context_t* ch)
{
  if (ch->instrument == NULL || ch->sample == NULL || ch->sample_position < 0)
  {
#if XM_RAMPING
    if (ch->frame_count < XM_SAMPLE_RAMPING_POINTS)
    {
      return XM_LERP(ch->end_of_previous_sample[ch->frame_count], .0f,
                     (float)ch->frame_count / (float)XM_SAMPLE_RAMPING_POINTS);
    }
#endif

    return .0f;
  }

  if (ch->sample->length == 0)
    return .0f;

  float    u, v, t;
  uint32_t a, b;
  a = (uint32_t)ch->sample_position;       /* This cast is fine, sample_position will not go above integer ranges */

  if (XM_LINEAR_INTERPOLATION)
  {
    b = a + 1;
    t = ch->sample_position - a; /* Cheaper than fmodf(., 1.f) */
  }

  u = xm_sample_at(ch->sample, a);

  switch (ch->sample->loop_type)
  {
    case XM_NO_LOOP:
      if (XM_LINEAR_INTERPOLATION)
        v = (b < ch->sample->length) ? xm_sample_at(ch->sample, b) : .0f;

      ch->sample_position += ch->step;

      if (ch->sample_position >= ch->sample->length)
        ch->sample_position = -1;

      break;

    case XM_FORWARD_LOOP:
      if (XM_LINEAR_INTERPOLATION)
      {
        v = xm_sample_at(ch->sample, (b == ch->sample->loop_end) ? ch->sample->loop_start : b);
      }

      ch->sample_position += ch->step;

      while (ch->sample_position >= ch->sample->loop_end)
        ch->sample_position -= ch->sample->loop_length;

      break;

    case XM_PING_PONG_LOOP:
      if (ch->ping)
        ch->sample_position += ch->step;
      else
        ch->sample_position -= ch->step;

      /* XXX: this may not work for very tight ping-pong loops (ie switches direction more than once per sample) */
      if (ch->ping)
      {
        if (XM_LINEAR_INTERPOLATION)
          v = xm_sample_at(ch->sample, (b >= ch->sample->loop_end) ? a : b);

        if (ch->sample_position >= ch->sample->loop_end)
        {
          ch->ping            = false;
          ch->sample_position = (ch->sample->loop_end << 1) - ch->sample_position;
        }

        /* sanity checking */
        if (ch->sample_position >= ch->sample->length)
        {
          ch->ping             = false;
          ch->sample_position -= ch->sample->length - 1;
        }
      }
      else
      {
        if (XM_LINEAR_INTERPOLATION)
        {
          v = u;
          u = xm_sample_at(ch->sample, (b == 1 || b - 2 <= ch->sample->loop_start) ? a : (b - 2));
        }

        if (ch->sample_position <= ch->sample->loop_start)
        {
          ch->ping            = true;
          ch->sample_position = (ch->sample->loop_start << 1) - ch->sample_position;
        }

        /* sanity checking */
        if (ch->sample_position <= .0f)
        {
          ch->ping            = true;
          ch->sample_position = .0f;
        }
      }
      break;

    default:
      v = .0f;
      break;
  }

  float endval = (XM_LINEAR_INTERPOLATION ? XM_LERP(u, v, t) : u);

#if XM_RAMPING
  if (ch->frame_count < XM_SAMPLE_RAMPING_POINTS)
  {
    /* Smoothly transition between old and new sample. */
    return XM_LERP(ch->end_of_previous_sample[ch->frame_count], endval,
                   (float)ch->frame_count / (float)XM_SAMPLE_RAMPING_POINTS);
  }
#endif

  return endval;
}
size_t IRAM_ATTR xm_pingpong_forward_subrun(float pos, float step, float limit, size_t count)
{
  if (count == 0) return 0;
  if (step <= 0.f) return count;

  size_t run = 0;
  while (run < count)
  {
    run++;
    pos += step;
    if (pos >= limit)
      break;
  }

  return run;
}

size_t IRAM_ATTR xm_pingpong_backward_subrun(float pos, float step, float limit, size_t count)
{
  if (count == 0) return 0;
  if (step <= 0.f) return count;

  size_t run = 0;
  while (run < count)
  {
    run++;
    pos -= step;
    if (pos <= limit)
      break;
  }

  return run;
}

void IRAM_ATTR xm_advance_no_loop(xm_channel_context_t* ch, size_t count)
{
  xm_sample_t* sample = ch->sample;
  const uint32_t length = sample->length;
  float pos = ch->sample_position;
  const float step = ch->step;

  for (size_t i = 0; i < count; i++)
  {
    pos += step;
    if (pos >= length)
    {
      pos = -1;
      break;
    }
  }

  ch->sample_position = pos;
}

void IRAM_ATTR xm_advance_forward_loop(xm_channel_context_t* ch, size_t count)
{
  xm_sample_t* sample = ch->sample;
  const uint32_t loop_end = sample->loop_end;
  const uint32_t loop_length = sample->loop_length;
  float pos = ch->sample_position;
  const float step = ch->step;

  for (size_t i = 0; i < count; i++)
  {
    pos += step;
    while (pos >= loop_end)
      pos -= loop_length;
  }

  ch->sample_position = pos;
}

void IRAM_ATTR xm_advance_pingpong_loop(xm_channel_context_t* ch, size_t count)
{
  xm_sample_t* sample = ch->sample;
  const uint32_t length = sample->length;
  const uint32_t loop_start = sample->loop_start;
  const uint32_t loop_end = sample->loop_end;
  float pos = ch->sample_position;
  const float step = ch->step;
  bool ping = ch->ping;
  size_t done = 0;

  while (done < count)
  {
    if (ping)
    {
      size_t run = xm_pingpong_forward_subrun(pos, step, loop_end, count - done);
      for (size_t i = 0; i < run; i++)
        pos += step;

      if (pos >= loop_end)
      {
        ping = false;
        pos  = (loop_end << 1) - pos;
      }

      if (pos >= length)
      {
        ping = false;
        pos -= length - 1;
      }

      done += run;
    }
    else
    {
      size_t run = xm_pingpong_backward_subrun(pos, step, loop_start, count - done);
      for (size_t i = 0; i < run; i++)
        pos -= step;

      if (pos <= loop_start)
      {
        ping = true;
        pos  = (loop_start << 1) - pos;
      }

      if (pos <= .0f)
      {
        ping = true;
        pos  = .0f;
      }

      done += run;
    }
  }

  ch->ping = ping;
  ch->sample_position = pos;
}
void IRAM_ATTR xm_advance_gain_ramp_no_loop(xm_channel_context_t* ch, size_t count, float volume_ramp, float panning_ramp)
{
  xm_sample_t* sample = ch->sample;
  const uint32_t length = sample->length;
  float pos = ch->sample_position;
  const float step = ch->step;
  float actual_volume = ch->actual_volume;
  float actual_panning = ch->actual_panning;
  const float target_volume = ch->target_volume;
  const float target_panning = ch->target_panning;
  unsigned long frame_count = ch->frame_count;

  for (size_t i = 0; i < count; i++)
  {
    pos += step;
    frame_count++;
    XM_SLIDE_TOWARDS(actual_volume, target_volume, volume_ramp);
    XM_SLIDE_TOWARDS(actual_panning, target_panning, panning_ramp);

    if (pos >= length)
    {
      pos = -1;
      break;
    }
  }

  ch->sample_position = pos;
  ch->frame_count = frame_count;
  ch->actual_volume = actual_volume;
  ch->actual_panning = actual_panning;
}

void IRAM_ATTR xm_advance_gain_ramp_forward_loop(xm_channel_context_t* ch, size_t count, float volume_ramp, float panning_ramp)
{
  xm_sample_t* sample = ch->sample;
  const uint32_t loop_end = sample->loop_end;
  const uint32_t loop_length = sample->loop_length;
  float pos = ch->sample_position;
  const float step = ch->step;
  float actual_volume = ch->actual_volume;
  float actual_panning = ch->actual_panning;
  const float target_volume = ch->target_volume;
  const float target_panning = ch->target_panning;
  unsigned long frame_count = ch->frame_count;

  for (size_t i = 0; i < count; i++)
  {
    pos += step;
    while (pos >= loop_end)
      pos -= loop_length;

    frame_count++;
    XM_SLIDE_TOWARDS(actual_volume, target_volume, volume_ramp);
    XM_SLIDE_TOWARDS(actual_panning, target_panning, panning_ramp);
  }

  ch->sample_position = pos;
  ch->frame_count = frame_count;
  ch->actual_volume = actual_volume;
  ch->actual_panning = actual_panning;
}

void IRAM_ATTR xm_advance_gain_ramp_pingpong_loop(xm_channel_context_t* ch, size_t count, float volume_ramp, float panning_ramp)
{
  xm_sample_t* sample = ch->sample;
  const uint32_t length = sample->length;
  const uint32_t loop_start = sample->loop_start;
  const uint32_t loop_end = sample->loop_end;
  float pos = ch->sample_position;
  const float step = ch->step;
  bool ping = ch->ping;
  float actual_volume = ch->actual_volume;
  float actual_panning = ch->actual_panning;
  const float target_volume = ch->target_volume;
  const float target_panning = ch->target_panning;
  unsigned long frame_count = ch->frame_count;
  size_t done = 0;

  while (done < count)
  {
    if (ping)
    {
      size_t run = xm_pingpong_forward_subrun(pos, step, loop_end, count - done);
      for (size_t i = 0; i < run; i++)
      {
        pos += step;
        frame_count++;
        XM_SLIDE_TOWARDS(actual_volume, target_volume, volume_ramp);
        XM_SLIDE_TOWARDS(actual_panning, target_panning, panning_ramp);
      }

      if (pos >= loop_end)
      {
        ping = false;
        pos  = (loop_end << 1) - pos;
      }

      if (pos >= length)
      {
        ping = false;
        pos -= length - 1;
      }

      done += run;
    }
    else
    {
      size_t run = xm_pingpong_backward_subrun(pos, step, loop_start, count - done);
      for (size_t i = 0; i < run; i++)
      {
        pos -= step;
        frame_count++;
        XM_SLIDE_TOWARDS(actual_volume, target_volume, volume_ramp);
        XM_SLIDE_TOWARDS(actual_panning, target_panning, panning_ramp);
      }

      if (pos <= loop_start)
      {
        ping = true;
        pos  = (loop_start << 1) - pos;
      }

      if (pos <= .0f)
      {
        ping = true;
        pos  = .0f;
      }

      done += run;
    }
  }

  ch->ping = ping;
  ch->sample_position = pos;
  ch->frame_count = frame_count;
  ch->actual_volume = actual_volume;
  ch->actual_panning = actual_panning;
}

void IRAM_ATTR xm_render_no_loop_8(xm_channel_context_t* ch, float* out, size_t count)
{
  xm_sample_t* sample = ch->sample;
  const int8_t* data = sample->data8;
  const uint32_t length = sample->length;
  float pos = ch->sample_position;
  const float step = ch->step;
  const float actual_volume = ch->actual_volume;
  const float actual_panning = ch->actual_panning;
  const float surround_sign = ch->surround ? -1.f : 1.f;

  for (size_t i = 0; i < count; i++)
  {
    uint32_t a = (uint32_t)pos;
    uint32_t b = a + 1;
    float t = pos - a;
    float u = data[a] / 128.f;
    float v = (b < length) ? data[b] / 128.f : .0f;
    float fval = XM_LERP(u, v, t);

    out[0] += fval * actual_volume * (1.f - actual_panning);
    out[1] += fval * actual_volume * actual_panning * surround_sign;
    out += 2;

    pos += step;

    if (pos >= length)
    {
      pos = -1;
      break;
    }
  }

  ch->sample_position = pos;
}

void IRAM_ATTR xm_render_no_loop_16(xm_channel_context_t* ch, float* out, size_t count)
{
  xm_sample_t* sample = ch->sample;
  const int16_t* data = sample->data16;
  const uint32_t length = sample->length;
  float pos = ch->sample_position;
  const float step = ch->step;
  const float actual_volume = ch->actual_volume;
  const float actual_panning = ch->actual_panning;
  const float surround_sign = ch->surround ? -1.f : 1.f;

  for (size_t i = 0; i < count; i++)
  {
    uint32_t a = (uint32_t)pos;
    uint32_t b = a + 1;
    float t = pos - a;
    float u = data[a] / 32768.f;
    float v = (b < length) ? data[b] / 32768.f : .0f;
    float fval = XM_LERP(u, v, t);

    out[0] += fval * actual_volume * (1.f - actual_panning);
    out[1] += fval * actual_volume * actual_panning * surround_sign;
    out += 2;

    pos += step;

    if (pos >= length)
    {
      pos = -1;
      break;
    }
  }

  ch->sample_position = pos;
}

void IRAM_ATTR xm_render_forward_loop_8(xm_channel_context_t* ch, float* out, size_t count)
{
  xm_sample_t* sample = ch->sample;
  const int8_t* data = sample->data8;
  const uint32_t loop_start = sample->loop_start;
  const uint32_t loop_end = sample->loop_end;
  const uint32_t loop_length = sample->loop_length;
  float pos = ch->sample_position;
  const float step = ch->step;
  const float actual_volume = ch->actual_volume;
  const float actual_panning = ch->actual_panning;
  const float surround_sign = ch->surround ? -1.f : 1.f;

  for (size_t i = 0; i < count; i++)
  {
    uint32_t a = (uint32_t)pos;
    uint32_t b = a + 1;
    float t = pos - a;
    float u = data[a] / 128.f;
    float v = data[(b == loop_end) ? loop_start : b] / 128.f;
    float fval = XM_LERP(u, v, t);

    out[0] += fval * actual_volume * (1.f - actual_panning);
    out[1] += fval * actual_volume * actual_panning * surround_sign;
    out += 2;

    pos += step;
    while (pos >= loop_end)
      pos -= loop_length;
  }

  ch->sample_position = pos;
}

void IRAM_ATTR xm_render_forward_loop_16(xm_channel_context_t* ch, float* out, size_t count)
{
  xm_sample_t* sample = ch->sample;
  const int16_t* data = sample->data16;
  const uint32_t loop_start = sample->loop_start;
  const uint32_t loop_end = sample->loop_end;
  const uint32_t loop_length = sample->loop_length;
  float pos = ch->sample_position;
  const float step = ch->step;
  const float actual_volume = ch->actual_volume;
  const float actual_panning = ch->actual_panning;
  const float surround_sign = ch->surround ? -1.f : 1.f;

  for (size_t i = 0; i < count; i++)
  {
    uint32_t a = (uint32_t)pos;
    uint32_t b = a + 1;
    float t = pos - a;
    float u = data[a] / 32768.f;
    float v = data[(b == loop_end) ? loop_start : b] / 32768.f;
    float fval = XM_LERP(u, v, t);

    out[0] += fval * actual_volume * (1.f - actual_panning);
    out[1] += fval * actual_volume * actual_panning * surround_sign;
    out += 2;

    pos += step;
    while (pos >= loop_end)
      pos -= loop_length;
  }

  ch->sample_position = pos;
}

void IRAM_ATTR xm_render_pingpong_loop_8(xm_channel_context_t* ch, float* out, size_t count)
{
  xm_sample_t* sample = ch->sample;
  const int8_t* data = sample->data8;
  const uint32_t length = sample->length;
  const uint32_t loop_start = sample->loop_start;
  const uint32_t loop_end = sample->loop_end;
  float pos = ch->sample_position;
  const float step = ch->step;
  bool ping = ch->ping;
  const float actual_volume = ch->actual_volume;
  const float actual_panning = ch->actual_panning;
  const float surround_sign = ch->surround ? -1.f : 1.f;

  size_t done = 0;
  while (done < count)
  {
    if (ping)
    {
      size_t run = xm_pingpong_forward_subrun(pos, step, loop_end, count - done);
      for (size_t i = 0; i < run; i++)
      {
        uint32_t a = (uint32_t)pos;
        uint32_t b = a + 1;
        float t = pos - a;
        float u = data[a] / 128.f;
        float v = data[(b >= loop_end) ? a : b] / 128.f;
        float fval = XM_LERP(u, v, t);

        out[0] += fval * actual_volume * (1.f - actual_panning);
        out[1] += fval * actual_volume * actual_panning * surround_sign;
        out += 2;
        pos += step;
      }

      if (pos >= loop_end)
      {
        ping = false;
        pos  = (loop_end << 1) - pos;
      }

      if (pos >= length)
      {
        ping = false;
        pos -= length - 1;
      }

      done += run;
    }
    else
    {
      size_t run = xm_pingpong_backward_subrun(pos, step, loop_start, count - done);
      for (size_t i = 0; i < run; i++)
      {
        uint32_t a = (uint32_t)pos;
        uint32_t b = a + 1;
        float t = pos - a;
        float v = data[a] / 128.f;
        float u = data[(b == 1 || b - 2 <= loop_start) ? a : (b - 2)] / 128.f;
        float fval = XM_LERP(u, v, t);

        out[0] += fval * actual_volume * (1.f - actual_panning);
        out[1] += fval * actual_volume * actual_panning * surround_sign;
        out += 2;
        pos -= step;
      }

      if (pos <= loop_start)
      {
        ping = true;
        pos  = (loop_start << 1) - pos;
      }

      if (pos <= .0f)
      {
        ping = true;
        pos  = .0f;
      }

      done += run;
    }
  }

  ch->ping = ping;
  ch->sample_position = pos;
}

void IRAM_ATTR xm_render_pingpong_loop_16(xm_channel_context_t* ch, float* out, size_t count)
{
  xm_sample_t* sample = ch->sample;
  const int16_t* data = sample->data16;
  const uint32_t length = sample->length;
  const uint32_t loop_start = sample->loop_start;
  const uint32_t loop_end = sample->loop_end;
  float pos = ch->sample_position;
  const float step = ch->step;
  bool ping = ch->ping;
  const float actual_volume = ch->actual_volume;
  const float actual_panning = ch->actual_panning;
  const float surround_sign = ch->surround ? -1.f : 1.f;

  size_t done = 0;
  while (done < count)
  {
    if (ping)
    {
      size_t run = xm_pingpong_forward_subrun(pos, step, loop_end, count - done);
      for (size_t i = 0; i < run; i++)
      {
        uint32_t a = (uint32_t)pos;
        uint32_t b = a + 1;
        float t = pos - a;
        float u = data[a] / 32768.f;
        float v = data[(b >= loop_end) ? a : b] / 32768.f;
        float fval = XM_LERP(u, v, t);

        out[0] += fval * actual_volume * (1.f - actual_panning);
        out[1] += fval * actual_volume * actual_panning * surround_sign;
        out += 2;
        pos += step;
      }

      if (pos >= loop_end)
      {
        ping = false;
        pos  = (loop_end << 1) - pos;
      }

      if (pos >= length)
      {
        ping = false;
        pos -= length - 1;
      }

      done += run;
    }
    else
    {
      size_t run = xm_pingpong_backward_subrun(pos, step, loop_start, count - done);
      for (size_t i = 0; i < run; i++)
      {
        uint32_t a = (uint32_t)pos;
        uint32_t b = a + 1;
        float t = pos - a;
        float v = data[a] / 32768.f;
        float u = data[(b == 1 || b - 2 <= loop_start) ? a : (b - 2)] / 32768.f;
        float fval = XM_LERP(u, v, t);

        out[0] += fval * actual_volume * (1.f - actual_panning);
        out[1] += fval * actual_volume * actual_panning * surround_sign;
        out += 2;
        pos -= step;
      }

      if (pos <= loop_start)
      {
        ping = true;
        pos  = (loop_start << 1) - pos;
      }

      if (pos <= .0f)
      {
        ping = true;
        pos  = .0f;
      }

      done += run;
    }
  }

  ch->ping = ping;
  ch->sample_position = pos;
}

void IRAM_ATTR xm_render_sample_ramp_no_loop_8(xm_channel_context_t* ch, float* out, size_t count, float volume_ramp, float panning_ramp)
{
  xm_sample_t* sample = ch->sample;
  const int8_t* data = sample->data8;
  const uint32_t length = sample->length;
  float pos = ch->sample_position;
  const float step = ch->step;
  float actual_volume = ch->actual_volume;
  float actual_panning = ch->actual_panning;
  const float surround_sign = ch->surround ? -1.f : 1.f;
  const float target_volume = ch->target_volume;
  const float target_panning = ch->target_panning;
  unsigned long frame_count = ch->frame_count;

  for (size_t i = 0; i < count; i++)
  {
    uint32_t a = (uint32_t)pos;
    uint32_t b = a + 1;
    float t = pos - a;
    float u = data[a] / 128.f;
    float v = (b < length) ? data[b] / 128.f : .0f;
    float fval = XM_LERP(u, v, t);
    fval = XM_LERP(ch->end_of_previous_sample[frame_count], fval,
                   (float)frame_count / (float)XM_SAMPLE_RAMPING_POINTS);

    out[0] += fval * actual_volume * (1.f - actual_panning);
    out[1] += fval * actual_volume * actual_panning * surround_sign;
    out += 2;

    pos += step;

    frame_count++;
    XM_SLIDE_TOWARDS(actual_volume, target_volume, volume_ramp);
    XM_SLIDE_TOWARDS(actual_panning, target_panning, panning_ramp);

    if (pos >= length)
    {
      pos = -1;
      break;
    }
  }

  ch->sample_position = pos;
  ch->frame_count = frame_count;
  ch->actual_volume = actual_volume;
  ch->actual_panning = actual_panning;
}

void IRAM_ATTR xm_render_sample_ramp_no_loop_16(xm_channel_context_t* ch, float* out, size_t count, float volume_ramp, float panning_ramp)
{
  xm_sample_t* sample = ch->sample;
  const int16_t* data = sample->data16;
  const uint32_t length = sample->length;
  float pos = ch->sample_position;
  const float step = ch->step;
  float actual_volume = ch->actual_volume;
  float actual_panning = ch->actual_panning;
  const float surround_sign = ch->surround ? -1.f : 1.f;
  const float target_volume = ch->target_volume;
  const float target_panning = ch->target_panning;
  unsigned long frame_count = ch->frame_count;

  for (size_t i = 0; i < count; i++)
  {
    uint32_t a = (uint32_t)pos;
    uint32_t b = a + 1;
    float t = pos - a;
    float u = data[a] / 32768.f;
    float v = (b < length) ? data[b] / 32768.f : .0f;
    float fval = XM_LERP(u, v, t);
    fval = XM_LERP(ch->end_of_previous_sample[frame_count], fval,
                   (float)frame_count / (float)XM_SAMPLE_RAMPING_POINTS);

    out[0] += fval * actual_volume * (1.f - actual_panning);
    out[1] += fval * actual_volume * actual_panning * surround_sign;
    out += 2;

    pos += step;

    frame_count++;
    XM_SLIDE_TOWARDS(actual_volume, target_volume, volume_ramp);
    XM_SLIDE_TOWARDS(actual_panning, target_panning, panning_ramp);

    if (pos >= length)
    {
      pos = -1;
      break;
    }
  }

  ch->sample_position = pos;
  ch->frame_count = frame_count;
  ch->actual_volume = actual_volume;
  ch->actual_panning = actual_panning;
}

void IRAM_ATTR xm_render_sample_ramp_forward_loop_8(xm_channel_context_t* ch, float* out, size_t count, float volume_ramp, float panning_ramp)
{
  xm_sample_t* sample = ch->sample;
  const int8_t* data = sample->data8;
  const uint32_t loop_start = sample->loop_start;
  const uint32_t loop_end = sample->loop_end;
  const uint32_t loop_length = sample->loop_length;
  float pos = ch->sample_position;
  const float step = ch->step;
  float actual_volume = ch->actual_volume;
  float actual_panning = ch->actual_panning;
  const float surround_sign = ch->surround ? -1.f : 1.f;
  const float target_volume = ch->target_volume;
  const float target_panning = ch->target_panning;
  unsigned long frame_count = ch->frame_count;

  for (size_t i = 0; i < count; i++)
  {
    uint32_t a = (uint32_t)pos;
    uint32_t b = a + 1;
    float t = pos - a;
    float u = data[a] / 128.f;
    float v = data[(b == loop_end) ? loop_start : b] / 128.f;
    float fval = XM_LERP(u, v, t);
    fval = XM_LERP(ch->end_of_previous_sample[frame_count], fval,
                   (float)frame_count / (float)XM_SAMPLE_RAMPING_POINTS);

    out[0] += fval * actual_volume * (1.f - actual_panning);
    out[1] += fval * actual_volume * actual_panning * surround_sign;
    out += 2;

    pos += step;
    while (pos >= loop_end)
      pos -= loop_length;

    frame_count++;
    XM_SLIDE_TOWARDS(actual_volume, target_volume, volume_ramp);
    XM_SLIDE_TOWARDS(actual_panning, target_panning, panning_ramp);
  }

  ch->sample_position = pos;
  ch->frame_count = frame_count;
  ch->actual_volume = actual_volume;
  ch->actual_panning = actual_panning;
}

void IRAM_ATTR xm_render_sample_ramp_forward_loop_16(xm_channel_context_t* ch, float* out, size_t count, float volume_ramp, float panning_ramp)
{
  xm_sample_t* sample = ch->sample;
  const int16_t* data = sample->data16;
  const uint32_t loop_start = sample->loop_start;
  const uint32_t loop_end = sample->loop_end;
  const uint32_t loop_length = sample->loop_length;
  float pos = ch->sample_position;
  const float step = ch->step;
  float actual_volume = ch->actual_volume;
  float actual_panning = ch->actual_panning;
  const float surround_sign = ch->surround ? -1.f : 1.f;
  const float target_volume = ch->target_volume;
  const float target_panning = ch->target_panning;
  unsigned long frame_count = ch->frame_count;

  for (size_t i = 0; i < count; i++)
  {
    uint32_t a = (uint32_t)pos;
    uint32_t b = a + 1;
    float t = pos - a;
    float u = data[a] / 32768.f;
    float v = data[(b == loop_end) ? loop_start : b] / 32768.f;
    float fval = XM_LERP(u, v, t);
    fval = XM_LERP(ch->end_of_previous_sample[frame_count], fval,
                   (float)frame_count / (float)XM_SAMPLE_RAMPING_POINTS);

    out[0] += fval * actual_volume * (1.f - actual_panning);
    out[1] += fval * actual_volume * actual_panning * surround_sign;
    out += 2;

    pos += step;
    while (pos >= loop_end)
      pos -= loop_length;

    frame_count++;
    XM_SLIDE_TOWARDS(actual_volume, target_volume, volume_ramp);
    XM_SLIDE_TOWARDS(actual_panning, target_panning, panning_ramp);
  }

  ch->sample_position = pos;
  ch->frame_count = frame_count;
  ch->actual_volume = actual_volume;
  ch->actual_panning = actual_panning;
}

void IRAM_ATTR xm_render_sample_ramp_pingpong_loop_8(xm_channel_context_t* ch, float* out, size_t count, float volume_ramp, float panning_ramp)
{
  xm_sample_t* sample = ch->sample;
  const int8_t* data = sample->data8;
  const uint32_t length = sample->length;
  const uint32_t loop_start = sample->loop_start;
  const uint32_t loop_end = sample->loop_end;
  float pos = ch->sample_position;
  const float step = ch->step;
  bool ping = ch->ping;
  float actual_volume = ch->actual_volume;
  float actual_panning = ch->actual_panning;
  const float surround_sign = ch->surround ? -1.f : 1.f;
  const float target_volume = ch->target_volume;
  const float target_panning = ch->target_panning;
  unsigned long frame_count = ch->frame_count;

  size_t done = 0;
  while (done < count)
  {
    if (ping)
    {
      size_t run = xm_pingpong_forward_subrun(pos, step, loop_end, count - done);
      for (size_t i = 0; i < run; i++)
      {
        uint32_t a = (uint32_t)pos;
        uint32_t b = a + 1;
        float t = pos - a;
        float u = data[a] / 128.f;
        float v = data[(b >= loop_end) ? a : b] / 128.f;
        float fval = XM_LERP(u, v, t);
        fval = XM_LERP(ch->end_of_previous_sample[frame_count], fval,
                       (float)frame_count / (float)XM_SAMPLE_RAMPING_POINTS);

        out[0] += fval * actual_volume * (1.f - actual_panning);
        out[1] += fval * actual_volume * actual_panning * surround_sign;
        out += 2;
        pos += step;
        frame_count++;
        XM_SLIDE_TOWARDS(actual_volume, target_volume, volume_ramp);
        XM_SLIDE_TOWARDS(actual_panning, target_panning, panning_ramp);
      }

      if (pos >= loop_end)
      {
        ping = false;
        pos  = (loop_end << 1) - pos;
      }

      if (pos >= length)
      {
        ping = false;
        pos -= length - 1;
      }

      done += run;
    }
    else
    {
      size_t run = xm_pingpong_backward_subrun(pos, step, loop_start, count - done);
      for (size_t i = 0; i < run; i++)
      {
        uint32_t a = (uint32_t)pos;
        uint32_t b = a + 1;
        float t = pos - a;
        float v = data[a] / 128.f;
        float u = data[(b == 1 || b - 2 <= loop_start) ? a : (b - 2)] / 128.f;
        float fval = XM_LERP(u, v, t);
        fval = XM_LERP(ch->end_of_previous_sample[frame_count], fval,
                       (float)frame_count / (float)XM_SAMPLE_RAMPING_POINTS);

        out[0] += fval * actual_volume * (1.f - actual_panning);
        out[1] += fval * actual_volume * actual_panning * surround_sign;
        out += 2;
        pos -= step;
        frame_count++;
        XM_SLIDE_TOWARDS(actual_volume, target_volume, volume_ramp);
        XM_SLIDE_TOWARDS(actual_panning, target_panning, panning_ramp);
      }

      if (pos <= loop_start)
      {
        ping = true;
        pos  = (loop_start << 1) - pos;
      }

      if (pos <= .0f)
      {
        ping = true;
        pos  = .0f;
      }

      done += run;
    }
  }

  ch->ping = ping;
  ch->sample_position = pos;
  ch->frame_count = frame_count;
  ch->actual_volume = actual_volume;
  ch->actual_panning = actual_panning;
}

void IRAM_ATTR xm_render_sample_ramp_pingpong_loop_16(xm_channel_context_t* ch, float* out, size_t count, float volume_ramp, float panning_ramp)
{
  xm_sample_t* sample = ch->sample;
  const int16_t* data = sample->data16;
  const uint32_t length = sample->length;
  const uint32_t loop_start = sample->loop_start;
  const uint32_t loop_end = sample->loop_end;
  float pos = ch->sample_position;
  const float step = ch->step;
  bool ping = ch->ping;
  float actual_volume = ch->actual_volume;
  float actual_panning = ch->actual_panning;
  const float surround_sign = ch->surround ? -1.f : 1.f;
  const float target_volume = ch->target_volume;
  const float target_panning = ch->target_panning;
  unsigned long frame_count = ch->frame_count;

  size_t done = 0;
  while (done < count)
  {
    if (ping)
    {
      size_t run = xm_pingpong_forward_subrun(pos, step, loop_end, count - done);
      for (size_t i = 0; i < run; i++)
      {
        uint32_t a = (uint32_t)pos;
        uint32_t b = a + 1;
        float t = pos - a;
        float u = data[a] / 32768.f;
        float v = data[(b >= loop_end) ? a : b] / 32768.f;
        float fval = XM_LERP(u, v, t);
        fval = XM_LERP(ch->end_of_previous_sample[frame_count], fval,
                       (float)frame_count / (float)XM_SAMPLE_RAMPING_POINTS);

        out[0] += fval * actual_volume * (1.f - actual_panning);
        out[1] += fval * actual_volume * actual_panning * surround_sign;
        out += 2;
        pos += step;
        frame_count++;
        XM_SLIDE_TOWARDS(actual_volume, target_volume, volume_ramp);
        XM_SLIDE_TOWARDS(actual_panning, target_panning, panning_ramp);
      }

      if (pos >= loop_end)
      {
        ping = false;
        pos  = (loop_end << 1) - pos;
      }

      if (pos >= length)
      {
        ping = false;
        pos -= length - 1;
      }

      done += run;
    }
    else
    {
      size_t run = xm_pingpong_backward_subrun(pos, step, loop_start, count - done);
      for (size_t i = 0; i < run; i++)
      {
        uint32_t a = (uint32_t)pos;
        uint32_t b = a + 1;
        float t = pos - a;
        float v = data[a] / 32768.f;
        float u = data[(b == 1 || b - 2 <= loop_start) ? a : (b - 2)] / 32768.f;
        float fval = XM_LERP(u, v, t);
        fval = XM_LERP(ch->end_of_previous_sample[frame_count], fval,
                       (float)frame_count / (float)XM_SAMPLE_RAMPING_POINTS);

        out[0] += fval * actual_volume * (1.f - actual_panning);
        out[1] += fval * actual_volume * actual_panning * surround_sign;
        out += 2;
        pos -= step;
        frame_count++;
        XM_SLIDE_TOWARDS(actual_volume, target_volume, volume_ramp);
        XM_SLIDE_TOWARDS(actual_panning, target_panning, panning_ramp);
      }

      if (pos <= loop_start)
      {
        ping = true;
        pos  = (loop_start << 1) - pos;
      }

      if (pos <= .0f)
      {
        ping = true;
        pos  = .0f;
      }

      done += run;
    }
  }

  ch->ping = ping;
  ch->sample_position = pos;
  ch->frame_count = frame_count;
  ch->actual_volume = actual_volume;
  ch->actual_panning = actual_panning;
}

void IRAM_ATTR xm_render_gain_ramp_no_loop_8(xm_channel_context_t* ch, float* out, size_t count, float volume_ramp, float panning_ramp)
{
  xm_sample_t* sample = ch->sample;
  const int8_t* data = sample->data8;
  const uint32_t length = sample->length;
  float pos = ch->sample_position;
  const float step = ch->step;
  float actual_volume = ch->actual_volume;
  float actual_panning = ch->actual_panning;
  const float surround_sign = ch->surround ? -1.f : 1.f;
  const float target_volume = ch->target_volume;
  const float target_panning = ch->target_panning;
  unsigned long frame_count = ch->frame_count;

  for (size_t i = 0; i < count; i++)
  {
    uint32_t a = (uint32_t)pos;
    uint32_t b = a + 1;
    float t = pos - a;
    float u = data[a] / 128.f;
    float v = (b < length) ? data[b] / 128.f : .0f;
    float fval = XM_LERP(u, v, t);

    out[0] += fval * actual_volume * (1.f - actual_panning);
    out[1] += fval * actual_volume * actual_panning * surround_sign;
    out += 2;

    pos += step;

    frame_count++;
    XM_SLIDE_TOWARDS(actual_volume, target_volume, volume_ramp);
    XM_SLIDE_TOWARDS(actual_panning, target_panning, panning_ramp);

    if (pos >= length)
    {
      pos = -1;
      break;
    }
  }

  ch->sample_position = pos;
  ch->frame_count = frame_count;
  ch->actual_volume = actual_volume;
  ch->actual_panning = actual_panning;
}

void IRAM_ATTR xm_render_gain_ramp_no_loop_16(xm_channel_context_t* ch, float* out, size_t count, float volume_ramp, float panning_ramp)
{
  xm_sample_t* sample = ch->sample;
  const int16_t* data = sample->data16;
  const uint32_t length = sample->length;
  float pos = ch->sample_position;
  const float step = ch->step;
  float actual_volume = ch->actual_volume;
  float actual_panning = ch->actual_panning;
  const float surround_sign = ch->surround ? -1.f : 1.f;
  const float target_volume = ch->target_volume;
  const float target_panning = ch->target_panning;
  unsigned long frame_count = ch->frame_count;

  for (size_t i = 0; i < count; i++)
  {
    uint32_t a = (uint32_t)pos;
    uint32_t b = a + 1;
    float t = pos - a;
    float u = data[a] / 32768.f;
    float v = (b < length) ? data[b] / 32768.f : .0f;
    float fval = XM_LERP(u, v, t);

    out[0] += fval * actual_volume * (1.f - actual_panning);
    out[1] += fval * actual_volume * actual_panning * surround_sign;
    out += 2;

    pos += step;

    frame_count++;
    XM_SLIDE_TOWARDS(actual_volume, target_volume, volume_ramp);
    XM_SLIDE_TOWARDS(actual_panning, target_panning, panning_ramp);

    if (pos >= length)
    {
      pos = -1;
      break;
    }
  }

  ch->sample_position = pos;
  ch->frame_count = frame_count;
  ch->actual_volume = actual_volume;
  ch->actual_panning = actual_panning;
}

void IRAM_ATTR xm_render_gain_ramp_forward_loop_8(xm_channel_context_t* ch, float* out, size_t count, float volume_ramp, float panning_ramp)
{
  xm_sample_t* sample = ch->sample;
  const int8_t* data = sample->data8;
  const uint32_t loop_start = sample->loop_start;
  const uint32_t loop_end = sample->loop_end;
  const uint32_t loop_length = sample->loop_length;
  float pos = ch->sample_position;
  const float step = ch->step;
  float actual_volume = ch->actual_volume;
  float actual_panning = ch->actual_panning;
  const float surround_sign = ch->surround ? -1.f : 1.f;
  const float target_volume = ch->target_volume;
  const float target_panning = ch->target_panning;
  unsigned long frame_count = ch->frame_count;

  for (size_t i = 0; i < count; i++)
  {
    uint32_t a = (uint32_t)pos;
    uint32_t b = a + 1;
    float t = pos - a;
    float u = data[a] / 128.f;
    float v = data[(b == loop_end) ? loop_start : b] / 128.f;
    float fval = XM_LERP(u, v, t);

    out[0] += fval * actual_volume * (1.f - actual_panning);
    out[1] += fval * actual_volume * actual_panning * surround_sign;
    out += 2;

    pos += step;
    while (pos >= loop_end)
      pos -= loop_length;

    frame_count++;
    XM_SLIDE_TOWARDS(actual_volume, target_volume, volume_ramp);
    XM_SLIDE_TOWARDS(actual_panning, target_panning, panning_ramp);
  }

  ch->sample_position = pos;
  ch->frame_count = frame_count;
  ch->actual_volume = actual_volume;
  ch->actual_panning = actual_panning;
}

void IRAM_ATTR xm_render_gain_ramp_forward_loop_16(xm_channel_context_t* ch, float* out, size_t count, float volume_ramp, float panning_ramp)
{
  xm_sample_t* sample = ch->sample;
  const int16_t* data = sample->data16;
  const uint32_t loop_start = sample->loop_start;
  const uint32_t loop_end = sample->loop_end;
  const uint32_t loop_length = sample->loop_length;
  float pos = ch->sample_position;
  const float step = ch->step;
  float actual_volume = ch->actual_volume;
  float actual_panning = ch->actual_panning;
  const float surround_sign = ch->surround ? -1.f : 1.f;
  const float target_volume = ch->target_volume;
  const float target_panning = ch->target_panning;
  unsigned long frame_count = ch->frame_count;

  for (size_t i = 0; i < count; i++)
  {
    uint32_t a = (uint32_t)pos;
    uint32_t b = a + 1;
    float t = pos - a;
    float u = data[a] / 32768.f;
    float v = data[(b == loop_end) ? loop_start : b] / 32768.f;
    float fval = XM_LERP(u, v, t);

    out[0] += fval * actual_volume * (1.f - actual_panning);
    out[1] += fval * actual_volume * actual_panning * surround_sign;
    out += 2;

    pos += step;
    while (pos >= loop_end)
      pos -= loop_length;

    frame_count++;
    XM_SLIDE_TOWARDS(actual_volume, target_volume, volume_ramp);
    XM_SLIDE_TOWARDS(actual_panning, target_panning, panning_ramp);
  }

  ch->sample_position = pos;
  ch->frame_count = frame_count;
  ch->actual_volume = actual_volume;
  ch->actual_panning = actual_panning;
}

void IRAM_ATTR xm_render_gain_ramp_pingpong_loop_8(xm_channel_context_t* ch, float* out, size_t count, float volume_ramp, float panning_ramp)
{
  xm_sample_t* sample = ch->sample;
  const int8_t* data = sample->data8;
  const uint32_t length = sample->length;
  const uint32_t loop_start = sample->loop_start;
  const uint32_t loop_end = sample->loop_end;
  float pos = ch->sample_position;
  const float step = ch->step;
  bool ping = ch->ping;
  float actual_volume = ch->actual_volume;
  float actual_panning = ch->actual_panning;
  const float surround_sign = ch->surround ? -1.f : 1.f;
  const float target_volume = ch->target_volume;
  const float target_panning = ch->target_panning;
  unsigned long frame_count = ch->frame_count;

  size_t done = 0;
  while (done < count)
  {
    if (ping)
    {
      size_t run = xm_pingpong_forward_subrun(pos, step, loop_end, count - done);
      for (size_t i = 0; i < run; i++)
      {
        uint32_t a = (uint32_t)pos;
        uint32_t b = a + 1;
        float t = pos - a;
        float u = data[a] / 128.f;
        float v = data[(b >= loop_end) ? a : b] / 128.f;
        float fval = XM_LERP(u, v, t);

        out[0] += fval * actual_volume * (1.f - actual_panning);
        out[1] += fval * actual_volume * actual_panning * surround_sign;
        out += 2;
        pos += step;
        frame_count++;
        XM_SLIDE_TOWARDS(actual_volume, target_volume, volume_ramp);
        XM_SLIDE_TOWARDS(actual_panning, target_panning, panning_ramp);
      }

      if (pos >= loop_end)
      {
        ping = false;
        pos  = (loop_end << 1) - pos;
      }

      if (pos >= length)
      {
        ping = false;
        pos -= length - 1;
      }

      done += run;
    }
    else
    {
      size_t run = xm_pingpong_backward_subrun(pos, step, loop_start, count - done);
      for (size_t i = 0; i < run; i++)
      {
        uint32_t a = (uint32_t)pos;
        uint32_t b = a + 1;
        float t = pos - a;
        float v = data[a] / 128.f;
        float u = data[(b == 1 || b - 2 <= loop_start) ? a : (b - 2)] / 128.f;
        float fval = XM_LERP(u, v, t);

        out[0] += fval * actual_volume * (1.f - actual_panning);
        out[1] += fval * actual_volume * actual_panning * surround_sign;
        out += 2;
        pos -= step;
        frame_count++;
        XM_SLIDE_TOWARDS(actual_volume, target_volume, volume_ramp);
        XM_SLIDE_TOWARDS(actual_panning, target_panning, panning_ramp);
      }

      if (pos <= loop_start)
      {
        ping = true;
        pos  = (loop_start << 1) - pos;
      }

      if (pos <= .0f)
      {
        ping = true;
        pos  = .0f;
      }

      done += run;
    }
  }

  ch->ping = ping;
  ch->sample_position = pos;
  ch->frame_count = frame_count;
  ch->actual_volume = actual_volume;
  ch->actual_panning = actual_panning;
}

void IRAM_ATTR xm_render_gain_ramp_pingpong_loop_16(xm_channel_context_t* ch, float* out, size_t count, float volume_ramp, float panning_ramp)
{
  xm_sample_t* sample = ch->sample;
  const int16_t* data = sample->data16;
  const uint32_t length = sample->length;
  const uint32_t loop_start = sample->loop_start;
  const uint32_t loop_end = sample->loop_end;
  float pos = ch->sample_position;
  const float step = ch->step;
  bool ping = ch->ping;
  float actual_volume = ch->actual_volume;
  float actual_panning = ch->actual_panning;
  const float surround_sign = ch->surround ? -1.f : 1.f;
  const float target_volume = ch->target_volume;
  const float target_panning = ch->target_panning;
  unsigned long frame_count = ch->frame_count;

  size_t done = 0;
  while (done < count)
  {
    if (ping)
    {
      size_t run = xm_pingpong_forward_subrun(pos, step, loop_end, count - done);
      for (size_t i = 0; i < run; i++)
      {
        uint32_t a = (uint32_t)pos;
        uint32_t b = a + 1;
        float t = pos - a;
        float u = data[a] / 32768.f;
        float v = data[(b >= loop_end) ? a : b] / 32768.f;
        float fval = XM_LERP(u, v, t);

        out[0] += fval * actual_volume * (1.f - actual_panning);
        out[1] += fval * actual_volume * actual_panning * surround_sign;
        out += 2;
        pos += step;
        frame_count++;
        XM_SLIDE_TOWARDS(actual_volume, target_volume, volume_ramp);
        XM_SLIDE_TOWARDS(actual_panning, target_panning, panning_ramp);
      }

      if (pos >= loop_end)
      {
        ping = false;
        pos  = (loop_end << 1) - pos;
      }

      if (pos >= length)
      {
        ping = false;
        pos -= length - 1;
      }

      done += run;
    }
    else
    {
      size_t run = xm_pingpong_backward_subrun(pos, step, loop_start, count - done);
      for (size_t i = 0; i < run; i++)
      {
        uint32_t a = (uint32_t)pos;
        uint32_t b = a + 1;
        float t = pos - a;
        float v = data[a] / 32768.f;
        float u = data[(b == 1 || b - 2 <= loop_start) ? a : (b - 2)] / 32768.f;
        float fval = XM_LERP(u, v, t);

        out[0] += fval * actual_volume * (1.f - actual_panning);
        out[1] += fval * actual_volume * actual_panning * surround_sign;
        out += 2;
        pos -= step;
        frame_count++;
        XM_SLIDE_TOWARDS(actual_volume, target_volume, volume_ramp);
        XM_SLIDE_TOWARDS(actual_panning, target_panning, panning_ramp);
      }

      if (pos <= loop_start)
      {
        ping = true;
        pos  = (loop_start << 1) - pos;
      }

      if (pos <= .0f)
      {
        ping = true;
        pos  = .0f;
      }

      done += run;
    }
  }

  ch->ping = ping;
  ch->sample_position = pos;
  ch->frame_count = frame_count;
  ch->actual_volume = actual_volume;
  ch->actual_panning = actual_panning;
}
static void IRAM_ATTR xm_sample_no_tick(xm_context_t* ctx, float* left, float* right)
{
  *left  = 0.f;
  *right = 0.f;

  if (ctx->max_loop_count > 0 && ctx->loop_count >= ctx->max_loop_count)
    return;

  for (uint8_t i = 0; i < ctx->module.num_channels; ++i)
  {
    xm_channel_context_t* ch = ctx->channels + i;

    if (ch->instrument == NULL || ch->sample == NULL || ch->sample_position < 0)
      continue;

    const float fval = xm_next_of_sample(ch);

    if (!ch->muted && !ch->instrument->muted)
    {
      *left  += fval * ch->actual_volume * (1.f - ch->actual_panning);
      *right += fval * ch->actual_volume * ch->actual_panning * (ch->surround ? -1.f : 1.f);
    }

#if XM_RAMPING
    ch->frame_count++;
    XM_SLIDE_TOWARDS(ch->actual_volume, ch->target_volume, ctx->volume_ramp);
    XM_SLIDE_TOWARDS(ch->actual_panning, ch->target_panning, ctx->panning_ramp);
#endif
  }

  const float fgvol = ctx->global_volume * ctx->amplification;
  *left  *= fgvol;
  *right *= fgvol;
}

void IRAM_ATTR xm_sample(xm_context_t* ctx, float* left, float* right)
{
  if (ctx->remaining_samples_in_tick <= 0)
    xm_tick(ctx);

  ctx->remaining_samples_in_tick--;
  xm_sample_no_tick(ctx, left, right);
}

/* Канал за run кадров — в стерео-буфер dst (прибавляется). VDAC4: вынесено
 * из xm_render, чтобы канал с фильтром рисовать кусками в свой буфер. */
static void IRAM_ATTR xm_render_channel(xm_context_t* ctx, xm_channel_context_t* ch, float* dst,
                                        size_t run)
{
  if (ch->instrument == NULL || ch->sample == NULL || ch->sample_position < 0)
    return;

  xm_sample_t* sample = ch->sample;
  if (sample->length == 0)
  {
#if XM_RAMPING
    for (size_t j = 0; j < run; j++)
    {
      ch->frame_count++;
      XM_SLIDE_TOWARDS(ch->actual_volume, ch->target_volume, ctx->volume_ramp);
      XM_SLIDE_TOWARDS(ch->actual_panning, ch->target_panning, ctx->panning_ramp);
    }
#endif
    return;
  }

  const xm_loop_type_t loop_type = sample->loop_type;
  const uint8_t bits = sample->bits;
  const bool muted = ch->muted || ch->instrument->muted;

#if XM_RAMPING
  size_t done = 0;

  if (!muted && ch->frame_count < XM_SAMPLE_RAMPING_POINTS)
  {
    size_t ramp_run = XM_SAMPLE_RAMPING_POINTS - ch->frame_count;
    if (ramp_run > run)
      ramp_run = run;

    if (bits == 8)
    {
      switch (loop_type)
      {
        case XM_NO_LOOP:
          xm_render_sample_ramp_no_loop_8(ch, dst, ramp_run, ctx->volume_ramp, ctx->panning_ramp);
          break;

        case XM_FORWARD_LOOP:
          xm_render_sample_ramp_forward_loop_8(ch, dst, ramp_run, ctx->volume_ramp, ctx->panning_ramp);
          break;

        case XM_PING_PONG_LOOP:
          xm_render_sample_ramp_pingpong_loop_8(ch, dst, ramp_run, ctx->volume_ramp, ctx->panning_ramp);
          break;

        default:
          break;
      }
    }
    else
    {
      switch (loop_type)
      {
        case XM_NO_LOOP:
          xm_render_sample_ramp_no_loop_16(ch, dst, ramp_run, ctx->volume_ramp, ctx->panning_ramp);
          break;

        case XM_FORWARD_LOOP:
          xm_render_sample_ramp_forward_loop_16(ch, dst, ramp_run, ctx->volume_ramp, ctx->panning_ramp);
          break;

        case XM_PING_PONG_LOOP:
          xm_render_sample_ramp_pingpong_loop_16(ch, dst, ramp_run, ctx->volume_ramp, ctx->panning_ramp);
          break;

        default:
          break;
      }
    }

    done += ramp_run;
    dst += 2 * ramp_run;
    if (ch->sample_position < 0)
      return;
  }

  size_t rest = run - done;
  if (rest > 0)
  {
    bool gain_ramp = ch->actual_volume != ch->target_volume
                     || ch->actual_panning != ch->target_panning;

    if (muted)
    {
      if (gain_ramp)
      {
        switch (loop_type)
        {
          case XM_NO_LOOP:
            xm_advance_gain_ramp_no_loop(ch, rest, ctx->volume_ramp, ctx->panning_ramp);
            break;

          case XM_FORWARD_LOOP:
            xm_advance_gain_ramp_forward_loop(ch, rest, ctx->volume_ramp, ctx->panning_ramp);
            break;

          case XM_PING_PONG_LOOP:
            xm_advance_gain_ramp_pingpong_loop(ch, rest, ctx->volume_ramp, ctx->panning_ramp);
            break;

          default:
            break;
        }
      }
      else
      {
        switch (loop_type)
        {
          case XM_NO_LOOP:
            xm_advance_no_loop(ch, rest);
            break;

          case XM_FORWARD_LOOP:
            xm_advance_forward_loop(ch, rest);
            break;

          case XM_PING_PONG_LOOP:
            xm_advance_pingpong_loop(ch, rest);
            break;

          default:
            break;
        }

        ch->frame_count += rest;
      }
    }
    else if (gain_ramp)
    {
      if (bits == 8)
      {
        switch (loop_type)
        {
          case XM_NO_LOOP:
            xm_render_gain_ramp_no_loop_8(ch, dst, rest, ctx->volume_ramp, ctx->panning_ramp);
            break;

          case XM_FORWARD_LOOP:
            xm_render_gain_ramp_forward_loop_8(ch, dst, rest, ctx->volume_ramp, ctx->panning_ramp);
            break;

          case XM_PING_PONG_LOOP:
            xm_render_gain_ramp_pingpong_loop_8(ch, dst, rest, ctx->volume_ramp, ctx->panning_ramp);
            break;

          default:
            break;
        }
      }
      else
      {
        switch (loop_type)
        {
          case XM_NO_LOOP:
            xm_render_gain_ramp_no_loop_16(ch, dst, rest, ctx->volume_ramp, ctx->panning_ramp);
            break;

          case XM_FORWARD_LOOP:
            xm_render_gain_ramp_forward_loop_16(ch, dst, rest, ctx->volume_ramp, ctx->panning_ramp);
            break;

          case XM_PING_PONG_LOOP:
            xm_render_gain_ramp_pingpong_loop_16(ch, dst, rest, ctx->volume_ramp, ctx->panning_ramp);
            break;

          default:
            break;
        }
      }
    }
    else if (bits == 8)
    {
      switch (loop_type)
      {
        case XM_NO_LOOP:
          xm_render_no_loop_8(ch, dst, rest);
          break;

        case XM_FORWARD_LOOP:
          xm_render_forward_loop_8(ch, dst, rest);
          break;

        case XM_PING_PONG_LOOP:
          xm_render_pingpong_loop_8(ch, dst, rest);
          break;

        default:
          break;
      }

      ch->frame_count += rest;
    }
    else
    {
      switch (loop_type)
      {
        case XM_NO_LOOP:
          xm_render_no_loop_16(ch, dst, rest);
          break;

        case XM_FORWARD_LOOP:
          xm_render_forward_loop_16(ch, dst, rest);
          break;

        case XM_PING_PONG_LOOP:
          xm_render_pingpong_loop_16(ch, dst, rest);
          break;

        default:
          break;
      }

      ch->frame_count += rest;
    }
  }
#else
  if (muted)
  {
    switch (loop_type)
    {
      case XM_NO_LOOP:
        xm_advance_no_loop(ch, run);
        break;

      case XM_FORWARD_LOOP:
        xm_advance_forward_loop(ch, run);
        break;

      case XM_PING_PONG_LOOP:
        xm_advance_pingpong_loop(ch, run);
        break;

      default:
        break;
    }
  }
  else if (bits == 8)
  {
    switch (loop_type)
    {
      case XM_NO_LOOP:
        xm_render_no_loop_8(ch, dst, run);
        break;

      case XM_FORWARD_LOOP:
        xm_render_forward_loop_8(ch, dst, run);
        break;

      case XM_PING_PONG_LOOP:
        xm_render_pingpong_loop_8(ch, dst, run);
        break;

      default:
        break;
    }
  }
  else
  {
    switch (loop_type)
    {
      case XM_NO_LOOP:
        xm_render_no_loop_16(ch, dst, run);
        break;

      case XM_FORWARD_LOOP:
        xm_render_forward_loop_16(ch, dst, run);
        break;

      case XM_PING_PONG_LOOP:
        xm_render_pingpong_loop_16(ch, dst, run);
        break;

      default:
        break;
    }
  }
#endif
}

#define XM_FILTER_FRAMES 128

/* VDAC4: кусок канала через его фильтр — в микс. Каналы L и R — копии одного
 * звука с разной громкостью: фильтр у каждого свой, итог — как у моно. */
static void IRAM_ATTR xm_filter_mix(xm_channel_context_t* ch, const float* in, float* out,
                                    size_t count)
{
  const float a0 = ch->filter_a0, b0 = ch->filter_b0, b1 = ch->filter_b1;
  float l1 = ch->filter_l1, l2 = ch->filter_l2, r1 = ch->filter_r1, r2 = ch->filter_r2;
  for (size_t i = 0; i < count; i++)
  {
    float l = in[0] * a0 + l1 * b0 + l2 * b1;
    float r = in[1] * a0 + r1 * b0 + r2 * b1;
    XM_CLAMP2F(l, 4.f, -4.f);
    XM_CLAMP2F(r, 4.f, -4.f);
    l2 = l1;
    l1 = l;
    r2 = r1;
    r1 = r;
    out[0] += l;
    out[1] += r;
    in += 2;
    out += 2;
  }
  /* Затухшее — в ноль: денормализованные числа FPU считает медленно. */
  if (fabsf(l1) + fabsf(l2) + fabsf(r1) + fabsf(r2) < 1e-20f)
    l1 = l2 = r1 = r2 = 0.f;
  ch->filter_l1 = l1;
  ch->filter_l2 = l2;
  ch->filter_r1 = r1;
  ch->filter_r2 = r2;
}

void IRAM_ATTR xm_render(xm_context_t* ctx, float* out, size_t samples)
{
  size_t offset = 0;

  while (offset < samples)
  {
    if (ctx->remaining_samples_in_tick <= 0)
      xm_tick(ctx);

    size_t run = (size_t)ctx->remaining_samples_in_tick;
    if ((float)run < ctx->remaining_samples_in_tick)
      run++;
    if (run == 0)
      run = 1;

    const size_t left = samples - offset;
    if (run > left)
      run = left;

    float* clear_dst = out + 2 * offset;
    for (size_t i = 0; i < run; i++)
    {
      clear_dst[0] = 0.f;
      clear_dst[1] = 0.f;
      clear_dst += 2;
    }

    if (!(ctx->max_loop_count > 0 && ctx->loop_count >= ctx->max_loop_count))
    {
      for (uint8_t i = 0; i < ctx->module.num_channels; ++i)
      {
        xm_channel_context_t* ch = ctx->channels + i;
        if (!ch->filter_active)
        {
          xm_render_channel(ctx, ch, out + 2 * offset, run);
          continue;
        }
        for (size_t done = 0; done < run;)
        {
          float part_buffer[2 * XM_FILTER_FRAMES];
          size_t part = run - done;
          if (part > XM_FILTER_FRAMES)
            part = XM_FILTER_FRAMES;
          memset(part_buffer, 0, sizeof(float) * 2 * part);
          xm_render_channel(ctx, ch, part_buffer, part);
          xm_filter_mix(ch, part_buffer, out + 2 * (offset + done), part);
          done += part;
        }
      }

      const float fgvol = ctx->global_volume * ctx->amplification;
      float* mix = out + 2 * offset;
      for (size_t i = 0; i < run; i++)
      {
        mix[0] *= fgvol;
        mix[1] *= fgvol;
        mix += 2;
      }
    }

    ctx->remaining_samples_in_tick -= (float)run;
    offset += run;
  }
}
