"""Full LINK ROM load while real RGB888 video DMA shares timed SDRAM pins.

This checks stored bytes, not just the LINK input CRC. Pad delays are a
representative routed corner, not a board-level signal-integrity model.
"""
from pathlib import Path
import argparse, ast, hashlib, json, os, re, subprocess

ap = argparse.ArgumentParser()
ap.add_argument('--rom', required=True)
ap.add_argument('--period', type=int, default=1140,
                help='QSPI period in ticks of 1/11.286 GHz')
ap.add_argument('--phase', type=int, default=0)
args = ap.parse_args()
root = Path(__file__).resolve().parents[1]
stage = root/'tests/rom-video-build'
stage.mkdir(exist_ok=True)
report_path = root/'tests/rom-video-result.json'
report_path.write_text(json.dumps({'passed': False, 'state': 'running'}))

# Reuse the complete production SDRAM instance, not a hand-written port map.
top = (root/'fpga/amiga_vdac4.sv').read_text()
controller = 'sdram #(\n'+top.split('sdram #(\n', 1)[1].split('// 720p renderer', 1)[0]
controller = controller.replace(') sdram (', ') memory (')
tree = ast.parse((root/'tests/run_rom_sdram.py').read_text())
prefix = next(ast.literal_eval(n.value) for n in tree.body
              if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'prefix' for t in n.targets))
prefix = prefix.replace('input clk,clk85,rst_n', 'input clk,clk85,hclk,sd_clk,rst_n')
prefix = prefix.replace('output [31:0] probe_data);',
                        'output [31:0] probe_data,output [31:0] video_writes,video_reads,frames);')
prefix += '''
wire [3:0] picture;
wire [31:0] video_diagnostic;
wire video_cs,video_we,video_ack,video_read_valid;
wire [20:0] video_address;
wire [127:0] video_din,video_dout;
wire [1:0] video_fetch_index;
'''
# Use the same 270-degree clock and directional pad bridge as the pixel test.
timed = root/'tests/video-timed-build'
staged = json.loads((timed/'sources.json').read_text())
for n in ['fpga/sdram.sv','tests/sdram_burst_model.v']:
    assert hashlib.sha256((root/n).read_bytes()).hexdigest()==staged[n], ('Stale IO model',n)
io = staged['io_paths']
assert io['clock_ps'] > 0 and io['read_access_ps'] == 6000
suffix = f'''
wire chip_clk;
wire [31:0] write_pin,read_pin,read_core;
wire read_enable,read_core_enable,write_pin_enable;
wire [10:0] addr_pin;wire [3:0] dqm_pin;wire [1:0] ba_pin;
wire cs_pin,we_pin,ras_pin,cas_pin;
assign #{io['clock_ps']} chip_clk=sd_clk;
assign #{io['write_ps']} write_pin=memory.io32.dq_out;
assign #{io['write_ps']} write_pin_enable=memory.sd_dq;
assign #{io['read_input_ps']} read_core=read_pin;
assign #{io['read_input_ps']} read_core_enable=read_enable;
assign IO_sdram_dq=!memory.sd_dq && read_core_enable ? read_core:32'bz;
assign #{io['command_ps']} {{addr_pin,dqm_pin,ba_pin,cs_pin,we_pin,ras_pin,cas_pin}}=
    {{O_sdram_addr,O_sdram_dqm,O_sdram_ba,O_sdram_cs_n,O_sdram_wen_n,O_sdram_ras_n,O_sdram_cas_n}};
sdr_chip_model chip(.clk(chip_clk),.dq_in(write_pin),.dq_out(read_pin),
 .dq_oe(read_enable),.write_enable(write_pin_enable),.addr(addr_pin),.dqm(dqm_pin),
 .ba(ba_pin),.cs_n(cs_pin),.we_n(we_pin),.ras_n(ras_pin),.cas_n(cas_pin));
assign probe_data=chip.mem[21'h1e0000+probe_addr];
reg [10:0] nx=0;reg [9:0] ny=0;
always @(posedge clk)begin
 if(!rst_n || !sdram_ready)begin nx<=0;ny<=0;end
 else if(nx==1815)begin nx<=0;ny<=ny==312?0:ny+1'b1;end
 else nx<=nx+1'b1;
end
reg [31:0] vw=0,vr=0,fp=0;reg published_seen=0;
assign video_writes=vw,video_reads=vr,frames=fp;
always @(posedge clk85)begin
 if(memory.state==memory.STATE_CAS && memory.sdram_port==memory.PORT_VIDEO)begin
  if(memory.ram_we)vw<=vw+1;else vr<=vr+1;
 end
 if(video.published!=published_seen)begin fp<=fp+1;published_seen<=video.published;end
end
amiga_video video(.clk28(clk),.clk85(clk85),.hclk(hclk),.hclk5(hclk),
 .reset28(!rst_n||!sdram_ready),.reset85(!rst_n||!sdram_ready),.hreset(!rst_n||!sdram_ready),
 .hs(nx>=132),.vs(ny>=3),.lace(1'b0),.field(1'b0),
 .window_in(nx>=411 && nx<1691 && ny>=42 && ny<242),
 .rgb_in({{nx[7:0],ny[7:0],nx[7:0]^ny[7:0]}}),.picture(picture),
 .osd_wr(1'b0),.osd_addr(9'd0),.osd_data(32'd0),.osd_show(1'b0),.osd_sel(6'd63),
 .osd_band(30'd0),.osd_rule(30'd0),
 .ram_cs(video_cs),.ram_we(video_we),.ram_address(video_address),.ram_din(video_din),
 .ram_dout(video_dout),.ram_ack(video_ack),.ram_read_valid(video_read_valid),.ram_fetch_index(video_fetch_index),
 .diagnostic(video_diagnostic),.tmds_clock(),.tmds());
endmodule
module serializer #(parameter NUM_CHANNELS=3,VIDEO_RATE=32000000)(
 input clk_pixel,clk_pixel_x5,reset,input [9:0] tmds_internal[NUM_CHANNELS-1:0],
 output [2:0] tmds,output tmds_clock);
assign tmds=0,tmds_clock=clk_pixel;
endmodule
'''
(stage/'rom_memory_top.sv').write_text(prefix+controller+suffix)

# Reuse the delay model already exercised by the pixel tests, guarded by hashes.
timed = root/'tests/video-timed-build'
for n in ['sdr_chip_model.v', 'sdram_io_test.sv']:
    (stage/n).write_bytes((timed/n).read_bytes())
assert (stage/'sdram_io_test.sv').read_text().replace('begin:io32', 'begin') == (root/'fpga/sdram.sv').read_text()

cpp = '#define ROM_MEMORY_TEST\n'+(root/'tests/link_test.cpp').read_text().split('int main(', 1)[0]
cpp = cpp.replace('Vlink_top', 'Vrom_memory_top')
cpp = cpp.replace('t=0,edge=200;', 't=0,e28=198,e85=66,eh=76,es=165;')
cpp = cpp.replace('q.clk=0;q.sck=0;', 'q.clk=0;q.clk85=0;q.hclk=0;q.sd_clk=0;q.probe_addr=0;q.sck=0;')
cpp = cpp.replace('advance(2000+phase);', 'advance(3000000+phase);CHECK(q.sdram_ready);')
a = cpp.index('    void advance(uint64_t d) {')
b = cpp.index('    unsigned cycle(', a)
cpp = cpp[:a]+'''    void settle(uint64_t tick) {
        uint64_t ps=(tick*1000000ull+5643ull)/11286ull;
        while(q.eventsPending() && q.nextTimeSlot()<ps){ctx.time(q.nextTimeSlot());q.eval();}
        ctx.time(ps);q.eval();
    }
    void advance(uint64_t d) {
        uint64_t end=t+d;
        for(;;){
            uint64_t next=std::min(std::min(e28,e85),std::min(eh,es));
            if(next>end)break;
            t=next;settle(t);
            if(t==e28 && !q.clk){
                if(q.rom_we && !last_we){
                    CHECK(q.rom_addr*2+1<memory.size());
                    memory[q.rom_addr*2]=q.rom_word>>8;
                    memory[q.rom_addr*2+1]=q.rom_word;writes++;
                }
                last_we=q.rom_we;
            }
            if(t==e28){q.clk=!q.clk;e28+=198;}
            if(t==e85){q.clk85=!q.clk85;e85+=66;}
            if(t==eh){q.hclk=!q.hclk;eh+=76;}
            if(t==es){q.sd_clk=!q.sd_clk;es+=66;}
            q.eval();
        }
        t=end;settle(t);
    }
'''+cpp[b:]
cpp = cpp.replace('advance(400ull*16*257);', 'advance(396ull*16*257);')
cpp += '''
int main(int argc,char **argv){
 CHECK(argc==4);std::ifstream f(argv[1],std::ios::binary);CHECK(f.good());
 std::vector<uint8_t> rom((std::istreambuf_iterator<char>(f)),{});CHECK(rom.size()==524288);
 unsigned period=unsigned(atoi(argv[2])),phase=unsigned(atoi(argv[3]));
 Sim s(period,phase);CHECK(s.rd(0)==0x56444334);s.wr(0x44,1);s.wr(0x58,crc(rom));
 for(unsigned off=0;off<rom.size();off+=256){
  s.packet(off/256+1,rom.data()+off);
  if(off%65536==0){printf("Loaded %u KiB, video writes %u reads %u fields %u\\n",off/1024,s.q.video_writes,s.q.video_reads,s.q.frames);fflush(stdout);}
 }
 s.wr(0x44,2);CHECK(s.q.rom_ready);CHECK(s.writes==262144);CHECK(s.memory==rom);
 s.advance(396*32);unsigned bad=0;
 for(unsigned n=0;n<rom.size()/4;n++){
  s.q.probe_addr=n;s.q.eval();
  uint32_t expected=(uint32_t(rom[4*n])<<24)|(uint32_t(rom[4*n+1])<<16)|(uint32_t(rom[4*n+2])<<8)|rom[4*n+3];
  if(s.q.probe_data!=expected){if(bad<12)printf("Mismatch %06X expected %08X got %08X\\n",n*4,expected,s.q.probe_data);bad++;}
 }
 printf("Stored ROM: %u/131072 wrong words, %u halfwords; video %u write %u read bursts, %u fields; QSPI period %u phase %u\\n",
        bad,s.writes,s.q.video_writes,s.q.video_reads,s.q.frames,period,phase);
 CHECK(bad==0);CHECK(s.q.video_writes>1000);CHECK(s.q.video_reads>1000);CHECK(s.q.frames>5);
 printf("PASS full 512 KiB QSPI ROM load under concurrent real RGB888 DMA with representative IO delays\\n");
}
'''
cpp = cpp.replace('#include <cstdint>', '#include <algorithm>\n#include <cstdint>')
(stage/'rom_memory_test.cpp').write_text(cpp)

tool = Path(r'E:\zx\tools\rtl\ucrt64')
vc = Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207')
sdk = Path(r'C:\Program Files (x86)\Windows Kits\10'); v = '10.0.26100.0'
env = os.environ.copy()
env['PATH'] = str(tool/'bin')+';'+str(vc/'bin/Hostx64/x64')+';'+env['PATH']
env['VERILATOR_ROOT'] = str(tool/'share/verilator')
env['INCLUDE'] = ';'.join(map(str, [vc/'include', *[sdk/'Include'/v/x for x in ['ucrt', 'shared', 'um']]]))
env['LIB'] = ';'.join(map(str, [vc/'lib/x64', *[sdk/'Lib'/v/x/'x64' for x in ['ucrt', 'um']]]))
def run(cmd, name):
    print(name, flush=True)
    with (stage/name).open('wb') as log:
        p = subprocess.run(list(map(str, cmd)), cwd=stage, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=1200)
    if p.returncode:
        raise RuntimeError((stage/name).read_text(errors='replace')[-5000:])

sources = ['fpga/'+n for n in ['amiga_link.sv', 'amiga_disk.sv', 'next_qspi.sv', 'fb_sdspi.sv',
           'sdram.sv', 'amiga_video.sv', 'amiga_video_capture.sv', 'amiga_video_geometry.sv',
           'amiga_video_cache.sv', 'amiga_video_dma.sv', 'nes_osd.sv', 'nes_osd_font.sv', 'hdmi_tx.sv', 'tmds_encoder.sv']]
run([tool/'bin/verilator_bin.exe', '--cc', '--timing', '--timescale', '1ps/1ps', '--Wno-fatal',
     '--top-module', 'rom_memory_top', '--Mdir', 'obj', stage/'rom_memory_top.sv',
     stage/'sdr_chip_model.v', stage/'sdram_io_test.sv', *[root/p for p in sources if p != 'fpga/sdram.sv']], 'verilator.log')
inc = tool/'share/verilator/include'
cmd = ['/nologo', '/EHsc', '/std:c++20', '/utf-8', '/O2', '/MD', '/DVL_TIME_CONTEXT', '/MP4',
       '/I'+str(inc), '/I'+str(inc/'vltstd'), '/Iobj', stage/'rom_memory_test.cpp',
       *list((stage/'obj').glob('*.cpp')), inc/'verilated.cpp', inc/'verilated_threads.cpp',
       inc/'verilated_timing.cpp', '/Ferom_memory_test.exe']
(stage/'compile.rsp').write_text(subprocess.list2cmdline(list(map(str, cmd))), encoding='utf-8-sig')
run([vc/'bin/Hostx64/x64/cl.exe', '@compile.rsp'], 'compile.log')
run([stage/'rom_memory_test.exe', Path(args.rom).resolve(), args.period, args.phase], 'run.log')
result = (stage/'run.log').read_text()
report = {'passed': True, 'result': result, 'hardware': False, 'electrical_timing': False,
          'io_paths': io, 'qspi_period_ticks': args.period, 'qspi_phase_ticks': args.phase,
          'source_sha256': {p: hashlib.sha256((root/p).read_bytes()).hexdigest()
                           for p in sources+['fpga/amiga_vdac4.sv', 'tests/link_test.cpp', 'tests/run_rom_video.py']},
          'rom_sha256': hashlib.sha256(Path(args.rom).read_bytes()).hexdigest()}
report_path.write_text(json.dumps(report, indent=2)+'\n')
print(result)
