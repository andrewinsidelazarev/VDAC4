"""Том EVO на настоящем Kickstart: модель ядра (run_boot.py) с двумя дисками, оба собраны кодом ESP (evo_vol.h).

Диск 0 — загрузочный: FAT-образ с S/startup-sequence и командами C/Type, C/Dir (взять с дискеты Workbench 3.1, в проект не
кладутся), app_test.exe собирает из него том FFS, здесь в дампе раздел становится загрузочным DH0, том — BOOT. Диск 1 — том EVO
из тестового образа make_fat.py (FAT32). Kickstart грузится с диска 0, startup-sequence печатает файлы с EVO: и список корня;
итог — экран модели (frame.png) и запросы к диску 1 в disk.csv: корень тома EVO и сектор hello.txt должны быть прочитаны.

RDB диска 0 — такой, каким его видит Amiga через AMIGA.ELF 0.18: без флага «последний диск» (ev_rdb_view снимает его у образов
из меню F11, app_test это проверяет). --last оставляет флаг, как в файле образа: Kickstart 3.1 тогда диск 1 не ищет вовсе,
startup-sequence останавливается на «Please insert volume EVO» (так найдена причина 02.11.2026).

    run_evo_boot.py --commands DIR_WITH_Type_AND_Dir [--seconds 10] [--last] [--no-build]
"""
from pathlib import Path
import argparse, csv, os, shutil, struct, subprocess, sys

root = Path(__file__).resolve().parents[1]
tests = root / 'tests'
ap = argparse.ArgumentParser()
ap.add_argument('--commands', type=Path, required=True, help='каталог с Type и Dir из C: Workbench 3.1')
ap.add_argument('--seconds', type=float, default=10)
ap.add_argument('--stage', type=Path, default=Path(r'E:\zx\cache\tmp\amiga-evosd\boot'))
ap.add_argument('--last', action='store_true', help='RDB диска 0 с флагом «последний диск», как в файле образа из меню F11')
ap.add_argument('--no-build', action='store_true')
args = ap.parse_args()
stage = args.stage
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
if stage.exists():
    shutil.rmtree(stage)
tree = stage / 'tree'
(tree / 'S').mkdir(parents=True)
(tree / 'C').mkdir()
for name in ('Type', 'Dir'):
    src = next(p for p in args.commands.iterdir() if p.name.lower() in (name.lower(), 'c__' + name.lower()))
    shutil.copyfile(src, tree / 'C' / name)
(tree / 'S' / 'startup-sequence').write_bytes(
    b'Echo "STARTUP FROM THE FFS VIEW OF FAT"\nType EVO:hello.txt\nType EVO:Archives/stored2/a/b/c.txt\n'
    b'Type EVO:Archives/stored0/readme.txt\nDir EVO:Archives\nEcho "END OF TEST"\n')
py = sys.executable
subprocess.run([py, '-I', str(tests / 'make_fat.py'), str(stage / 'boot.img'), '--fat16', '--tree', str(tree)], check=True)
lha = Path(r'E:\zx\esp\downloads\libxmp\test-dev\data')
samples = stage / 'lha'
samples.mkdir()
for n in ('l1_lzhuff5', 'l2_lzhuff7'):                  # сжатые архивы — каталогами рядом со своими .lha
    if (lha / n).exists():
        shutil.copyfile(lha / n, samples / n)
subprocess.run([py, '-I', str(tests / 'make_fat.py'), str(stage / 'evo.img'), '--lha', str(samples)], check=True)

vc = Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207')
sdk = Path(r'C:\Program Files (x86)\Windows Kits\10'); v = '10.0.26100.0'
env = os.environ.copy()
env['PATH'] = str(vc / 'bin/Hostx64/x64') + ';' + env['PATH']
env['INCLUDE'] = ';'.join(map(str, [vc / 'include', *[sdk / 'Include' / v / x for x in ['ucrt', 'shared', 'um']]]))
env['LIB'] = ';'.join(map(str, [vc / 'lib/x64', *[sdk / 'Lib' / v / x / 'x64' for x in ['ucrt', 'um']]]))
subprocess.run([str(vc / 'bin/Hostx64/x64/cl.exe'), '/nologo', '/TC', '/std:c11', '/utf-8', '/O2', '/MD',
                '/I' + str(root / 'esp/main'), str(tests / 'app_test.c'), str(root / 'esp/main/ps2_hid.c'), '/Feapp_test.exe'],
               cwd=stage, env=env, check=True, stdout=subprocess.DEVNULL)
env.update(EVO_FAT=str(stage / 'boot.img'), EVO_DUMP=str(stage / 'boot.dump'),
           EVO_FAT2=str(stage / 'evo.img'), EVO_DUMP2=str(stage / 'evo.dump'))
subprocess.run([str(stage / 'app_test.exe'), str(root / 'ROM/kickstart-3.1-a500_a600_a2000.rom'),
                *map(str, sorted((tests / 'build').glob('*.zip')))], cwd=stage, env=env, check=True)


def blocks(path):
    data = path.read_bytes()
    return {struct.unpack_from('<I', data, i)[0]: bytearray(data[i + 4:i + 516]) for i in range(0, len(data), 516)}


def save(path, bl):
    path.write_bytes(b''.join(struct.pack('<I', k) + bytes(b) for k, b in sorted(bl.items())))


def checksum(b, at, words):
    struct.pack_into('>I', b, at * 4, 0)
    struct.pack_into('>I', b, at * 4, -sum(struct.unpack_from(f'>{words}I', b)) & 0xFFFFFFFF)


CYL = 16 * 63
PART = 3900 * CYL
ROOT = (PART - 1 + 2) // 2
boot = blocks(stage / 'boot.dump')
struct.pack_into('>I', boot[0], 5 * 4, 7 if args.last else 6)             # флаги RDB
checksum(boot[0], 2, 64)
struct.pack_into('>I', boot[1], 5 * 4, 1)                                   # раздел загрузочный
boot[1][36:40] = b'\x03DH0'
struct.pack_into('>i', boot[1], 47 * 4, 0)
checksum(boot[1], 2, 64)
rb = boot[CYL + ROOT]
rb[0x1B0:0x1B5] = b'\x04BOOT'
checksum(rb, 5, 128)
assert all(len(b) == 512 for b in boot.values())
save(stage / 'boot.dump', boot)
evo = blocks(stage / 'evo.dump')
hello = [k for k, b in evo.items() if b.startswith(b'EVO VOLUME OK\n')]
nested = [k for k, b in evo.items() if b.startswith(b'level 2, nested\n')]   # файл внутри архива stored2.lha
assert len(hello) == 1 and len(nested) == 1, (hello, nested)

run = subprocess.run([py, str(tests / 'run_boot.py'), '--seconds', str(args.seconds), '--hdf', str(stage / 'boot.dump'),
                      '--hdf1', str(stage / 'evo.dump')] + (['--no-build'] if args.no_build else []))
build = tests / 'boot-build'
for name in ('disk.csv', 'frame.png', 'run.log', 'boot.json'):
    if (build / name).exists():
        shutil.copyfile(build / name, stage / name)
with (stage / 'disk.csv').open() as f:
    rows = [r for r in csv.DictReader(f)]
evo_reads = [int(r['sector']) for r in rows if r['drive'] == '32']
boot_reads = [int(r['sector']) for r in rows if r['drive'] == '16']
print(f'диск 0 (BOOT): прочитано {len(boot_reads)}; диск 1 (EVO): {len(evo_reads)}; корень EVO {"прочитан" if CYL + ROOT in evo_reads else "НЕ прочитан"}; '
      f'hello.txt {"прочитан" if hello[0] in evo_reads else "НЕ прочитан"}; экран: {stage / "frame.png"}')
print(f'файл из архива stored2.lha {"прочитан" if nested[0] in evo_reads else "НЕ прочитан"}')
ok = run.returncode == 0 and CYL + ROOT in evo_reads and hello[0] in evo_reads and nested[0] in evo_reads
print('PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
