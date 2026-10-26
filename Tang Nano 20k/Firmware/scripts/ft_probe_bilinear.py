"""Замер на bt8xxemu: BILINEAR у BITMAP_SIZE против модели (ft_render_model.py), точка в точку.

Несколько битмапов в разных местах кадра — форматы, масштаб, поворот, дробный сдвиг, BORDER и
REPEAT, альфа и COLOR_RGB; каждый — свой прямоугольник кадра, расхождения считаются по ним.
Нужны bt8xxemu.dll (BT8XXEMU_DIR) и tools/ft812emu.py заставки Weather 2 (VDAC4).

    python ft_probe_bilinear.py OUT_DIR
"""
from __future__ import annotations

import random
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ft_render_model as model  # noqa: E402
from ft_golden import golden_frame  # noqa: E402
from ft_probe_math import BEGIN, BLEND, CLEAR, CLEAR_COLOR_RGB, COLOR_A, COLOR_RGB, END, \
    HANDLE, LAYOUT, SOURCE, V2F, w  # noqa: E402


def SIZE_F(wd, h, filt=1, wx=0, wy=0):
    return w(0x08, (filt << 20) | (wx << 19) | (wy << 18) | ((wd & 511) << 9) | (h & 511))


def TF(k, v, bits):
    return w(0x15 + k, v & ((1 << bits) - 1))       # BITMAP_TRANSFORM_A…F


def transform(a, b, c, d, e, f):
    return [TF(0, a, 17), TF(1, b, 17), TF(2, c, 24), TF(3, d, 17), TF(4, e, 17), TF(5, f, 24)]


def main(argv: list[str]) -> int:
    out = Path(argv[1])
    out.mkdir(parents=True, exist_ok=True)
    rnd = random.Random(1)
    # (имя, формат, ширина, высота, байт на тексель, данные, преобразование, обёртка, цвет,
    #  альфа, смешение, место (x, y), размер на экране)
    cases = []

    def tex16(n, mask=0xFFFF, force=0):
        return b"".join(struct.pack("<H", (rnd.randrange(65536) & mask) | force) for _ in range(n))

    def tex8(n):
        return bytes(rnd.randrange(256) for _ in range(n))

    cases.append(("RGB565 x3 BORDER", 7, 8, 8, 2, tex16(64), (85, 0, 0, 0, 85, 0), (0, 0),
                  (255, 255, 255), 255, (1, 0), (16, 16), (40, 40)))
    cases.append(("RGB565 x2.56 REPEAT, дробный сдвиг", 7, 8, 8, 2, tex16(64), (100, 0, 37, 0, 100, -61),
                  (1, 1), (255, 255, 255), 255, (1, 0), (96, 16), (60, 60)))
    cases.append(("ARGB4 x3.2, альфа, смешение по умолчанию", 6, 6, 5, 2, tex16(30), (80, 0, 0, 0, 80, 0),
                  (0, 0), (255, 255, 255), 255, (2, 4), (200, 16), (40, 40)))
    cases.append(("L8 поворот", 3, 16, 16, 1, tex8(256), (230, 60, 0, -60, 230, 0), (0, 0),
                  (255, 255, 255), 255, (1, 0), (16, 120), (64, 64)))
    cases.append(("ARGB1555 x4, COLOR_RGB, как цель DEMO3D", 0, 8, 6, 2, tex16(48), (64, 0, 0, 0, 64, 0),
                  (0, 0), (200, 150, 255), 255, (2, 4), (120, 120), (32, 24)))
    cases.append(("RGB565 x1.5 REPEAT, COLOR_A", 7, 4, 4, 2, tex16(16), (170, 0, 90, 0, 170, 200),
                  (1, 1), (255, 255, 255), 160, (2, 4), (200, 120), (48, 48)))

    ram = bytearray(1 << 16)
    dl = [CLEAR_COLOR_RGB(20, 40, 60), CLEAR()]
    addr = 0
    rects = []
    for h, (name, fmt, tw, th, bpt, data, tf, wrap, rgb, a, blend, pos, size) in enumerate(cases):
        ram[addr:addr + len(data)] = data
        dl += [HANDLE(h), SOURCE(addr), LAYOUT(fmt, tw * bpt, th), SIZE_F(size[0], size[1], 1, *wrap)]
        dl += transform(*tf)
        dl += [COLOR_RGB(*rgb), COLOR_A(a), BLEND(*blend), BEGIN(1), w(0x2E, h),   # BITMAPS
               V2F(pos[0] * 16, pos[1] * 16), END()]
        rects.append((name, pos, size))
        addr += (len(data) + 3) & ~3
    dl += [w(0x00)]                                                                # DISPLAY

    from PIL import Image, ImageChops
    ref = golden_frame(bytes(ram), dl).convert("RGB")
    ref.save(out / "bt8xxemu.png")
    mine = Image.frombytes("RGB", (1024, 768), model.Renderer(bytes(ram), model.rom_area_with_ramp()).render(dl))
    mine.save(out / "model.png")
    diff = ImageChops.difference(ref, mine)
    diff.point(lambda v: min(255, v * 32)).save(out / "diff.png")
    bad_total = 0
    for name, (x, y), (sw, sh) in rects:
        box = (x, y, x + sw, y + sh)
        d = list(diff.crop(box).getdata())
        bad = sum(1 for p in d if max(p) > 0)
        worst = max(max(p) for p in d)
        lit = sum(1 for p in ref.crop(box).getdata() if p != (20, 40, 60))
        bad_total += bad
        print(f"{name}: расходятся {bad} из {len(d)} (наибольшая разница {worst}), не фон {lit}")
    print("ЗАМЕР СОШЁЛСЯ" if bad_total == 0 else "ЕСТЬ РАСХОЖДЕНИЯ")
    return 0 if bad_total == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
