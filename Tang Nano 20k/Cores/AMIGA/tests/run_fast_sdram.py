"""Exercise CPU SDRAM writes, byte masks and read bursts on the real controller."""
from pathlib import Path
import hashlib,json,os,subprocess
root=Path(__file__).resolve().parents[1]
stage=root/'tests/fast-sdram-build';stage.mkdir(exist_ok=True)
(stage/'fast_memory_top.sv').write_text('''module fast_memory_top(
 input clk,sd_clk,reset_n,sync,p2_cs,p2_we,input [22:0] p2_addr,
 input [15:0] p2_din,input [1:0] p2_ds,
 output ready,p2_ack,output [15:0] p2_dout,output [47:0] p2_dout48,
 input [20:0] probe_addr,output [31:0] probe_data);
 wire [31:0] dq;wire [10:0] a;wire [3:0] dqm;wire [1:0] ba;
 wire cke,cs_n,we_n,ras_n,cas_n;
 sdram #(.DATA_WIDTH(32),.RAS_WIDTH(11),.CAS_WIDTH(8),
 .RAM_CLOCK_SPEED(85500000),.SYNC_CLOCK_SPEED(7125000),
 .CHIP48_BURST(1),.SYNC_DELAY(3)) dut(
 .clk(clk),.reset_n(reset_n),.ready(ready),.sync(sync),.refresh(1'b0),
 .sd_data(dq),.sd_addr(a),.sd_dqm(dqm),.sd_ba(ba),.sd_cke(cke),
 .sd_cs(cs_n),.sd_we(we_n),.sd_ras(ras_n),.sd_cas(cas_n),
 .din(16'b0),.dout(),.dout48(),.addr(22'b0),.ds(2'b0),.cs(1'b0),.we(1'b0),
 .p2_din(p2_din),.p2_dout(p2_dout),.p2_dout48(p2_dout48),
 .p2_addr(p2_addr),.p2_ds(p2_ds),.p2_cs(p2_cs),.p2_we(p2_we),.p2_ack(p2_ack));
 sdr_chip_model chip(.clk(sd_clk),.dq(dq),.addr(a),.dqm(dqm),.ba(ba),
 .cs_n(cs_n),.ras_n(ras_n),.cas_n(cas_n),.we_n(we_n));
 assign probe_data=chip.mem[probe_addr];
 endmodule
''')
model=(root/'upstream/sim/sdr_chip_model.v').read_text()
model=model.replace('reg [10:0] row [0:3];',"initial for(integer n=0;n<2097152;n=n+1) mem[n]=32'hccaa55cc;\nreg [10:0] row [0:3];")
(stage/'sdr_chip_model.v').write_text(model)
(stage/'fast_memory_test.cpp').write_text(r'''
#include "Vfast_memory_top.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
int main(){
 for(unsigned phase:{0u,180u}){
 Vfast_memory_top q;unsigned cycles=0,seed=0x12345678,reads=0,writes=0;
 std::vector<unsigned> mem(2097152,0xccaa55cc);
 auto tick=[&](){q.clk=0;q.sd_clk=phase==180;q.sync=(cycles%12)<3;q.eval();q.clk=1;q.sd_clk=phase==0;q.eval();cycles++;};
 auto idle=[&](){q.p2_cs=0;for(int n=0;n<24;n++)tick();};
 auto random=[&](){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;};
 auto word=[&](unsigned a){return (mem[a>>1]>>((a&1)?0:16))&65535;};
 q.reset_n=0;q.p2_cs=0;tick();q.reset_n=1;
 for(unsigned n=0;n<12000&&!q.ready;n++)tick();CHECK(q.ready);idle();
 auto transfer=[&](unsigned a,bool write,unsigned data,unsigned mask){
  unsigned ack=q.p2_ack;q.p2_addr=a;q.p2_we=write;q.p2_din=data;q.p2_ds=mask;q.p2_cs=1;
  unsigned wait=0;do {tick();CHECK(++wait<100);}while(q.p2_ack==ack);
  q.p2_cs=0;
 };
 for(unsigned n=0;n<8192;n++){
  // All banks, both physical halfwords, row boundaries and every byte mask.
  unsigned a=(n<256)?((n&3)<<20)|((n*127)&0xfffff):random()&0x3fffff;
  unsigned data=(n<256)?(1u<<(n%16)):random()&65535,mask=(n/16)&3;
  transfer(a,true,data,mask);idle();writes++;
  unsigned shift=(a&1)?0:16,m=(((mask&1)?0:255)|((mask&2)?0:65280))<<shift;
  mem[a>>1]=(mem[a>>1]&~m)|((data<<shift)&m);
  q.probe_addr=a>>1;q.eval();CHECK(q.probe_data==mem[a>>1]);
  transfer(a,false,0,0);reads++;
  if(q.p2_dout!=word(a)){fprintf(stderr,"read %06X got %04X expected %04X\n",a,q.p2_dout,word(a));return 1;}
  // Four 32-bit beats wrap within the aligned SDRAM burst block.
  unsigned a1=(a&~7u)|((a+1)&7),a2=(a&~7u)|((a+2)&7),a3=(a&~7u)|((a+3)&7);
  uint64_t tail=(uint64_t(word(a1))<<32)|(uint64_t(word(a2))<<16)|word(a3);
  CHECK(q.p2_dout48==tail);idle();
 }
 printf("PASS phase %u: %u CPU writes and %u burst reads: four banks, both DQ halves, all byte masks, walking bits and random data\n",phase,writes,reads);
 }
}
''')
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
run([tool/'bin/verilator_bin.exe','--cc','--Wno-fatal','--top-module','fast_memory_top','--Mdir','obj',stage/'fast_memory_top.sv',stage/'sdr_chip_model.v',root/'fpga/sdram.sv'],'verilator.log')
inc=tool/'share/verilator/include'
cmd=['/nologo','/EHsc','/std:c++20','/utf-8','/O2','/MD','/DVL_TIME_CONTEXT','/MP4','/I'+str(inc),'/I'+str(inc/'vltstd'),'/Iobj',stage/'fast_memory_test.cpp',*list((stage/'obj').glob('*.cpp')),inc/'verilated.cpp',inc/'verilated_threads.cpp','/Fefast_memory_test.exe']
(stage/'compile.rsp').write_text(subprocess.list2cmdline(list(map(str,cmd))))
run([vc/'bin/Hostx64/x64/cl.exe','@compile.rsp'],'compile.log')
run([stage/'fast_memory_test.exe'],'run.log')
text=(stage/'run.log').read_text()
report={'passed':True,'result':text,'hardware':False,'electrical_timing':False,
 'source_sha256':{p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in ['fpga/sdram.sv','upstream/sim/sdr_chip_model.v','tests/run_fast_sdram.py']}}
(root/'tests/fast-sdram-result.json').write_text(json.dumps(report,indent=2)+'\n')
print(text)
