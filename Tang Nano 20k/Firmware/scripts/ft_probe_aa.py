"""Замер на bt8xxemu: покрытие края RECTS и POINTS по расстоянию до ядра фигуры.

Вырожденный прямоугольник (y0 = y1 = Y·16 + k) радиуса r рисуется белым с COLOR_A 255 поверх
чёрного: яркость строки — покрытие на расстоянии |строка·16 − (Y·16 + k)| в 1/16 пикселя.
Точки — так же, по столбцам через центр. Выход — JSON {фигура: {r: {расстояние: покрытие}}}.

    python ft_probe_aa.py OUT.json
"""
from __future__ import annotations

import json
import struct
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from ft_probe_math import BEGIN, BLEND, CLEAR, CLEAR_COLOR_RGB, COLOR_A, COLOR_RGB, END, V2F, W2, w  # noqa: E402

sys.path.insert(0, str(W2 / "tools"))
import os
RADII = tuple(int(v) for v in os.environ.get("AA_RADII", "8,16,24,32,48,61,62,64,80,128,256").split(","))


def main(argv: list[str]) -> int:
    import ft812emu
    dl = [CLEAR_COLOR_RGB(0, 0, 0), CLEAR(), w(0x27, 4), COLOR_RGB(255, 255, 255), COLOR_A(255), BLEND(2, 4)]
    probes = []
    y = 4
    x = 8
    for r in RADII:
        reach = (r + 16) // 16 + 2
        for k in range(16):
            cy = y * 16 + k
            dl += [w(0x0E, r), BEGIN(9), V2F(x * 16, cy), V2F((x + 3) * 16, cy), END()]
            probes.append(("rect", r, k, x + 1, y, reach))
            x += 6 + 2 * reach
            if x > 990:
                x = 8
                y += 2 * reach + 3
        x = 8
        y += 2 * ((r + 16) // 16 + 2) + 3
    # точки: центр (cx·16 + k, cy·16) — по строке через центр
    ypoints = y + 20
    xp = 30
    pts = []
    for r in RADII:
        reach = (r + 16) // 16 + 2
        for k in (0, 5, 8, 13):
            dl += [w(0x0D, r), BEGIN(2), V2F(xp * 16 + k, ypoints * 16), END()]
            pts.append((r, k, xp, ypoints, reach))
            xp += 2 * reach + 4
            if xp > 980:
                xp = 30
                ypoints += 2 * 18 + 4
        xp = 30
        ypoints += 2 * ((r + 16) // 16 + 2) + 6
    assert ypoints < 760, ypoints
    with ft812emu.FT812() as chip:
        chip.boot()
        dl.append(0)
        assert len(dl) <= 2048, len(dl)
        chip.write(0x300000, struct.pack(f"<{len(dl)}I", *dl))
        chip.wr8(0x302054, 2)
        time.sleep(0.2)
        img = chip.frame().convert("RGB")
    res = {"rect": {}, "point": {}}
    for _, r, k, px, py, reach in probes:
        d = res["rect"].setdefault(str(r), {})
        for row in range(py - reach, py + reach + 1):
            dist = abs(row * 16 - (py * 16 + k))
            d.setdefault(str(dist), set()).add(img.getpixel((px, row))[0])
    for r, k, px, py, reach in pts:
        d = res["point"].setdefault(str(r), {})
        for col in range(px - reach, px + reach + 1):
            dist = abs(col * 16 - (px * 16 + k))
            d.setdefault(str(dist), set()).add(img.getpixel((col, py))[0])
    out = {s: {r: {dd: sorted(v) for dd, v in sorted(dm.items(), key=lambda z: int(z[0]))}
               for r, dm in rm.items()} for s, rm in res.items()}
    Path(argv[1]).write_text(json.dumps(out))
    for s in out:
        for r in out[s]:
            print(s, r, list(out[s][r].items())[:26])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
