"""Cross-backend SDRAM check: original vendor model vs Verilator phase adapter.

The test keeps the FPGA controller unchanged, drives its real pins through
the same pad delays as the full model, and checks reads and masked writes.
"""
from pathlib import Path
import argparse,hashlib,json,shutil,subprocess
from build import HERE,PROJECT,RTL_TOOLS,VC,vendor_models,generate_wrapper,run,environment,sha

def check(profile='stable',negative=False):
    source=HERE/'reference/0.11' if profile=='stable' else PROJECT
    inputs={n:sha(source/'fpga'/n) for n in ['amigaclks.v','sdram.sv','amiga_vdac4.sv']}
    inputs.update(test=sha(__file__),builder=sha(HERE/'build.py'),negative=negative)
    key=hashlib.sha256(json.dumps(inputs,sort_keys=True).encode()).hexdigest()[:12]
    stage=Path(r'E:\zx\cache\tmp\amiga-simulator')/('memory-'+key);stage.mkdir(parents=True,exist_ok=True)
    if (stage/'result.json').exists():return json.loads((stage/'result.json').read_text())
    vendor=vendor_models(stage);generate_wrapper(source,stage,True)
    # Icarus requires declarations before references inside generate scopes.
    # Relocate declarations only; the full Verilator model uses the original order.
    controller=(stage/'sdram_sim.sv').read_text()
    if 'reg output_video=0;' in controller:
        declarations=[]
        for declaration in ['reg [1:0] video_word_index;','reg output_video=0;',
            'wire [31:0] video_word = VIDEO_STREAM ? video_din[31:0]:video_din[video_word_index*32 +: 32];']:
            assert controller.count(declaration)==1
            controller=controller.replace(declaration,'');declarations.append(declaration)
        controller=controller.replace('localparam WIDTH32', '\n'.join(declarations)+'\nlocalparam WIDTH32',1)
        (stage/'sdram_sim.sv').write_text(controller)
    if negative:
        p=stage/'gowin_models.v';p.write_text('`undef VERILATOR\n'+p.read_text()+'\n`define VERILATOR 1\n')
    shutil.copy2(source/'fpga/amigaclks.v',stage/'amigaclks.v')
    pads=(stage/'sim_top.sv').read_text().split(' wire chip_clock;')[1].split(' assign probe_data')[0]
    pads=' wire chip_clock;'+pads.replace('dut.sdram.sim_io','ctrl.sim_io').replace('dut.sdram.sd_dq','ctrl.sd_dq')
    extra=",.video_cs(1'b0),.video_we(1'b0),.video_addr(21'd0),.video_din(128'd0)" if 'input wire video_cs' in (stage/'sdram_sim.sv').read_text() else ''
    tb='''`timescale 1ps/1ps
module tb;
reg osc=0,g=0;always #18518.518 osc=~osc;
GSR GSR(.GSRI(g));
wire clk28,clk85,sd_clock,locked;
amigaclks clocks(.clk_in(osc),.clk_28m(clk28),.clk_85m(clk85),.clk_sdram(sd_clock),.locked(locked),.vidmode(1'b1));
wire [31:0] dq;wire ready,cke,cs,we,ras,cas;wire [10:0] addr;wire [1:0] ba;wire [3:0] dqm;
reg [1:0] div=0;always @(posedge clk28)div<=div+1'b1;
reg write_request=0;reg [21:0] location=0;reg [15:0] value=0;
wire [15:0] data;
sdram #(.DATA_WIDTH(32),.RAS_WIDTH(11),.CAS_WIDTH(8),.RAM_CLOCK_SPEED(85500000),.SYNC_CLOCK_SPEED(7125000),.CHIP48_BURST(1)) ctrl(
.clk(clk85),.reset_n(locked),.ready(ready),.sync(div==0),.refresh(1'b0),
.sd_data(dq),.sd_addr(addr),.sd_ba(ba),.sd_dqm(dqm),.sd_cke(cke),.sd_cs(cs),.sd_we(we),.sd_ras(ras),.sd_cas(cas),
.din(value),.addr(location),.ds(2'd0),.cs(1'b1),.we(write_request),.dout(data),
.p2_din(16'd0),.p2_addr(23'd0),.p2_ds(2'd0),.p2_cs(1'b0),.p2_we(1'b0)'''+extra+''');
'''+pads+'''
always @(posedge sd_clock)if($time>500000000 && $time<500050000)$display("SD %0t",$time);
initial begin
 #100000 g=1;#500000000;
 if(!ready)$fatal(1,"SDRAM not ready");
 if(data!==16'hccaa)$fatal(1,"Read mismatch %h; expected ccaa",data);
 @(negedge clk28);write_request=1;value=16'h1234;repeat(24)@(negedge clk28);
 write_request=0;repeat(24)@(negedge clk28);
 if(data!==16'h1234)$fatal(1,"Upper write/read mismatch %h",data);
 location=1;write_request=1;value=16'hab12;repeat(24)@(negedge clk28);
 write_request=0;repeat(24)@(negedge clk28);
 if(data!==16'hab12)$fatal(1,"Lower write/read mismatch %h",data);
 location=0;repeat(24)@(negedge clk28);
 if(data!==16'h1234 || chip.mem[0]!==32'h1234ab12)$fatal(1,"Byte-mask preservation failed");
 $display("PASS memory: reads, writes and lane masks");$finish;
end
endmodule
'''
    (stage/'tb.sv').write_text(tb)
    sources=['tb.sv','sdram_sim.sv','sdram_chip.sv','amigaclks.v','gowin_models.v']
    run([RTL_TOOLS/'bin/iverilog.exe','-g2012','-s','tb','-o','memory.vvp',*sources],stage,'iverilog.log')
    run([RTL_TOOLS/'bin/vvp.exe','memory.vvp'],stage,'icarus.log')
    (stage/'main.cpp').write_text('''#include "Vtb.h"
#include "verilated.h"
int main(){VerilatedContext c;const char*a[]={"memory-test"};c.commandArgs(1,a);Vtb q{&c};q.eval();while(!c.gotFinish()){c.time(q.nextTimeSlot());q.eval();}q.final();}
''')
    run([RTL_TOOLS/'bin/verilator_bin.exe','--cc','--timing','--Wno-fatal','-Wno-BLKANDNBLK','--top-module','tb','--Mdir','obj',*sources],stage,'verilator.log')
    inc=RTL_TOOLS/'share/verilator/include'
    args=['/nologo','/EHsc','/std:c++20','/O2','/MD','/MP4','/DVL_TIME_CONTEXT','/I'+str(inc),'/I'+str(inc/'vltstd'),'/Iobj','main.cpp',*list((stage/'obj').glob('*.cpp')),inc/'verilated.cpp',inc/'verilated_threads.cpp',inc/'verilated_timing.cpp','/Fememory.exe']
    (stage/'compile.rsp').write_text(subprocess.list2cmdline(list(map(str,args))),encoding='utf-8-sig')
    run([VC/'bin/Hostx64/x64/cl.exe','@compile.rsp'],stage,'compile.log')
    p=subprocess.run([str(stage/'memory.exe')],cwd=stage,env=environment(),capture_output=True,text=True,timeout=30)
    text=p.stdout+p.stderr;(stage/'verilator-run.log').write_text(text)
    reference=(stage/'icarus.log').read_text()
    edges=lambda t:[n for n in t.splitlines() if n.startswith('SD ')]
    result={'passed':p.returncode==0 and edges(reference)==edges(text) and len(edges(text))>=4,
            'negative_control':negative,'profile':profile,'stage':str(stage),'vendor':vendor,
            'icarus':reference,'verilator':text,'phase_timestamps_equal':edges(reference)==edges(text)}
    (stage/'result.json').write_text(json.dumps(result,indent=2));return result

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--profile',choices=['stable','current'],default='stable');ap.add_argument('--negative',action='store_true');a=ap.parse_args()
    result=check(a.profile,a.negative);print(json.dumps(result,indent=2));raise SystemExit(0 if (not result['passed'] if a.negative else result['passed']) else 1)
