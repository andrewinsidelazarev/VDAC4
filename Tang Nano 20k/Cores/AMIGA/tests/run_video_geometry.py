from pathlib import Path
import hashlib,json,os,subprocess
root=Path(__file__).resolve().parents[1]
stage=root/'tests/geometry-build';stage.mkdir(exist_ok=True)
tool=Path(r'E:\zx\tools\rtl\ucrt64')
vc=Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207')
sdk=Path(r'C:\Program Files (x86)\Windows Kits\10');v='10.0.26100.0'
env=os.environ.copy();env['PATH']=str(tool/'bin')+';'+str(vc/'bin/Hostx64/x64')+';'+env['PATH']
env['VERILATOR_ROOT']=str(tool/'share/verilator')
env['INCLUDE']=';'.join(map(str,[vc/'include',*[sdk/'Include'/v/x for x in ['ucrt','shared','um']]]))
env['LIB']=';'.join(map(str,[vc/'lib/x64',*[sdk/'Lib'/v/x/'x64' for x in ['ucrt','um']]]))
def run(cmd,name):
 with (stage/name).open('wb') as f:p=subprocess.run(list(map(str,cmd)),cwd=stage,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=240)
 if p.returncode:raise RuntimeError((stage/name).read_text(errors='replace')[-4000:])
run([tool/'bin/verilator_bin.exe','--cc','--Wno-fatal','--top-module','amiga_video_geometry','--Mdir','obj',root/'fpga/amiga_video_geometry.sv'],'verilator.log')
inc=tool/'share/verilator/include'
cmd=['/nologo','/EHsc','/std:c++20','/utf-8','/O2','/MD','/DVL_TIME_CONTEXT','/MP4','/I'+str(inc),'/I'+str(inc/'vltstd'),'/Iobj',root/'tests/video_geometry_test.cpp',*list((stage/'obj').glob('*.cpp')),inc/'verilated.cpp',inc/'verilated_threads.cpp','/Fegeometry.exe']
(stage/'compile.rsp').write_text(subprocess.list2cmdline(list(map(str,cmd))),encoding="utf-8-sig")
run([vc/'bin/Hostx64/x64/cl.exe','@compile.rsp'],'compile.log')
run([stage/'geometry.exe'],'run.log')
report={'passed':True,'hardware':False,'result':(stage/'run.log').read_text(),'sha256':{n:hashlib.sha256((root/n).read_bytes()).hexdigest() for n in ['fpga/amiga_video_geometry.sv','tests/video_geometry_test.cpp','tests/run_video_geometry.py']}}
(root/'tests/video-geometry-result.json').write_text(json.dumps(report,indent=2)+'\n')
print(report['result'])
