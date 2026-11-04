# SPDX-License-Identifier: GPL-3.0-or-later
# Стенд окна и масштаба md_video (pic_top.sv + pic_check.cpp): Verilator + MSVC.
# python run_pic.py [--stage папка]   [picture_size кадров]  (стадия — E:/zx/cache/tmp/mdtang/pic)
import argparse, os, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ap = argparse.ArgumentParser()
ap.add_argument('--stage', type=Path, default=Path('E:/zx/cache/tmp/mdtang/pic'))
args, _rest = ap.parse_known_args(); sys.argv = [sys.argv[0]] + _rest
stage = args.stage
stage.mkdir(parents=True, exist_ok=True)
tool = Path('E:/zx/tools/rtl/ucrt64')
vc = Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207')
sdk = Path('C:/Program Files (x86)/Windows Kits/10'); v = '10.0.26100.0'
env = os.environ.copy()
env['PATH'] = str(tool / 'bin') + ';' + str(vc / 'bin/Hostx64/x64') + ';' + env['PATH']
env['VERILATOR_ROOT'] = str(tool / 'share/verilator')
env['INCLUDE'] = ';'.join(map(str, [vc / 'include', *[sdk / 'Include' / v / x for x in ['ucrt', 'shared', 'um']]]))
env['LIB'] = ';'.join(map(str, [vc / 'lib/x64', *[sdk / 'Lib' / v / x / 'x64' for x in ['ucrt', 'um']]]))
env['TMP'] = env['TEMP'] = 'E:/zx/cache/tmp'


def run(cmd, name, timeout=1800):
    print(name, flush=True)
    with (stage / name).open('wb') as log:
        p = subprocess.run(list(map(str, cmd)), cwd=stage, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
    if p.returncode:
        raise SystemExit((stage / name).read_text(encoding='utf-8', errors='replace')[-4000:])


F = ROOT / 'fpga'
sources = [ROOT / 'tests/pic_top.sv', F / 'tmds_encoder.sv', F / 'hdmi_tx.sv', F / 'md_video.sv', F / 'nes_osd.sv', F / 'nes_osd_font.sv',
           ROOT / 'upstream/src/hdmi/tmds_channel.sv', ROOT / 'tests/gowin_stubs.v']
run([tool / 'bin/verilator_bin.exe', '--cc', '-Wno-fatal', '-Wno-lint', '-Wno-style', '--no-timing',
     '-Wno-MULTIDRIVEN', '--top-module', 'pic_top', '--Mdir', 'obj', *sources], 'verilator.log')
inc = tool / 'share/verilator/include'
command = ['/nologo', '/EHsc', '/std:c++20', '/utf-8', '/O2', '/MD', '/DVL_TIME_CONTEXT', '/MP8',
           '/I' + str(inc), '/I' + str(inc / 'vltstd'), '/Iobj', str(ROOT / 'tests/pic_check.cpp'),
           *map(str, (stage / 'obj').glob('*.cpp')), str(inc / 'verilated.cpp'), str(inc / 'verilated_threads.cpp'),
           '/Fepic_check.exe']
(stage / 'compile.rsp').write_text(subprocess.list2cmdline(command), encoding='utf-8')
run([vc / 'bin/Hostx64/x64/cl.exe', '@compile.rsp'], 'compile.log')
p = subprocess.run([str(stage / 'pic_check.exe'), *sys.argv[1:]], cwd=stage, env=env, capture_output=True)
print(p.stdout.decode('utf-8', 'replace')[-3000:])
raise SystemExit(p.returncode)
