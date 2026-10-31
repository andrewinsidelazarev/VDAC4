"""Package successful builds for review; never accesses hardware.

python package.py <fpga build-results directory> <esp build-results directory> --version 0.2 --update-staging
Also stages this user's ROM and the two core files in sibling SD/.
"""
from pathlib import Path
import argparse, datetime, hashlib, json, re, shutil, subprocess
from validation import fpga_report

root=Path(__file__).resolve().parent
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
ap=argparse.ArgumentParser()
ap.add_argument('fpga');ap.add_argument('esp');ap.add_argument('--version',default='0.2')
ap.add_argument('--update-staging',action='store_true',help='Replace staging binaries only if the previous files exist in a retained release')
args=ap.parse_args()
if not re.fullmatch(r'\d+(?:\.\d+)+(?:-debug\d+)?',args.version): raise SystemExit('Invalid release version')
fp,esp=map(lambda p:Path(p).resolve(),(args.fpga,args.esp))
out=root/'release'/args.version
if out.exists(): raise SystemExit('Release already exists; select a new version or review it manually.')
test=json.loads((root/'tests/result.json').read_text())
if not test.get('passed'): raise SystemExit('Interface tests have not passed')
for name,digest in test.get('source_sha256',{}).items():
    if sha(root/name)!=digest:raise SystemExit('Interface source changed since test: '+name)
debug=None
reset=None
if '-debug' in args.version:
    debug=json.loads((root/'tests/debug-result.json').read_text())
    if not debug.get('passed'): raise SystemExit('CPU observer tests have not passed')
top=(root/'fpga/amiga_vdac4.sv').read_text(encoding='utf-8')
adf=None
if '.disk_mounted(sd_img_mounted)' in top:
    adf=json.loads((root/'disks/manifest.json').read_text())
    adf_path=root/'disks'/adf['adf']
    if adf_path.stat().st_size!=adf['bytes'] or sha(adf_path)!=adf['sha256']:
        raise SystemExit('Test disk does not match its manifest')
    disk_test=json.loads((root/'tests/adf-boot-result.json').read_text())
    if not disk_test.get('testkit_menu_verified') or disk_test.get('adf_sha256')!=adf['sha256']:
        raise SystemExit('ADF boot to Test Kit menu has not been verified')
    for name,digest in disk_test['source_sha256'].items():
        if sha(root/name)!=digest:raise SystemExit('ADF boot source changed since test: '+name)
clock_margin=None
pll=json.loads((root/'tests/pll-result.json').read_text())
if not pll.get('passed'):raise SystemExit('Vendor PLL phase regression has not passed')
for name,digest in pll['source_sha256'].items():
    if sha(root/name)!=digest:raise SystemExit('PLL phase source changed since test: '+name)
fast_sdram=None
registered_sdram_output=(root/'fpga/sdram.sv').is_file() and 'dq_out <= addr_0' in (root/'fpga/sdram.sv').read_text(encoding='utf-8')
shared_sdram=None
video_probe_enabled='.video_probe(video_probe)' in top
if video_probe_enabled:
    video_probe_test=json.loads((root/'tests/board-video-probe-result.json').read_text())
    if not video_probe_test.get('passed'):raise SystemExit('Output probe client tests have not passed')
    for name,digest in video_probe_test['source_sha256'].items():
        if sha(root/name)!=digest:raise SystemExit('Output probe client changed since test: '+name)
    shared_sdram=json.loads((root/'tests/shared-sdram-result.json').read_text())
    if not shared_sdram.get('passed'):raise SystemExit('Concurrent CPU/chipset SDRAM test has not passed')
    for name,digest in shared_sdram['source_sha256'].items():
        if sha(root/name)!=digest:raise SystemExit('Concurrent SDRAM source changed since test: '+name)
if registered_sdram_output:
    fast_sdram=json.loads((root/'tests/fast-sdram-result.json').read_text())
    if not fast_sdram.get('passed'):raise SystemExit('CPU SDRAM byte-mask regression has not passed')
    for name,digest in fast_sdram['source_sha256'].items():
        if sha(root/name)!=digest:raise SystemExit('CPU SDRAM source changed since test: '+name)
registered_sdram=(root/'fpga/sdram.sv').is_file() and 'dq_q <= sd_data' in (root/'fpga/sdram.sv').read_text(encoding='utf-8')
if '.SYNC_DELAY(3)' in top or '.SYNC_DELAY(2)' in top:
    clock_margin=json.loads((root/'tests/clock-margin-result.json').read_text())
    if not clock_margin.get('passed'):raise SystemExit('Clock crossing regression has not passed')
    for name,digest in clock_margin['source_sha256'].items():
        if sha(root/name)!=digest:raise SystemExit('Clock timing source changed since test: '+name)
    if registered_sdram:
        rom_sdram=json.loads((root/'tests/rom-sdram-result.json').read_text())
        if not rom_sdram.get('passed'):raise SystemExit('Registered SDRAM ROM transfer regression has not passed')
        for name,digest in rom_sdram['source_sha256'].items():
            if sha(root/name)!=digest:raise SystemExit('ROM SDRAM source changed since test: '+name)
if 'button_reset_sync' in top:
    reset=json.loads((root/'tests/reset-result.json').read_text())
    if not reset.get('passed'): raise SystemExit('Reset regression has not passed')
    for name,digest in reset['source_sha256'].items():
        if sha(root/name)!=digest: raise SystemExit('Reset source changed since test: '+name)
video=json.loads((root/'tests/video-result.json').read_text())
if not video.get('passed'): raise SystemExit('RGB888 tests have not passed')
for name,digest in video['source_sha256'].items():
    if sha(root/name)!=digest: raise SystemExit('Video source changed since test: '+name)
records=[]
for directory,kind,name in ((fp,'fpga','AMIGA.BIN'),(esp,'esp','AMIGA.ELF')):
    record=json.loads((directory/'result.json').read_text())
    if record['kind']!=kind or record['exit_code']!=0 or sha(directory/name)!=record['sha256']:
        raise SystemExit('Artifact does not match successful build: '+str(directory))
    for relative,digest in json.loads((directory/'sources.json').read_text()).items():
        if sha(root/relative)!=digest: raise SystemExit('Source changed since build: '+relative)
    records.append(record)
# Re-evaluate routed timing even for results made before the build gate existed.
records[0]['timing']=fpga_report(Path(records[0]['stage']),fp)
if not records[0]['timing']['passed']: raise SystemExit('Routed timing failed')
(fp/'result.json').write_text(json.dumps(records[0],indent=2)+'\n')
pin=json.loads((root/'upstream.lock.json').read_text())
actual=subprocess.check_output(['git','--git-dir',str(root/'upstream.git'),'rev-parse','HEAD'],text=True).strip()
if actual!=pin['commit']: raise SystemExit('Upstream revision changed')
dirty=subprocess.check_output(['git','--git-dir',str(root/'upstream.git'),'--work-tree',str(root/'upstream'),'status','--porcelain','--untracked-files=no'],text=True)
if dirty: raise SystemExit('Tracked upstream sources have changed')

# Validate the user's actual ROM before preparing the local card layout.
rom=root/'ROM/kickstart-3.1-a500_a600_a2000.rom'
data=rom.read_bytes()
if len(data)!=524288: raise SystemExit('Expected a 512 KiB ROM')
check=0
for i in range(0,len(data),4):
    check+=int.from_bytes(data[i:i+4],'big');check=(check&0xffffffff)+(check>>32)
if check!=0xffffffff: raise SystemExit('ROM Amiga checksum failed')
sd=root.parent/'SD'
copies=[(fp/'AMIGA.BIN',sd/'CORES/AMIGA.BIN'),(esp/'AMIGA.ELF',sd/'CORES/AMIGA.ELF'),
        (rom,sd/'AMIGA/KICK31.ROM')]
if adf:copies.append((adf_path,sd/adf['target']))
for source,target in copies:
    if target.exists() and sha(source)!=sha(target):
        previous=list((root/'release').glob('*/'+target.relative_to(sd).as_posix()))
        if not args.update_staging or not any(sha(p)==sha(target) for p in previous):
            raise SystemExit('Existing card staging file has no retained release; no overwrite: '+str(target))

(out/'CORES').mkdir(parents=True)
(out/'reports').mkdir()
shutil.copyfile(root/'tests/pll-result.json',out/'reports/pll-result.json')
shutil.copytree(root/'diagnostics/pll-audit',out/'reports/pll-audit')
if adf:
    (out/'AMIGA').mkdir()
    shutil.copy2(adf_path,out/'AMIGA/DF0.ADF')
    shutil.copy2(root/'disks/manifest.json',out/'AMIGA/disk-manifest.json')
    shutil.copy2(root/'disks/AmigaTestKit-1.21/COPYING',out/'AMIGA/COPYING-AmigaTestKit')
    shutil.copy2(root/'disks/README.txt',out/'AMIGA/README.txt')
    shutil.copytree(root/'diagnostics/adf-boot',out/'reports/adf-boot')
for directory,kind,name in ((fp,'fpga','AMIGA.BIN'),(esp,'esp','AMIGA.ELF')):
    shutil.copyfile(directory/name,out/'CORES'/name)
    report=out/'reports'/kind;report.mkdir()
    for p in directory.iterdir():
        if p.is_file() and p.name!=name: shutil.copyfile(p,report/p.name)
for name in ('result.json','run.log'):
    shutil.copyfile(root/'tests'/name,out/'reports'/('tests-'+name))
for name in ('video-result.json','video-run.log'):
    shutil.copyfile(root/'tests'/name,out/'reports'/name)
if clock_margin:
    shutil.copyfile(root/'tests/clock-margin-result.json',out/'reports/clock-margin-result.json')
    shutil.copytree(root/'diagnostics/chipset-timing',out/'reports/chipset-timing')
    if registered_sdram:
        shutil.copyfile(root/'tests/rom-sdram-result.json',out/'reports/rom-sdram-result.json')
if fast_sdram:
    shutil.copyfile(root/'tests/fast-sdram-result.json',out/'reports/fast-sdram-result.json')
if shared_sdram:
    shutil.copyfile(root/'tests/shared-sdram-result.json',out/'reports/shared-sdram-result.json')
    shutil.copyfile(root/'tests/shared-sdram-build/run.log',out/'reports/shared-sdram-run.log')
if video_probe_enabled:
    shutil.copyfile(root/'board_video_probe.py',out/'board_video_probe.py')
    shutil.copyfile(root/'tests/board-video-probe-result.json',out/'reports/board-video-probe-result.json')
if debug:
    for name in ('debug-result.json','debug.log'):
        shutil.copyfile(root/'tests'/name,out/'reports'/name)
    shutil.copyfile(root/'diagnostics.md',out/'diagnostics.md')
if reset:
    for name in ('reset-result.json','reset.log'):
        shutil.copyfile(root/'tests'/name,out/'reports'/name)
for name in ('README.md','NOTICE.txt','COPYING','upstream.lock.json'):
    shutil.copyfile(root/name,out/name)
for source,target in copies:
    target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,target)
    if sha(source)!=sha(target): raise SystemExit('Staging copy verification failed')
manifest={'version':args.version,'link_version':'0x05010001','video_format':'RGB888','upstream':pin,
    'created_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
    'hardware_tested':False,'uploaded':False,'builds':records,
    'tests':test,'video_tests':video,'rom':{'bytes':len(data),'md5':hashlib.md5(data).hexdigest(),
                       'sha256':sha(rom),'amiga_checksum':'ffffffff','included_in_release':False},
    'card_staging':str(sd),
    'files':{str(p.relative_to(out)):sha(p) for p in out.rglob('*') if p.is_file()}}
if debug:
    build_id=re.search(r"assign debug_words\[0\]=32'h([0-9a-fA-F]+);",top)[1].upper()
    manifest.update(diagnostic_build_id=build_id,debug_tests=debug,purpose='A1200 bring-up with passive CPU diagnostics; boot not yet verified')
if adf:manifest.update(test_disk=adf,adf_simulation=disk_test,purpose='Read-only DF0 ADF boot; Test Kit menu verified in simulation, hardware validation pending')
if reset:manifest.update(reset_tests=reset,reset_fix='Tang S2 pin 87 is active high; synchronized input with pull-down')
if clock_margin:
    fix=('Controller on clk85 falling edge, 32-bit DQ input and output registers on rising edge; SYNC_DELAY 3; original read states; '
         'half-cycle IREG to output timing; single-cycle clk85 to clk28; clk28 to clk85 setup 3/hold 2') if registered_sdram else (
         'SYNC_DELAY 3; default single-cycle clk85 to clk28 timing; clk28 to clk85 setup 3/hold 2')
    manifest.update(clock_margin_tests=clock_margin,sdram_timing_fix=fix)
if fast_sdram:
    manifest.update(fast_sdram_tests=fast_sdram,
                    sdram_write_fix='One unconditional output register per DQ pin; inactive half cleared and masked by existing DQM; original command/address timing preserved')
manifest.update(pll_tests=pll,clock28_phase_fix='Upstream PLL: CLKOUTD3 from CLKOUT, CLKOUTP 270 degrees to the SDRAM clock pin; clk28 rising edge aligned with the controller edge (clk85 falling); SDRAM clock 90 degrees after it')
if video_probe_enabled:
    manifest.update(shared_sdram_tests=shared_sdram,
        purpose='SDRAM clock moved 90 degrees earlier against the read window; fixes the yellow Kickstart screen and visual dots on the board; solid RGB probe retained',
        video_probe={'control_byte_address':'0x1a0','signature_byte_address':'0x1a4',
                     'signature':'0x56505231','enable':'0xA5RRGGBB','disable':0,
                     'default':'normal RGB','guest_reset_clears':True})
(out/'manifest.json').write_text(json.dumps(manifest,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
print(json.dumps({'release':str(out),'card_staging':str(sd),'timing':records[0]['timing'],
                  'hardware_tested':False,'uploaded':False},indent=2,ensure_ascii=False))
