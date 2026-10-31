from pathlib import Path
import hashlib, json, os, subprocess, sys
root=Path(__file__).resolve().parents[1]
stage=root/'tests/build'; stage.mkdir(exist_ok=True)
tool=Path(r'E:\zx\tools\rtl\ucrt64')
vc=Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207')
sdk=Path(r'C:\Program Files (x86)\Windows Kits\10'); v='10.0.26100.0'
env=os.environ.copy()
env['PATH']=str(tool/'bin')+';'+str(vc/'bin/Hostx64/x64')+';'+env['PATH']
env['VERILATOR_ROOT']=str(tool/'share/verilator')
env['INCLUDE']=';'.join(map(str,[vc/'include',*[sdk/'Include'/v/x for x in ['ucrt','shared','um']]]))
# Windows SDK libraries are laid out as <version>/<kind>/x64.
env['LIB']=';'.join(map(str,[vc/'lib/x64',*[sdk/'Lib'/v/x/'x64' for x in ['ucrt','um']]]))
logs=[]
def run(cmd):
    p=subprocess.run(list(map(str,cmd)),cwd=stage,env=env,capture_output=True,timeout=240)
    text=(p.stdout+p.stderr).decode('utf-8','replace'); logs.append(text)
    (root/'tests/run.log').write_text('\n'.join(logs),encoding='utf-8')
    if p.returncode: raise RuntimeError(text[-6000:])
    return text
run([tool/'bin/verilator_bin.exe','--cc','--Wno-fatal','--top-module','link_top','--Mdir','obj',
     root/'tests/link_top.sv',root/'fpga/amiga_link.sv',root/'fpga/amiga_disk.sv',root/'fpga/next_qspi.sv',root/'fpga/fb_sdspi.sv'])
inc=tool/'share/verilator/include'
run([vc/'bin/Hostx64/x64/cl.exe','/nologo','/EHsc','/std:c++20','/utf-8','/O2','/MD','/DVL_TIME_CONTEXT',
     '/I'+str(inc),'/I'+str(inc/'vltstd'),'/Iobj',root/'tests/link_test.cpp',*list((stage/'obj').glob('*.cpp')),
     inc/'verilated.cpp',inc/'verilated_threads.cpp','/Felink_test.exe'])
rom=Path(sys.argv[1]) if len(sys.argv)>1 else root/'ROM/kickstart-3.1-a500_a600_a2000.rom'
result=run([stage/'link_test.exe',rom])
run([vc/'bin/Hostx64/x64/cl.exe','/nologo','/TC','/std:c11','/utf-8','/O2','/MD',
     '/I'+str(root/'esp/main'),root/'tests/app_test.c',root/'esp/main/ps2_hid.c','/Feapp_test.exe'])
result+=run([stage/'app_test.exe',rom])
tested=['fpga/amiga_link.sv','fpga/amiga_disk.sv','fpga/next_qspi.sv','fpga/fb_sdspi.sv',
        'esp/main/app.c','esp/main/ps2_hid.c','esp/main/ps2_hid.h','esp/main/zm_app.h',
        'tests/link_top.sv','tests/link_test.cpp','tests/app_test.c','tests/run.py']
hashes={p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in tested}
(root/'tests/result.json').write_text(json.dumps({'passed':True,'result':result,'hardware':False,'sdf':False,'source_sha256':hashes},indent=2))
print(result)
