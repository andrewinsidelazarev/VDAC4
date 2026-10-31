"""Stress concurrent chipset DMA, CPU SDRAM traffic and refresh arbitration."""
from pathlib import Path
import hashlib,json,os,subprocess
root=Path(__file__).resolve().parents[1]
stage=root/'tests/shared-sdram-build';stage.mkdir(exist_ok=True)
(root/'tests/shared-sdram-result.json').write_text(json.dumps({'passed':False,'state':'running'}))
(stage/'shared_memory_top.sv').write_text('''module shared_memory_top(
 input clk,sd_clk,reset_n,sync,p2_cs,p2_we,input [22:0] p2_addr,
 input p1_cs,p1_we,refresh,input [21:0] p1_addr, input [15:0] p1_din,input [1:0] p1_ds,
 output [15:0] p1_dout,output [47:0] p1_dout48,
 input [15:0] p2_din,input [1:0] p2_ds,
 output ready,p2_ack,output [15:0] p2_dout,output [47:0] p2_dout48,
 input [20:0] probe_addr,output [31:0] probe_data);
 wire [31:0] dq;wire [10:0] a;wire [3:0] dqm;wire [1:0] ba;
 wire cke,cs_n,we_n,ras_n,cas_n;
 sdram #(.DATA_WIDTH(32),.RAS_WIDTH(11),.CAS_WIDTH(8),
 .RAM_CLOCK_SPEED(85500000),.SYNC_CLOCK_SPEED(7125000),
 .CHIP48_BURST(1),.SYNC_DELAY(3)) dut(
 .clk(clk),.reset_n(reset_n),.ready(ready),.sync(sync),.refresh(refresh),
 .sd_data(dq),.sd_addr(a),.sd_dqm(dqm),.sd_ba(ba),.sd_cke(cke),
 .sd_cs(cs_n),.sd_we(we_n),.sd_ras(ras_n),.sd_cas(cas_n),
 .din(p1_din),.dout(p1_dout),.dout48(p1_dout48),.addr(p1_addr),.ds(p1_ds),.cs(p1_cs),.we(p1_we),
 .p2_din(p2_din),.p2_dout(p2_dout),.p2_dout48(p2_dout48),
 .p2_addr(p2_addr),.p2_ds(p2_ds),.p2_cs(p2_cs),.p2_we(p2_we),.p2_ack(p2_ack));
 sdr_chip_model chip(.clk(sd_clk),.dq(dq),.addr(a),.dqm(dqm),.ba(ba),
 .cs_n(cs_n),.ras_n(ras_n),.cas_n(cas_n),.we_n(we_n));
 assign probe_data=chip.mem[probe_addr];
 endmodule
''')
model=(root/'upstream/sim/sdr_chip_model.v').read_text()
model=model.replace('reg [10:0] row [0:3];',"initial for(integer n=0;n<2097152;n=n+1) mem[n]=32'hccaa55cc ^ (n*32'h9e3779b9);\nreg [10:0] row [0:3];")
(stage/'sdr_chip_model.v').write_text(model)
tool=Path(r'E:\zx\tools\rtl\ucrt64')
vc=Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207')
sdk=Path(r'C:\Program Files (x86)\Windows Kits\10');v='10.0.26100.0'
env=os.environ.copy();env['PATH']=str(tool/'bin')+';'+str(vc/'bin/Hostx64/x64')+';'+env['PATH']
env['VERILATOR_ROOT']=str(tool/'share/verilator')
env['INCLUDE']=';'.join(map(str,[vc/'include',*[sdk/'Include'/v/x for x in ['ucrt','shared','um']]]))
env['LIB']=';'.join(map(str,[vc/'lib/x64',*[sdk/'Lib'/v/x/'x64' for x in ['ucrt','um']]]))
def run(cmd,name):
    print(name,flush=True)
    with (stage/name).open('wb') as log:
        p=subprocess.run(list(map(str,cmd)),cwd=stage,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=300)
    if p.returncode:raise RuntimeError((stage/name).read_text(errors='replace')[-5000:])
run([tool/'bin/verilator_bin.exe','--cc','--Wno-fatal','--top-module','shared_memory_top','--Mdir','obj',stage/'shared_memory_top.sv',stage/'sdr_chip_model.v',root/'fpga/sdram.sv'],'verilator.log')
inc=tool/'share/verilator/include'
cmd=['/nologo','/EHsc','/std:c++20','/utf-8','/O2','/MD','/DVL_TIME_CONTEXT','/MP4','/I'+str(inc),'/I'+str(inc/'vltstd'),'/Iobj',root/'tests/shared_memory_test.cpp',*list((stage/'obj').glob('*.cpp')),inc/'verilated.cpp',inc/'verilated_threads.cpp','/Feshared_memory_test.exe']
(stage/'compile.rsp').write_text(subprocess.list2cmdline(list(map(str,cmd))))
run([vc/'bin/Hostx64/x64/cl.exe','@compile.rsp'],'compile.log')
run([stage/'shared_memory_test.exe'],'run.log')
text=(stage/'run.log').read_text()
report={'passed':True,'result':text,'hardware':False,'electrical_timing':False,
 'source_sha256':{p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in ['fpga/sdram.sv','upstream/sim/sdr_chip_model.v','tests/run_shared_sdram.py','tests/shared_memory_test.cpp']}}
(root/'tests/shared-sdram-result.json').write_text(json.dumps(report,indent=2)+'\n')
print(text)
