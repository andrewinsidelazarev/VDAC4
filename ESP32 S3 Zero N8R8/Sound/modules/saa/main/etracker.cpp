// E-Tracker format/player adapted from ZXTune by vitamin.caig@gmail.com.
// The VDAC4 port uses bounded arrays and the ELF host allocator instead of STL.
// SPDX-License-Identifier: LGPL-3.0-only
#include "etracker.h"
#include "zm_app.h"
#include <string.h>

namespace etracker {
namespace {
constexpr unsigned MAX_LINES = 4096;
struct View {
  const uint8_t* data;
  size_t size;
  bool valid;
  uint8_t byte(size_t offset) {
    if (offset >= size) { valid = false; return 0; }
    return data[offset];
  }
  uint16_t word(size_t offset) {
    const unsigned lo = byte(offset);
    return static_cast<uint16_t>(lo | (unsigned(byte(offset + 1)) << 8));
  }
  bool pointer(size_t offset) const { return offset >= 30 && offset < size; }
};
struct DecodeTable { uint8_t marker, length[256]; };

bool sample(View& v, size_t cursor, const DecodeTable& table, Sample& out, bool fill) {
  if (!v.pointer(cursor)) return false;
  unsigned count = 0, loop = MAX_LINES, dev_left = 0, vol_left = 0;
  SampleLine line{};
  bool finished = false;
  while (v.valid && !finished) {
    if (!dev_left) {
      dev_left = 1;
      for (unsigned guard = 0;; ++guard) {
        if (guard >= v.size || !v.valid) return false;
        const unsigned value = v.byte(cursor++);
        if (value & 1) {
          line.flags = static_cast<uint8_t>(value & 0xf0);
          line.deviation = static_cast<uint16_t>(((value & 14) << 7) | v.byte(cursor++));
          break;
        }
        if (value == 0xfe) { loop = count; continue; }
        if (value == 0xfc) { finished = true; break; }
        dev_left = 2 + (value >> 1);
      }
      if (finished) break;
    }
    if (!vol_left) {
      const unsigned value = v.byte(cursor++);
      if (value == table.marker) {
        vol_left = v.byte(cursor++);
        line.amplitude = v.byte(cursor++);
      } else if (table.length[value]) {
        vol_left = table.length[value];
        line.amplitude = v.byte(cursor++);
      } else {
        vol_left = 1;
        line.amplitude = static_cast<uint8_t>(value);
      }
      if (!vol_left) return false;
    }
    const unsigned step = dev_left < vol_left ? dev_left : vol_left;
    if (!v.valid || !step || count + step > MAX_LINES) return false;
    if (fill) {
      if (count + step > out.count) return false;
      for (unsigned i = 0; i < step; ++i) out.lines[count + i] = line;
    }
    count += step;
    dev_left -= step;
    vol_left -= step;
  }
  if (!v.valid || !finished) return false;
  if (fill && count != out.count) return false;
  out.count = static_cast<uint16_t>(count);
  out.loop = static_cast<uint16_t>(loop < count ? loop : count);
  return true;
}

bool ornament(View& v, size_t cursor, Ornament& out, bool fill) {
  if (!v.pointer(cursor)) return false;
  unsigned count = 0, loop = MAX_LINES, repeat = 1;
  for (unsigned guard = 0; guard < v.size && v.valid; ++guard) {
    const unsigned value = v.byte(cursor++);
    if (value == 0xff) {
      if (!v.valid || (fill && out.count != count)) return false;
      out.count = static_cast<uint16_t>(count);
      out.loop = static_cast<uint16_t>(loop < count ? loop : count);
      return true;
    }
    if (value == 0xfe) { loop = count; continue; }
    if (value >= 0x60) { repeat = value + 2 - 0x60; continue; }
    if (count + repeat > MAX_LINES || (fill && count + repeat > out.count)) return false;
    if (fill) memset(out.lines + count, value, repeat);
    count += repeat;
    repeat = 1;
  }
  return false;
}

bool pattern(View& v, size_t table, Pattern& out, uint32_t& samples, uint32_t& ornaments) {
  unsigned cursor[6], wait[6]{};
  for (unsigned c = 0; c < 6; ++c) {
    cursor[c] = v.word(table + 2 * c);
    if (!v.pointer(cursor[c])) return false;
  }
  for (unsigned row = 0; row < 64; ++row) {
    Row& dst = out.rows[row];
    dst.tempo = 0;
    for (Event& e : dst.channel) { memset(&e, 0xff, sizeof(e)); e.rest = 0; }
    for (unsigned c = 0; c < 6; ++c) {
      if (!wait[c] && v.byte(cursor[c]) == 0x51) {
        out.count = static_cast<uint8_t>(row ? row : 1);
        return v.valid;
      }
    }
    for (unsigned c = 0; c < 6; ++c) {
      if (wait[c]) { --wait[c]; continue; }
      Event& e = dst.channel[c];
      bool ended = false;
      for (unsigned guard = 0; guard < v.size && v.valid; ++guard) {
        const unsigned cmd = v.byte(cursor[c]++);
        if (cmd >= 0xd2) { wait[c] = cmd - 0xd2; ended = true; break; }
        if (cmd >= 0x72) e.note = static_cast<uint8_t>(cmd - 0x72);
        else if (cmd >= 0x52) { e.sample = static_cast<uint8_t>(cmd - 0x52); samples |= 1u << e.sample; }
        else if (cmd == 0x51) { ended = true; break; }
        else if (cmd == 0x50) e.rest = 1;
        else if (cmd >= 0x30) { e.ornament = static_cast<uint8_t>(cmd - 0x30); ornaments |= 1u << e.ornament; }
        else if (cmd >= 0x2e) e.swap = static_cast<uint8_t>(cmd - 0x2e);
        else if (cmd >= 0x21) e.envelope = static_cast<uint8_t>(cmd - 0x21);
        else if (cmd >= 0x11) e.attenuation = static_cast<uint8_t>(cmd - 0x11);
        else if (cmd >= 0x0f) e.noise = cmd == 0x0f ? 0 : 3;
        else dst.tempo = static_cast<uint8_t>(cmd + 1);
      }
      if (!ended || !v.valid) return false;
    }
    out.count = static_cast<uint8_t>(row + 1);
  }
  return v.valid;
}
} // namespace

void unload(Tune& tune, const zm_host_t* host) {
  for (Pattern& p : tune.patterns) if (p.rows) host->free(p.rows);
  for (Sample& s : tune.samples) if (s.lines) host->free(s.lines);
  for (Ornament& o : tune.ornaments) if (o.lines) host->free(o.lines);
  memset(&tune, 0, sizeof(tune));
}

bool load(Tune& tune, const uint8_t* data, size_t size, const zm_host_t* host) {
  memset(&tune, 0, sizeof(tune));
  if (!data || size < 96 || size > 32768 || memcmp(data + 10, "ETracker", 8)) return false;
  View v{data, size, true};
  const unsigned pos_table=v.word(0), pat_table=v.word(2), sam_table=v.word(4), orn_table=v.word(6), dec_table=v.word(8);
  if (!v.pointer(pos_table) || !v.pointer(pat_table) || !v.pointer(sam_table) ||
      !v.pointer(orn_table) || !v.pointer(dec_table)) return false;
  DecodeTable decode{};
  unsigned cursor=dec_table;
  decode.marker=v.byte(cursor++);
  for (unsigned guard=0;; ++guard) {
    if (!v.valid || guard > 128) return false;
    const unsigned code=v.byte(cursor++);
    if (!code) break;
    const unsigned length=v.byte(cursor++);
    if (!length) return false;
    if (!decode.length[code]) decode.length[code]=static_cast<uint8_t>(length);
  }
  uint32_t used_patterns=0, used_samples=0, used_ornaments=0;
  unsigned transpose=0;
  bool terminated=false;
  for (cursor=pos_table; cursor < size; ++cursor) {
    const unsigned value=v.byte(cursor);
    if (value == 0xff) { terminated=true; break; }
    if (value == 0xfe) tune.loop=tune.orders;
    else if (value >= 0x60) transpose=value-0x60;
    else {
      if (value % 3 || tune.orders >= 255) return false;
      Position& p=tune.positions[tune.orders++];
      p.pattern=static_cast<uint8_t>(value/3);p.transpose=static_cast<uint8_t>(transpose);
      used_patterns |= 1u << p.pattern;
    }
  }
  if (!terminated || !tune.orders || tune.loop >= tune.orders) return false;
  for (unsigned i=0; i<32; ++i) {
    if (used_patterns & (1u << i)) {
      Pattern& p=tune.patterns[i];
      p.rows=static_cast<Row*>(host->alloc(64*sizeof(Row), ZM_MEM_PSRAM));
      if (!p.rows || !pattern(v, pat_table+12*i, p, used_samples, used_ornaments)) return false;
    }
  }
  if (!used_samples) return false;
  for (unsigned i=0; i<32; ++i) {
    if (used_samples & (1u << i)) {
      Sample& s=tune.samples[i];const unsigned offset=v.word(sam_table+2*i);
      if (!sample(v, offset, decode, s, false)) return false;
      if (s.count) {
        s.lines=static_cast<SampleLine*>(host->alloc(s.count*sizeof(SampleLine), ZM_MEM_PSRAM));
        if (!s.lines || !sample(v, offset, decode, s, true)) return false;
        ++tune.sample_count;
      }
    }
    if (used_ornaments & (1u << i)) {
      Ornament& o=tune.ornaments[i];const unsigned offset=v.word(orn_table+2*i);
      if (!ornament(v, offset, o, false)) return false;
      if (o.count) {
        o.lines=static_cast<uint8_t*>(host->alloc(o.count, ZM_MEM_PSRAM));
        if (!o.lines || !ornament(v, offset, o, true)) return false;
      }
    }
  }
  unsigned tempo=6;
  for (unsigned i=0; i<tune.orders; ++i) {
    tune.position_ticks[i]=tune.ticks;
    const Pattern& p=tune.patterns[tune.positions[i].pattern];
    for (unsigned row=0; row<p.count; ++row) {
      if (p.rows[row].tempo) tempo=p.rows[row].tempo;
      tune.ticks += tempo;
    }
  }
  tune.position_ticks[tune.orders]=tune.ticks;
  return v.valid && tune.ticks != 0;
}

void Engine::reset(const Tune* source) {
  memset(this, 0, sizeof(*this));tune=source;tempo=6;
  for (Channel& c : channel) c.sample=c.ornament=0xff;
}

bool Engine::tick(uint8_t regs[32], uint32_t& mask) {
  memset(regs, 0, 32);mask=0;
  if (ticks >= tune->ticks || order >= tune->orders) return false;
  if (!row_left) {
    if (row >= tune->patterns[tune->positions[order].pattern].count) { ++order;row=0; }
    if (order >= tune->orders) return false;
    const Row& src=tune->patterns[tune->positions[order].pattern].rows[row];
    if (src.tempo) tempo=src.tempo;
    row_left=tempo;
    static const uint8_t envelope[]={0,0x96,0x9e,0x9a,0x86,0x8e,0x8a,0x97,0x9f,0x9b,0x87,0x8f,0x8b};
    for (unsigned i=0; i<6; ++i) {
      const Event& e=src.channel[i];Channel& c=channel[i];
      if (e.rest) { c.sample_pos=c.sample<32 ? tune->samples[c.sample].count : 0;c.ornament_pos=0; }
      if (e.note != 0xff) { c.note=e.note;c.sample_pos=c.ornament_pos=0; }
      if (e.sample != 0xff) { c.sample=e.sample;c.sample_pos=c.ornament_pos=0; }
      if (e.ornament != 0xff) { c.ornament=e.ornament;c.ornament_pos=0; }
      if (e.attenuation != 0xff) c.attenuation=e.attenuation;
      if (e.swap != 0xff) c.swap=e.swap;
      if (e.noise != 0xff) noise[i/3]=e.noise;
      if (e.envelope != 0xff) { regs[0x18+i/3]=envelope[e.envelope];mask |= 1u<<(0x18+i/3); }
    }
  }
  static const uint8_t tones[]={5,0x21,0x3c,0x55,0x6d,0x84,0x99,0xad,0xc0,0xd2,0xe3,0xf3};
  for (unsigned i=0; i<6; ++i) {
    Channel& c=channel[i];SampleLine line{};
    if (c.sample<32) {
      const Sample& s=tune->samples[c.sample];
      if (c.sample_pos<s.count) { line=s.lines[c.sample_pos++];if (c.sample_pos==s.count)c.sample_pos=s.loop; }
    }
    unsigned note=c.note;
    if (c.ornament<32) {
      const Ornament& o=tune->ornaments[c.ornament];
      if (c.ornament_pos<o.count) { note+=o.lines[c.ornament_pos++];if(c.ornament_pos==o.count)c.ornament_pos=o.loop; }
    }
    unsigned tone=0x7ff;
    if (note != 0x5f) {
      note+=tune->positions[order].transpose;
      if (note>=0x100)note-=0x60;
      tone=((note/12)<<8)+tones[note%12];
    }
    tone+=line.deviation;
    regs[8+i]=static_cast<uint8_t>(tone);
    regs[0x10+i/2] |= static_cast<uint8_t>(((tone>>8)&7) << ((i&1)*4));
    int left=line.amplitude&15, right=line.amplitude>>4;
    if (c.swap) { const int temp=left;left=right;right=temp; }
    left-=c.attenuation;right-=c.attenuation;
    if (left<0) left=0;
    if (right<0) right=0;
    regs[i]=static_cast<uint8_t>(left|(right<<4));
    levels[i]=static_cast<uint8_t>((left>right?left:right)*17);
    if(line.flags&16)regs[0x14]|=static_cast<uint8_t>(1u<<i);
    if(line.flags&32)regs[0x15]|=static_cast<uint8_t>(1u<<i);
    const unsigned shift=(i/3)*4;
    if((line.flags&32)||i==0)regs[0x16]=static_cast<uint8_t>((regs[0x16]&~(7u<<shift))|((line.flags>>6)<<shift));
    regs[0x16]|=static_cast<uint8_t>(noise[i/3]<<shift);
  }
  mask |= 0x3f | (0x3fu<<8) | (7u<<16) | (7u<<20);
  ++ticks;
  if (!--row_left) ++row;
  return true;
}
} // namespace etracker
