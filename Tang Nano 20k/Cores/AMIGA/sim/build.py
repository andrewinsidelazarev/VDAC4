"""Build a source-identified simulation without editing the FPGA project."""
from pathlib import Path
import argparse, hashlib, json, os, re, shutil, subprocess, sys

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent
RTL_TOOLS = Path(r'E:\zx\tools\rtl\ucrt64')
GOWIN = Path(r'C:\Gowin\Gowin_V1.9.11.03_Education_x64\IDE')
VC = Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207')
SDK = Path(r'C:\Program Files (x86)\Windows Kits\10')
SDK_VERSION = '10.0.26100.0'
GHDL = HERE/'.tools/ghdl-6.0.0/bin/ghdl.exe'
VFLAGS=['--cc','--timing','--timescale','1ps/1ps','--Wno-fatal','-Wno-BLKANDNBLK','--trace','--top-module','sim_top','--Mdir','obj','-DINFER_DPRAM']

def cpp_flags():
    inc=RTL_TOOLS/'share/verilator/include'
    return ['/nologo','/EHsc','/std:c++20','/utf-8','/O2','/MD','/DVL_TIME_CONTEXT','/MP4','/I'+str(inc),'/I'+str(inc/'vltstd'),'/Iobj']

def model_signature(stage,names):
    replacements={'fpga/cpu_wrapper.v':'cpu_wrapper_sim.v','fpga/sdram.sv':'sdram_sim.sv','fpga/amiga_vdac4.sv':'amiga_top_sim.sv'}
    paths=['sim_top.sv','sdram_chip.sv','cpu_generated.v','gowin_models.v']+[replacements.get(n,n) for n in names if not n.endswith('.vhd')]
    paths+=sorted(p.name for p in stage.glob('*') if p.suffix in {'.hex','.mem'})
    inc=RTL_TOOLS/'share/verilator/include'
    return {'hdl':{n:sha(stage/n) for n in paths},'verilator_flags':VFLAGS,'cpp_flags':cpp_flags(),'sdk':SDK_VERSION,
            'verilator':sha(RTL_TOOLS/'bin/verilator_bin.exe'),'cl':sha(VC/'bin/Hostx64/x64/cl.exe'),
            'runtime':{n:sha(inc/n) for n in ['verilated.cpp','verilated.h','verilated_timing.cpp','verilated_timing.h','verilated_vcd_c.cpp','verilated_vcd_c.h','verilated_threads.cpp','verilated_threads.h']}}

def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()

def environment():
    e = os.environ.copy()
    e['PATH'] = str(RTL_TOOLS/'bin')+';'+str(VC/'bin/Hostx64/x64')+';'+e['PATH']
    e['VERILATOR_ROOT'] = str(RTL_TOOLS/'share/verilator')
    e['INCLUDE'] = ';'.join(map(str, [VC/'include', *[SDK/'Include'/SDK_VERSION/n for n in ['ucrt','shared','um']]]))
    e['LIB'] = ';'.join(map(str, [VC/'lib/x64', *[SDK/'Lib'/SDK_VERSION/n/'x64' for n in ['ucrt','um']]]))
    return e

def run(cmd, stage, log, timeout=900):
    print(log, flush=True)
    with (stage/log).open('wb') as f:
        p = subprocess.run(list(map(str, cmd)), cwd=stage, env=environment(), stdout=f,
                           stderr=subprocess.STDOUT, timeout=timeout)
    if p.returncode:
        raise RuntimeError((stage/log).read_text(errors='replace')[-7000:])

def vendor_models(stage):
    vendor = GOWIN/'simlib/gw2a/prim_sim.v'
    text = vendor.read_text(errors='strict')
    # Preserve the vendor's nanosecond time unit. Pad models use picoseconds.
    modules = '`timescale 1ns/1ps\n'
    for name in ['rPLL','CLKDIV','OSER10','GSR','ELVDS_OBUF']:
        m = re.search(r'module\s+'+name+r'\b', text)
        assert m, name
        end = text.index('endmodule', m.start())+len('endmodule')
        modules += text[m.start():end]+'\n'
    original='assign CLKOUTP = (CLKOUTP_BYPASS == "true")? CLKIN : clk_ps_reg;'
    assert modules.count(original)==1
    # Verilator 5.050 mis-schedules the vendor's transport NBA when the
    # phase delay exceeds half the period. For this static 50%-duty mode,
    # 270 degrees is exactly an inverted clock delayed by a quarter period.
    # Icarus retains the original vendor implementation. check_memory.py
    # compares phase timestamps and data across both backends.
    lowered='''`ifdef VERILATOR
wire phase_quarter;
assign #(clkout_period/4) phase_quarter=clk_out;
assign CLKOUTP = (CLKOUTP_BYPASS == "true")? CLKIN :
 (DYN_DA_EN=="false" && PSDA_SEL=="1100" && DUTYDA_SEL=="1000") ? ~phase_quarter : clk_ps_reg;
`else
'''+original+'\n`endif'
    modules=modules.replace(original,lowered)
    (stage/'gowin_models.v').write_text(modules)
    return {'path':str(vendor), 'sha256':sha(vendor), 'modules':['rPLL','CLKDIV','OSER10','GSR','ELVDS_OBUF'],
            'adapter':'Verilator static 270-degree/50%-duty phase represented as inverted quarter-period delay; original vendor model in Icarus',
            'generated_sha256':sha(stage/'gowin_models.v')}

def cpu_frontend(source, stage):
    if not GHDL.is_file():
        raise RuntimeError('GHDL missing; run sim/setup.py first')
    names = ['TG68K_Pack.vhd','TG68K_ALU.vhd','TG68KdotC_Kernel.vhd']
    for n in names:
        shutil.copy2(source/'upstream/src/tg68k'/n, stage/n)
    # 8.11.2026: параметры TG68K — из cpu_wrapper.v, как в сборке ПЛИС (с 0.12.12: 68020 зашит, без barrel shifter)
    wtext = (source/'fpga/cpu_wrapper.v').read_text()
    wa = wtext.index('TG68KdotC_Kernel\n'); wb = wtext.index('cpu_inst_p', wa)
    generics = {k: int(v) for k, v in re.findall(r'\.(\w+)\((\d+)\)', wtext[wa:wb])}
    order = ['SR_Read','VBR_Stackframe','extAddr_Mode','MUL_Mode','DIV_Mode','BitField','BarrelShifter','MUL_Hardware']
    cmd = [GHDL,'--synth','--std=08','-fsynopsys','--latches','--out=verilog',
           *[f'-g{k}={generics[k]}' for k in order if k in generics],*names,'-e','TG68KdotC_Kernel']
    print('ghdl.log', flush=True)
    with (stage/'cpu_generated.v').open('wb') as out, (stage/'ghdl.log').open('wb') as log:
        p = subprocess.run(list(map(str,cmd)), cwd=stage, stdout=out, stderr=log, timeout=180)
    if p.returncode:
        raise RuntimeError((stage/'ghdl.log').read_text(errors='replace'))
    text = (stage/'cpu_generated.v').read_text()
    assert len(re.findall(r'module TG68KdotC_Kernel\s',text)) == 1
    (stage/'cpu_generated.v').write_text(text)
    # Use the physical CPU wrapper branch, including berr and skipFetch.
    wrapper = (source/'fpga/cpu_wrapper.v').read_text()
    a = wrapper.index('TG68KdotC_Kernel\n')
    b = wrapper.index('cpu_inst_p', a)
    generic = wrapper[a:b]
    assert 'MUL_Hardware(1)' in generic
    wrapper = wrapper[:a]+'TG68KdotC_Kernel\n'+wrapper[b:]
    wrapper = '`undef VERILATOR\n'+wrapper+'\n`define VERILATOR 1\n'
    (stage/'cpu_wrapper_sim.v').write_text(wrapper)
    version = subprocess.check_output([str(GHDL),'--version'], text=True).splitlines()[0]
    return {'tool':version, 'generated_sha256':sha(stage/'cpu_generated.v'),
            'vhdl_sha256':{n:sha(source/'upstream/src/tg68k'/n) for n in names},
            'generics':generics,
            'adjustments':['Physical CPU wrapper branch; elaborated generics removed']}

def generate_wrapper(source, stage, timed):
    top = (source/'fpga/amiga_vdac4.sv').read_text()
    modern = 'amiga_video video(' in top
    # Optional interactive acceleration bypasses only the ROM-ready flag.
    # The strict mode leaves this input low and exercises the real QSPI loader.
    assert top.count('.rom_ready(rom_done)')==1
    staged_top,count=re.subn(r'module amiga_vdac4\s*\(', 'module amiga_vdac4 (input sim_rom_ready,',top)
    assert count==1
    staged_top=staged_top.replace('.rom_ready(rom_done)','.rom_ready(sim_link_rom_done)',1)
    staged_top=staged_top.replace('wire rom_done,','wire sim_link_rom_done;\nassign rom_done=sim_link_rom_done | sim_rom_ready;\nwire rom_done,',1)
    assert 'assign rom_done=' in staged_top
    (stage/'amiga_top_sim.sv').write_text(staged_top)
    controller = (source/'fpga/sdram.sv').read_text()
    marker = 'if (WIDTH32) begin\n    reg [31:0] dq_q;'
    assert controller.count(marker) == 1
    (stage/'sdram_sim.sv').write_text(controller.replace(marker, 'if (WIDTH32) begin:sim_io\n    reg [31:0] dq_q;'))
    model = (PROJECT/'tests/sdram_burst_model.v').read_text()
    model = model.replace('reg [10:0] row [0:3];', "initial for(integer n=0;n<2097152;n=n+1) mem[n]=32'hccaa55cc ^ (n*32'h9e3779b9);\nreg [10:0] row [0:3];")
    model, count = re.subn(r'inout\s+\[31:0\]\s+dq\s*,', 'input [31:0] dq_in,output [31:0] dq_out,output dq_oe,input write_enable,', model)
    assert count == 1
    model = model.replace('dq[', 'dq_in[').replace("assign dq = v1 ? d1 : 32'hzzzzzzzz;",
                      'assign #6000 dq_out=d1;\nassign #6000 dq_oe=v1;' if timed else 'assign dq_out=d1;\nassign dq_oe=v1;')
    model = '`timescale 1ps/1ps\n'+model
    model=re.sub(r'wire \[2:0\]\s+bmask = blen\[2:0\] - 3\'d1;', '', model)
    model=re.sub(r'reg \[3:0\]\s+blen;', '', model)
    model=model.replace('reg [3:0] wleft=0;', "reg [3:0] blen;wire [2:0] bmask=blen[2:0]-3'd1;\nreg [3:0] wleft=0;")
    model=model.replace('reg [10:0] row [0:3];','''string rom_path;
initial begin
 if($value$plusargs("ROMHEX=%s",rom_path))$readmemh(rom_path,mem,21'h1e0000,21'h1fffff);
end
reg [10:0] row [0:3];''')
    (stage/'sdram_chip.sv').write_text(model)
    delay = lambda ps: '#'+str(ps)+' ' if timed else ''
    text = '''`timescale 1ps/1ps
module sim_top(input osc,gsr_n,sim_rom_ready,input [1:0] buttons,
 input link_sck,link_cs,link_drive,input [3:0] link_data,
 output [3:0] link_reply,output link_oe,
 output clk28,clk85,pclk,fclk,output sys_lock,video_lock,
 output native_hs,native_vs,output [23:0] native_rgb,
 output [2:0] serial_data,output serial_clock,
 output [29:0] parallel_symbols,
 output [31:0] cpu_address,cpu_status,output [15:0] cpu_data_read,cpu_data_write,
 output reg [63:0] cpu_steps=0,output reg [31:0] completed_address=0,
 output reg [15:0] completed_read=0,completed_write=0,output reg [1:0] completed_state=0,
 output [1:0] cpu_state,output cpu_enable,reset_n,
 output sdram_ready,rom_ready,output [31:0] video_diag,
 input [20:0] probe_address,output [31:0] probe_data,
 output reg [31:0] audio_sample=0,output reg audio_toggle=0);
 GSR GSR(.GSRI(gsr_n));
 wire [3:0] link_bus;assign link_bus=link_drive ? link_data:4'bz;
 assign link_reply=link_bus;assign link_oe=dut.link_oe;
 wire [31:0] dq;wire sd_clock,cke,cs,we,ras,cas;
 wire [10:0] addr;wire [1:0] ba;wire [3:0] dqm;
 amiga_vdac4 dut(.sim_rom_ready(sim_rom_ready),.clk_27m(osc),.btn(buttons),.link_sck(link_sck),.link_cs_n(link_cs),.link_io(link_bus),
 .link_irq_n(),.cp_sck(1'b0),.cp_cs_n(1'b1),.cp_mosi(1'b0),.cp_miso(),.cp_irq_n(),
 .mux_sel(),.pa_en(),.ft_int_n(),.ft_miso(),.mux_d(8'd0),.hsync_in(1'b1),.vsync_in(1'b1),
 .pclk_in(1'b0),.m_sel(1'b0),.ft_sck(1'b0),.ft_cs_n(1'b1),.ft_mosi(1'b0),
 .O_sdram_clk(sd_clock),.O_sdram_cke(cke),.O_sdram_cs_n(cs),.O_sdram_cas_n(cas),.O_sdram_ras_n(ras),.O_sdram_wen_n(we),
 .O_sdram_addr(addr),.O_sdram_ba(ba),.O_sdram_dqm(dqm),.IO_sdram_dq(dq),
 .SD_SCK(),.SD_MOSI(),.SD_MISO(1'b1),.SD_CS(),.O_tmds_clk_p(serial_clock),.O_tmds_clk_n(),
 .O_tmds_data_p(serial_data),.O_tmds_data_n());
 assign clk28=dut.clk_28m,clk85=dut.clk_85m,pclk=dut.clk_pixel,fclk=dut.clk_pixel_x5;
 assign sys_lock=dut.pll_lock;
 assign native_hs=dut.hs_n,native_vs=dut.vs_n,native_rgb={dut.red,dut.green,dut.blue};
 assign sdram_ready=dut.sdram_ready,rom_ready=dut.rom_done;
 assign cpu_address=dut.nanomig.debug_cpu_address;
 assign cpu_status=dut.nanomig.debug_cpu_status;
 assign cpu_data_read=dut.nanomig.debug_cpu_read,cpu_data_write=dut.nanomig.debug_cpu_write;
 assign cpu_state=dut.nanomig.cpu_state,cpu_enable=dut.nanomig.cpu_clkena,reset_n=dut.nanomig.cpu_rst;
 always @(posedge clk28)begin
  if(reset_n && cpu_enable && cpu_state!=1)begin
   cpu_steps<=cpu_steps+1;completed_address<=cpu_address;completed_read<=cpu_data_read;
   completed_write<=cpu_data_write;completed_state<=cpu_state;
  end
  if(dut.link.audio_phase>=28500000-48000)begin
   audio_toggle<=~audio_toggle;
   audio_sample<=rom_ready && !dut.soft_reset ? {dut.audio_right[14],dut.audio_right,dut.audio_left[14],dut.audio_left}:32'd0;
  end
 end
'''
    if modern:
        text += ' assign video_lock=dut.video_locked;\n assign video_diag=dut.video_diagnostic;\n assign parallel_symbols={dut.video.t2,dut.video.t1,dut.video.t0};\n'
    else:
        # hdmi uses an array called tmds_internal at the serializer boundary.
        text += ' assign video_lock=dut.amigaclks.video_locked;\n assign video_diag=0;\n assign parallel_symbols={dut.hdmi.tmds_internal[2],dut.hdmi.tmds_internal[1],dut.hdmi.tmds_internal[0]};\n'
    text += f'''
 wire chip_clock;wire [31:0] write_pin,read_pin,read_core;
 wire write_enable,read_enable,core_read_enable;
 wire [10:0] addr_pin;wire [1:0] ba_pin;wire [3:0] dqm_pin;wire cs_pin,we_pin,ras_pin,cas_pin;
 assign {delay(4971)}chip_clock=sd_clock;
 assign {delay(5043)}write_pin=dut.sdram.sim_io.dq_out;
 assign {delay(5043)}write_enable=dut.sdram.sd_dq;
 assign {delay(1853)}read_core=read_pin;
 assign {delay(1853)}core_read_enable=read_enable;
 assign dq=!dut.sdram.sd_dq && core_read_enable ? read_core:32'bz;
 assign {delay(5043)}{{addr_pin,ba_pin,dqm_pin,cs_pin,we_pin,ras_pin,cas_pin}}={{addr,ba,dqm,cs,we,ras,cas}};
 sdr_chip_model chip(.clk(chip_clock),.dq_in(write_pin),.dq_out(read_pin),.dq_oe(read_enable),.write_enable(write_enable),
 .addr(addr_pin),.ba(ba_pin),.dqm(dqm_pin),.cs_n(cs_pin),.we_n(we_pin),.ras_n(ras_pin),.cas_n(cas_pin));
 assign probe_data=chip.mem[probe_address];
endmodule
'''
    (stage/'sim_top.sv').write_text(text)
    return modern

def build(profile='stable', timed=True, rebuild=False):
    from check_config import check as check_config
    constraints=check_config(profile)
    source = HERE/'reference/0.11' if profile == 'stable' else PROJECT
    if profile == 'stable':
        for n,h in json.loads((source/'sources.json').read_text()).items():
            if sha(source/n) != h:
                raise RuntimeError('Stable reference changed: '+n)
    names = re.findall(r'^add_file (.+)$', (source/'build.tcl').read_text(), re.M)
    names = [n for n in names if Path(n).suffix in {'.v','.sv','.vhd'}]
    inputs = {n:sha(source/n) for n in names}
    for n in ['build.tcl']:
        inputs[n] = sha(source/n)
    for n in ['build.py','engine.cpp']:
        inputs['sim/'+n] = sha(HERE/n)
    inputs['tests/sdram_burst_model.v'] = sha(PROJECT/'tests/sdram_burst_model.v')
    inputs['fpga/amigaVdac4.cst']=constraints['sha256']
    for n,p in {'verilator':RTL_TOOLS/'bin/verilator_bin.exe','GHDL':GHDL,'Gowin':GOWIN/'simlib/gw2a/prim_sim.v','MSVC':VC/'bin/Hostx64/x64/cl.exe'}.items():inputs['tool/'+n]=sha(p)
    key = hashlib.sha256(json.dumps({'files':inputs,'timed':timed},sort_keys=True).encode()).hexdigest()[:16]
    stage = Path(r'E:\zx\cache\tmp\amiga-simulator')/(profile+'-'+key)
    stage.mkdir(parents=True, exist_ok=True)
    if (stage/'amiga_sim.exe').exists() and not rebuild:
        return stage
    for n in names:
        p = stage/n;p.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source/n,p)
    for p in (source/'fpga').glob('*'):
        if p.suffix in {'.hex','.mem'}:shutil.copy2(p,stage/p.name)
    vendor = vendor_models(stage)
    cpu = cpu_frontend(source, stage)
    modern = generate_wrapper(source, stage, timed)
    sources = []
    for n in names:
        if n.endswith('.vhd'):
            continue
        if n == 'fpga/cpu_wrapper.v':sources.append(stage/'cpu_wrapper_sim.v')
        elif n == 'fpga/sdram.sv':sources.append(stage/'sdram_sim.sv')
        elif n == 'fpga/amiga_vdac4.sv':sources.append(stage/'amiga_top_sim.sv')
        else:sources.append(stage/n)
    signature=model_signature(stage,names)
    reuse=None
    if not rebuild:
        for old in stage.parent.glob(profile+'-*/build.json'):
            if old.parent!=stage and json.loads(old.read_text()).get('model_signature')==signature:
                reuse=old.parent;break
    if reuse:
        shutil.copytree(reuse/'obj',stage/'obj',dirs_exist_ok=True)
        for obj in reuse.glob('*.obj'):
            if obj.name!='engine.obj':shutil.copy2(obj,stage/obj.name)
        args=cpp_flags()+['/c',str(HERE/'engine.cpp'),'/Foengine.obj']
        (stage/'engine.rsp').write_text(subprocess.list2cmdline(args),encoding='utf-8-sig')
        run([VC/'bin/Hostx64/x64/cl.exe','@engine.rsp'],stage,'engine-compile.log')
        run([VC/'bin/Hostx64/x64/link.exe','/nologo','/OUT:amiga_sim.exe',*stage.glob('*.obj')],stage,'link.log')
    else:
        cmd=[RTL_TOOLS/'bin/verilator_bin.exe',*VFLAGS,stage/'sim_top.sv',stage/'sdram_chip.sv',stage/'cpu_generated.v',stage/'gowin_models.v',*sources]
        run(cmd,stage,'verilator.log')
    inc = RTL_TOOLS/'share/verilator/include'
    compile = [*cpp_flags(),HERE/'engine.cpp',
               *list((stage/'obj').glob('*.cpp')),inc/'verilated.cpp',inc/'verilated_threads.cpp',
               inc/'verilated_timing.cpp',inc/'verilated_vcd_c.cpp','/Feamiga_sim.exe']
    (stage/'compile.rsp').write_text(subprocess.list2cmdline(list(map(str,compile))),encoding='utf-8-sig')
    if not reuse:run([VC/'bin/Hostx64/x64/cl.exe','@compile.rsp'],stage,'compile.log')
    report = {'profile':profile,'source':str(source),'source_sha256':inputs,'vendor':vendor,'cpu':cpu,
              'timed_io':timed,'modern_video':modern,'stage':str(stage),'constraints':constraints,
              'model_signature':signature,'reused_model':str(reuse) if reuse else None,
              'limits':['RTL simulation, not routed FPGA timing or analog HDMI/SDRAM signal integrity',
                        'ESP32/BL616 role is a host protocol model; their firmware is not executed',
                        'Tang SD filesystem/SPI and Evo physical transport are not emulated']}
    (stage/'build.json').write_text(json.dumps(report,indent=2)+'\n')
    return stage

if __name__ == '__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--profile',choices=['stable','current'],default='stable')
    ap.add_argument('--ideal-io',action='store_true');ap.add_argument('--rebuild',action='store_true');a=ap.parse_args()
    print(build(a.profile,not a.ideal_io,a.rebuild))
