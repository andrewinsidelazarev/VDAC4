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
#include <map>
#include <string>
// Образ диска: файл целиком или разреженный дамп тома EVO ([LBA LE32][512 байтов] подряд, tests/run_evo_boot.py),
// размер дампа — по геометрии RDB в блоке 0; блока нет — нули.
struct Disk {
    std::vector<uint8_t> flat;std::map<uint32_t,std::vector<uint8_t>> blocks;uint64_t bytes=0;bool sparse=false;
    bool load(const char *path){
        std::ifstream f(path,std::ios::binary);std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)),{});
        std::string name(path);sparse=name.size()>5&&name.substr(name.size()-5)==".dump";
        if(!sparse){flat.swap(raw);bytes=flat.size();return bytes&&bytes%512==0;}
        if(raw.empty()||raw.size()%516)return false;
        for(size_t at=0;at<raw.size();at+=516){
            uint32_t lba=raw[at]|raw[at+1]<<8|raw[at+2]<<16|uint32_t(raw[at+3])<<24;
            blocks[lba].assign(raw.begin()+at+4,raw.begin()+at+516);
        }
        auto r=blocks.find(0);if(r==blocks.end())return false;
        const uint8_t *b=r->second.data();
        auto be=[&](unsigned i){return uint32_t(b[i*4])<<24|uint32_t(b[i*4+1])<<16|uint32_t(b[i*4+2])<<8|b[i*4+3];};
        bytes=uint64_t(be(16))*be(17)*be(18)*512;return bytes!=0;
    }
    bool empty()const{return bytes==0;}
    uint64_t size()const{return bytes;}
    uint8_t *at(uint32_t lba){
        if(!sparse)return flat.data()+size_t(lba)*512;
        auto &v=blocks[lba];if(v.empty())v.assign(512,0);return v.data();
    }
};
int main(int argc,char **argv) {
    const double duration=argc>1?atof(argv[1]):.12;
    const int clock_order=argc>2?atoi(argv[2]):0;
    if(duration<=0 || duration>120)return 2;   // 11.11.2026: 120 с — сцены демо после загрузки
    std::vector<uint8_t> adf;
    if(argc>3 && argv[3][0]!='-'){std::ifstream disk(argv[3],std::ios::binary);adf.assign(std::istreambuf_iterator<char>(disk),{});if(adf.size()!=901120)return 3;}
    Disk hdf;
    if(argc>4&&!hdf.load(argv[4]))return 3;
    // Второй диск (unit 5, ядро 0.11+): у AMIGA.ELF 0.18 — карта Evo; здесь — образ из файла, только чтение.
    Disk hdf1;
    if(argc>5&&!hdf1.load(argv[5]))return 3;
    unsigned hdf_reads=0,hdf_writes=0,hdf1_reads=0;Disk *serve=nullptr;
    // FLOPPY_FAST: дискета отвечает без задержки, как ESP на плате, а не раз в 4 мс.
    const bool floppy_fast=getenv("FLOPPY_FAST")!=nullptr;
    unsigned sectors_served=0,transfer_word=0;uint32_t sector=0,token=0;bool transferring=false;
    // JOY_EVENTS="секунды:устройство:байт,…": пакет джойстика HID (команда 3), как hid_packet у AMIGA.ELF; пишется, когда шина хоста свободна.
    std::vector<std::pair<uint64_t,unsigned>> hid_writes;size_t hid_next=0;
    if(const char *e=getenv("JOY_EVENTS"))for(const char *p=e;*p;){
        double s;unsigned d,b;int used=0;
        if(sscanf(p,"%lf:%u:%u%n",&s,&d,&b,&used)!=3)return 6;
        const uint64_t at=uint64_t(s*28500000);
        hid_writes.push_back({at,0x103});hid_writes.push_back({at,d});hid_writes.push_back({at,b});
        p+=used;if(*p==',')p++;
    }
    std::ofstream disklog("disk.csv");disklog<<"seconds,sector,token,drive\n";
    std::ofstream written("written.bin",std::ios::binary);
    VerilatedContext ctx;Vboot_top q{&ctx};
    // DIV3 uses CLKOUTP at 180 degrees; its rise aligns with CLKOUT rise.
    q.clk_28m=0;q.clk_85m=1;q.hclk=0;q.por=1;q.rom_done=0;q.trace_index=0;q.eval();
    unsigned pixel_phase=0;
    // 11.11.2026: такт вывода 27 МГц (576p50/480p60, ядро 0.13.0+): 6/19 фронтов 85,5 МГц. Прежде 33/38 — 74,25 МГц (720p),
    // вывод читал кадровый буфер в 2,75 раза чаще, чем на плате.
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
        // Как hd0_attach: снять образ, подключить новый. HDF_ATTACH_S — на работающей Amiga, HDF_RESET_S — затем сброс, как меню F11.
        static const uint64_t attach_at=getenv("HDF_ATTACH_S")?uint64_t(atof(getenv("HDF_ATTACH_S"))*28500000):6000;
        static const uint64_t reset_at=getenv("HDF_RESET_S")?uint64_t(atof(getenv("HDF_RESET_S"))*28500000):0;
        q.soft_reset=reset_at&&n>=reset_at&&n<reset_at+300;
        if(!hdf.empty()&&n==attach_at){q.host_wr=1;q.host_addr=0x65;q.host_data=4;}
        if(!hdf.empty()&&n==attach_at+100){q.host_wr=1;q.host_addr=0x60;q.host_data=0;}
        if(!hdf.empty()&&n==attach_at+200){q.host_wr=1;q.host_addr=0x60;q.host_data=unsigned(hdf.size());}
        if(!hdf.empty()&&n==attach_at+300){q.host_wr=1;q.host_addr=0x65;q.host_data=0;}
        if(!hdf1.empty()&&n==attach_at+400){q.host_wr=1;q.host_addr=0x65;q.host_data=5;}
        if(!hdf1.empty()&&n==attach_at+500){q.host_wr=1;q.host_addr=0x60;q.host_data=unsigned(hdf1.size());}
        if(!hdf1.empty()&&n==attach_at+600){q.host_wr=1;q.host_addr=0x65;q.host_data=0;}
        if(q.disk_status&4){printf("FAIL disk mailbox error %08X\n",q.disk_status);return 4;}
        // Poll at the real app's 4 ms cadence; bound host transfer rate too.
        // Опрос как у настоящего приложения: дискета раз в 4 мс; к диску — сразу, как при HD0.HDF.
        if(!transferring && (q.disk_status&1) && (((q.disk_status>>8)&255)>=0x10 || floppy_fast || n%114000==0)) {
            unsigned drive=(q.disk_status>>8)&255;
            if(drive==1){if(adf.empty()||(q.disk_status&8)||q.disk_sector>=1760)return 5;}
            else if(drive==0x10){
                if(hdf.empty()||q.disk_sector>=hdf.size()/512)return 5;
                if(q.disk_status&8){
                    // Как хост: окно передачи целиком в образ, в порядке байт ESP; копия — в written.bin для проверки.
                    uint8_t *p=hdf.at(q.disk_sector);
                    for(unsigned i=0;i<128;i++){
                        q.tx_index=i;q.eval();
                        for(unsigned k=0;k<4;k++)p[i*4+k]=uint8_t(q.tx_word>>(8*k));
                    }
                    const uint32_t number=q.disk_sector;
                    written.write((const char *)&number,4);written.write((const char *)p,512);written.flush();
                    hdf_writes++;q.host_wr=1;q.host_addr=0x64;q.host_data=q.disk_token;goto clocks;
                }
                hdf_reads++;
            } else if(drive==0x20){
                // Том EVO Amiga только читает: запись или чтение за краем — сразу провал с номером сектора.
                if(hdf1.empty()||(q.disk_status&8)||q.disk_sector>=hdf1.size()/512){
                    printf("FAIL HD1 %s sector %u\n",(q.disk_status&8)?"write":"read",q.disk_sector);return 5;}
                hdf1_reads++;
            } else return 5;
            serve=drive==0x10?&hdf:drive==0x20?&hdf1:nullptr;
            sector=q.disk_sector;token=q.disk_token;transfer_word=0;transferring=true;
            disklog<<double(n)/28500000<<","<<sector<<","<<token<<","<<drive<<"\n";disklog.flush();
        }
        if(transferring && (serve || floppy_fast || n%228==0)){
            q.host_wr=1;
            if(transfer_word<128){
                auto p=(serve?serve->at(sector):adf.data()+size_t(sector)*512)+transfer_word*4;q.host_addr=0x1400+transfer_word;
                q.host_data=uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
                transfer_word++;
            }else{q.host_addr=0x64;q.host_data=token;transferring=false;sectors_served++;}
        }
        clocks:
        if(!q.host_wr && hid_next<hid_writes.size() && n>=hid_writes[hid_next].first){
            q.host_wr=1;q.host_addr=0x04;q.host_data=hid_writes[hid_next++].second;
        }
        if(n==2850)q.por=0;            // 100 us
        if(n==142500)q.rom_done=1;     // 5 ms, preloaded ROM ready
        for(int phase=0;phase<2;phase++) {
            auto fast_edge=[&](){
                bool publish=clock85 && q.timing_publish;
                clock85=!clock85;q.clk_85m=clock85;
                pixel_phase+=6;if(pixel_phase>=19){pixel_phase-=19;q.hclk=!q.hclk;}
                ctx.timeInc(1);q.eval();
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
                pixel_phase+=6;if(pixel_phase>=19){pixel_phase-=19;q.hclk=!q.hclk;}
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
            // 11.11.2026: диагностика видео (amiga_video.sv: старший байт — пропуски захвата, overruns) — с --video
            printf("sim %.2f s, wall %.1f s, CPU status %08X steps %u addr %08X RGB clocks %llu frames %u video %08X\n",
                double(n)/28500000,wall,q.cpu_debug[0],q.cpu_debug[1],q.cpu_debug[2],(unsigned long long)color,frames,q.video_diag);
            fflush(stdout);
        }
    }
    bool vectors=q.cpu_debug[8]==0 && q.cpu_debug[10]==2 && q.cpu_debug[12]==4 && q.cpu_debug[14]==6 &&
        (q.cpu_debug[9]>>16)==0x1114 && (q.cpu_debug[11]>>16)==0x4ef9 &&
        (q.cpu_debug[13]>>16)==0xf8 && (q.cpu_debug[15]>>16)==0xd2;
    std::ofstream out("boot.json");
    printf("HDF sectors read %u, written %u; HD1 read %u\n",hdf_reads,hdf_writes,hdf1_reads);
    out<<"{\n\"hdf_reads\":"<<hdf_reads<<",\n\"hdf1_reads\":"<<hdf1_reads<<",\n\"sectors_served\":"<<sectors_served<<",\n\"simulated_seconds\":"<<duration<<",\n\"reset_released\":"<<(!q.reset_out?"true":"false")
       <<",\n\"cpu_reset_released\":"<<((q.cpu_debug[0]&1)?"true":"false")
       <<",\n\"sdram_ready\":"<<(q.sdram_ready?"true":"false")
       <<",\n\"reset_vectors_match\":"<<(vectors?"true":"false")
       <<",\n\"cpu_steps\":"<<q.cpu_debug[1]<<",\n\"nonblack_cycles\":"<<color
       <<",\n\"frames\":"<<frames<<",\n\"video_diagnostic\":"<<q.video_diag<<",\n\"words\":[";
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
