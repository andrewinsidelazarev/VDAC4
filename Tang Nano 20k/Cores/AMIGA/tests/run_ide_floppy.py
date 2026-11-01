"""Дискета и диск одновременно, настоящая цепочка ядра: 68000 -> Paula/DMA дискеты и Gayle/IDE -> nanomig -> amiga_disk.

Вместо Kickstart в ROM лежит программа: запускает чтение дорожки по DMA и, пока оно идёт, пишет на диск блоками WRITE MULTIPLE,
затем читает то же блоками READ MULTIPLE и сверяет сама. Все слова разные (счётчик), поэтому потерянный сектор виден точно.
Ящик у дискеты и диска один. Ошибка ядра 0.9: IDE принимал занятость ящика дискетой за подтверждение своего запроса, снимал запрос
и по чужому done считал сектор записанным; на плате в файле Prefs/Palette на HD02 сектор остался нулевым (ошибка #8000000B).
"""
from pathlib import Path
import argparse, struct, subprocess, sys

root = Path(__file__).resolve().parent
stage = root / 'boot-build'
ap = argparse.ArgumentParser()
ap.add_argument('--adf', type=Path, required=True)
ap.add_argument('--blocks', type=int, default=16)
ap.add_argument('--seconds', type=float, default=0.8)
ap.add_argument('--no-build', action='store_true')
ap.add_argument('--single', action='store_true', help='WRITE SECTORS: буфер заново на каждый сектор, вместо WRITE MULTIPLE')
args = ap.parse_args()
hdf = root / 'build/new.hdf'
SPB, CYLINDER, REPORT_CYL = 16, 5, 100

W = lambda *words: b''.join(bytes.fromhex(x) for x in words)
wait_ready = W('1028001C', '6BFA')                          # move.b $1C(a0),d0 / bmi.s
wait_drq = W('1028001C', '08000003', '67F6')                # move.b $1C(a0),d0 / btst #3,d0 / beq.s
start_dma = W('257C000100000020',                           # move.l #$10000,$20(a2)   DSKPT
              '357C40000024', '357CBFFF0024', '357CBFFF0024')  # DSKLEN: 0, затем дважды запуск на $3FFF слов
registers = lambda command: W('1147' '0010', '117C00A00018', f'117C{SPB:04X}0008', '117C0001000C', '117C00000014', f'117C{command:04X}001C')
restart_floppy = (W('302A001E', '08000001') +               # move.w $1E(a2),d0 / btst #1,d0   DSKBLK
                  bytes.fromhex('67') + bytes([6 + 2 + len(start_dma)]) +  # beq.s дальше
                  W('357C0002009C', '5245') + start_dma)    # снять запрос / addq.w #1,d5 / снова DMA

code = bytearray()
code += W('13FC000300BFE201', '13FC000200BFE001')           # OVL снят: чип-память по адресу 0
code += W('41F900DA0000', '45F900DFF000', '47F900BFD100')   # a0 IDE, a2 custom, a3 CIAB PRB
code += wait_ready
code += W('177C00FF0200', '16BC00FF', '16BC007F', '16BC0077')  # DDRB; мотор, DF0
code += W('357C82100096', '357C7F00009E', '357C9500009E', '357C4489007E')  # DMACON, ADKCON, DSKSYNC
code += start_dma
code += W('117C00A00018', f'117C{SPB:04X}0008', '117C00C6001C') + wait_ready  # SET MULTIPLE
code += W(f'76{args.blocks - 1:02X}', '7800', '7A00', '7C00', f'7E{CYLINDER:02X}')  # d3 блоки, d4 счётчик, d5 DMA, d6 ошибки, d7 цилиндр
loop = len(code)
if args.single:
    code += registers(0x30) + W(f'72{SPB - 1:02X}')         # d1 — секторы команды, DRQ на каждый
    sector = len(code)
    code += wait_drq + W('343C00FF', '3084', '5244', '51CAFFFA') + wait_ready
    code += W('51C9') + struct.pack('>h', sector - (len(code) + 2))
    code += W('5207') + restart_floppy
else:
    code += registers(0xC5) + wait_drq + W('343C0FFF', '3084', '5244', '51CAFFFA') + wait_ready + W('5207') + restart_floppy
code += W('51CB') + struct.pack('>h', loop - (len(code) + 2))
code += W(f'76{args.blocks - 1:02X}', '7800', f'7E{CYLINDER:02X}')
loop = len(code)
code += registers(0xC4) + wait_drq + W('343C0FFF', 'B850', '6702', '5246', '5244', '51CAFFF6') + wait_ready + W('5207') + restart_floppy
code += W('51CB') + struct.pack('>h', loop - (len(code) + 2))
# Итог: сектор с числом законченных DMA дискеты, числом неверных слов чтения и меткой.
code += W(f'117C{REPORT_CYL:04X}0010', '117C00A00018', '117C00010008', '117C0001000C', '117C00000014', '117C0030001C') + wait_drq
code += W('3085', '3086', '30BCCAFE', '343C00FC', '3084', '51CAFFFC') + wait_ready + W('60FE')
rom = bytearray(b'\xff' * 524288)
rom[:8] = struct.pack('>II', 0x00080000, 0x00F80008)
rom[8:8 + len(code)] = code
path = stage / 'ide_floppy.rom'
stage.mkdir(exist_ok=True)
path.write_bytes(rom)

disk = hdf.open('rb').read(512)
assert disk[:4] == b'RDSK', hdf
sectors, heads = struct.unpack_from('>II', disk, 17 * 4)
command = [sys.executable, str(root / 'run_boot.py'), '--seconds', str(args.seconds), '--rom', str(path), '--adf', str(args.adf), '--hdf', str(hdf)]
if args.no_build:
    command.append('--no-build')
run = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, errors='replace')
if run.returncode:
    print('модель остановлена с ошибкой:', run.stderr.strip().splitlines()[-1] if run.stderr.strip() else run.returncode)
log = (stage / 'run.log').read_text(encoding='utf-8', errors='replace')
print('\n'.join(line for line in log.splitlines() if line.startswith(('HDF sectors', 'FAIL', 'first read'))))

written = (stage / 'written.bin').read_bytes()
records = [written[n:n + 516] for n in range(0, len(written), 516)]
want = {}
for block in range(args.blocks):
    for s in range(SPB):
        lba = (CYLINDER + block) * heads * sectors + s
        first = (block * SPB + s) * 256
        want[lba] = b''.join(struct.pack('>H', (first + i) & 0xFFFF) for i in range(256))
got = {}
report = None
for record in records:
    number = struct.unpack_from('<I', record)[0]
    if number == REPORT_CYL * heads * sectors:
        report = struct.unpack_from('>HHH', record, 4)
    else:
        got[number] = record[4:]
lost = [n for n in want if n not in got]
wrong = [n for n in want if n in got and got[n] != want[n]]
extra = [n for n in got if n not in want]
print(f'записей {len(records)}, ожидалось секторов {len(want)} + итог; потеряно {len(lost)} {lost[:12]}, неверных {len(wrong)} {wrong[:12]}, лишних {len(extra)} {extra[:12]}')
zero = [n for n in wrong if not any(got[n])]
print(f'из неверных нулевых {len(zero)} {zero[:12]}; соседи нулевых верны: {all(got.get(n + 1, want.get(n + 1)) == want.get(n + 1) and got.get(n - 1, want.get(n - 1)) == want.get(n - 1) for n in zero)}')
if wrong:
    n = wrong[0]
    bad = [i for i in range(512) if got[n][i] != want[n][i]]
    print(f'  сектор {n}: неверных байт {len(bad)}, было {got[n][:8].hex()}, надо {want[n][:8].hex()}')
served = log.count('\n')
if report:
    print(f'итог программы: DMA дискеты закончено {report[0]}, неверных слов при чтении {report[1]}, метка {report[2]:04X}')
else:
    print('итог программы не записан: программа не дошла до конца')
ok = run.returncode == 0 and not lost and not wrong and not extra and report and report[0] >= 1 and report[1] == 0 and report[2] == 0xCAFE
print('PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
