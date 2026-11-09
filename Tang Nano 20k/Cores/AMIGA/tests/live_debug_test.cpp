
#include "Vamiga_debug.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do {if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(){
 Vamiga_debug q;unsigned r=0x12345678;
 auto random=[&](){r^=r<<13;r^=r>>17;r^=r<<5;return r;};
 for(unsigned n=0;n<100000;n++){
  q.cpu_address=random();q.status=random();q.cpu_state=n&3;q.cpu_clkena=n&1;
  q.reset=(n>>1)&1;q.cpu_reset_n=q.status&1;q.clk=n&1;
  q.cpu_read=random();q.cpu_write=random();q.chip_address=random()&0x7fffff;
  q.chip_read=random();q.chip_write=random();q.trace_index=n&511;q.trace_start=random();q.eval();
  unsigned expected=(q.cpu_clkena<<12)|(((q.status>>18)&7)<<9)|(q.cpu_state<<7)|(q.status&127);
  CHECK(q.words[0]==expected);CHECK(q.words[2]==q.cpu_address);
  for(unsigned j=1;j<24;j++)if(j!=2)CHECK(q.words[j]==0);
  for(unsigned j=0;j<4;j++)CHECK(q.trace_out[j]==0);
 }
 puts("PASS 100000 live CPU status/address combinations through reset and stalls; trace outputs remain disabled");
}
