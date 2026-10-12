// SAA.ELF: E-Tracker (SAM Coupe), six stereo SAA1099 channels at 50 Hz.
// SPDX-License-Identifier: LGPL-3.0-only
#include "etracker.h"
#include "zm_app.h"
#include "types.h"
#include "SAADevice.h"
#include <new>
#include <stddef.h>
#include <string.h>

namespace {
constexpr uint32_t RATE = 44100, TICK_FRAMES = RATE / 50;
const zm_host_t* host;
zm_source_t table;
struct Source {
  etracker::Tune tune;
  etracker::Engine engine;
  CSAADevice* chip;
  uint32_t frame, remaining;
  float previous[2], dc[2];
};
void write(CSAADevice* chip, unsigned reg, unsigned value) {
  chip->_WriteAddress(static_cast<BYTE>(reg));
  chip->_WriteData(static_cast<BYTE>(value));
}
void reset(Source* s) {
  s->engine.reset(&s->tune);
  s->chip->~CSAADevice();
  new(s->chip) CSAADevice;
  s->chip->_SetSampleRate(RATE);
  s->chip->_SetOversample(1);
  write(s->chip, 0x1c, 1);
  s->frame = s->remaining = 0;
  memset(s->previous, 0, sizeof(s->previous));
  memset(s->dc, 0, sizeof(s->dc));
}
bool tick(Source* s) {
  uint8_t regs[32]; uint32_t mask;
  if (!s->engine.tick(regs, mask)) return false;
  for (unsigned r=0; r<32; ++r) if (mask & (1u << r)) write(s->chip, r, regs[r]);
  s->remaining = TICK_FRAMES;
  return true;
}
void pcm(Source* s, float* out, uint32_t count) {
  for (uint32_t i=0; i<count; ++i) {
    unsigned mixed[2];
    s->chip->_TickAndOutputStereo(mixed[0], mixed[1], 0x3f);
    for (unsigned c=0; c<2; ++c) {
      // Six voices, maximum 15 * 2 * 16 per voice, two oversamples.
      const float x = mixed[c] * (1.0f / 5760.0f);
      const float y = x - s->previous[c] + 0.995f * s->dc[c];
      s->previous[c] = x; s->dc[c] = y;
      if (out) out[2*i+c] = y < -1 ? -1 : y > 1 ? 1 : y;
    }
  }
  s->frame += count; s->remaining -= count;
}
int probe(const uint8_t* data, size_t size) {
  return data && size >= 18 && !memcmp(data+10, "ETracker", 8) ? 100 : 0;
}
void close_source(void* instance) {
  Source* s = static_cast<Source*>(instance);
  if (!s) return;
  if (s->chip) { s->chip->~CSAADevice(); host->free(s->chip); }
  etracker::unload(s->tune, host);
  host->free(s);
}
void* open_source(uint32_t size, const uint8_t* data) {
  Source* s = static_cast<Source*>(host->alloc(sizeof(Source), ZM_MEM_PSRAM));
  if (!s) return nullptr;
  memset(s, 0, sizeof(*s));
  if (!etracker::load(s->tune, data, size, host)) { close_source(s); return nullptr; }
  void* mem = host->alloc(sizeof(CSAADevice), ZM_MEM_INTERNAL);
  if (!mem) mem = host->alloc(sizeof(CSAADevice), ZM_MEM_PSRAM);
  if (!mem) { close_source(s); return nullptr; }
  s->chip = new(mem) CSAADevice;
  reset(s);
  return s;
}
int render(void* instance, float* out, uint32_t max_frames, uint32_t* frames) {
  Source* s = static_cast<Source*>(instance);
  *frames = 0;
  if (max_frames > ZM_RENDER_MAX_FRAMES) max_frames = ZM_RENDER_MAX_FRAMES;
  while (*frames < max_frames) {
    if (!s->remaining && !tick(s)) break;
    uint32_t count = max_frames - *frames;
    if (count > s->remaining) count = s->remaining;
    pcm(s, out + 2 * *frames, count);
    *frames += count;
  }
  return *frames ? ZM_RENDER_OK : s->frame >= s->tune.ticks*TICK_FRAMES ? ZM_RENDER_END : ZM_RENDER_NEED_INPUT;
}
int seek(void* instance, int32_t delta, uint32_t* offset) {
  Source* s = static_cast<Source*>(instance);
  int64_t target_ms = static_cast<int64_t>(s->frame) * 1000 / RATE + delta;
  if (target_ms < 0) target_ms = 0;
  const uint32_t duration = s->tune.ticks * 20;
  if (target_ms > duration) target_ms = duration;
  const uint32_t target = static_cast<uint32_t>(target_ms) * (RATE / 100) / 10;
  reset(s);
  // Rebuild tracker/register state quickly; render the last 60 ms to settle
  // the chip envelopes and DC filter, as in the OPL decoder.
  while (s->frame < target) {
    if (!s->remaining && !tick(s)) break;
    uint32_t count = target - s->frame;
    if (count > s->remaining) count = s->remaining;
    if (target-s->frame > 3*TICK_FRAMES && count == TICK_FRAMES) {
      s->frame += count; s->remaining = 0;
    } else pcm(s, nullptr, count);
    if (host->size >= offsetof(zm_host_t, rest)+sizeof(host->rest) && host->rest) host->rest();
  }
  *offset = UINT32_MAX;
  return ZM_OK;
}
void info(const void* instance, zm_info_t* out) {
  const Source* s = static_cast<const Source*>(instance);
  memset(out, 0, sizeof(*out));
  strcpy(out->format, "SAA1099 E-Tracker");
  out->rate = RATE; out->channels = 2; out->voices = 6;
  out->duration_ms = s->tune.ticks * 20;
  out->position_ms = static_cast<uint32_t>(static_cast<uint64_t>(s->frame)*1000/RATE);
  out->order = s->engine.order; out->order_length = s->tune.orders;
  out->row = s->engine.row - (s->engine.row && !s->engine.row_left ? 1 : 0);
  out->samples = s->tune.sample_count;
}
int levels(const void* instance, uint8_t* out, int max) {
  const Source* s = static_cast<const Source*>(instance);
  const int count = max < 6 ? max : 6;
  if (count > 0) memcpy(out, s->engine.levels, count);
  return count > 0 ? count : 0;
}
} // namespace

extern "C" int main(int argc, char* argv[]) {
  if (argc < 3 || !argv[1] || !argv[2]) return 0;
  host = reinterpret_cast<const zm_host_t*>(argv[1]);
  if (host->abi_major != ZM_SOURCE_ABI_MAJOR ||
      host->size < offsetof(zm_host_t, free)+sizeof(host->free) || !host->alloc || !host->free) return 0;
  memset(&table, 0, sizeof(table));
  table.size=sizeof(table); table.abi_major=ZM_SOURCE_ABI_MAJOR; table.abi_minor=ZM_SOURCE_ABI_MINOR;
  table.name="saa"; table.version="1.0"; table.extensions="SAA COP";
  table.input=ZM_INPUT_WHOLE; table.seek_unit=ZM_SEEK_MS;
  table.probe=probe; table.open=open_source; table.render=render; table.seek=seek;
  table.info=info; table.levels=levels; table.close=close_source;
  *reinterpret_cast<const zm_source_t**>(argv[2])=&table;
  return ZM_SOURCE_ABI_MAJOR;
}
