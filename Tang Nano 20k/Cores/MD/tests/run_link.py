# SPDX-License-Identifier: GPL-3.0-or-later
# Стенд LINK ядра Mega Drive (md_link_top.sv + md_link_test.cpp): Verilator + MSVC.
# python run_link.py [--stage папка]   (стадия на E: — E:\zx\cache\tmp\mdtang\link)
import argparse, os, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ap = argparse.ArgumentParser()
ap.add_argument('--stage', type=Path, default=Path(r'E:\zx\cache\tmp\mdtang\link'))
args = ap.parse_args()
stage = args.stage
stage.mkdir(parents=True, exist_ok=True)
tool = Path(r'E:\zx\tools\rtl\ucrt64')
vc = Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207')
sdk = Path(r'C:\Program Files (x86)\Windows Kits\10'); v = '10.0.26100.0'
env = os.environ.copy()
env['PATH'] = str(tool / 'bin') + ';' + str(vc / 'bin/Hostx64/x64') + ';' + env['PATH']
env['VERILATOR_ROOT'] = str(tool / 'share/verilator')
env['INCLUDE'] = ';'.join(map(str, [vc / 'include', *[sdk / 'Include' / v / x for x in ['ucrt', 'shared', 'um']]]))
env['LIB'] = ';'.join(map(str, [vc / 'lib/x64', *[sdk / 'Lib' / v / x / 'x64' for x in ['ucrt', 'um']]]))
env['TMP'] = env['TEMP'] = r'E:\zx\cache\tmp'


def run(cmd, name, timeout=1800):
    print(name, flush=True)
    with (stage / name).open('wb') as log:
        p = subprocess.run(list(map(str, cmd)), cwd=stage, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
    if p.returncode:
        raise SystemExit((stage / name).read_text(encoding='utf-8', errors='replace')[-4000:])


F = ROOT / 'fpga'
sources = [ROOT / 'tests/md_link_top.sv', F / 'md_link.sv', F / 'next_qspi.sv', F / 'fb_sdspi.sv', F / 'md_sdram.sv',
           ROOT / 'tests/sdr_chip_model.v']
run([tool / 'bin/verilator_bin.exe', '--cc', '-Wno-fatal', '-Wno-lint', '-Wno-style', '--no-timing',
     '-Wno-MULTIDRIVEN', '--top-module', 'md_link_top', '--Mdir', 'obj', *sources], 'verilator.log')
inc = tool / 'share/verilator/include'
command = ['/nologo', '/EHsc', '/std:c++20', '/utf-8', '/O2', '/MD', '/DVL_TIME_CONTEXT', '/MP8',
           '/I' + str(inc), '/I' + str(inc / 'vltstd'), '/Iobj', str(ROOT / 'tests/md_link_test.cpp'),
           *map(str, (stage / 'obj').glob('*.cpp')), str(inc / 'verilated.cpp'), str(inc / 'verilated_threads.cpp'),
           '/Femd_link_test.exe']
(stage / 'compile.rsp').write_text(subprocess.list2cmdline(command), encoding='utf-8')
run([vc / 'bin/Hostx64/x64/cl.exe', '@compile.rsp'], 'compile.log')
run([stage / 'md_link_test.exe'], 'run.log', 3600)
print((stage / 'run.log').read_text(encoding='utf-8', errors='replace')[-1500:])
