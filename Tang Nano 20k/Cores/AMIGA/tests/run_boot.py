"""Headless logical boot test: actual Amiga glue + SDRAM controller + upstream chip model.

ROM is preloaded into modeled SDRAM. This does not test LINK download, PLLs,
electrical SDRAM timing, or the synthesized VHDL CPU: it uses NanoMig's checked-in
GHDL Verilog translation for Verilator, as its upstream simulation does.
"""
from pathlib import Path
import argparse,hashlib,json,os,re,struct,subprocess,time,zlib
ap=argparse.ArgumentParser();ap.add_argument('--seconds',type=float,default=.12)
ap.add_argument('--clock-order',choices=['28-first','85-first','together'],default='together')
ap.add_argument('--adf',type=Path)
ap.add_argument('--no-build',action='store_true');args=ap.parse_args()
root=Path(__file__).resolve().parents[1]
stage=root/'tests/boot-build';stage.mkdir(exist_ok=True)
top=(root/'fpga/amiga_vdac4.sv').read_text(encoding='utf-8')
defines='\n'.join(re.findall(r'^`define .+$',top,re.M))+'\n`define INFER_DPRAM\n'
config=top.split('wire [1:0] osd_cpu=',1)[1].split('reg [1:0] button_reset_sync=',1)[0]
config='wire [1:0] osd_cpu='+config
config=config.replace('wire hs_n, vs_n; wire [7:0] red, green, blue;','')
reset=re.search(r'reg \[1:0\] button_reset_sync=0;.*?wire nanomig_reset=.*?;',top,re.S)[0]
memory=top.split('// Minimig ram/rom interface',1)[1].split('wire [15:0] silent_audio',1)[0]
memory=memory.replace('assign O_sdram_clk = clk_85m_shifted;','')
wrapper=defines+'''module boot_top(input clk_28m,clk_85m,por,rom_done,input [8:0] trace_index,
output hs_n,vs_n,output [7:0] red,green,blue,
output [31:0] cpu_debug [0:23],output [31:0] cpu_trace [0:3],output sdram_ready,output reset_out,
output timing_publish,timing_latch,output [15:0] timing_data,timing_latched,output [21:0] timing_address,
input host_wr,input [17:0] host_addr,input [31:0] host_data,
output [31:0] disk_status,disk_sector,disk_token);
wire [31:0] trace_start=0;
wire rst_sdram_n=~por;
wire rst_28m=por || !sdram_ready;
wire [1:0] btn=0;wire soft_reset=0;
wire clk7_en,clk7n_en,rom_write=0;
wire [17:0] rom_address=0;wire [15:0] rom_value=0;
wire [14:0] audio_left,audio_right;
wire [5:0] leds;wire fdd_step_n;
wire [31:0] IO_sdram_dq;
wire [10:0] O_sdram_addr;
wire [3:0] O_sdram_dqm;wire [1:0] O_sdram_ba;
wire O_sdram_cs_n,O_sdram_wen_n,O_sdram_ras_n,O_sdram_cas_n,O_sdram_cke;
'''+config+reset+'''
assign kbd_reset=0;
assign mouse_buttons=0,kbd_mouse_level=0,kbd_mouse_type=0,kbd_mouse_data=0,joystick0=0,joystick1=0;
assign reset_out=nanomig_reset;
amiga_disk disk(.clk(clk_28m),.rst_n(~por),.guest_reset(nanomig_reset),
.wr_stb(host_wr),.wr_addr(host_addr),.wr_data(host_data),.rd_addr(13'h61),.rd_data(disk_status),
.request_rd(sd_rd),.request_wr(sd_wr),.request_sector(sd_sector),
.image_mounted(sd_img_mounted),.image_size(sd_img_size),.busy(sd_busy),.done(sd_done),
.byte_strobe(sd_rd_byte_strobe),.byte_addr(sd_byte_index),.byte_data(sd_rd_data));
assign disk_sector=disk.sector,disk_token=disk.token;
'''+memory+'''
sdr_chip_model chip(.clk(clk_85m),.dq(IO_sdram_dq),.addr(O_sdram_addr),.dqm(O_sdram_dqm),
.ba(O_sdram_ba),.cs_n(O_sdram_cs_n),.ras_n(O_sdram_ras_n),.cas_n(O_sdram_cas_n),.we_n(O_sdram_wen_n));
assign timing_publish=sdram.state==sdram.STATE_READ_DATA && sdram.sdram_port==sdram.PORT_1;
assign timing_latch=!nanomig.c1 && nanomig.c3 && nanomig.minimig.CPU1.enable;
assign timing_data=sdram_dout;
assign timing_latched=nanomig.minimig.CPU1.ldata_in;
assign timing_address=sdram_addr;
endmodule
'''
(stage/'boot_top.sv').write_text(wrapper)
model=(root/'upstream/sim/sdr_chip_model.v').read_text(encoding='utf-8')
model=model.replace('reg [10:0] row [0:3];', '''initial begin
  for (integer n=0;n<2097152;n=n+1) mem[n]=32'hcafe1234 ^ (n * 32'h9e3779b9);
  $readmemh("kick32.hex",mem);
end
reg [10:0] row [0:3];''')
(stage/'sdr_chip_model.v').write_text(model)
# Upstream's VERILATOR branch omits these ports, although the checked-in
# translation exports them. Match the synthesized VHDL branch for bus behavior.
cpu=(root/'fpga/cpu_wrapper.v').read_text(encoding='utf-8')
assert cpu.count('.ipl_autovector(1),')==1
cpu=cpu.replace('.ipl_autovector(1),',".ipl_autovector(1'b1),\n  .berr(1'b0),\n  .skipFetch(skip_fetch_i),")
(stage/'cpu_wrapper_sim.v').write_text(cpu)
rom=(root/'ROM/kickstart-3.1-a500_a600_a2000.rom').read_bytes()
assert len(rom)==524288
(stage/'kick32.hex').write_text('@1e0000\n'+'\n'.join(rom[n:n+4].hex() for n in range(0,len(rom),4))+'\n')
sources=[]
for name in re.findall(r'^add_file (.+)$',(root/'build.tcl').read_text(),re.M):
    if name.startswith('upstream/src/minimig-aga/') or name in (
        'fpga/nanomig.v','fpga/cpu_wrapper.v','fpga/amiga_debug.sv','fpga/amiga_disk.sv','fpga/Amber.v',
        'upstream/src/misc/amiga_keymap.v','fpga/sdram.sv'):
        sources.append(name)
sources+=['upstream/sim/TG68KdotC_Kernel.v']
hashes={p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in sources+[
    'fpga/amiga_vdac4.sv','fpga/amigaclks.v','upstream/sim/sdr_chip_model.v','tests/run_boot.py','tests/boot_test.cpp']}
tool=Path(r'E:\zx\tools\rtl\ucrt64')
vc=Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207')
sdk=Path(r'C:\Program Files (x86)\Windows Kits\10');v='10.0.26100.0'
env=os.environ.copy()
env['PATH']=str(tool/'bin')+';'+str(vc/'bin/Hostx64/x64')+';'+env['PATH']
env['VERILATOR_ROOT']=str(tool/'share/verilator')
env['INCLUDE']=';'.join(map(str,[vc/'include',*[sdk/'Include'/v/x for x in ['ucrt','shared','um']]]))
env['LIB']=';'.join(map(str,[vc/'lib/x64',*[sdk/'Lib'/v/x/'x64' for x in ['ucrt','um']]]))
def run(cmd,name,timeout=900):
    print(name,flush=True)
    with (stage/name).open('wb') as log:
        p=subprocess.run(list(map(str,cmd)),cwd=stage,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=timeout)
    if p.returncode:raise RuntimeError((stage/name).read_text(encoding='utf-8',errors='replace')[-6000:])
if not args.no_build:
    run([tool/'bin/verilator_bin.exe','--cc','--Wno-fatal','-Wno-BLKANDNBLK','--no-timing',
        '--top-module','boot_top','--Mdir','obj',stage/'boot_top.sv',stage/'sdr_chip_model.v',
        *[stage/'cpu_wrapper_sim.v' if p=='fpga/cpu_wrapper.v' else root/p for p in sources]],'verilator.log')
    inc=tool/'share/verilator/include'
    command=['/nologo','/EHsc','/std:c++20','/utf-8','/O2','/MD','/DVL_TIME_CONTEXT','/MP4',
        '/I'+str(inc),'/I'+str(inc/'vltstd'),'/Iobj',str(root/'tests/boot_test.cpp'),
        *map(str,(stage/'obj').glob('*.cpp')),str(inc/'verilated.cpp'),str(inc/'verilated_threads.cpp'),'/Feboot_test.exe']
    (stage/'compile.rsp').write_text(subprocess.list2cmdline(command),encoding='utf-8')
    run([vc/'bin/Hostx64/x64/cl.exe','@compile.rsp'],'compile.log')
    (stage/'sources.json').write_text(json.dumps(hashes,indent=2)+'\n')
else:
    assert json.loads((stage/'sources.json').read_text())==hashes,'Sources changed; rebuild required'
run([stage/'boot_test.exe',str(args.seconds),str(['28-first','85-first','together'].index(args.clock_order))]+([str(args.adf.resolve())] if args.adf else []),'run.log',3600)
report=json.loads((stage/'boot.json').read_text())
report.update(source_sha256=hashes,rom_sha256=hashlib.sha256(rom).hexdigest(),hardware=False,clock_order=args.clock_order,
    simulator_adjustments=['Translated CPU skipFetch and berr wired as in synthesized VHDL branch'],
    limitations=['SDRAM preloaded; LINK ROM download excluded','Upstream Verilog CPU translation','No electrical timing or PLL model'])
if args.adf:report['adf_sha256']=hashlib.sha256(args.adf.read_bytes()).hexdigest()
(root/'tests/boot-result.json').write_text(json.dumps(report,indent=2)+'\n')
if (stage/'frame.ppm').exists():
    with (stage/'frame.ppm').open('rb') as f:
        assert f.readline()==b'P6\n'
        width,height=map(int,f.readline().split());assert f.readline()==b'255\n';rgb=f.read()
    def chunk(t,b):return struct.pack('>I',len(b))+t+b+struct.pack('>I',zlib.crc32(t+b)&0xffffffff)
    pixels=b''.join(b'\0'+rgb[y*width*3:(y+1)*width*3] for y in range(height))
    png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(pixels))+chunk(b'IEND',b'')
    (stage/'frame.png').write_bytes(png)
print(json.dumps({k:v for k,v in report.items() if k!='source_sha256'},indent=2),flush=True)
if not all(report[k] for k in ['reset_released','cpu_reset_released','sdram_ready','reset_vectors_match']):
    raise SystemExit('FAIL reset release or ROM reset vectors; artifacts retained')
