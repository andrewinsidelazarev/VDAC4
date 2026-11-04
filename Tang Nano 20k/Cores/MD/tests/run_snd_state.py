# SPDX-License-Identifier: GPL-3.0-or-later
# Снимок звука F5/F6 на ПК (03.11.2026): snd_state_test.cpp + движок ESP (esp/main/md_sound.cpp, ymfm), MSVC.
# python run_snd_state.py events.bin [блоков] [--stage папка]   (стадия — E:\zx\cache\tmp\mdtang\snd_state)
import argparse, os, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ap = argparse.ArgumentParser()
ap.add_argument('events', type=Path)
ap.add_argument('blocks', nargs='?', default='64')
ap.add_argument('--stage', type=Path, default=Path(r'E:\zx\cache\tmp\mdtang\snd_state'))
args = ap.parse_args()
stage = args.stage
stage.mkdir(parents=True, exist_ok=True)
vc = Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207')
sdk = Path(r'C:\Program Files (x86)\Windows Kits\10'); v = '10.0.26100.0'
env = os.environ.copy()
env['PATH'] = str(vc / 'bin/Hostx64/x64') + ';' + env['PATH']
env['INCLUDE'] = ';'.join(map(str, [vc / 'include', *[sdk / 'Include' / v / x for x in ['ucrt', 'shared', 'um']]]))
env['LIB'] = ';'.join(map(str, [vc / 'lib/x64', *[sdk / 'Lib' / v / x / 'x64' for x in ['ucrt', 'um']]]))
env['TMP'] = env['TEMP'] = r'E:\zx\cache\tmp'
E = ROOT / 'esp/main'
cmd = [vc / 'bin/Hostx64/x64/cl.exe', '/nologo', '/EHsc', '/std:c++20', '/utf-8', '/O2', '/MD', '/DNDEBUG', '/w',
       '/I' + str(E), '/I' + str(E / 'ymfm'), ROOT / 'tests/snd_state_test.cpp', E / 'md_sound.cpp',
       E / 'ymfm/ymfm_opn.cpp', E / 'ymfm/ymfm_adpcm.cpp', E / 'ymfm/ymfm_ssg.cpp', '/Fesnd_state_test.exe']
p = subprocess.run(list(map(str, cmd)), cwd=stage, env=env, capture_output=True, text=True, encoding='cp866', errors='replace')
if p.returncode:
    sys.exit(p.stdout[-4000:] + p.stderr[-2000:])
r = subprocess.run([str(stage / 'snd_state_test.exe'), str(args.events), args.blocks], cwd=stage, capture_output=True)
sys.stdout.buffer.write(r.stdout)
sys.exit(r.returncode)
