"""Copy the reviewed RGB888 release and user ROM to the specified Tang SD.
Uses the existing verified SDPUT protocol; does not flash or start a core.
"""
from pathlib import Path
import argparse, contextlib, datetime, functools, hashlib, importlib.util, io, json, re, subprocess, sys
root=Path(r'E:\zx\Tang Nano 20k VDAC\VDAC4\Tang Nano 20k\Cores\AMIGA')
tool=root.parents[2]/'ESP32 S3 Zero N8R8/Firmware/tools/tang_tool.py'
spec=importlib.util.spec_from_file_location('tang_tool',tool)
t=importlib.util.module_from_spec(spec);spec.loader.exec_module(t)
t.CONNECT_TIMEOUT=10
parser=argparse.ArgumentParser()
parser.add_argument('address');parser.add_argument('--version',default='0.2')
parser.add_argument('--only-bin',action='store_true')
parser.add_argument('--skip-rom',action='store_true',help='Keep the already verified Kickstart on SD')
parser.add_argument('--with-adf',action='store_true',help='Install the pinned Amiga Test Kit as AMIGA/DF0.ADF')
parser.add_argument('--source',help='Local LAN address for hosts with overlapping network adapters')
parser.add_argument('--sync-cache',action='store_true',help='After verified SD upload, synchronize only AMIGA in the BL616 core cache')
parser.add_argument('--step-timeout',type=int,default=180,help='Seconds without a reply during SD/cache operations (30..600)')
args=parser.parse_args()
if not 30<=args.step_timeout<=600:parser.error('--step-timeout must be 30..600 seconds')
t.STEP_TIMEOUT=args.step_timeout
assert re.fullmatch(r'\d+(?:\.\d+)+(?:-debug\d+)?',args.version)
address=args.address
if args.source:
    # Per-process socket binding; leaves shared firmware tools and Windows routes unchanged.
    t.socket.create_connection=functools.partial(t.socket.create_connection,source_address=(args.source,0))
t.CONNECT_TIMEOUT=20
release=root/'release'/args.version
manifest=json.loads((release/'manifest.json').read_text(encoding='utf-8'))
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
files=[(release/'CORES/AMIGA.ELF','CORES/AMIGA.ELF'),
       (root/'ROM/kickstart-3.1-a500_a600_a2000.rom','AMIGA/KICK31.ROM'),
       (release/'CORES/AMIGA.BIN','CORES/AMIGA.BIN')]
assert sha(files[0][0])==manifest['builds'][1]['sha256']
assert sha(files[1][0])==manifest['rom']['sha256']
assert sha(files[2][0])==manifest['builds'][0]['sha256']
out=root/'deployment'/args.version;out.mkdir(parents=True,exist_ok=True)
# The installed heat2 firmware exposes SD writes through TS-Config. Confirm it
# before touching files; never implicitly stop a running core.
preflight=[sys.executable,str(Path(__file__).with_name('board_read.py')),address,
           'tang rd 0','tang rd 4','--output',str(out/'preflight.json'),'--connect-timeout','20']
if args.source: preflight+=['--source',args.source]
subprocess.run(preflight,check=True)
identity=json.loads((out/'preflight.json').read_text())
values=[r.get('value') for r in identity if 'command' in r]
assert len(values)==2 and values[0]==0x56444334 and values[1] is not None and values[1]>>24==1, \
       'Return to Wild Commander / TS-Config before updating the SD card'
if args.only_bin: files=files[2:]
elif args.skip_rom: files=[files[0],files[2]]
if args.with_adf:
    disk=json.loads((root/'disks/manifest.json').read_text())
    image=root/'disks'/disk['adf']
    assert image.stat().st_size==disk['bytes'] and sha(image)==disk['sha256']
    files.append((image,disk['target']))
report={'address':address,'release':args.version,'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'files':[],'hardware_execution_tested':False,'flash_written':False}
for source,target in files:
    log=io.StringIO();print('SDPUT '+target,flush=True)
    try:
        with contextlib.redirect_stdout(log): t.sd_put(address,source,target)
    except Exception as e:
        (out/(source.name+'.log')).write_text(log.getvalue(),encoding='utf-8')
        report['error']=str(e);(out/'sd-upload.json').write_text(json.dumps(report,indent=2))
        raise
    (out/(source.name+'.log')).write_text(log.getvalue(),encoding='utf-8')
    report['files'].append({'path':target,'bytes':source.stat().st_size,'sha256':sha(source),'readback_verified':True})
    (out/'sd-upload.json').write_text(json.dumps(report,indent=2))
    print('Read-back SHA-256 verified: '+target,flush=True)
report['complete']=True
(out/'sd-upload.json').write_text(json.dumps(report,indent=2)+'\n')
if args.sync_cache:
    image=release/'CORES/AMIGA.BIN'
    cache={'address':address,'core':'AMIGA','sha256':sha(image),
           'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'confirmed':False}
    log=io.StringIO();print('Synchronizing AMIGA in the BL616 core cache',flush=True)
    try:
        with contextlib.redirect_stdout(log):t.core_put(address,image,'AMIGA')
        cache['confirmed']=True
    except Exception as e:
        cache['error']=str(e)
        raise
    finally:
        (out/'cache-put.log').write_text(log.getvalue(),encoding='utf-8')
        (out/'cache-put.json').write_text(json.dumps(cache,indent=2)+'\n')
    print('AMIGA cache SHA-256 confirmed',flush=True)
print('Requested files copied and verified. Reopen the WC core list.',flush=True)
