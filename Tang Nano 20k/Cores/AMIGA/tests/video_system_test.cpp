#include "Vvideo_system_top.h"
#include "verilated.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>
static uint32_t colour(unsigned x,unsigned y){return ((x&255)<<16)|((y&255)<<8)|((x^y^85)&255);}
static unsigned decode(unsigned w){
  unsigned d=(w&255)^((w&512)?255:0),v=d&1;
  for(unsigned b=1;b<8;b++){unsigned bit=((d>>b)^(d>>(b-1)))&1;if(!(w&256))bit^=1;v|=bit<<b;}
  return v;
}
int main(int argc,char **argv){
  unsigned lines=argc>1?unsigned(atoi(argv[1])):200;
  bool lace=argc>2 && atoi(argv[2]);
  unsigned mode=argc>3?unsigned(atoi(argv[3])):0;
  bool pal=argc>4?bool(atoi(argv[4])):true;
  unsigned xstart=argc>5?unsigned(atoi(argv[5])):40;
  unsigned traffic=argc>6?unsigned(atoi(argv[6])):0;
  unsigned source_width=argc>7?unsigned(atoi(argv[7])):640;
  VerilatedContext context;Vvideo_system_top q(&context);
  uint64_t t=0,e28=198,e85=66,eh=209,es=165;
  unsigned nx=0,ny=0,field=0,frames=0,bad=0;bool req=0,done=0;std::ofstream trace("cache.csv");trace<<"us,x,y,event,slot,row,half\n";
  unsigned sy=lines==200?42:34;unsigned ybase=pal?23:20;
  struct Pixel{unsigned rgb;bool de;};Pixel history[16]={};
  uint64_t hcycles=0,encoded=0;unsigned symbols_bad=0,published_start=0,cpu_start=0,chip_start=0;
  uint64_t checked=0;unsigned misses_start=0,overruns_start=0,switches_start=0,black_start=0;bool started=false;
  unsigned read_start=0,write_start=0,refresh_start=0;
  std::ofstream bus("bus.csv");bus<<"us,y,video_reads,video_writes,cpu_reads,chip_reads,refreshes,misses,overruns\n";
  // 0.13.0: вывод 576p50 / 480p60 — 720 точек, 576/480 строк (pal источника задаёт развёртку).
  const unsigned AH=pal?576:480;
  std::vector<unsigned char> image(720*576*3);
  q.reset=1;q.clk28=0;q.clk85=0;q.hclk=0;q.sd_clk=0;q.picture=mode;
  q.traffic=traffic;q.hs=1;q.vs=1;q.window_in=0;q.lace=lace;q.field=0;q.rgb_in=0;q.eval();
  // Common time grid: 132 ticks per 85.5MHz period, 396 per 28.5MHz,
  // 418 per 27MHz (0.13.0; 720p was 152 per 74.25MHz). CLKOUTP rises 270 degrees after CLKOUT,
  // i.e. one quarter-cycle after the controller falling edge. 576p has fewer pixels per second:
  // 150 ms instead of 130, so that the checked interval still holds over 900000 pixels.
  const uint64_t hz=11286000000ull,end=hz*15/100;
  while(t<end){
    t=std::min(std::min(e28,e85),std::min(eh,es));
#ifdef SDRAM_IO_TIMING
    uint64_t ps=(t*1000000ull+5643ull)/11286ull;
    while(q.eventsPending() && q.nextTimeSlot()<ps){context.time(q.nextTimeSlot());q.eval();}
    context.time(ps);
#endif
    if(t>50000)q.reset=0;
    bool p28=t==e28&&!q.clk28,ph=t==eh&&!q.hclk;
    if(p28){
      if(!q.ready){nx=0;ny=0;field=0;}
      unsigned x=nx>=331?(nx-331)/2:0,y=ny>=ybase?ny-ybase:0;
      // 7 ноября 2026: VSYNC как у Agnus (agnus_beamcounter.v). Длинное поле (field 0) —
      // вместе с HSYNC в начале строки, короткое (field 1) в interlace — на полстроки раньше,
      // посреди последней строки предыдущего поля. Прежде оба поля шли как длинное,
      // и стенд не видел сдвига полей, который на плате давал гребёнку.
      // В Agnus поле меняется в начале кадра, VSYNC — позже: ко входу VSYNC field уже новое.
      bool fswap=ny==(pal?313u:263u)-1&&nx>=908;
      bool short_early=lace&&field==0&&fswap;
      q.hs=nx>=132;q.vs=!(ny<3||short_early);q.field=fswap?!field:field;
      q.window_in=nx>=331+2*xstart&&nx<331+2*(xstart+source_width)&&ny>=sy&&ny<sy+lines;
      q.rgb_in=q.window_in?colour(x,lace?2*y+field:y):0;
    }
    if(t==e28){q.clk28=!q.clk28;e28+=198;}
    if(t==e85){q.clk85=!q.clk85;e85+=66;}
    if(t==eh){q.hclk=!q.hclk;eh+=209;}
    if(t==es){q.sd_clk=!q.sd_clk;es+=66;}
    q.eval();
    if(req!=q.req || done!=q.done){trace<<double(t)*1e6/hz<<","<<q.hx<<","<<q.vy<<","<<(req!=q.req?"request":"done")<<","<<(q.req_info>>11)<<","<<((q.req_info>>1)&1023)<<","<<(q.req_info&1)<<"\n";req=q.req;done=q.done;}
    if(p28&&q.ready){if(++nx==1816){nx=0;if(++ny==(pal?313u:263u)){ny=0;field^=1;}}}
    if(ph){
      Pixel old=history[(hcycles-q.latency)&15];
      if(t>hz/10 && old.de){
        unsigned decoded=decode(q.t0)|(decode(q.t1)<<8)|(decode(q.t2)<<16);
        if(decoded!=old.rgb){if(symbols_bad++<5)fprintf(stderr,"TMDS got %06X expected %06X\n",decoded,old.rgb);}
        encoded++;
      }
      history[hcycles&15]={unsigned(q.rgb),bool(q.de)};hcycles++;
    }
    if(ph && t>hz/10 && q.de){
      if(!started){started=true;misses_start=q.diagnostic&255;overruns_start=q.diagnostic>>24;switches_start=(q.diagnostic>>20)&15;black_start=(q.diagnostic>>8)&255;published_start=q.frames_published;cpu_start=q.cpu_reads;chip_start=q.chip_reads;read_start=q.video_reads;write_start=q.video_writes;refresh_start=q.refreshes;}
      if(q.hx==0)bus<<double(t)*1e6/hz<<","<<q.vy<<","<<q.video_reads<<","<<q.video_writes<<","<<q.cpu_reads<<","<<q.chip_reads<<","<<q.refreshes<<","<<(q.diagnostic&255)<<","<<(q.diagnostic>>24)<<"\n";
      // 0.13.0: размеры 100/95/90 % от 720 x 576 (480); wide меняет только AVI. Окно шире/выше кадра —
      // середина размером с кадр (геометрия режет по центру).
      unsigned W=(mode&3)==1?684:(mode&3)==2?648:720;
      unsigned H=(mode&3)==1?(pal?548:456):(mode&3)==2?(pal?520:432):AH,L=(720-W)/2,T=(AH-H)/2;
      unsigned ew=source_width>W?W:source_width,eh2=2*lines>H?H:2*lines;
      unsigned expect=0;
      if(q.hx>=L&&q.hx<L+W&&q.vy>=T&&q.vy<T+H){
        unsigned x=xstart+(source_width-ew)/2+(q.hx-L)*ew/W;
        unsigned y=2*(sy-ybase)+(2*lines-eh2)/2+(q.vy-T)*eh2/H;
        if(x>=xstart&&x<xstart+source_width&&y>=2*(sy-ybase)&&y<2*(sy-ybase+lines))expect=colour(x,lace?y:y/2);
      }
      if(q.rgb!=expect){if(bad++<12)fprintf(stderr,"pixel %u,%u got %06X expected %06X info %llX geom %u,%u %u,%u diag %08X\n",q.hx,q.vy,q.rgb,expect,(unsigned long long)q.info,q.sx0,q.sy0,q.sw,q.sh,q.diagnostic);}
      checked++;
      if(q.hx<720&&q.vy<AH){size_t a=(q.vy*720+q.hx)*3;image[a]=q.rgb>>16;image[a+1]=q.rgb>>8;image[a+2]=q.rgb;}
      if(q.hx==719&&q.vy==AH-1)frames++;
    }
  }
  std::ofstream f("frame.ppm",std::ios::binary);f<<"P6\n720 "<<AH<<"\n255\n";f.write((char*)image.data(),720*AH*3);f.close();
  printf("source %u lace %u mode %u: %llu pixels, %u frames, %u bad, diagnostic %08X info %llX geometry %u,%u %ux%u\n",
      lines,lace,mode,(unsigned long long)checked,frames,bad,q.diagnostic,(unsigned long long)q.info,q.sx0,q.sy0,q.sw,q.sh);
  unsigned late_misses=(q.diagnostic&255)-misses_start;
  printf("traffic %u, misses in checked interval %u, chip data errors %u, CPU chip cycles held %u\n",traffic,late_misses,q.chip_bad,q.cpu_held);
  printf("SDRAM bursts in checked interval: video read %u write %u refresh %u\n",q.video_reads-read_start,q.video_writes-write_start,q.refreshes-refresh_start);
  printf("TMDS %llu pixels, %u bad; CPU %u chipset %u reads, %u fields published in checked interval\n",
    (unsigned long long)encoded,symbols_bad,q.cpu_reads-cpu_start,q.chip_reads-chip_start,q.frames_published-published_start);
  bool stalled=q.frames_published==published_start || (traffic && (q.cpu_reads==cpu_start || q.chip_reads==chip_start));
  q.final();return q.chip_bad||stalled||symbols_bad||encoded<900000||bad||late_misses||(q.diagnostic&255)||((q.diagnostic>>8)&255)!=black_start||((q.diagnostic>>20)&15)!=switches_start||checked<900000||((q.diagnostic>>24)!=overruns_start)?1:0; // 8.11.2026: переполнения по кругу
}
