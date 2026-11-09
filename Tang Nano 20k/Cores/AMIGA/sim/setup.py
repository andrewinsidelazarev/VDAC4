"""Install the pinned VHDL frontend locally and check existing Windows tools."""
from pathlib import Path
import hashlib,io,subprocess,urllib.request,zipfile
from build import HERE,GHDL,RTL_TOOLS,GOWIN,VC,SDK,SDK_VERSION
URL='https://github.com/ghdl/ghdl/releases/download/v6.0.0/ghdl-mcode-6.0.0-ucrt64.zip'
SHA='76e160ceec35834c73ada6e4e484416d13aed4b64cbc7c74c5cca53a7ef60e41'
if not GHDL.exists():
    data=urllib.request.urlopen(URL,timeout=120).read()
    if hashlib.sha256(data).hexdigest()!=SHA:raise RuntimeError('GHDL archive hash mismatch')
    target=HERE/'.tools/ghdl-6.0.0';target.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for n in z.namelist():
            if not (target/n).resolve().is_relative_to(target.resolve()):raise RuntimeError('Unsafe ZIP path')
        z.extractall(target)
for p in [GHDL,RTL_TOOLS/'bin/verilator_bin.exe',RTL_TOOLS/'bin/iverilog.exe',RTL_TOOLS/'bin/vvp.exe',GOWIN/'simlib/gw2a/prim_sim.v',VC/'bin/Hostx64/x64/cl.exe',SDK/'Include'/SDK_VERSION/'um/Windows.h']:
    if not p.is_file():raise RuntimeError('Missing dependency: '+str(p))
from PIL import Image
print('Tools ready. Start Amiga Simulator.cmd')
