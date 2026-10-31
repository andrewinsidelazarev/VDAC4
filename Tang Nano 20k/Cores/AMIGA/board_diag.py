"""Read the passive Amiga bring-up registers; no writes or core changes."""
from pathlib import Path
import argparse, datetime, json, subprocess, sys
parser=argparse.ArgumentParser();parser.add_argument('address');parser.add_argument('--source')
args=parser.parse_args()
root=Path(__file__).resolve().parent
out=root/'diagnostics';out.mkdir(exist_ok=True)
stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%SZ')
prefix=out/stamp
def read(commands,suffix):
    result=prefix.with_suffix(suffix+'.json')
    command=[sys.executable,str(root/'board_read.py'),args.address,*commands,'--output',str(result)]
    if args.source:command+=['--source',args.source]
    subprocess.run(command,check=True,stdout=subprocess.PIPE)
    return json.loads(result.read_text())
identity=read(['tang rd 0','tang rd 4','tang rd 60'],'.identity')
values=[r['value'] for r in identity if 'value' in r]
assert len(values)==3 and values[:2]==[0x56444334,0x05010001] and values[2] in (0x414d4401,0x414d4402,0x414d4403,0x414d4404,0x414d4405),f'Unexpected running core/build: {values}; identity saved at {prefix}'
raw=read([f'tang rd {i:x}' for i in range(0x60,0xe0,4)],'.raw')
words=[r['value'] for r in raw if 'value' in r]
assert len(words)==32 and all(w is not None for w in words),f'Incomplete diagnostics: {prefix}'
top=words[1];cpu=words[4]
report={'utc':stamp,'address':args.address,'build_tag':f'{values[2]:08X}','words':[f'{w:08X}' for w in words],
        'top':{'reset28':bool(top&2),'pll_locked':bool(top&4),'sdram_ready':bool(top&8),
               'rom_ready':bool(top&16),'core_reset':bool(top&32),'soft_reset':bool(top&64),
               'keyboard_reset':bool(top&128),'button_levels':(top>>8)&3,'warm_reset':(top>>10)&255},
        'video':{'vs_edges':words[2]>>16,'hs_edges':words[2]&65535,'nonblack_cycles':words[3]},
        'cpu':{'reset_n':bool(cpu&1),'reset_out_n':bool(cpu&2),'rw':bool(cpu&4),
               'as_n':bool(cpu&8),'dtack_n':bool(cpu&16),'overlay':bool(cpu&32),
               'sync':bool(cpu&64),'cacr':(cpu>>7)&15,'state':(cpu>>11)&3,
               'ipl':(cpu>>13)&7,'ram_ready':bool(cpu&(1<<18)),'ram_sel':bool(cpu&(1<<19)),
               'memory_state_cpu_steps':words[5],'address':f'{words[6]:08X}',
               'last_address':f'{words[8]:08X}','last_read':f'{words[9]>>16:04X}'},
        'first_cycles':[{'address':f'{words[i]:08X}','read':f'{words[i+1]>>16:04X}',
                         'write':f'{words[i+1]&65535:04X}'} for i in range(12,28,2)],
        'rom_write_pulses':words[28]}
prefix.with_suffix('.decoded.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
print(f'Saved: {prefix}.decoded.json')
