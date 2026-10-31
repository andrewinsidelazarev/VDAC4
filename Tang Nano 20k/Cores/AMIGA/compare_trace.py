"""Compare observed CPU memory-state steps with a frozen simulation reference."""
from pathlib import Path
import argparse,json
ap=argparse.ArgumentParser();ap.add_argument('hardware',type=Path);ap.add_argument('reference',type=Path)
ap.add_argument('--rom',type=Path);args=ap.parse_args()
hardware=json.loads(args.hardware.read_text());reference=json.loads(args.reference.read_text())
assert hardware['complete'] and hardware['trace_start']==0
actual=hardware['entries'];assert len(actual)==len(reference)
rom=args.rom.read_bytes() if args.rom else None
def meaningful(w):
    addr,data,status,meta=w
    state=(status>>11)&3;skip=bool(status&(1<<20))
    result={'address':f'{addr:08X}','state':state,'skip_fetch':skip}
    if not skip:result['write' if state==3 else 'read']=f'{(data&65535) if state==3 else (data>>16):04X}'
    return result
differences=[];bad_rom=[];rom_reads=0
for i,(a,b) in enumerate(zip(actual,reference)):
    assert a[3]&(1<<19) and ((a[3]>>10)&511)==i,'Invalid trace metadata'
    va,vb=meaningful(a),meaningful(b)
    if va!=vb:differences.append({'index':i,'actual':va,'reference':vb})
    if rom is not None and 'read' in va and 0xf80000<=a[0]<0x1000000 and not(a[0]&1):
        rom_reads+=1;offset=a[0]-0xf80000;expected=int.from_bytes(rom[offset:offset+2],'big')
        if a[1]>>16!=expected:bad_rom.append({'index':i,'address':va['address'],'actual':va['read'],'rom':f'{expected:04X}'})
report={'entries':len(actual),'matching_steps':len(actual)-len(differences),
        'first_divergence':differences[0] if differences else None,'divergences':differences,
        'rom_reads_checked':rom_reads,'rom_read_mismatches':bad_rom}
out=args.hardware.with_suffix('.comparison.json');out.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({**{k:v for k,v in report.items() if k not in ['divergences','rom_read_mismatches']},
                  'first_rom_read_mismatches':bad_rom[:12],'saved':str(out)},indent=2))
