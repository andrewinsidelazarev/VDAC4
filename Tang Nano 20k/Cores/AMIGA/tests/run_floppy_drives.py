"""Отсутствующие дисководы DF1–DF3 (11.11.2026, демо «9 Fingers» Spaceballs: «Disk sector transfer failed» на просьбе
вставить вторую дискету). Настоящая цепочка ядра: 68000 -> CIA -> Paula (paula_floppy) -> nanomig -> amiga_disk.

Вместо Kickstart в ROM лежит программа:
1) опрос дисководов DF0–DF3 точно как в загрузочном блоке демо: выбрать дисковод, если нет RDY — шагать наружу до 84 раз,
   пока не загорится TK0 (дорожка 0). Дисковод считается подключённым, если ответил RDY или TK0. У настоящего A500/A1200
   без внешних дисководов линии DF1–DF3 никто не тянет вниз: они не ответят ничем;
2) чтение по DMA при выбранном только DF1 (так демо ищет вторую дискету во всех «найденных» дисководах). Ядро 0.13.7
   отдавало такой запрос хосту (дисковод 02), ящик не принимал на него ответа — на плате AMIGA.ELF обрывал сеанс;
3) чтение по DMA с DF0 — оно обязано пройти и после попытки с DF1.
Итог программа пишет сектором на HD0 (как run_ide_floppy.py): слово 0 — найденные дисководы (бит n — DFn), слово 1 —
пришёл ли DSKBLK при DF1, слово 2 — пришёл ли DSKBLK при DF0, слово 3 — метка CAFE.
    python run_floppy_drives.py --adf образ.adf [--seconds 0.4] [--no-build]
"""
from pathlib import Path
import argparse, os, struct, subprocess, sys

root = Path(__file__).resolve().parent
stage = root / 'boot-build'
ap = argparse.ArgumentParser()
ap.add_argument('--adf', type=Path, required=True)
ap.add_argument('--seconds', type=float, default=0.4)
ap.add_argument('--no-build', action='store_true')
args = ap.parse_args()
hdf = root / 'build/new.hdf'
REPORT_CYL = 100


class Asm:
    """Крошечный ассемблер: слова машинного кода и переходы bxx.w / dbra на метки."""
    def __init__(self):
        self.code, self.labels, self.fix = bytearray(), {}, []

    def w(self, *words):
        for x in words:
            self.code += bytes.fromhex(x)

    def label(self, name):
        self.labels[name] = len(self.code)

    def branch(self, opcode, name):
        # смещение считается от адреса слова смещения (адрес команды + 2)
        self.code += bytes.fromhex(opcode)
        self.fix.append((len(self.code), name))
        self.code += b'\0\0'

    def done(self):
        for at, name in self.fix:
            self.code[at:at + 2] = struct.pack('>h', self.labels[name] - at)
        return bytes(self.code)


a = Asm()
a.w('13FC000300BFE201', '13FC000200BFE001')            # OVL снят: чип-память по адресу 0
a.w('41F900DA0000', '45F900DFF000')                    # a0 IDE, a2 custom
a.w('47F900BFD100', '49F900BFE001')                    # a3 CIAB PRB (мотор, выбор, шаг), a4 CIAA PRA (RDY, TK0)
a.label('ide0'); a.w('1028001C'); a.branch('6B00', 'ide0')  # ждать готовности IDE
a.w('177C00FF0200', '16BC00FF')                        # DDRB — выходы; никто не выбран, мотор выключен
a.w('357C82100096', '357C7F00009E', '357C9500009E', '357C4489007E')  # DMACON, ADKCON (MFM, WORDSYNC, FAST), DSKSYNC

# 1) Опрос дисководов, как загрузочный блок «9 Fingers» (ADF A, код $C0…$F8)
a.w('7C00', '7A03')                                    # d6 — найденные, d5 — бит выбора SEL (3 = DF0 … 6 = DF3)
a.label('drive')
a.w('16BC007F')                                        # мотор вкл., никто не выбран, DIR наружу, STEP в 1
a.w('0B93')                                            # bclr d5,(a3) — выбрать дисковод
a.w('08140005'); a.branch('6700', 'present')           # btst #5,(a4): RDY
a.w('383C0053')                                        # до 84 шагов
a.label('step')
a.w('08140004'); a.branch('6700', 'present')           # btst #4,(a4): TK0
a.w('08930000', '08D30000')                            # шаг: STEP вниз и вверх (фронт)
a.w('303C0028', '51C8FFFE')                            # пауза больше 256 тактов 7 МГц — шаг принимается не чаще
a.branch('51CC', 'step')                               # dbra d4
a.branch('6000', 'next')
a.label('present')
a.w('3005', '5740', '01C6')                            # move.w d5,d0 / subq.w #3,d0 / bset d0,d6
a.label('next')
a.w('5245', '0C450007'); a.branch('6600', 'drive')     # следующий дисковод до DF3

# 2) DMA при выбранном только DF1: ответа нет — DSKBLK не приходит, через ~17 мс DMA снимается
a.w('16BC007F', '08930004')                            # bclr #4,(a3) — SEL1
a.w('357C0002009C', '257C000100000020', '357C40000024', '357C83E80024', '357C83E80024')
a.w('7E00', '343C0BB8')                                # d7 = 0, 3000 опросов
a.label('wa')
a.w('302A001E', '08000001'); a.branch('6600', 'a_blk')  # INTREQR, DSKBLK
a.branch('51CA', 'wa')
a.branch('6000', 'a_end')
a.label('a_blk'); a.w('7E01')
a.label('a_end'); a.w('357C40000024')                  # DMA выкл.

# 3) DMA с DF0: тысяча слов, DSKBLK обязан прийти
a.w('16BC007F', '08930003')                            # bclr #3,(a3) — SEL0
a.w('357C0002009C', '257C000100000020', '357C40000024', '357C83E80024', '357C83E80024')
a.w('7600', '343C7FFF')                                # d3 = 0, до 32768 опросов
a.label('wb')
a.w('302A001E', '08000001'); a.branch('6600', 'b_blk')
a.branch('51CA', 'wb')
a.branch('6000', 'b_end')
a.label('b_blk'); a.w('7601')
a.label('b_end'); a.w('357C40000024', '16BC00FF')

# Итог — сектор на HD0, цилиндр REPORT_CYL
a.w(f'117C{REPORT_CYL:04X}0010', '117C00A00018', '117C00010008', '117C0001000C', '117C00000014', '117C0030001C')
a.label('drq'); a.w('1028001C', '08000003'); a.branch('6700', 'drq')
a.w('3086', '3087', '3083', '30BCCAFE', '343C00FB', '4250', '51CAFFFC')   # d6, d7, d3, CAFE и 252 нуля
a.label('ide1'); a.w('1028001C'); a.branch('6B00', 'ide1')
a.w('60FE')
code = a.done()

rom = bytearray(b'\xff' * 524288)
rom[:8] = struct.pack('>II', 0x00080000, 0x00F80008)
rom[8:8 + len(code)] = code
path = stage / 'floppy_drives.rom'
stage.mkdir(exist_ok=True)
path.write_bytes(rom)

disk = hdf.open('rb').read(512)
assert disk[:4] == b'RDSK', hdf
sectors, heads = struct.unpack_from('>II', disk, 17 * 4)
command = [sys.executable, str(root / 'run_boot.py'), '--seconds', str(args.seconds), '--rom', str(path),
           '--adf', str(args.adf), '--hdf', str(hdf)]
if args.no_build:
    command.append('--no-build')
env = os.environ.copy()
env['FLOPPY_FAST'] = '1'                                # дискета отвечает сразу, как AMIGA.ELF
# HD0 — после старта процессора (0,08 с модели), как у AMIGA.ELF: подключённый во время сброса гостя (стенд по умолчанию
# через 0,2 мс) не получает чтения RDB — сброс снимает запрос, и программа вечно ждёт готовности IDE
env.setdefault('HDF_ATTACH_S', '0.09')
run = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, errors='replace', env=env)
log = (stage / 'run.log').read_text(encoding='utf-8', errors='replace')
lines = [x for x in log.splitlines() if x.startswith(('first read request', 'FAIL'))]
print('\n'.join(lines))
if run.returncode:
    print('модель остановлена с ошибкой:', run.stderr.strip().splitlines()[-1] if run.stderr.strip() else run.returncode)

written = (stage / 'written.bin').read_bytes()
report = None
for n in range(0, len(written), 516):
    if struct.unpack_from('<I', written, n)[0] == REPORT_CYL * heads * sectors:
        report = struct.unpack_from('>HHHH', written, n + 4)
foreign = [x for x in lines if x.startswith('first read request') and int(x.split()[4], 16) & 0x0E]
if report:
    print(f'итог программы: найдены дисководы {report[0]:04b} (бит n — DFn), DSKBLK при DF1 {report[1]}, '
          f'при DF0 {report[2]}, метка {report[3]:04X}')
else:
    print('итог программы не записан: программа не дошла до конца')
print('запросов секторов с DF1–DF3:', len(foreign))
ok = run.returncode == 0 and report == (1, 0, 1, 0xCAFE) and not foreign
print('PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
