#define NOMINMAX
#include <windows.h>
#include "Vsim_top.h"
#include "verilated.h"
#include "verilated_vcd_c.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
constexpr uint64_t US=1000000,MS=1000*US,SECOND=1000*MS;
void require(bool x,const std::string&s){if(!x)throw std::runtime_error(s);}
uint32_t crc32(const std::vector<uint8_t>&v){uint32_t c=~0u;for(auto b:v){c^=b;for(int k=0;k<8;k++)c=(c>>1)^((c&1)?0xedb88320u:0);}return ~c;}
void atomicText(const fs::path&p,const std::string&s){auto tmp=p;tmp+=".tmp";{std::ofstream f(tmp,std::ios::binary);f<<s;}MoveFileExW(tmp.c_str(),p.c_str(),MOVEFILE_REPLACE_EXISTING);}
struct Frame {
 fs::path path;std::vector<std::vector<uint32_t>> rows;std::vector<uint32_t> line;
 uint64_t frames=0,lit=0;unsigned width=0,height=0;bool lastVs=true,lastHs=true;
 explicit Frame(fs::path p):path(p){}
 void finish(){
  if(rows.size()>=100){
   size_t w=0;for(auto&r:rows)w=std::max(w,r.size());
   if(w>=200&&w<=2048&&rows.size()<=1024){
    auto tmp=path;tmp+=".tmp";std::ofstream f(tmp,std::ios::binary);f<<"P6\n"<<w<<" "<<rows.size()<<"\n255\n";lit=0;
    for(auto&r:rows)for(size_t x=0;x<w;x++){uint32_t p=x<r.size()?r[x]:0;char c[3]={char(p>>16),char(p>>8),char(p)};f.write(c,3);lit+=p!=0;}
    f.close();MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING);width=unsigned(w);height=unsigned(rows.size());frames++;
   }
  }rows.clear();line.clear();
 }
 void sync(bool hs,bool vs){
  if(!lastVs&&vs)finish();
  if(!lastHs&&hs){if(!line.empty()&&rows.size()<1024)rows.push_back(std::move(line));line.clear();}
  lastHs=hs;lastVs=vs;
 }
 void pixel(uint32_t c){if(line.size()<2048)line.push_back(c);}
};
struct Media {
 fs::path path,cow;uint64_t bytes=0,reads=0,writes=0;std::ifstream file;std::map<uint32_t,std::array<uint8_t,512>> overlay;
 void open(fs::path p,fs::path c){path=p;cow=c;file.open(path,std::ios::binary);require(file.good(),"Cannot open disk");bytes=fs::file_size(path);require(bytes>0&&bytes<=0xffffffffu&&bytes%512==0,"Disk size must be a positive multiple of 512, <=4GB");
  std::ifstream old(cow,std::ios::binary);uint32_t n;std::array<uint8_t,512>a;while(old.read((char*)&n,4)&&old.read((char*)a.data(),512))overlay[n]=a;
 }
 std::array<uint8_t,512> read(uint32_t sector){require(uint64_t(sector)*512+512<=bytes,"Disk read past image end");reads++;if(overlay.count(sector))return overlay[sector];std::array<uint8_t,512>a{};file.clear();file.seekg(uint64_t(sector)*512);file.read((char*)a.data(),512);require(file.gcount()==512,"Short disk read");return a;}
 void write(uint32_t sector,std::array<uint8_t,512>a){require(uint64_t(sector)*512+512<=bytes,"Disk write past image end");overlay[sector]=a;writes++;std::ofstream f(cow,std::ios::binary|std::ios::app);f.write((char*)&sector,4);f.write((char*)a.data(),512);require(f.good(),"Cannot save disk overlay");}
};
static bool g_compat=false;   // --compat (0.13.3): режим совместимости до старта ROM
struct Sim {
 VerilatedContext ctx;Vsim_top q{&ctx};fs::path out;Frame native,hdmi;Clock::time_point started=Clock::now(),published=started;
 uint64_t t=0,oscEdge=18518,oscCount=1,cpuCount=0,audioCount=0,nextDisk=0,serialBits=0,traceEnd=0;
 uint64_t pEdges=0,fEdges=0,symbols=0,badSymbols=0;bool old28=false,oldF=false,oldP=false,oldAudio=false;
 uint16_t shift[3]{};int phase=-1;unsigned controlRun[10]{};uint64_t matched=0;std::deque<uint32_t> expected;std::deque<std::string> bus;std::vector<std::pair<uint32_t,uint16_t>> vectors;
 std::map<int,Media> media;std::ofstream audio;std::unique_ptr<VerilatedVcdC> trace;
 bool paused=false,stop=false,romVerified=false,fastBoot=false;std::string state="initializing",error,pendingInput;uint64_t stepUntil=0,stepFrame=0;std::array<uint16_t,4> expectedVectors{};std::ofstream boot;
 explicit Sim(fs::path p,bool fast):out(p),native(p/"native.ppm"),hdmi(p/"hdmi.ppm"){
  fs::create_directories(out);ctx.traceEverOn(true);fastBoot=fast;std::string plus="+ROMHEX="+(p/"private-rom.hex").string();const char*args[]={"amiga_sim",plus.c_str()};ctx.commandArgs(fast?2:1,args);
  q.sim_rom_ready=0;q.osc=0;q.gsr_n=0;q.buttons=0;q.link_sck=0;q.link_cs=1;q.link_drive=1;q.link_data=0;q.probe_address=0;q.eval();
  audio.open(out/"paula.wav",std::ios::binary);char zero[44]{};audio.write(zero,44);
  boot.open(out/"boot.csv");boot<<"time_ps,address,state,read,write\n";
 }
 ~Sim(){q.final();finishAudio();}
 void finishAudio(){if(!audio.is_open())return;uint32_t n=uint32_t(audioCount*4),size=n+36,rate=48000,byteRate=rate*4;uint16_t fmt=1,ch=2,align=4,bits=16;audio.seekp(0);audio.write("RIFF",4);audio.write((char*)&size,4);audio.write("WAVEfmt ",8);uint32_t sz=16;audio.write((char*)&sz,4);audio.write((char*)&fmt,2);audio.write((char*)&ch,2);audio.write((char*)&rate,4);audio.write((char*)&byteRate,4);audio.write((char*)&align,2);audio.write((char*)&bits,2);audio.write("data",4);audio.write((char*)&n,4);audio.close();}
 static int ctl(unsigned v){switch(v){case 0x354:return 0;case 0x0ab:return 1;case 0x154:return 2;case 0x2ab:return 3;default:return -1;}}
 static unsigned decode(unsigned w){unsigned m=(w&255)^((w&512)?255:0),d=m&1;for(int k=1;k<8;k++){unsigned b=((m>>k)^(m>>(k-1)))&1;if(!(w&256))b^=1;d|=b<<k;}return d;}
 void serial(){
  serialBits++;for(int c=0;c<3;c++)shift[c]=(shift[c]>>1)|(((q.serial_data>>c)&1)<<9);
  int slot=int(serialBits%10);bool control=ctl(shift[0])>=0&&shift[1]==0x354&&shift[2]==0x354;
  if(phase<0){if(controlRun[slot]=(control?controlRun[slot]+1:0);controlRun[slot]>=8)phase=slot;}
  if(phase!=slot)return;
  symbols++;uint32_t packed=uint32_t(shift[0])|(uint32_t(shift[1])<<10)|(uint32_t(shift[2])<<20);
  bool found=false;for(auto v:expected)found|=v==packed;if(found)matched++;else if(expected.size()>=3)badSymbols++;
  if(control){int c=ctl(shift[0]);hdmi.sync(c&1,c&2);}else hdmi.pixel((decode(shift[2])<<16)|(decode(shift[1])<<8)|decode(shift[0]));
 }
 void observe(){
  if(q.clk28&&!old28){native.sync(q.native_hs,q.native_vs);if(q.native_hs&&q.native_vs)native.pixel(q.native_rgb);}
  old28=q.clk28;
  if(q.pclk&&!oldP){pEdges++;expected.push_back(q.parallel_symbols);if(expected.size()>8)expected.pop_front();}oldP=q.pclk;
  if(q.fclk!=oldF){fEdges++;serial();oldF=q.fclk;}
  if(q.cpu_steps!=cpuCount){cpuCount=q.cpu_steps;
   std::ostringstream s;s<<t<<","<<std::hex<<std::setfill('0')<<std::setw(8)<<q.completed_address<<","<<unsigned(q.completed_state)<<","<<std::setw(4)<<q.completed_read<<","<<std::setw(4)<<q.completed_write;
   bus.push_back(s.str());if(bus.size()>4096)bus.pop_front();if(cpuCount<=128){boot<<s.str()<<"\n";boot.flush();}
   if(vectors.size()<4&&q.completed_state!=3&&q.completed_address<8){vectors.emplace_back(q.completed_address,q.completed_read);unsigned i=unsigned(vectors.size()-1);require(q.completed_address==i*2&&q.completed_read==expectedVectors[i],"CPU reset-vector read mismatch; see boot.csv");}
  }
  if(q.audio_toggle!=oldAudio){oldAudio=q.audio_toggle;uint32_t a=q.audio_sample;audio.write((char*)&a,4);audioCount++;}
  if(trace){trace->dump(t);if(t>=traceEnd){trace->close();trace.reset();}}
 }
 void evaluate(){q.eval();observe();}
 void advance(uint64_t duration){uint64_t end=t+duration;while(t<end){uint64_t next=std::min(end,oscEdge);if(q.eventsPending())next=std::min(next,q.nextTimeSlot());require(next>=t,"RTL event in the past");t=next;ctx.time(t);if(t==oscEdge){q.osc=!q.osc;oscCount++;oscEdge=oscCount*500000/27;}evaluate();if(ctx.gotFinish())throw std::runtime_error("RTL $finish");}}
 unsigned cycle(unsigned v){q.link_data=v;evaluate();advance(25000);q.link_sck=1;evaluate();unsigned got=q.link_reply;advance(25000);q.link_sck=0;evaluate();return got;}
 void begin(unsigned cmd,uint32_t a){q.link_cs=0;q.link_drive=1;evaluate();for(int i=7;i>=0;i--)cycle((cmd>>i)&1);for(int i=7;i>=0;i--)cycle((a>>(i*4))&15);}
 void end(){q.link_cs=1;q.link_drive=1;evaluate();advance(100000);}
 void write(uint32_t a,const std::vector<uint32_t>&w){begin(1,a);for(auto v:w)for(int i=7;i>=0;i--)cycle((v>>(i*4))&15);end();}
 void wr(uint32_t a,uint32_t v){write(a,{v});advance(100000);}
 uint32_t rd(uint32_t a){begin(2,a);q.link_drive=0;evaluate();for(int i=0;i<8;i++)cycle(0);uint32_t v=0;for(int i=0;i<8;i++)v=(v<<4)|cycle(0);end();return v;}
 void report(bool force=false){auto now=Clock::now();if(!force&&now-published<std::chrono::milliseconds(500))return;published=now;
  std::ostringstream j;j<<"{\"state\":\""<<state<<"\",\"paused\":"<<(paused?"true":"false")<<",\"sim_seconds\":"<<double(t)/SECOND<<",\"wall_seconds\":"<<std::chrono::duration<double>(now-started).count()<<",\"rom_verified\":"<<(romVerified?"true":"false")<<",\"sys_lock\":"<<unsigned(q.sys_lock)<<",\"video_lock\":"<<unsigned(q.video_lock)<<",\"cpu_reset_n\":"<<unsigned(q.reset_n)<<",\"cpu_steps\":"<<cpuCount<<",\"cpu_address\":"<<q.cpu_address<<",\"native_frames\":"<<native.frames<<",\"hdmi_frames\":"<<hdmi.frames<<",\"hdmi_lock\":"<<(phase>=0?"true":"false")<<",\"hdmi_symbols\":"<<symbols<<",\"hdmi_mismatches\":"<<badSymbols<<",\"hdmi_matches\":"<<matched<<",\"native_lit\":"<<native.lit<<",\"hdmi_lit\":"<<hdmi.lit<<",\"pixel_edges\":"<<pEdges<<",\"serializer_edges\":"<<fEdges<<",\"video_diag\":"<<q.video_diag<<",\"disks\":[";
  bool first=true;for(auto&[id,m]:media){if(!first)j<<",";first=false;j<<"{\"unit\":"<<id<<",\"reads\":"<<m.reads<<",\"writes\":"<<m.writes<<"}";}j<<"]}";atomicText(out/"status.json",j.str());
  std::ostringstream b;b<<"time_ps,address,state,read,write\n";for(auto&s:bus)b<<s<<"\n";atomicText(out/"cpu.csv",b.str());
 }
 void loadRom(fs::path path,bool fast){state=fast?"fast ROM initialization":"loading ROM through QSPI";std::ifstream f(path,std::ios::binary);require(f.good(),"Cannot open ROM");std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),{});require(bytes.size()==524288,"ROM must be 512 KiB");for(int i=0;i<4;i++)expectedVectors[i]=(uint16_t(bytes[i*2])<<8)|bytes[i*2+1];
  advance(10*US);q.gsr_n=1;evaluate();advance(500*US);require(q.sys_lock,"System PLL did not lock");require(q.sdram_ready,"SDRAM not initialized");
  require(rd(0)==0x56444334,"QSPI core identity mismatch");std::cout<<"LINK version "<<std::hex<<rd(4)<<std::dec<<std::endl;
  // 8.11.2026 (0.13.3): --compat — режим совместимости (68000, без fast RAM, без турбо) до старта ROM, процессор в сбросе.
  if(g_compat){wr(0x1ac,1);require(rd(0x1ac)==1,"compat register 0x1AC");require(rd(0x1b0)==0x414D4331,"no AMC1");std::cout<<"compatibility mode: 68000"<<std::endl;}
  if(!fast){wr(0x44,1);wr(0x58,crc32(bytes));
  for(unsigned seq=1;seq<=2048;seq++){std::vector<uint32_t>w(64);for(int i=0;i<256;i++)w[i/4]|=uint32_t(bytes[(seq-1)*256+i])<<(8*(i%4));write(0x4000,w);wr(0x48,(seq<<16)|256);
   unsigned tries=0;while(rd(0x44)&2){advance(2*US);require(++tries<100,"ROM block timeout");}require(rd(0x48)==seq&&rd(0x54)==0,"ROM block rejected");report();input();while(paused&&!stop){report();Sleep(25);input();}if(stop){state="stopped";report(true);return;}if(seq%256==0)std::cout<<"ROM "<<seq/2048.0*100<<"%"<<std::endl;
  }
  require(rd(0x4c)==bytes.size()&&rd(0x50)==crc32(bytes),"ROM transfer CRC mismatch");}
  for(unsigned n=0;n<bytes.size()/4;n++){q.probe_address=0x1e0000+n;q.eval();uint32_t want=(uint32_t(bytes[n*4])<<24)|(uint32_t(bytes[n*4+1])<<16)|(uint32_t(bytes[n*4+2])<<8)|bytes[n*4+3];require(q.probe_data==want,"SDRAM ROM mismatch at byte "+std::to_string(n*4)+" got "+std::to_string(q.probe_data)+" expected "+std::to_string(want));}
  romVerified=true;state="ROM verified; CPU starting";if(fast){q.sim_rom_ready=1;evaluate();}else wr(0x44,2);report(true);
 }
 void mount(int id,fs::path p){media[id].open(p,out/("unit"+std::to_string(id)+".cow"));wr(0x194,id);wr(0x180,uint32_t(media[id].bytes));wr(0x194,0);}
 void disks(){if(media.empty()||t<nextDisk)return;nextDisk=t+50*US;unsigned s=rd(0x184);if(!(s&1))return;require(!(s&4),"Disk mailbox error");if(!(s&2))return;unsigned mask=(s>>8)&255;int unit=mask&0x20?5:mask&0x10?4:0;require(media.count(unit),"Guest requested unmounted disk");auto sector=rd(0x188),token=rd(0x18c);auto&m=media[unit];std::vector<uint32_t>w(128);
  if(s&8){begin(2,0x5000);q.link_drive=0;evaluate();for(int i=0;i<8;i++)cycle(0);for(auto&v:w)for(int k=0;k<8;k++)v=(v<<4)|cycle(0);end();std::array<uint8_t,512>a{};for(int i=0;i<512;i++)a[i]=w[i/4]>>(8*(i%4));m.write(sector,a);}
  else {auto a=m.read(sector);for(int i=0;i<512;i++)w[i/4]|=uint32_t(a[i])<<(8*(i%4));write(0x5000,w);}wr(0x190,token);
 }
 void command(const std::string&line){std::istringstream s(line);std::string cmd;s>>cmd;if(cmd=="pause")paused=true;else if(cmd=="run")paused=false;else if(cmd=="stop")stop=true;else if(cmd=="frame"){paused=false;stepUntil=t+40*MS;stepFrame=hdmi.frames+1;}else if(cmd=="reset"){if(fastBoot){q.sim_rom_ready=0;evaluate();advance(10*US);q.sim_rom_ready=1;evaluate();}else wr(0x44,3);}else if(cmd=="key"){unsigned code,up;s>>code>>up;wr(0x10,0x101);wr(0x10,(code&127)|(up?128:0));}else if(cmd=="mouse"){int b,x,y;s>>b>>x>>y;wr(0x10,0x102);wr(0x10,b&7);wr(0x10,x&255);advance(2*US);wr(0x10,y&255);}else if(cmd=="video"){unsigned flags;s>>flags;wr(0x1a0,flags&15);}else if(cmd=="osd"){unsigned show;s>>show;if(show){std::vector<uint32_t>text(16*32,0);std::string label="SIMULATOR  QSPI OSD";for(unsigned i=0;i<label.size();i++){uint8_t c=0xc0|((label[i]-32)&63);text[16+i/4]=(text[16+i/4]&~(255u<<((i%4)*8)))|(uint32_t(c)<<((i%4)*8));}write(0x3000,text);}wr(0xc0,show?1:0);}else if(cmd=="regs"){std::ostringstream r;r<<"{";for(unsigned a=0;a<=0xe8;a+=4){if(a)r<<",";r<<"\"0x"<<std::hex<<a<<"\":"<<std::dec<<rd(a);}r<<"}";atomicText(out/"registers.json",r.str());}else if(cmd=="trace"&&!trace){trace=std::make_unique<VerilatedVcdC>();q.trace(trace.get(),20);trace->open((out/"wave.vcd").string().c_str());traceEnd=t+10*US;}}
 void input(){
  HANDLE h=GetStdHandle(STD_INPUT_HANDLE);DWORD available=0;
  while(PeekNamedPipe(h,nullptr,0,nullptr,&available,nullptr)&&available){
   char chunk[4096];DWORD received=0;
   if(!ReadFile(h,chunk,std::min<DWORD>(available,sizeof(chunk)),&received,nullptr)||!received)break;
   pendingInput.append(chunk,received);
  }
  size_t newline;
  while((newline=pendingInput.find('\n'))!=std::string::npos){
   auto line=pendingInput.substr(0,newline);pendingInput.erase(0,newline+1);command(line);
  }
 }
 void run(double seconds,bool interactive){uint64_t end=t+uint64_t(seconds*SECOND);state="running";report(true);while(!stop&&(interactive||t<end)){input();if(paused){report();Sleep(25);continue;}advance(20*US);disks();if(stepUntil&&(t>=stepUntil||hdmi.frames>=stepFrame)){paused=true;stepUntil=0;}report();}state="stopped";report(true);
  std::ostringstream v;v<<"[";for(size_t i=0;i<vectors.size();i++){if(i)v<<",";v<<"["<<vectors[i].first<<","<<vectors[i].second<<"]";}v<<"]";atomicText(out/"vectors.json",v.str());
 }
};
int wmain(int argc,wchar_t**argv){fs::path rom,out="run";std::vector<std::pair<int,fs::path>> disks;double seconds=1;bool interactive=false,fast=false;
 try{for(int i=1;i<argc;i++){std::wstring a=argv[i];auto arg=[&](){require(i+1<argc,"Missing option value");return std::wstring(argv[++i]);};if(a==L"--rom")rom=arg();else if(a==L"--compat")g_compat=true;else if(a==L"--output")out=arg();else if(a==L"--seconds")seconds=std::stod(arg());else if(a==L"--adf")disks.emplace_back(0,arg());else if(a==L"--hdf")disks.emplace_back(4,arg());else if(a==L"--hdf1")disks.emplace_back(5,arg());else if(a==L"--interactive")interactive=true;else if(a==L"--fast-rom")fast=true;else throw std::runtime_error("Unknown option");}require(!rom.empty(),"--rom is required");if(fast){fs::create_directories(out);std::ifstream f(rom,std::ios::binary);std::ofstream h(out/"private-rom.hex");require(f.good()&&h.good(),"Cannot stage private ROM");char c[4];while(f.read(c,4)){uint32_t w=(uint32_t(uint8_t(c[0]))<<24)|(uint32_t(uint8_t(c[1]))<<16)|(uint32_t(uint8_t(c[2]))<<8)|uint8_t(c[3]);h<<std::hex<<std::setfill('0')<<std::setw(8)<<w<<"\n";}}
  Sim s(out,fast);s.loadRom(rom,fast);if(s.stop)return 0;for(auto&[id,p]:disks)s.mount(id,p);s.run(seconds,interactive);if(interactive&&s.stop)return 0;require(s.cpuCount>100&&s.vectors.size()==4,"CPU did not complete reset vectors / boot prefix");require(s.hdmi.frames>0,"No decoded HDMI frames");require(s.matched>1000&&s.badSymbols==0,"HDMI serialization mismatch");std::cout<<"PASS: ROM and CPU vectors verified, "<<s.cpuCount<<" CPU cycles, "<<s.hdmi.frames<<" decoded HDMI frames"<<std::endl;return 0;}
 catch(const std::exception&e){fs::create_directories(out);atomicText(out/"error.txt",e.what());std::cerr<<"FAIL: "<<e.what()<<std::endl;return 1;}
}
