from pathlib import Path
import subprocess,os,json,hashlib
root=Path(__file__).resolve().parents[1];stage=root/'.test-build/task-memory';stage.mkdir(parents=True,exist_ok=True)
cpp=root/'src/apps/module_app.cpp';s=cpp.read_text(encoding='utf-8');a=s.index('int ModuleApp::coreTaskStart(');b=s.index('void* ModuleApp::coreMutexCreate()',a)
(stage/'task_memory.cpp').write_text((root/'tests/task_memory_preamble.cpp').read_text(encoding='utf-8')+s[a:b]+(root/'tests/task_memory_cases.cpp').read_text(encoding='utf-8'),encoding='utf-8')
vc=Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207');sdk=Path(r'C:\Program Files (x86)\Windows Kits\10');version='10.0.26100.0'
env=os.environ.copy();env['PATH']=str(vc/'bin/Hostx64/x64')+';'+env['PATH'];env['INCLUDE']=';'.join(map(str,[vc/'include',*[sdk/'Include'/version/x for x in ['ucrt','shared','um']]]));env['LIB']=';'.join(map(str,[vc/'lib/x64',*[sdk/'Lib'/version/x/'x64' for x in ['ucrt','um']]]))
fw=Path(r'E:\zx\Tang Nano 20k VDAC\VDAC4\ESP32 S3 Zero N8R8\Firmware')
args=['/nologo','/std:c++17','/EHsc','/utf-8','/O2','/I'+str(root/'include'),'/I'+str(fw/'modules/sdk/include'),'task_memory.cpp','/Fetask_memory.exe']
(stage/'compile.rsp').write_text(subprocess.list2cmdline(args),encoding='utf-8-sig')
for cmd,name in [([str(vc/'bin/Hostx64/x64/cl.exe'),'@compile.rsp'],'compile.log'),([str(stage/'task_memory.exe')],'run.log')]:
 p=subprocess.run(cmd,cwd=stage,env=env,capture_output=True);(stage/name).write_bytes(p.stdout+p.stderr)
 if p.returncode:raise SystemExit((p.stdout+p.stderr).decode('utf-8','replace'))
result=(stage/'run.log').read_text();print(result)
(root/'tests/task-memory-result.json').write_text(json.dumps({'passed':True,'scope':'Actual extracted coreTaskStart with deterministic FreeRTOS/allocation stubs','hardware':False,'result':result,'source_sha256':{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [cpp,root/'include/zifi/memory_retry.hpp',root/'tests/task_memory_cases.cpp',root/'tests/task_memory_preamble.cpp']}},indent=2)+'\n')
