// SPDX-License-Identifier: LGPL-3.0-only
#include "tfc.h"
#include <string.h>
namespace tsfm {
unsigned Tfc::byte(uint32_t& p){if(p>=size){error=true;return 0;}return data[p++];}
unsigned Tfc::be16(uint32_t& p){unsigned hi=byte(p);return (hi<<8)|byte(p);}
void Tfc::freq(unsigned c,unsigned f,FmWrite write,void* user){
  channel[c].frequency=f;
  if(write){write(user,c/3,0xa4+c%3,f>>8);write(user,c/3,0xa0+c%3,f&255);}
}
uint32_t Tfc::frame_data(unsigned c,uint32_t p,FmWrite write,void* user){
  const unsigned cmd=byte(p);
  if((cmd&0xc0)&&write)write(user,c/3,0x28,c%3);
  if(cmd&1)freq(c,be16(p),write,user);
  for(unsigned i=0;i<((cmd&0x3e)>>1);++i){
    unsigned reg=byte(p),val=byte(p);
    if(write)write(user,c/3,reg,val);
  }
  if((cmd&0x80)&&write)write(user,c/3,0x28,0xf0|(c%3));
  return p;
}
int Tfc::next(unsigned c,FmWrite write,void* user){
  TfcChannel& s=channel[c];
  if(s.skip){--s.skip;++s.tick;return 1;}
  if(s.repeat && !--s.repeat){s.cursor=s.ret;s.ret=0;}
  uint32_t p=s.cursor;
  for(unsigned fuel=0;fuel<1024 && !error;++fuel){
    unsigned cmd=byte(p);
    if(cmd==0x7f)return 0;
    if(cmd==0x7e){
      if(scanning){loop[c]=s;loop[c].cursor=p;}
      continue;
    }
    if(cmd==0xd0){
      if(s.ret || s.repeat){error=true;return -1;}
      s.repeat=byte(p);
      int offset=static_cast<int16_t>(be16(p));s.ret=p;
      if(!s.repeat || int64_t(p)+offset<0){error=true;return -1;}
      p+=offset;cmd=byte(p);
    }
    if(cmd==0xbf || cmd==0xff){
      const int offset=cmd==0xbf?static_cast<int16_t>(be16(p)):-256+int(byte(p));
      if(int64_t(p)+offset<0){error=true;return -1;}
      frame_data(c,p+offset,write,user);
    }else if(cmd>=0xe0){s.skip=255-cmd;}
    else if(cmd>=0xc0){freq(c,(s.frequency&0xff00)|((s.frequency+cmd+0x30)&255),write,user);}
    else p=frame_data(c,p-1,write,user);
    s.cursor=p;++s.tick;return error?-1:1;
  }
  error=true;return -1;
}
void Tfc::reset(){
  memset(channel,0,sizeof(channel));error=false;
  for(unsigned i=0;i<6;++i)channel[i].cursor=data[10+2*i]|(data[11+2*i]<<8);
}
bool Tfc::open(const uint8_t* d,size_t n,const zm_host_t* host){
  memset(this,0,sizeof(*this));data=d;size=n;
  if(n<43 || n>65535 || memcmp(d,"TFMcom",6) || (d[9]!=50 && d[9]!=60))return false;
  rate=d[9];uint32_t pos=34;
  for(unsigned t=0;t<3;++t){
    unsigned limit=t==2?384:64;bool end=false;
    for(unsigned i=0;i<limit;++i){unsigned b=byte(pos);if(!b){end=true;break;}
      if(t<2 && i<63)(t?author:title)[i]=b;
    }
    if(!end || error)return false;
  }
  reset();scanning=true;
  for(unsigned c=0;c<6;++c){
    if(channel[c].cursor<pos || channel[c].cursor>=n)return false;
    loop[c]=channel[c];
    int result;
    do {
      result=next(c,nullptr,nullptr);
      if(channel[c].tick>60*60*60)return false;
      if(!(channel[c].tick&1023) && host->size>=offsetof(zm_host_t,rest)+sizeof(host->rest) && host->rest)host->rest();
    }while(result>0);
    if(result<0)return false;
    lengths[c]=channel[c].tick;
    if(lengths[c] && loop[c].tick>=lengths[c])return false;
    if(lengths[c]>ticks)ticks=lengths[c];
  }
  scanning=false;reset();return ticks!=0;
}
bool Tfc::tick(FmWrite write,void* user){
  for(unsigned c=0;c<6;++c){
    if(!lengths[c])continue;
    if(channel[c].tick>=lengths[c])channel[c]=loop[c];
    if(next(c,write,user)!=1)return false;
  }
  return true;
}
}
