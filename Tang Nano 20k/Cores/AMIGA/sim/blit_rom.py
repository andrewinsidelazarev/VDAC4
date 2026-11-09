"""Тестовый ROM: захват видео при непрерывной работе блиттера и турбо chip RAM (9 ноября 2026, 0.13.5).

Nexus 7 (финальные титры): блиттер почти весь кадр гонит цепочку проходов ABD шириной в одно слово
(c2p, каждый следующий проход запускает прерывание «блиттер закончил»), шесть плоскостей AGA с
выборкой 4x, а CPU исполняет код из chip RAM через кэш (турбо chip, 0.13.5). На плате захват
пропускал половины строк — на HDMI оставались куски прошлого кадра (полосы в левой или правой
половине строки). Захват (amiga_video_capture.sv) пишет половину строки в SDRAM только в слотах,
где нет обращения чипсета; блиттер занимает все слоты, а уступает их лишь процессору, который ждёт
шину чипсета (bls). Выборка команд через кэш идёт мимо шины чипсета — bls молчит.

Этот ROM повторяет ту же нагрузку:
  * 6 плоскостей lowres, FMODE=3 (выборка 4x), окно 320x256 (как объединение окон титров), копер;
  * обработчик уровня 3 в chip RAM ($1000): по прерыванию BLIT сразу запускает следующий проход
    ABD (BLTCON0 $0DF8, BLTCON1 $4000, AFWM $F0F0, CDAT $0F0F, модули A/B 2, D 0, BLTSIZV $900,
    BLTSIZH 1 — как шаг 0 цепочки титров), блиттер занят непрерывно; счётчик проходов идёт в COLOR00
    (полосы на бордюре — проходы идут);
  * основной цикл CPU в chip RAM ($2000): чтение списка и ADDQ.B в буфер — данные по шине чипсета.
    blit.rom — цикл развёрнут больше кэша команд (4 КБ), выборка с промахами через порт 2 SDRAM;
    blitc.rom — короткий цикл в кэше, как отрисовка титров: шина чипсета занята данными CPU.
Проверка: счётчик пропусков захвата (overruns в video_diag) должен остаться нулём.

Не Kickstart и не замена Amiga OS: только собственный код 68020.
  python blit_rom.py                  — записать fixtures/blit.rom
  python blit_rom.py --check RUN_DIR  — сверить status.json (overruns), native.ppm, cpu.csv
"""
from pathlib import Path
import argparse, json, struct

PLANES = [0x10000 + 0x1800 * p for p in range(6)]   # 24 байта x 256 строк на плоскость
COPPER = 0x8000
HANDLER = 0x1000
MAIN = 0x2000
LIST = 0x40000
BUFFER = 0x50000
UNROLL = 1000          # 6 байт на пару команд: ~6 КБ кода, больше кэша команд (4 КБ)


def handler_code():
    w = []
    w += [0x33FC, 0x0040, 0x00DF, 0xF09C]               # MOVE.W #$0040,INTREQ — снять BLIT
    w += [0x33FC, 0x0040, 0x00DF, 0xF09C]               # повтор, как в обработчиках демок
    w += [0x23FC, 0x0DF8, 0x4000, 0x00DF, 0xF040]       # BLTCON0=$0DF8 (A,B,D; A|BC), BLTCON1=$4000
    w += [0x33FC, 0xF0F0, 0x00DF, 0xF044]               # BLTAFWM
    w += [0x33FC, 0xFFFF, 0x00DF, 0xF046]               # BLTALWM
    w += [0x33FC, 0x0F0F, 0x00DF, 0xF070]               # BLTCDAT
    w += [0x23FC, 0x0002, 0x0000, 0x00DF, 0xF050]       # BLTAPT = $20000
    w += [0x23FC, 0x0002, 0x0002, 0x00DF, 0xF04C]       # BLTBPT = $20002
    w += [0x23FC, 0x0003, 0x0000, 0x00DF, 0xF054]       # BLTDPT = $30000
    w += [0x23FC, 0x0002, 0x0002, 0x00DF, 0xF062]       # BLTBMOD=2, BLTAMOD=2
    w += [0x33FC, 0x0000, 0x00DF, 0xF066]               # BLTDMOD=0
    w += [0x23FC, 0x0900, 0x0001, 0x00DF, 0xF05C]       # BLTSIZV=$900, BLTSIZH=1 — пуск
    w += [0x5278, 0x0F00]                               # ADDQ.W #1,$0F00.W — счётчик проходов
    w += [0x33F8, 0x0F00, 0x00DF, 0xF180]               # MOVE.W $0F00.W,COLOR00
    w += [0x4E73]                                       # RTE
    return w


def main_code(small=False):
    w = [0x4DF9, LIST >> 16, LIST & 0xFFFF,             # LEA LIST,A6
         0x4BF9, BUFFER >> 16, BUFFER & 0xFFFF]         # LEA BUFFER,A5
    if small:
        # Цикл в кэше, как отрисовка титров ($4a058a): плотные обращения к данным chip RAM
        # почти в каждом чётном слоте; промахов кэша нет.
        w += [0x3E3C, UNROLL - 1]                       # MOVE.W #n-1,D7
        inner = len(w)
        w += [0x301E,                                   # MOVE.W (A6)+,D0
              0x5235, 0x0000]                           # ADDQ.B #1,0(A5,D0.W)
        w += [0x51CF, ((inner - (len(w) + 1)) * 2) & 0xFFFF]   # DBRA D7,inner
    else:
        for _ in range(UNROLL):
            w += [0x301E,                               # MOVE.W (A6)+,D0
                  0x5235, 0x0000]                       # ADDQ.B #1,0(A5,D0.W)
    w += [0x52B8, 0x0F04]                               # ADDQ.L #1,$0F04.W — обороты
    w += [0x6000, ((0 - (len(w) + 1)) * 2) & 0xFFFF]    # BRA.W в начало
    return w


def make(path, small=False):
    words = []

    def emit(*x):
        words.extend(x)

    def move_w(value, address):               # MOVE.W #imm,abs.L
        emit(0x33FC, value & 0xFFFF, address >> 16, address & 0xFFFF)

    def move_b(value, address):
        emit(0x13FC, value, address >> 16, address & 0xFFFF)

    def move_l(value, address):               # MOVE.L #imm,abs.L
        emit(0x23FC, value >> 16, value & 0xFFFF, address >> 16, address & 0xFFFF)

    def lea(address, reg):                    # LEA abs.L,An
        emit(0x41F9 | (reg << 9), address >> 16, address & 0xFFFF)

    def dbra(reg, target):
        here = len(words)
        emit(0x51C8 | reg, ((target - (here + 1)) * 2) & 0xFFFF)

    def fill(address, count, seed, step):     # count слов: seed, seed+step, ...
        lea(address, 0)
        emit(0x323C, seed)                    # MOVE.W #seed,D1
        emit(0x3E3C, count - 1)               # MOVE.W #n-1,D7
        loop = len(words)
        emit(0x30C1)                          # MOVE.W D1,(A0)+
        emit(0x0641, step)                    # ADDI.W #step,D1
        dbra(7, loop)

    copies = []

    def copy(dest, code):                     # копия кода из ROM в chip RAM (адрес ROM — в конце)
        lea(0, 0)
        copies.append((len(words) - 2, code))
        lea(dest, 1)
        emit(0x3E3C, len(code) - 1)           # MOVE.W #n-1,D7
        loop = len(words)
        emit(0x32D8)                          # MOVE.W (A0)+,(A1)+
        dbra(7, loop)

    emit(0x0020, 0x0000, 0x00F8, 0x0008)      # SSP=$200000, PC=$F80008
    emit(0x46FC, 0x2700)                      # MOVE #$2700,SR
    move_w(0x7FFF, 0xDFF09A)                  # INTENA: всё выключить
    move_w(0x7FFF, 0xDFF09C)                  # INTREQ: всё снять
    move_w(0x7FFF, 0xDFF096)                  # DMACON: всё выключить
    move_b(3, 0xBFE201)                       # CIA-A: выходы LED/OVL
    move_b(2, 0xBFE001)                       # снять оверлей ROM (турбо chip — только без оверлея)
    emit(0x7009)                              # MOVEQ #9,D0 — EI + CI
    emit(0x4E7B, 0x0002)                      # MOVEC D0,CACR — кэш CPU включён, как у демок
    move_w(0x0000, 0xDFF180)
    move_l(0, 0x0F00)                         # счётчик проходов и оборотов
    move_l(0, 0x0F04)
    fill(LIST, UNROLL, 0x0000, 37)            # список смещений для ADDQ.B
    fill(PLANES[0], 6 * 0x1800 // 2, 0x1234, 0x1357)   # плоскости: узор
    fill(0x20000, 0x2400 // 2, 0x0F0F, 0x0101)          # источник блиттера
    copy(HANDLER, handler_code())
    copy(MAIN, main_code(small))
    move_l(HANDLER, 0x006C)                   # вектор уровня 3
    for i in range(64):                       # палитра: оттенки, чтобы картинка была видна
        move_w(((i * 5) & 0xF) << 8 | ((i * 3) & 0xF) << 4 | (i & 0xF), 0xDFF180 + 2 * (i & 31))
    # Экран как в титрах Nexus 7: 6 плоскостей lowres, выборка 4x. Окно захвата в титрах — объединение
    # окон кадра: вверху экран 320 точек ($2C81..$2CC1), поэтому захват пишет всю ширину 320 lowres
    # (640 точек захвата) — здесь сразу окно 320x256 и DDF $38..$A0.
    for reg, value in [(0x100, 0x6201), (0x102, 0), (0x104, 0x0200), (0x108, 0), (0x10A, 0),
                       (0x08E, 0x2C81), (0x090, 0x2CC1), (0x092, 0x0038), (0x094, 0x00A0),
                       (0x1FC, 0x0003)]:
        move_w(value, 0xDFF000 + reg)
    lea(COPPER, 0)
    for p in range(6):
        for reg, value in [(0xE0 + 4 * p, PLANES[p] >> 16), (0xE2 + 4 * p, PLANES[p] & 0xFFFF)]:
            emit(0x30FC, reg)                 # MOVE.W #imm,(A0)+
            emit(0x30FC, value)
    emit(0x30FC, 0xFFFF, 0x30FC, 0xFFFE)      # конец списка копера
    move_l(COPPER, 0xDFF080)                  # COP1LC
    move_w(0, 0xDFF088)                       # COPJMP1
    move_w(0x83C0, 0xDFF096)                  # DMACON: SET|DMAEN|BPLEN|COPEN|BLTEN (без BLTPRI)
    move_w(0xC040, 0xDFF09A)                  # INTENA: INTEN|BLIT
    move_w(0x8040, 0xDFF09C)                  # INTREQ: BLIT — первый проход запустит обработчик
    emit(0x46FC, 0x2000)                      # MOVE #$2000,SR — прерывания разрешены
    emit(0x4EF9, MAIN >> 16, MAIN & 0xFFFF)   # JMP MAIN
    for at, code in copies:                   # код для копирования — сразу за программой
        address = 0xF80000 + 2 * len(words)
        words[at] = address >> 16
        words[at + 1] = address & 0xFFFF
        words.extend(code)
    data = struct.pack('>' + str(len(words)) + 'H', *words)
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(data + b'\xff' * (524288 - len(data)))
    return Path(path)


def check(run):
    from PIL import Image
    run = Path(run)
    status = json.loads((run / 'status.json').read_text())
    diag = status.get('video_diag', 0)
    with Image.open(run / 'native.ppm') as im:
        colours = len(set(im.convert('RGB').getdata()))
    # Шаги CPU в последних 4096 циклах: самый длинный промежуток (голодание CPU) и где он
    rows = [l.split(',') for l in (run / 'cpu.csv').read_text().splitlines()[1:]]
    steps = [(int(r[0]), int(r[1], 16), r[2]) for r in rows]
    gaps = [(b[0] - a[0], a[1], b[1], b[2]) for a, b in zip(steps, steps[1:])]
    worst = max(gaps) if gaps else None
    in_main = sum(1 for t, a, s in steps if MAIN <= a < MAIN + 8000)
    in_handler = sum(1 for t, a, s in steps if HANDLER <= a < HANDLER + 200)
    result = {
        'video_diag': '%08X' % diag, 'overruns': diag >> 24, 'frame_valid': (diag >> 16) & 1,
        'native_frames': status.get('native_frames'), 'hdmi_frames': status.get('hdmi_frames'),
        'hdmi_mismatches': status.get('hdmi_mismatches'), 'native_colours': colours,
        'cpu_steps': status.get('cpu_steps'), 'trace_steps_main': in_main, 'trace_steps_handler': in_handler,
        'cpu_worst_gap_us': worst and round(worst[0] / 1e6, 3),
        'cpu_worst_gap_at': worst and ['%06X' % worst[1], '%06X' % worst[2], worst[3]],
    }
    result['passed'] = (result['overruns'] == 0 and result['frame_valid'] == 1 and
                        status.get('hdmi_mismatches', 1) == 0 and in_main > 0)
    (run / 'blit-check.json').write_text(json.dumps(result, indent=2, ensure_ascii=False))
    return result


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', type=Path)
    a = ap.parse_args()
    if a.check:
        r = check(a.check)
        print(json.dumps(r, indent=2, ensure_ascii=False))
        raise SystemExit(0 if r['passed'] else 1)
    print(make(Path(__file__).parent / 'fixtures/blit.rom'))
    print(make(Path(__file__).parent / 'fixtures/blitc.rom', small=True))   # цикл в кэше
