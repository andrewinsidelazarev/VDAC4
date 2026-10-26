// Стенд превью: те же begin/render/end, что вызовет существующий player_overlay.
// Сохраняет точный RGB565; pui_preview.py разворачивает его в PNG по правилам Tang.
#include "player_ui.h"
#include "preview_text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace zifi::player_view;
namespace {
uint16_t canvas[kWidth*kHeight], coverPixels[kCoverSide*kCoverSide];
uint8_t spectrum[100][64];
bool haveSpectrum=false;
FILE* metrics=nullptr;
unsigned frames=0, maxArea=0;
uint64_t totalArea=0;
void* allocate(void*,size_t n) { return malloc(n); }
void release(void*,void* p) { free(p); }
template<size_t N> void set(char (&out)[N],const char* value) {
  snprintf(out,N,"%s",value);
}
void levels(zm_ui_model_t& m,int frame) {
  for(int i=0;i<64;++i) {
    if(haveSpectrum && m.view==ZM_UI_VIEW_SPECTRUM) m.level[i]=spectrum[frame%100][i];
    else m.level[i]=static_cast<uint8_t>(32+((i*37+frame*19+(i*frame*7)%97)%210));
  }
}
zm_ui_model_t base() {
  zm_ui_model_t m{};
  m.size=sizeof(m); m.visible=1; m.page=ZM_UI_PAGE_MAIN; m.state=ZM_UI_PLAYING;
  m.view=ZM_UI_VIEW_SPECTRUM; m.count=64;
  set(m.title,"After the Light"); set(m.artist,"Northbound"); set(m.album,"Quiet Horizons");
  set(m.year,"2026"); set(m.file,"03_AFTER.MP3");
  set(m.format,"MP3  128 kbps  44.1 kHz  stereo");
  m.track=3; m.tracks=10; m.played_ms=83000; m.duration_ms=225000;
  m.volume=72; m.temperature=47; m.cover_serial=1;
  m.sent_kb=2048; m.queue_bytes=131072;
  levels(m,0);
  return m;
}
bool save(const char* name) {
  char path[180]; snprintf(path,sizeof(path),"raw/%s.rgb565",name);
  FILE* f=fopen(path,"wb");
  if(!f) return false;
  const bool ok=fwrite(canvas,sizeof(canvas),1,f)==1;
  return fclose(f)==0 && ok;
}
void frame(zm_ui_model_t& m,const uint16_t* cover,bool full,uint32_t now,const char* series,int number) {
  Rect dirty[32];
  const int n=render(m,cover,full,full,now,canvas,dirty,32);
  unsigned area=0;
  for(int i=0;i<n;++i) area+=(dirty[i].x1-dirty[i].x0)*(dirty[i].y1-dirty[i].y0);
  if(metrics) fprintf(metrics,"%s,%d,%u,%d,%u\n",series,number,unsigned(now),n,area);
  if(!full) { ++frames; totalArea+=area; if(area>maxArea) maxArea=area; }
}
bool snapshot(const char* name,zm_ui_model_t m,bool artwork=true) {
  if(!begin(allocate,release,nullptr)) return false;
  if(!artwork) m.cover_serial=0;
  for(int i=0;i<14;++i) {
    levels(m,i);
    frame(m,artwork?coverPixels:nullptr,i==0,1000+i*40,name,i);
  }
  const bool ok=save(name); end(); return ok;
}
}
int main() {
  FILE* f=fopen("cover.rgb565","rb");
  if(!f) return 2;
  const bool coverOk=fread(coverPixels,sizeof(coverPixels),1,f)==1;
  fclose(f); if(!coverOk) return 3;
  f=fopen("spectrum.bin","rb");
  if(f) { haveSpectrum=fread(spectrum,sizeof(spectrum),1,f)==1; fclose(f); }
  metrics=fopen("preview_metrics.csv","w");
  if(!metrics) return 4;
  fprintf(metrics,"series,frame,now_ms,rectangles,pixels\n");
  zm_ui_model_t m=base();
  bool ok=snapshot("01_mp3",m);
  m=base(); set(m.title,kLongTitle); set(m.artist,kLongArtist); set(m.album,kAlbum);
  ok=snapshot("02_cyrillic",m)&&ok;
  m=base(); m.view=ZM_UI_VIEW_CHANNELS; m.count=8;
  set(m.title,"Midnight in 8 channels"); set(m.artist,"Tracker session"); set(m.album,"Module collection");
  set(m.year,""); set(m.file,"MIDNIGHT.XM"); set(m.format,"XM  8 channels  40 positions");
  set(m.detail,"Samples: 31  Effects: 12");
  ok=snapshot("03_xm_8_channels",m,false)&&ok;
  m.count=32; set(m.title,"Thirty-two voices"); set(m.format,"XM  32 channels  64 positions");
  ok=snapshot("04_xm_32_channels",m,false)&&ok;
  m=base(); set(m.title,"Neon circuitry"); set(m.artist,"FM archive"); set(m.album,""); set(m.year,"");
  set(m.file,"NEON.VGZ"); set(m.format,"VGZ  YMF262 (OPL3)"); set(m.detail,"18 channels / FM synthesis");
  m.view=ZM_UI_VIEW_CHANNELS; m.count=18;
  ok=snapshot("05_vgz_opl3",m,false)&&ok;
  m=base(); set(m.title,"Evening arrangement"); set(m.artist,"MIDI session"); set(m.album,""); set(m.year,"");
  set(m.file,"EVENING.MID"); set(m.format,"MIDI  16 channels  44.1 kHz");
  set(m.detail,"Instruments: 20  Tracks: 9");
  ok=snapshot("06_midi",m,false)&&ok;
  zm_ui_model_t radio=base(); radio.radio=1; radio.duration_ms=0; radio.played_ms=754000;
  radio.tracks=11; set(radio.station,"Nightwave FM"); set(radio.title,"A city after midnight");
  set(radio.artist,"Harbour Lights"); set(radio.album,"Live from the stream"); set(radio.year,"");
  set(radio.file,"RADIO.M3U"); set(radio.format,"M3U  HE-AAC  64 kbps  44.1 kHz  stereo");
  ok=snapshot("07_radio_icy",radio,false)&&ok;
  m=radio; m.view=ZM_UI_VIEW_STATIONS; m.count=11; m.current=2;
  const char* stations[]={"Ambient Space","Jazz Room","Nightwave FM","Radio Electronica",kStation,
                           "Classic Sessions","Deep Focus","Chiptune Radio","Morning Coffee","Orbit FM","Late Night"};
  for(int i=0;i<11;++i) set(m.stations[i],stations[i]);
  ok=snapshot("08_radio_stations",m,false)&&ok;
  m=base(); m.state=ZM_UI_LOADING; m.load_percent=45; m.played_ms=0;
  set(m.status,"Sending to the ESP: OPL.ELF");
  ok=snapshot("09_loading_45",m)&&ok;
  m=radio; m.state=ZM_UI_BUFFERING; set(m.status,"Buffering"); m.played_ms=0;
  ok=snapshot("10_buffering",m,false)&&ok;
  m=base(); m.state=ZM_UI_PAUSED;
  ok=snapshot("11_paused",m)&&ok;
  m=base(); m.state=ZM_UI_ERROR; set(m.status,"Cannot play this file, skipping");
  ok=snapshot("12_error_skipping",m)&&ok;
  set(m.status,"ERROR: ESP closed the stream");
  ok=snapshot("13_error_stream_closed",m)&&ok;
  m=base(); m.page=ZM_UI_PAGE_HELP;
  ok=snapshot("14_help_files",m)&&ok;
  m=radio; m.page=ZM_UI_PAGE_HELP;
  ok=snapshot("15_help_radio",m,false)&&ok;
  m=base(); m.page=ZM_UI_PAGE_INFO; m.faults_decode=2; m.faults_starve=1;
  ok=snapshot("16_details",m)&&ok;
  m=base(); m.state=ZM_UI_IDLE; m.view=ZM_UI_VIEW_NONE; m.tracks=0; m.played_ms=0; m.duration_ms=0;
  set(m.title,""); set(m.artist,""); set(m.album,""); set(m.year,""); set(m.file,""); set(m.format,"");
  ok=snapshot("17_idle",m,false)&&ok;
  m=base(); m.state=ZM_UI_FINISHING; m.played_ms=217000;
  ok=snapshot("18_finishing",m)&&ok;
  m=base(); m.temperature=70;
  ok=snapshot("19_temperature_70",m)&&ok;
  m.temperature=75;
  ok=snapshot("20_temperature_75",m)&&ok;
  m=base(); set(m.title,""); set(m.artist,""); set(m.album,""); set(m.year,"");
  set(m.file,"LIVE_SESSION_03.MP3");
  ok=snapshot("21_filename_fallback",m,false)&&ok;
  m=base(); m.duration_ms=0; m.played_ms=3765000;
  ok=snapshot("22_unknown_duration",m)&&ok;
  m=base(); set(m.file,"03_AFTER.FLAC"); set(m.format,"FLAC  24 bit  96 kHz  stereo");
  ok=snapshot("23_flac",m)&&ok;
  m=radio; m.state=ZM_UI_BUFFERING; set(m.title,""); set(m.artist,""); set(m.album,"");
  set(m.status,"Connecting..."); m.played_ms=0;
  ok=snapshot("24_connecting",m,false)&&ok;
  m=base(); m.view=ZM_UI_VIEW_NONE; set(m.status,"Playing");
  ok=snapshot("25_view_none",m)&&ok;
  m=base();
  if(!begin(allocate,release,nullptr)) return 5;
  for(int i=0;i<100;++i) {
    levels(m,i); m.played_ms=83000+i*40;
    frame(m,coverPixels,i==0,1000+i*40,"animation",i);
    char name[40]; snprintf(name,sizeof(name),"motion_%03d",i);
    ok=save(name)&&ok;
  }
  end(); fclose(metrics);
  printf("Preview: %u incremental frames, mean %llu pixels, max %u pixels\n",frames,
         static_cast<unsigned long long>(frames?totalArea/frames:0),maxArea);
  return ok?0:6;
}
