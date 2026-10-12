// E-Tracker format/player adapted from ZXTune (vitamin.caig@gmail.com).
// SPDX-License-Identifier: LGPL-3.0-only
#pragma once
#include "zm_source.h"
#include <stdint.h>

namespace etracker {
struct Event { uint8_t note, sample, ornament, attenuation, envelope, noise, swap, rest; };
struct Row { uint8_t tempo; Event channel[6]; };
struct Pattern { Row* rows; uint8_t count; };
struct SampleLine { uint16_t deviation; uint8_t amplitude, flags; };
struct Sample { SampleLine* lines; uint16_t count, loop; };
struct Ornament { uint8_t* lines; uint16_t count, loop; };
struct Position { uint8_t pattern, transpose; };
struct Tune {
  Pattern patterns[32];
  Sample samples[32];
  Ornament ornaments[32];
  Position positions[255];
  uint32_t position_ticks[256];
  uint32_t ticks;
  uint16_t orders, loop;
  uint8_t sample_count;
};
bool load(Tune& tune, const uint8_t* data, size_t size, const zm_host_t* host);
void unload(Tune& tune, const zm_host_t* host);
struct Channel {
  uint16_t sample_pos, ornament_pos;
  uint8_t note, sample, ornament, attenuation, swap;
};
struct Engine {
  const Tune* tune;
  Channel channel[6];
  uint16_t order;
  uint8_t row, tempo, row_left, noise[2], levels[6];
  uint32_t ticks;
  void reset(const Tune* source);
  bool tick(uint8_t regs[32], uint32_t& mask);
};
} // namespace etracker
