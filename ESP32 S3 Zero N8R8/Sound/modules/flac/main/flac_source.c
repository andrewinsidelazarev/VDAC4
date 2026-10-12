/* FLAC.ELF — native FLAC streamed from the PLAYER ring. libfoxenflac is a
 * resumable push decoder: render never waits for the producer while PLAYER
 * holds its decoder mutex (stop/flush must remain possible during starvation).
 * Metadata is consumed incrementally, including comments larger than the ring.
 * Only a bounded PICTURE is retained; padding/seek tables need no allocation.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "foxen-flac.h"
#include "zm_app.h"
#include "zm_source.h"

#define INPUT_BYTES 4096u
#define WORK_BYTES (64u * 1024u)
#define PICTURE_MAX (2u * 1024u * 1024u)

enum { MAGIC, HEADER, PAYLOAD, SKIP, AUDIO };
typedef struct {
  uint8_t streaminfo[34];
  zm_info_t info;
  char track[16];
  uint32_t file_size, audio_start;
  uint64_t total_frames;
  uint8_t source_channels, valid;
} saved_t;

typedef struct {
  saved_t meta;
  fx_flac_t* decoder;
  void* decoder_mem;
  int32_t* pcm;
  uint8_t input[INPUT_BYTES];
  uint32_t input_at, input_size, consumed;
  uint32_t block_size, block_at, skip;
  uint8_t header[4], header_at, state, block_type, last, have_info;
  uint8_t* picture;
  uint8_t cover_priority, resume, failed;
  /* Vorbis comment state: vendor length, vendor, count, length, value. */
  uint8_t comment_state, scalar_at;
  uint8_t scalar[4];
  uint32_t comment_left, comment_count, text_at;
  char text[256];
  uint64_t frames;
} flac_t;

static const zm_host_t* host;
static zm_source_t table;
static saved_t saved;

static uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint32_t le32(const uint8_t* p) {
  return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}
static uint32_t min32(uint32_t a, uint32_t b) { return a < b ? a : b; }

static void copy_text(char* dst, size_t size, const char* src) {
  size_t n = strlen(src);
  if (n >= size) {
    n = size - 1;
    /* Do not leave a partial UTF-8 code point at the display boundary. */
    while (n && ((uint8_t)src[n] & 0xC0) == 0x80) --n;
  }
  memcpy(dst, src, n);
  dst[n] = 0;
}

static int fail(flac_t* f, const char* why) {
  if (!f->failed) {
    host->log("flac: %s", why);
    ++f->meta.info.errors;
    copy_text(f->meta.info.title, sizeof(f->meta.info.title), why);
  }
  f->failed = 1;
  return 0;
}

static uint32_t fill(flac_t* f) {
  if (f->input_at == f->input_size) {
    f->input_at = 0;
    f->input_size = (uint32_t)host->input_read(f->input, sizeof(f->input));
  }
  return f->input_size - f->input_at;
}
static void consume(flac_t* f, uint32_t n) {
  f->input_at += n;
  f->consumed += n;
}
static int ended(void) { return host->input_ended() && host->input_available() == 0; }

static void comment_value(flac_t* f) {
  f->text[f->text_at] = 0;
  char* value = strchr(f->text, '=');
  if (value == NULL) return;
  *value++ = 0;
  for (char* p = f->text; *p; ++p) {
    if (*p >= 'a' && *p <= 'z') *p -= 'a' - 'A';
  }
  zm_info_t* i = &f->meta.info;
  if (strcmp(f->text, "TITLE") == 0) copy_text(i->title, sizeof(i->title), value);
  else if (strcmp(f->text, "ARTIST") == 0 ||
           (strcmp(f->text, "ALBUMARTIST") == 0 && i->artist[0] == 0))
    copy_text(i->artist, sizeof(i->artist), value);
  else if (strcmp(f->text, "ALBUM") == 0) copy_text(i->album, sizeof(i->album), value);
  else if (strcmp(f->text, "TRACKNUMBER") == 0) copy_text(f->meta.track, sizeof(f->meta.track), value);
  else if (strcmp(f->text, "DATE") == 0 || strcmp(f->text, "YEAR") == 0) {
    unsigned year = 0, n = 0;
    while (value[n] >= '0' && value[n] <= '9' && n < 4) year = year * 10 + value[n++] - '0';
    if (n == 4) i->year = (uint16_t)year;
  }
}

/* Comments never require a whole metadata block in memory. A long unneeded
 * value is skipped, so TITLE after e.g. a 500 KiB COMMENT is still available. */
static void comments(flac_t* f, const uint8_t* p, uint32_t n) {
  while (n && f->comment_state < 5) {
    if (f->comment_state == 1 || f->comment_state == 4) {
      uint32_t take = min32(n, f->comment_left);
      if (f->comment_state == 4) {
        uint32_t keep = min32(take, (uint32_t)sizeof(f->text) - 1 - f->text_at);
        memcpy(f->text + f->text_at, p, keep);
        f->text_at += keep;
      }
      p += take; n -= take; f->comment_left -= take;
      if (f->comment_left) continue;
      if (f->comment_state == 1) f->comment_state = 2;
      else {
        comment_value(f);
        f->comment_state = --f->comment_count ? 3 : 5;
      }
    } else {
      f->scalar[f->scalar_at++] = *p++; --n;
      if (f->scalar_at != 4) continue;
      uint32_t value = le32(f->scalar);
      f->scalar_at = 0;
      if (f->comment_state == 2) {
        f->comment_count = value;
        f->comment_state = value ? 3 : 5;
      } else {
        f->comment_left = value;
        f->text_at = 0;
        ++f->comment_state;
      }
    }
  }
}

static void picture(flac_t* f) {
  const uint8_t* p = f->picture;
  uint32_t size = f->block_size;
  if (p == NULL || size < 32 || host->cover == NULL) return;
  uint32_t kind = be32(p), mime = be32(p + 4), at = 8;
  if (mime > size - at) return;
  /* Linked pictures (MIME "-->") are not downloaded. */
  if (mime == 3 && memcmp(p + at, "-->", 3) == 0) return;
  at += mime;
  if (size - at < 4) return;
  uint32_t description = be32(p + at); at += 4;
  if (description > size - at) return;
  at += description;
  if (size - at < 20) return;
  uint32_t length = be32(p + at + 16); at += 20;
  if (length > size - at || length < 3) return;
  uint8_t priority = kind == 3 ? 2 : 1;
  if (priority <= f->cover_priority) return;
  /* Same cover path as MP3/OGG: PLAYER converts baseline JPEG to a thumbnail. */
  if (p[at] != 0xFF || p[at + 1] != 0xD8 || p[at + 2] != 0xFF) return;
  host->cover(p + at, length);
  f->cover_priority = priority;
}

static int streaminfo(flac_t* f) {
  const uint8_t* s = f->meta.streaminfo;
  uint32_t block = ((uint32_t)s[2] << 8) | s[3];
  uint32_t rate = ((uint32_t)s[10] << 12) | ((uint32_t)s[11] << 4) | (s[12] >> 4);
  uint8_t channels = ((s[12] >> 1) & 7) + 1;
  uint8_t bits = ((s[12] & 1) << 4) + (s[13] >> 4) + 1;
  if (block < 16 || rate == 0 || bits < 4 || bits > 24)
    return fail(f, bits > 24 ? "FLAC >24 bit unsupported" : "Invalid FLAC STREAMINFO");
  f->meta.total_frames = ((uint64_t)(s[13] & 15) << 32) | be32(s + 14);
  f->meta.source_channels = channels;
  f->meta.info.rate = rate;
  f->meta.info.channels = channels == 1 ? 1 : 2;
  f->meta.info.voices = channels;
  f->meta.info.duration_ms = (uint32_t)(f->meta.total_frames * 1000u / rate);
  snprintf(f->meta.info.format, sizeof(f->meta.info.format), "FLAC %u bit", bits);
  uint32_t bytes = fx_flac_size(block, channels);
  if (bytes == 0) return fail(f, "Invalid FLAC block size");
  f->decoder_mem = host->alloc(bytes, ZM_MEM_PSRAM);
  f->pcm = (int32_t*)host->alloc(ZM_RENDER_MAX_FRAMES * channels * sizeof(int32_t), ZM_MEM_PSRAM);
  if (f->decoder_mem == NULL || f->pcm == NULL) return fail(f, "FLAC: not enough memory");
  f->decoder = fx_flac_init(f->decoder_mem, (uint16_t)block, channels);
  /* The wrapper handles all metadata. Give the decoder only STREAMINFO,
   * marked as the last block; this also restores it after AUDIO_FLUSH. */
  uint8_t prefix[42] = {'f','L','a','C',0x80,0,0,34};
  memcpy(prefix + 8, s, 34);
  uint32_t at = 0;
  for (unsigned tries = 0; tries < 8 && at < sizeof(prefix); ++tries) {
    uint32_t n = (uint32_t)sizeof(prefix) - at;
    fx_flac_state_t state = fx_flac_process(f->decoder, prefix + at, &n, NULL, NULL);
    at += n;
    if (state == FLAC_ERR) return fail(f, "Invalid FLAC STREAMINFO");
    if (!n) break;
  }
  if (at != sizeof(prefix)) return fail(f, "Invalid FLAC header");
  f->have_info = 1;
  return 1;
}

static int block_done(flac_t* f) {
  if (f->block_type == 0 && !streaminfo(f)) return 0;
  if (f->block_type == 6) picture(f);
  if (f->picture) { host->free(f->picture); f->picture = NULL; }
  if (f->last) {
    f->state = AUDIO;
    f->meta.audio_start = f->consumed;
    /* Include metadata in the average bitrate: the Z80 seek maps the whole
     * file length to the exact STREAMINFO duration, like other stream codecs. */
    if (f->meta.total_frames) {
      uint64_t kbps = (uint64_t)f->meta.file_size * 8u * f->meta.info.rate /
                      f->meta.total_frames / 1000u;
      f->meta.info.bitrate = (uint16_t)(kbps > 65535 ? 65535 : kbps ? kbps : 1);
    }
    f->meta.valid = 1;
    saved = f->meta;
  } else f->state = HEADER;
  return 1;
}

/* Bound work per render, even for a many-megabyte PADDING block. */
static int metadata(flac_t* f, uint32_t* budget) {
  while (f->state != AUDIO && *budget) {
    uint32_t n = min32(fill(f), *budget);
    if (!n) {
      if (ended()) return fail(f, "Truncated FLAC metadata");
      return 0;
    }
    const uint8_t* p = f->input + f->input_at;
    if (f->state == MAGIC || f->state == HEADER) {
      n = min32(n, 4u - f->header_at);
      memcpy(f->header + f->header_at, p, n);
      f->header_at += (uint8_t)n;
      consume(f, n); *budget -= n;
      if (f->header_at != 4) continue;
      f->header_at = 0;
      if (f->state == MAGIC) {
        if (memcmp(f->header, "fLaC", 4) != 0) return fail(f, "Not a native FLAC file");
        f->state = HEADER;
        continue;
      }
      f->block_type = f->header[0] & 127;
      f->last = f->header[0] >> 7;
      f->block_size = be32(f->header) & 0xFFFFFF;
      f->block_at = 0;
      if ((!f->have_info && f->block_type != 0) ||
          (f->block_type == 0 && (f->have_info || f->block_size != 34)) ||
          f->block_type == 127 ||
          (f->meta.file_size && (f->consumed > f->meta.file_size ||
                                f->block_size > f->meta.file_size - f->consumed)))
        return fail(f, "Invalid FLAC metadata");
      f->comment_state = f->scalar_at = 0;
      if (f->block_type == 6 && f->block_size <= PICTURE_MAX && f->block_size >= 32)
        f->picture = (uint8_t*)host->alloc(f->block_size, ZM_MEM_PSRAM);
      f->state = PAYLOAD;
      if (!f->block_size && !block_done(f)) return 0;
    } else if (f->state == SKIP) {
      n = min32(n, f->skip);
      f->skip -= n;
      consume(f, n); *budget -= n;
      if (!f->skip) f->state = AUDIO;
    } else {
      n = min32(n, f->block_size - f->block_at);
      if (f->block_type == 0) memcpy(f->meta.streaminfo + f->block_at, p, n);
      else if (f->block_type == 4) comments(f, p, n);
      else if (f->picture) memcpy(f->picture + f->block_at, p, n);
      f->block_at += n;
      consume(f, n); *budget -= n;
      if (f->block_at == f->block_size && !block_done(f)) return 0;
    }
  }
  return f->state == AUDIO;
}

static int probe(const uint8_t* head, size_t length) {
  return length >= 4 && memcmp(head, "fLaC", 4) == 0 ? 100 : 0;
}

static void* open_source(uint32_t file_size, const uint8_t* file) {
  (void)file;
  flac_t* f = (flac_t*)host->alloc(sizeof(*f), ZM_MEM_PSRAM);
  if (f == NULL) return NULL;
  memset(f, 0, sizeof(*f));
  f->meta.file_size = file_size;
  copy_text(f->meta.info.format, sizeof(f->meta.info.format), "FLAC");
  uint32_t offset = host->size >= offsetof(zm_host_t, stream_offset) + sizeof(uint32_t)
                      ? host->stream_offset : 0;
  if (offset != 0 && offset != UINT32_MAX) {
    f->resume = 1;
    f->consumed = offset;
    if (!saved.valid || saved.file_size != file_size) fail(f, "FLAC seek needs headers");
    else {
      f->meta = saved;
      if (streaminfo(f)) {
        f->skip = offset < saved.audio_start ? saved.audio_start - offset : 0;
        f->state = f->skip ? SKIP : AUDIO;
      }
    }
  } else saved.valid = 0;
  return f;
}

static void output(flac_t* f, float* out, uint32_t count) {
  uint8_t ch = f->meta.source_channels;
  const float unit = 1.0f / 2147483648.0f;
  if (ch <= 2) {
    for (uint32_t j = 0; j < count * ch; ++j) out[j] = f->pcm[j] * unit;
    return;
  }
  /* FLAC channel order (different from Vorbis). Normalize each side to keep
   * the surround downmix within full scale, including centre and LFE. */
  static const float left[6][8] = {
    {1,0,.7071f}, {1,0,.7071f,0}, {1,0,.7071f,.7071f,0},
    {1,0,.7071f,.5f,.7071f,0}, {1,0,.7071f,.5f,.5f,.7071f,0},
    {1,0,.7071f,.5f,.7071f,0,.7071f,0}};
  static const float right[6][8] = {
    {0,1,.7071f}, {0,1,0,.7071f}, {0,1,.7071f,0,.7071f},
    {0,1,.7071f,.5f,0,.7071f}, {0,1,.7071f,.5f,.5f,0,.7071f},
    {0,1,.7071f,.5f,0,.7071f,0,.7071f}};
  const float* l = left[ch - 3];
  const float* r = right[ch - 3];
  float scale = 0;
  for (uint8_t c = 0; c < ch; ++c) scale += l[c];
  scale = unit / scale;
  for (uint32_t j = 0; j < count; ++j) {
    float a = 0, b = 0;
    for (uint8_t c = 0; c < ch; ++c) {
      a += l[c] * f->pcm[j * ch + c];
      b += r[c] * f->pcm[j * ch + c];
    }
    out[2 * j] = a * scale; out[2 * j + 1] = b * scale;
  }
}

static int render(void* instance, float* out, uint32_t max_frames, uint32_t* frames) {
  flac_t* f = (flac_t*)instance;
  *frames = 0;
  if (f->failed) return ZM_RENDER_ERROR;
  uint32_t budget = WORK_BYTES;
  if (!metadata(f, &budget)) return f->failed ? ZM_RENDER_ERROR : ZM_RENDER_NEED_INPUT;
  max_frames = min32(max_frames, ZM_RENDER_MAX_FRAMES);
  if (!max_frames) return ZM_RENDER_NEED_INPUT;
  for (unsigned step = 0; step < 128 && budget; ++step) {
    uint32_t n = min32(fill(f), budget), read = n;
    uint32_t samples = max_frames * f->meta.source_channels;
    fx_flac_state_t before = fx_flac_get_state(f->decoder);
    fx_flac_state_t state = fx_flac_process(f->decoder, f->input + f->input_at, &read, f->pcm, &samples);
    consume(f, read); budget -= read;
    if (state == FLAC_ERR) {
      fail(f, "Invalid FLAC audio");
      return ZM_RENDER_ERROR;
    }
    if (samples) {
      uint32_t count = samples / f->meta.source_channels;
      output(f, out, count);
      f->frames += count;
      *frames = count;
      return ZM_RENDER_OK;
    }
    if (!read && state == before) {
      if (n == 0 && ended()) {
        if ((!f->resume && f->meta.total_frames && f->frames != f->meta.total_frames) ||
            state == FLAC_IN_FRAME) ++f->meta.info.errors;
        return ZM_RENDER_END;
      }
      return ZM_RENDER_NEED_INPUT;
    }
  }
  return ZM_RENDER_NEED_INPUT;
}

static int seek(void* instance, int32_t delta, uint32_t* offset) {
  (void)instance; (void)delta; (void)offset;
  return ZM_ERROR; /* Z80 reopens the file and uses AUDIO_FLUSH, as for MP3. */
}
static void info(const void* instance, zm_info_t* out) {
  const flac_t* f = (const flac_t*)instance;
  *out = f->meta.info;
  if (out->rate) out->position_ms = (uint32_t)(f->frames * 1000u / out->rate);
}
static const char* tag(const void* instance, const char* name) {
  const flac_t* f = (const flac_t*)instance;
  return strcmp(name, "TRACKNUMBER") == 0 ? f->meta.track : NULL;
}
static void close_source(void* instance) {
  flac_t* f = (flac_t*)instance;
  if (f->picture) host->free(f->picture);
  if (f->decoder_mem) host->free(f->decoder_mem);
  if (f->pcm) host->free(f->pcm);
  host->free(f);
}

int main(int argc, char* argv[]) {
  if (argc < 3) return 0;
  host = (const zm_host_t*)argv[1];
  table.size = sizeof(table);
  table.abi_major = ZM_SOURCE_ABI_MAJOR;
  table.abi_minor = ZM_SOURCE_ABI_MINOR;
  table.name = "flac";
  table.version = "1.0";
  table.extensions = "FLA FLAC";
  table.input = ZM_INPUT_STREAM;
  table.seek_unit = ZM_SEEK_MS;
  table.probe = probe;
  table.open = open_source;
  table.render = render;
  table.seek = seek;
  table.info = info;
  table.close = close_source;
  table.tag = tag;
  *(const zm_source_t**)argv[2] = &table;
  return ZM_SOURCE_ABI_MAJOR;
}
