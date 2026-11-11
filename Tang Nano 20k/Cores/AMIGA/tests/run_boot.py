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
ap.add_argument('--hdf',type=Path)
ap.add_argument('--hdf1',type=Path,help='второй диск IDE (unit 5), только чтение; нужен и --hdf')
ap.add_argument('--rom',type=Path,help='свой образ ROM 512 КБ вместо Kickstart (run_ide_write.py)')
ap.add_argument('--video',action='store_true',help='Run framebuffer DMA beside the actual CPU/chipset')
ap.add_argument('--compat',action='store_true',help='режим совместимости (F11 -> SETUP): 68000, без fast RAM и турбо, как compat_req=1 у AMIGA.ELF')
ap.add_argument('--slot-stats',action='store_true',help='с --video: кто занимает слоты SDRAM в момент пропуска захвата HDMI')
ap.add_argument('--stage',help='своя папка сборки в tests/ (два прогона одновременно)')
ap.add_argument('--hdmi-dump',help='с --video: «С,N» — кадры вывода 576p/480p (то, что уходит в TMDS) в hdmi_NNNNN.ppm с секунды С, каждый N-й')
ap.add_argument('--threads',type=int,default=0,help='Verilator --threads N (быстрее на многоядерном ПК)')
ap.add_argument('--lace-field',action='store_true',help='с --video: бит 2 регистра картинки — interlace, показ последнего поля (0.13.9)')
ap.add_argument('--no-build',action='store_true');args=ap.parse_args()
root=Path(__file__).resolve().parents[1]
stage=root/('tests/'+args.stage if args.stage else 'tests/boot-video-build' if args.video else 'tests/boot-build');stage.mkdir(exist_ok=True)
top=(root/'fpga/amiga_vdac4.sv').read_text(encoding='utf-8')
defines='\n'.join(re.findall(r'^`define .+$',top,re.M))+'\n`define INFER_DPRAM\n`define AMIGA_DEBUG\n'
config=top.split('wire [1:0] osd_cpu=',1)[1].split('reg [1:0] button_reset_sync=',1)[0]
config='wire [1:0] osd_cpu='+config
config=config.replace('wire hs_n, vs_n; wire [7:0] red, green, blue;','')
if args.compat:
    # 11.11.2026: в ядре режим приходит по LINK (0x6B) и защёлкивается в сбросе; у стенда LINK нет — константа
    assert config.count('wire compat_req;')==1
    config=config.replace('wire compat_req;',"wire compat_req=1'b1;")
reset=re.search(r'reg \[1:0\] button_reset_sync=0;.*?wire nanomig_reset=.*?;',top,re.S)[0]
memory=top.split('// Minimig ram/rom interface',1)[1].split('// 720p renderer',1)[0]
memory=memory.replace('assign O_sdram_clk = clk_85m_shifted;','')
if not args.video:
    memory=memory.replace('.video_cs(video_cs),.video_we(video_we),.video_addr(video_address)',".video_cs(1'b0),.video_we(1'b0),.video_addr(21'd0)").replace('.video_din(video_din)',".video_din(128'd0)")
video='''
amiga_video video(.clk28(clk_28m),.clk85(clk_85m),.hclk(hclk),.hclk5(hclk),
 .reset28(rst_28m),.reset85(por||!sdram_ready),.hreset(por),
 .hs(hs_n),.vs(vs_n),.lace(video_lace),.field(video_field),.window_in(video_window),
 .rgb_in({red,green,blue}),.picture(PICTURE),
 .osd_wr(1'b0),.osd_addr(9'd0),.osd_data(32'd0),.osd_show(1'b0),.osd_sel(6'd63),
 .osd_band(30'd0),.osd_rule(30'd0),
 .ram_cs(video_cs),.ram_we(video_we),.ram_double(video_double),.ram_address(video_address),.ram_din(video_din),
 .ram_dout(video_dout),.ram_ack(video_ack),.ram_read_valid(video_read_valid),.ram_single(video_single),.ram_fetch_index(video_fetch_index),
 .diagnostic(video_diagnostic),.steal(video_steal),.raster_debug(),.tmds_clock(),.tmds());
assign video_diag=video_diagnostic;
// 11.11.2026: окно DIW кадра на захвате (координаты захвата: x — точки 14 МГц, y — логические строки) при каждой смене
reg [40:0] fi_seen=0;reg [31:0] fi_t=0;
always @(posedge clk_28m) begin
  fi_t<=fi_t+1;
  if (video.capture.frame_info[40:0]!=fi_seen) begin
    fi_seen<=video.capture.frame_info[40:0];
    $display("FRAMEINFO %0d.%03d s x %0d y %0d w %0d h %0d lace %b",fi_t/28500000,(fi_t%28500000)/28500,
      video.capture.frame_info[39:30],video.capture.frame_info[29:20],video.capture.frame_info[19:10],
      video.capture.frame_info[9:0],video.capture.frame_info[40]);
  end
end
''' if args.video else 'assign video_diag=0;\n'
# 11.11.2026 (0.13.9): --lace-field — бит 2 регистра картинки: interlace, показ последнего поля
video=video.replace('PICTURE',"4'd4" if args.lace_field else "4'd0")
# 11.11.2026: ram_double, ram_single и steal — как в amiga_vdac4.sv. Прежде стенд их не подключал: steal был 0 (CPU и
# блиттер никогда не уступали отстающему видео), сдвоенного чтения не было — пропуски захвата стенда завышены.
if args.video and args.hdmi_dump:
    # 11.11.2026: кадр вывода из регистров перед кодером TMDS (tx_rgb/tx_de) — без OSD, как видит телевизор
    dump_from,dump_step=args.hdmi_dump.split(',')
    video+=f'''
integer hd_f=0;reg hd_on=0;reg [15:0] hd_frame=0;reg [31:0] hd_t=0;
always @(posedge hclk) begin
  hd_t<=hd_t+1;
  if (video.new_frame) begin
    if (hd_on) $fclose(hd_f);
    hd_on=0;hd_frame<=hd_frame+1'b1;
    if (hd_t>=32'd{int(float(dump_from)*27000000)} && hd_frame%{int(dump_step)}==0) begin
      hd_f=$fopen($sformatf("hdmi_%05d.ppm",hd_frame),"wb");$fwrite(hd_f,"P6\\n720 %0d\\n255\\n",video.v_active);hd_on=1;
    end
  end else if (hd_on && video.tx_de) $fwrite(hd_f,"%c%c%c",video.tx_rgb[23:16],video.tx_rgb[15:8],video.tx_rgb[7:0]);
end
'''
if args.video and args.slot_stats:
    # Кто занимает слоты SDRAM (такт 7 МГц, одно обращение): чипсет (DMA Agnus / CPU на шине chip), регенерация,
    # видео, CPU порта 2 (турбо), пусто; vwait — видео хотело, но слот ушёл другому. Окно — 457 слотов (строка
    # Amiga, 64 мкс). На каждый пропуск захвата (overruns) — прошлое окно, текущее и адрес CPU.
    video+='''
reg [8:0] sl_n=0;reg [8:0] s_dma=0,s_cpu=0,s_ref=0,s_vid=0,s_p2=0,s_idle=0,s_vw=0;
reg [8:0] p_dma=0,p_cpu=0,p_ref=0,p_vid=0,p_p2=0,p_idle=0,p_vw=0;
wire sl_go=sdram.state==sdram.STATE_RAS && sdram.sync_i;
wire sl_chip=sdram.cs && !sdram.refresh, sl_dmab=nanomig.minimig.dbr;
wire sl_ref=!sl_chip && (sdram.refreshcnt==0 || (sdram.cs && sdram.refresh));
wire sl_vid=!sl_chip && !sl_ref && sdram.video_grant;
wire sl_p2=!sl_chip && !sl_ref && !sl_vid && sdram.p2_cs;
wire sl_vw=sdram.video_cs && !sl_vid;
always @(negedge clk_85m) if (sl_go) begin
  if (sl_n==9'd456) begin
    p_dma<=s_dma;p_cpu<=s_cpu;p_ref<=s_ref;p_vid<=s_vid;p_p2<=s_p2;p_idle<=s_idle;p_vw<=s_vw;sl_n<=0;
    s_dma<=sl_chip&&sl_dmab;s_cpu<=sl_chip&&!sl_dmab;s_ref<=sl_ref;s_vid<=sl_vid;s_p2<=sl_p2;
    s_idle<=!sl_chip&&!sl_ref&&!sl_vid&&!sl_p2;s_vw<=sl_vw;
  end else begin
    sl_n<=sl_n+1'b1;
    s_dma<=s_dma+(sl_chip&&sl_dmab);s_cpu<=s_cpu+(sl_chip&&!sl_dmab);s_ref<=s_ref+sl_ref;s_vid<=s_vid+sl_vid;
    s_p2<=s_p2+sl_p2;s_idle<=s_idle+(!sl_chip&&!sl_ref&&!sl_vid&&!sl_p2);s_vw<=s_vw+sl_vw;
  end
end
reg [7:0] ov_seen=0;reg [31:0] t28=0;
always @(posedge clk_28m) begin
  t28<=t28+1;ov_seen<=video.capture.overruns;
  if (video.capture.overruns!=ov_seen)
    $display("OVR %0d.%06d s y %0d | прошл: dma %0d cpu %0d ref %0d vid %0d p2 %0d idle %0d vwait %0d | тек %0d: dma %0d cpu %0d vid %0d p2 %0d vwait %0d | steal %b cpu %h",
      t28/28500000,(t28%28500000)*10/285,video.capture.cy,p_dma,p_cpu,p_ref,p_vid,p_p2,p_idle,p_vw,sl_n,s_dma,s_cpu,s_vid,s_p2,s_vw,
      video_steal,cpu_debug[2]);
end
'''
wrapper=defines+'''module boot_top(input clk_28m,clk_85m,hclk,por,rom_done,input [8:0] trace_index,
output hs_n,vs_n,output [7:0] red,green,blue,
output [31:0] video_diag,
output [31:0] cpu_debug [0:23],output [31:0] cpu_trace [0:3],output sdram_ready,output reset_out,
output timing_publish,timing_latch,output [15:0] timing_data,timing_latched,output [21:0] timing_address,
input soft_reset,input host_wr,input [17:0] host_addr,input [31:0] host_data,
output [31:0] disk_status,disk_sector,disk_token,input [6:0] tx_index,output [31:0] tx_word);
wire [31:0] trace_start=0;
wire rst_sdram_n=~por;
wire rst_28m=por || !sdram_ready;
wire [1:0] btn=0;
wire floppy_wrprot;wire [11:0] rtc=0;
wire clk7_en,clk7n_en,rom_write=0;
wire [17:0] rom_address=0;wire [15:0] rom_value=0;
wire [14:0] audio_left,audio_right;
wire [5:0] leds;wire fdd_step_n;
wire [31:0] IO_sdram_dq;
wire [10:0] O_sdram_addr;
wire [3:0] O_sdram_dqm;wire [1:0] O_sdram_ba;
wire O_sdram_cs_n,O_sdram_wen_n,O_sdram_ras_n,O_sdram_cas_n,O_sdram_cke;
'''+config+reset+'''
// Настоящий hid, как в ядре: запись в слово 0x04 (адрес 0x10 у ESP) — байт HID, бит 8 — начало команды, как amiga_link.
wire [7:0] key_free;
reg hid_strobe=0,hid_start=0;reg [7:0] hid_data=0;
always @(posedge clk_28m) begin hid_strobe<=host_wr && host_addr==18'h04; hid_start<=host_data[8]; hid_data<=host_data[7:0]; end
hid hid(.clk(clk_28m),.reset(rst_28m),.data_in_strobe(hid_strobe),.data_in_start(hid_start),.data_in(hid_data),.data_out(),
    .db9_port(6'b0),.irq(),.iack(1'b1),.mouse_buttons(mouse_buttons),.kbd_mouse_level(kbd_mouse_level),.kbd_mouse_type(kbd_mouse_type),
    .kbd_mouse_data(kbd_mouse_data),.keyboard_ack(keyboard_ack),.vsync_n(vs_n),.kbd_reset(kbd_reset),
    .joystick0(joystick0),.joystick1(joystick1),.key_free(key_free));
assign reset_out=nanomig_reset;
// Наблюдение обмена с диском: первый запрос каждого устройства.
reg [7:0] seen_rd=0;
always @(posedge clk_28m) if (|(sd_rd & ~seen_rd)) begin seen_rd<=seen_rd|sd_rd; $display("first read request drive %02x sector %0d busy %b",sd_rd,sd_sector,sd_busy); end
amiga_disk disk(.clk(clk_28m),.rst_n(~por),.guest_reset(nanomig_reset),
.wr_stb(host_wr),.wr_addr(host_addr),.wr_data(host_data),.rd_addr(13'h61),.rd_data(disk_status),
.rd_clk(clk_28m),.tx_q(),.floppy_wrprot(floppy_wrprot),.guest_data(sd_wr_data),
.request_rd(sd_rd),.request_wr(sd_wr),.request_sector(sd_sector),
.image_mounted(sd_img_mounted),.image_size(sd_img_size),.busy(sd_busy),.served_ide(sd_served_ide),.done(sd_done),
.byte_strobe(sd_rd_byte_strobe),.byte_addr(sd_byte_index),.byte_data(sd_rd_data));
assign disk_sector=disk.sector,disk_token=disk.token,tx_word=disk.tx[tx_index];
'''+memory+video+'''
sdr_chip_model chip(.clk(clk_85m),.dq(IO_sdram_dq),.addr(O_sdram_addr),.dqm(O_sdram_dqm),
.ba(O_sdram_ba),.cs_n(O_sdram_cs_n),.ras_n(O_sdram_ras_n),.cas_n(O_sdram_cas_n),.we_n(O_sdram_wen_n));
assign timing_publish=sdram.state==sdram.STATE_READ_DATA && sdram.sdram_port==sdram.PORT_1;
assign timing_latch=!nanomig.c1 && nanomig.c3 && nanomig.minimig.CPU1.enable;
assign timing_data=sdram_dout;
assign timing_latched=nanomig.minimig.CPU1.ldata_in;
assign timing_address=sdram_addr;
endmodule
'''
if args.video:
    wrapper+='''module serializer #(parameter NUM_CHANNELS=3,VIDEO_RATE=32000000)(
input clk_pixel,clk_pixel_x5,reset,input [9:0] tmds_internal[NUM_CHANNELS-1:0],output [2:0] tmds,output tmds_clock);
assign tmds=0,tmds_clock=clk_pixel;
endmodule
'''
(stage/'boot_top.sv').write_text(wrapper)
model=(root/'tests/sdram_burst_model.v').read_text(encoding='utf-8')
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
rom=(args.rom or root/'ROM/kickstart-3.1-a500_a600_a2000.rom').read_bytes()
assert len(rom)==524288
(stage/'kick32.hex').write_text('@1e0000\n'+'\n'.join(rom[n:n+4].hex() for n in range(0,len(rom),4))+'\n')
sources=[]
for name in re.findall(r'^add_file (.+)$',(root/'build.tcl').read_text(),re.M):
    if name.startswith('upstream/src/minimig-aga/') or name in (
        'fpga/minimig.v','fpga/nanomig.v','fpga/cpu_wrapper.v','fpga/amiga_debug.sv','fpga/amiga_disk.sv','fpga/Amber.v','fpga/hid.v',
        'upstream/src/misc/amiga_keymap.v','fpga/sdram.sv','fpga/agnus.v','fpga/agnus_bitplanedma.v','fpga/denise.v',
        'fpga/paula_floppy.v'):
        sources.append(name)
sources+=['upstream/sim/TG68KdotC_Kernel.v']
if args.video:
    sources+=['fpga/'+n+'.sv' for n in ['amiga_video','amiga_video_capture','amiga_video_cache','amiga_video_dma','amiga_video_geometry','hdmi_tx','tmds_encoder','nes_osd','nes_osd_font']]
hashes={p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in sources+[
    'fpga/amiga_vdac4.sv','fpga/amigaclks.v','tests/sdram_burst_model.v','tests/run_boot.py','tests/boot_test.cpp']}
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
        *(['--threads',str(args.threads)] if args.threads else []),'--top-module','boot_top','--Mdir','obj',stage/'boot_top.sv',stage/'sdr_chip_model.v',
        *[stage/'cpu_wrapper_sim.v' if p=='fpga/cpu_wrapper.v' else root/p for p in sources]],'verilator.log')
    inc=tool/'share/verilator/include'
    command=['/nologo','/EHsc','/std:c++20','/utf-8','/O2','/MD','/DVL_TIME_CONTEXT','/MP4',
        '/I'+str(inc),'/I'+str(inc/'vltstd'),'/Iobj',str(root/'tests/boot_test.cpp'),
        *map(str,(stage/'obj').glob('*.cpp')),str(inc/'verilated.cpp'),str(inc/'verilated_threads.cpp'),'/Feboot_test.exe']
    (stage/'compile.rsp').write_text(subprocess.list2cmdline(command),encoding='utf-8-sig')
    run([vc/'bin/Hostx64/x64/cl.exe','@compile.rsp'],'compile.log')
    (stage/'sources.json').write_text(json.dumps(hashes,indent=2)+'\n')
else:
    assert json.loads((stage/'sources.json').read_text())==hashes,'Sources changed; rebuild required'
run([stage/'boot_test.exe',str(args.seconds),str(['28-first','85-first','together'].index(args.clock_order))]+[str(args.adf.resolve()) if args.adf else '-']+([str(args.hdf.resolve())] if args.hdf else [])+([str(args.hdf1.resolve())] if args.hdf1 else []),'run.log',3600)
report=json.loads((stage/'boot.json').read_text())
report.update(source_sha256=hashes,rom_sha256=hashlib.sha256(rom).hexdigest(),hardware=False,clock_order=args.clock_order,video_enabled=args.video,
    simulator_adjustments=['Translated CPU skipFetch and berr wired as in synthesized VHDL branch'],
    limitations=['SDRAM preloaded; LINK ROM download excluded','Upstream Verilog CPU translation','No electrical timing or PLL model'])
if args.adf:report['adf_sha256']=hashlib.sha256(args.adf.read_bytes()).hexdigest()
(root/('tests/boot-video-result.json' if args.video else 'tests/boot-result.json')).write_text(json.dumps(report,indent=2)+'\n')
if (stage/'frame.ppm').exists():
    with (stage/'frame.ppm').open('rb') as f:
        assert f.readline()==b'P6\n'
        width,height=map(int,f.readline().split());assert f.readline()==b'255\n';rgb=f.read()
    def chunk(t,b):return struct.pack('>I',len(b))+t+b+struct.pack('>I',zlib.crc32(t+b)&0xffffffff)
    pixels=b''.join(b'\0'+rgb[y*width*3:(y+1)*width*3] for y in range(height))
    png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(pixels))+chunk(b'IEND',b'')
    (stage/'frame.png').write_bytes(png)
print(json.dumps({k:v for k,v in report.items() if k!='source_sha256'},indent=2),flush=True)
if not all(report[k] for k in ['reset_released','cpu_reset_released','sdram_ready']+([] if args.rom else ['reset_vectors_match'])):
    raise SystemExit('FAIL reset release or ROM reset vectors; artifacts retained')
