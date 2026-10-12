// SPDX-License-Identifier: LGPL-3.0-only
// TurboFM Dump: семантика кадров — по Formats::Chiptune::TFD и Module::TFD ZXTune.
#include "tfd.h"
#include <string.h>
namespace tsfm {
namespace {
constexpr unsigned BEGIN_FRAME=0xff,SKIP_FRAMES=0xfe,SECOND_CHIP=0xfd,FIRST_CHIP=0xfc,FINISH=0xfb,LOOP_MARKER=0xfa;
// Песня — не длиннее часа (как у остальных форматов модуля).
constexpr uint32_t MAX_FRAMES=50u*60u*60u;
void rest(const zm_host_t* host){
  if(host && host->size>=offsetof(zm_host_t,rest)+sizeof(host->rest) && host->rest)host->rest();
}
}
unsigned Tfd::byte(uint32_t& p){if(p>=size){error=true;return FINISH;}return data[p++];}

// Начало группы границ: FF — одна, FE n — 3+n. Иначе false.
bool Tfd::group(unsigned code,uint32_t& p,uint32_t& count){
  if(code==BEGIN_FRAME){count=1;return true;}
  if(code==SKIP_FRAMES){count=3+byte(p);return true;}
  return false;
}

bool Tfd::open(const uint8_t* d,size_t n,const zm_host_t* host){
  memset(this,0,sizeof(*this));data=d;size=n;
  if(n<8 || n>1048576 || memcmp(d,"TFMD",4))return false;
  uint32_t p=4;
  for(unsigned t=0;t<3;++t){
    const unsigned limit=t==2?384:64;bool end=false;
    for(unsigned i=0;i<=limit;++i){
      const unsigned b=byte(p);if(error)return false;
      if(!b){end=true;break;}
      if(t<2 && i<63)(t?author:title)[i]=char(b);
    }
    if(!end)return false;
  }
  start=p;
  // Проход по потоку: число кадров и место метки повтора.
  uint32_t boundaries=0,frame_start=0;bool loop=false;
  for(uint32_t fuel=1;;++fuel){
    const unsigned b=byte(p);
    if(error)return false;                       // нет FB — файл обрезан
    if(b==FINISH)break;
    uint32_t count=0;
    if(group(b,p,count)){
      if(error)return false;
      boundaries+=count;frame_start=p;
      if(boundaries>MAX_FRAMES+1)return false;
    }else if(b==LOOP_MARKER){
      // Кадр, в котором стоит метка: начало его записей (после группы).
      if(boundaries){loop=true;loop_frame=boundaries-1;loop_cursor=frame_start;}
    }else if(b!=SECOND_CHIP && b!=FIRST_CHIP){
      byte(p);if(error)return false;              // значение регистра
    }
    if(!(fuel&4095))rest(host);
  }
  if(boundaries<2)return false;
  frames=boundaries-1;
  reset();
  if(error)return false;
  if(!loop){loop_frame=0;loop_cursor=cursor;loop_empty=empty;}
  else loop_empty=0;
  return true;
}

void Tfd::reset(){
  cursor=start;frame=0;empty=0;error=false;
  // Записи до первой границы не звучат; первая группа открывает кадр 0, а её
  // остальные границы — пустые кадры.
  for(;;){
    const unsigned b=byte(cursor);
    if(error || b==FINISH){error=true;return;}
    uint32_t count=0;
    if(group(b,cursor,count)){empty=count-1;return;}
    if(b!=SECOND_CHIP && b!=FIRST_CHIP && b!=LOOP_MARKER)byte(cursor);
  }
}

bool Tfd::tick(FmWrite write,void* user){
  if(frame>=frames){cursor=loop_cursor;empty=loop_empty;frame=loop_frame;}
  if(empty){--empty;++frame;return true;}
  unsigned chip=0;
  uint32_t count=1;
  for(uint32_t fuel=0;fuel<65536;++fuel){
    const unsigned b=byte(cursor);
    if(error)return false;
    if(b==FINISH){--cursor;break;}                // последний кадр без границы
    if(group(b,cursor,count))break;
    if(b==SECOND_CHIP){chip=1;continue;}
    if(b==FIRST_CHIP){chip=0;continue;}
    if(b==LOOP_MARKER)continue;
    const unsigned value=byte(cursor);
    if(error)return false;
    if(write)write(user,chip,b,value);
  }
  empty=count-1;++frame;
  return !error;
}
}
