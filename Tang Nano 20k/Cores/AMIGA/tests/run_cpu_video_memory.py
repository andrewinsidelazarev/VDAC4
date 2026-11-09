"""CPU writes/masks/48-bit reads with VIDEO_PORT burst mode and timed pads.

Reuses the existing exhaustive CPU test, adds a contending video-port client,
and checks the bytes actually stored in the behavioural SDRAM. The delays
are representative SDF paths, not a board signal-integrity model.
"""
from pathlib import Path
import argparse, ast, hashlib, json, os, subprocess

ap=argparse.ArgumentParser()
ap.add_argument('--saturated-video',action='store_true',help='Back-to-back legal video requests; CPU must still receive service within 600 SDRAM clocks')
args=ap.parse_args()

root = Path(__file__).resolve().parents[1]
stage = root/('tests/cpu-video-saturated-build' if args.saturated_video else 'tests/cpu-video-memory-build')
stage.mkdir(exist_ok=True)
out = root/('tests/cpu-video-saturated-result.json' if args.saturated_video else 'tests/cpu-video-memory-result.json')
out.write_text(json.dumps({'passed': False, 'state': 'running'}))
old = (root/'tests/run_fast_sdram.py').read_text()
literals = [n.value for n in ast.walk(ast.parse(old))
            if isinstance(n, ast.Constant) and isinstance(n.value, str)]
wrapper = next(s for s in literals if s.startswith('module fast_memory_top('))
cpp = next(s for s in literals if '#include "Vfast_memory_top.h"' in s)
io = json.loads((root/'diagnostics/stripe/original-sdf-margins.json').read_text())
# Same documented representative paths as the full ROM and pixel tests.
clock_ps, write_ps, read_ps = 4971, 5043, 1853
wrapper = wrapper.replace('output ready,p2_ack,', 'output [31:0] video_transactions,output ready,p2_ack,')
wrapper = wrapper.replace('.CHIP48_BURST(1)', '.VIDEO_PORT(1),.VIDEO_STREAM(1),.CHIP48_BURST(1)')
wrapper = wrapper.replace('.p2_ack(p2_ack));', '''.p2_ack(p2_ack),
 .video_cs(vc),.video_we(vw),.video_addr(21'h180000),.video_din({4{32'hcb9e2734}}),
 .video_ack(va),.video_dout(vd),.video_read_valid(vv),.video_fetch_index());
 reg vc=0,vw=1,seen=0;reg [5:0] wait_count=0;
 reg [31:0] transfers=0;wire va,vv;wire [127:0] vd;
 assign video_transactions=transfers;
 always @(negedge clk) begin
  if(!reset_n)begin vc<=0;vw<=1;seen<=0;wait_count<=0;transfers<=0;end
  else if(ready)begin
   if(vv)assert(vd[31:0]==32'hcb9e2734)else $fatal(1,"Corrupted video data during CPU transactions");
   if(vc && va!=seen)begin vc<=0;seen<=va;vw<=!vw;wait_count<=36;transfers<=transfers+1;end
   else if(!vc && wait_count!=0)wait_count<=wait_count-1'b1;
   else if(!vc)begin vc<=1;seen<=va;end
  end
 end''')
if args.saturated_video:wrapper=wrapper.replace('wait_count<=36;','wait_count<=0;')
old_model = '''sdr_chip_model chip(.clk(sd_clk),.dq(dq),.addr(a),.dqm(dqm),.ba(ba),
 .cs_n(cs_n),.ras_n(ras_n),.cas_n(cas_n),.we_n(we_n));'''
assert wrapper.count(old_model) == 1
wrapper = wrapper.replace(old_model, f'''
 wire chip_clk;wire [31:0] write_pin,read_pin,read_core;
 wire read_enable,read_core_enable,write_enable;
 wire [10:0] a_pin;wire [3:0] dqm_pin;wire [1:0] ba_pin;
 wire cs_pin,we_pin,ras_pin,cas_pin;
 assign #{clock_ps} chip_clk=sd_clk;
 assign #{write_ps} write_pin=dut.io32.dq_out;
 assign #{write_ps} write_enable=dut.sd_dq;
 assign #{read_ps} read_core=read_pin;
 assign #{read_ps} read_core_enable=read_enable;
 assign dq=!dut.sd_dq && read_core_enable ? read_core:32'bz;
 assign #{write_ps} {{a_pin,dqm_pin,ba_pin,cs_pin,we_pin,ras_pin,cas_pin}}={{a,dqm,ba,cs_n,we_n,ras_n,cas_n}};
 sdr_chip_model chip(.clk(chip_clk),.dq_in(write_pin),.dq_out(read_pin),.dq_oe(read_enable),
 .write_enable(write_enable),.addr(a_pin),.dqm(dqm_pin),.ba(ba_pin),
 .cs_n(cs_pin),.we_n(we_pin),.ras_n(ras_pin),.cas_n(cas_pin));''')
(stage/'fast_memory_top.sv').write_text(wrapper)
controller = (root/'fpga/sdram.sv').read_text()
assert controller.count('if (WIDTH32) begin\n    reg [31:0] dq_q;') == 1
(stage/'sdram_io_test.sv').write_text(controller.replace('if (WIDTH32) begin\n    reg [31:0] dq_q;', 'if (WIDTH32) begin:io32\n    reg [31:0] dq_q;'))
# Reuse and guard the timed model generated from the same production controller.
timed = root/'tests/video-timed-build'
assert (timed/'sdram_io_test.sv').read_text() == (stage/'sdram_io_test.sv').read_text()
model = (timed/'sdr_chip_model.v').read_text()
model = model.replace("mem[n]=32'hccaa55cc ^ (n*32'h9e3779b9);", "mem[n]=32'hccaa55cc;")
(stage/'sdr_chip_model.v').write_text(model)
cpp = cpp.replace('for(unsigned phase:{0u,180u}){', 'for(unsigned phase:{270u}){')
cpp = cpp.replace('Vfast_memory_top q;unsigned cycles', 'VerilatedContext ctx;Vfast_memory_top q(&ctx);unsigned cycles')
start = cpp.index(' auto tick=')
stop = cpp.index(' auto idle=', start)
cpp = cpp[:start]+''' uint64_t t=0,ec=66,es=165;
 auto settle=[&](uint64_t tick){
  uint64_t ps=(tick*1000000ull+5643ull)/11286ull;
  while(q.eventsPending() && q.nextTimeSlot()<ps){ctx.time(q.nextTimeSlot());q.eval();}
  ctx.time(ps);q.eval();
 };
 auto tick=[&](){
  q.sync=(cycles%12)<3;q.eval();uint64_t end=t+132;
  while(std::min(ec,es)<=end){t=std::min(ec,es);settle(t);
   if(t==ec){q.clk=!q.clk;ec+=66;}if(t==es){q.sd_clk=!q.sd_clk;es+=66;}q.eval();
  }
  t=end;settle(t);cycles++;
 };
'''+cpp[stop:]
cpp = cpp.replace('#include <cstdio>', '#include <algorithm>\n#include <cstdio>')
cpp = cpp.replace(' auto transfer=', ' unsigned maximum_wait=0;\n auto transfer=')
cpp = cpp.replace('  q.p2_cs=0;\n };', '  maximum_wait=std::max(maximum_wait,wait);q.p2_cs=0;\n };')
if args.saturated_video:cpp=cpp.replace('CHECK(++wait<100);','CHECK(++wait<600);')
cpp = cpp.replace('unsigned data=(n<256)', 'if((a>>1)>=0x180000 && (a>>1)<0x180004)a^=0x10000;\n  unsigned data=(n<256)')
cpp = cpp.replace(' printf("PASS phase', ' CHECK(q.video_transactions>100);printf("Concurrent video bursts: %u\\n",q.video_transactions);\n printf("PASS phase')
cpp = cpp.replace(' CHECK(q.video_transactions>100);', ' printf("Maximum CPU wait: %u SDRAM clocks\\n",maximum_wait);CHECK(q.video_transactions>100);')
(stage/'fast_memory_test.cpp').write_text(cpp)

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
    with (stage/name).open('wb') as f:
        p = subprocess.run(list(map(str, cmd)), cwd=stage, env=env, stdout=f, stderr=subprocess.STDOUT, timeout=300)
    if p.returncode:
        error=(stage/name).read_text(errors='replace')[-5000:]
        out.write_text(json.dumps({'passed':False,'state':'failed','stage':str(stage),
            'saturated_video':args.saturated_video,'error':error,
            'sdram_sha256':hashlib.sha256((root/'fpga/sdram.sv').read_bytes()).hexdigest()},indent=2))
        raise RuntimeError(error)
run([tool/'bin/verilator_bin.exe', '--cc', '--timing', '--timescale', '1ps/1ps', '--Wno-fatal',
     '--top-module', 'fast_memory_top', '--Mdir', 'obj', stage/'fast_memory_top.sv',
     stage/'sdr_chip_model.v', stage/'sdram_io_test.sv'], 'verilator.log')
inc = tool/'share/verilator/include'
cmd = ['/nologo', '/EHsc', '/std:c++20', '/utf-8', '/O2', '/MD', '/DVL_TIME_CONTEXT', '/MP4',
       '/I'+str(inc), '/I'+str(inc/'vltstd'), '/Iobj', stage/'fast_memory_test.cpp',
       *list((stage/'obj').glob('*.cpp')), inc/'verilated.cpp', inc/'verilated_threads.cpp',
       inc/'verilated_timing.cpp', '/Fefast_memory_test.exe']
(stage/'compile.rsp').write_text(subprocess.list2cmdline(list(map(str, cmd))), encoding='utf-8-sig')
run([vc/'bin/Hostx64/x64/cl.exe', '@compile.rsp'], 'compile.log')
run([stage/'fast_memory_test.exe'], 'run.log')
result = (stage/'run.log').read_text()
report = {'passed': True, 'hardware': False, 'electrical_timing': False, 'result': result,'saturated_video':args.saturated_video,
          'clock_phase_degrees': 270, 'io_paths_ps': {'clock': clock_ps, 'write': write_ps, 'read': read_ps, 'tAC': 6000},
          'sdf_sha256': io['sdf_sha256'],
          'source_sha256': {n: hashlib.sha256((root/n).read_bytes()).hexdigest() for n in
                           ['fpga/sdram.sv', 'tests/run_cpu_video_memory.py', 'tests/run_fast_sdram.py', 'tests/sdram_burst_model.v']}}
out.write_text(json.dumps(report, indent=2)+'\n')
print(result)
