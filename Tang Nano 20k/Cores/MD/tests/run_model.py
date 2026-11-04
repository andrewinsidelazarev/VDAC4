# SPDX-License-Identifier: GPL-3.0-or-later
# Модель Mega Drive VDAC4 (03.11.2026): Verilator + MSVC, прогон ROM, кадры в PNG, события звука.
# python run_model.py ROM [--frames N] [--shots 60,120|all|-30] [--pads файл] [--ctrl 0x9] [--stage папка]
#   [--no-build]
# Стадия — на E: (E:\zx\cache\tmp\mdtang\model): obj, exe, кадры, events.bin, stats.json.
import argparse, hashlib, json, os, struct, subprocess, sys, zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ap = argparse.ArgumentParser()
ap.add_argument('rom', type=Path)
ap.add_argument('--frames', type=int, default=120)
ap.add_argument('--shots', default='-30')
ap.add_argument('--pads', default='-')
ap.add_argument('--ctrl', default='0x9')            # работает + экспорт (NTSC США)
ap.add_argument('--stage', type=Path, default=Path(r'E:\zx\cache\tmp\mdtang\model'))
ap.add_argument('--no-build', action='store_true')
ap.add_argument('--ideal', action='store_true', help='память без задержки (для сравнения)')
ap.add_argument('--video', action='store_true', help='с md_video: кадры HDMI 1280x720 (такт 74,25 МГц)')
args = ap.parse_args()

stage = args.stage.with_name(args.stage.name + ('-ideal' if args.ideal else '') + ('-video' if args.video else ''))
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


def run(cmd, name, timeout=1800, cwd=stage):
    print(name, flush=True)
    with (stage / name).open('wb') as log:
        p = subprocess.run(list(map(str, cmd)), cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
    if p.returncode:
        raise SystemExit((stage / name).read_text(encoding='utf-8', errors='replace')[-6000:])


U = ROOT / 'upstream/src'
sources = [ROOT / 'tests/md_model.sv', ROOT / 'tests/sdr_chip_model.v', ROOT / 'fpga/core/system.sv',
           ROOT / 'fpga/md_ym_stub.sv', ROOT / 'fpga/md_snd_events.sv', ROOT / 'fpga/md_sdram.sv',
           ROOT / 'fpga/core/vdp.v', U / 'fx68k/verilator/fx68k.sv', U / 'fx68k/verilator/fx68kAlu.sv',
           U / 'fx68k/verilator/uaddrPla.sv', U / 't80/t80.v', U / 't80/t80_alu.v', U / 't80/t80_mcode.v',
           U / 't80/t80_reg.v', ROOT / 'fpga/core/t80s.v', U / 'common/dpram.v', U / 'common/dpram_block.v',
           U / 'common/dpram32_block.v', U / 'peripherals/multitap.sv', U / 'peripherals/gen_io.sv',
           U / 'peripherals/fourway.v', U / 'peripherals/teamplayer.sv']
if args.video:
    sources += [ROOT / 'fpga/tmds_encoder.sv', ROOT / 'fpga/hdmi_tx.sv', ROOT / 'fpga/md_video.sv', ROOT / 'fpga/nes_osd.sv', ROOT / 'fpga/nes_osd_font.sv',
                U / 'hdmi/tmds_channel.sv', ROOT / 'tests/gowin_stubs.v']
hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources + [ROOT / 'tests/md_model.cpp']}

for n in ('microrom.mem', 'nanorom.mem'):
    (stage / n).write_bytes((U / 'fx68k' / n).read_bytes())

if not args.no_build:
    run([tool / 'bin/verilator_bin.exe', '--cc', '-Wno-fatal', '-Wno-lint', '-Wno-style', '--no-timing',
         '-Wno-BLKANDNBLK', '-Wno-MULTIDRIVEN', '--x-assign', 'fast', '--x-initial', 'fast', '-O3',
         '--top-module', 'md_model', *(['-DIDEAL_MEM'] if args.ideal else []), *(['-DWITH_VIDEO'] if args.video else []), '--Mdir', 'obj', '-I' + str(U / 'vdp'), '-I' + str(U / 'common'),
         '-I' + str(U / 'fx68k'), *sources], 'verilator.log')
    inc = tool / 'share/verilator/include'
    command = ['/nologo', '/EHsc', '/std:c++20', '/utf-8', '/O2', '/MD', '/DVL_TIME_CONTEXT', '/MP8',
               '/I' + str(inc), '/I' + str(inc / 'vltstd'), '/Iobj', str(ROOT / 'tests/md_model.cpp'),
               *map(str, (stage / 'obj').glob('*.cpp')), str(inc / 'verilated.cpp'),
               str(inc / 'verilated_threads.cpp'), '/Femd_model.exe']
    (stage / 'compile.rsp').write_text(subprocess.list2cmdline(command), encoding='utf-8')
    run([vc / 'bin/Hostx64/x64/cl.exe', '@compile.rsp'], 'compile.log')
    (stage / 'sources.json').write_text(json.dumps(hashes, indent=2) + '\n')
else:
    assert json.loads((stage / 'sources.json').read_text()) == hashes, 'исходники изменились — нужна сборка'

# ROM -> слова SDRAM: слово k = {байт 4k+2, 4k+3, 4k, 4k+1} (два слова 68000, младшее — по чётному адресу)
rom = args.rom.read_bytes()
if rom[0x100:0x104] not in (b'SEGA', b' SEG'):
    print('предупреждение: нет SEGA в заголовке (SMD?)')
pad = rom + bytes((-len(rom)) % 4)
with (stage / 'rom.hex').open('w') as f:
    f.write('@0\n')
    for k in range(0, len(pad), 4):
        b = pad[k:k + 4]
        f.write('%02x%02x%02x%02x\n' % (b[2], b[3], b[0], b[1]))
out = stage / 'run'
out.mkdir(exist_ok=True)
for p in [*out.glob('frame_*'), *out.glob('hdmi_*')]:
    p.unlink()
pads = str(Path(args.pads).resolve()) if args.pads != '-' else '-'
if args.video:
    env['MD_VIDEO'] = '1'
run([stage / 'md_model.exe', args.frames, out, len(rom), int(args.ctrl, 0), pads, args.shots], 'run.log', 7200)


def ppm_to_png(src, dst):
    data = src.read_bytes()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    raw = b''.join(b'\x00' + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    dst.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                    chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


for p in sorted([*out.glob('frame_*.ppm'), *out.glob('hdmi_*.ppm')]):
    ppm_to_png(p, p.with_suffix('.png'))
    p.unlink()
print((stage / 'run.log').read_text(encoding='utf-8', errors='replace')[-600:])
