// fast_opl.cpp — FM-часть OPL блоками по каналу; арифметика — ymfm, см. fast_opl.h.
//
// Порядок такта тот же, что у ymfm (fm_engine_base::clock, затем output):
// подготовка (кэш операторов, клавиши) — после записи или раз в 4097 тактов;
// счётчик огибающих +4; шум и LFO; у каждого канала — сдвиг обратной связи,
// огибающие и фазы операторов; потом выход активных каналов. Каналы друг от
// друга не зависят (кроме ударных 6..8: их фазы общие), поэтому канал
// проходит весь блок тактов сразу, а ударные — вместе.
//
// Copyright (c) 2021, Aaron Giles (ymfm); VDAC4, 2026. BSD-3-Clause.
#include "fast_opl.h"

#include <string.h>

namespace fast_opl {
namespace {

enum { EG_DEPRESS = 0, EG_ATTACK = 1, EG_DECAY = 2, EG_SUSTAIN = 3, EG_RELEASE = 4 };
enum { KEYON_NORMAL = 0, KEYON_RHYTHM = 1, KEYON_CSM = 2 };
const uint32_t EG_QUIET = 0x380;

// ymfm: abs_sin_attenuation — четверть синуса как ослабление 4.8.
const uint16_t sin_table[256] = {
    0x859, 0x6c3, 0x607, 0x58b, 0x52e, 0x4e4, 0x4a6, 0x471, 0x443, 0x41a, 0x3f5, 0x3d3, 0x3b5, 0x398, 0x37e, 0x365,
    0x34e, 0x339, 0x324, 0x311, 0x2ff, 0x2ed, 0x2dc, 0x2cd, 0x2bd, 0x2af, 0x2a0, 0x293, 0x286, 0x279, 0x26d, 0x261,
    0x256, 0x24b, 0x240, 0x236, 0x22c, 0x222, 0x218, 0x20f, 0x206, 0x1fd, 0x1f5, 0x1ec, 0x1e4, 0x1dc, 0x1d4, 0x1cd,
    0x1c5, 0x1be, 0x1b7, 0x1b0, 0x1a9, 0x1a2, 0x19b, 0x195, 0x18f, 0x188, 0x182, 0x17c, 0x177, 0x171, 0x16b, 0x166,
    0x160, 0x15b, 0x155, 0x150, 0x14b, 0x146, 0x141, 0x13c, 0x137, 0x133, 0x12e, 0x129, 0x125, 0x121, 0x11c, 0x118,
    0x114, 0x10f, 0x10b, 0x107, 0x103, 0x0ff, 0x0fb, 0x0f8, 0x0f4, 0x0f0, 0x0ec, 0x0e9, 0x0e5, 0x0e2, 0x0de, 0x0db,
    0x0d7, 0x0d4, 0x0d1, 0x0cd, 0x0ca, 0x0c7, 0x0c4, 0x0c1, 0x0be, 0x0bb, 0x0b8, 0x0b5, 0x0b2, 0x0af, 0x0ac, 0x0a9,
    0x0a7, 0x0a4, 0x0a1, 0x09f, 0x09c, 0x099, 0x097, 0x094, 0x092, 0x08f, 0x08d, 0x08a, 0x088, 0x086, 0x083, 0x081,
    0x07f, 0x07d, 0x07a, 0x078, 0x076, 0x074, 0x072, 0x070, 0x06e, 0x06c, 0x06a, 0x068, 0x066, 0x064, 0x062, 0x060,
    0x05e, 0x05c, 0x05b, 0x059, 0x057, 0x055, 0x053, 0x052, 0x050, 0x04e, 0x04d, 0x04b, 0x04a, 0x048, 0x046, 0x045,
    0x043, 0x042, 0x040, 0x03f, 0x03e, 0x03c, 0x03b, 0x039, 0x038, 0x037, 0x035, 0x034, 0x033, 0x031, 0x030, 0x02f,
    0x02e, 0x02d, 0x02b, 0x02a, 0x029, 0x028, 0x027, 0x026, 0x025, 0x024, 0x023, 0x022, 0x021, 0x020, 0x01f, 0x01e,
    0x01d, 0x01c, 0x01b, 0x01a, 0x019, 0x018, 0x017, 0x017, 0x016, 0x015, 0x014, 0x014, 0x013, 0x012, 0x011, 0x011,
    0x010, 0x00f, 0x00f, 0x00e, 0x00d, 0x00d, 0x00c, 0x00c, 0x00b, 0x00a, 0x00a, 0x009, 0x009, 0x008, 0x008, 0x007,
    0x007, 0x007, 0x006, 0x006, 0x005, 0x005, 0x005, 0x004, 0x004, 0x004, 0x003, 0x003, 0x003, 0x002, 0x002, 0x002,
    0x002, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000};

// ymfm: attenuation_to_volume — мантиссы с неявной 0x400, сдвинуты на 2.
#define X(a) (((a) | 0x400) << 2)
const uint16_t power_table[256] = {
    X(0x3fa), X(0x3f5), X(0x3ef), X(0x3ea), X(0x3e4), X(0x3df), X(0x3da), X(0x3d4),
    X(0x3cf), X(0x3c9), X(0x3c4), X(0x3bf), X(0x3b9), X(0x3b4), X(0x3ae), X(0x3a9),
    X(0x3a4), X(0x39f), X(0x399), X(0x394), X(0x38f), X(0x38a), X(0x384), X(0x37f),
    X(0x37a), X(0x375), X(0x370), X(0x36a), X(0x365), X(0x360), X(0x35b), X(0x356),
    X(0x351), X(0x34c), X(0x347), X(0x342), X(0x33d), X(0x338), X(0x333), X(0x32e),
    X(0x329), X(0x324), X(0x31f), X(0x31a), X(0x315), X(0x310), X(0x30b), X(0x306),
    X(0x302), X(0x2fd), X(0x2f8), X(0x2f3), X(0x2ee), X(0x2e9), X(0x2e5), X(0x2e0),
    X(0x2db), X(0x2d6), X(0x2d2), X(0x2cd), X(0x2c8), X(0x2c4), X(0x2bf), X(0x2ba),
    X(0x2b5), X(0x2b1), X(0x2ac), X(0x2a8), X(0x2a3), X(0x29e), X(0x29a), X(0x295),
    X(0x291), X(0x28c), X(0x288), X(0x283), X(0x27f), X(0x27a), X(0x276), X(0x271),
    X(0x26d), X(0x268), X(0x264), X(0x25f), X(0x25b), X(0x257), X(0x252), X(0x24e),
    X(0x249), X(0x245), X(0x241), X(0x23c), X(0x238), X(0x234), X(0x230), X(0x22b),
    X(0x227), X(0x223), X(0x21e), X(0x21a), X(0x216), X(0x212), X(0x20e), X(0x209),
    X(0x205), X(0x201), X(0x1fd), X(0x1f9), X(0x1f5), X(0x1f0), X(0x1ec), X(0x1e8),
    X(0x1e4), X(0x1e0), X(0x1dc), X(0x1d8), X(0x1d4), X(0x1d0), X(0x1cc), X(0x1c8),
    X(0x1c4), X(0x1c0), X(0x1bc), X(0x1b8), X(0x1b4), X(0x1b0), X(0x1ac), X(0x1a8),
    X(0x1a4), X(0x1a0), X(0x19c), X(0x199), X(0x195), X(0x191), X(0x18d), X(0x189),
    X(0x185), X(0x181), X(0x17e), X(0x17a), X(0x176), X(0x172), X(0x16f), X(0x16b),
    X(0x167), X(0x163), X(0x160), X(0x15c), X(0x158), X(0x154), X(0x151), X(0x14d),
    X(0x149), X(0x146), X(0x142), X(0x13e), X(0x13b), X(0x137), X(0x134), X(0x130),
    X(0x12c), X(0x129), X(0x125), X(0x122), X(0x11e), X(0x11b), X(0x117), X(0x114),
    X(0x110), X(0x10c), X(0x109), X(0x106), X(0x102), X(0x0ff), X(0x0fb), X(0x0f8),
    X(0x0f4), X(0x0f1), X(0x0ed), X(0x0ea), X(0x0e7), X(0x0e3), X(0x0e0), X(0x0dc),
    X(0x0d9), X(0x0d6), X(0x0d2), X(0x0cf), X(0x0cc), X(0x0c8), X(0x0c5), X(0x0c2),
    X(0x0be), X(0x0bb), X(0x0b8), X(0x0b5), X(0x0b1), X(0x0ae), X(0x0ab), X(0x0a8),
    X(0x0a4), X(0x0a1), X(0x09e), X(0x09b), X(0x098), X(0x094), X(0x091), X(0x08e),
    X(0x08b), X(0x088), X(0x085), X(0x082), X(0x07e), X(0x07b), X(0x078), X(0x075),
    X(0x072), X(0x06f), X(0x06c), X(0x069), X(0x066), X(0x063), X(0x060), X(0x05d),
    X(0x05a), X(0x057), X(0x054), X(0x051), X(0x04e), X(0x04b), X(0x048), X(0x045),
    X(0x042), X(0x03f), X(0x03c), X(0x039), X(0x036), X(0x033), X(0x030), X(0x02d),
    X(0x02a), X(0x028), X(0x025), X(0x022), X(0x01f), X(0x01c), X(0x019), X(0x016),
    X(0x014), X(0x011), X(0x00e), X(0x00b), X(0x008), X(0x006), X(0x003), X(0x000)};
#undef X

// ymfm: attenuation_increment.
const uint32_t increment_table[64] = {
    0x00000000, 0x00000000, 0x10101010, 0x10101010, 0x10101010, 0x10101010, 0x11101110, 0x11101110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x11111111, 0x21112111, 0x21212121, 0x22212221, 0x22222222, 0x42224222, 0x42424242, 0x44424442,
    0x44444444, 0x84448444, 0x84848484, 0x88848884, 0x88888888, 0x88888888, 0x88888888, 0x88888888};

// ymfm: s_algorithm_ops (OPL3: 8..11).
#define ALGORITHM(op2in, op3in, op4in, op1out, op2out, op3out) \
  ((op2in) | ((op3in) << 1) | ((op4in) << 4) | ((op1out) << 7) | ((op2out) << 8) | ((op3out) << 9))
const uint16_t algorithm_ops[12] = {
    ALGORITHM(1, 2, 3, 0, 0, 0), ALGORITHM(0, 5, 3, 0, 0, 0), ALGORITHM(0, 2, 6, 0, 0, 0),
    ALGORITHM(1, 0, 7, 0, 0, 0), ALGORITHM(1, 0, 3, 0, 1, 0), ALGORITHM(1, 1, 1, 0, 1, 1),
    ALGORITHM(1, 0, 0, 0, 1, 1), ALGORITHM(0, 0, 0, 1, 1, 1), ALGORITHM(1, 2, 3, 0, 0, 0),
    ALGORITHM(0, 2, 3, 1, 0, 0), ALGORITHM(1, 0, 3, 0, 1, 0), ALGORITHM(0, 2, 0, 1, 0, 1)};
#undef ALGORITHM

inline uint32_t bits(uint32_t value, int start, int length = 1) {
  return (value >> start) & ((1u << length) - 1);
}

inline int32_t clamp(int32_t value, int32_t low, int32_t high) {
  return value < low ? low : (value > high ? high : value);
}

inline uint32_t abs_sin_attenuation(uint32_t input) {
  if (bits(input, 8)) {
    input = ~input;
  }
  return sin_table[input & 0xff];
}

inline uint32_t key_scale_atten(uint32_t block, uint32_t fnum_4msb) {
  static const uint8_t fnum_to_atten[16] = {0, 24, 32, 37, 40, 43, 45, 47, 48, 50, 51, 52, 53, 54, 55, 56};
  const int32_t result = fnum_to_atten[fnum_4msb] - 8 * (int32_t)(block ^ 7);
  return result > 0 ? (uint32_t)result : 0;
}

inline uint32_t effective_rate(uint32_t rawrate, uint32_t ksr) {
  return rawrate == 0 ? 0 : (rawrate + ksr < 63 ? rawrate + ksr : 63);
}

inline uint32_t phase_step(uint32_t block_freq, uint32_t multiple, int32_t lfo_raw_pm) {
  uint32_t fnum = bits(block_freq, 0, 10) << 2;
  fnum += (uint32_t)((lfo_raw_pm * (int32_t)bits(block_freq, 7, 3)) >> 1);
  fnum &= 0xfff;
  const uint32_t block = bits(block_freq, 10, 3);
  const uint32_t step = (fnum << block) >> 2;
  return (step * multiple) >> 1;
}

// ymfm: roundtrip_fp — ЦАП YM3014 (мантисса 10 бит и порядок).
inline int32_t roundtrip_fp(int32_t value) {
  if (value < -32768) {
    return -32768;
  }
  if (value > 32767) {
    return 32767;
  }
  const int32_t scan = value ^ (value >> 31);
  const uint32_t shifted = (uint32_t)scan << 17;
  int zeros = 32;
  if (shifted != 0) {
#if defined(__GNUC__)
    zeros = __builtin_clz(shifted);
#else
    zeros = 0;
    for (uint32_t probe = 0x80000000u; (shifted & probe) == 0; probe >>= 1) {
      ++zeros;
    }
#endif
  }
  int exponent = 7 - zeros;
  if (exponent < 1) {
    exponent = 1;
  }
  exponent -= 1;
  return value & ~((1 << exponent) - 1);
}

}  // namespace

void engine::init_waveforms(uint16_t* table) {
  uint16_t* wf0 = table;
  for (uint32_t index = 0; index < 1024; index++) {
    wf0[index] = (uint16_t)(abs_sin_attenuation(index) | (bits(index, 9) << 15));
  }
  const uint16_t zero = wf0[0];
  for (uint32_t index = 0; index < 1024; index++) {
    table[1 * 1024 + index] = bits(index, 9) ? zero : wf0[index];
    table[2 * 1024 + index] = wf0[index] & 0x7fff;
    table[3 * 1024 + index] = bits(index, 8) ? zero : (uint16_t)(wf0[index] & 0x7fff);
    table[4 * 1024 + index] = bits(index, 9) ? zero : wf0[(index * 2) & 0x3ff];
    table[5 * 1024 + index] = bits(index, 9) ? zero : wf0[(index * 2) & 0x1ff];
    table[6 * 1024 + index] = (uint16_t)(bits(index, 9) << 15);
    table[7 * 1024 + index] = (uint16_t)((bits(index, 9) ? (index ^ 0x13ff) : index) << 3);
  }
}

engine::engine(int revision, const uint16_t* waveforms)
    : revision_(revision),
      channels_(revision >= 3 ? 18 : 9),
      operators_(revision >= 3 ? 36 : 18),
      waveforms_(waveforms),
      env_counter_(0),
      noise_lfsr_(1),
      lfo_am_counter_(0),
      lfo_pm_counter_(0),
      lfo_am_(0),
      modified_(true),
      prepare_count_(0),
      active_((1u << (revision >= 3 ? 18 : 9)) - 1) {
  memset(regs_, 0, sizeof(regs_));
  memset(op_, 0, sizeof(op_));
  memset(ch_, 0, sizeof(ch_));
  for (uint32_t i = 0; i < operators_; ++i) {
    const uint32_t n = i % 18;
    op_[i].opoffs = (uint16_t)(n + 2 * (n / 6) + 0x100 * (i / 18));
    op_[i].env = 0x3ff;
    op_[i].state = EG_RELEASE;
    op_[i].waveform = waveforms_;
  }
  for (uint32_t c = 0; c < channels_; ++c) {
    ch_[c].choffs = (uint16_t)((c % 9) + 0x100 * (c / 9));
  }
  assign_operators();
}

void engine::reset() {
  // Как fm_engine_base::reset: регистры, каналы и операторы; LFO, шум и
  // счётчик огибающих идут дальше.
  memset(regs_, 0, sizeof(regs_));
  for (uint32_t c = 0; c < channels_; ++c) {
    ch_[c].feedback[0] = ch_[c].feedback[1] = 0;
    ch_[c].feedback_in = 0;
  }
  for (uint32_t i = 0; i < operators_; ++i) {
    op_t& op = op_[i];
    op.phase = 0;
    op.env = 0x3ff;
    op.state = EG_RELEASE;
    op.key_state = 0;
    op.keyon_live = 0;
    op.key_dropped = 0;
  }
  modified_ = true;
}

void engine::assign_operators() {
  static const uint8_t two[18][2] = {{0, 3},   {1, 4},   {2, 5},   {6, 9},   {7, 10},  {8, 11},
                                     {12, 15}, {13, 16}, {14, 17}, {18, 21}, {19, 22}, {20, 23},
                                     {24, 27}, {25, 28}, {26, 29}, {30, 33}, {31, 34}, {32, 35}};
  const uint32_t fourop = revision_ >= 3 ? (regs_[0x104] & 0x3f) : 0;
  for (uint32_t c = 0; c < channels_; ++c) {
    channel_t& ch = ch_[c];
    ch.op[0] = two[c][0];
    ch.op[1] = two[c][1];
    ch.op[2] = ch.op[3] = 0xff;
  }
  for (uint32_t pair = 0; pair < 6 && revision_ >= 3; ++pair) {
    if (bits(fourop, (int)pair) == 0) {
      continue;
    }
    const uint32_t first = (pair < 3) ? pair : 9 + (pair - 3);
    channel_t& primary = ch_[first];
    channel_t& secondary = ch_[first + 3];
    primary.op[2] = secondary.op[0];
    primary.op[3] = secondary.op[1];
    secondary.op[0] = secondary.op[1] = 0xff;
  }
  for (uint32_t c = 0; c < channels_; ++c) {
    for (int i = 0; i < 4; ++i) {
      if (ch_[c].op[i] != 0xff) {
        op_[ch_[c].op[i]].choffs = ch_[c].choffs;
      }
    }
  }
}

void engine::cache_operator(op_t& op) {
  const uint32_t choffs = op.choffs;
  const uint32_t opoffs = op.opoffs;
  const uint8_t r20 = regs_[0x20 + opoffs];
  const uint8_t r40 = regs_[0x40 + opoffs];
  const uint8_t r60 = regs_[0x60 + opoffs];
  const uint8_t r80 = regs_[0x80 + opoffs];
  // Волна: OPL2 — при разрешении (регистр 1, бит 5), OPL3 — 2 или 3 бита.
  uint32_t waveform = 0;
  if (revision_ == 2) {
    waveform = bits(regs_[0x01], 5) ? bits(regs_[0xe0 + opoffs], 0, 2) : 0;
  } else if (revision_ >= 3) {
    waveform = bits(regs_[0xe0 + opoffs], 0, newflag() ? 3 : 2);
  }
  op.waveform = waveforms_ + 1024 * waveform;
  const uint32_t block_freq = (bits(regs_[0xb0 + choffs], 0, 5) << 8) | regs_[0xa0 + choffs];
  op.block_freq = block_freq;
  uint32_t keycode = bits(block_freq, 10, 3) << 1;
  keycode |= bits(block_freq, 9 - (int)bits(regs_[0x08], 6), 1);
  const uint32_t multiple = bits(r20, 0, 4);
  op.multiple = ((multiple & 0xe) | bits(0xc2aa, (int)multiple)) * 2;
  if (op.multiple == 0) {
    op.multiple = 1;
  }
  op.am = (uint8_t)bits(r20, 7);
  op.pm = (uint8_t)bits(r20, 6);
  op.phase_step = phase_step(block_freq, op.multiple, 0);
  uint32_t total_level = bits(r40, 0, 6) << 3;
  const uint32_t ksl_raw = bits(r40, 6, 2);
  const uint32_t ksl = bits(ksl_raw, 1) | (bits(ksl_raw, 0) << 1);
  if (ksl != 0) {
    total_level += key_scale_atten(bits(block_freq, 10, 3), bits(block_freq, 6, 4)) << ksl;
  }
  op.total_level = (uint16_t)total_level;
  uint32_t sustain = bits(r80, 4, 4);
  sustain |= (sustain + 1) & 0x10;
  op.eg_sustain = (uint16_t)(sustain << 5);
  const uint32_t ksrval = keycode >> (2 * (bits(r20, 4) ^ 1));
  op.eg_rate[EG_ATTACK] = (uint8_t)effective_rate(bits(r60, 4, 4) * 4, ksrval);
  op.eg_rate[EG_DECAY] = (uint8_t)effective_rate(bits(r60, 0, 4) * 4, ksrval);
  op.eg_rate[EG_SUSTAIN] = bits(r20, 5) ? 0 : (uint8_t)effective_rate(bits(r80, 0, 4) * 4, ksrval);
  op.eg_rate[EG_RELEASE] = (uint8_t)effective_rate(bits(r80, 0, 4) * 4, ksrval);
  op.eg_rate[EG_DEPRESS] = 0x3f;
}

void engine::clock_keystate(op_t& op, uint32_t keystate) {
  if ((keystate ^ op.key_state) == 0) {
    return;
  }
  op.key_state = (uint8_t)keystate;
  if (keystate != 0) {
    if (op.state == EG_ATTACK) {
      return;                       // start_attack: уже атака
    }
    op.state = EG_ATTACK;
    op.phase = 0;
    if (op.eg_rate[EG_ATTACK] >= 62) {
      op.env = 0;
    }
  } else if (op.state < EG_RELEASE) {
    op.state = EG_RELEASE;
  }
}

void engine::prepare() {
  if (revision_ >= 3) {
    assign_operators();
  }
  active_ = 0;
  for (uint32_t c = 0; c < channels_; ++c) {
    bool active = false;
    for (int i = 0; i < 4; ++i) {
      if (ch_[c].op[i] == 0xff) {
        continue;
      }
      op_t& op = op_[ch_[c].op[i]];
      cache_operator(op);
      if (op.key_dropped != 0) {
        clock_keystate(op, 0);        // отпускание между тактами — как у микросхемы
        op.key_dropped = 0;
      }
      clock_keystate(op, op.keyon_live != 0 ? 1 : 0);
      op.keyon_live &= (uint8_t)~(1 << KEYON_CSM);
      if (op.state != EG_RELEASE || op.env < EG_QUIET) {
        active = true;
      }
    }
    if (active) {
      active_ |= 1u << c;
    }
  }
}

void engine::keyonoff(uint32_t channel, uint32_t states, uint32_t type) {
  channel_t& ch = ch_[channel];
  for (int i = 0; i < 4; ++i) {
    if (ch.op[i] != 0xff) {
      op_t& op = op_[ch.op[i]];
      const uint8_t before = op.keyon_live;
      op.keyon_live = (uint8_t)((op.keyon_live & ~(1u << type)) | (bits(states, i) << type));
      if (before != 0 && op.keyon_live == 0) {
        op.key_dropped = 1;           // такт увидит отпускание (key_dropped)
      }
    }
  }
}

void engine::write(uint32_t index, uint8_t data) {
  index &= revision_ >= 3 ? 0x1ff : 0xff;
  modified_ = true;
  if (index == 0x04) {
    // Регистр режима (таймеры): со старшим битом остальные не меняются.
    if (bits(data, 7) != 0) {
      regs_[index] |= 0x80;
    } else {
      regs_[index] = data;
    }
    return;
  }
  regs_[index] = data;
  if (index == 0xbd) {
    const uint32_t mask = bits(data, 5) ? bits(data, 0, 5) : 0;
    keyonoff(6, bits(mask, 4) ? 3 : 0, KEYON_RHYTHM);
    keyonoff(7, bits(mask, 0) | (bits(mask, 3) << 1), KEYON_RHYTHM);
    keyonoff(8, bits(mask, 2) | (bits(mask, 1) << 1), KEYON_RHYTHM);
    return;
  }
  if ((index & 0xf0) == 0xb0) {
    uint32_t channel = index & 0x0f;
    if (channel < 9) {
      if (revision_ >= 3) {
        channel += 9 * bits(index, 8);
      }
      keyonoff(channel, bits(data, 5) ? 15 : 0, KEYON_NORMAL);
    }
  }
}

// Шум и LFO по тактам блока (opl_clock_noise_and_lfo у ymfm).
void engine::lfo_block(uint32_t clocks) {
  static const int8_t pm_scale[8] = {8, 4, 0, -4, -8, -4, 0, 4};
  const uint32_t am_depth = bits(regs_[0xbd], 7);
  const uint32_t pm_depth = bits(regs_[0xbd], 6);
  const int shift = 9 - 2 * (int)am_depth;
  uint32_t noise = noise_lfsr_;
  uint16_t am_counter = lfo_am_counter_;
  uint16_t pm_counter = lfo_pm_counter_;
  for (uint32_t k = 0; k < clocks; ++k) {
    noise <<= 1;
    noise |= bits(noise, 23) ^ bits(noise, 9) ^ bits(noise, 8) ^ bits(noise, 1);
    noise_[k] = (uint8_t)bits(noise, 23);
    const uint32_t am = am_counter++;
    if (am >= 210 * 64 - 1) {
      am_counter = 0;
    }
    am_[k] = (uint8_t)(((am < 105 * 64) ? am : (210 * 64 + 63 - am)) >> shift);
    const uint32_t pm = pm_counter++;
    pm_[k] = (int8_t)(pm_scale[bits(pm, 10, 3)] >> (pm_depth ^ 1));
  }
  noise_lfsr_ = noise;
  lfo_am_counter_ = am_counter;
  lfo_pm_counter_ = pm_counter;
  if (clocks != 0) {
    lfo_am_ = am_[clocks - 1];
  }
}

namespace {

// Огибающая (fm_operator::clock_envelope).
template <class Op>
inline void clock_envelope(Op& op, uint32_t counter) {
  if (op.state == EG_ATTACK && op.env == 0) {
    op.state = EG_DECAY;
  }
  if (op.state == EG_DECAY && op.env >= op.eg_sustain) {
    op.state = EG_SUSTAIN;
  }
  const uint32_t rate = op.eg_rate[op.state];
  const uint32_t rate_shift = rate >> 2;
  counter <<= rate_shift;
  if ((counter & 0x7ff) != 0) {
    return;
  }
  const uint32_t relevant = bits(counter, rate_shift <= 11 ? 11 : (int)rate_shift, 3);
  const uint32_t increment = bits(increment_table[rate], 4 * (int)relevant, 4);
  if (op.state == EG_ATTACK) {
    if (rate < 62) {
      op.env = (uint16_t)(op.env + ((uint32_t)~(uint32_t)op.env * increment >> 4));
    }
  } else {
    uint32_t env = op.env + increment;
    op.env = (uint16_t)(env >= 0x400 ? 0x3ff : env);
  }
}

// Громкость оператора (fm_operator::compute_volume).
template <class Op>
inline int32_t compute_volume(const Op& op, uint32_t phase, uint32_t am_offset) {
  if (op.env > EG_QUIET) {
    return 0;
  }
  const uint32_t sin_attenuation = op.waveform[phase & 0x3ff];
  uint32_t env = op.env + (op.am ? am_offset : 0) + op.total_level;
  env = (env < 0x3ff ? env : 0x3ff) << 2;
  const uint32_t input = (sin_attenuation & 0x7fff) + env;
  const int32_t result = power_table[input & 0xff] >> (input >> 8);
  return (sin_attenuation & 0x8000) ? -result : result;
}

}  // namespace

// Выход канала по флагам регистра C0 (add_to_output у ymfm).
#define ADD_TO_OUTPUT(value)                       \
  do {                                             \
    const int32_t v_ = (value);                    \
    if (outputs & 1) out[0][sample] += v_;         \
    if (outputs & 2) out[1][sample] += v_;         \
    if (outputs & 4) out[2][sample] += v_;         \
    if (outputs & 8) out[3][sample] += v_;         \
  } while (0)

// Рабочая копия оператора на блок: локальная, поэтому компилятор держит её
// поближе и не перечитывает после каждой записи в выход (int32_t* мог бы
// указывать на uint32_t поля оператора).
struct work_op {
  uint32_t phase;
  uint32_t phase_step;
  uint32_t block_freq;
  uint32_t multiple;
  const uint16_t* waveform;
  uint32_t env;
  uint32_t envtl;             // огибающая + TL/KSL (ослабление без AM)
  uint32_t am_mask;           // ~0 — оператору AM, 0 — нет
  uint32_t total_level;
  uint32_t eg_sustain;
  uint32_t state;
  uint32_t pm;
  int32_t last_pm;
  uint32_t pm_step;
  uint8_t eg_rate[5];
  // Огибающая по состоянию: такты срабатывания и строка приращений.
  uint32_t fire_mask;         // такт меняет огибающую, если (счётчик & маска) == 0
  uint32_t step_shift;        // номер шага: (счётчик >> step_shift) & 7
  uint32_t increments;
  bool pending;               // огибающая изменилась: проверить переходы состояний
};

// Параметры канала на блок (регистры в блоке не меняются).
struct work_channel {
  int32_t fb0, fb1, fb_in;
  uint32_t feedback;          // 0 — без обратной связи
  uint32_t fb_shift;
  uint32_t algorithm;
  uint32_t alg_ops;
  uint32_t outputs;
  uint32_t rshift;
  bool modulator_delay;
};

namespace {

// Темп текущего состояния — в маску тактов (clock_envelope у ymfm: такт
// срабатывает, если (счётчик << rate_shift) & 0x7FF == 0). Темп, при котором
// огибающая стоит (0 и 1, атака от 62), — маска «никогда» и нулевые приращения
// (на переполнении счётчика срабатывание ничего не изменит).
inline void envelope_rate(work_op& op) {
  const uint32_t rate = op.eg_rate[op.state];
  const uint32_t rate_shift = rate >> 2;
  const bool still = rate < 2 || (op.state == EG_ATTACK && rate >= 62);
  op.fire_mask = still ? 0xffffffffu : (rate_shift <= 11 ? (0x7ffu >> rate_shift) : 0);
  op.step_shift = rate_shift <= 11 ? 11 - rate_shift : 0;
  op.increments = still ? 0 : increment_table[rate];
}

// То же, что clock_envelope, но переходы состояний проверяются только после
// изменения огибающей (иначе их условия те же), а такт без срабатывания — одно
// сравнение.
inline void clock_envelope_fast(work_op& op, uint32_t counter) {
  if (op.pending) {
    op.pending = false;
    bool changed = false;
    if (op.state == EG_ATTACK && op.env == 0) {
      op.state = EG_DECAY;
      changed = true;
    }
    if (op.state == EG_DECAY && op.env >= op.eg_sustain) {
      op.state = EG_SUSTAIN;
      changed = true;
    }
    if (changed) {
      envelope_rate(op);
    }
  }
  if ((counter & op.fire_mask) != 0) {
    return;
  }
  const uint32_t increment = (op.increments >> (4 * ((counter >> op.step_shift) & 7))) & 15;
  if (op.state == EG_ATTACK) {
    op.env = (uint16_t)(op.env + ((uint32_t)~op.env * increment >> 4));
  } else {
    const uint32_t env = op.env + increment;
    op.env = env >= 0x400 ? 0x3ff : env;
  }
  op.envtl = op.env + op.total_level;
  op.pending = true;
}

// compute_volume ymfm. Проверка «огибающая тише EG_QUIET — 0» не нужна: при
// огибающей от 0x381 сдвиг таблицы степеней не меньше 14, и выходит 0 и так.
inline int32_t volume(const work_op& op, uint32_t phase, uint32_t am) {
  const uint32_t sin_attenuation = op.waveform[phase & 0x3ff];
  uint32_t attenuation = op.envtl + (am & op.am_mask);
  attenuation = (attenuation < 0x3ff ? attenuation : 0x3ff) << 2;
  const uint32_t input = (sin_attenuation & 0x7fff) + attenuation;
  const int32_t result = power_table[input & 0xff] >> (input >> 8);
  const int32_t sign = -(int32_t)(sin_attenuation >> 15);
  return (result ^ sign) - sign;
}

inline void clock_phase(work_op& op, int32_t pm) {
  if (op.pm) {
    if (pm != op.last_pm) {                   // вибрато: шаг меняется раз в 1024 такта
      op.last_pm = pm;
      op.pm_step = phase_step(op.block_freq, op.multiple, pm);
    }
    op.phase += op.pm_step;
  } else {
    op.phase += op.phase_step;
  }
}

// Такты блока одного канала; выход канала — в result[] (по отсчёту на такт с
// выходом), возвращает число таких отсчётов. FOUR — 4 оператора, FLAGS — не
// каждый такт с выходом (OPL4).
template <bool FOUR, bool FLAGS>
uint32_t run_channel(work_op* op, work_channel& ch, int32_t* result, uint32_t clocks, uint32_t base,
                     const uint8_t* am_line, const int8_t* pm_line, const uint8_t* outflag, bool active) {
  const int32_t clipmax = 32767;
  const uint32_t rshift = ch.rshift;
  const bool compute = active;
  const bool any = ch.outputs != 0;
  uint32_t sample = 0;
  int32_t fb0 = ch.fb0, fb1 = ch.fb1, fb_in = ch.fb_in;
  for (uint32_t k = 0; k < clocks; ++k) {
    fb0 = fb1;
    fb1 = fb_in;
    const uint32_t counter = (base + k + 1) & 0x3fffffff;
    const int32_t pm = pm_line[k];
    clock_envelope_fast(op[0], counter);
    clock_phase(op[0], pm);
    clock_envelope_fast(op[1], counter);
    clock_phase(op[1], pm);
    if (FOUR) {
      clock_envelope_fast(op[2], counter);
      clock_phase(op[2], pm);
      clock_envelope_fast(op[3], counter);
      clock_phase(op[3], pm);
    }
    if (FLAGS && !outflag[k]) {
      continue;
    }
    if (compute) {
      const uint32_t am = am_line[k];
      const int32_t opmod = ch.feedback != 0 ? (fb0 + fb1) >> ch.fb_shift : 0;
      const int32_t op1value = volume(op[0], (op[0].phase >> 10) + (uint32_t)opmod, am);
      fb_in = (int16_t)op1value;
      if (any) {
        int32_t value;
        if (!FOUR) {
          const int32_t modulator = ch.modulator_delay ? fb1 : op1value;
          if ((ch.algorithm & 1) == 0) {
            value = volume(op[1], (op[1].phase >> 10) + (uint32_t)(modulator >> 1), am) >> rshift;
          } else {
            value = modulator >> rshift;
            value += volume(op[1], op[1].phase >> 10, am) >> rshift;
            value = clamp(value, -clipmax - 1, clipmax);
          }
        } else {
          const uint32_t alg_ops = ch.alg_ops;
          int16_t opout[8];
          opout[0] = 0;
          opout[1] = (int16_t)op1value;
          int32_t mod = opout[bits(alg_ops, 0, 1)] >> 1;
          opout[2] = (int16_t)volume(op[1], (op[1].phase >> 10) + (uint32_t)mod, am);
          opout[5] = (int16_t)(opout[1] + opout[2]);
          mod = opout[bits(alg_ops, 1, 3)] >> 1;
          opout[3] = (int16_t)volume(op[2], (op[2].phase >> 10) + (uint32_t)mod, am);
          opout[6] = (int16_t)(opout[1] + opout[3]);
          opout[7] = (int16_t)(opout[2] + opout[3]);
          mod = opout[bits(alg_ops, 4, 3)] >> 1;
          value = volume(op[3], (op[3].phase >> 10) + (uint32_t)mod, am) >> rshift;
          if (bits(alg_ops, 7) != 0) {
            value = clamp(value + (opout[1] >> rshift), -clipmax - 1, clipmax);
          }
          if (bits(alg_ops, 8) != 0) {
            value = clamp(value + (opout[2] >> rshift), -clipmax - 1, clipmax);
          }
          if (bits(alg_ops, 9) != 0) {
            value = clamp(value + (opout[3] >> rshift), -clipmax - 1, clipmax);
          }
        }
        result[sample] = value;
      }
    }
    ++sample;
  }
  ch.fb0 = fb0;
  ch.fb1 = fb1;
  ch.fb_in = fb_in;
  return sample;
}

}  // namespace

void engine::channel_block(uint32_t index, int32_t* const* out, uint32_t clocks, const uint8_t* outflag) {
  channel_t& ch = ch_[index];
  op_t* ops[4] = {nullptr, nullptr, nullptr, nullptr};
  for (int i = 0; i < 4; ++i) {
    if (ch.op[i] != 0xff) {
      ops[i] = &op_[ch.op[i]];
    }
  }
  const bool active = ((active_ >> index) & 1) != 0;
  bool silent = ops[0] == nullptr;            // вторая половина 4-операторного канала
  if (!silent && !active && index != 7 && index != 8) {
    // Молчащий канал (все операторы отпущены до конца, канал не активен):
    // огибающие не меняются, фаза до следующего нажатия не нужна (оно её
    // сбрасывает) — такт ничего не меняет, кроме обратной связи. Кроме
    // каналов 7 и 8: фазы их операторов 13 и 17 берут ударные (хай-хэт,
    // малый барабан, тарелка) и без нажатия этих операторов.
    silent = true;
    for (int i = 0; i < 4; ++i) {
      if (ops[i] != nullptr && (ops[i]->state != EG_RELEASE || ops[i]->env != 0x3ff)) {
        silent = false;
      }
    }
  }
  if (silent) {
    for (uint32_t k = 0; k < clocks && k < 2; ++k) {
      ch.feedback[0] = ch.feedback[1];
      ch.feedback[1] = ch.feedback_in;
    }
    return;
  }
  const bool four = ops[2] != nullptr;
  const int count = four ? 4 : 2;
  work_op op[4];
  for (int i = 0; i < count; ++i) {
    const op_t& src = *ops[i];
    work_op& w = op[i];
    w.phase = src.phase;
    w.phase_step = src.phase_step;
    w.block_freq = src.block_freq;
    w.multiple = src.multiple;
    w.waveform = src.waveform;
    w.env = src.env;
    w.total_level = src.total_level;
    w.envtl = w.env + w.total_level;
    w.am_mask = src.am ? 0xffffffffu : 0;
    w.eg_sustain = src.eg_sustain;
    w.state = src.state;
    w.pm = src.pm;
    w.last_pm = 1000;
    w.pm_step = 0;
    memcpy(w.eg_rate, src.eg_rate, sizeof(w.eg_rate));
    envelope_rate(w);
    w.pending = true;
  }
  const uint32_t choffs = ch.choffs;
  const bool opl3 = revision_ >= 3;
  work_channel wc;
  wc.fb0 = ch.feedback[0];
  wc.fb1 = ch.feedback[1];
  wc.fb_in = ch.feedback_in;
  wc.feedback = bits(regs_[0xc0 + choffs], 1, 3);
  wc.fb_shift = 10 - wc.feedback;
  wc.algorithm = bits(regs_[0xc0 + choffs], 0);
  if (opl3) {
    wc.algorithm |= 8 | (bits(regs_[0xc3 + choffs], 0) << 1);
  }
  wc.alg_ops = algorithm_ops[wc.algorithm & 0xb];
  wc.outputs = !opl3 ? 1 : (newflag() ? bits(regs_[0xc0 + choffs], 4, 4) : 3);
  wc.rshift = opl3 ? 0 : 1;
  wc.modulator_delay = !opl3;
  const uint32_t base = env_counter_ >> 2;
  int32_t result[BLOCK];
  uint32_t samples;
  if (four) {
    samples = outflag != nullptr
                  ? run_channel<true, true>(op, wc, result, clocks, base, am_, pm_, outflag, active)
                  : run_channel<true, false>(op, wc, result, clocks, base, am_, pm_, outflag, active);
  } else {
    samples = outflag != nullptr
                  ? run_channel<false, true>(op, wc, result, clocks, base, am_, pm_, outflag, active)
                  : run_channel<false, false>(op, wc, result, clocks, base, am_, pm_, outflag, active);
  }
  if (active) {
    // Выход канала — на выходы, которые ему разрешены (add_to_output у ymfm).
    for (int o = 0; o < 4; ++o) {
      if ((wc.outputs >> o) & 1) {
        int32_t* target = out[o];
        for (uint32_t i = 0; i < samples; ++i) {
          target[i] += result[i];
        }
      }
    }
  }
  for (int i = 0; i < count; ++i) {
    op_t& dst = *ops[i];
    dst.phase = op[i].phase;
    dst.env = (uint16_t)op[i].env;
    dst.state = (uint8_t)op[i].state;
  }
  ch.feedback[0] = (int16_t)wc.fb0;
  ch.feedback[1] = (int16_t)wc.fb1;
  ch.feedback_in = (int16_t)wc.fb_in;
}

// Ударные: каналы 6..8 вместе — у малого барабана и тарелки общие фазы.
void engine::rhythm_block(int32_t* const* out, uint32_t clocks, const uint8_t* outflag) {
  channel_t* chs[3] = {&ch_[6], &ch_[7], &ch_[8]};
  op_t* ops[3][2];
  for (int c = 0; c < 3; ++c) {
    ops[c][0] = &op_[chs[c]->op[0]];
    ops[c][1] = &op_[chs[c]->op[1]];
  }
  const op_t& op13 = op_[13];
  const op_t& op17 = op_[17];
  const bool opl3 = revision_ >= 3;
  const uint32_t rshift = opl3 ? 0 : 1;
  const int32_t clipmax = 32767;
  const bool modulator_delay = !opl3;
  (void)modulator_delay;
  uint32_t outs[3];
  bool active[3];
  for (int c = 0; c < 3; ++c) {
    const uint32_t choffs = chs[c]->choffs;
    outs[c] = !opl3 ? 1 : (newflag() ? bits(regs_[0xc0 + choffs], 4, 4) : 3);
    active[c] = ((active_ >> (6 + c)) & 1) != 0;
  }
  const uint32_t feedback6 = bits(regs_[0xc0 + 6], 1, 3);
  const uint32_t algorithm6 = bits(regs_[0xc0 + 6], 0);
  const uint32_t base = env_counter_ >> 2;
  uint32_t sample = 0;
  for (uint32_t k = 0; k < clocks; ++k) {
    const uint32_t counter = (base + k + 1) & 0x3fffffff;
    const int32_t pm = pm_[k];
    for (int c = 0; c < 3; ++c) {
      chs[c]->feedback[0] = chs[c]->feedback[1];
      chs[c]->feedback[1] = chs[c]->feedback_in;
      for (int i = 0; i < 2; ++i) {
        op_t& op = *ops[c][i];
        clock_envelope(op, counter);
        op.phase += op.pm ? phase_step(op.block_freq, op.multiple, pm) : op.phase_step;
      }
    }
    if (outflag != nullptr && !outflag[k]) {
      continue;
    }
    const uint32_t am = am_[k];
    const uint32_t noise = noise_[k];
    const uint32_t p13 = op13.phase >> 10;
    const uint32_t p17 = op17.phase >> 10;
    const uint32_t phase_select = (bits(p13, 2) ^ bits(p13, 7)) | bits(p13, 3) | (bits(p17, 5) ^ bits(p17, 3));
    if (active[0]) {
      // Большой барабан (output_rhythm_ch6).
      channel_t& ch = *chs[0];
      int32_t opmod = 0;
      if (feedback6 != 0) {
        opmod = (ch.feedback[0] + ch.feedback[1]) >> (10 - feedback6);
      }
      const int32_t opout1 = compute_volume(*ops[0][0], (ops[0][0]->phase >> 10) + (uint32_t)opmod, am);
      ch.feedback_in = (int16_t)opout1;
      opmod = algorithm6 ? 0 : (opout1 >> 1);
      const int32_t result = compute_volume(*ops[0][1], (ops[0][1]->phase >> 10) + (uint32_t)opmod, am) >> rshift;
      const uint32_t outputs = outs[0];
      ADD_TO_OUTPUT(result * 2);
    }
    if (active[1]) {
      // Хай-хэт и малый барабан (output_rhythm_ch7).
      uint32_t phase = (phase_select << 9) | (0xd0 >> (2 * (noise ^ phase_select)));
      int32_t result = compute_volume(*ops[1][0], phase, am) >> rshift;
      const uint32_t p = ops[1][0]->phase >> 10;
      phase = (0x100u << bits(p, 8)) ^ (noise << 8);
      result += compute_volume(*ops[1][1], phase, am) >> rshift;
      result = clamp(result, -clipmax - 1, clipmax);
      const uint32_t outputs = outs[1];
      ADD_TO_OUTPUT(result * 2);
    }
    if (active[2]) {
      // Том и тарелка (output_rhythm_ch8).
      int32_t result = compute_volume(*ops[2][0], ops[2][0]->phase >> 10, am) >> rshift;
      const uint32_t phase = 0x100 | (phase_select << 9);
      result += compute_volume(*ops[2][1], phase, am) >> rshift;
      result = clamp(result, -clipmax - 1, clipmax);
      const uint32_t outputs = outs[2];
      ADD_TO_OUTPUT(result * 2);
    }
    ++sample;
  }
}

#undef ADD_TO_OUTPUT

void engine::generate(int32_t* const* out, uint32_t count, const uint8_t* extra) {
  const uint32_t outputs = revision_ >= 3 ? 4 : 1;
  for (uint32_t o = 0; o < outputs; ++o) {
    memset(out[o], 0, count * sizeof(int32_t));
  }
  uint32_t sample = 0;
  bool extra_done = false;          // OPL4: лишний такт этого отсчёта уже был
  uint8_t outflag[BLOCK];
  while (sample < count) {
    // Подготовка перед первым тактом блока (fm_engine_base::clock).
    if (modified_) {
      prepare();
      modified_ = false;
      prepare_count_ = 0;
    } else if (prepare_count_ >= 4096) {
      prepare();
      prepare_count_ = 0;
    } else {
      prepare_count_++;
    }
    uint32_t limit = 4097 - prepare_count_;
    if (limit > BLOCK) {
      limit = BLOCK;
    }
    uint32_t clocks = 0;
    uint32_t samples = 0;
    while (clocks < limit && sample + samples < count) {
      if (extra != nullptr && extra[sample + samples] && !extra_done) {
        outflag[clocks++] = 0;
        extra_done = true;
        continue;
      }
      outflag[clocks++] = 1;
      extra_done = false;
      ++samples;
    }
    prepare_count_ += clocks - 1;
    lfo_block(clocks);
    int32_t* block_out[4] = {out[0] + sample, outputs > 1 ? out[1] + sample : nullptr,
                             outputs > 1 ? out[2] + sample : nullptr, outputs > 1 ? out[3] + sample : nullptr};
    const uint8_t* flags = extra != nullptr ? outflag : nullptr;
    const bool rhythm = bits(regs_[0xbd], 5) != 0;
    for (uint32_t c = 0; c < channels_; ++c) {
      if (rhythm && c >= 6 && c <= 8) {
        if (c == 6) {
          rhythm_block(block_out, clocks, flags);
        }
        continue;
      }
      channel_block(c, block_out, clocks, flags);
    }
    env_counter_ += 4 * clocks;
    sample += samples;
  }
  if (revision_ <= 2) {
    for (uint32_t i = 0; i < count; ++i) {
      out[0][i] = roundtrip_fp(out[0][i]);
    }
  } else if (revision_ == 3) {
    for (uint32_t o = 0; o < 4; ++o) {
      for (uint32_t i = 0; i < count; ++i) {
        out[o][i] = clamp(out[o][i], -32768, 32767);
      }
    }
  }
}

uint32_t engine::channel_attenuation(uint32_t channel) const {
  if (channel >= channels_) {
    return 0x3ff;
  }
  const channel_t& ch = ch_[channel];
  if (ch.op[0] == 0xff) {
    return 0x3ff;
  }
  const bool four = ch.op[2] != 0xff;
  uint32_t algorithm = bits(regs_[0xc0 + ch.choffs], 0);
  if (revision_ >= 3) {
    algorithm |= bits(regs_[0xc3 + ch.choffs], 0) << 1;
  }
  uint32_t carriers;
  if (bits(regs_[0xbd], 5) && channel >= 6 && channel <= 8) {
    carriers = 3;                                   // ударные: звучат оба
  } else if (four) {
    static const uint8_t masks[4] = {0x8, 0x9, 0xA, 0xD};
    carriers = masks[algorithm & 3];
  } else {
    carriers = (algorithm & 1) ? 3 : 2;
  }
  uint32_t best = 0x3ff;
  for (int i = 0; i < 4; ++i) {
    if (ch.op[i] == 0xff || ((carriers >> i) & 1) == 0) {
      continue;
    }
    const op_t& op = op_[ch.op[i]];
    const uint32_t attenuation = op.env + op.total_level;
    best = attenuation < best ? attenuation : best;
  }
  return best;
}

// --- таблицы волн OPL4 (ymfm_pcm.cpp) ---------------------------------------

namespace {

enum { EG_REVERB = 5 };
const uint32_t PCM_EG_QUIET = 0x200;
const uint8_t KEY_ON = 0x01;
const uint8_t KEY_PENDING_ON = 0x02;
const uint8_t KEY_PENDING = 0x04;

inline uint32_t pcm_rate(uint32_t raw, int32_t correction) {
  if (raw == 0) {
    return 0;
  }
  if (raw == 15) {
    return 63;
  }
  return (uint32_t)clamp((int32_t)raw * 4 + correction, 0, 63);
}

inline uint32_t pcm_volume(uint32_t attenuation) {
  return power_table[attenuation & 0xff] >> (attenuation >> 8);
}

}  // namespace

pcm::pcm(const pcm_memory* memory) : memory_(memory), env_counter_(0), modified_(true), prepare_count_(0) {
  memset(regs_, 0, sizeof(regs_));
  memset(ch_, 0, sizeof(ch_));
  for (channel_t& ch : ch_) {
    ch.state = EG_RELEASE;
    ch.env = 0x3ff;
    ch.total_level = 0x7f << 10;
  }
}

void pcm::reset() {
  // Как pcm_engine::reset: регистры и каналы; счётчик огибающих и признак
  // изменений — как были.
  memset(regs_, 0, sizeof(regs_));
  regs_[0xf8] = 0x1b;
  for (channel_t& ch : ch_) {
    ch.baseaddr = 0;
    ch.endpos = 0;
    ch.looppos = 0;
    ch.curpos = 0;
    ch.nextpos = 0;
    ch.lfo_counter = 0;
    ch.state = EG_RELEASE;
    ch.env = 0x3ff;
    ch.total_level = 0x7f << 10;
    ch.format = 0;
    ch.key_state = 0;
  }
}

uint8_t pcm::read(uint32_t address) const {
  address &= 0x3fffff;
  if (address < 0x200000) {
    return address < memory_->rom_size ? memory_->rom[address] : 0;
  }
  address -= 0x200000;
  return address < memory_->ram_size ? memory_->ram[address] : 0;
}

void pcm::cache_channel(uint32_t c) {
  channel_t& ch = ch_[c];
  static const uint8_t lfo_steps[8] = {1, 12, 19, 25, 31, 35, 37, 42};
  static const uint8_t am_depths[8] = {0, 0x14, 0x20, 0x28, 0x30, 0x40, 0x50, 0x80};
  static const uint8_t pm_depths[8] = {0, 2, 3, 4, 6, 12, 24, 48};
  const int32_t octave = (int8_t)(bits(regs_[0x38 + c], 4, 4) << 4) >> 4;
  const uint32_t fnum = bits(regs_[0x20 + c], 1, 7) | (bits(regs_[0x38 + c], 0, 3) << 7);
  ch.step = ((0x400 | fnum) << ((octave + 7) & 31)) >> 2;
  ch.target_level = bits(regs_[0x50 + c], 1, 7) << 10;
  const int32_t panpot = (int8_t)(bits(regs_[0x68 + c], 0, 4) << 4) >> 4;
  if (panpot >= 0) {
    ch.pan_left = panpot == 7 ? 0x3ff : 0x20 * (uint32_t)panpot;
    ch.pan_right = 0;
  } else if (panpot >= -7) {
    ch.pan_left = 0;
    ch.pan_right = panpot == -7 ? 0x3ff : (uint32_t)(-0x20 * panpot);
  } else {
    ch.pan_left = ch.pan_right = 0x3ff;
  }
  ch.lfo_step = lfo_steps[bits(regs_[0x80 + c], 3, 3)];
  ch.am_depth = am_depths[bits(regs_[0xe0 + c], 0, 3)];
  ch.pm_depth = pm_depths[bits(regs_[0x80 + c], 0, 3)];
  uint32_t sustain = bits(regs_[0xb0 + c], 4, 4);
  sustain |= (sustain + 1) & 0x10;
  ch.eg_sustain = sustain << 5;
  int32_t correction = (int32_t)bits(regs_[0xc8 + c], 4, 4);
  correction = correction == 15 ? 0 : (octave + correction) * 2 + (int32_t)bits(fnum, 9);
  ch.eg_rate[EG_ATTACK] = (uint8_t)pcm_rate(bits(regs_[0x98 + c], 4, 4), correction);
  ch.eg_rate[EG_DECAY] = (uint8_t)pcm_rate(bits(regs_[0x98 + c], 0, 4), correction);
  ch.eg_rate[EG_SUSTAIN] = (uint8_t)pcm_rate(bits(regs_[0xb0 + c], 0, 4), correction);
  ch.eg_rate[EG_RELEASE] = (uint8_t)pcm_rate(bits(regs_[0xc8 + c], 0, 4), correction);
  ch.eg_rate[EG_REVERB] = 5;
  if (bits(regs_[0x68 + c], 6) != 0) {
    // Демпфер: спад 48 до −12 дБ, дальше — самым быстрым темпом.
    ch.eg_rate[EG_DECAY] = 48;
    ch.eg_rate[EG_SUSTAIN] = 63;
    ch.eg_rate[EG_RELEASE] = 63;
    ch.eg_sustain = 0x80;
  }
}

void pcm::prepare() {
  for (uint32_t c = 0; c < 24; ++c) {
    channel_t& ch = ch_[c];
    cache_channel(c);
    if ((ch.key_state & KEY_PENDING) != 0) {
      const uint8_t old = ch.key_state;
      ch.key_state = (uint8_t)((ch.key_state >> 1) & KEY_ON);
      if (((old ^ ch.key_state) & KEY_ON) != 0) {
        if ((ch.key_state & KEY_ON) != 0) {
          if (ch.state != EG_ATTACK) {        // start_attack
            ch.state = EG_ATTACK;
            if (bits(regs_[0x68 + c], 5) != 0) {
              ch.lfo_counter = 0;
            }
            if (ch.eg_rate[EG_ATTACK] == 63) {
              ch.env = 0;
            }
            ch.curpos = ch.nextpos = 0;
          }
        } else if (ch.state < EG_RELEASE) {   // start_release
          ch.state = EG_RELEASE;
        }
      }
    }
    if (bits(regs_[0x50 + c], 0) != 0) {
      ch.total_level = ch.target_level;       // уровень сразу, без плавного хода
    }
  }
}

void pcm::load_wavetable(uint32_t c) {
  channel_t& ch = ch_[c];
  const uint32_t wavnum = regs_[0x08 + c] | (bits(regs_[0x20 + c], 0) << 8);
  uint32_t header = 12 * wavnum;
  if (wavnum >= 384) {
    const uint32_t bank = bits(regs_[0x02], 2, 3);
    if (bank != 0) {
      header = 512 * 1024 * bank + (wavnum - 384) * 12;
    }
  }
  const uint8_t first = read(header + 0);
  ch.format = (uint8_t)bits(first, 6, 2);
  ch.baseaddr = (bits(first, 0, 6) << 16) | (read(header + 1) << 8) | read(header + 2);
  ch.looppos = ((read(header + 3) << 8) | read(header + 4)) << 16;
  const uint32_t end = (read(header + 5) << 8) | read(header + 6);
  ch.endpos = (uint32_t)(-(int32_t)end) << 16;
  write(0x80 + c, read(header + 7));
  write(0x98 + c, read(header + 8));
  write(0xb0 + c, read(header + 9));
  write(0xc8 + c, read(header + 10));
  write(0xe0 + c, read(header + 11));
  ch.env = 0x3ff;
}

void pcm::write(uint32_t index, uint8_t data) {
  index &= 0xff;
  if (index == 0x06 && bits(regs_[0x02], 0) != 0) {
    // Запись памяти через порт: адрес в регистрах 3..5, сам растёт.
    const uint32_t address = (bits(regs_[0x03], 0, 6) << 16) | (regs_[0x04] << 8) | regs_[0x05];
    const uint32_t next = address + 1;
    regs_[0x05] = (uint8_t)next;
    regs_[0x04] = (uint8_t)(next >> 8);
    regs_[0x03] = (uint8_t)((next >> 16) & 0x3f);
    if (memory_->write != nullptr) {
      memory_->write(memory_->context, address, data);
    }
    return;
  }
  modified_ = true;
  regs_[index] = data;
  if (index >= 0x68 && index <= 0x7f) {
    channel_t& ch = ch_[index - 0x68];
    ch.key_state |= (uint8_t)(KEY_PENDING | (bits(data, 7) ? KEY_PENDING_ON : 0));
  } else if (index >= 0x08 && index <= 0x1f) {
    load_wavetable(index - 0x08);
  }
}

void pcm::channel_block(uint32_t c, int32_t* const* out, uint32_t count) {
  channel_t& ch = ch_[c];
  if (ch.state >= EG_RELEASE && ch.env == 0x3ff) {
    // Молчит до следующей атаки (она сбросит позицию): идут только LFO и
    // плавный уровень — их значения атака не сбрасывает.
    ch.lfo_counter += ch.lfo_step * count;
    if (ch.total_level < ch.target_level) {
      const uint32_t level = ch.total_level + 19 * count;
      ch.total_level = level < ch.target_level ? level : ch.target_level;
    } else if (ch.total_level > ch.target_level) {
      const uint32_t down = 38 * count;
      ch.total_level = ch.total_level > ch.target_level + down ? ch.total_level - down : ch.target_level;
    }
    return;
  }
  const bool reverb = bits(regs_[0x38 + c], 3) != 0;
  const uint32_t outnum = bits(regs_[0x68 + c], 4) * 2;
  int32_t* out_left = out[outnum];
  int32_t* out_right = out[outnum + 1];
  uint32_t lfo = ch.lfo_counter;
  uint32_t env = ch.env;
  uint32_t state = ch.state;
  uint32_t level = ch.total_level;
  uint32_t curpos = ch.curpos;
  uint32_t nextpos = ch.nextpos;
  const uint32_t target = ch.target_level;
  for (uint32_t k = 0; k < count; ++k) {
    lfo += ch.lfo_step;
    // Огибающая (pcm_channel::clock_envelope).
    if (state == EG_ATTACK && env == 0) {
      state = EG_DECAY;
    }
    if (state == EG_DECAY && env >= ch.eg_sustain) {
      state = EG_SUSTAIN;
    }
    const uint32_t rate = ch.eg_rate[state];
    const uint32_t rate_shift = rate >> 2;
    const uint32_t counter = ((env_counter_ + k + 1) >> 1) << rate_shift;
    if ((counter & 0x7ff) == 0) {
      const uint32_t relevant = bits(counter, rate_shift <= 11 ? 11 : (int)rate_shift, 3);
      const uint32_t increment = bits(increment_table[rate], 4 * (int)relevant, 4);
      if (state == EG_ATTACK) {
        env = (uint16_t)(env + ((uint32_t)~env * increment >> 4));
      } else {
        env += increment;
        if (env >= 0x400) {
          env = 0x3ff;
        }
        if (env >= 0xc0 && state < EG_REVERB && reverb) {
          state = EG_REVERB;
        }
      }
    }
    // Шаг с вибрато и позиция с петлёй.
    uint32_t step = ch.step;
    if (ch.pm_depth != 0) {
      const uint32_t shifted = lfo + (1 << 16);
      int32_t value = (int32_t)bits(shifted, 10, 7);
      if (bits(shifted, 17) != 0) {
        value ^= 0x7f;
      }
      value -= 0x40;
      step += (uint32_t)((value * (int32_t)ch.pm_depth) >> 7);
    }
    curpos = nextpos;
    nextpos = curpos + step;
    if (nextpos >= ch.endpos) {
      nextpos += ch.looppos - ch.endpos;
    }
    if (level != target) {
      if (level < target) {
        level = level + 19 < target ? level + 19 : target;
      } else {
        level = level > target + 38 ? level - 38 : target;
      }
    }
    // Выход (pcm_channel::output).
    if (env > PCM_EG_QUIET) {
      continue;
    }
    uint32_t envelope = env;
    if (ch.am_depth != 0) {
      uint32_t value = bits(lfo, 10, 7);
      if (bits(lfo, 17) != 0) {
        value ^= 0x7f;
      }
      envelope += (value * ch.am_depth) >> 7;
    }
    envelope += level >> 8;
    const uint32_t lenv = envelope + ch.pan_left < 0x3ff ? envelope + ch.pan_left : 0x3ff;
    const uint32_t renv = envelope + ch.pan_right < 0x3ff ? envelope + ch.pan_right : 0x3ff;
    const int32_t lvol = (int32_t)pcm_volume(lenv << 2);
    const int32_t rvol = (int32_t)pcm_volume(renv << 2);
    const uint32_t pos = curpos >> 16;
    int16_t sample;
    if (ch.format == 0) {
      sample = (int16_t)(read(ch.baseaddr + pos) << 8);
    } else if (ch.format == 2) {
      const uint32_t address = ch.baseaddr + pos * 2;
      sample = (int16_t)((read(address) << 8) | read(address + 1));
    } else {
      const uint32_t address = ch.baseaddr + (pos / 2) * 3;
      if ((pos & 1) == 0) {
        sample = (int16_t)((read(address) << 8) | ((read(address + 1) << 4) & 0xf0));
      } else {
        sample = (int16_t)((read(address + 2) << 8) | (read(address + 1) & 0xf0));
      }
    }
    out_left[k] += (lvol * sample) >> 15;
    out_right[k] += (rvol * sample) >> 15;
  }
  ch.lfo_counter = lfo;
  ch.env = (uint16_t)env;
  ch.state = (uint8_t)state;
  ch.total_level = level;
  ch.curpos = curpos;
  ch.nextpos = nextpos;
}

void pcm::generate(int32_t* const* out, uint32_t count) {
  for (int o = 0; o < 4; ++o) {
    memset(out[o], 0, count * sizeof(int32_t));
  }
  uint32_t done = 0;
  while (done < count) {
    // Подготовка перед первым тактом блока (pcm_engine::clock).
    if (modified_) {
      prepare();
      modified_ = false;
      prepare_count_ = 0;
    } else if (prepare_count_ >= 4096) {
      prepare();
      prepare_count_ = 0;
    } else {
      prepare_count_++;
    }
    uint32_t n = 4097 - prepare_count_;
    if (n > count - done) {
      n = count - done;
    }
    if (n > 64) {
      n = 64;
    }
    prepare_count_ += n - 1;
    int32_t* block[4] = {out[0] + done, out[1] + done, out[2] + done, out[3] + done};
    for (uint32_t c = 0; c < 24; ++c) {
      channel_block(c, block, n);
    }
    env_counter_ += n;
    done += n;
  }
}

uint32_t pcm::channel_attenuation(uint32_t channel) const {
  if (channel >= 24) {
    return 0x3ff;
  }
  return ch_[channel].env + (ch_[channel].total_level >> 8);
}

}  // namespace fast_opl
