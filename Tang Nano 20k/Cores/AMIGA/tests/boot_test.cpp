#include "Vboot_top.h"
#include "verilated.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>
int main(int argc,char **argv) {
    const double duration=argc>1?atof(argv[1]):.12;
    const int clock_order=argc>2?atoi(argv[2]):0;
    if(duration<=0 || duration>30)return 2;
    std::vector<uint8_t> adf;
    if(argc>3){std::ifstream disk(argv[3],std::ios::binary);adf.assign(std::istreambuf_iterator<char>(disk),{});if(adf.size()!=901120)return 3;}
    unsigned sectors_served=0,transfer_word=0;uint32_t sector=0,token=0;bool transferring=false;
    std::ofstream disklog("disk.csv");disklog<<"seconds,sector,token\n";
    VerilatedContext ctx;Vboot_top q{&ctx};
    // DIV3 uses CLKOUTP at 180 degrees; its rise aligns with CLKOUT rise.
    q.clk_28m=0;q.clk_85m=1;q.por=1;q.rom_done=0;q.trace_index=0;q.eval();
    const uint64_t cycles=uint64_t(duration*28500000);
    uint64_t color=0;unsigned frames=0,x=0,y=0,width=0,height=0;
    bool hs=q.hs_n,vs=q.vs_n,clock85=1,trace_printed=false;
    std::vector<uint8_t> frame(2048*1024*3),last(frame.size());
    unsigned last_width=0,last_height=0;
    auto start=std::chrono::steady_clock::now();
    uint64_t publish_time=0;
    std::ofstream pipeline("pipeline.csv");pipeline<<"half85_tick,publish_tick,margin_half85,word_address,returned,latched_before\n";
    for(uint64_t n=0;n<cycles;n++) {
        q.host_wr=0;
        if(n==3000){q.host_wr=1;q.host_addr=0x60;q.host_data=unsigned(adf.size());}
        if(q.disk_status&4){printf("FAIL disk mailbox error %08X\n",q.disk_status);return 4;}
        // Poll at the real app's 4 ms cadence; bound host transfer rate too.
        if(!adf.empty() && !transferring && n%114000==0 && (q.disk_status&1)) {
            if((q.disk_status&0xff08)!=0x100 || q.disk_sector>=1760)return 5;
            sector=q.disk_sector;token=q.disk_token;transfer_word=0;transferring=true;
            disklog<<double(n)/28500000<<","<<sector<<","<<token<<"\n";disklog.flush();
        }
        if(transferring && n%228==0){
            q.host_wr=1;
            if(transfer_word<128){
                auto p=&adf[sector*512+transfer_word*4];q.host_addr=0x1400+transfer_word;
                q.host_data=uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
                transfer_word++;
            }else{q.host_addr=0x64;q.host_data=token;transferring=false;sectors_served++;}
        }
        if(n==2850)q.por=0;            // 100 us
        if(n==142500)q.rom_done=1;     // 5 ms, preloaded ROM ready
        for(int phase=0;phase<2;phase++) {
            auto fast_edge=[&](){
                bool publish=clock85 && q.timing_publish;
                clock85=!clock85;q.clk_85m=clock85;ctx.timeInc(1);q.eval();
                if(publish)publish_time=ctx.time();
            };
            // Exercise both evaluation orderings at the aligned rising edges.
            if(clock_order==1) fast_edge();
            bool capture=phase==0 && (q.cpu_debug[0]&1) && q.cpu_debug[1]<40 && q.timing_latch;
            auto old_address=q.timing_address;auto old_data=q.timing_data;auto old_latched=q.timing_latched;
            q.clk_28m=phase==0;
            if(clock_order==2){
                bool publish=clock85 && q.timing_publish;
                clock85=!clock85;q.clk_85m=clock85;ctx.timeInc(1);
                if(publish)publish_time=ctx.time();
            }
            q.eval();
            if(capture)pipeline<<ctx.time()<<","<<publish_time<<","<<(ctx.time()-publish_time)<<","<<old_address<<","<<old_data<<","<<old_latched<<"\n";
            for(int k=(clock_order==0?0:1);k<3;k++)fast_edge();
        }
        if(hs&&!q.hs_n){width=std::max(width,x);x=0;y++;}
        if(vs&&!q.vs_n){
            frames++;
            if(y>100 && width>100 && width<=2048 && y<=1024){last=frame;last_width=width;last_height=y;}
            x=0;y=0;width=0;std::fill(frame.begin(),frame.end(),0);
        }
        hs=q.hs_n;vs=q.vs_n;
        if(q.red||q.green||q.blue)color++;
        if(x<2048 && y<1024){auto p=3*(y*2048+x);frame[p]=q.red;frame[p+1]=q.green;frame[p+2]=q.blue;}
        x++;
        if(!trace_printed && q.cpu_debug[1]>=8){
            printf("Initial CPU memory-state steps at %.6f s:\n",double(n)/28500000);
            for(unsigned i=8;i<24;i+=2)printf("  %08X -> %04X (write %04X)\n",q.cpu_debug[i],q.cpu_debug[i+1]>>16,q.cpu_debug[i+1]&65535);
            fflush(stdout);trace_printed=true;
        }
        if(n && n%2850000==0){
            double wall=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
            printf("sim %.2f s, wall %.1f s, CPU status %08X steps %u addr %08X RGB clocks %llu frames %u\n",
                double(n)/28500000,wall,q.cpu_debug[0],q.cpu_debug[1],q.cpu_debug[2],(unsigned long long)color,frames);
            fflush(stdout);
        }
    }
    bool vectors=q.cpu_debug[8]==0 && q.cpu_debug[10]==2 && q.cpu_debug[12]==4 && q.cpu_debug[14]==6 &&
        (q.cpu_debug[9]>>16)==0x1114 && (q.cpu_debug[11]>>16)==0x4ef9 &&
        (q.cpu_debug[13]>>16)==0xf8 && (q.cpu_debug[15]>>16)==0xd2;
    std::ofstream out("boot.json");
    out<<"{\n\"sectors_served\":"<<sectors_served<<",\n\"simulated_seconds\":"<<duration<<",\n\"reset_released\":"<<(!q.reset_out?"true":"false")
       <<",\n\"cpu_reset_released\":"<<((q.cpu_debug[0]&1)?"true":"false")
       <<",\n\"sdram_ready\":"<<(q.sdram_ready?"true":"false")
       <<",\n\"reset_vectors_match\":"<<(vectors?"true":"false")
       <<",\n\"cpu_steps\":"<<q.cpu_debug[1]<<",\n\"nonblack_cycles\":"<<color
       <<",\n\"frames\":"<<frames<<",\n\"words\":[";
    for(unsigned i=0;i<24;i++)out<<(i?",":"")<<q.cpu_debug[i];out<<"]\n}\n";
    if(last_width && last_height){
        std::ofstream image("frame.ppm",std::ios::binary);image<<"P6\n"<<last_width<<" "<<last_height<<"\n255\n";
        for(unsigned row=0;row<last_height;row++)image.write((char*)last.data()+row*2048*3,last_width*3);
    }
    printf("Finished: vectors %s, reset %u, CPU steps %u, RGB clocks %llu\n",vectors?"match":"differ",q.reset_out,q.cpu_debug[1],(unsigned long long)color);
    std::ofstream trace("trace.json");trace<<"[\n";
    for(unsigned i=0;i<512;i++){
        q.trace_index=i;q.clk_28m=0;q.eval();q.clk_28m=1;q.eval();
        trace<<(i?",\n":"")<<"["<<q.cpu_trace[0]<<","<<q.cpu_trace[1]<<","<<q.cpu_trace[2]<<","<<q.cpu_trace[3]<<"]";
    }
    trace<<"\n]\n";
    q.final();return 0;
}
