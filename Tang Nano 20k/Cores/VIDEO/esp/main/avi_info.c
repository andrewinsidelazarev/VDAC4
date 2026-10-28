/* SPDX-License-Identifier: Apache-2.0
 * VIDEO.ELF: формат звука ролика из заголовка AVI. Плагин шлёт начало файла до movi (не больше
 * HDR_MAX) командой 76 частями; по последней части здесь ищется поток «auds» (LIST strl: strh,
 * затем strf — WAVEFORMATEX) и берётся его формат. Ядро видео звук только переносит (чанки NNwb
 * — в FIFO звука), что это за звук — знает отсюда audio.c.
 */
#include <string.h>

#include "video.h"

#define HDR_MAX 16384u

static uint8_t* hdr;                 /* PSRAM: внутренней памяти ESP мало */
static uint32_t hdr_len;
static uint32_t avih_w, avih_h;      /* avih: размер кадра (для окна картинки в hud.cpp) */

static uint32_t le32(const uint8_t* p)
{
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

void avi_info_reset(void) { hdr_len = 0; }

/* Чанки подряд в [at, end): RIFF и LIST — внутрь, strh «auds» запоминается, strf после него —
   формат. Вернёт 1, если формат найден. */
static int walk(uint32_t at, uint32_t end, int* auds, uint16_t f[6], uint32_t rate[2])
{
  while (at + 8 <= end) {
    const uint8_t* c = hdr + at;
    const uint32_t size = le32(c + 4);
    if (!memcmp(c, "RIFF", 4) || !memcmp(c, "LIST", 4)) {
      if (at + 12 > end) return 0;
      if (!memcmp(c + 8, "movi", 4)) return 0;          /* дальше — данные */
      const uint32_t inner_end = at + 8 + size > end ? end : at + 8 + size;
      if (walk(at + 12, inner_end, auds, f, rate)) return 1;
      at = at + 8 + size + (size & 1);
      continue;
    }
    if (!memcmp(c, "avih", 4) && size >= 40 && at + 8 + 40 <= end) {
      avih_w = le32(c + 8 + 32);        /* dwWidth */
      avih_h = le32(c + 8 + 36);        /* dwHeight */
    } else if (!memcmp(c, "strh", 4) && size >= 4 && at + 12 <= end)
      *auds = !memcmp(c + 8, "auds", 4);
    else if (!memcmp(c, "strf", 4) && *auds && size >= 16 && at + 8 + 16 <= end) {
      const uint8_t* w = c + 8;
      f[0] = le16(w);           /* wFormatTag */
      f[1] = le16(w + 2);       /* nChannels */
      rate[0] = le32(w + 4);    /* nSamplesPerSec */
      rate[1] = le32(w + 8);    /* nAvgBytesPerSec */
      f[2] = le16(w + 12);      /* nBlockAlign */
      f[3] = le16(w + 14);      /* wBitsPerSample */
      /* WAVE_FORMAT_EXTENSIBLE: настоящий тег — первые 2 байта SubFormat */
      if (f[0] == 0xFFFE && size >= 40 && at + 8 + 26 <= end) f[0] = le16(w + 24);
      return 1;
    }
    at = at + 8 + size + (size & 1);
  }
  return 0;
}

void avi_info_add(uint32_t offset, const uint8_t* data, uint32_t length, int last)
{
  if (offset == 0) hdr_len = 0;
  if (!hdr && !(hdr = (uint8_t*)V.core->alloc(HDR_MAX, ZM_MEM_PSRAM))) return;
  if (offset == hdr_len && offset < HDR_MAX) {
    const uint32_t n = length > HDR_MAX - offset ? HDR_MAX - offset : length;
    memcpy(hdr + offset, data, n);
    hdr_len += n;
  }
  if (!last) return;
  int auds = 0;
  uint16_t f[6] = {0};
  uint32_t rate[2] = {0, 0};
  avih_w = avih_h = 0;
  const int found = hdr_len >= 12 && !memcmp(hdr, "RIFF", 4) && walk(0, hdr_len, &auds, f, rate);
  V.core->mutex_lock(V.lock);
  V.frame_w = avih_w;
  V.frame_h = avih_h;
  if (found && f[1] >= 1 && rate[0] >= 4000 && rate[0] <= 96000) {
    V.audio_fmt.tag = f[0];
    V.audio_fmt.channels = f[1];
    V.audio_fmt.rate = rate[0];
    V.audio_fmt.avg_bytes = rate[1];
    V.audio_fmt.block_align = f[2];
    V.audio_fmt.bits = f[3];
  } else {
    V.audio_fmt.tag = 0;
  }
  V.audio_fmt.serial++;
  V.core->mutex_unlock(V.lock);
  V.core->log("video: audio tag %04X, %u ch, %u Hz, %u bits", (unsigned)V.audio_fmt.tag,
              (unsigned)V.audio_fmt.channels, (unsigned)V.audio_fmt.rate, (unsigned)V.audio_fmt.bits);
}
