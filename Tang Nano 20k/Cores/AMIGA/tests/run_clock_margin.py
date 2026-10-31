"""Regress chipset-return timing across three relative clock-edge orderings."""
from pathlib import Path
import argparse,csv,hashlib,json,shutil,subprocess,sys
ap=argparse.ArgumentParser();ap.add_argument('--verify-existing',action='store_true');ap.add_argument('--label',default='adf1');args=ap.parse_args()
root=Path(__file__).resolve().parents[1];base=root/'diagnostics/chipset-timing'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
orders=['together','28-first','85-first'];results=[];reference=None;hashes=None
def norm(w):
    addr,data,status,meta=w;state=(status>>11)&3;skip=(status>>20)&1
    return addr,state,skip,None if skip else (data&65535 if state==3 else data>>16)
for mode in orders:
    dest=base/(args.label+'-'+mode);dest.mkdir(parents=True,exist_ok=True)
    if not args.verify_existing:
        cmd=[sys.executable,str(root/'tests/run_boot.py'),'--seconds','0.65' if mode=='together' else '0.1','--clock-order',mode]
        if mode!='together':cmd+=['--no-build']
        subprocess.run(cmd,check=True)
        for name in ['pipeline.csv','trace.json','run.log','frame.png']:shutil.copy2(root/'tests/boot-build'/name,dest/name)
        shutil.copy2(root/'tests/boot-result.json',dest/'result.json')
    report=json.loads((dest/'result.json').read_text());trace=json.loads((dest/'trace.json').read_text())
    assert report['clock_order']==mode and all(report[k] for k in ['reset_released','cpu_reset_released','sdram_ready','reset_vectors_match'])
    for p,h in report['source_sha256'].items():assert sha(root/p)==h,p
    if hashes is None:hashes=report['source_sha256']
    else:assert hashes==report['source_sha256'],'Cases used different sources'
    assert len(trace)==512
    assert all(w[3]&(1<<19) and (w[3]&1023)==512 and ((w[3]>>10)&511)==i for i,w in enumerate(trace))
    normalized=list(map(norm,trace))
    if reference is None:reference=normalized
    else:assert normalized==reference,'Clock ordering changed CPU trace'
    rows=list(csv.DictReader((dest/'pipeline.csv').open()));assert rows
    margin=min(int(x['margin_half85']) for x in rows);assert margin>=2
    if mode=='together':assert report['simulated_seconds']>=.65 and report['nonblack_cycles']>0
    results.append({'clock_order':mode,'vectors_match':True,'trace_steps_match':512,'minimum_margin_half85':margin,
                    'minimum_logical_margin_ns':margin*1000/171,'nonblack_cycles':report['nonblack_cycles'],
                    'simulated_seconds':report['simulated_seconds']})
hashes.update({p:sha(root/p) for p in ['fpga/amigaVdac4.sdc','tests/run_clock_margin.py']})
result={'passed':True,'hardware':False,'cases':results,'source_sha256':hashes,
        'scope':'Logical margin from SDRAM publication to chipset capture, not analog measurement; routed STA is checked separately'}
(root/'tests/clock-margin-result.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({k:v for k,v in result.items() if k!='source_sha256'},indent=2))
