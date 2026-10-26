"""Замер на bt8xxemu: какие слова дисплей-листа сопроцессор FT812 пишет на CMD_GRADIENT.

Для каждого набора (x0, y0, rgb0, x1, y1, rgb1) — CMD_DLSTART, CMD_GRADIENT, затем слова
RAM_DL до REG_CMD_DL. Выход — JSON [{"p": [x0, y0, rgb0, x1, y1, rgb1], "dl": [...]}].
С --sweep — ещё развёртки, по которым снято правило C при L² > 2²⁰ (диагонали, оси с
разными x0/y0, обратные направления; цвета 0 → FFFFFF). Данные — в
bl616/copro/test/data/gradient.txt (bl616/copro/test/make_data.py).

    python ft_probe_gradient.py OUT.json [--sweep]
"""
from __future__ import annotations

import json
import random
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from ft_probe_math import W2  # noqa: E402

sys.path.insert(0, str(W2 / "tools"))

CMD_DLSTART, CMD_GRADIENT = 0xFFFFFF00, 0xFFFFFF0B
REG_CMD_DL = 0x302100


def cases():
    out = [(0, 0, 0x1E293B, 1023, 767, 0x0F172A)]           # Weather 2
    for x0, y0, x1, y1 in ((0, 0, 1023, 0), (0, 0, 0, 767), (0, 0, 1, 0), (0, 0, 0, 1),
                           (10, 20, 300, 20), (10, 20, 10, 300), (100, 100, 50, 50),
                           (512, 384, 513, 385), (0, 0, 2047, 2047), (-100, -50, 200, 400),
                           (5, 5, 5, 5), (1023, 767, 0, 0), (0, 767, 1023, 0), (3, 7, 11, 2)):
        out.append((x0, y0, 0x000000, x1, y1, 0xFFFFFF))
    for rgb0, rgb1 in ((0xFF0000, 0x0000FF), (0x123456, 0x654321), (0x808080, 0x808080),
                       (0x00FF00, 0x00FF00), (0xFFFFFF, 0x000000)):
        out.append((0, 0, rgb0, 640, 480, rgb1))
    rnd = random.Random(5)
    for _ in range(60):
        out.append((rnd.randint(-200, 1200), rnd.randint(-200, 1000), rnd.randint(0, 0xFFFFFF),
                    rnd.randint(-200, 1200), rnd.randint(-200, 1000), rnd.randint(0, 0xFFFFFF)))
    return out


def sweep():
    out = []
    for n in range(1, 2048, 7):
        out.append((0, 0, n, n))
    for n in range(600, 2048, 9):
        out.append((0, 0, n, 0))
    for k in range(0, 768, 5):
        out.append((0, 0, 1023, k))
    for k in range(0, 1024, 13):
        out.append((0, 0, k, 767))
    for x0 in (0, 1, 2, 3, 7, 100, 513):
        for n in list(range(1018, 1032)) + list(range(1444, 1456)) + list(range(2040, 2048)):
            out.append((x0, 0, x0 + n, 0))
    for x0 in (0, 5):
        for n in (1020, 1023, 1024, 1025, 1027, 1448, 1449, 1451, 1453):
            out.append((x0 + n, 0, x0, 0))
            out.append((0, x0, 0, x0 + n))
    for y0 in range(0, 8):
        for n in (1025, 1026, 1027, 1028, 1449, 1450, 1451, 1452):
            out.append((0, y0, 0, y0 + n))
            out.append((0, y0 + n, 0, y0))
    for y0 in (-1, -2, -3, -5, 100, 101):
        for n in (1025, 1026, 1449, 1450, 1451):
            out.append((0, y0, 0, y0 + n))
    for x0 in (1, 2, 3):
        for n in (1025, 1026, 1449, 1451):
            out.append((x0, 3, x0, 3 + n))
    for x0 in range(-3, 8):
        for n in (1025, 1026, 1027, 1028, 1449, 1450, 1451, 1452):
            out.append((x0 + n, 0, x0, 0))
    return [(x0, y0, 0x000000, x1, y1, 0xFFFFFF) for x0, y0, x1, y1 in out]


def s16(v):
    return v & 0xFFFF


def main(argv: list[str]) -> int:
    import ft812emu
    res = []
    with ft812emu.FT812() as chip:
        chip.boot()
        for p in cases() + (sweep() if "--sweep" in argv else []):
            x0, y0, rgb0, x1, y1, rgb1 = p
            chip.cmd(struct.pack("<6I", CMD_DLSTART, CMD_GRADIENT, s16(x0) | (s16(y0) << 16), rgb0,
                                 s16(x1) | (s16(y1) << 16), rgb1))
            chip.wait_idle()
            n = chip.rd32(REG_CMD_DL) // 4
            words = list(struct.unpack(f"<{n}I", chip.read(0x300000, 4 * n)))
            res.append({"p": list(p), "dl": words})
    Path(argv[1]).write_text(json.dumps(res))
    print(len(res), "cases")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
