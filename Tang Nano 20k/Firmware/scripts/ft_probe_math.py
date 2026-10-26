"""Замеры арифметики FT812 на bt8xxemu: как тексель умножается на цвет и COLOR_A, как
раскрываются каналы 4 бит, как округляется смешение. Каждый опыт — своя строка кадра
(через 4 строки), результат — в JSON и сверка с формулами-кандидатами.

    python ft_probe_math.py OUT.json
"""
from __future__ import annotations

import json
import struct
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
W2 = HERE.parents[2] / "VDAC4" / "ESP32 S3 Zero N8R8" / "Network" / "Weather screensaver VDAC2"
sys.path.insert(0, str(W2 / "tools"))


def w(op, v=0): return (op << 24) | v
def BEGIN(p): return w(0x1F, p)
def END(): return w(0x21)
def CLEAR_COLOR_RGB(r, g, b): return w(0x02, (r << 16) | (g << 8) | b)
def CLEAR(): return w(0x26, 7)
def COLOR_RGB(r, g, b): return w(0x04, (r << 16) | (g << 8) | b)
def COLOR_A(a): return w(0x10, a)
def BLEND(s, d): return w(0x0B, (s << 3) | d)
def HANDLE(h): return w(0x05, h)
def SOURCE(a): return w(0x01, a)
def LAYOUT(fmt, stride, h): return w(0x07, (fmt << 19) | (stride << 9) | h)
def SIZE(wd, h): return w(0x08, (wd << 9) | h)
def V2F(x, y): return (1 << 30) | ((x & 0x7FFF) << 15) | (y & 0x7FFF)     # в 1/16


def bitmap_row(handle, src, fmt, stride, width, y, color, a, blend=(2, 4), under=None):
    out = []
    if under is not None:                   # подложка: строка цвета under, без смешения
        out += [COLOR_RGB(*under), COLOR_A(255), BLEND(1, 0), w(0x0E, 0), BEGIN(9),
                V2F(0, y * 16), V2F(width * 16 - 16, y * 16), END()]
    out += [COLOR_RGB(*color), COLOR_A(a), BLEND(*blend), HANDLE(handle), SOURCE(src),
            LAYOUT(fmt, stride, 1), SIZE(width, 1), BEGIN(1), V2F(0, y * 16), END()]
    return out


def main(argv: list[str]) -> int:
    import ft812emu
    # RAM_G: 0 — L8 0…255; 0x1000 — L4 0…15; 0x2000 — ARGB4: альфа 0…15 при белом цвете
    ram = {0: bytes(range(256)),
           0x1000: bytes(((2 * i) << 4) | (2 * i + 1) for i in range(8)),
           0x2000: b"".join(struct.pack("<H", (a << 12) | 0xFFF) for a in range(16))}
    dl = [CLEAR_COLOR_RGB(0, 0, 0), CLEAR(), w(0x27, 4)]
    tests = []
    y = 2
    for a in (255, 254, 253, 200, 129, 128, 127, 64, 3):
        dl += bitmap_row(1, 0, 3, 256, 256, y, (255, 255, 255), a)
        tests.append({"name": f"L8 white A={a} over black", "y": y, "w": 256, "a": a, "c": 255, "d": 0})
        y += 4
    for c in (254, 248, 200, 128, 1):
        dl += bitmap_row(1, 0, 3, 256, 256, y, (c, c, c), 255)
        tests.append({"name": f"L8 color={c} A=255 over black", "y": y, "w": 256, "a": 255, "c": c, "d": 0})
        y += 4
    for c, a in ((248, 254), (200, 200), (128, 100)):
        dl += bitmap_row(1, 0, 3, 256, 256, y, (c, c, c), a)
        tests.append({"name": f"L8 color={c} A={a} over black", "y": y, "w": 256, "a": a, "c": c, "d": 0})
        y += 4
    for d in (255, 35, 100):
        dl += bitmap_row(1, 0, 3, 256, 256, y, (248, 248, 248), 255, under=(d, d, d))
        tests.append({"name": f"L8 color=248 A=255 over {d}", "y": y, "w": 256, "a": 255, "c": 248, "d": d})
        y += 4
    dl += bitmap_row(2, 0x1000, 2, 8, 16, y, (255, 255, 255), 255)
    tests.append({"name": "L4 white A=255 over black", "y": y, "w": 16}); y += 4
    dl += bitmap_row(3, 0x2000, 6, 32, 16, y, (255, 255, 255), 255)
    tests.append({"name": "ARGB4 alpha 0..15 white over black", "y": y, "w": 16}); y += 4
    with ft812emu.FT812() as chip:
        chip.boot()
        for addr, data in ram.items():
            chip.write(addr, data)
        dl.append(0)
        chip.write(0x300000, struct.pack(f"<{len(dl)}I", *dl))
        chip.wr8(0x302054, 2)
        time.sleep(0.2)
        img = chip.frame().convert("RGB")
    for t in tests:
        t["r"] = [img.getpixel((x, t["y"]))[0] for x in range(t["w"])]
    Path(argv[1]).write_text(json.dumps(tests))
    for t in tests:
        print(t["name"], t["r"][:20], "...", t["r"][-4:])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
