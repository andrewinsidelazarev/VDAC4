#include "tests/pui_profile.h"
#include "player_ui.h"
#include "fonts/pui_font.h"

#include <stdio.h>
#include <string.h>

namespace zifi {
namespace player_view {
namespace {

struct Color { int r, g, b; };
constexpr Color white{233, 241, 243}, muted{146, 168, 181}, teal{92, 226, 198};
constexpr Color amber{255, 196, 101}, red{255, 120, 117}, line{38, 55, 66};
constexpr Color panel{18, 32, 43};
constexpr Rect whole{0, 0, kWidth, kHeight};
constexpr int bx = 248, by = 230, bw = 352, bh = 28;
constexpr uint8_t bayer[16] = {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};

// Неизменный фон: точные результаты прежней целочисленной формулы, во flash.
// Таблицы убирают деления и расчёт RGB565 для каждой точки полного кадра.
struct BackgroundTables {
  uint16_t shades[256][16];
  uint16_t dx[kWidth], dy[kHeight];
  constexpr BackgroundTables() : shades{}, dx{}, dy{} {
    for (int light=0; light<256; ++light) for (int phase=0; phase<16; ++phase) {
      const int d=bayer[phase];
      const int r=((10+light*5/255)*31*16/255+d)/16;
      const int g=((16+light*15/255)*63*16/255+d)/16;
      const int b=((25+light*17/255)*31*16/255+d)/16;
      shades[light][phase]=static_cast<uint16_t>((r<<11)|(g<<5)|b);
    }
    for (int x=0; x<kWidth; ++x) dx[x]=static_cast<uint16_t>((x-125)*(x-125)/560);
    for (int y=0; y<kHeight; ++y) dy[y]=static_cast<uint16_t>((y-120)*(y-120)/360);
  }
};
constexpr BackgroundTables background{};

int lo(int a, int b) { return a < b ? a : b; }
int hi(int a, int b) { return a > b ? a : b; }
int clamp(int a, int low, int high) { return lo(hi(a, low), high); }
uint16_t rgb(Color c) {
  return static_cast<uint16_t>(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
}
// Квантование с упорядоченным дизерингом; рисунок привязан к координатам холста.
uint16_t dither(Color c, int x, int y) { ++pui_profile::counts.dither;
  const int d = bayer[(y & 3) * 4 + (x & 3)];
  const int r = clamp(c.r * 31 * 16 / 255 + d, 0, 31 * 16) / 16;
  const int g = clamp(c.g * 63 * 16 / 255 + d, 0, 63 * 16) / 16;
  const int b = clamp(c.b * 31 * 16 / 255 + d, 0, 31 * 16) / 16;
  return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}
Color unpack(uint16_t p) {
  const int r = p >> 11, g = (p >> 5) & 63, b = p & 31;
  return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)};
}

struct State {
  uint16_t* base;
  Release release;
  void* context;
  bool begun, drawn;
  uint32_t headHash, lastMs, timeKey, infoHash;
  uint32_t valuesHash;              // формат и длительность (drawValues)
  int progress, volume, temperature;
  uint8_t levels[32], peaks[32];
  uint32_t holdUntil[32];
};
State s{};
uint16_t* pixels;
Rect changed;
Rect* output;
int outputCount, outputCapacity;

void resetChange() { changed = {kWidth,kHeight,0,0}; }
void put(int x, int y, uint16_t color) { ++pui_profile::counts.put;
  if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return;
  uint16_t& p = pixels[y * kWidth + x];
  if (p == color) return;
  ++pui_profile::counts.stores; p = color;
  changed.x0 = static_cast<uint16_t>(lo(changed.x0, x));
  changed.y0 = static_cast<uint16_t>(lo(changed.y0, y));
  changed.x1 = static_cast<uint16_t>(hi(changed.x1, x + 1));
  changed.y1 = static_cast<uint16_t>(hi(changed.y1, y + 1));
}
void flushChange() {
  if (changed.x0 >= changed.x1 || changed.y0 >= changed.y1) return;
  changed.x0 &= static_cast<uint16_t>(~1U);
  changed.x1 = static_cast<uint16_t>((changed.x1 + 1) & ~1U);
  if (outputCount < outputCapacity) output[outputCount++] = changed;
  else {
    // Даже при одном слоте ни одна изменённая точка не теряется.
    Rect& r = output[outputCount - 1];
    r.x0 = static_cast<uint16_t>(lo(r.x0, changed.x0));
    r.y0 = static_cast<uint16_t>(lo(r.y0, changed.y0));
    r.x1 = static_cast<uint16_t>(hi(r.x1, changed.x1));
    r.y1 = static_cast<uint16_t>(hi(r.y1, changed.y1));
  }
  resetChange();
}
void box(int x, int y, int w, int h, Color c) {
  const uint16_t value = rgb(c);
  for (int yy = hi(y,0); yy < lo(y+h,kHeight); ++yy)
    for (int xx = hi(x,0); xx < lo(x+w,kWidth); ++xx) put(xx,yy,value);
}
bool insideRound(int x, int y, int w, int h, int radius) { ++pui_profile::counts.round;
  const int dx = x < radius ? radius - x - 1 : (x >= w-radius ? x-(w-radius) : 0);
  const int dy = y < radius ? radius - y - 1 : (y >= h-radius ? y-(h-radius) : 0);
  return dx*dx+dy*dy <= radius*radius;
}
void roundBox(int x, int y, int w, int h, int radius, Color c) {
  for (int yy=0; yy<h; ++yy) for (int xx=0; xx<w; ++xx)
    if (insideRound(xx,yy,w,h,radius)) put(x+xx,y+yy,rgb(c));
}
void restore(int x, int y, int w, int h) { pui_profile::counts.restore += w*h;
  for (int yy=y; yy<y+h; ++yy) for (int xx=x; xx<x+w; ++xx)
    put(xx,yy,s.base[yy*kWidth+xx]);
}
const pui_font::Glyph& glyph(unsigned char ch, int font) {
  return pui_font::fonts[font].glyphs[(ch < 32 ? '?' : ch)-32];
}
int width(const char* str, size_t n, int font) {
  int w=0;
  for (size_t i=0; i<n && str[i]; ++i) w += glyph(static_cast<unsigned char>(str[i]),font).width;
  return w;
}
int text(int x, int y, const char* str, size_t n, int font, Color color, int right=608) {
  const auto& f = pui_font::fonts[font];
  for (size_t k=0; k<n && str[k]; ++k) {
    const auto& g = glyph(static_cast<unsigned char>(str[k]),font);
    if (x + g.width > right) break;
    for (int yy=0; yy<g.height; ++yy) for (int xx=0; xx<g.width; ++xx) {
      ++pui_profile::counts.glyph; const int index=yy*g.width+xx;
      const uint8_t packed=f.pixels[g.offset+index/2];
      const int a=(index&1) ? packed&15 : packed>>4;
      if (!a || x+xx<0 || x+xx>=kWidth || y+yy<0 || y+yy>=kHeight) continue;
      if (a==15) put(x+xx,y+yy,rgb(color));
      else {
        ++pui_profile::counts.alpha; Color bg=unpack(pixels[(y+yy)*kWidth+x+xx]);
        put(x+xx,y+yy,rgb({(bg.r*(15-a)+color.r*a+7)/15,
                          (bg.g*(15-a)+color.g*a+7)/15,
                          (bg.b*(15-a)+color.b*a+7)/15}));
      }
    }
    x += g.width;
  }
  return x;
}
void label(int x,int y,const char* str,int font,Color color,int right=608) {
  text(x,y,str,160,font,color,right);
}
void ellipsis(int x,int y,const char* str,size_t n,int font,Color color,int right=608) {
  if (width(str,n,font) <= right-x) { text(x,y,str,n,font,color,right); return; }
  const int dots=width("...",3,font);
  const int end=text(x,y,str,n,font,color,right-dots);
  label(end,y,"...",font,color,right);
}
void rightLabel(int right,int y,const char* str,int font,Color color) {
  label(right-width(str,160,font),y,str,font,color,right);
}
const char* stateName(const zm_ui_model_t& m) {
  switch(m.state) {
    case ZM_UI_LOADING: return "LOADING";
    case ZM_UI_BUFFERING: return "BUFFERING";
    case ZM_UI_PLAYING: return m.radio ? "ON AIR" : "NOW PLAYING";
    case ZM_UI_PAUSED: return "PAUSED";
    case ZM_UI_FINISHING: return "FINISHING";
    case ZM_UI_ERROR: return "PLAYBACK ERROR";
    default: return "READY TO LISTEN";
  }
}
Color stateColor(const zm_ui_model_t& m) {
  if (m.state==ZM_UI_ERROR) return red;
  if (m.state==ZM_UI_PAUSED || m.state==ZM_UI_LOADING || m.state==ZM_UI_BUFFERING) return amber;
  return teal;
}
void drawBackground() {
  for (int y=0; y<kHeight; ++y) for (int x=0; x<kWidth; ++x) {
    ++pui_profile::counts.background; const int light=hi(0,255-background.dx[x]-background.dy[y]);
    // Этот путь вызывается только для полного кадра: dirty уже будет всем холстом.
    pixels[y*kWidth+x]=background.shades[light][(y&3)*4+(x&3)];
  }
}
void brand(const zm_ui_model_t& m) {
  box(32,25,3,8,teal); box(38,21,3,12,teal); box(44,23,3,10,teal);
  label(57,20,"VDAC4",0,white);
  box(112,22,1,12,line);
  label(125,20,"MEDIA PLAYER",0,muted);
  char t[48];
  if (m.tracks) pui_profile::format(t,sizeof(t),"%s %u of %u",m.radio?"Station":"Track",unsigned(m.track),unsigned(m.tracks));
  else pui_profile::format(t,sizeof(t),"%s",m.radio?"INTERNET RADIO":"YOUR MUSIC, IN FOCUS");
  rightLabel(608,20,t,0,muted);
  box(32,47,576,1,line);
}
void drawCover(const zm_ui_model_t& m,const uint16_t* cover) {
  roundBox(28,62,200,198,10,{5,12,19});
  for (int y=0; y<kCoverSide; ++y) for (int x=0; x<kCoverSide; ++x) {
    if (!insideRound(x,y,kCoverSide,kCoverSide,6)) continue;
    if (cover) put(32+x,64+y,cover[y*kCoverSide+x]);
    else {
      const int dx=x-96,dy=y-96,dist=dx*dx+dy*dy;
      ++pui_profile::counts.placeholder; const int glow=hi(0,180-(dx*dx+dy*dy)/100);
      Color c{18+glow/18,33+glow/8,45+glow/7};
      if (dist>1400 && dist<6800 && ((dist/230)%3==0)) c={32+glow/14,61+glow/7,72+glow/6};
      put(32+x,64+y,dither(c,32+x,64+y));
    }
  }
  if (!cover) {
    roundBox(111,143,34,34,17,{89,215,192});
    roundBox(122,154,12,12,6,{16,40,49});
    const char* format=m.radio?"LIVE RADIO":(m.view==ZM_UI_VIEW_CHANNELS?"TRACKER / CHIP":"VDAC4 AUDIO");
    label(48,78,format,0,white,216);
    label(48,230,m.state==ZM_UI_IDLE?"SELECT A FILE":"NO ARTWORK",0,muted,216);
  }
}
void drawFooter(const zm_ui_model_t& m) {
  box(32,319,576,1,line);
  if (m.page==ZM_UI_PAGE_MAIN) {
    label(321,331,"Space",0,white); label(370,331,m.state==ZM_UI_PAUSED?"Play":"Pause",0,muted);
    label(426,331,"H",0,white); label(444,331,"Help",0,muted);
    label(488,331,"I",0,white); label(503,331,"Info",0,muted);
    label(548,331,"Esc",0,white); label(580,331,"Exit",0,muted);
  } else {
    rightLabel(608,331,"Esc  Back to player",0,white);
  }
}
void drawStations(const zm_ui_model_t& m) {
  label(bx,64,"INTERNET RADIO",0,teal);
  label(bx,85,"Stations",2,white);
  const int count=lo(m.count,ZM_UI_STATIONS), current=clamp(m.current,0,hi(0,count-1));
  const int first=clamp(current-2,0,hi(0,count-5));
  for (int r=0; r<5 && first+r<count; ++r) {
    const int i=first+r, y=123+r*27;
    const bool selected=i==current;
    if (selected) { roundBox(bx-4,y-2,360,25,4,{33,67,68}); box(bx-4,y+2,3,17,teal); }
    char index[12]; pui_profile::format(index,sizeof(index),"%02d",i+1);
    label(bx+7,y+2,index,0,selected?teal:muted);
    ellipsis(bx+38,y+2,m.stations[i],ZM_UI_STATION_TEXT,0,selected?white:muted,586);
    if (selected) box(591,y+9,4,4,teal);
  }
  if (!count) label(bx,145,"No stations in this playlist",1,muted);
  else if (count>5) {
    box(606,122,2,130,line);
    box(606,122+130*first/count,2,hi(8,130*5/count),teal);
  }
}
void drawMain(const zm_ui_model_t& m,const uint16_t* cover) {
  drawCover(m,cover);
  if (m.view==ZM_UI_VIEW_STATIONS) drawStations(m);
  else {
    label(bx,64,stateName(m),0,stateColor(m));
    if (m.radio && m.station[0])
      ellipsis(bx+width(stateName(m),32,0)+14,64,m.station,sizeof(m.station),0,white);
    const char* title=m.title[0]?m.title:(m.file[0]?m.file:"Nothing queued");
    if (m.radio && !m.title[0]) title=m.status[0]?m.status:(m.station[0]?m.station:"Internet radio");
    ellipsis(bx,86,title,ZM_UI_TEXT,2,white);
    if (m.artist[0]) ellipsis(bx,120,m.artist,sizeof(m.artist),1,teal);
    else if (m.state==ZM_UI_IDLE) label(bx,120,"Open a file in Wild Commander",1,muted);
    char album[80];
    pui_profile::format(album,sizeof(album),"%.*s%s%.*s",ZM_UI_TEXT,m.album,
             m.album[0]&&m.year[0]?"  /  ":"",int(sizeof(m.year)),m.year);
    ellipsis(bx,146,album,sizeof(album),0,muted);
    ellipsis(bx,196,m.detail,sizeof(m.detail),0,muted);
    if (m.state==ZM_UI_ERROR || m.state==ZM_UI_LOADING || m.state==ZM_UI_BUFFERING || m.state==ZM_UI_IDLE) {
      const Color color=stateColor(m);
      roundBox(bx,221,bw,37,5,m.state==ZM_UI_ERROR?Color{48,29,35}:Color{24,42,49});
      box(bx,228,3,23,color);
      const char* status=m.status[0]?m.status:(m.state==ZM_UI_IDLE?"Choose music to get started":stateName(m));
      ellipsis(bx+13,231,status,ZM_UI_TEXT,0,color,592);
    } else if (m.view==ZM_UI_VIEW_SPECTRUM || m.view==ZM_UI_VIEW_CHANNELS) {
      label(bx,213,m.view==ZM_UI_VIEW_SPECTRUM?"SPECTRUM":"CHANNEL LEVELS",0,muted);
      char t[32];
      if (m.view==ZM_UI_VIEW_SPECTRUM) pui_profile::format(t,sizeof(t),"40 Hz - 20 kHz");
      else pui_profile::format(t,sizeof(t),"%u channels",unsigned(lo(m.count,32)));
      rightLabel(600,213,t,0,muted);
      box(bx,by+bh,bw,1,line);
    } else if (m.status[0]) ellipsis(bx,234,m.status,sizeof(m.status),0,muted);
  }
  // Символ транспорта — геометрический, без зависимости от системного шрифта.
  roundBox(32,273,28,28,14,{28,58,60});
  if (m.state==ZM_UI_PAUSED) { box(42,281,3,11,amber); box(48,281,3,11,amber); }
  else if (m.state==ZM_UI_ERROR || m.state==ZM_UI_IDLE) box(42,282,9,9,stateColor(m));
  else for(int x=0;x<9;++x) box(43+x,280+x/2,1,13-x,teal);
  if (m.state==ZM_UI_LOADING) label(155,280,"Loading file",0,muted);
  else if (m.radio) label(155,280,"Listening time",0,muted);
  const char* hint=m.status[0]?m.status:(m.state==ZM_UI_FINISHING?"File sent / playing the queue":(m.radio?"LIVE STREAM":""));
  if (m.state==ZM_UI_ERROR || m.state==ZM_UI_LOADING || m.state==ZM_UI_BUFFERING) hint="";
  ellipsis(310,280,hint,ZM_UI_TEXT,0,m.radio?teal:muted);
  box(32,306,576,3,line);
}
void keyRow(int x,int y,const char* key,const char* description) {
  roundBox(x,y,95,26,4,{27,43,54});
  label(x+9,y+5,key,0,white,x+93);
  label(x+108,y+5,description,0,muted,x+280);
}
void drawHelp(const zm_ui_model_t& m) {
  label(32,66,"Playback controls",2,white);
  label(32,101,m.radio?"INTERNET RADIO / KEYBOARD CONTROLS":"FILE PLAYBACK / KEYBOARD CONTROLS",0,teal);
  keyRow(32,137,"Space","Pause / resume");
  keyRow(32,174,"Left / Right",m.radio?"Change station":"Seek 10 seconds");
  keyRow(32,211,"PgUp / PgDn",m.radio?"Change station":"Previous / next file");
  keyRow(32,248,"Up / Down","Adjust volume");
  keyRow(334,137,"I","Technical details");
  keyRow(334,174,"H","Show help");
  keyRow(334,211,"Esc","Close this page");
  label(334,254,"Esc on player: stop and exit",0,muted);
  label(32,289,m.radio?"Paused radio resumes from the same place.":"Tracker modules: Left / Right changes the order position.",0,muted);
}
constexpr int infoY=117;
void drawInfoStatic(const zm_ui_model_t& m) {
  label(32,66,"Technical details",2,white);
  label(32,101,"PLAYER & STREAM",0,teal);
  const char* names[]={"File","State","Format","Sent","Queue","Faults","Volume"};
  for(int i=0;i<7;++i) {
    const int y=infoY+i*25;
    if (!(i&1)) roundBox(32,y,576,25,3,panel);
    label(44,y+5,names[i],0,muted);
  }
  ellipsis(142,infoY+5,m.file,sizeof(m.file),0,white,596);
  ellipsis(142,infoY+30,m.status[0]?m.status:stateName(m),ZM_UI_TEXT,0,stateColor(m),596);
}
uint32_t hashBytes(uint32_t h,const void* data,size_t n) {
  const auto* b=static_cast<const uint8_t*>(data);
  for(size_t i=0;i<n;++i) { ++pui_profile::counts.hash; h=(h^b[i])*16777619U; }
  return h;
}
uint32_t hashString(uint32_t h,const char* str,size_t n) {
  for(size_t i=0;i<n && str[i];++i) { ++pui_profile::counts.hash; h=(h^static_cast<unsigned char>(str[i]))*16777619U; }
  return (h^0U)*16777619U;
}
uint32_t headHash(const zm_ui_model_t& m,bool cover) {
  uint32_t h=2166136261U;
  const uint8_t options[]={m.page,m.state,m.radio,m.view,m.count,m.current,static_cast<uint8_t>(cover)};
  h=hashBytes(h,options,sizeof(options));
  h=hashBytes(h,&m.track,sizeof(m.track)); h=hashBytes(h,&m.tracks,sizeof(m.tracks));
  h=hashBytes(h,&m.cover_serial,sizeof(m.cover_serial));
  // Формат (в нём битрейт — у VBR и FLAC он меняется от кадра к кадру) и длительность — не здесь:
  // их рисует drawValues своими участками, а не перерисовкой всего холста
  const char* strings[]={m.title,m.artist,m.album,m.file,m.station,m.detail,m.status};
  for(const char* str:strings) h=hashString(h,str,ZM_UI_TEXT);
  h=hashString(h,m.year,sizeof(m.year));
  if(m.view==ZM_UI_VIEW_STATIONS)
    for(int i=0;i<lo(m.count,ZM_UI_STATIONS);++i) h=hashString(h,m.stations[i],ZM_UI_STATION_TEXT);
  return h;
}
void clockText(char* out,size_t size,uint32_t ms) {
  const unsigned sec=ms/1000;
  if(sec>=3600) pui_profile::format(out,size,"%u:%02u:%02u",sec/3600,sec/60%60,sec%60);
  else pui_profile::format(out,size,"%u:%02u",sec/60,sec%60);
}
void drawTime(const zm_ui_model_t& m,bool full) {
  const uint32_t key=m.state==ZM_UI_LOADING?0x80000000U+lo(m.load_percent,100):m.played_ms/1000;
  if(full || s.timeKey!=key) {
    resetChange(); restore(70,278,83,19);
    char t[24];
    if(m.state==ZM_UI_LOADING) pui_profile::format(t,sizeof(t),"%u%%",unsigned(lo(m.load_percent,100)));
    else clockText(t,sizeof(t),m.played_ms);
    label(72,279,t,1,white,153);
    s.timeKey=key; flushChange();
  }
  int progress=0;
  if(m.state==ZM_UI_LOADING) progress=576*lo(m.load_percent,100)/100;
  else if(!m.radio && m.duration_ms)
    progress=static_cast<int>(static_cast<uint64_t>(m.played_ms<m.duration_ms?m.played_ms:m.duration_ms)*576U/m.duration_ms);
  if(full || progress!=s.progress) {
    resetChange();
    const int start=full?0:lo(progress,s.progress),finish=full?576:hi(progress,s.progress);
    for(int x=start;x<finish;++x) for(int y=306;y<309;++y)
      put(32+x,y,x<progress?rgb(m.state==ZM_UI_LOADING?amber:teal):s.base[y*kWidth+32+x]);
    s.progress=progress; flushChange();
  }
}
void drawVitals(const zm_ui_model_t& m,bool full) {
  if(full || s.volume!=m.volume) {
    resetChange(); restore(32,330,126,17);
    char t[20]; pui_profile::format(t,sizeof(t),"Vol %u%%",unsigned(lo(m.volume,100)));
    label(32,331,t,0,white);
    for(int i=0;i<10;++i) box(110+i*4,336,2,5,i*10<m.volume?teal:line);
    s.volume=m.volume; flushChange();
  }
  if(full || s.temperature!=m.temperature) {
    resetChange(); restore(174,330,140,17);
    char t[24];
    if(m.temperature==ZM_UI_TEMPERATURE_UNKNOWN) pui_profile::format(t,sizeof(t),"ESP --");
    else pui_profile::format(t,sizeof(t),"ESP %d%cC",int(m.temperature),static_cast<char>(0xf8));
    const Color c=m.temperature>=75?red:(m.temperature>=70?amber:muted);
    label(174,331,t,0,c);
    if(m.temperature>=70) label(259,331,m.temperature>=75?"HOT":"WARM",0,c);
    s.temperature=m.temperature; flushChange();
  }
}
// Формат и длительность — поверх фона полной перерисовки (s.base), своими участками: меняются они
// часто, а весь холст — это 115 200 слов в Tang.
void drawValues(const zm_ui_model_t& m,bool full) {
  const bool mainFormat=m.page==ZM_UI_PAGE_MAIN && m.view!=ZM_UI_VIEW_STATIONS;
  const bool duration=m.page==ZM_UI_PAGE_MAIN && m.state!=ZM_UI_LOADING && !m.radio;
  const unsigned sec=m.duration_ms/1000;
  uint32_t h=hashString(2166136261U,m.format,sizeof(m.format));
  const uint8_t where[]={m.page,static_cast<uint8_t>(mainFormat),static_cast<uint8_t>(duration),
                         static_cast<uint8_t>(m.duration_ms!=0)};
  h=hashBytes(h,where,sizeof(where)); h=hashBytes(h,&sec,sizeof(sec));
  if(!full && h==s.valuesHash) return;
  s.valuesHash=h;
  if(mainFormat) {
    resetChange(); restore(bx,176,608-bx,15);
    ellipsis(bx,176,m.format,sizeof(m.format),0,white);
    flushChange();
  } else if(m.page==ZM_UI_PAGE_INFO) {
    resetChange(); restore(142,infoY+50,456,25);
    ellipsis(142,infoY+55,m.format,sizeof(m.format),0,white,596);
    flushChange();
  }
  if(duration) {
    resetChange(); restore(155,278,150,19);
    if (m.duration_ms) {
      char t[24];
      if (sec>=3600) pui_profile::format(t,sizeof(t),"/ %u:%02u:%02u",sec/3600,sec/60%60,sec%60);
      else pui_profile::format(t,sizeof(t),"/ %u:%02u",sec/60,sec%60);
      label(155,279,t,1,muted);
    } else label(155,280,"/ --:--",0,muted);
    flushChange();
  }
}
void drawInfoValues(const zm_ui_model_t& m,bool full) {
  uint32_t h=hashBytes(2166136261U,&m.sent_kb,sizeof(m.sent_kb));
  h=hashBytes(h,&m.queue_bytes,sizeof(m.queue_bytes));
  h=hashBytes(h,&m.faults_decode,sizeof(m.faults_decode));
  h=hashBytes(h,&m.faults_starve,sizeof(m.faults_starve));
  h=hashBytes(h,&m.faults_i2s,sizeof(m.faults_i2s));
  h=hashBytes(h,&m.volume,sizeof(m.volume));
  if(!full && h==s.infoHash) return;
  s.infoHash=h;
  char t[96];
  for(int i=3;i<7;++i) {
    resetChange(); const int y=infoY+i*25;
    restore(142,y,456,25);
    switch(i) {
      case 3: pui_profile::format(t,sizeof(t),"%u KB delivered to ESP",unsigned(m.sent_kb)); break;
      case 4: pui_profile::format(t,sizeof(t),"%u bytes / %u KB",unsigned(m.queue_bytes),unsigned(m.queue_bytes/1024)); break;
      case 5: pui_profile::format(t,sizeof(t),"decode %u    starve %u    i2s %u",unsigned(m.faults_decode),unsigned(m.faults_starve),unsigned(m.faults_i2s)); break;
      default: pui_profile::format(t,sizeof(t),"%u%%",unsigned(lo(m.volume,100))); break;
    }
    label(142,y+5,t,0,i==5 && (m.faults_decode||m.faults_starve||m.faults_i2s)?amber:white,596);
    flushChange();
  }
}
void drawLevels(const zm_ui_model_t& m,uint32_t now,bool full,uint32_t dt) {
  if(m.view!=ZM_UI_VIEW_SPECTRUM && m.view!=ZM_UI_VIEW_CHANNELS) return;
  if(m.state!=ZM_UI_PLAYING && m.state!=ZM_UI_PAUSED && m.state!=ZM_UI_FINISHING) return;
  const int inputCount=lo(m.count,m.view==ZM_UI_VIEW_CHANNELS?32:64);
  const int n=m.view==ZM_UI_VIEW_SPECTRUM?(inputCount+1)/2:inputCount;
  if(n<=0) return;
  for(int group=0;group<8;++group) {
    resetChange();
    const int first=group*n/8,last=(group+1)*n/8;
    for(int i=first;i<last;++i) {
      int target=m.level[m.view==ZM_UI_VIEW_SPECTRUM?i*2:i];
      if(m.view==ZM_UI_VIEW_SPECTRUM && i*2+1<inputCount) target=hi(target,m.level[i*2+1]);
      if(m.state==ZM_UI_PAUSED) target=0;
      const int previous=s.levels[i],oldPeak=s.peaks[i];
      int next=previous;
      if(full && !s.drawn) next=target;
      else if(dt) {
        const int attack=hi(1,static_cast<int>(dt)*1800/1000);
        const int decay=hi(1,static_cast<int>(dt)*380/1000);
        next=target>previous?lo(target,previous+attack):hi(target,previous-decay);
      }
      s.levels[i]=static_cast<uint8_t>(next);
      if(next>=s.peaks[i]) { s.peaks[i]=static_cast<uint8_t>(next); s.holdUntil[i]=now+240; }
      else if(dt && static_cast<int32_t>(now-s.holdUntil[i])>=0)
        s.peaks[i]=static_cast<uint8_t>(hi(next,s.peaks[i]-hi(1,static_cast<int>(dt)*220/1000)));
      const int height=next*(bh-2)/255,peak=s.peaks[i]*(bh-2)/255;
      if(!full && previous*(bh-2)/255==height && oldPeak*(bh-2)/255==peak) continue;
      const int x0=bx+i*bw/n,x1=bx+(i+1)*bw/n-3;
      restore(x0,by,x1-x0,bh);
      for(int y=0;y<height;++y) {
        const int yy=by+bh-1-y;
        const Color c{54+y*2,135+y*3,137+y*2};
        for(int x=x0;x<x1;++x) put(x,yy,dither(c,x,yy));
      }
      if(peak>0) box(x0,by+bh-1-peak,x1-x0,1,m.state==ZM_UI_PAUSED?muted:teal);
    }
    flushChange();
  }
}

} // namespace

bool begin(Allocate allocate,Release release,void* context) {
  end();
  if(!allocate || !release) return false;
  s.base=static_cast<uint16_t*>(allocate(context,kWidth*kHeight*sizeof(uint16_t)));
  if(!s.base) return false;
  s.release=release; s.context=context; s.begun=true;
  return true;
}
void end() {
  if(s.base && s.release) s.release(s.context,s.base);
  memset(&s,0,sizeof(s));
}
int render(const zm_ui_model_t& m,const uint16_t* cover,bool coverChanged,bool full,
           uint32_t nowMs,uint16_t* canvas,Rect* dirty,int maxDirty) {
  if(!s.begun || !canvas || !dirty || maxDirty<=0) return 0;
  pixels=canvas; output=dirty; outputCapacity=maxDirty; outputCount=0;
  const uint32_t hash=headHash(m,cover!=nullptr);
  const bool repaint=full || !s.drawn || coverChanged || hash!=s.headHash;
  const uint32_t elapsed=nowMs-s.lastMs;
  const uint32_t dt=s.drawn?(elapsed>250?250:elapsed):0;
  s.lastMs=nowMs;
  if(repaint) {
    resetChange(); drawBackground(); brand(m);
    if(m.page==ZM_UI_PAGE_HELP) drawHelp(m);
    else if(m.page==ZM_UI_PAGE_INFO) drawInfoStatic(m);
    else drawMain(m,cover);
    drawFooter(m);
    pui_profile::counts.copied += kWidth*kHeight; memcpy(s.base,canvas,kWidth*kHeight*sizeof(uint16_t));
    s.headHash=hash;
  }
  drawValues(m,repaint);
  if(m.page==ZM_UI_PAGE_MAIN) { drawTime(m,repaint); drawLevels(m,nowMs,repaint,dt); }
  else if(m.page==ZM_UI_PAGE_INFO) drawInfoValues(m,repaint);
  drawVitals(m,repaint);
  s.drawn=true;
  if(repaint) { dirty[0]=whole; return 1; }
  return outputCount;
}

} // namespace player_view
} // namespace zifi
