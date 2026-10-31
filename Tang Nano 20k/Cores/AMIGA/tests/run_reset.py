"""Regression for the observed idle-low button keeping Amiga permanently reset.

Extract the actual generated top-level reset circuit, not a reimplementation.
"""
from pathlib import Path
import hashlib,json,os,re,subprocess
root=Path(__file__).resolve().parents[1]
stage=root/'tests/reset-build';stage.mkdir(exist_ok=True)
result_path=root/'tests/reset-result.json'
result_path.write_text(json.dumps({'passed':False,'state':'running'}))
source=root/'fpga/amiga_vdac4.sv'
top=source.read_text(encoding='utf-8')
reset=re.search(r'reg \[1:0\] button_reset_sync=0;.*?wire nanomig_reset=.*?;',top,re.S)[0]
(stage/'reset_top.sv').write_text('''module reset_top(input clk_28m,rst_28m,
input [1:0] btn,input soft_reset,kbd_reset,rom_done,
output [7:0] hold_count,output reset_out);
'''+reset+'''
assign hold_count=warm_reset;
assign reset_out=nanomig_reset;
endmodule
''')
cst=(root/'fpga/amigaVdac4.cst').read_text(encoding='utf-8')
assert re.search(r'IO_LOC\s+"btn\[1\]"\s+87;',cst)
assert 'IO_PORT "btn[1]" IO_TYPE=LVCMOS33 PULL_MODE=DOWN;' in cst
tool=Path(r'E:\zx\tools\rtl\ucrt64')
vc=Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207')
sdk=Path(r'C:\Program Files (x86)\Windows Kits\10');v='10.0.26100.0'
env=os.environ.copy()
env['PATH']=str(tool/'bin')+';'+str(vc/'bin/Hostx64/x64')+';'+env['PATH']
env['VERILATOR_ROOT']=str(tool/'share/verilator')
env['INCLUDE']=';'.join(map(str,[vc/'include',*[sdk/'Include'/v/x for x in ['ucrt','shared','um']]]))
env['LIB']=';'.join(map(str,[vc/'lib/x64',*[sdk/'Lib'/v/x/'x64' for x in ['ucrt','um']]]))
logs=[]
def run(cmd):
    p=subprocess.run(list(map(str,cmd)),cwd=stage,env=env,capture_output=True,timeout=240)
    output=(p.stdout+p.stderr).decode('utf-8','replace');logs.append(output)
    (root/'tests/reset.log').write_text('\n'.join(logs),encoding='utf-8')
    if p.returncode:raise RuntimeError(output[-6000:])
    return output
run([tool/'bin/verilator_bin.exe','--cc','--Wno-fatal','--top-module','reset_top','--Mdir','obj',stage/'reset_top.sv'])
inc=tool/'share/verilator/include'
run([vc/'bin/Hostx64/x64/cl.exe','/nologo','/EHsc','/std:c++20','/utf-8','/O2','/MD','/DVL_TIME_CONTEXT',
     '/I'+str(inc),'/I'+str(inc/'vltstd'),'/Iobj',root/'tests/reset_test.cpp',*list((stage/'obj').glob('*.cpp')),
     inc/'verilated.cpp',inc/'verilated_threads.cpp','/Fereset_test.exe'])
result=run([stage/'reset_test.exe'])
files=['fpga/amiga_vdac4.sv','fpga/amigaVdac4.cst','tests/run_reset.py','tests/reset_test.cpp']
result_path.write_text(json.dumps({'passed':True,'result':result,'hardware':False,
    'source_sha256':{p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in files}},indent=2)+'\n')
print(result)
