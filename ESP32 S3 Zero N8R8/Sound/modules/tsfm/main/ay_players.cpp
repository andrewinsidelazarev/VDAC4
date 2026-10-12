// AYFly native replayers adapted to the VDAC source ABI.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "ay_players.h"
#include "zm_app.h"
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

namespace {
struct Text {
  char text[64];
  Text& operator=(const char* src) {
    unsigned n=0; for (; n<63 && src[n]; ++n) text[n]=src[n];
    text[n]=0; return *this;
  }
};
struct AYSongInfo {
  uint8_t *module, *module1, *file_data;
  unsigned long module_len, file_len, Length, Loop;
  void *data, *data1;
  bool is_ts;
  Text Name, Author;
  uint8_t regs[2][14];
  uint16_t masks[2];
};
struct Guard {
  jmp_buf jump;
  uint8_t* begin;
  size_t size;
  uint32_t left;
  const zm_host_t* host;
};
// One task owns all decoder calls (zm_source.h). Never survives a call.
Guard* guard;
void fail() { longjmp(guard->jump,1); }
void fuel() { if (!guard->left--) fail(); }
void range(const void* p, size_t n) {
  fuel();
  const uintptr_t offset=reinterpret_cast<uintptr_t>(p)-reinterpret_cast<uintptr_t>(guard->begin);
  if (offset>guard->size || n>guard->size-offset) fail();
}
uint8_t& memory(uint8_t* p, int64_t index) {
  if (index<0 || uint64_t(index)>65535) fail();
  range(p+index,1); return p[index];
}
template<class T> T& field(T* p,int64_t index){
  if(index<0 || index>65535)fail();range(p+index,sizeof(T));return p[index];
}
template<class T, size_t N> T& element(T (&a)[N], int index) {
  if (index<0 || unsigned(index)>=N) fail(); return a[index];
}
template<class T> T* allocate() {
  T* p=static_cast<T*>(guard->host->alloc(sizeof(T),ZM_MEM_PSRAM));
  if (!p) fail(); memset(p,0,sizeof(T)); return p;
}
template<class T> void release(T* p) { if (p) guard->host->free(p); }
unsigned short ay_sys_getword(uint8_t* p) { range(p,2); return p[0]|(p[1]<<8); }
void ay_sys_writeword(uint8_t* p, unsigned short v) { range(p,2);p[0]=v;p[1]=v>>8; }
Text ay_sys_getstr(const uint8_t* p,unsigned long n) {
  range(p,n);Text t={};if(n>63)n=63;memcpy(t.text,p,n);
  while(n && static_cast<uint8_t>(t.text[n-1])<=32)t.text[--n]=0;
  return t;
}
enum {AY_CHNL_A_FINE,AY_CHNL_A_COARSE,AY_CHNL_B_FINE,AY_CHNL_B_COARSE,
      AY_CHNL_C_FINE,AY_CHNL_C_COARSE,AY_NOISE_PERIOD,AY_MIXER,
      AY_CHNL_A_VOL,AY_CHNL_B_VOL,AY_CHNL_C_VOL,AY_ENV_FINE,AY_ENV_COARSE,AY_ENV_SHAPE};
void ay_writeay(AYSongInfo* s,unsigned reg,unsigned value,unsigned chip=0) {
  if (chip>=2 || reg>=14) fail();s->regs[chip][reg]=value;s->masks[chip]|=1u<<reg;
}
unsigned char ay_readay(AYSongInfo* s,unsigned reg,unsigned chip=0) {
  if(chip>=2 || reg>=14)fail();return s->regs[chip][reg];
}
void ay_resetay(AYSongInfo* s,unsigned chip) {
  memset(s->regs[chip],0,14);s->regs[chip][7]=0x3f;s->masks[chip]=0x3fff;
}
#include "ayfly/ASCPlay.h"
#include "ayfly/PT1Play.h"
#include "ayfly/PT2Play.h"
#include "ayfly/PT3Play.h"
#include "ayfly/STCPlay.h"
#include "ayfly/STPPlay.h"
#include "ayfly/PSCPlay.h"
#include "ayfly/SQTPlay.h"
#include "extra_players.h"
#undef PT3
#undef PT3_A
#undef PT3_B
#undef PT3_C
struct PlayerOps {
  const char* name;
  void (*init)(AYSongInfo&);
  void (*play)(AYSongInfo&);
  void (*cleanup)(AYSongInfo&);
  void (*info)(AYSongInfo&);
  bool (*detect)(uint8_t*,unsigned long);
};
#define OPS(n) {#n,n##_Init,n##_Play,n##_Cleanup,n##_GetInfo,n##_Detect}
const PlayerOps players[]={OPS(ASC),OPS(PT1),OPS(PT2),OPS(PT3),OPS(STC),OPS(STP),OPS(PSC),OPS(SQT),OPS(GTR),OPS(ST3),OPS(PSM),OPS(FTC)};
#undef OPS
}
namespace tsfm {
struct AyPlayer {
  AYSongInfo song;
  Guard guard;
  const PlayerOps* ops;
  bool internal_ts;
};
void ay_close(AyPlayer* p) {
  if (!p) return;
  const zm_host_t* host=p->guard.host;
  if (p->song.data)host->free(p->song.data);
  if (p->song.data1)host->free(p->song.data1);
  host->free(p->song.module);host->free(p);
}
bool ay_reset(AyPlayer* p) {
  guard=&p->guard;guard->left=1000000;
  if(setjmp(guard->jump)) {guard=nullptr;return false;}
  p->ops->init(p->song);
  guard=nullptr;return p->song.data!=nullptr;
}
AyPlayer* ay_open(const uint8_t* data,size_t size,const char* type,const zm_host_t* host) {
  if (!data || size<16 || size>65535) return nullptr;
  AyPlayer* p=static_cast<AyPlayer*>(host->alloc(sizeof(AyPlayer),ZM_MEM_PSRAM));
  if(!p)return nullptr;memset(p,0,sizeof(*p));p->guard.host=host;
  const bool dual=!strcmp(type,"PT3") && size>=202 && data[98]!=32 && data[98]>0 && data[98]<=85;
  p->internal_ts=dual;
  const size_t capacity=dual?131072:65536;
  p->song.module=static_cast<uint8_t*>(host->alloc(capacity,ZM_MEM_PSRAM));
  if(!p->song.module){ay_close(p);return nullptr;}
  memset(p->song.module,0,capacity);memcpy(p->song.module,data,size);
  if(dual){
    if(201u+data[101]>size){ay_close(p);return nullptr;}
    memcpy(p->song.module+size,data,size);p->song.module1=p->song.module+size;
    for(unsigned pos=0;pos<data[101];++pos){
      int pattern=int(data[98])*3-3-data[201+pos];
      if(pattern<0 || pattern>=255){ay_close(p);return nullptr;}
      p->song.module[size+201+pos]=pattern;
    }
    p->song.module[98]=p->song.module[size+98]=32;size*=2;
  }
  p->song.file_data=p->song.module;p->song.module_len=p->song.file_len=size;
  p->guard.begin=p->song.module;p->guard.size=size;
  guard=&p->guard;guard->left=10000000;
  if(setjmp(guard->jump)){guard=nullptr;ay_close(p);return nullptr;}
  for(const auto& ops:players)if(!strcmp(type,ops.name)||(!strcmp(type,"AS0")&&!strcmp(ops.name,"ASC")))p->ops=&ops;
  if(!p->ops){guard=nullptr;ay_close(p);return nullptr;}
  const bool asc0=!strcmp(type,"AS0");
  if(!asc0 && !p->ops->detect(p->song.module,size)){guard=nullptr;ay_close(p);return nullptr;}
  // ASC v0 has no loop byte. All data offsets are relative to their own
  // tables, so inserting the missing header byte only moves the three tables.
  if(p->ops==&players[0] && (asc0 || !ASC1_Detect(p->song.module,size))) {
    if(size>=65535)fail();
    memmove(p->song.module+2,p->song.module+1,size-1);p->song.module[1]=0;
    ++size;p->guard.size=size;p->song.module_len=p->song.file_len=size;
    for(unsigned i=2;i<8;i+=2)ay_sys_writeword(p->song.module+i,ay_sys_getword(p->song.module+i)+1);
  }
  p->ops->info(p->song);
  if(dual)p->song.Length=PT3_GetTime(p->song.module,p->song.Loop);
  if(!p->song.Length || p->song.Length>50*60*60)fail();
  p->ops->init(p->song);
  guard=nullptr;
  if(!p->song.data || (dual && !p->song.data1)){ay_close(p);return nullptr;}
  return p;
}
bool ay_tick(AyPlayer* p,uint8_t regs[2][14],uint16_t masks[2]) {
  guard=&p->guard;guard->left=100000;
  if(setjmp(guard->jump)){guard=nullptr;return false;}
  p->song.masks[0]=p->song.masks[1]=0;
  if(p->internal_ts){
    auto& first=*static_cast<PT3_SongInfo*>(p->song.data);
    auto& second=*static_cast<PT3_SongInfo*>(p->song.data1);
    // PT3 TurboSound stores six channels under one order list and tempo.
    // The second set of patterns does not drive its own song clock.
    const unsigned delay=first.PT3.DelayCounter,position=first.PT3.CurrentPosition;
    PT3_Play_Chip(p->song,0);
    auto* m=p->song.module1;
    if(first.PT3.CurrentPosition!=position){
      second.PT3.CurrentPosition=first.PT3.CurrentPosition;
      unsigned ptr=ay_sys_getword(m+103)+2*memory(m,201+first.PT3.CurrentPosition);
      second.PT3_A.Address_In_Pattern=ay_sys_getword(&memory(m,ptr));
      second.PT3_B.Address_In_Pattern=ay_sys_getword(&memory(m,ptr+2));
      second.PT3_C.Address_In_Pattern=ay_sys_getword(&memory(m,ptr+4));
      second.PT3_A.Note_Skip_Counter=second.PT3_B.Note_Skip_Counter=second.PT3_C.Note_Skip_Counter=1;
      second.PT3.Noise_Base=0;
    }
    second.PT3.Delay=first.PT3.Delay;second.PT3.DelayCounter=delay;
    if(delay==1 && second.PT3_A.Note_Skip_Counter==1 && !memory(m,second.PT3_A.Address_In_Pattern))second.PT3.DelayCounter=2;
    PT3_Play_Chip(p->song,1);second.PT3.DelayCounter=first.PT3.DelayCounter;
  }else p->ops->play(p->song);
  memcpy(regs,p->song.regs,sizeof(p->song.regs));
  memcpy(masks,p->song.masks,sizeof(p->song.masks));guard=nullptr;return true;
}
uint32_t ay_ticks(const AyPlayer* p){return p->song.Length;}
unsigned ay_chips(const AyPlayer* p){return p->song.is_ts?2:1;}
const char* ay_title(const AyPlayer* p){return p->song.Name.text;}
const char* ay_author(const AyPlayer* p){return p->song.Author.text;}
}
