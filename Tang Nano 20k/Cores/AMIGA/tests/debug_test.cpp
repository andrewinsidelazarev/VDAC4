#include "Vamiga_debug.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do {if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main() {
    Vamiga_debug q;
    auto tick=[&](){q.clk=0;q.eval();q.clk=1;q.eval();};
    q.reset=1; q.cpu_reset_n=0; q.cpu_clkena=0; q.cpu_state=1;
    q.cpu_address=0; q.cpu_read=0; q.cpu_write=0; q.trace_index=0; q.trace_start=0;
    q.status=0x12345678; q.chip_address=0x1234; q.chip_read=0xabcd; q.chip_write=0x5678;
    tick(); q.reset=0;
    // No completed cycles while held in CPU reset, stalled, or internal CPU state.
    q.cpu_clkena=1; q.cpu_state=0; tick(); CHECK(q.words[1]==0);
    q.cpu_reset_n=1; q.cpu_clkena=0; tick(); CHECK(q.words[1]==0);
    q.cpu_clkena=1; q.cpu_state=1; tick(); CHECK(q.words[1]==0);
    const unsigned data[8]={0x1114,0x4ef9,0x00f8,0x00d2,0x4ff9,0x0004,0x0000,0x41f9};
    for(unsigned i=0;i<20;i++) {
        q.cpu_state=2; q.cpu_address=i*2; q.cpu_read=i<8?data[i]:i;
        q.cpu_write=0xbeef; q.cpu_clkena=0; tick();
        q.cpu_clkena=1; tick();
    }
    CHECK(q.words[0]==0x12345678); CHECK(q.words[1]==20);
    CHECK(q.words[4]==38); CHECK(q.words[5]==0x0013beef);
    CHECK(q.words[6]==0x2468); CHECK(q.words[7]==0xabcd5678);
    for(unsigned i=0;i<8;i++) {CHECK(q.words[8+i*2]==i*2);CHECK(q.words[9+i*2]==(data[i]<<16|0xbeef));}
    // A subsequent CPU reset must not discard evidence of the initial boot.
    q.cpu_reset_n=0; tick(); CHECK(q.words[1]==20); CHECK(q.words[9]==0x1114beef);
    q.reset=1; tick(); CHECK(q.words[1]==0);
    for(unsigned i=8;i<24;i++) CHECK(q.words[i]==0);
    CHECK((q.trace_out[3]&1023)==0);
    q.trace_start=9; tick(); q.reset=0;q.cpu_reset_n=1;q.cpu_clkena=1;q.cpu_state=2;
    for(unsigned i=0;i<600;i++) {
        q.cpu_address=0xf80000+i*2;q.cpu_read=i;q.cpu_write=~i;q.status=0x12340000+i;tick();
    }
    q.cpu_clkena=0;
    for(unsigned i=0;i<512;i++) {
        q.trace_index=i;tick();
        CHECK((q.trace_out[3]&1023)==512);CHECK(q.trace_out[3]&(1<<19));
        CHECK(((q.trace_out[3]>>10)&511)==i);
        CHECK(q.trace_out[0]==0xf80000+(i+9)*2);
        CHECK(q.trace_out[1]==((i+9)<<16|((~(i+9))&65535)));
        CHECK(q.trace_out[2]==0x12340000+i+9);
    }
    q.cpu_reset_n=0;tick();CHECK((q.trace_out[3]&1023)==512);
    q.reset=1;tick();CHECK((q.trace_out[3]&1023)==0);CHECK(q.trace_out[0]==0);
    puts("PASS passive CPU capture: reset/stall gating, first boot reads retained, latest cycle, external reset; 512-entry block RAM trace, delayed capture, frozen contents, synchronous indexed read and valid metadata");
}
