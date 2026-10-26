"""Замер на bt8xxemu: края RECTS и POINTS и какие пиксели получают фрагмент.

Для каждого радиуса r — прямоугольник с ядром 4×4 пикселя, сдвинутым на k/16, и точка:
 - «cov»: белым, COLOR_A 255 поверх чёрного — яркость пикселя = покрытие;
 - «st»: та же фигура с COLOR_MASK 0 и STENCIL_OP(KEEP, REPLACE) ставит трафарет 1,
   потом белый прямоугольник на весь участок с STENCIL_FUNC(EQUAL, 1) — белое = был фрагмент.
Выход — JSON {"cov|st,rect|point,r,k": {x, y, core, px}} — весь участок фигуры, и "_dl".

    python ft_probe_corner.py OUT.json
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
RADII = tuple(int(v) for v in os.environ.get("CORNER_RADII", "16,24,40,64,128,200,304,320").split(","))
KS = (0, 8)


def main(argv: list[str]) -> int:
    import ft812emu
    dl = [CLEAR_COLOR_RGB(0, 0, 0), CLEAR(), w(0x27, 4)]
    cases = []
    x, y = 2, 2
    row_h = 0
    for kind in ("cov", "st"):
        for shape in ("rect", "point"):
            for r in RADII:
                for k in KS:
                    reach = (r + 16) // 16 + 2
                    core_w = 4 if shape == "rect" else 0
                    box = 2 * reach + core_w + 1
                    if x + box + 2 > 1020:
                        x = 2
                        y += row_h + 2
                        row_h = 0
                    cx, cy = x + reach, y + reach
                    if shape == "rect":
                        prim = [w(0x0E, r), BEGIN(9), V2F(cx * 16 + k, cy * 16 + k),
                                V2F((cx + 4) * 16 + k, (cy + 4) * 16 + k), END()]
                    else:
                        prim = [w(0x0D, r), BEGIN(2), V2F(cx * 16 + k, cy * 16 + k), END()]
                    if kind == "cov":
                        dl += [COLOR_RGB(255, 255, 255), COLOR_A(255), BLEND(2, 4), *prim]
                    else:
                        dl += [w(0x20, 0), w(0x0C, (1 << 3) | 2), w(0x0A, (7 << 16) | (1 << 8) | 255), *prim,
                               w(0x20, 0xF), w(0x0C, (1 << 3) | 1), w(0x0A, (5 << 16) | (1 << 8) | 255),
                               COLOR_RGB(255, 255, 255), COLOR_A(255), BLEND(1, 0), w(0x0E, 16), BEGIN(9),
                               V2F(x * 16, y * 16), V2F((x + box - 1) * 16, (y + box - 1) * 16), END(),
                               w(0x0A, (7 << 16) | 255), w(0x0C, (1 << 3) | 1)]
                    cases.append((kind, shape, r, k, x, y, box, cx, cy))
                    x += box + 2
                    row_h = max(row_h, box)
    assert y + row_h < 768, y
    assert len(dl) < 2047, len(dl)
    with ft812emu.FT812() as chip:
        chip.boot()
        dl.append(0)
        chip.write(0x300000, struct.pack(f"<{len(dl)}I", *dl))
        chip.wr8(0x302054, 2)
        time.sleep(0.2)
        img = chip.frame().convert("RGB")
    out = {}
    for kind, shape, r, k, x0, y0, box, cx, cy in cases:
        out[f"{kind},{shape},{r},{k}"] = {"x": x0, "y": y0, "core": [cx, cy],
                                           "px": [[img.getpixel((xx, yy))[0] for xx in range(x0, x0 + box)]
                                                  for yy in range(y0, y0 + box)]}
    out["_dl"] = dl
    Path(argv[1]).write_text(json.dumps(out))
    print(len(cases), "cases")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
