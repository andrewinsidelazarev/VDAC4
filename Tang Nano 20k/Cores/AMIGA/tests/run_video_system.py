"""Actual RGB888 capture -> SDRAM bursts -> cache -> 576p/480p geometry (0.13.0), pixel by pixel."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess,shutil,struct,zlib,re,concurrent.futures
ap=argparse.ArgumentParser();ap.add_argument("--no-build",action="store_true");ap.add_argument("--all",action="store_true");ap.add_argument("--case",help="rows,lace,picture,pal")
ap.add_argument('--io-timing',action='store_true',help='270-degree chip clock with explicit SDF pad paths and bounded SDRAM access time')
ap.add_argument('--extended',action='store_true',help='Full-width AGA overscan under CPU/chipset traffic')
ap.add_argument('--amiga-bus',action='store_true',help='traffic=2 game / 3 Workbench: DMA in odd syncs (hpos[0]=1), CPU in even syncs, as minimig')
ap.add_argument('--jobs',type=int,default=1,choices=range(1,9),help='Independent isolated simulation processes')
args=ap.parse_args()
root=Path(__file__).resolve().parents[1]
stage=root/('tests/video-timed-build' if args.io_timing else 'tests/video-system-build');stage.mkdir(exist_ok=True)
result_path=root/('tests/video-timed-result.json' if args.io_timing else 'tests/video-system-result.json')
if args.extended:result_path=root/'tests/video-overscan-result.json'
if args.amiga_bus:result_path=root/('tests/video-amiga-bus-timed-result.json' if args.io_timing else 'tests/video-amiga-bus-result.json')
result_path.write_text(json.dumps({'passed':False,'state':'running'}))
wrapper=(root/'tests/video_system_top.sv').read_text()
io_paths=None
if args.io_timing:
    evidence=json.loads((root/'diagnostics/stripe/original-sdf-margins.json').read_text())
    rows=[r for r in evidence['rows'] if r['corner']=='typ']
    write_ps=round(max(r['data_ns']-evidence['period_ns']/2 for r in rows)*1000)
    clock_ps=round(rows[0]['clock_ns']*1000-evidence['period_ns']*250)
    io_paths={'clock_ps':clock_ps,'write_ps':write_ps,'command_ps':write_ps,
              'read_input_ps':1853,'read_access_ps':6000,'sdf_sha256':evidence['sdf_sha256'],
              'scope':'Representative typ pad paths; SDRAM tAC=6 ns assumption; full write corners checked separately from SDF'}
    old='sdr_chip_model chip(.clk(sd_clk),.dq(dq),.addr(addr),.dqm(dqm),.ba(ba),.cs_n(cs),.we_n(we),.ras_n(ras),.cas_n(cas));'
    assert wrapper.count(old)==1
    bridge=f'''wire chip_clk;
wire [31:0] write_pin,read_pin,read_core;wire read_enable,read_core_enable,write_pin_enable;
wire [10:0] addr_pin;wire [3:0] dqm_pin;wire [1:0] ba_pin;
wire cs_pin,we_pin,ras_pin,cas_pin;
assign #{clock_ps} chip_clk=sd_clk;
assign #{write_ps} write_pin=memory.io32.dq_out;
assign #{write_ps} write_pin_enable=memory.sd_dq;
assign #1853 read_core=read_pin;
assign #1853 read_core_enable=read_enable;
assign dq=!memory.sd_dq && read_core_enable ? read_core:32'bz;
assign #{write_ps} {{addr_pin,dqm_pin,ba_pin,cs_pin,we_pin,ras_pin,cas_pin}}={{addr,dqm,ba,cs,we,ras,cas}};
sdr_chip_model chip(.clk(chip_clk),.dq_in(write_pin),.dq_out(read_pin),.dq_oe(read_enable),.write_enable(write_pin_enable),.addr(addr_pin),.dqm(dqm_pin),.ba(ba_pin),.cs_n(cs_pin),.we_n(we_pin),.ras_n(ras_pin),.cas_n(cas_pin));'''
    wrapper=wrapper.replace(old,bridge)
(stage/'video_system_top.sv').write_text(wrapper)
model=(root/'tests/sdram_burst_model.v').read_text()
model=model.replace('reg [10:0] row [0:3];',"initial for(integer n=0;n<2097152;n=n+1) mem[n]=32'hccaa55cc ^ (n*32'h9e3779b9);\nreg [10:0] row [0:3];")
if args.io_timing:
    model,n=re.subn(r'inout\s+\[31:0\]\s+dq\s*,','input [31:0] dq_in, output [31:0] dq_out, output dq_oe, input write_enable,',model)
    assert n==1
    model=model.replace('dq[','dq_in[').replace("assign dq = v1 ? d1 : 32'hzzzzzzzz;",'assign #6000 dq_out=d1;\nassign #6000 dq_oe=v1;')
    model=model.replace('always @(posedge clk) begin','always @(posedge clk) begin\n   if((!cs_n && command==CMD_WRITE || wleft!=0) && dqm!=4\'b1111) assert(write_enable) else $fatal(1,"DQ not driven at SDRAM WRITE");',1)
(stage/'sdr_chip_model.v').write_text(model)
controller=root/'fpga/sdram.sv'
if args.io_timing:
    controller=stage/'sdram_io_test.sv'
    text=(root/'fpga/sdram.sv').read_text()
    text=text.replace('if (WIDTH32) begin\n    reg [31:0] dq_q;', 'if (WIDTH32) begin:io32\n    reg [31:0] dq_q;',1)
    controller.write_text(text)
tool=Path(r'E:\zx\tools\rtl\ucrt64')
vc=Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207')
sdk=Path(r'C:\Program Files (x86)\Windows Kits\10');v='10.0.26100.0'
env=os.environ.copy();env['PATH']=str(tool/'bin')+';'+str(vc/'bin/Hostx64/x64')+';'+env['PATH']
env['VERILATOR_ROOT']=str(tool/'share/verilator')
env['INCLUDE']=';'.join(map(str,[vc/'include',*[sdk/'Include'/v/x for x in ['ucrt','shared','um']]]))
env['LIB']=';'.join(map(str,[vc/'lib/x64',*[sdk/'Lib'/v/x/'x64' for x in ['ucrt','um']]]))
def run(cmd,name,cwd=None):
    print(name,flush=True)
    directory=cwd or stage
    with (directory/name).open('wb') as log:
        p=subprocess.run(list(map(str,cmd)),cwd=directory,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=900)
    if p.returncode:raise RuntimeError((directory/name).read_text(errors='replace')[-5000:])
inputs=['fpga/'+n for n in ['sdram.sv','amiga_video.sv','amiga_video_capture.sv','amiga_video_geometry.sv','amiga_video_cache.sv','amiga_video_dma.sv','nes_osd.sv','nes_osd_font.sv','hdmi_tx.sv','tmds_encoder.sv']]+['tests/sdram_burst_model.v','tests/video_system_top.sv','tests/video_system_test.cpp','tests/run_video_system.py']
hashes={n:hashlib.sha256((root/n).read_bytes()).hexdigest() for n in inputs}
if args.io_timing:hashes['io_paths']=io_paths
if not args.no_build:
    run([tool/'bin/verilator_bin.exe','--cc','--Wno-fatal',*(['--timing','--timescale','1ps/1ps'] if args.io_timing else []),'--top-module','video_system_top','--Mdir','obj',stage/'video_system_top.sv',stage/'sdr_chip_model.v',controller,*[root/'fpga'/n for n in ['amiga_video.sv','amiga_video_capture.sv','amiga_video_geometry.sv','amiga_video_cache.sv','amiga_video_dma.sv','nes_osd.sv','nes_osd_font.sv','hdmi_tx.sv','tmds_encoder.sv']]],'verilator.log')
    inc=tool/'share/verilator/include'
    cmd=['/nologo','/EHsc','/std:c++20','/utf-8','/O2','/MD','/DVL_TIME_CONTEXT','/MP4','/I'+str(inc),'/I'+str(inc/'vltstd'),'/Iobj',root/'tests/video_system_test.cpp',*list((stage/'obj').glob('*.cpp')),inc/'verilated.cpp',inc/'verilated_threads.cpp','/Fevideo_system_test.exe']
    if args.io_timing:cmd+=['/DSDRAM_IO_TIMING',inc/'verilated_timing.cpp']
    (stage/'compile.rsp').write_text(subprocess.list2cmdline(list(map(str,cmd))),encoding="utf-8-sig")
    run([vc/'bin/Hostx64/x64/cl.exe','@compile.rsp'],'compile.log')
    (stage/'sources.json').write_text(json.dumps(hashes,indent=2))
else:
    assert json.loads((stage/'sources.json').read_text())==hashes,'Sources changed; rebuild required'
cases=[tuple(map(int,args.case.split(',')))] if args.case else [(200,0,0,1)]
if args.all:cases=[(200,0,0,1),(256,0,0,1),(256,1,0,1),(200,0,4,1),(200,0,5,1),(256,1,2,1),(200,0,8,1),(200,0,0,0),(200,1,0,0),(256,1,0,1,41,1),(256,1,2,1,42,1),(200,1,0,0,43,1),(200,0,4,1,41,1),(256,0,0,1,43,1)]
if args.extended:cases=[(256,1,0,1,0,1,736),(256,1,2,1,0,1,736),(256,0,0,1,0,1,736),(200,0,4,1,0,1,736)]
if args.amiga_bus:cases=[(256,0,0,1,40,2),(256,0,0,1,0,2,736),(200,0,4,0,40,2),(256,1,0,1,40,3),(256,1,2,1,40,3),(256,1,0,1,0,3,736),(256,0,0,1,40,3),(256,1,0,1,40,2)]
results=[]
def png(ppm,out):
 with ppm.open('rb') as f:
  assert f.readline()==b'P6\n';w,h=map(int,f.readline().split());assert f.readline()==b'255\n';rgb=f.read()
 def chunk(t,b):return struct.pack('>I',len(b))+t+b+struct.pack('>I',zlib.crc32(t+b)&0xffffffff)
 pixels=b''.join(b'\0'+rgb[y*w*3:(y+1)*w*3] for y in range(h))
 out.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(pixels))+chunk(b'IEND',b''))
def execute_case(case):
 name='-'.join(map(str,case))
 directory=stage/'cases'/name;directory.mkdir(parents=True,exist_ok=True)
 run([stage/'video_system_test.exe',*case],'run.log',cwd=directory)
 text=(directory/'run.log').read_text()
 png(directory/'frame.ppm',stage/(name+'.png'))
 shutil.copy2(directory/'cache.csv',stage/(name+'.csv'))
 shutil.copy2(directory/'bus.csv',stage/(name+'-bus.csv'))
 return {'case':case,'result':text}
failures=[]
with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
 futures={pool.submit(execute_case,c):c for c in cases}
 for future in concurrent.futures.as_completed(futures):
  try:
   result=future.result();results.append(result);print(result['result'],flush=True)
  except Exception as error:
   failures.append({'case':futures[future],'error':str(error)});print(str(error),flush=True)
  result_path.write_text(json.dumps({'passed':False,'state':'running','completed':results,'failures':failures},indent=2))
report={'passed':not failures,'cases':results,'failures':failures,'hardware':False,'electrical_timing':False,
 'scope':'Actual capture/DMA/cache/RGB pixel path; behavioural SDRAM; parallel RGB and decoded TMDS; synthetic CPU/chipset traffic on marked cases',
 'source_sha256':hashes}
if args.io_timing:report.update(io_paths=io_paths,clock_phase_degrees=270,scope='Actual framebuffer path with representative SDF IO delays and assumed SDRAM tAC; no board validation')
result_path.write_text(json.dumps(report,indent=2)+'\n')
if failures:raise SystemExit('Failed video cases: '+str([f['case'] for f in failures]))
