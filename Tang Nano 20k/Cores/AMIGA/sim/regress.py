"""Unattended RTL smoke test with a visible, original test ROM."""
from pathlib import Path
import argparse,datetime,json,subprocess
from build import HERE,build,sha
from check_clocks import check
from check_memory import check as check_memory
from diagnostic_rom import make
ap=argparse.ArgumentParser();ap.add_argument('--profile',choices=['stable','current'],default='current');ap.add_argument('--seconds',type=float,default=.16);ap.add_argument('--rom',type=Path);ap.add_argument('--full-loader',action='store_true');ap.add_argument('--inspect',type=Path);ap.add_argument('--output',type=Path);a=ap.parse_args()
rom=a.rom or make(HERE/'fixtures/diagnostic.rom')
startup=None
if a.inspect:
    out=a.inspect;gate=json.loads((out/'clocks.json').read_text());memory=json.loads((out/'memory.json').read_text());manifest=json.loads((out/'manifest.json').read_text());code=0 if not (out/'error.txt').exists() else 1
    if (out/'startup.json').exists():startup=json.loads((out/'startup.json').read_text())
else:
    gate=check(a.profile)
    if not gate['passed']:raise SystemExit(1)
    memory=check_memory(a.profile)
    if not memory['passed']:raise SystemExit(1)
    if a.profile=='current':
        from check_sdram_startup import check as check_startup
        startup=check_startup('current')
        if not startup['passed']:raise SystemExit('Нарушена последовательность запуска SDRAM')
    stage=build(a.profile);out=a.output.resolve() if a.output else HERE/'runs'/('regression-'+a.profile+'-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'));out.mkdir(parents=True)
    manifest=json.loads((stage/'build.json').read_text());manifest.update(rom_sha256=sha(rom),rom_route='QSPI' if a.full_loader else 'preinitialized SDRAM')
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2));(out/'clocks.json').write_text(json.dumps(gate,indent=2));(out/'memory.json').write_text(json.dumps(memory,indent=2))
    if startup:(out/'startup.json').write_text(json.dumps(startup,indent=2))
    cmd=[str(stage/'amiga_sim.exe'),'--rom',str(rom),'--seconds',str(a.seconds),'--output',str(out)]
    if not a.full_loader:cmd.append('--fast-rom')
    with (out/'engine.log').open('w') as f:code=subprocess.run(cmd,cwd=stage,stdout=f,stderr=subprocess.STDOUT).returncode
s=json.loads((out/'status.json').read_text());v=json.loads((out/'vectors.json').read_text()) if (out/'vectors.json').exists() else []
data=rom.read_bytes();expected=[[i,int.from_bytes(data[i:i+2],'big')] for i in range(0,8,2)]
checks={'engine_no_error':code==0,'completed':s.get('state')=='stopped','profile':manifest['profile']==a.profile,'clock_gate':gate['passed'],'memory_gate':memory['passed'],'ROM':s.get('rom_verified') is True,'PLL':s.get('sys_lock')==s.get('video_lock')==1,'CPU_reset_released':s.get('cpu_reset_n')==1,'CPU_vectors':v==expected,'CPU_progress':s.get('cpu_steps',0)>100,'native_frames':s.get('native_frames',0)>0,'HDMI_frames':s.get('hdmi_frames',0)>0,'TMDS_data':s.get('hdmi_mismatches',1)==0}
if startup:checks['SDRAM_startup']=startup['passed']
if not a.rom:
    from PIL import Image
    colours={}
    for name in ['native','hdmi']:
        counts=[0]*4
        with Image.open(out/(name+'.ppm')) as im:
            pixels=im.convert('RGB').tobytes()
            for r,g,b in zip(pixels[0::3],pixels[1::3],pixels[2::3]):
                counts[0]+=r>128 and g<16 and b<16
                counts[1]+=g>128 and r<16 and b<16
                counts[2]+=b>128 and r<16 and g<16
                counts[3]+=min(r,g,b)>128
        colours[name]=counts
        checks[name+'_RAM_test_and_colours']=min(counts)>100
    s['test_pattern_colours_R_G_B_W']=colours
result={'passed':all(checks.values()),'checks':checks,'status':s,'vectors':v,'expected_vectors':expected}
(out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2));print(out)
raise SystemExit(0 if result['passed'] else 1)
