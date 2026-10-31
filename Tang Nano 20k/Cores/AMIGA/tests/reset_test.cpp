#include "Vreset_top.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do {if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main() {
    Vreset_top q;
    auto tick=[&](unsigned n){while(n--){q.clk_28m=0;q.eval();q.clk_28m=1;q.eval();}};
    q.rst_28m=1;q.btn=0;q.soft_reset=0;q.kbd_reset=0;q.rom_done=0;
    tick(10);CHECK(q.hold_count==255 && q.reset_out);
    q.rst_28m=0;tick(300);CHECK(q.hold_count==0 && q.reset_out);
    q.rom_done=1;tick(1);CHECK(!q.reset_out); // reported hardware idle state
    q.btn=1;tick(300);CHECK(!q.reset_out);   // S1 is not the reset key
    q.btn=2;tick(4);CHECK(q.hold_count==255 && q.reset_out);
    tick(300);CHECK(q.hold_count==255 && q.reset_out); // S2 held high
    q.btn=0;tick(2);CHECK(q.reset_out);
    tick(254);CHECK(q.reset_out);tick(1);CHECK(!q.reset_out);
    q.soft_reset=1;tick(1);CHECK(q.hold_count==255 && q.reset_out);
    q.soft_reset=0;tick(254);CHECK(q.reset_out);tick(1);CHECK(!q.reset_out);
    q.kbd_reset=1;tick(1);CHECK(q.hold_count==255 && q.reset_out);
    q.kbd_reset=0;tick(255);CHECK(!q.reset_out);
    q.rom_done=0;tick(1);CHECK(q.reset_out); // reload must hold the core
    q.rom_done=1;q.rst_28m=1;tick(1);CHECK(q.reset_out && q.hold_count==255);
    q.rst_28m=0;tick(255);CHECK(!q.reset_out);
    puts("PASS actual generated reset logic: idle-low boot, S2 active-high reset, S1 ignored, synchronized release, ROM gate, software/keyboard/power reset");
}
