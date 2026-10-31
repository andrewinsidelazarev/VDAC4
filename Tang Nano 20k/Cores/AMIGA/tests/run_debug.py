"""Verify that the passive observer retains the actual first completed CPU reads."""
from pathlib import Path
import json, os, subprocess
root=Path(__file__).resolve().parents[1]
stage=root/'tests/debug-build';stage.mkdir(exist_ok=True)
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
    (root/'tests/debug.log').write_text('\n'.join(logs),encoding='utf-8')
    if p.returncode: raise RuntimeError(output[-6000:])
    return output
run([tool/'bin/verilator_bin.exe','--cc','--Wno-fatal','--top-module','amiga_debug','--Mdir','obj',root/'fpga/amiga_debug.sv'])
inc=tool/'share/verilator/include'
run([vc/'bin/Hostx64/x64/cl.exe','/nologo','/EHsc','/std:c++20','/utf-8','/O2','/MD','/DVL_TIME_CONTEXT',
     '/I'+str(inc),'/I'+str(inc/'vltstd'),'/Iobj',root/'tests/debug_test.cpp',*list((stage/'obj').glob('*.cpp')),
     inc/'verilated.cpp',inc/'verilated_threads.cpp','/Fedebug_test.exe'])
result=run([stage/'debug_test.exe'])
(root/'tests/debug-result.json').write_text(json.dumps({'passed':True,'result':result,'hardware':False},indent=2)+'\n')
print(result)
