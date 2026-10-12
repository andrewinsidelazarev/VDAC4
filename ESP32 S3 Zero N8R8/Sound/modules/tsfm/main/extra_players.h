// Compiled Global Tracker, Sound Tracker 3, Pro Sound Maker and Fast Tracker.
// Format semantics adapted from ZXTune by vitamin.caig@gmail.com.
// SPDX-License-Identifier: LGPL-3.0-only
#include "freq_tables.h"
unsigned read8(AYSongInfo& i,unsigned p){return memory(i.module,p);}
unsigned read16(AYSongInfo& i,unsigned p){return ay_sys_getword(&memory(i.module,p));}
int bound(int x,int lo,int hi){return x<lo?lo:x>hi?hi:x;}
void tone(AYSongInfo& i,unsigned c,unsigned t){ay_writeay(&i,2*c,t);ay_writeay(&i,2*c+1,(t>>8)&15);}
void env(AYSongInfo& i,unsigned type,unsigned value){ay_writeay(&i,13,type);ay_writeay(&i,11,value);ay_writeay(&i,12,value>>8);}
template<class T> void cleanup(AYSongInfo& i){release(static_cast<T*>(i.data));i.data=nullptr;}
template<class T> void measure(AYSongInfo& i,void(*init)(AYSongInfo&),void(*play)(AYSongInfo&)){
  init(i);i.Length=0;
  for(unsigned n=0;n<180000;++n){
    play(i);if(static_cast<T*>(i.data)->wrapped){i.Length=n;return;}
    if(!(n&255) && guard->host->size>=offsetof(zm_host_t,rest)+sizeof(guard->host->rest) && guard->host->rest)guard->host->rest();
  }
  fail();
}
struct GtrChannel {unsigned cursor,period,left,note,sample,ornament,sp,op,volume;bool enabled,envelope;};
struct GtrState {GtrChannel ch[3];unsigned order,row,rows,delay;bool wrapped;};
unsigned gtr_ptr(AYSongInfo& i,unsigned p){unsigned v=read16(i,p),base=read16(i,5);if(v && v<base)fail();return v?v-base:0;}
unsigned GTR_Length(AYSongInfo& i,const GtrChannel (&source)[3]){
  GtrChannel ch[3];memcpy(ch,source,sizeof(ch));unsigned row=0;
  for(;row<64;++row){
    unsigned skip=ch[0].left;for(unsigned c=1;c<3;++c)if(ch[c].left<skip)skip=ch[c].left;
    for(auto& c:ch)c.left-=skip;row+=skip;
    if(!ch[0].left && read8(i,ch[0].cursor)==255)return row<4?4:row;
    for(auto& c:ch){if(c.left){--c.left;continue;}c.period=0;
      for(;;){unsigned cmd=read8(i,c.cursor++);
        if(cmd<=0x5f)break;
        if(cmd>=0x80 && cmd<=0xbf)c.period=cmd-0x80;
        else if(cmd>=0xc0 && cmd<=0xcf)++c.cursor;
        else if(cmd>=0xd0 && cmd<=0xdf)break;
        else if(cmd==0xe0 && read8(i,4)!=0x10)break;
      }c.left=c.period;
    }
  }
  return row;
}
void GTR_Order(AYSongInfo& i){
  auto& s=*static_cast<GtrState*>(i.data);
  unsigned count=read8(i,293);if(!count)fail();
  if(s.order>=count){s.order=read8(i,294);s.wrapped=true;if(s.order>=count)fail();}
  unsigned pat=read8(i,295+s.order);if(pat%6 || pat>=192)fail();
  for(unsigned c=0;c<3;++c){auto& ch=s.ch[c];ch.cursor=gtr_ptr(i,101+pat+2*c);ch.period=ch.left=0;}
  s.row=0;s.rows=GTR_Length(i,s.ch);
}
void GTR_Init(AYSongInfo& i){cleanup<GtrState>(i);i.data=allocate<GtrState>();ay_resetay(&i,0);GTR_Order(i);}
void GTR_Play(AYSongInfo& i){
  auto& s=*static_cast<GtrState*>(i.data);
  if(!s.delay){
    if(s.row>=s.rows){++s.order;GTR_Order(i);}
    for(auto& ch:s.ch){
      if(ch.left){--ch.left;continue;}ch.period=0;
      for(;;){
        unsigned cmd=read8(i,ch.cursor++);
        if(cmd<=0x5f){ch.note=cmd;ch.enabled=true;ch.sp=ch.op=0;break;}
        if(cmd<=0x6f){ch.sample=cmd-0x60;continue;}
        if(cmd<=0x7f){ch.ornament=cmd-0x70;ch.op=0;if(read8(i,4)!=0x10)ch.envelope=false;continue;}
        if(cmd<=0xbf){ch.period=cmd-0x80;continue;}
        if(cmd<=0xcf){env(i,cmd-0xc0,read8(i,ch.cursor++));ch.envelope=true;continue;}
        if(cmd<=0xdf)break;
        if(cmd==0xe0){ch.enabled=false;if(read8(i,4)!=0x10)break;continue;}
        if(cmd<=0xef)ch.volume=15-(cmd-0xe0);
      }
      ch.left=ch.period;
    }
    ++s.row;s.delay=read8(i,0);if(!s.delay)fail();
  }
  --s.delay;unsigned mixer=0,noise=0;
  for(unsigned c=0;c<3;++c){auto& ch=s.ch[c];
    if(!ch.enabled){ay_writeay(&i,8+c,0);mixer|=9<<c;continue;}
    if(ch.sample>=15 || ch.ornament>=16)fail();
    unsigned smp=gtr_ptr(i,39+2*ch.sample),orn=gtr_ptr(i,69+2*ch.ornament);
    unsigned len=smp?read8(i,smp+1):4;if(len%4)fail();len=(len?len:256)/4;
    unsigned off=smp+2+4*(ch.sp%64),flags=smp?read8(i,off+1):0x60;
    int ornament=orn?static_cast<int8_t>(read8(i,orn+2+(ch.op&255))):0;
    unsigned period=PT3NoteTable_ST[bound(int(ch.note)+ornament,0,95)]+(smp?read16(i,off+2):0);
    tone(i,c,period);
    unsigned level=smp?read8(i,off):0;
    ay_writeay(&i,8+c,(bound(int(level)-int(ch.volume),0,255)&15)|((ch.envelope&&(flags&128))?16:0));
    if(flags&32)mixer|=1<<c;if(flags&64)mixer|=8<<c;noise|=flags&31;
    if(++ch.sp>=len)ch.sp=smp?read8(i,smp)/4:0;
    if(++ch.op>=(orn?read8(i,orn+1):0))ch.op=orn?read8(i,orn):0;
  }
  ay_writeay(&i,7,mixer);ay_writeay(&i,6,noise);
}
bool GTR_Detect(uint8_t* d,unsigned long n){return n>=296 && d[0] && d[1]=='G' && d[2]=='T' && d[3]=='R';}
void GTR_GetInfo(AYSongInfo& i){i.Name=ay_sys_getstr(i.module+7,32);measure<GtrState>(i,GTR_Init,GTR_Play);}
void GTR_Cleanup(AYSongInfo& i){cleanup<GtrState>(i);}

struct St3Channel {unsigned cursor,period,left,note,sample,ornament,pos;int counter;bool envelope;};
struct St3State {St3Channel ch[3];unsigned order,row,delay;int transposition;bool wrapped;};
unsigned ST3_Object(AYSongInfo& i,unsigned table,unsigned index){
  unsigned p=read16(i,table);if(index>=read8(i,p))fail();return read16(i,p+1+2*index);
}
void ST3_Order(AYSongInfo& i){
  auto& s=*static_cast<St3State*>(i.data);unsigned p=read16(i,1),count=read8(i,p);
  if(!count)fail();if(s.order>=count){s.order=0;s.wrapped=true;}
  s.transposition=static_cast<int8_t>(read8(i,p+1+2*s.order));
  unsigned pat=read8(i,p+2+2*s.order);if(pat%6)fail();
  for(unsigned c=0;c<3;++c){s.ch[c].cursor=read16(i,read16(i,7)+pat+2*c);s.ch[c].left=s.ch[c].period=0;}
  s.row=0;
}
void ST3_Init(AYSongInfo& i){
  cleanup<St3State>(i);i.data=allocate<St3State>();auto& s=*static_cast<St3State*>(i.data);
  for(auto& ch:s.ch){ch.counter=-1;ch.ornament=ST3_Object(i,5,0);}
  ay_resetay(&i,0);ST3_Order(i);
}
void ST3_Play(AYSongInfo& i){
  auto& s=*static_cast<St3State*>(i.data);
  if(!s.delay){
    if(s.row>=64 || (!s.ch[0].left && read8(i,s.ch[0].cursor)==255)){++s.order;ST3_Order(i);}
    for(auto& ch:s.ch){
      if(ch.left){--ch.left;continue;}
      for(;;){
        unsigned cmd=read8(i,ch.cursor++);
        if(cmd<=0x5f){ch.note=cmd;ch.pos=0;ch.counter=32;break;}
        if(cmd<=0x6f){ch.sample=ST3_Object(i,3,cmd-0x60);continue;}
        if(cmd<=0x7f){ch.ornament=ST3_Object(i,5,cmd-0x70);ch.envelope=false;continue;}
        if(cmd==0x80){ch.counter=-1;break;}if(cmd==0x81)break;
        if(cmd==0x82){ch.ornament=ST3_Object(i,5,0);ch.envelope=false;continue;}
        if(cmd<=0x8e){ch.ornament=ST3_Object(i,5,0);env(i,cmd-0x80,read8(i,ch.cursor++));ch.envelope=true;continue;}
        ch.period=(cmd-0xa1)&255;
      }
      ch.left=ch.period;
    }
    ++s.row;s.delay=read8(i,0);if(!s.delay)fail();
  }
  --s.delay;unsigned mixer=0;
  for(unsigned c=0;c<3;++c){auto& ch=s.ch[c];
    if(ch.counter>=0){
      --ch.counter;ch.pos=(ch.pos+1)&31;
      if(!ch.counter){unsigned loop=ch.sample?read8(i,ch.sample):0;
        if(loop){ch.pos=loop&31;ch.counter=bound(loop+read8(i,ch.sample+1),0,32)-loop+1;}
        else ch.counter=-1;
      }
    }
    if(ch.counter<0 || !ch.sample){ay_writeay(&i,8+c,0);continue;}
    unsigned pos=(ch.pos-1)&31,off=ch.sample+2+4*pos,flags=read8(i,off+2);
    int note=ch.note+static_cast<int8_t>(read8(i,ch.ornament+pos))+s.transposition;
    tone(i,c,ST_Table[bound(note,0,95)]+static_cast<int16_t>(read16(i,off)));
    ay_writeay(&i,8+c,(flags&15)|(ch.envelope?16:0));
    if(flags&16)mixer|=1<<c;if(flags&128)mixer|=8<<c;else ay_writeay(&i,6,read8(i,off+3)&31);
  }
  ay_writeay(&i,7,mixer);
}
bool ST3_Detect(uint8_t* d,unsigned long n){return n>=9 && d[0]>0 && d[0]<=32;}
void ST3_GetInfo(AYSongInfo& i){
  if(i.module_len>=64 && !memcmp(i.module+9,"KSA SOFTWARE COMPILATION OF ",27))i.Name=ay_sys_getstr(i.module+36,27);
  measure<St3State>(i,ST3_Init,ST3_Play);
}
void ST3_Cleanup(AYSongInfo& i){cleanup<St3State>(i);}

struct PsmChannel {
  unsigned cursor,period,left,streamNote,note,sample,ornament,sp,op,volume,baseVolume,loops,ret,repeat;
  unsigned envType,envValue;
  int slide,envNote;
  bool enabled,envelope,reinit,ornFinished,keepOrnFinished,sampleFinished;
};
struct PsmState {PsmChannel ch[3];unsigned order,row,delay,tempo;int transposition;bool wrapped;};
void PSM_Order(AYSongInfo& i){
  auto& s=*static_cast<PsmState*>(i.data);unsigned p=read16(i,0)+2*s.order;
  if(read8(i,p)==255){unsigned loop=read8(i,p+1);s.order=loop==255?0:loop;s.wrapped=true;p=read16(i,0)+2*s.order;}
  unsigned pat=read8(i,p);if(pat>=32)fail();s.transposition=static_cast<int8_t>(read8(i,p+1));
  p=read16(i,6)+7*pat;s.tempo=read8(i,p);if(s.tempo<2 || s.tempo>50)fail();
  for(unsigned c=0;c<3;++c){auto& ch=s.ch[c];ch.cursor=read16(i,p+1+2*c);ch.left=ch.period=ch.ret=ch.repeat=0;ch.streamNote=48;ch.note=0;}
  s.row=0;
}
void PSM_Init(AYSongInfo& i){
  cleanup<PsmState>(i);i.data=allocate<PsmState>();
  for(auto& c:static_cast<PsmState*>(i.data)->ch){c.sample=read16(i,read16(i,2));c.envNote=-1;
    if(read16(i,4)!=read16(i,6))c.ornament=read16(i,read16(i,4));}
  ay_resetay(&i,0);PSM_Order(i);
}
void PSM_Note(AYSongInfo& i,PsmChannel& c,unsigned note,int transposition){
  c.note=note+transposition;c.volume=c.baseVolume;c.sp=c.op=0;c.sampleFinished=false;c.slide=0;c.loops=1;c.ornFinished=c.keepOrnFinished;
  if(c.envelope && c.reinit){unsigned value=c.envValue;
    if(c.envNote>=0){unsigned n=c.note+c.envNote;if(n>=96)n-=96;else if(n>=48)n-=48;value=FREQ_PROSOUNDMAKER[bound((n&127)+48,0,95)]&255;}
    env(i,c.envType,value);
  }
}
void PSM_Play(AYSongInfo& i){
  auto& s=*static_cast<PsmState*>(i.data);
  if(!s.delay){
    bool end=s.row>=64;for(const auto& c:s.ch)if(!c.left && read8(i,c.cursor)>=0xfc)end=true;
    if(end){++s.order;PSM_Order(i);}
    for(int index=2;index>=0;--index){auto& c=s.ch[index];
      if(c.left){--c.left;continue;}
      if(c.repeat && !--c.repeat){c.cursor=c.ret;c.ret=0;}
      for(;;){unsigned cmd=read8(i,c.cursor++);
        if(cmd<=0x5f){c.streamNote=c.streamNote>=cmd?c.streamNote-cmd:(c.streamNote+96-cmd)&255;c.enabled=!(c.streamNote&128);PSM_Note(i,c,c.streamNote&127,s.transposition);break;}
        if(cmd==0x60){c.enabled=false;break;}
        if(cmd<=0x6f){c.sample=read16(i,read16(i,2)+2*(cmd-0x61));continue;}
        if(cmd<=0x8f){c.ornament=read16(i,read16(i,4)+2*(cmd-0x70));c.op=0;c.ornFinished=c.keepOrnFinished=false;c.envelope=c.reinit=false;continue;}
        if(cmd==0x90)break;
        if(cmd<=0x9f){c.baseVolume=cmd-0x90;continue;}
        if(cmd==0xa0){c.ornFinished=c.keepOrnFinished=true;c.op=0;c.envelope=false;continue;}
        if(cmd<=0xb0){unsigned packed=cmd-0xa1;c.envType=8+((packed&3)<<1);c.envNote=3*(packed&12);c.reinit=c.envelope=true;continue;}
        if(cmd<=0xb7){c.envType=cmd-0xa9;unsigned v=read8(i,c.cursor++);c.envValue=v>=0xf1?256*(v&15):v;c.envNote=-1;c.envelope=true;env(i,c.envType,c.envValue);continue;}
        if(cmd<=0xf8){c.period=cmd-0xb8;continue;}
        if(cmd==0xf9){
          // A compiled PSM phrase call: LE16 address, number of rows.
          if(c.repeat)fail();unsigned dest=read16(i,c.cursor);c.repeat=read8(i,c.cursor+2);
          if(!c.repeat)fail();c.ret=c.cursor+3;c.cursor=dest;continue;
        }
        if(cmd<=0xfb){c.reinit=cmd==0xfb;c.ornament=0;continue;}
        break;
      }
      c.left=c.period;
    }
    ++s.row;s.delay=s.tempo;
  }
  --s.delay;unsigned mixer=0;
  for(unsigned num=0;num<3;++num){auto& c=s.ch[num];
    int orn=(!c.envelope && !c.ornFinished && c.ornament && c.op<=read8(i,c.ornament))?static_cast<int8_t>(read8(i,c.ornament+2+(c.op&255))):0;
    unsigned slen=c.sample?1+(read8(i,c.sample)&31):0,off=c.sample+2+3*c.sp;
    unsigned flags=c.sp<slen?read8(i,off):0x90,n=c.sp<slen?read8(i,off+1):0;
    int shift=c.sp<slen?((n&7)<<8)|read8(i,off+2):0;if(shift&0x400)shift-=0x800;
    c.slide=static_cast<int16_t>(c.slide+shift);
    tone(i,num,bound(FREQ_PROSOUNDMAKER[bound(int(c.note)+orn,0,95)]+c.slide,0,4095));
    int level=int((flags&15)|(c.envelope?16:0))+int(c.volume)-15;if(!c.enabled || level<0)level=0;
    ay_writeay(&i,8+num,level&31);
    if(flags&16)mixer|=1<<num;
    if(!(flags&128)&&c.enabled){if(level)ay_writeay(&i,6,n>>3);}else mixer|=8<<num;
    if(!c.sampleFinished && ++c.sp>=slen){unsigned loop=c.sample?read8(i,c.sample+1):0;
      if((loop>>5) && (loop&31)<slen){c.sp=loop&31;if(!--c.loops){c.loops=loop>>5;unsigned v=read8(i,c.sample);int delta=(v&32)?-int(v>>6)-1:int(v>>6);c.volume=bound(int(c.volume)+delta,0,15);}}
      else {c.enabled=false;c.sampleFinished=true;}
    }
    if(!c.ornFinished && ++c.op>=(c.ornament?read8(i,c.ornament)+1:0)){
      unsigned loop=c.ornament?read8(i,c.ornament+1):0;
      if((loop&128) && (loop&31)<=read8(i,c.ornament))c.op=loop&31;else c.ornFinished=true;
    }
  }
  ay_writeay(&i,7,mixer);
}
bool PSM_Detect(uint8_t* d,unsigned long n){return n>=128 && (d[0]|d[1]<<8)>=8;}
void PSM_GetInfo(AYSongInfo& i){unsigned end=read16(i,0),start=8;if(end>i.module_len)fail();if(end>=13&&!memcmp(i.module+8,"psm1",5))start=13;if(end>start)i.Name=ay_sys_getstr(i.module+start,end-start);measure<PsmState>(i,PSM_Init,PSM_Play);}
void PSM_Cleanup(AYSongInfo& i){cleanup<PsmState>(i);}

struct FtcChannel {
  unsigned cursor,period,left,note,sample,ornament,sp,op,volume,envelope;
  int volumeSlide,noise,noteAccum,noiseAccum,toneAccum,sampleNoiseAccum,envAccum,toneAddon;
  int sliding,gliss,direction;
  bool envEnabled;
};
struct FtcState {FtcChannel ch[3];unsigned order,row,delay,tempo,base;int transposition;const unsigned short* table;bool wrapped;};
unsigned FTC_Ptr(AYSongInfo& i,unsigned p){unsigned v=read16(i,p),b=static_cast<FtcState*>(i.data)->base;if(v<b)fail();return v-b;}
unsigned FTC_Length(AYSongInfo& i,unsigned p){return read8(i,p+2)+1;}
void FTC_Order(AYSongInfo& i){
  auto& s=*static_cast<FtcState*>(i.data);unsigned p=212+2*s.order;
  if(read8(i,p)==255){s.order=read8(i,70);s.wrapped=true;p=212+2*s.order;}
  unsigned pat=read8(i,p);if(pat>=32)fail();s.transposition=static_cast<int8_t>(read8(i,p+1));
  p=read16(i,75)+6*pat;
  for(unsigned c=0;c<3;++c){s.ch[c].cursor=FTC_Ptr(i,p+2*c);s.ch[c].period=s.ch[c].left=0;}
  s.row=0;
}
void FTC_Init(AYSongInfo& i){
  cleanup<FtcState>(i);i.data=allocate<FtcState>();auto& s=*static_cast<FtcState*>(i.data);
  unsigned first=0;
  for(unsigned p=read16(i,75);;p+=6){unsigned ptr=read16(i,p);
    if(ptr==65535){if(first<p+2)fail();s.base=first-p-2;break;}if(!first && ptr)first=ptr;
  }
  s.table=read8(i,50)==1?ST_Table:read8(i,50)==2?FREQ_FASTTRACKER:FREQ_PROTRACKER2;
  s.tempo=read8(i,69);if(!s.tempo)fail();
  for(auto& c:s.ch){c.volume=15;c.sample=FTC_Ptr(i,82);c.sp=FTC_Length(i,c.sample);c.ornament=FTC_Ptr(i,146);}
  ay_resetay(&i,0);FTC_Order(i);
}
void FTC_Line(AYSongInfo& i,FtcChannel& c){
  auto& s=*static_cast<FtcState*>(i.data);
  int note=-1,smp=-1,orn=-1,vol=-1,enabled=-1;
  struct Effect {unsigned kind,value,param;};Effect effects[32];unsigned count=0;
  for(;;){unsigned cmd=read8(i,c.cursor++),kind=0,value=0,param=0;
    if(cmd<=0x1f){smp=cmd;continue;}if(cmd<=0x2f){vol=cmd-0x20;continue;}
    if(cmd==0x30){enabled=0;c.period=0;break;}
    if(cmd<=0x3e){kind=1;param=cmd-0x30;value=read16(i,c.cursor);c.cursor+=2;}
    else if(cmd==0x3f)kind=2;
    else if(cmd<=0x5f){c.period=cmd-0x40;break;}
    else if(cmd<=0xcb){note=cmd-0x60;enabled=1;c.period=0;break;}
    else if(cmd<=0xec){orn=cmd-0xcc;continue;}
    else if(cmd==0xed){kind=3;value=read16(i,c.cursor);c.cursor+=2;}
    else if(cmd==0xee){kind=4;value=read8(i,c.cursor++);}
    else if(cmd==0xef){kind=5;value=read8(i,c.cursor++);}
    else {s.tempo=read8(i,c.cursor++);if(!s.tempo)fail();continue;}
    if(count==32)fail();effects[count++]={kind,value,param};
  }
  if(enabled>=0){
    c.sp=enabled?0:FTC_Length(i,c.sample);c.op=0;c.volumeSlide=0;
    c.noteAccum=c.noiseAccum=c.toneAccum=c.sampleNoiseAccum=c.envAccum=0;c.sliding=c.gliss=c.direction=0;
  }
  bool slideNote=false;for(unsigned j=0;j<count;++j)if(effects[j].kind==4)slideNote=true;
  if(note>=0 && !slideNote)c.note=note+s.transposition;
  if(smp>=0)c.sample=FTC_Ptr(i,82+2*smp);
  if(orn>=0){c.ornament=FTC_Ptr(i,146+2*orn);c.op=0;c.noteAccum=c.noiseAccum=0;}
  if(vol>=0)c.volume=vol;
  for(unsigned j=0;j<count;++j){auto& e=effects[j];
    switch(e.kind){
    case 1:ay_writeay(&i,13,e.param);c.envelope=e.value;c.envEnabled=true;break;
    case 2:c.envEnabled=false;break;
    case 3:c.gliss=static_cast<int16_t>(e.value);break;
    case 4:{int target=note>=0?note:0;
      c.sliding=s.table[bound(int(c.note),0,95)]-s.table[bound(target,0,95)];
      c.gliss=c.sliding>=0?-int(e.value):int(e.value);c.direction=c.sliding>=0?-1:1;c.note=target+s.transposition;break;}
    case 5:c.noise=e.value;break;
    }
  }
  c.left=c.period;
}
int accumulate(int& state,int delta,bool keep){int v=static_cast<int16_t>(state+delta);if(keep)state=v;return v;}
void FTC_Play(AYSongInfo& i){
  auto& s=*static_cast<FtcState*>(i.data);
  if(!s.delay){
    if(s.row>=64 || (!s.ch[0].left && read8(i,s.ch[0].cursor)==255)){++s.order;FTC_Order(i);}
    for(auto& c:s.ch){if(c.left)--c.left;else FTC_Line(i,c);}++s.row;s.delay=s.tempo;
  }
  --s.delay;unsigned mixer=0;
  for(unsigned num=0;num<3;++num){auto& c=s.ch[num];
    unsigned olen=FTC_Length(i,c.ornament),of=c.ornament+3+2*c.op;
    if(c.op>=olen)fail();unsigned flags=read8(i,of);
    int note=accumulate(c.noteAccum,static_cast<int8_t>(read8(i,of+1)),flags&64);
    int noise=accumulate(c.noiseAccum,flags&31,flags&128);
    if(++c.op==olen)c.op=read8(i,c.ornament+1);
    unsigned slen=FTC_Length(i,c.sample);
    if(c.sp<slen){
      unsigned p=c.sample+3+5*c.sp,n=read8(i,p),lo=read8(i,p+1),hi=read8(i,p+2),v=read8(i,p+3);
      int sampleNoise=accumulate(c.sampleNoiseAccum,n&31,n&128);
      if(n&64)mixer|=8<<num;else ay_writeay(&i,6,(c.noise+noise+sampleNoise)&31);
      c.toneAddon=accumulate(c.toneAccum,((hi&15)<<8)|lo,hi&128);
      if(hi&64)mixer|=1<<num;
      c.volumeSlide=static_cast<int16_t>(c.volumeSlide+((v&32)?((v&16)?-1:1):0));
      unsigned volume=bound(int(v&15)+c.volumeSlide,0,15);
      unsigned level=((c.volume*17+(c.volume>7))*volume+128)/256;
      int envelope=accumulate(c.envAccum,static_cast<int8_t>(read8(i,p+4)),v&128);
      if((v&64)&&c.envEnabled){level|=16;unsigned value=c.envelope-envelope;ay_writeay(&i,11,value);ay_writeay(&i,12,value>>8);}
      ay_writeay(&i,8+num,level);
      if(++c.sp==slen)c.sp=read8(i,c.sample+1);
    }else {ay_writeay(&i,8+num,0);mixer|=9<<num;}
    c.sliding=static_cast<int16_t>(c.sliding+c.gliss);
    if((c.direction>0 && c.sliding>=0)||(c.direction<0 && c.sliding<0))c.sliding=c.gliss=0;
    tone(i,num,s.table[bound(int(c.note)+note,0,95)]+c.toneAddon+c.sliding);
  }
  ay_writeay(&i,7,mixer);
}
bool FTC_Detect(uint8_t* d,unsigned long n){return n>=220 && d[69]>=3;}
void FTC_GetInfo(AYSongInfo& i){if(!memcmp(i.module,"Module: ",8))i.Name=ay_sys_getstr(i.module+8,42);measure<FtcState>(i,FTC_Init,FTC_Play);}
void FTC_Cleanup(AYSongInfo& i){cleanup<FtcState>(i);}
