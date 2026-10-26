// Доступ к внутренним форматированию и фону без тестовых hooks в коде ESP.
#include "../player_ui.cpp"
#include <cstdlib>

int main() {
  using namespace zifi::player_view;
  struct TimeCase { uint32_t ms; const char* expected; };
  const TimeCase cases[] = {{0,"0:00"}, {999,"0:00"}, {1000,"0:01"}, {59999,"0:59"},
    {60000,"1:00"}, {3599999,"59:59"}, {3600000,"1:00:00"}, {3765000,"1:02:45"},
    {36000000,"10:00:00"}, {360000000,"100:00:00"}, {0xffffffffU,"1193:02:47"}};
  for (const auto& test : cases) {
    char out[24]; clockText(out,sizeof(out),test.ms);
    if (strcmp(out,test.expected)) { fprintf(stderr,"FAIL time %u: %s\n",unsigned(test.ms),out); return 1; }
  }
  static uint16_t canvas[kWidth*kHeight]; pixels=canvas; drawBackground();
  // Независимая исходная формула проверяется на всём фоне, включая закрытые обложкой точки.
  for (int y=0; y<kHeight; ++y) for (int x=0; x<kWidth; ++x) {
    const int light=hi(0,255-(x-125)*(x-125)/560-(y-120)*(y-120)/360);
    const uint16_t expected=dither({10+light*5/255,16+light*15/255,25+light*17/255},x,y);
    if (canvas[y*kWidth+x]!=expected) { fprintf(stderr,"FAIL background %d,%d\n",x,y); return 1; }
  }
  for (int font=0; font<3; ++font) for (unsigned code=0; code<256; ++code) {
    const char str[]={static_cast<char>(code),0};
    const int expected=code==0?0:pui_font::fonts[font].glyphs[(code<32?'?':code)-32].width;
    if (width(str,sizeof(str),font)!=expected) return 1;
  }
  puts("PASS 11 time strings / 230400 background pixels / 768 CP866 widths (NUL terminates)");
  return 0;
}
