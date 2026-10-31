#include "Vshared_memory_top.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"FAIL line %d cycle %u: %s\n",__LINE__,cycles,#c);exit(1);}}while(0)

// Keep chipset DMA active while CPU transactions wait for vacant slots.
// Each port owns a disjoint part of every bank, so the reference memory does
// not depend on the controller's internal arbitration or state variables.
int main() {
 for(unsigned phase : {0u,180u}) {
  Vshared_memory_top q;
  unsigned cycles=0,seed=0x7a62d941,chip_reads=0,chip_writes=0,refresh_requests=0;
  bool active=false;
  std::vector<unsigned> mem(2097152);
  for(unsigned n=0;n<mem.size();n++) mem[n]=0xccaa55ccu^(n*0x9e3779b9u);
  auto random=[&](){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;};
  auto word=[&](unsigned a){return (mem[a>>1]>>((a&1)?0:16))&65535;};
  auto tail=[&](unsigned a){
   return (uint64_t(word((a&~7u)|((a+1)&7)))<<32)|
          (uint64_t(word((a&~7u)|((a+2)&7)))<<16)|word((a&~7u)|((a+3)&7));
  };
  auto store=[&](unsigned a,unsigned d,unsigned ds){
   unsigned shift=(a&1)?0:16,m=(((ds&1)?0:255)|((ds&2)?0:65280))<<shift;
   mem[a>>1]=(mem[a>>1]&~m)|((d<<shift)&m);
  };
  struct Pending { unsigned due,a,data; uint64_t rest; bool write; };
  std::deque<Pending> pending;
  auto tick=[&](){
   // Sample completed chipset bursts at the next fixed chipset deadline.
   if(!pending.empty() && pending.front().due==cycles) {
    auto p=pending.front();pending.pop_front();
    if(p.write) {
     q.probe_addr=p.a>>1;q.eval();CHECK(q.probe_data==p.data);
     chip_writes++;
    } else {
     if(q.p1_dout!=p.data || q.p1_dout48!=p.rest) {
      fprintf(stderr,"chipset %06X got %04X/%012llX expected %04X/%012llX\n",
       p.a,q.p1_dout,(unsigned long long)q.p1_dout48,p.data,(unsigned long long)p.rest);
      CHECK(false);
     }
     chip_reads++;
    }
   }
   if(active && cycles%12==0) {
    unsigned slot=cycles/12,r=random();
    q.p1_cs=0;q.refresh=0;
    if(slot%53==0) {q.p1_cs=1;q.refresh=1;refresh_requests++;}
    else if((slot&7)!=2 && (slot&7)!=4 && (slot&7)!=7) {
     q.p1_cs=1;
     // Both DQ halves, all banks, row boundaries, 16/64-bit reads and writes.
     unsigned a=(r&0x300000)|((slot*127)&0xffff),d=random()&65535,ds=(slot/16)&3;
     q.p1_addr=a;q.p1_din=d;q.p1_ds=ds;q.p1_we=(slot&1);
     if(q.p1_we) store(a,d,ds);
     pending.push_back({cycles+14,a,q.p1_we?mem[a>>1]:word(a),tail(a),bool(q.p1_we)});
    }
   }
   q.clk=0;q.sd_clk=phase==180;q.sync=(cycles%12)<3;q.eval();
   q.clk=1;q.sd_clk=phase==0;q.eval();cycles++;
  };
  q.reset_n=0;q.p1_cs=0;q.p2_cs=0;q.refresh=0;tick();q.reset_n=1;
  for(unsigned n=0;n<12000&&!q.ready;n++)tick();CHECK(q.ready);
  active=true;
  auto transfer=[&](unsigned a,bool write,unsigned data,unsigned mask){
   unsigned ack=q.p2_ack;q.p2_addr=a;q.p2_we=write;q.p2_din=data;q.p2_ds=mask;q.p2_cs=1;
   unsigned wait=0;do {tick();CHECK(++wait<200);}while(q.p2_ack==ack);
   q.p2_cs=0;
  };
  unsigned writes=0,reads=0;
  for(unsigned n=0;n<16384;n++) {
   unsigned a=(random()&0x3fffff)|0x10000;
   unsigned d=n<256?(1u<<(n%16)):random()&65535,ds=(n/16)&3;
   transfer(a,true,d,ds);store(a,d,ds);writes++;
   for(unsigned i=0;i<2+(n%9);i++)tick();
   q.probe_addr=a>>1;q.eval();CHECK(q.probe_data==mem[a>>1]);
   transfer(a,false,0,0);reads++;
   CHECK(q.p2_dout==word(a));CHECK(q.p2_dout48==tail(a));
   for(unsigned i=0;i<n%5;i++)tick();
  }
  active=false;q.p1_cs=0;q.refresh=0;
  for(unsigned i=0;i<30;i++)tick();
  CHECK(pending.empty());CHECK(chip_reads>10000);CHECK(chip_writes>10000);CHECK(refresh_requests>500);
  printf("PASS phase %u: CPU %u writes/%u burst reads, concurrent chipset %u writes/%u burst reads, %u refresh requests; no mismatches\n",
   phase,writes,reads,chip_writes,chip_reads,refresh_requests);
 }
}
