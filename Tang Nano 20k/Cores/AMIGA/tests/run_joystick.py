"""Джойстик через настоящую цепочку ядра: пакет HID 3 (как hid_packet у AMIGA.ELF) -> hid -> nanomig -> minimig/userio -> регистры.

Вместо Kickstart в ROM лежит программа 68000. Она читает порты так же, как игра: JOY1DAT/JOY0DAT, CIAA PRA (огонь 1 портов 2 и 1),
POTINP (огонь 2), ждёт изменения, выдерживает паузу и пишет состояние сектором на диск; так два раза. Модель шлёт два пакета:
вверх+вправо+огонь 1, затем вниз+влево+огонь 2. Игровой порт Amiga — порт 2 (JOY1DAT, PRA бит 7, POTINP бит 14).

userio при joy_swap=0 перекрещивает входы: joystick0 (устройство HID 0) -> _joy1 -> порт 2. AMIGA.ELF до 0.16 слал устройство 1,
и джойстик попадал в порт 1 (мышиный): на плате клавиши назначались, а игры их не видели. `--device 1` показывает ту ошибку.
"""
from pathlib import Path
import argparse, os, struct, subprocess, sys

root = Path(__file__).resolve().parent
stage = root / 'boot-build'
ap = argparse.ArgumentParser()
ap.add_argument('--device', type=int, default=0, help='устройство HID в пакете: 0 — как AMIGA.ELF 0.16, 1 — как до 0.16')
ap.add_argument('--seconds', type=float, default=0.3)
ap.add_argument('--no-build', action='store_true')
args = ap.parse_args()
hdf = root / 'build/new.hdf'
REPORT_CYL, PHASES = 100, 2
UP, DOWN, LEFT, RIGHT, FIRE1, FIRE2 = 8, 4, 2, 1, 16, 32       # биты байта джойстика NanoMig, как ja_bit у AMIGA.ELF
EVENTS = ((0.15, UP | RIGHT | FIRE1), (0.2, DOWN | LEFT | FIRE2))   # процессор модели стартует около 0,08 с
# Ожидаемый ключ: старшее слово — порт 1 (JOY0DAT&0303), младшее — порт 2 (JOY1DAT&0303) | PRA&C0 | POTINP&4400.
# Вправо: JOY1DAT бит 1, вниз: бит 1 ^ бит 0; влево: бит 9, вверх: бит 9 ^ бит 8. Кнопки активны нулём:
# PRA бит 7 — огонь 1 порта 2, бит 6 — порта 1; POTINP бит 14 — огонь 2 порта 2, бит 10 — порта 1.
WANT = (0x00000103 | 0x40 | 0x4400,    # вверх+вправо, огонь 1 порта 2 нажат, огонь 2 отпущен
        0x00000301 | 0xC0 | 0x0400)    # вниз+влево, огонь 1 отпущен, огонь 2 порта 2 нажат

W = lambda *words: b''.join(bytes.fromhex(x) for x in words)
wait_ready = W('1028001C', '6BFA')                          # move.b $1C(a0),d0 / bmi.s
wait_drq = W('1028001C', '08000003', '67F6')                # move.b $1C(a0),d0 / btst #3,d0 / beq.s
sample = W('302A000A', '02400303', '4840',                  # move.w JOY0DAT,d0 / and.w #$0303 / swap
           '302A000C', '02400303',                          # move.w JOY1DAT,d0 / and.w #$0303
           '123900BFE001', '024100C0', '8041',              # move.b CIAA PRA,d1 / and.w #$C0 / or.w d1,d0
           '322A0016', '02414400', '8041')                  # move.w POTINP,d1 / and.w #$4400 / or.w d1,d0
code = bytearray()
code += W('13FC000300BFE201', '13FC000200BFE001')           # CIAA DDRA, OVL снят
code += W('41F900DA0000', '45F900DFF000') + wait_ready      # a0 IDE, a2 custom
code += sample + W('2E00', f'7C{REPORT_CYL:02X}')           # d7 прежний ключ, d6 цилиндр итога
phase = len(code)
code += sample + W('B087')                                  # cmp.l d7,d0
code += bytes([0x67, (phase - (len(code) + 2)) & 0xFF])     # beq.s: ждать изменения
code += W('343C00FF', '51CAFFFE') + sample + W('2E00')      # выдержка, окончательное чтение в d7
code += W('117C00A00018', '117C00010008', '117C0001000C', '117C00000014', '11460010', '117C0030001C') + wait_drq
code += W('2207', '4841', '3081', '3087', '30BCCAFE', '343C00FC', '3086', '51CAFFFC') + wait_ready
code += W('5246', f'0C46{REPORT_CYL + PHASES:04X}')         # addq.w #1,d6 / cmp.w
pos = len(code)
code += W('6600') + struct.pack('>h', phase - (pos + 2))    # bne.w: следующее состояние
code += W('60FE')
rom = bytearray(b'\xff' * 524288)
rom[:8] = struct.pack('>II', 0x00080000, 0x00F80008)
rom[8:8 + len(code)] = code
path = stage / 'joystick.rom'
stage.mkdir(exist_ok=True)
path.write_bytes(rom)

disk = hdf.open('rb').read(512)
assert disk[:4] == b'RDSK', hdf
sectors, heads = struct.unpack_from('>II', disk, 17 * 4)
env = os.environ.copy()
env['JOY_EVENTS'] = ','.join(f'{s}:{args.device}:{state}' for s, state in EVENTS)
command = [sys.executable, str(root / 'run_boot.py'), '--seconds', str(args.seconds), '--rom', str(path), '--hdf', str(hdf)]
if args.no_build:
    command.append('--no-build')
run = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, errors='replace', env=env)
if run.returncode:
    print('модель остановлена с ошибкой:', run.stderr.strip().splitlines()[-1] if run.stderr.strip() else run.returncode)

written = (stage / 'written.bin').read_bytes()
got = {}
for n in range(0, len(written), 516):
    number = struct.unpack_from('<I', written, n)[0]
    hi, lo, mark = struct.unpack_from('>HHH', written, n + 4)
    got[number // (heads * sectors) - REPORT_CYL] = ((hi << 16) | lo, mark)
ok = run.returncode == 0
for k, (when, state) in enumerate(EVENTS):
    key, mark = got.get(k, (None, None))
    shown = 'нет записи' if key is None else f'{key:08X} метка {mark:04X}'
    good = key == WANT[k] and mark == 0xCAFE
    ok = ok and good
    print(f'{when:.2f} с, устройство {args.device}, байт {state:02X}: прочитано {shown}, надо {WANT[k]:08X} — {"верно" if good else "НЕВЕРНО"}')
print('PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
