// TSFM.ELF — compiled ZX Spectrum AY/TurboSound and TurboFM modules.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Песня — до MAX_TRACKS дорожек, звучащих одновременно; у каждой свои микросхемы
// и своя частота кадров:
//   - AY: проигрыватель компилированной музыки (AS0…STP, PT3 с TurboSound внутри —
//     два AY) на AYumi;
//   - TFC: команды TurboFM для двух YM2203;
//   - TFD: дамп регистров двух YM2203 (TurboFM Dump).
// Обычный файл — одна дорожка, TS — две дорожки AY, MTC (Multitrack Container
// ZXTune) — дорожки из его блоков TRCK. Дорожки смешиваются поровну (среднее),
// как у ZXTune; короче самой длинной — повторяются (проигрыватели AY и TFC — со
// своей точки повтора, TFD — с метки повтора).
//
// Громкость: при открытии вся песня прогоняется по тактам без синтеза, по
// регистрам оценивается её громкость (LUFS), и выбирается одно усиление на всю
// песню к цели −12 LUFS — той же, что у TRACKER.ELF и OPL.ELF. По ходу песни
// усиление не меняется: прежнее выравнивание на тихом вступлении задирало его
// (до +8 дБ в «Popcorn.tfc»), а на громкой части давило пики ограничителем.
// Редкие пики выше −1 дБ снимает чистый ограничитель: он убавляет общее
// усиление сразу и отпускает за ~150 мс, форму волны не гнёт.
#include "ay_players.h"
#include "tfc.h"
#include "tfd.h"
#include "zm_app.h"
#include "ymfm/ymfm_opn.h"
extern "C" {
#include "ayumi/ayumi.h"
}
#include <new>
#include <stdio.h>
#include <string.h>
#include <math.h>
namespace {
// Вывод — 96 кГц (с 1.3): прямоугольные волны AY звучат с гармониками выше
// 20 кГц, как аналоговый выход настоящего TSFM (запись ZOOM0185: на 44,1 кГц
// выше 22 кГц у ESP было на 10–32 дБ меньше, ниже 18 кГц — вровень).
// Частота вывода (с 1.5): песня только из AY — 96 кГц (гармоники AY выше 20 кГц,
// как у настоящего TSFM); есть YM2203 — 44,1 кГц: FM всё равно срезается фильтром
// пересчёта около 20 кГц, а два AY и два YM2203 (MTC) на 96 кГц не успевали —
// 113 % времени декодера на плате.
constexpr unsigned AY_RATE=96000,FM_SONG_RATE=44100,AY_CLOCK=1773400,FM_CLOCK=3500000;
constexpr unsigned MAX_TRACKS=4,MAX_AY=4,MAX_FM=4,MAX_TFC=2,MAX_TFD=2;
constexpr float TARGET_LUFS=-12.0f;
constexpr float MIN_GAIN_DB=-12.0f,MAX_GAIN_DB=12.0f;
constexpr float CEILING=0.891f;                 // −1 дБ: выше — ограничитель
constexpr float RELEASE_SECONDS=0.15f;
constexpr float PEAK_ALLOWED=1.12f;             // +1 дБ: оценённый пик после усиления
// Поправки оценки громкости по регистрам к громкости R128 отрисованного звука
// (фильтр K поднимает верха прямоугольных волн AY и гармоники FM); подобраны по
// песням проверки — см. README модуля.
constexpr float AY_LOUDNESS_DB=1.9f;
constexpr float FM_LOUDNESS_DB=-3.1f;
const zm_host_t* host;
zm_source_t table;
void* fast_alloc(size_t n){void* p=host->alloc(n,ZM_MEM_INTERNAL);return p?p:host->alloc(n,ZM_MEM_PSRAM);}
void rest(){if(host->size>=offsetof(zm_host_t,rest)+sizeof(host->rest)&&host->rest)host->rest();}
void log_line(const char* format,float a,float b){if(host->log)host->log(format,double(a),double(b));}
// Такты ядра (CCOUNT): замер частей отрисовки на плате (1.8), раз в 10 с песни —
// в журнал (report_prof). На ПК — 0. Сам замер звук не меняет.
inline uint32_t cycles(){
#if defined(__XTENSA__)
  uint32_t c;__asm__ __volatile__("rsr %0, ccount":"=a"(c));return c;
#else
  return 0;
#endif
}

// Пересчёт частоты FM (48,6 → 44,1 кГц, песни с YM2203 всегда на ней): sinc с
// окном Блэкмана, 32 точки, 64 фазы, срез 19,8 кГц (0,449 частоты вывода, как у
// ступенек AY). Линейная интерполяция (1.1) теряла верха: -2,8 дБ на 15 кГц,
// -5,1 дБ на 20 кГц. Задержка — 16 отсчётов FM. Таблица считается при открытии
// файла (до отрисовки).
constexpr unsigned FM_TAPS=32,FM_PHASES=64;
float fm_kernel[FM_PHASES+1][FM_TAPS];
bool fm_kernel_ready;
void fm_kernel_init(){
  if(fm_kernel_ready)return;
  const float pi=3.14159265358979f,cutoff=0.449f*float(FM_SONG_RATE)/(FM_CLOCK/72.0f);
  for(unsigned p=0;p<=FM_PHASES;++p){
    const float f=float(p)/FM_PHASES;float sum=0;
    for(unsigned j=0;j<FM_TAPS;++j){
      const float x=f-(float(j)-15.0f);                  // от отсчёта j до точки выхода
      const float arg=2.0f*cutoff*x;
      const float s=fabsf(arg)<1e-6f?1.0f:sinf(pi*arg)/(pi*arg);
      const float w=(x+16.0f)/32.0f;
      const float window=w<=0.0f||w>=1.0f?0.0f:0.42f-0.5f*cosf(2.0f*pi*w)+0.08f*cosf(4.0f*pi*w);
      fm_kernel[p][j]=2.0f*cutoff*s*window;sum+=fm_kernel[p][j];
    }
    for(unsigned j=0;j<FM_TAPS;++j)fm_kernel[p][j]/=sum;
  }
  fm_kernel_ready=true;
}

class FmChip: public ymfm::ymfm_interface {
public:
  ymfm::ym2203 chip;
  float ring[64];
  uint32_t phase,rate,out,pos;   // rate — частота FM, out — частота вывода
  uint64_t gen;                  // такты синтеза ymfm (generate_fm) за окно замера
  uint8_t regs[256],keys[3];
  // Только FM-часть на её собственной частоте (такт / (делитель * 12), 48,6 кГц
  // при 3,5 МГц). SSG не звучит: TFC и TFD пишут лишь регистры FM, как и у
  // ZXTune. Прежний generate() шёл на такт/24 и гонял SSG — два чипа на ESP32-S3
  // не успевали (108 % времени декодера).
  explicit FmChip(uint32_t output):chip(*this),phase(0),out(output),pos(0),gen(0){
    memset(ring,0,sizeof(ring));memset(regs,0,sizeof(regs));memset(keys,0,sizeof(keys));
    chip.set_fidelity(ymfm::OPN_FIDELITY_MIN);chip.reset();rate=chip.fm_sample_rate(FM_CLOCK);
  }
  void write(unsigned reg,unsigned value){
    reg&=255;value&=255;regs[reg]=value;
    if(reg==0x28 && (value&3)<3)keys[value&3]=value>>4;
    chip.write_address(reg);chip.write_data(value);
    // Делитель частоты (2D..2F) меняет частоту FM-движка.
    if(reg>=0x2d && reg<=0x2f)rate=chip.fm_sample_rate(FM_CLOCK);
  }
  // Точка выхода — между отсчётами pos-17 и pos-16 FM, доля phase/out.
  float sample(){
    phase+=rate;
    if(phase>=out){
      const uint32_t t0=cycles();
      while(phase>=out){
        phase-=out;pos=(pos+1)&63;
        ring[pos]=chip.generate_fm()*(1.0f/32768.0f);
      }
      gen+=uint32_t(cycles()-t0);
    }
    unsigned p=(phase*FM_PHASES+out/2)/out;if(p>FM_PHASES)p=FM_PHASES;
    const float* k=fm_kernel[p];float y=0;
    const unsigned base=pos-32;                       // отсчёт с весом k[0]
    for(unsigned j=0;j<FM_TAPS;++j)y+=ring[(base+j)&63]*k[j];
    return y;
  }
};

// Несущие операторы алгоритмов OPN в порядке регистров: +0 — op1, +4 — op3,
// +8 — op2, +C — op4 (бит k — смещение 4k).
const unsigned CARRIERS[8]={8,8,8,8,12,14,14,15};

enum Kind:uint8_t{KIND_AY,KIND_TFC,KIND_TFD};
struct Track {
  Kind kind;
  uint8_t index;            // номер TFC или TFD
  uint8_t first,count;      // микросхемы в ay[] или fm[]
  tsfm::AyPlayer* player;
  uint32_t ticks;           // длина песни в своих кадрах
  uint16_t frequency;       // кадров в секунду (50, 60…)
  uint32_t samples;         // отсчётов на кадр: частота вывода / frequency
  uint32_t remaining;       // отсчётов до следующего кадра
};
struct Source {
  Track track[MAX_TRACKS];
  unsigned tracks,ays,fms,tfcs,tfds;
  tsfm::Tfc tfc[MAX_TFC];
  tsfm::Tfd tfd[MAX_TFD];
  ayumi* ay[MAX_AY];
  FmChip* fm[MAX_FM];
  uint8_t regs[MAX_AY][14];
  zm_info_t info;
  uint64_t frame,total;     // отсчётов сыграно и в песне
  unsigned rate;            // частота вывода: AY_RATE или FM_SONG_RATE
  float gain,limit,release;
  bool error;
  // Буферы отрезка (до ZM_RENDER_MAX_FRAMES): FM — [0, MAX_FM), AY — с MAX_FM.
  float* buffer[MAX_FM+MAX_AY];
  uint8_t chip_id[MAX_FM+MAX_AY],side[MAX_FM+MAX_AY];
  unsigned chip_count,job_count;
  bool split;
  int (*host_worker_start)(void (*)(void*),void*);
  void (*host_worker_wait)(void);
  // Замер (1.8): такты за окно — по микросхемам (FM — синтез и пересчёт вместе;
  // синтез отдельно — FmChip::gen) и по частям render(); calls — вызовов pcm,
  // то есть запусков помощника на ядре 1.
  struct Prof {
    uint64_t chip[MAX_FM+MAX_AY];
    uint64_t tick,start,own,wait,mix,gain,total,frames;
    uint32_t calls;
  } prof;
};

const char* const AY_TYPES[]={"AS0","ASC","FTC","GTR","PSC","PSM","PT1","PT2","PT3","SQT","ST3","STC","STP"};
bool ay_type(const char* type){for(const char* name:AY_TYPES)if(!strcmp(type,name))return true;return false;}
bool supported(const char* type){
  return ay_type(type) || !strcmp(type,"TS") || !strcmp(type,"TFC") || !strcmp(type,"TFD") || !strcmp(type,"MTC");
}
const char* file_type(){return host->size>=offsetof(zm_host_t,file_type)+sizeof(host->file_type)&&host->file_type?host->file_type:"";}
unsigned word(const uint8_t* p){return p[0]|p[1]<<8;}
uint32_t be32(const uint8_t* p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}

void close_source(void* instance){
  auto* s=static_cast<Source*>(instance);if(!s)return;
  for(unsigned t=0;t<s->tracks;++t)tsfm::ay_close(s->track[t].player);
  for(unsigned c=0;c<MAX_AY;++c)if(s->ay[c])host->free(s->ay[c]);
  for(unsigned c=0;c<MAX_FM;++c)if(s->fm[c]){s->fm[c]->~FmChip();host->free(s->fm[c]);}
  for(float* b:s->buffer)if(b)host->free(b);
  host->free(s);
}

// --- дорожки ----------------------------------------------------------------

Track* new_track(Source* s,Kind kind,unsigned chips,unsigned frequency){
  if(s->tracks>=MAX_TRACKS)return nullptr;
  if(kind==KIND_AY ? s->ays+chips>MAX_AY : s->fms+chips>MAX_FM)return nullptr;
  Track& t=s->track[s->tracks];
  memset(&t,0,sizeof(t));t.kind=kind;t.count=uint8_t(chips);t.frequency=uint16_t(frequency);
  t.first=uint8_t(kind==KIND_AY?s->ays:s->fms);
  return &t;
}
void commit_track(Source* s,Track* t){
  if(t->kind==KIND_AY)s->ays+=t->count;else s->fms+=t->count;
  ++s->tracks;
}
bool add_ay(Source* s,const uint8_t* data,size_t size,const char* type){
  if(s->tracks>=MAX_TRACKS)return false;
  tsfm::AyPlayer* player=tsfm::ay_open(data,size,type,host);
  if(!player)return false;
  Track* t=new_track(s,KIND_AY,tsfm::ay_chips(player),50);
  if(!t){tsfm::ay_close(player);return false;}
  t->player=player;t->ticks=tsfm::ay_ticks(player);
  commit_track(s,t);return true;
}
bool add_tfc(Source* s,const uint8_t* data,size_t size){
  if(s->tfcs>=MAX_TFC)return false;
  tsfm::Tfc& tfc=s->tfc[s->tfcs];
  if(!tfc.open(data,size,host))return false;
  Track* t=new_track(s,KIND_TFC,2,tfc.rate);if(!t)return false;
  t->index=uint8_t(s->tfcs++);t->ticks=tfc.ticks;
  commit_track(s,t);return true;
}
bool add_tfd(Source* s,const uint8_t* data,size_t size){
  if(s->tfds>=MAX_TFD)return false;
  tsfm::Tfd& tfd=s->tfd[s->tfds];
  if(!tfd.open(data,size,host))return false;
  Track* t=new_track(s,KIND_TFD,2,50);if(!t)return false;
  t->index=uint8_t(s->tfds++);t->ticks=tfd.frames;
  commit_track(s,t);return true;
}
// TS: два компилированных модуля AY и подвал «тип1!размер1 тип2!размер2 02TS».
bool add_ts(Source* s,const uint8_t* data,size_t size){
  if(size<272 || memcmp(data+size-4,"02TS",4))return false;
  const uint8_t* footer=data+size-16;unsigned size1=word(footer+4),size2=word(footer+10);
  if(size1+size2!=size-16 || !size1 || !size2)return false;
  char ext1[4]={char(footer[0]),char(footer[1]),char(footer[2]),0};
  char ext2[4]={char(footer[6]),char(footer[7]),char(footer[8]),0};
  if(footer[3]!='!' || footer[9]!='!' || !ay_type(ext1) || !ay_type(ext2))return false;
  const unsigned before=s->tracks;
  if(add_ay(s,data,size1,ext1) && add_ay(s,data+size1,size2,ext2))return true;
  while(s->tracks>before){                        // откат: дорожки TS только парой
    Track& t=s->track[--s->tracks];s->ays-=t.count;tsfm::ay_close(t.player);t.player=nullptr;
  }
  return false;
}

// Данные дорожки MTC: тип — по подписи, затем по расширению имени из PROP
// (Filename=…), затем перебором проигрывателей AY (у каждого своя проверка).
bool add_mtc_data(Source* s,const uint8_t* data,uint32_t size,const char* ext){
  if(size>=6 && !memcmp(data,"TFMcom",6))return add_tfc(s,data,size);
  if(size>=4 && !memcmp(data,"TFMD",4))return add_tfd(s,data,size);
  if(!strcmp(ext,"TS"))return add_ts(s,data,size);
  if(ay_type(ext) && add_ay(s,data,size,ext))return true;
  if((size>=13 && !memcmp(data,"ProTracker 3.",13)) || (size>=17 && !memcmp(data,"Vortex Tracker II",17)))
    if(add_ay(s,data,size,"PT3"))return true;
  for(const char* type:AY_TYPES)if(strcmp(type,"AS0") && add_ay(s,data,size,type))return true;
  return false;
}
void copy_text(char* out,const uint8_t* text,uint32_t length){
  if(length>63)length=63;memcpy(out,text,length);out[length]=0;
}
// Блоки контейнера: [имя 4][длина BE32][данные], данные выровнены на 2.
bool parse_mtc_track(Source* s,const uint8_t* body,uint32_t length){
  const uint8_t* data[4]={};uint32_t sizes[4]={};unsigned count=0;char ext[4]={};
  for(uint32_t p=0;p+8<=length;){
    const uint8_t* id=body+p;const uint32_t n=be32(body+p+4);const uint32_t at=p+8;
    if(n>length-at)break;
    if(!memcmp(id,"DATA",4) && count<4){data[count]=body+at;sizes[count]=n;++count;}
    else if(!memcmp(id,"PROP",4) && n>9 && !memcmp(body+at,"Filename=",9)){
      // Расширение — после последней точки имени, заглавными, до трёх букв.
      uint32_t dot=0;for(uint32_t i=9;i<n;++i)if(body[at+i]=='.')dot=i;
      if(dot){unsigned k=0;for(uint32_t i=dot+1;i<n && k<3;++i){char c=char(body[at+i]);ext[k++]=c>='a'&&c<='z'?char(c-32):c;}ext[k]=0;}
    }
    p=at+((n+1)&~1u);
  }
  for(unsigned i=0;i<count;++i)if(add_mtc_data(s,data[i],sizes[i],ext))return true;
  return false;
}
bool parse_mtc(Source* s,const uint8_t* d,uint32_t n){
  if(n<16 || memcmp(d,"MTC1",4))return false;
  const uint32_t size=be32(d+4);if(size>n-8)return false;
  for(uint32_t p=8,end=8+size;p+8<=end;){
    const uint8_t* id=d+p;const uint32_t len=be32(d+p+4);const uint32_t at=p+8;
    if(len>end-at)break;
    if(!memcmp(id,"NAME",4))copy_text(s->info.title,d+at,len);
    else if(!memcmp(id,"AUTH",4))copy_text(s->info.artist,d+at,len);
    else if(!memcmp(id,"TRCK",4))parse_mtc_track(s,d+at,len);
    p=at+((len+1)&~1u);
  }
  return s->tracks!=0;
}

// --- такт и синтез ------------------------------------------------------------

struct FmTarget {Source* s;unsigned first;};
void fm_write(void* p,unsigned chip,unsigned reg,unsigned value){
  auto* t=static_cast<FmTarget*>(p);t->s->fm[t->first+chip]->write(reg,value);
}
// Кадр дорожки. За своим концом дорожка продолжается (повтор) — песня кончается
// по самой длинной.
bool tick_track(Source* s,Track& t){
  if(t.kind==KIND_AY){
    uint8_t regs[2][14];uint16_t masks[2];
    if(!tsfm::ay_tick(t.player,regs,masks))return false;
    for(unsigned c=0;c<t.count;++c){
      const unsigned chip=t.first+c;memcpy(s->regs[chip],regs[c],14);
      auto* ay=s->ay[chip];const uint8_t* r=regs[c];
      for(unsigned ch=0;ch<3;++ch){
        ayumi_set_tone(ay,ch,word(r+2*ch));ayumi_set_volume(ay,ch,r[8+ch]&15);
        ayumi_set_mixer(ay,ch,r[7]>>ch,r[7]>>(3+ch),r[8+ch]&16);
      }
      ayumi_set_noise(ay,r[6]);ayumi_set_envelope(ay,word(r+11));
      if(masks[c]&0x2000)ayumi_set_envelope_shape(ay,r[13]);
    }
    return true;
  }
  FmTarget target={s,t.first};
  return t.kind==KIND_TFC ? s->tfc[t.index].tick(fm_write,&target) : s->tfd[t.index].tick(fm_write,&target);
}
bool reset_tracks(Source* s){
  for(unsigned i=0;i<s->tracks;++i){
    Track& t=s->track[i];t.remaining=0;
    if(t.kind==KIND_AY){if(!tsfm::ay_reset(t.player))return false;}
    else if(t.kind==KIND_TFC)s->tfc[t.index].reset();
    else s->tfd[t.index].reset();
  }
  return true;
}
void reset_chips(Source* s){
  memset(s->regs,0,sizeof(s->regs));
  for(unsigned c=0;c<s->fms;++c){s->fm[c]->~FmChip();new(s->fm[c]) FmChip(s->rate);}
  for(unsigned c=0;c<s->ays;++c){
    memset(s->ay[c],0,sizeof(ayumi));ayumi_configure(s->ay[c],0,float(AY_CLOCK),int(s->rate));
    // ABC, как у настоящего TSFM: A — влево, B — в центр, C — вправо (с 1.3;
    // прежние 10/50/90 % давали стерео заметно уже записи с платы).
    for(unsigned ch=0;ch<3;++ch)ayumi_set_pan(s->ay[c],ch,0.5f*float(ch),0);
  }
  s->frame=0;s->error=false;s->limit=1.0f;
}
// Кадры дорожек, чей срок настал.
bool tick_due(Source* s){
  for(unsigned i=0;i<s->tracks;++i){
    Track& t=s->track[i];
    if(t.remaining)continue;
    if(!tick_track(s,t)){s->error=true;return false;}
    t.remaining=t.samples;
  }
  return true;
}
uint32_t until_tick(const Source* s){
  uint32_t n=UINT32_MAX;
  for(unsigned i=0;i<s->tracks;++i)if(s->track[i].remaining<n)n=s->track[i].remaining;
  return n;
}
void advance(Source* s,uint32_t n){
  for(unsigned i=0;i<s->tracks;++i)s->track[i].remaining-=n;
  s->frame+=n;
}
// Отрезок одной микросхемы — в её буфер: FM — моно, AY — левый и правый.
void render_chip(Source* s,unsigned id,unsigned count){
  float* out=s->buffer[id];const uint32_t t0=cycles();
  if(id<MAX_FM){FmChip* f=s->fm[id];for(unsigned i=0;i<count;++i)out[i]=f->sample();}
  else{
    ayumi* ay=s->ay[id-MAX_FM];
    for(unsigned i=0;i<count;++i){ayumi_process_blep(ay);ayumi_remove_dc(ay);out[2*i]=ay->left;out[2*i+1]=ay->right;}
  }
  s->prof.chip[id]+=uint32_t(cycles()-t0);
}
void render_side(Source* s,unsigned side){
  for(unsigned i=0;i<s->chip_count;++i)if(s->side[i]==side)render_chip(s,s->chip_id[i],s->job_count);
}
void worker_job(void* arg){auto* s=static_cast<Source*>(arg);render_side(s,1);}
// Среднее дорожек; дорожка AY — среднее своих AY, FM — первый чип левее, второй
// правее (как у одиночного TFC). Микросхемы стороны 1 считает помощник плеера на
// ядре 1, пока основная задача считает сторону 0: два YM2203 и два AY (MTC)
// одному ядру ESP32-S3 не под силу.
void pcm(Source* s,float* out,unsigned count){
  s->job_count=count;++s->prof.calls;
  uint32_t t=cycles();
  if(s->split && s->host_worker_start(worker_job,s)==ZM_OK){
    s->prof.start+=uint32_t(cycles()-t);t=cycles();
    render_side(s,0);s->prof.own+=uint32_t(cycles()-t);t=cycles();
    s->host_worker_wait();s->prof.wait+=uint32_t(cycles()-t);
  }
  else {render_side(s,0);render_side(s,1);s->prof.own+=uint32_t(cycles()-t);}
  t=cycles();
  if(out){
    const float tracks=1.0f/float(s->tracks);
    memset(out,0,sizeof(float)*2*count);
    for(unsigned k=0;k<s->tracks;++k){
      const Track& t=s->track[k];
      for(unsigned c=0;c<t.count;++c){
        if(t.kind==KIND_AY){
          const float* b=s->buffer[MAX_FM+t.first+c];const float scale=tracks/float(t.count);
          for(unsigned i=0;i<count;++i){out[2*i]+=b[2*i]*scale;out[2*i+1]+=b[2*i+1]*scale;}
        }else{
          const float* b=s->buffer[t.first+c];
          const float wl=(c?0.25f:0.75f)*tracks,wr=(c?0.75f:0.25f)*tracks;
          for(unsigned i=0;i<count;++i){out[2*i]+=b[i]*wl;out[2*i+1]+=b[i]*wr;}
        }
      }
    }
  }
  s->prof.mix+=uint32_t(cycles()-t);
  advance(s,count);
}
// Итог замера за окно — тактов на отсчёт вывода (бюджет ядра: 240 МГц / частота):
// tsfm prof: … total, tick (кадры дорожек), start (запуск помощника), own (своя
// сторона на ядре 0), wait (ожидание помощника), mix, gain; по микросхемам —
// fmN всего/синтез ymfm, ayN; side1 — что считал помощник.
void report_prof(Source* s){
  auto& p=s->prof;const uint64_t f=p.frames?p.frames:1;
  if(host->log){
    host->log("tsfm prof %us: total %u tick %u start %u own %u wait %u mix %u gain %u, calls %u",
              unsigned(s->frame/s->rate),unsigned(p.total/f),unsigned(p.tick/f),unsigned(p.start/f),
              unsigned(p.own/f),unsigned(p.wait/f),unsigned(p.mix/f),unsigned(p.gain/f),unsigned(p.calls));
    char line[128];int n=0;uint64_t side1=0;
    for(unsigned i=0;i<s->chip_count && n<100;++i){
      const unsigned id=s->chip_id[i];
      if(s->side[i])side1+=p.chip[id];
      if(id<MAX_FM)n+=snprintf(line+n,sizeof(line)-n,"fm%u %u/%u ",id,unsigned(p.chip[id]/f),unsigned(s->fm[id]->gen/f));
      else n+=snprintf(line+n,sizeof(line)-n,"ay%u %u ",id-MAX_FM,unsigned(p.chip[id]/f));
    }
    host->log("tsfm prof chips: %sside1 %u",line,unsigned(side1/f));
    // AY: событий счётчиков и ступенек (смен уровня) в секунду — сколько событий
    // не меняют звука (1.11).
    n=0;
    for(unsigned c=0;c<s->ays && n<100;++c)
      n+=snprintf(line+n,sizeof(line)-n,"ay%u %u/%u ",c,unsigned(uint64_t(s->ay[c]->events)*s->rate/f),
                  unsigned(uint64_t(s->ay[c]->steps)*s->rate/f));
    if(s->ays)host->log("tsfm prof ay events/steps per second: %s",line);
  }
  memset(&p,0,sizeof(p));
  for(unsigned c=0;c<s->fms;++c)s->fm[c]->gen=0;
  for(unsigned c=0;c<s->ays;++c){s->ay[c]->events=0;s->ay[c]->steps=0;}
}
// Две стороны примерно поровну по стоимости: YM2203 вдвое дороже AY (замер на
// ПК). Без помощника (старый плеер, нет задачи на ядре 1) — всё на одном ядре.
void plan_split(Source* s){
  s->chip_count=0;
  for(unsigned c=0;c<s->fms;++c)s->chip_id[s->chip_count++]=uint8_t(c);
  for(unsigned c=0;c<s->ays;++c)s->chip_id[s->chip_count++]=uint8_t(MAX_FM+c);
  const bool worker=host->size>=offsetof(zm_host_t,worker_wait)+sizeof(host->worker_wait) &&
                    host->worker_start && host->worker_wait;
  s->split=worker && s->chip_count>1;
  if(s->split){s->host_worker_start=host->worker_start;s->host_worker_wait=host->worker_wait;}
  unsigned load[2]={0,0};
  for(unsigned i=0;i<s->chip_count;++i){
    const unsigned cost=s->chip_id[i]<MAX_FM?2:1;
    // Основная задача ещё тактует и смешивает: при равенстве — помощнику.
    const uint8_t side=!s->split?0:(load[1]<=load[0]?1:0);
    s->side[i]=side;load[side]+=cost;
  }
}
// Постоянное усиление песни и чистый ограничитель: при пике выше CEILING общее
// усиление сразу убавляется до нужного, затем плавно возвращается.
void apply_gain(Source* s,float* out,unsigned frames){
  float limit=s->limit;
  for(unsigned i=0;i<frames;++i){
    const float l=out[2*i]*s->gain,r=out[2*i+1]*s->gain;
    const float al=fabsf(l),ar=fabsf(r),peak=al>ar?al:ar;
    limit+=(1.0f-limit)*s->release;
    if(peak*limit>CEILING)limit=CEILING/peak;
    out[2*i]=l*limit;out[2*i+1]=r*limit;
  }
  s->limit=limit;
}

// --- оценка громкости по регистрам --------------------------------------------

// Средняя громкость огибающей (мощность прямоугольной волны с размахом уровня).
float ay_envelope_level(const ayumi* ay){float sum=0;for(int i=0;i<32;++i)sum+=ay->dac_table[i];return sum/32.0f;}
// Мощность (левый, правый) дорожки AY: каждый канал — прямоугольная волна или
// шум с размахом уровня ЦАП (СКЗ — половина размаха, тон и шум вместе — 0,433),
// раскладка ABC; среднее своих AY — множитель 1/count по амплитуде.
void ay_power(const Source* s,const Track& t,float& pl,float& pr,float& kl,float& kr){
  const ayumi* ay=s->ay[t.first];const float env=ay_envelope_level(ay);
  const float scale=1.0f/float(t.count*t.count);
  for(unsigned c=0;c<t.count;++c){
    const uint8_t* r=s->regs[t.first+c];
    for(unsigned ch=0;ch<3;++ch){
      const bool tone=!((r[7]>>ch)&1),noise=!((r[7]>>(3+ch))&1);
      if(!tone && !noise)continue;               // постоянный уровень: после фильтра — тишина
      const float level=(r[8+ch]&16)?env:ay->dac_table[(r[8+ch]&15)*2+1];
      const float rms=level*(tone&&noise?0.433f:0.5f);
      const float wl=1.0f-0.5f*float(ch),wr=0.5f*float(ch);
      pl+=wl*wl*rms*rms*scale;pr+=wr*wr*rms*rms*scale;
      // Пик: 0,8 полного размаха — фильтр постоянной составляющей AYumi
      // медленный, и волна в начале нот не центрирована; огибающая — по верху.
      const float full=0.8f*((r[8+ch]&16)?ay->dac_table[31]:level)/float(t.count);
      kl+=wl*full;kr+=wr*full;
    }
  }
}
// Мощность дорожки FM: у нажатого канала — несущие по TL (0,75 дБ на шаг) и
// половине уровня удержания SL (3 дБ на шаг); синус полной шкалы канала — 0,25.
void fm_power(const Source* s,const Track& t,float& pl,float& pr,float& kl,float& kr){
  for(unsigned c=0;c<t.count;++c){
    const FmChip* f=s->fm[t.first+c];float p=0,peak=0;
    for(unsigned ch=0;ch<3;++ch){
      if(!f->keys[ch])continue;
      const unsigned mask=CARRIERS[f->regs[0xb0+ch]&7];
      for(unsigned op=0;op<4;++op){
        if(!(mask&(1u<<op)))continue;
        const unsigned tl=f->regs[0x40+4*op+ch]&127;
        const float db=0.75f*float(tl)+1.5f*float(f->regs[0x80+4*op+ch]>>4);
        const float amp=0.25f*powf(10.0f,-db/20.0f);
        p+=0.5f*amp*amp;
        // Пик — без уровня удержания; несущие складываются не в фазе: 0,8 суммы.
        peak+=0.8f*0.25f*powf(10.0f,-0.75f*float(tl)/20.0f);
      }
    }
    const float wl=c?0.25f:0.75f,wr=c?0.75f:0.25f;
    pl+=p*wl*wl;pr+=p*wr*wr;kl+=peak*wl;kr+=peak*wr;
  }
}
// Песня целиком по кадрам, без синтеза: мощность каждого промежутка между
// кадрами — в шаги по 100 мс, шаги — в блоки 400 мс, пороги R128 (−70 LUFS и
// −10 LU от среднего). Возвращает громкость, LUFS (−99 — тишина).
float estimate_loudness(Source* s,float& peak){
  const uint32_t step=s->rate/10;
  float* steps=static_cast<float*>(host->alloc(sizeof(float)*(size_t(s->total/step)+2),ZM_MEM_PSRAM));
  if(!steps)return -99.0f;
  uint32_t count=0;double acc=0;uint32_t in_step=0;
  const float ay_k=powf(10.0f,AY_LOUDNESS_DB/10.0f),fm_k=powf(10.0f,FM_LOUDNESS_DB/10.0f);
  const float tracks2=1.0f/float(s->tracks*s->tracks);
  const float tracks1=1.0f/float(s->tracks);
  unsigned fuel=0;peak=0;
  while(s->frame<s->total){
    if(!tick_due(s))break;
    float power=0,kl=0,kr=0;
    for(unsigned k=0;k<s->tracks;++k){
      const Track& t=s->track[k];float pl=0,pr=0;
      if(t.kind==KIND_AY){ay_power(s,t,pl,pr,kl,kr);power+=(pl+pr)*ay_k;}
      else{fm_power(s,t,pl,pr,kl,kr);power+=(pl+pr)*fm_k;}
    }
    power*=tracks2;
    const float k=(kl>kr?kl:kr)*tracks1;if(k>peak)peak=k;
    uint32_t n=until_tick(s);if(n>s->total-s->frame)n=uint32_t(s->total-s->frame);
    while(n){
      const uint32_t take=n<step-in_step?n:step-in_step;
      acc+=double(power)*take;in_step+=take;n-=take;advance(s,take);
      if(in_step==step){steps[count++]=float(acc/step);acc=0;in_step=0;}
    }
    if(!(++fuel&1023))rest();
  }
  double sum=0;unsigned blocks=0;
  for(uint32_t i=3;i<count;++i){
    const double e=(steps[i]+steps[i-1]+steps[i-2]+steps[i-3])/4.0;
    if(e>0 && -0.691+10.0*log10(e)>-70.0){sum+=e;++blocks;}
  }
  float result=-99.0f;
  if(blocks){
    const double gate=-0.691+10.0*log10(sum/blocks)-10.0;double gated=0;unsigned kept=0;
    for(uint32_t i=3;i<count;++i){
      const double e=(steps[i]+steps[i-1]+steps[i-2]+steps[i-3])/4.0;
      if(e>0 && -0.691+10.0*log10(e)>gate){gated+=e;++kept;}
    }
    if(kept)result=float(-0.691+10.0*log10(gated/kept));
  }
  host->free(steps);
  return result;
}

// --- таблица декодера ---------------------------------------------------------

int render(void* instance,float* out,uint32_t max,uint32_t* frames){
  auto* s=static_cast<Source*>(instance);*frames=0;if(max>ZM_RENDER_MAX_FRAMES)max=ZM_RENDER_MAX_FRAMES;
  const uint32_t begin=cycles();
  while(*frames<max && s->frame<s->total){
    uint32_t t=cycles();
    if(!tick_due(s))break;
    s->prof.tick+=uint32_t(cycles()-t);
    uint32_t n=max-*frames;const uint32_t due=until_tick(s);if(n>due)n=due;
    if(n>s->total-s->frame)n=uint32_t(s->total-s->frame);
    pcm(s,out+2**frames,n);*frames+=n;
  }
  if(*frames){
    uint32_t t=cycles();apply_gain(s,out,*frames);s->prof.gain+=uint32_t(cycles()-t);
    s->prof.total+=uint32_t(cycles()-begin);s->prof.frames+=*frames;
    if(s->prof.frames>=uint64_t(s->rate)*10)report_prof(s);
    return ZM_RENDER_OK;
  }
  return s->error?ZM_RENDER_ERROR:ZM_RENDER_END;
}
int seek(void* instance,int32_t delta,uint32_t* offset){
  auto* s=static_cast<Source*>(instance);int64_t target=int64_t(s->frame)*1000/s->rate+delta;
  if(target<0)target=0;if(target>s->info.duration_ms)target=s->info.duration_ms;
  // The millisecond duration is rounded down. Seeking to the end must still
  // consume the last partial millisecond of audio.
  const uint64_t goal=target==s->info.duration_ms?s->total:uint64_t(target)*s->rate/1000;
  if(!reset_tracks(s))return ZM_ERROR;
  reset_chips(s);
  while(s->frame<goal){
    if(!tick_due(s))break;
    uint64_t n=goal-s->frame;const uint32_t due=until_tick(s);if(n>due)n=due;
    // Далеко до цели — кадры без синтеза; последние 60 мс — со звуком, чтобы
    // синтезатор подошёл к месту прогретым. Отрезок синтеза — не длиннее
    // буферов микросхем (на 96 кГц кадр 50 Гц — 1920 отсчётов, буфер — 1024).
    if(goal-s->frame>3*(s->rate/50) && n==due)advance(s,uint32_t(n));
    else {if(n>ZM_RENDER_MAX_FRAMES)n=ZM_RENDER_MAX_FRAMES;pcm(s,nullptr,unsigned(n));}
    rest();
  }
  *offset=UINT32_MAX;return s->error?ZM_ERROR:ZM_OK;
}
void info(const void* instance,zm_info_t* out){auto* s=static_cast<const Source*>(instance);*out=s->info;out->position_ms=uint32_t(s->frame*1000/s->rate);out->errors=s->error;}
int levels(const void* instance,uint8_t* out,int max){
  auto* s=static_cast<const Source*>(instance);int n=0;
  for(unsigned k=0;k<s->tracks;++k){
    const Track& t=s->track[k];
    for(unsigned c=0;c<t.count;++c)for(unsigned ch=0;ch<3;++ch){
      if(n>=max)return n;
      if(t.kind==KIND_AY){unsigned v=s->regs[t.first+c][8+ch];out[n++]=uint8_t((v&16)?192:(v&15)*17);continue;}
      const FmChip* f=s->fm[t.first+c];const unsigned mask=CARRIERS[f->regs[0xb0+ch]&7];unsigned level=0;
      for(unsigned op=0;op<4;++op)if(mask&(1u<<op)){unsigned tl=f->regs[0x40+4*op+ch]&127;unsigned v=255-(tl*2);if(v>level)level=v;}
      out[n++]=uint8_t(f->keys[ch]?level:0);
    }
  }
  return n;
}
int probe(const uint8_t* d,size_t n){
  if(!d)return 0;
  if((n>=6&&!memcmp(d,"TFMcom",6)) || (n>=4&&!memcmp(d,"TFMD",4)) || (n>=4&&!memcmp(d,"MTC1",4)))return 100;
  return supported(file_type())?70:0;
}
void* open_source(uint32_t size,const uint8_t* data){
  const char* type=file_type();if(!data || !supported(type))return nullptr;
  const bool big=!strcmp(type,"MTC") || !strcmp(type,"TFD");
  if(size>(big?1048576u:131072u))return nullptr;
  auto* s=static_cast<Source*>(host->alloc(sizeof(Source),ZM_MEM_PSRAM));if(!s)return nullptr;memset(s,0,sizeof(*s));
  fm_kernel_init();
  bool opened;
  if(!strcmp(type,"MTC"))opened=parse_mtc(s,data,size);
  else if(!strcmp(type,"TFC"))opened=add_tfc(s,data,size);
  else if(!strcmp(type,"TFD"))opened=add_tfd(s,data,size);
  else if(!strcmp(type,"TS"))opened=add_ts(s,data,size);
  else opened=add_ay(s,data,size,type);
  if(!opened){close_source(s);return nullptr;}
  // Частота вывода — по микросхемам песни; такт дорожек — от неё.
  s->rate=s->fms?FM_SONG_RATE:AY_RATE;
  for(unsigned k=0;k<s->tracks;++k){Track& t=s->track[k];if(!t.frequency){close_source(s);return nullptr;}t.samples=s->rate/t.frequency;}
  // Название и автор: у MTC — из его блоков, иначе — первой дорожки.
  const Track& first=s->track[0];
  if(!s->info.title[0] && !s->info.artist[0]){
    if(first.kind==KIND_AY){snprintf(s->info.title,64,"%s",tsfm::ay_title(first.player));snprintf(s->info.artist,64,"%s",tsfm::ay_author(first.player));}
    else if(first.kind==KIND_TFC){memcpy(s->info.title,s->tfc[first.index].title,64);memcpy(s->info.artist,s->tfc[first.index].author,64);}
    else {memcpy(s->info.title,s->tfd[first.index].title,64);memcpy(s->info.artist,s->tfd[first.index].author,64);}
  }
  // Микросхемы: FM и первые два AY — во внутренней памяти, если она есть.
  for(unsigned c=0;c<s->fms;++c){void* mem=fast_alloc(sizeof(FmChip));if(mem)s->fm[c]=new(mem) FmChip(s->rate);if(!s->fm[c]){close_source(s);return nullptr;}}
  for(unsigned c=0;c<s->ays;++c){
    s->ay[c]=static_cast<ayumi*>(c<2?fast_alloc(sizeof(ayumi)):host->alloc(sizeof(ayumi),ZM_MEM_PSRAM));
    if(!s->ay[c]){close_source(s);return nullptr;}
  }
  for(unsigned c=0;c<s->fms;++c){s->buffer[c]=static_cast<float*>(host->alloc(sizeof(float)*ZM_RENDER_MAX_FRAMES,ZM_MEM_PSRAM));if(!s->buffer[c]){close_source(s);return nullptr;}}
  for(unsigned c=0;c<s->ays;++c){s->buffer[MAX_FM+c]=static_cast<float*>(host->alloc(sizeof(float)*2*ZM_RENDER_MAX_FRAMES,ZM_MEM_PSRAM));if(!s->buffer[MAX_FM+c]){close_source(s);return nullptr;}}
  plan_split(s);
  for(unsigned k=0;k<s->tracks;++k){const Track& t=s->track[k];const uint64_t length=uint64_t(t.ticks)*t.samples;if(length>s->total)s->total=length;}
  if(!s->total || s->total>uint64_t(s->rate)*3600){close_source(s);return nullptr;}
  reset_chips(s);
  float peak=0;
  const float loudness=estimate_loudness(s,peak);
  float gain_db=loudness>-99.0f?TARGET_LUFS-loudness:0.0f;
  // Песня с громкими пиками поднимается, только пока оценённый пик не выше
  // +1 дБ (PEAK_ALLOWED): остаток снимает ограничитель — не больше 1–2 дБ и на
  // мгновение, иначе он держал бы пики подолгу.
  if(peak>0.0f){const float cap=20.0f*log10f(PEAK_ALLOWED/peak);if(gain_db>cap)gain_db=cap;}
  if(gain_db<MIN_GAIN_DB)gain_db=MIN_GAIN_DB;
  if(gain_db>MAX_GAIN_DB)gain_db=MAX_GAIN_DB;
  s->gain=powf(10.0f,gain_db/20.0f);s->release=1.0f-expf(-1.0f/(RELEASE_SECONDS*float(s->rate)));
  log_line("tsfm: level %.1f LUFS estimated, gain %+.1f dB",loudness,gain_db);
  if(!reset_tracks(s)){close_source(s);return nullptr;}
  reset_chips(s);
  memset(&s->prof,0,sizeof(s->prof));
  // Что звучит — по микросхемам (тип файла плагин пишет сам, перед строкой);
  // одиночный AY — просто «AY» (1.3 и раньше писали «TSFM PT3 AY» и на обычной
  // музыке AY).
  char* format=s->info.format;const size_t room=sizeof(s->info.format);
  if(s->ays && s->fms)snprintf(format,room,"%uxAY+%uxYM2203",s->ays,s->fms);
  else if(s->fms==2)snprintf(format,room,"TurboSound FM 2xYM2203");
  else if(s->fms)snprintf(format,room,"%uxYM2203",s->fms);
  else if(s->ays==2)snprintf(format,room,"TurboSound 2xAY");
  else if(s->ays>2)snprintf(format,room,"%uxAY",s->ays);
  else snprintf(format,room,"AY");
  s->info.rate=s->rate;s->info.channels=2;s->info.voices=uint8_t((s->ays+s->fms)*3);
  s->info.duration_ms=uint32_t(s->total*1000/s->rate);
  return s;
}
}
#if defined(__XTENSA__)
void* operator new(size_t n){return fast_alloc(n);}
void* operator new[](size_t n){return fast_alloc(n);}
void operator delete(void* p) noexcept {if(p)host->free(p);}
void operator delete[](void* p) noexcept {if(p)host->free(p);}
void operator delete(void* p,size_t) noexcept {if(p)host->free(p);}
void operator delete[](void* p,size_t) noexcept {if(p)host->free(p);}
// Unused ymfm save-state/logging paths refer to C++ runtime helpers.
// As in OPL.ELF, the decoder has no dependency on the C++ shared runtime.
extern "C" void __cxa_pure_virtual(){for(;;){}}
namespace std {void __throw_length_error(const char*){for(;;){}}}
#endif
extern "C" int main(int argc,char* argv[]){
  if(argc<3 || !argv[1] || !argv[2])return 0;host=reinterpret_cast<const zm_host_t*>(argv[1]);
  if(host->abi_major!=ZM_SOURCE_ABI_MAJOR || host->size<offsetof(zm_host_t,free)+sizeof(host->free) || !host->alloc || !host->free)return 0;
  memset(&table,0,sizeof(table));table.size=sizeof(table);table.abi_major=ZM_SOURCE_ABI_MAJOR;table.abi_minor=ZM_SOURCE_ABI_MINOR;
  table.name="tsfm";table.version="1.12";table.extensions="AS0 ASC FTC GTR PSC PSM PT1 PT2 PT3 SQT ST3 STC STP TS TFC TFD MTC";
  table.input=ZM_INPUT_WHOLE;table.keeps_file=1;table.seek_unit=ZM_SEEK_MS;
  table.probe=probe;table.open=open_source;table.render=render;table.seek=seek;table.info=info;table.levels=levels;table.close=close_source;
  *reinterpret_cast<const zm_source_t**>(argv[2])=&table;return ZM_SOURCE_ABI_MAJOR;
}
