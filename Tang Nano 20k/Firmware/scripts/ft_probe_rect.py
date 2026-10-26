"""Замер на bt8xxemu: вертикальный профиль RECTS при разной высоте ядра h (y1 − y0, в 1/16
пикселя) и радиусе r. Белый, COLOR_A 255, поверх чёрного: яркость строки — покрытие.
Выход — JSON {"r,h,k": [покрытие строк от y0 − 3 пикселя до y1 + 3]}.

    python ft_probe_rect.py OUT.json
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


def main(argv: list[str]) -> int:
    import ft812emu
    dl = [CLEAR_COLOR_RGB(0, 0, 0), CLEAR(), w(0x27, 4), COLOR_RGB(255, 255, 255), COLOR_A(255), BLEND(2, 4)]
    cases = []
    x, y = 8, 6
    for r in (4, 8, 12, 16):
        for h in (0, 1, 2, 4, 8, 15, 16, 17, 32):
            for k in (0, 5, 8):
                y0 = y * 16 + k
                dl += [w(0x0E, r), BEGIN(9), V2F(x * 16, y0), V2F((x + 3) * 16, y0 + h), END()]
                cases.append((r, h, k, x + 1, y, y0))
                x += 8
                if x > 1000:
                    x = 8
                    y += 9
        x = 8
        y += 9
    assert y < 760
    with ft812emu.FT812() as chip:
        chip.boot()
        dl.append(0)
        chip.write(0x300000, struct.pack(f"<{len(dl)}I", *dl))
        chip.wr8(0x302054, 2)
        time.sleep(0.2)
        img = chip.frame().convert("RGB")
    out = {}
    for r, h, k, px, py, y0 in cases:
        out[f"{r},{h},{k}"] = {"y0": y0, "rows": {str(row): img.getpixel((px, row))[0]
                                                  for row in range(py - 3, py + 5)}}
    Path(argv[1]).write_text(json.dumps(out))
    for key, v in out.items():
        print(key, "y0=%d" % v["y0"], [v["rows"][r] for r in sorted(v["rows"], key=int)])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
