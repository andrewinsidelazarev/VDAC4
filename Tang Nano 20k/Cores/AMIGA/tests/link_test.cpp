#include "Vlink_top.h"
#include "verilated.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <fstream>
#include <iterator>
#define CHECK(x) do {if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
struct Sim {
    VerilatedContext ctx; Vlink_top q{&ctx};
    uint64_t t=0,edge=200; unsigned period, writes=0, hid_events=0, clears=0;
    unsigned last_hid=0; bool last_we=false;
    std::vector<uint8_t> memory=std::vector<uint8_t>(524288,0xcc);
#ifndef ROM_MEMORY_TEST
    std::vector<uint8_t> disk_bytes;
    unsigned disk_done_count=0;
#endif
    Sim(unsigned p,unsigned phase):period(p) {
        q.clk=0;q.sck=0;q.cs_n=1;q.io_in=0;q.rst_n=0;
        q.eval();advance(2000);q.rst_n=1;advance(2000+phase);
    }
    void advance(uint64_t d) {
        uint64_t end=t+d;
        while(edge<=end) {
            t=edge;edge+=200;ctx.time(t);
            if(!q.clk) {
                if(q.rom_we && !last_we) {
                    CHECK(q.rom_addr*2+1<memory.size());
                    memory[q.rom_addr*2]=q.rom_word>>8;
                    memory[q.rom_addr*2+1]=q.rom_word;
                    writes++;
                }
                last_we=q.rom_we;
                if(q.hid_strobe){hid_events++;last_hid=(q.hid_start<<8)|q.hid_data;}
                if(q.hid_reset)clears++;
#ifndef ROM_MEMORY_TEST
                if(q.disk_strobe){CHECK(q.disk_addr==disk_bytes.size());disk_bytes.push_back(q.disk_data);}
                if(q.disk_done)disk_done_count++;
#endif
            }
            q.clk=!q.clk;q.eval();
        }
        t=end;ctx.time(t);q.eval();
    }
    unsigned cycle(unsigned v) {
        q.io_in=v;q.eval();advance(period/2);q.sck=1;q.eval();unsigned got=q.io_out;
        advance(period-period/2);q.sck=0;q.eval();return got;
    }
    void begin(unsigned cmd,uint32_t a) {
        q.cs_n=0;q.eval();
        for(int i=7;i>=0;i--)cycle((cmd>>i)&1);
        for(int i=7;i>=0;i--)cycle((a>>(i*4))&15);
    }
    void end(){q.cs_n=1;q.eval();CHECK(!q.io_oe);advance(1);}
    void write(uint32_t a,const std::vector<uint32_t>& words) {
        begin(1,a);for(auto v:words)for(int i=7;i>=0;i--)cycle((v>>(i*4))&15);end();
    }
    void wr(uint32_t a,uint32_t v){write(a,{v});advance(2400);}
    uint32_t rd(uint32_t a){begin(2,a);for(int i=0;i<8;i++)cycle(0);uint32_t v=0;for(int i=0;i<8;i++)v=(v<<4)|cycle(0);end();return v;}
    void packet(unsigned seq,const uint8_t *p,bool duplicate=false) {
        std::vector<uint32_t> w(64,0);for(unsigned i=0;i<256;i++)w[i/4]|=uint32_t(p[i])<<(8*(i%4));
        write(0x4000,w);wr(0x48,(seq<<16)|256);if(duplicate)wr(0x48,(seq<<16)|256);
        advance(400ull*16*257);CHECK(!(rd(0x44)&2));CHECK(rd(0x48)==seq);CHECK(rd(0x54)==0);
    }
};
uint32_t crc(const std::vector<uint8_t>&v){uint32_t c=~0u;for(auto b:v){c^=b;for(int i=0;i<8;i++)c=(c>>1)^((c&1)?0xedb88320u:0);}return ~c;}
int main(int argc,char **argv) {
    CHECK(argc==2);std::ifstream f(argv[1],std::ios::binary);CHECK(f.good());
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(f)),{});CHECK(rom.size()==524288);
    unsigned cases=0;
    for(unsigned p:{285u,570u,1140u,5700u})for(unsigned phase=0;phase<400;phase+=40) {
        Sim s(p,phase);CHECK(s.rd(0)==0x56444334);CHECK(s.rd(4)==0x05010001);
        for(unsigned i=0;i<36;i++)CHECK(s.rd(0x60+4*i)==0xd1a60000+i);
        s.wr(0x60,0xffffffff);CHECK(s.rd(0x60)==0xd1a60000); // diagnostics are read-only
        CHECK(s.rd(0xf4)==0);CHECK(s.rd(0xf8)==0);
        CHECK(s.rd(0x1a4)==0x56505231);CHECK(s.rd(0x1a0)==0);CHECK(s.q.video_probe==0);
        s.wr(0x1a0,0xa5123456);CHECK(s.rd(0x1a0)==0xa5123456);CHECK(s.q.video_probe==0xa5123456);
        s.q.disk_reset=1;s.advance(2000);CHECK(s.q.video_probe==0);
        s.wr(0x1a0,0xa5ffffff);CHECK(s.rd(0x1a0)==0);
        s.q.disk_reset=0;s.wr(0x1a0,0xa5abcdef);CHECK(s.q.video_probe==0xa5abcdef);
        s.wr(0x1a0,0);CHECK(s.q.video_probe==0);
        s.wr(0xf4,0x3ff);CHECK(s.rd(0xf4)==0x1ff);
        s.wr(0xf8,123456);CHECK(s.rd(0xf8)==123456);
        CHECK(!s.q.rom_ready);s.wr(0x3c,0xfedcba98);CHECK(s.rd(0x3c)==0xfedcba98);
        s.wr(0x10,0x101);CHECK(s.hid_events==1&&s.last_hid==0x101);
        s.wr(0x10,4);CHECK(s.hid_events==2&&s.last_hid==4);CHECK(s.rd(0xc)==8);
        s.wr(0x1c,0);CHECK(s.clears==1);
        s.wr(0x44,1);s.packet(1,rom.data(),true);CHECK(s.writes==128);
        for(unsigned i=0;i<256;i++)CHECK(s.memory[i]==rom[i]);
        CHECK(s.rd(0x4c)==256);CHECK(s.rd(0x50)==crc(std::vector<uint8_t>(rom.begin(),rom.begin()+256)));
        s.wr(0x44,2);CHECK(!s.q.rom_ready);CHECK(s.rd(0x54)==1); // premature release
        s.wr(0x44,1);s.wr(0x48,0x10100);CHECK(s.rd(0x54)==1); // missing payload
        s.wr(0x44,1);s.wr(0x4000,0);s.wr(0x48,0x10100);CHECK(s.rd(0x54)==1); // partial payload
        s.wr(0x44,1);s.wr(0x48,0x20100);CHECK(s.rd(0x54)==1); // sequence gap
        CHECK((s.rd(0x108)>>16)==0x5d01);CHECK((s.rd(0x100)>>16)==2850);
        CHECK(s.q.sd_cs_n);s.wr(0x100,0x103);CHECK(!s.q.sd_cs_n);
        s.write(0x2000,{0x12345678,0x9abcdef0});s.advance(2400);s.wr(0x104,0);s.wr(0x108,8);
        unsigned limit=0;while((s.rd(0x108)&1)&&limit++<100)s.advance(400*200);
        CHECK(limit<100);CHECK(s.rd(0x2000)==0x12345678);CHECK(s.rd(0x2004)==0x9abcdef0);
        // Actual QSPI path to the DF0 mailbox: all byte values and final sector.
        CHECK(s.rd(0x198)==0x41444631);s.wr(0x180,901120);CHECK(s.q.disk_size==901120);
        s.q.disk_sector=1759;s.q.disk_rd=1;s.advance(1600);CHECK(s.q.disk_busy);
        s.q.disk_rd=0;s.advance(1600);CHECK(s.rd(0x184)==0x103);CHECK(s.rd(0x188)==1759);
        unsigned token=s.rd(0x18c);CHECK(token==1);
        std::vector<uint32_t> payload(128);
        for(unsigned i=0;i<512;i++)payload[i/4]|=((i*37+11)&255)<<(8*(i%4));
        s.write(0x5000,payload);s.wr(0x190,token);s.advance(400ull*2100);
        CHECK(s.disk_bytes.size()==512);CHECK(s.disk_done_count==1);CHECK(!s.q.disk_busy);
        for(unsigned i=0;i<512;i++)CHECK(s.disk_bytes[i]==((i*37+11)&255));
        CHECK(s.rd(0x190)==token);s.wr(0x190,token);CHECK(s.disk_done_count==1);
        s.disk_bytes.clear();s.q.disk_rd=1;s.advance(1600);s.q.disk_rd=0;
        s.wr(0x5000,1);s.wr(0x190,token+1);CHECK(s.rd(0x184)&4);CHECK(s.disk_bytes.empty());
        // Reset cancels a pending operation, but keeps the disk inserted.
        s.q.disk_reset=1;s.advance(1600);s.q.disk_reset=0;s.advance(1600);
        CHECK(!s.q.disk_busy);CHECK(s.q.disk_size==901120);
        s.q.disk_rd=1;s.advance(1600);s.q.disk_rd=0;
        CHECK(s.rd(0x18c)==token+2);s.write(0x5000,payload);
        s.wr(0x190,token+1);CHECK(s.rd(0x184)&4);CHECK(s.disk_bytes.empty());
        s.q.disk_reset=1;s.advance(1600);s.q.disk_reset=0;s.advance(1600);
        s.wr(0x180,0);CHECK(s.q.disk_size==0);
        s.wr(0xf0,0x424f4f54);CHECK(s.q.boot_req);cases++;
    }
    Sim s(285,173);s.wr(0x44,1);s.wr(0x58,crc(rom));
    for(unsigned off=0;off<rom.size();off+=256)s.packet(off/256+1,rom.data()+off,off==0);
    CHECK(s.memory==rom);CHECK(s.writes==262144);CHECK(s.rd(0x4c)==rom.size());CHECK(s.rd(0x50)==crc(rom));
    // Wrong expected CRC must hold CPU; a new transfer is required after error.
    s.wr(0x58,crc(rom)^1);s.wr(0x44,2);CHECK(!s.q.rom_ready);CHECK(s.rd(0x54)==1);
    s.wr(0x58,crc(rom));s.wr(0x44,2);CHECK(!s.q.rom_ready);
    s.wr(0x44,1);s.wr(0x58,crc(rom));
    for(unsigned off=0;off<rom.size();off+=256)s.packet(off/256+1,rom.data()+off);
    s.wr(0x44,2);CHECK(s.q.rom_ready);CHECK(s.rd(0x44)==4);
    s.advance(400ull*1600);uint32_t wp=s.rd(0x20)&1023;
    for(unsigned shift=1;shift<10;shift*=2)wp^=wp>>shift;
    CHECK(s.rd(0x6000+4*((wp-2)&1023))==0xe0011234);
    s.wr(0x44,3);CHECK(s.q.soft_reset);s.advance(400*300);CHECK(!s.q.soft_reset);CHECK(s.q.rom_ready);
    printf("PASS %u QSPI phase/rate cases, 2 full 512 KiB ROM transfers, byte order, CRC, packet rejection, HID, stereo, reset, SD bridge loopback, DF0 full-sector/partial/stale/cancel/eject, BOOT\n",cases);
}
