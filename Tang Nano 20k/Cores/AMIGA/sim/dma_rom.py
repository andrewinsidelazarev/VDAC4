"""Тестовый ROM с настоящей нагрузкой шины chip RAM (7 ноября 2026).

Четыре битовые плоскости 320x256 низкого разрешения через копер (DMA Agnus
держит запрос весь CCK), а CPU всё время копирует chip RAM -> chip RAM через
шину чипсета (turbo chip выключен). Так в полном симуляторе проверяется
пропуск повторных обращений контроллера SDRAM на настоящей шине minimig
и видеопуть 720p под нагрузкой, как на плате с чёрным экраном 0.12.3.

Не Kickstart и не замена Amiga OS: только собственный код 68000.
  python dma_rom.py                  — записать fixtures/dma.rom
  python dma_rom.py --check RUN_DIR  — сверить native.ppm/hdmi.ppm/status.json
"""
from pathlib import Path
import argparse, json, struct

PLANES = [0x10000, 0x13000, 0x16000, 0x19000]
SEEDS = [0xFFFF, 0x1234, 0x0F0F, 0x8001]
STEPS = [0x0001, 0x0003, 0x0105, 0x0F07]
WORDS = 20 * 256          # 320 точек = 20 слов на строку, 256 строк
COPPER = 0x20000
# Палитра OCS: фон 0x111, остальные цвета различимы по каждому каналу.
PALETTE = [0x111, 0xF00, 0x0F0, 0x00F, 0xFF0, 0x0FF, 0xF0F, 0xFFF,
           0x800, 0x080, 0x008, 0x880, 0x088, 0x808, 0x888, 0x4A6]


def rgb24(c):
    r, g, b = (c >> 8) & 15, (c >> 4) & 15, c & 15
    return (r * 17, g * 17, b * 17)


def plane_word(p, k):
    return (SEEDS[p] + k * STEPS[p]) & 0xFFFF


def expected_index(x, y):
    k = y * 20 + x // 16
    bit = 15 - x % 16
    return sum(((plane_word(p, k) >> bit) & 1) << p for p in range(4))


def make(path):
    words = []

    def emit(*w):
        words.extend(w)

    def move_w(value, address):
        emit(0x33FC, value, address >> 16, address & 0xFFFF)

    def move_b(value, address):
        emit(0x13FC, value, address >> 16, address & 0xFFFF)

    def move_l(value, address):
        emit(0x23FC, value >> 16, value & 0xFFFF, address >> 16, address & 0xFFFF)

    def lea(address, reg):                    # LEA abs.L,An
        emit(0x41F9 | (reg << 9), address >> 16, address & 0xFFFF)

    def dbra(reg, target):                    # DBRA Dn,target
        here = len(words)
        emit(0x51C8 | reg, ((target - (here + 1)) * 2) & 0xFFFF)

    emit(0x0020, 0x0000, 0x00F8, 0x0008)      # SSP=$200000, PC=$F80008
    emit(0x46FC, 0x2700)                      # MOVE #$2700,SR
    move_w(0x7FFF, 0xDFF09A)                  # INTENA: всё выключить
    move_w(0x7FFF, 0xDFF096)                  # DMACON: всё выключить
    move_b(3, 0xBFE201)                       # CIA-A: выходы LED/OVL
    move_b(2, 0xBFE001)                       # снять оверлей ROM
    for p in range(4):                        # заполнить плоскости
        lea(PLANES[p], 0)
        emit(0x323C, SEEDS[p])                # MOVE.W #seed,D1
        emit(0x363C, STEPS[p])                # MOVE.W #step,D3
        emit(0x3E3C, WORDS - 1)               # MOVE.W #n-1,D7
        loop = len(words)
        emit(0x30C1)                          # MOVE.W D1,(A0)+
        emit(0xD243)                          # ADD.W D3,D1
        dbra(7, loop)
    for i, c in enumerate(PALETTE):
        move_w(c, 0xDFF180 + 2 * i)
    for reg, value in [(0x100, 0x4200), (0x102, 0), (0x104, 0), (0x108, 0), (0x10A, 0),
                       (0x08E, 0x2C81), (0x090, 0x2CC1), (0x092, 0x0038), (0x094, 0x00D0),
                       (0x1FC, 0)]:
        move_w(value, 0xDFF000 + reg)
    lea(COPPER, 0)                            # список копера: указатели плоскостей
    for p in range(4):
        for reg, value in [(0xE0 + 4 * p, PLANES[p] >> 16), (0xE2 + 4 * p, PLANES[p] & 0xFFFF)]:
            emit(0x30FC, reg)                 # MOVE.W #imm,(A0)+
            emit(0x30FC, value)
    emit(0x30FC, 0xFFFF, 0x30FC, 0xFFFE)      # конец списка
    move_l(COPPER, 0xDFF080)                  # COP1LC
    move_w(0, 0xDFF088)                       # COPJMP1
    move_w(0x8380, 0xDFF096)                  # DMACON: DMAEN|BPLEN|COPEN
    outer = len(words)                        # CPU: копирование chip -> chip
    lea(0x40000, 1)
    lea(0x50000, 2)
    emit(0x3E3C, 4095)                        # MOVE.W #4095,D7
    inner = len(words)
    emit(0x24D9)                              # MOVE.L (A1)+,(A2)+
    dbra(7, inner)
    emit(0x6000, ((outer - (len(words) + 1)) * 2) & 0xFFFF)  # BRA.W outer
    data = struct.pack('>' + str(len(words)) + 'H', *words)
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(data + b'\xff' * (524288 - len(data)))
    return Path(path)


def read_ppm(path):
    from PIL import Image
    with Image.open(path) as im:
        im = im.convert('RGB')
        w, h = im.size
        data = im.tobytes()
    return w, h, data


def check(run):
    run = Path(run)
    status = json.loads((run / 'status.json').read_text())
    w, h, px = read_ppm(run / 'native.ppm')
    at = lambda x, y: tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3])
    border = rgb24(PALETTE[0])
    # Окно: первая строка, где есть цвет, отличный от фона и чёрного гашения.
    y0 = x0 = None
    for y in range(h):
        for x in range(w):
            c = at(x, y)
            if c != border and c != (0, 0, 0):
                y0, x0 = y, x
                break
        if y0 is not None:
            break
    best = None
    if y0 is not None:
        palette = [rgb24(c) for c in PALETTE]
        for scale in (4, 2):
            for dy in (-1, 0, 1):
                for dx in range(-8, 9):
                    ox, oy = x0 + dx, y0 + dy
                    if ox < 0 or oy < 0 or ox + 320 * scale > w or oy + 256 > h:
                        continue
                    bad = 0
                    for y in range(256):
                        for x in range(320):
                            if at(ox + x * scale + scale // 2, oy + y) != palette[expected_index(x, y)]:
                                bad += 1
                                if best and bad >= best[0]:
                                    break
                        if best and bad >= best[0]:
                            break
                    if best is None or bad < best[0]:
                        best = (bad, scale, ox, oy)
    hw, hh, hp = read_ppm(run / 'hdmi.ppm')
    colours = set(tuple(hp[i:i + 3]) for i in range(0, len(hp), 3))
    pal_seen = sum(1 for c in PALETTE[1:] if rgb24(c) in colours)
    diag = status.get('video_diag', 0)
    result = {
        'native_size': [w, h], 'window_origin': best and best[2:], 'pixel_samples': best and best[1],
        'native_bad_pixels': best[0] if best else None,
        'hdmi_size': [hw, hh], 'hdmi_palette_colours_seen': pal_seen,
        'video_diag': '%08X' % diag, 'overruns': diag >> 24, 'misses': diag & 0xFFFF,
        'frame_valid': (diag >> 16) & 1, 'native_frames': status.get('native_frames'),
        'hdmi_frames': status.get('hdmi_frames'), 'hdmi_mismatches': status.get('hdmi_mismatches'),
    }
    result['passed'] = (best is not None and best[0] == 0 and pal_seen == 15 and
                        result['overruns'] == 0 and result['misses'] == 0 and result['frame_valid'] == 1 and
                        status.get('hdmi_mismatches', 1) == 0)
    (run / 'dma-check.json').write_text(json.dumps(result, indent=2))
    return result


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', type=Path)
    a = ap.parse_args()
    if a.check:
        r = check(a.check)
        print(json.dumps(r, indent=2))
        raise SystemExit(0 if r['passed'] else 1)
    print(make(Path(__file__).parent / 'fixtures/dma.rom'))
