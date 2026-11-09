#include "Vamiga_video_geometry.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
static void fail(unsigned x,unsigned y,unsigned got,unsigned want,const char *s){
    fprintf(stderr,"%s at %u,%u: got %u expected %u\n",s,x,y,got,want);exit(1);
}
// 8 ноября 2026 (0.13.0): вывод 576p50 (864 x 625, активно 720 x 576) и 480p60 (858 x 525, 720 x 480).
// Размеры 100/95/90 % по центру; бит wide (16:9) меняет только флаг AVI. Окно DIW шире/выше кадра
// режется по центру до размера кадра (шаг источника 0 или 1 точка).
int main(){
  unsigned long long checked=0;
  for(unsigned pal=0;pal<2;pal++)for(unsigned mode=0;mode<16;mode++)
  for(unsigned sw:{640u,736u})for(unsigned sh:{200u,256u,400u,512u,576u}){
    if(!pal && sh==576)continue;
    Vamiga_video_geometry q;
    const unsigned HT=pal?864:858,VT=pal?625:525,AH=pal?576:480;
    q.pal=pal;q.total_width=HT;q.config_in=mode;
    q.source_x=40;q.source_width=sw;q.source_y=20;q.source_height=sh;
    auto tick=[&](){q.clk=0;q.eval();q.clk=1;q.eval();};
    q.hx=0;q.vy=0;q.reset=1;tick();q.reset=0;
    for(unsigned i=0;i<3;i++){q.vy=AH;tick();}
    q.hx=1;tick();
    // active and the DDA are registered one clock ahead: end of the line before the checked one.
    q.hx=HT-1;q.vy=VT-2;tick();
    unsigned W=(mode&3)==1?684:(mode&3)==2?648:720;
    unsigned H=(mode&3)==1?(pal?548:456):(mode&3)==2?(pal?520:432):AH;
    unsigned L=(720-W)/2,T=(AH-H)/2;
    unsigned ew=sw>W?W:sw,eh=sh>H?H:sh;
    unsigned sx=40+(sw-ew)/2,sy=20+(sh-eh)/2;
    if(q.wide!=((mode>>2)&1))fail(0,0,q.wide,(mode>>2)&1,"wide");
    // The final blanking line prepares source row zero of the next frame.
    for(unsigned y=VT-1;y<2*VT;y++)for(unsigned x=0;x<HT;x++){
      unsigned yy=y%VT;q.hx=x;q.vy=yy;q.clk=0;q.eval();
      bool a=x>=L&&x<L+W&&yy>=T&&yy<T+H;
      if(q.active!=a)fail(x,yy,q.active,a,"active");
      if(a){
        unsigned ex=sx+(x-L)*ew/W,ey=sy+(yy-T)*eh/H;
        if(q.sample_x!=ex)fail(x,yy,q.sample_x,ex,"source x");
        if(q.sample_y!=ey)fail(x,yy,q.sample_y,ey,"source y");
        checked++;
      }
      q.clk=1;q.eval();
    }
  }
  printf("PASS %llu active pixels: 576p50/480p60, games 200/256 rows, interlace 400/512/576, width 640/736 (crop), 4:3/wide, sizes 100/95/90%% and reserved config bits\n",checked);
}
