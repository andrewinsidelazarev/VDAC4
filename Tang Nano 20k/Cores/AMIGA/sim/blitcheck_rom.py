"""Тестовый ROM: правильность блиттера, когда он уступает слоты видео (9 ноября 2026, 0.13.6).

С 0.13.6 блиттер, пока захват видео HDMI отстаёт (video_steal), пропускает каждый второй свой слот (agnus.v,
blt_yield). В 0.12.16 похожая уступка без защёлки на границе слота портила chip RAM. Этот ROM создаёт ту же
видеонагрузку, что финальные титры Nexus 7 (широкий оверскан: окно 352 точки, DDF $18..$C0, 5 плоскостей с
выборкой 4x, блиттер в «злом» режиме BLTPRI), и много раз проверяет результат блита:
  D = A xor B, A, B, D — по 20 слов x 100 строк, B меняется на каждом обороте, D перед блитом обнуляется CPU;
  CPU сверяет каждое слово D с A xor B.
Итог: COLOR00 зелёный ($0F0) — все обороты верны (после 4 оборотов цвет держится, обороты продолжаются);
красный ($F00) — найдено неверное слово (CPU останавливается). Пока идут первые обороты — синий ($00F).
Счётчик пропусков захвата (overruns в video_diag) с правкой должен оставаться нулём.

Не Kickstart и не замена Amiga OS: только собственный код 68000.
  python blitcheck_rom.py                  — записать fixtures/blitcheck.rom
  python blitcheck_rom.py --check RUN_DIR  — сверить native.ppm и status.json
"""
from pathlib import Path
import argparse, json, struct

SRC_A, SRC_B, DST = 0x30000, 0x34000, 0x40000
W, H = 20, 100                       # слов в строке, строк (100 — несколько оборотов за 0,4 с симулятора)
N = W * H
PLANES = [0x50000 + 0x3000 * p for p in range(5)]
COPPER = 0x8000
COLOURS = {'pass': 0x0F0, 'fail': 0xF00, 'run': 0x00F}


def make(path):
    words = []
    fix = []

    def emit(*x): words.extend(x)
    def move_w(v, a): emit(0x33FC, v & 0xFFFF, a >> 16, a & 0xFFFF)
    def move_b(v, a): emit(0x13FC, v & 0xFF, a >> 16, a & 0xFFFF)
    def move_l(v, a): emit(0x23FC, v >> 16, v & 0xFFFF, a >> 16, a & 0xFFFF)
    def lea(a, r): emit(0x41F9 | (r << 9), a >> 16, a & 0xFFFF)
    def dbra(r, target):
        here = len(words); emit(0x51C8 | r, ((target - (here + 1)) * 2) & 0xFFFF)
    def fill(a, n, seed, step):
        lea(a, 0); emit(0x323C, seed, 0x3E3C, n - 1); loop = len(words)
        emit(0x30C1, 0x0641, step); dbra(7, loop)   # MOVE.W D1,(A0)+ / ADDI.W #step,D1

    emit(0x0020, 0x0000, 0x00F8, 0x0008)              # SSP=$200000, PC=$F80008
    emit(0x46FC, 0x2700)
    move_w(0x7FFF, 0xDFF09A); move_w(0x7FFF, 0xDFF09C); move_w(0x7FFF, 0xDFF096)
    move_b(3, 0xBFE201); move_b(2, 0xBFE001)
    move_w(COLOURS['run'], 0xDFF180)
    for p in PLANES: fill(p, 0x3000 // 2, (0x1234 + p) & 0xFFFF, 0x0F1D)      # плоскости: узор
    for i in range(1, 32): move_w((i * 0x111) & 0xFFF, 0xDFF180 + 2 * i)
    # Экран как в финальных титрах Nexus 7: широкий оверскан, 5 плоскостей lowres, выборка 4x.
    for reg, value in [(0x100, 0x5201), (0x102, 0), (0x104, 0x0200), (0x108, 0xFFF8), (0x10A, 0xFFF8),
                       (0x08E, 0x2C71), (0x090, 0x2CD1), (0x092, 0x0018), (0x094, 0x00C0), (0x1FC, 0x0003)]:
        move_w(value, 0xDFF000 + reg)
    lea(COPPER, 0)
    for p in range(5):
        for reg, value in [(0xE0 + 4 * p, PLANES[p] >> 16), (0xE2 + 4 * p, PLANES[p] & 0xFFFF)]:
            emit(0x30FC, reg, 0x30FC, value)          # MOVE.W #imm,(A0)+ x2
    emit(0x30FC, 0xFFFF, 0x30FC, 0xFFFE)
    move_l(COPPER, 0xDFF080); move_w(0, 0xDFF088)
    move_w(0x87C0, 0xDFF096)                          # SET|BLTPRI|DMAEN|BPLEN|COPEN|BLTEN — «злой» режим
    fill(SRC_A, N, 0x5A3C, 0x0137)                    # A — постоянный
    emit(0x7A00)                                      # MOVEQ #0,D5 — номер оборота
    outer = len(words)
    # B = узор, зависящий от оборота: seed = D5 * $0F0F
    lea(SRC_B, 0); emit(0x3205, 0xC2FC, 0x0F0F)       # MOVE.W D5,D1 / MULU.W #$0F0F,D1
    emit(0x3E3C, N - 1); loop = len(words)
    emit(0x30C1, 0x0641, 0x2B1D); dbra(7, loop)
    lea(DST, 0); emit(0x7000, 0x3E3C, N // 2 - 1); loop = len(words)   # D обнулить
    emit(0x20C0); dbra(7, loop)                       # MOVE.L D0,(A0)+
    # блит D = A xor B
    move_l(0x0D3C0000, 0xDFF040)                      # BLTCON0 $0D3C (A,B,D; A^B), BLTCON1 0
    move_l(0xFFFFFFFF, 0xDFF044)
    move_l(SRC_A, 0xDFF050); move_l(SRC_B, 0xDFF04C); move_l(DST, 0xDFF054)
    move_l(0, 0xDFF062); move_l(0, 0xDFF064)          # BLTBMOD/BLTAMOD, BLTDMOD/BLTCMOD... (0)
    move_w(0, 0xDFF066)
    move_l((H << 16) | W, 0xDFF05C)                   # BLTSIZV=400, BLTSIZH=20 — пуск
    emit(0x0839, 0x0006, 0x00DF, 0xF002)              # BTST #6,DMACONR (первое чтение, по HRM)
    wait = len(words)
    emit(0x0839, 0x0006, 0x00DF, 0xF002)
    emit(0x6600 | (((wait - (len(words) + 1)) * 2) & 0xFF))     # BNE.B wait
    # сверка: D == A ^ B
    lea(SRC_A, 0); lea(SRC_B, 2); lea(DST, 1)
    emit(0x3E3C, N - 1); loop = len(words)
    emit(0x3018, 0xB15A)                              # MOVE.W (A0)+,D0 / EOR.W D0,(A2)+  — (A2) := A^B
    emit(0x302A, 0xFFFE)                              # MOVE.W -2(A2),D0
    emit(0xB059)                                      # CMP.W (A1)+,D0
    emit(0x6600, 0); fix.append(len(words) - 1)       # BNE.W fail
    dbra(7, loop)
    emit(0x5245)                                      # ADDQ.W #1,D5
    emit(0x0C45, 4)                                   # CMPI.W #4,D5
    emit(0x6500 | 0x08)                               # BCS.B +8 (пропустить «зелёный»)
    move_w(COLOURS['pass'], 0xDFF180)
    emit(0x6000, ((outer - (len(words) + 1)) * 2) & 0xFFFF)     # BRA.W outer
    fail = len(words)
    move_w(COLOURS['fail'], 0xDFF180)
    emit(0x60FE)                                      # BRA.S * — стоп
    for at in fix: words[at] = ((fail - at) * 2) & 0xFFFF
    data = struct.pack('>%dH' % len(words), *words)
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(data + b'\xff' * (524288 - len(data)))
    return Path(path)


def check(run):
    from PIL import Image
    run = Path(run)
    status = json.loads((run / 'status.json').read_text())
    with Image.open(run / 'native.ppm') as im:
        counts = {}
        for c in im.convert('RGB').getdata(): counts[c] = counts.get(c, 0) + 1
    rgb = lambda c: (((c >> 8) & 15) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17)
    seen = {k: counts.get(rgb(v), 0) for k, v in COLOURS.items()}
    diag = status.get('video_diag', 0)
    result = {'colour_pixels': seen, 'video_diag': '%08X' % diag, 'overruns': diag >> 24,
              'native_frames': status.get('native_frames'), 'hdmi_mismatches': status.get('hdmi_mismatches'),
              'cpu_steps': status.get('cpu_steps')}
    result['outcome'] = 'fail' if seen['fail'] > 1000 else 'pass' if seen['pass'] > 1000 else 'run'
    result['passed'] = result['outcome'] == 'pass' and result['overruns'] == 0
    (run / 'blitcheck.json').write_text(json.dumps(result, indent=2, ensure_ascii=False))
    return result


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', type=Path)
    a = ap.parse_args()
    if a.check:
        r = check(a.check)
        print(json.dumps(r, indent=2, ensure_ascii=False))
        raise SystemExit(0 if r['passed'] else 1)
    print(make(Path(__file__).parent / 'fixtures/blitcheck.rom'))
