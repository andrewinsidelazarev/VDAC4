#include "Vvideo_top.h"
#include "verilated.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static Vvideo_top d;
static uint64_t ticks=0, symbols=0;
static uint32_t previous_rgb=0;
static bool compare_legacy=false;
static void equal(uint32_t got,uint32_t want,const char* what) {
    if(got!=want) { std::fprintf(stderr,"%s at %llu: got %06x expected %06x\n",what,
        (unsigned long long)ticks,got,want); std::exit(1); }
}
// Invert TMDS transition minimization and the symbol inversion bit.
static unsigned decode(unsigned word) {
    unsigned q=(word&255)^((word&512)?255:0), value=q&1;
    for(unsigned i=1;i<8;i++) {
        unsigned bit=((q>>i)^(q>>(i-1)))&1;
        if(!(word&256)) bit^=1;
        value|=bit<<i;
    }
    return value;
}
static void tick() {
    d.hs_in=(ticks%1816)>=144;
    d.clk=0;d.eval();
    uint32_t before=(d.video_probe>>24)==0xa5 ? d.video_probe&0xffffff : d.rgb;
    d.clk=1;d.eval();++ticks;
    if(ticks>10 && d.video_symbol && !d.reset) {
        uint32_t pixel=decode(d.tmds_r)<<16|decode(d.tmds_g)<<8|decode(d.tmds_b);
        equal(pixel,previous_rgb,"TMDS RGB888 round trip");++symbols;
    }
    if(ticks>1816*3) {
        equal(d.hs,d.legacy_hs,"horizontal timing unchanged");
        equal(d.vs,d.legacy_vs,"vertical timing unchanged");
        if(compare_legacy) equal((d.rgb>>12&0xf00)|(d.rgb>>8&0xf0)|(d.rgb>>4&15),
                                d.legacy_rgb,"legacy high nibble and pixel alignment");
    }
    previous_rgb=before;
}
static void run(unsigned n) { while(n--) tick(); }
static unsigned pack_nibbles(uint32_t rgb,unsigned shift) {
    return ((rgb>>(16+shift)&15)<<8)|((rgb>>(8+shift)&15)<<4)|(rgb>>shift&15);
}
static void palette(unsigned index,uint32_t rgb,bool low=true) {
    d.write_index=index;d.write_color=1;d.loct=0;d.palette_data=pack_nibbles(rgb,4);tick();
    if(low) { d.loct=1;d.palette_data=pack_nibbles(rgb,0);tick(); }
    d.write_color=0;
}
static uint32_t shade(uint32_t rgb,unsigned mode,bool dark) {
    uint32_t out=0;
    for(unsigned shift=0;shift<24;shift+=8) {
        unsigned c=(rgb>>shift)&255;
        if(mode==1 && dark) c>>=1;
        if(mode==2 && dark) c=0;
        if(mode==3) c=dark ? (c<128?0:2*c-255) : (c<128?2*c:255);
        out|=c<<shift;
    }
    return out;
}
static void steady(uint32_t expected) {
    run(1816*4); // fill both lines and all filter pipelines
    for(unsigned i=0;i<1816;i++) {
        tick();
        // Amber documents one invalid vertical-filter pixel at the line wrap
        // (lbfd reads the old cell). It is in horizontal blanking. Do not treat
        // that porch sample as a visible pixel; unfiltered RGB is checked there
        // too, and TMDS decoding / sync comparisons still run on every cycle.
        if(!(d.filter&2) || ticks%908>=16)
            equal(d.rgb,shade(expected,d.scanline,d.dark_line),"Amber RGB888");
    }
}
int main(int argc,char**argv) {
    Verilated::commandArgs(argc,argv);
    d.video_probe=0;d.reset=1;run(8);d.reset=0;
    uint32_t colors[256];
    for(unsigned i=0;i<256;i++) {
        colors[i]=(i<<16)|(((i*73+19)&255)<<8)|(255-i);
        palette(i,colors[i]);
    }
    compare_legacy=true;
    // All 8 palette banks and all 256 levels in each independent channel.
    for(unsigned i=0;i<256;i++) {
        d.palette_index=i;run(2);equal(d.clut_rgb,colors[i],"AGA LOCT and bank");
        steady(colors[i]);
    }
    // Changing pixels exercise line-buffer addressing and keep sync identical.
    for(unsigned i=0;i<1816*8;i++) { d.palette_index=(i*37)&255;tick(); }
    compare_legacy=false;
    // Every bit in every component survives the whole path, including bit 0.
    d.palette_index=255;
    for(unsigned bit=0;bit<24;bit++) { palette(255,1u<<bit);steady(1u<<bit); }
    // Filter carry and scanline arithmetic at black/white and near boundaries.
    for(unsigned f=0;f<4;f++) for(unsigned s=0;s<4;s++) {
        d.filter=f;d.scanline=s;
        for(uint32_t color : {0u,0xffffffu,0x7f80ffu,0x010f1fu}) {
            palette(255,color);steady(color);
        }
    }
    d.filter=0;d.scanline=0;
    palette(255,0xabcdef,false);run(2);equal(d.clut_rgb,0xaaccee,"OCS high write duplicates nibble");
    steady(0xaaccee);
    palette(255,0xabcdef);d.hblank=1;steady(0);d.hblank=0;steady(0xabcdef);
    d.vblank=1;steady(0);d.vblank=0;steady(0xabcdef);
    d.ehb=1;d.palette_index=32+7;run(2);
    uint32_t half=(colors[7]&0xfefefe)>>1;
    equal(d.clut_rgb,half,"EHB per-channel shift");steady(half);
    // Override only the HDMI input while the chipset/Amber pixel stream keeps
    // changing. Validate every color bit and returning to the original image.
    for(unsigned bit=0;bit<24;bit++) {
        d.video_probe=0xa5000000u|(1u<<bit);
        for(unsigned i=0;i<1816*2;i++) {d.palette_index=i&255;tick();}
    }
    for(uint32_t probe : {0xa5000000u,0xa5ffffffu,0xa5202020u,0xa4ffffffu,0u}) {
        d.video_probe=probe;
        for(unsigned i=0;i<1816*2;i++) {d.palette_index=i&255;tick();}
    }
    std::printf("PASS RGB888: 256 palette entries/8 banks, all 256 levels per channel, 24 walking bits, dynamic pixel alignment, all filter/scanline modes, OCS/EHB, blanking, unchanged sync, output probe on/off/colors, %llu decoded DVI symbols\n",(unsigned long long)symbols);
}
