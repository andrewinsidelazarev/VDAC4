"""Тестовый ROM: код в chip RAM при турбо chip RAM и снупе кэша CPU (9 ноября 2026, 0.13.5).

С 0.13.5 выборка команд CPU из chip RAM идёт через кэш CPU (osd_turbo=3), а записи chip-шины
(CPU и DMA) должны обновлять кэш (CHIPRAM_CACHE). Тест повторяет то, что делают демки и распаковщики:
пишет код в chip RAM, исполняет его, меняет код записью CPU и блиттером и снова исполняет.

  1. $3000: MOVEQ #1,D0 / RTS — записан CPU, вызов трижды (строка попадает в кэш), D0 = 1;
  2. запись CPU $7002 по $3000, сразу вызов — D0 = 2 (снуп записи CPU по шине чипсета);
  3. блиттер копирует $7003 в $3000 — D0 = 3 (снуп записи DMA);
  4. цикл в chip RAM ($3200, 1000 оборотов ADDQ.L #1,D1) — D1 = 1000; запись CPU меняет ADDQ #1 на
     ADDQ #2 внутри уже исполнявшегося цикла — D1 = 2000.
Итог: COLOR00 зелёный ($0F0) — всё верно; красный $F00 — шаг 1, жёлтый $FF0 — шаг 2,
пурпурный $F0F — шаг 3, синий $00F — шаг 4. После установки цвета CPU уходит в цикл NOP/BRA
в chip RAM ($3300): по cpu.csv видно, что выборка идёт быстрым путём (кэш), а не по шине чипсета.

Не Kickstart и не замена Amiga OS: только собственный код 68000.
  python cache_rom.py                  — записать fixtures/cache.rom
  python cache_rom.py --check RUN_DIR  — сверить native.ppm и cpu.csv
"""
from pathlib import Path
import argparse, json, struct

COLOURS = {'pass': 0x0F0, 'step1': 0xF00, 'step2': 0xFF0, 'step3': 0xF0F, 'step4': 0x00F}


def make(path):
    words = []
    fixups = []

    def emit(*w):
        words.extend(w)

    def move_w(value, address):               # MOVE.W #imm,abs.L
        emit(0x33FC, value, address >> 16, address & 0xFFFF)

    def move_b(value, address):
        emit(0x13FC, value, address >> 16, address & 0xFFFF)

    def move_l(value, address):               # MOVE.L #imm,abs.L
        emit(0x23FC, value >> 16, value & 0xFFFF, address >> 16, address & 0xFFFF)

    def jsr(address):
        emit(0x4EB9, address >> 16, address & 0xFFFF)

    def bne_fail(name):                       # BNE.W метка (дописывается в конце)
        emit(0x6600, 0)
        fixups.append((len(words) - 1, name))

    def cmpi_l(value, reg):                   # CMPI.L #imm,Dn
        emit(0x0C80 | reg, value >> 16, value & 0xFFFF)

    def code(address, ws):                    # записать код в chip RAM по слову (записи CPU)
        for i, w in enumerate(ws):
            move_w(w, address + 2 * i)

    emit(0x0020, 0x0000, 0x00F8, 0x0008)      # SSP=$200000, PC=$F80008
    emit(0x46FC, 0x2700)                      # MOVE #$2700,SR
    move_w(0x7FFF, 0xDFF09A)                  # INTENA: всё выключить
    move_w(0x7FFF, 0xDFF096)                  # DMACON: всё выключить
    move_b(3, 0xBFE201)                       # CIA-A: выходы LED/OVL
    move_b(2, 0xBFE001)                       # снять оверлей ROM (турбо chip — только без оверлея)
    # Кэш CPU (cpu_cache_new) включается битом EI регистра CACR 68020, как у Kickstart и демок
    # (Nexus 7 пишет $A009). Без этого строки не кэшируются и снуп не проверяется.
    emit(0x7009)                              # MOVEQ #9,D0 — EI + CI (очистить)
    emit(0x4E7B, 0x0002)                      # MOVEC D0,CACR
    move_w(0x0000, 0xDFF180)                  # COLOR00 чёрный, пока идёт тест
    # шаг 1
    code(0x3000, [0x7001, 0x4E75])            # MOVEQ #1,D0 / RTS
    emit(0x7000)                              # MOVEQ #0,D0
    jsr(0x3000); jsr(0x3000); jsr(0x3000)
    cmpi_l(1, 0); bne_fail('step1')
    # шаг 2: запись CPU и сразу исполнение
    move_w(0x7002, 0x3000)
    jsr(0x3000)
    cmpi_l(2, 0); bne_fail('step2')
    # шаг 3: блиттер A -> D, одно слово $7003 из $3100 в $3000
    move_w(0x7003, 0x3100)
    move_w(0x8240, 0xDFF096)                  # DMACON: SET|DMAEN|BLTEN
    move_l(0x09F00000, 0xDFF040)              # BLTCON0=$09F0 (A,D, D=A), BLTCON1=0
    move_l(0xFFFFFFFF, 0xDFF044)              # BLTAFWM/BLTALWM
    move_l(0x00003100, 0xDFF050)              # BLTAPT
    move_l(0x00003000, 0xDFF054)              # BLTDPT
    move_w(0, 0xDFF064); move_w(0, 0xDFF066)  # BLTAMOD, BLTDMOD
    move_w(0x0041, 0xDFF058)                  # BLTSIZE: высота 1, ширина 1 слово — пуск
    emit(0x0839, 0x0006, 0x00DF, 0xF002)      # BTST #6,$DFF002 (первое чтение — по HRM, пустое)
    wait = len(words)
    emit(0x0839, 0x0006, 0x00DF, 0xF002)      # BTST #6,$DFF002 (BBUSY)
    emit(0x6600 | ((((wait - (len(words) + 1)) * 2) - 0) & 0xFF))  # BNE.B wait
    jsr(0x3000)
    cmpi_l(3, 0); bne_fail('step3')
    # шаг 4: цикл в chip RAM, затем правка команды внутри него
    code(0x3200, [0x7200, 0x343C, 999, 0x5281, 0x51CA, 0xFFFC, 0x4E75])
    jsr(0x3200)
    cmpi_l(1000, 1); bne_fail('step4')
    move_w(0x5481, 0x3206)                    # ADDQ.L #1,D1 -> ADDQ.L #2,D1
    jsr(0x3200)
    cmpi_l(2000, 1); bne_fail('step4')
    labels = {}
    for name in ['pass', 'step1', 'step2', 'step3', 'step4']:
        labels[name] = len(words)
        move_w(COLOURS[name], 0xDFF180)
        code(0x3300, [0x4E71, 0x60FC])        # $3300: NOP / BRA.S $3300 — цикл выборки в chip RAM
        emit(0x4EF9, 0x0000, 0x3300)          # JMP $3300
    for at, name in fixups:
        words[at] = ((labels[name] - at) * 2) & 0xFFFF   # смещение от адреса слова расширения
    # BNE.B wait: смещение от адреса после кода команды
    data = struct.pack('>' + str(len(words)) + 'H', *words)
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(data + b'\xff' * (524288 - len(data)))
    return Path(path)


def check(run):
    from PIL import Image
    run = Path(run)
    status = json.loads((run / 'status.json').read_text())
    with Image.open(run / 'native.ppm') as im:
        px = im.convert('RGB').getdata()
        counts = {}
        for c in px:
            counts[c] = counts.get(c, 0) + 1
    rgb = lambda c: (((c >> 8) & 15) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17)
    seen = {name: counts.get(rgb(c), 0) for name, c in COLOURS.items()}
    # выборка цикла $3300/$3302: интервалы между соседними шагами CPU
    rows = [l.split(',') for l in (run / 'cpu.csv').read_text().splitlines()[1:]]
    loop = [(int(t), int(a, 16)) for t, a, s, r, w in rows if int(a, 16) in (0x3300, 0x3302) and s == '0']
    gaps = sorted(b[0] - a[0] for a, b in zip(loop, loop[1:]))
    median_ns = gaps[len(gaps) // 2] / 1000 if gaps else None
    result = {'colour_pixels': seen, 'loop_fetches': len(loop), 'loop_fetch_median_ns': median_ns,
              'cpu_steps': status.get('cpu_steps'), 'native_frames': status.get('native_frames')}
    best = max(seen, key=seen.get)
    result['outcome'] = best if seen[best] else 'нет цвета'
    # шина чипсета: ~560 нс на слово; кэш — заметно быстрее
    result['passed'] = best == 'pass' and seen[best] > 1000 and median_ns is not None and median_ns < 400
    (run / 'cache-check.json').write_text(json.dumps(result, indent=2, ensure_ascii=False))
    return result


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', type=Path)
    a = ap.parse_args()
    if a.check:
        r = check(a.check)
        print(json.dumps(r, indent=2, ensure_ascii=False))
        raise SystemExit(0 if r['passed'] else 1)
    print(make(Path(__file__).parent / 'fixtures/cache.rom'))
