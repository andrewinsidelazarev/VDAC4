// TurboFM compiled command stream. Based on the ZXTune TFC format decoder
// by vitamin.caig@gmail.com; SPDX-License-Identifier: LGPL-3.0-only
#pragma once
#include "zm_source.h"
namespace tsfm {
using FmWrite=void(*)(void*,unsigned,unsigned,unsigned);
struct TfcChannel {
  uint32_t cursor,ret,repeat,skip,tick;
  uint16_t frequency;
};
struct Tfc {
  const uint8_t* data;
  size_t size;
  TfcChannel channel[6],loop[6];
  uint32_t lengths[6],ticks;
  uint8_t rate;
  bool error,scanning;
  char title[64],author[64];
  bool open(const uint8_t*,size_t,const zm_host_t*);
  void reset();
  bool tick(FmWrite,void*);
private:
  unsigned byte(uint32_t&);
  unsigned be16(uint32_t&);
  void freq(unsigned,unsigned,FmWrite,void*);
  uint32_t frame_data(unsigned,uint32_t,FmWrite,void*);
  int next(unsigned,FmWrite,void*);
};
}
