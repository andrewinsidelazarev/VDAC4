"""Check electrical constraints that RTL cannot represent."""
from pathlib import Path
import argparse,json,re
from build import HERE,PROJECT,sha
PORTS=['O_tmds_clk_p',*[f'O_tmds_data_p[{i}]' for i in range(3)]]
def check(profile='current',path=None):
    p=Path(path) if path else (HERE/'reference/0.11' if profile=='stable' else PROJECT)/'fpga/amigaVdac4.cst'
    declarations={}
    for line in p.read_text().splitlines():
        match=re.match(r'\s*IO_PORT\s+"([^"]+)"\s+([^;]+);',line)
        if match and match[1] in PORTS:
            drive=re.search(r'\bDRIVE\s*=\s*(\d+)\b',match[2])
            if match[1] in declarations:raise RuntimeError('Duplicate TMDS constraint: '+match[1])
            declarations[match[1]]=int(drive[1]) if drive else None
    result={'passed':all(declarations.get(n)==4 for n in PORTS),'file':str(p),'sha256':sha(p),'TMDS_DRIVE':declarations}
    if not result['passed']:raise RuntimeError('TMDS DRIVE must be 4 on all four pairs: '+json.dumps(result))
    return result
if __name__=='__main__':
    a=argparse.ArgumentParser();a.add_argument('--profile',choices=['stable','current'],default='current');args=a.parse_args();print(json.dumps(check(args.profile),indent=2))
