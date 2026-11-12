"""Захват HDMI под нагрузкой блиттера (11.11.2026, «9 Fingers» Spaceballs: на плате штрихи в правой половине строки —
пропуски захвата в сценах с залитыми многоугольниками). Вместо Kickstart в ROM — своя программа: 4 плоскости 320x256
(копер ставит указатели каждый кадр), блиттер без остановки заливает плоскости (A -> D, заливка EFE, по убыванию) в «злом»
режиме (BLTPRI). Программа сама работает из ROM: в режиме совместимости это обращения CPU к SDRAM через шину chip.
Итог — строки OVR стенда (кто занимал слоты SDRAM на каждом пропуске) и счётчик пропусков.
    python run_video_load.py [--compat] [--seconds 0.4] [--no-build] [--stage boot-video-load]
"""
from pathlib import Path
import argparse, os, re, struct, subprocess, sys

root = Path(__file__).resolve().parent
ap = argparse.ArgumentParser()
ap.add_argument('--compat', action='store_true')
ap.add_argument('--seconds', type=float, default=0.4)
ap.add_argument('--no-build', action='store_true')
ap.add_argument('--stage', default='boot-video-load')
ap.add_argument('--planes', type=int, default=4)
ap.add_argument('--hires', action='store_true', help='640 точек: выборка плоскостей занимает все слоты окна DDF')
ap.add_argument('--lace', action='store_true', help='interlace: 512 строк в плоскости, модуль — строка')
ap.add_argument('--lace-field', action='store_true', help='вывод interlace: последнее поле (0.13.9)')
ap.add_argument('--no-stretch', action='store_true', help='не растягивать по высоте (0.13.11)')
ap.add_argument('--dump', help='кадры вывода HDMI: «С,N» (run_boot.py --hdmi-dump)')
ap.add_argument('--diw', default='2C81,2CC1', help='DIWSTRT,DIWSTOP (hex), по умолчанию стандартный экран PAL')
args = ap.parse_args()
stage = root / args.stage


class Asm:
    def __init__(self):
        self.code, self.labels, self.fix = bytearray(), {}, []

    def w(self, *words):
        for x in words:
            self.code += bytes.fromhex(x)

    def label(self, name):
        self.labels[name] = len(self.code)

    def branch(self, opcode, name):
        self.code += bytes.fromhex(opcode)
        self.fix.append((len(self.code), name))
        self.code += b'\0\0'

    def done(self):
        for at, name in self.fix:
            self.code[at:at + 2] = struct.pack('>h', self.labels[name] - at)
        return bytes(self.code)


BPL = 80 if args.hires else 40                    # байт в строке плоскости
ROWS = 512 if args.lace else 256
PLANE0, PLANE_BYTES, COPPER = 0x10000, BPL * ROWS, 0x70000
a = Asm()
a.w('13FC000300BFE201', '13FC000200BFE001')        # OVL снят
a.w('4DF900DFF000')                                # a6 = custom
a.w('3D7C7FFF009A', '3D7C7FFF0096', '3D7C7FFF009C')  # INTENA, DMACON, INTREQ — всё выкл.
con0 = (args.planes << 12) | 0x200 | (0x8000 if args.hires else 0) | (4 if args.lace else 0)
mod = BPL if args.lace else 0                     # interlace: поле берёт каждую вторую строку
a.w(f'3D7C{con0:04X}0100', '3D7C00000102', '3D7C00000104', f'3D7C{mod:04X}0108', f'3D7C{mod:04X}010A')
diw0, diw1 = args.diw.split(',')
a.w(f'3D7C{diw0}008E', f'3D7C{diw1}0090', '3D7C003C0092' if args.hires else '3D7C00380092',
    '3D7C00D40094' if args.hires else '3D7C00D00094')   # DIW, DDF — стандартный экран
# палитра: 16 различимых цветов
for i in range(16):
    a.w(f'3D7C{(i * 0x111) & 0xFFF:04X}{0x180 + 2 * i:04X}')
# копер: указатели плоскостей, конец
a.w(f'41F9{COPPER:08X}')                           # lea COPPER,a0
for p in range(args.planes):
    addr = PLANE0 + p * PLANE_BYTES
    a.w(f'20FC{0xE0 + 4 * p:04X}{addr >> 16:04X}', f'20FC{0xE2 + 4 * p:04X}{addr & 0xFFFF:04X}')
a.w('20FCFFFFFFFE')
a.w(f'2D7C{COPPER:08X}0080', '3D7C00000088')       # COP1LC, COPJMP1
# плоскости — узор (каждая строка свой), чтобы заливка давала разное
a.w(f'41F9{PLANE0:08X}', f'303C{args.planes * PLANE_BYTES // 4 - 1:04X}', '223C12345678')
a.label('pat'); a.w('20C1', 'E399', 'D2BC9E3779B9'); a.branch('51C8', 'pat')   # move.l d1,(a0)+ / rol.l #1,d1 / add.l
a.w('3D7C87C00096')                                # DMACON: SET BLTPRI DMAEN BPL COP BLT
a.w('7E00')                                        # d7 — номер плоскости
a.label('loop')
a.label('wb'); a.w('08390006', '00DFF002'); a.branch('6600', 'wb')           # btst #6,$DFF002 (BBUSY)
a.w('2D7C09F000120040', '2D7CFFFFFFFF0044')        # BLTCON0 $09F0, BLTCON1 $0012 (EFE, DESC); маски
a.w('3007', f'C0FC{PLANE_BYTES:04X}', '2040', 'D1FC' + f'{PLANE0 + PLANE_BYTES - 2:08X}')   # a0 = конец плоскости d7
a.w('2D480050', '2D480054', '3D7C00000064', '3D7C00000066', f'3D7C{((ROWS & 1023) << 6) | (BPL // 2):04X}0058')   # A, D, модули, BLTSIZE строки x слова
a.w('5247', f'0C47{args.planes:04X}'); a.branch('6600', 'loop')
a.w('7E00'); a.branch('6000', 'loop')
code = a.done()
rom = bytearray(b'\xff' * 524288)
rom[:8] = struct.pack('>II', 0x00080000, 0x00F80008)
rom[8:8 + len(code)] = code
stage.mkdir(exist_ok=True)
path = stage / 'video_load.rom'
path.write_bytes(rom)
cmd = [sys.executable, str(root / 'run_boot.py'), '--video', '--slot-stats', '--stage', args.stage,
       '--seconds', str(args.seconds), '--rom', str(path)] + (['--compat'] if args.compat else []) + (['--lace-field'] if args.lace_field else []) + (['--no-stretch'] if args.no_stretch else []) +(['--hdmi-dump', args.dump] if args.dump else []) + \
      (['--no-build'] if args.no_build else [])
run = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, errors='replace')
log = (stage / 'run.log').read_text(encoding='utf-8', errors='replace')
if run.returncode:
    print('модель остановлена с ошибкой:', run.stderr.strip().splitlines()[-1] if run.stderr.strip() else run.returncode)
sims = [l for l in log.splitlines() if l.startswith('sim ')]
print('\n'.join(sims[-3:]))
ovr = [l for l in log.splitlines() if l.startswith('OVR')]
print('пропусков захвата:', len(ovr))
for l in ovr[:6]:
    print(' ', l[:260])
