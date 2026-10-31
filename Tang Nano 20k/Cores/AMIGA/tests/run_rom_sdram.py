"""Exercise the real QSPI ROM writer through the actual SDRAM controller/model."""
from pathlib import Path
import hashlib,json,os,re,subprocess
root=Path(__file__).resolve().parents[1]
stage=root/'tests/rom-sdram-build';stage.mkdir(exist_ok=True)
top=(root/'fpga/amiga_vdac4.sv').read_text()
controller=top.split('sdram #(\n',1)[1].split('wire [15:0] silent_audio',1)[0]
controller='sdram #(\n'+controller
prefix='''module rom_memory_top(input clk,clk85,rst_n,sck,cs_n,input [3:0] io_in,
output [3:0] io_out,output io_oe,boot_req,soft_reset,rom_ready,rom_we,
output [17:0] rom_addr,output [15:0] rom_word,
output hid_strobe,hid_start,hid_reset,output [7:0] hid_data,
output sd_cs_n,sd_sck,sd_mosi,output sdram_ready,
input [16:0] probe_addr,output [31:0] probe_data);
reg [1:0] phase=0;
always @(posedge clk) if (!rst_n) phase<=0; else phase<=phase+1'b1;
wire [31:0] debug_words [0:35];
wire [8:0] trace_index;wire [31:0] trace_start,video_probe;
for(genvar i=0;i<36;i=i+1) assign debug_words[i]=0;
amiga_link dut(.clk7_en(phase==0),.clk7n_en(phase==2),
.disk_reset(1'b0),.disk_rd(8'b0),.disk_wr(8'b0),.disk_sector(32'b0),
.disk_mounted(),.disk_data(),.disk_size(),.disk_busy(),.disk_done(),.disk_strobe(),.disk_addr(),
.audio_left(15'b0),.audio_right(15'b0),.key_free(8'd8),.sd_miso(sd_mosi),.*);
wire clk_85m=clk85,rst_sdram_n=rst_n;
wire [31:0] IO_sdram_dq;
wire [10:0] O_sdram_addr;wire [3:0] O_sdram_dqm;wire [1:0] O_sdram_ba;
wire O_sdram_cs_n,O_sdram_wen_n,O_sdram_ras_n,O_sdram_cas_n,O_sdram_cke;
localparam CHIP48_BURST=1;
wire sdram_sync=phase==0,sdram_refresh=0,sdram_cs=rom_we,sdram_we=rom_we;
wire [15:0] sdram_din=rom_word,sdram_dout;
wire [21:0] sdram_addr={4'b1111,rom_addr};wire [1:0] sdram_be=0;
wire [47:0] chip48,fastram_dout48;
wire [15:0] fastram_din=0,fastram_dout;
wire [21:0] fastram_addr=0;wire [1:0] fastram_be=0;
wire fastram_sel=0,fastram_wr=0,fastram_ready;
'''
suffix='''sdr_chip_model chip(.clk(clk85),.dq(IO_sdram_dq),.addr(O_sdram_addr),.dqm(O_sdram_dqm),
.ba(O_sdram_ba),.cs_n(O_sdram_cs_n),.ras_n(O_sdram_ras_n),.cas_n(O_sdram_cas_n),.we_n(O_sdram_wen_n));
assign probe_data=chip.mem[21'h1e0000+probe_addr];
endmodule
'''
(stage/'rom_memory_top.sv').write_text(prefix+controller+suffix)
model=(root/'upstream/sim/sdr_chip_model.v').read_text()
model=model.replace('reg [10:0] row [0:3];',"initial for(integer n=0;n<2097152;n=n+1) mem[n]=32'hccaa55cc;\nreg [10:0] row [0:3];")
(stage/'sdr_chip_model.v').write_text(model)
cpp='#define ROM_MEMORY_TEST\n'+(root/'tests/link_test.cpp').read_text().split('int main(',1)[0]
cpp=cpp.replace('Vlink_top','Vrom_memory_top').replace('edge=200','edge=200')
cpp=cpp.replace('q.clk=0;q.sck=0;', 'q.clk=0;q.clk85=1;q.probe_addr=0;q.sck=0;')
cpp=cpp.replace('advance(2000+phase);','advance(1200ull*10000+phase);CHECK(q.sdram_ready);')
cpp=cpp.replace('if(!q.clk) {','if(t%600==0 && !q.clk) {')
cpp=cpp.replace('q.clk=!q.clk;q.eval();','if(t%600==0){q.clk=!q.clk;q.eval();}\n            q.clk85=!q.clk85;q.eval();')
cpp=cpp.replace('advance(400ull*16*257);','advance(1200ull*16*257);')
cpp=cpp.replace('advance(2400);','advance(7200);')
cpp+='''int main(int argc,char **argv){
CHECK(argc==2);std::ifstream f(argv[1],std::ios::binary);CHECK(f.good());
std::vector<uint8_t> rom((std::istreambuf_iterator<char>(f)),{});CHECK(rom.size()==524288);
unsigned cases=0;
for(unsigned period:{855u,3420u})for(unsigned phase:{0u,173u,599u}) {
 Sim s(period,phase);s.wr(0x44,1);s.wr(0x58,crc(rom));
 for(unsigned off=0;off<rom.size();off+=256)s.packet(off/256+1,rom.data()+off);
 s.wr(0x44,2);CHECK(s.q.rom_ready);CHECK(s.writes==262144);CHECK(s.memory==rom);
 unsigned bad=0;
 for(unsigned n=0;n<rom.size()/4;n++) {
  s.q.probe_addr=n;s.q.eval();
  uint32_t expected=(uint32_t(rom[4*n])<<24)|(uint32_t(rom[4*n+1])<<16)|(uint32_t(rom[4*n+2])<<8)|rom[4*n+3];
  if(s.q.probe_data!=expected){if(bad<12)printf("Mismatch %06X expected %08X got %08X\\n",n*4,expected,s.q.probe_data);bad++;}
 }
 printf("QSPI period %u phase %u: %u mismatching SDRAM words\\n",period,phase,bad);fflush(stdout);CHECK(bad==0);cases++;
}
printf("PASS %u complete 512 KiB transfers through QSPI writer and SDRAM controller; all 131072 physical 32-bit words match ROM\\n",cases);
}
'''
(stage/'rom_memory_test.cpp').write_text(cpp)
tool=Path(r'E:\zx\tools\rtl\ucrt64')
vc=Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207')
sdk=Path(r'C:\Program Files (x86)\Windows Kits\10');v='10.0.26100.0'
env=os.environ.copy();env['PATH']=str(tool/'bin')+';'+str(vc/'bin/Hostx64/x64')+';'+env['PATH']
env['VERILATOR_ROOT']=str(tool/'share/verilator')
env['INCLUDE']=';'.join(map(str,[vc/'include',*[sdk/'Include'/v/x for x in ['ucrt','shared','um']]]))
env['LIB']=';'.join(map(str,[vc/'lib/x64',*[sdk/'Lib'/v/x/'x64' for x in ['ucrt','um']]]))
def run(cmd,name):
 print(name,flush=True)
 with (stage/name).open('wb') as log:p=subprocess.run(list(map(str,cmd)),cwd=stage,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=300)
 if p.returncode:raise RuntimeError((stage/name).read_text(errors='replace')[-5000:])
sources=['fpga/amiga_link.sv','fpga/amiga_disk.sv','fpga/next_qspi.sv','fpga/fb_sdspi.sv','fpga/sdram.sv']
run([tool/'bin/verilator_bin.exe','--cc','--Wno-fatal','--top-module','rom_memory_top','--Mdir','obj',stage/'rom_memory_top.sv',stage/'sdr_chip_model.v',*[root/p for p in sources]],'verilator.log')
inc=tool/'share/verilator/include'
cmd=['/nologo','/EHsc','/std:c++20','/utf-8','/O2','/MD','/DVL_TIME_CONTEXT','/MP4','/I'+str(inc),'/I'+str(inc/'vltstd'),'/Iobj',stage/'rom_memory_test.cpp',*list((stage/'obj').glob('*.cpp')),inc/'verilated.cpp',inc/'verilated_threads.cpp','/Ferom_memory_test.exe']
(stage/'compile.rsp').write_text(subprocess.list2cmdline(list(map(str,cmd))))
run([vc/'bin/Hostx64/x64/cl.exe','@compile.rsp'],'compile.log')
rom=root/'ROM/kickstart-3.1-a500_a600_a2000.rom'
run([stage/'rom_memory_test.exe',rom],'run.log')
text=(stage/'run.log').read_text()
report={'passed':True,'result':text,'hardware':False,'electrical_timing':False,
'source_sha256':{p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in sources+['fpga/amiga_vdac4.sv','upstream/sim/sdr_chip_model.v','tests/link_test.cpp','tests/run_rom_sdram.py']},'rom_sha256':hashlib.sha256(rom.read_bytes()).hexdigest()}
(root/'tests/rom-sdram-result.json').write_text(json.dumps(report,indent=2)+'\n')
print(text)
