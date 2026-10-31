"""Isolated builds; never programs hardware. Run: python build.py esp|fpga."""
from pathlib import Path
import argparse, datetime, hashlib, json, os, shutil, subprocess
from validation import fpga_report

root=Path(__file__).resolve().parent
ap=argparse.ArgumentParser();ap.add_argument('kind',choices=['esp','fpga'])
ap.add_argument('--cache',default=r'E:\zx\cache\tmp')
args=ap.parse_args()
stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%SZ')
stage=Path(args.cache)/('amiga-'+args.kind+'-'+stamp)
stage.mkdir(parents=True,exist_ok=False)
out=root/'build-results'/stamp/args.kind;out.mkdir(parents=True)
env=os.environ.copy();env['TEMP']=env['TMP']=str(stage)
if args.kind=='esp':
    shutil.copytree(root/'esp',stage/'esp',ignore=shutil.ignore_patterns('build','dist','sdkconfig','sdkconfig.old'))
    command=['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',str(stage/'esp/tools/build.ps1')]
    target=stage/'esp/dist/AMIGA.ELF'
else:
    shutil.copytree(root/'upstream/src',stage/'upstream/src',ignore=shutil.ignore_patterns('impl'))
    shutil.copytree(root/'fpga',stage/'fpga')
    shutil.copyfile(root/'build.tcl',stage/'build.tcl')
    command=[r'C:\Gowin\Gowin_V1.9.11.03_Education_x64\IDE\bin\gw_sh.exe','build.tcl']
    target=stage/'impl/pnr/amigaVdac4.bin'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
(out/'sources.json').write_text(json.dumps({str(p.relative_to(stage)):sha(p) for p in stage.rglob('*') if p.is_file()},indent=2))
print(f'{args.kind}: {stage}',flush=True)
with (out/'build.log').open('wb') as log:
    result=subprocess.run(command,cwd=stage,env=env,stdout=log,stderr=subprocess.STDOUT,
                          creationflags=subprocess.CREATE_NO_WINDOW)
report={'kind':args.kind,'exit_code':result.returncode,'stage':str(stage),'hardware_tested':False,'uploaded':False}
exit_code=result.returncode
if exit_code==0 and not target.is_file():
    exit_code=1; report['error']='Build returned success without the requested artifact'
if exit_code==0 and args.kind=='fpga':
    try:
        report['timing']=fpga_report(stage,out)
        if not report['timing']['passed']: exit_code=1
    except (OSError,ValueError) as e:
        exit_code=1; report['error']=str(e)
report['exit_code']=exit_code
if exit_code==0:
    report.update(bytes=target.stat().st_size,sha256=sha(target))
    shutil.copyfile(target,out/('AMIGA.ELF' if args.kind=='esp' else 'AMIGA.BIN'))
(out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2),flush=True)
raise SystemExit(exit_code)
