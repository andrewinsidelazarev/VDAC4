"""Эталон вывода кадра FT812 через масштаб 720p (fb_scanout, ft = 1) против tb_ft_scan.

    python ft_scan_check.py DIR [ASPECT SIZE]   # DIR: ft_frame.hex (вход) и scan_out.hex (выход RTL)

Эталон повторяет развёртку дословно: счётчики N/D (шаг 1 или 2 точки при уменьшении),
вес правой и нижней точки по доле за последней границей (1/16), смесь 2×2, RGB565 → 8 бит
повтором старших разрядов. Пишет scan_ref.png и scan_rtl.png.
"""
from __future__ import annotations

import sys
from pathlib import Path

WIN = {  # (aspect, size) → x0, x1, y0, y1
    (0, 0): (160, 1120, 0, 720), (0, 1): (184, 1096, 18, 702), (0, 2): (208, 1072, 36, 684),
    (1, 0): (0, 1280, 0, 720), (1, 1): (32, 1248, 18, 702), (1, 2): (64, 1216, 36, 684),
}
FRAC_1024 = {(0, 0): (16, 15, 16, 15), (0, 1): (64, 57, 64, 57), (0, 2): (32, 27, 32, 27),
             (1, 0): (4, 5, 16, 15), (1, 1): (16, 19, 64, 57), (1, 2): (8, 9, 32, 27)}
FRAC_640 = {(0, 0): (2, 3, 2, 3), (0, 1): (40, 57, 40, 57), (0, 2): (20, 27, 20, 27),
            (1, 0): (1, 2, 2, 3), (1, 1): (10, 19, 40, 57), (1, 2): (5, 9, 20, 27)}


def recip(n):
    return (65536 + n // 2) // n


def dda(acc, n, d):
    s = acc + n
    if s >= 2 * d:
        return 2, s - 2 * d
    if s >= d:
        return 1, s - d
    return 0, s


def over_part(acc, n, d):
    o = acc + n
    if o >= 2 * d:
        return o - 2 * d
    if o > d:
        return o - d
    return 0


def weight(part, rc):
    return 0 if part == 0 else min(16, (part * rc + 2048) >> 12)


def main(argv):
    d = Path(argv[1])
    asp = int(argv[2]) if len(argv) > 2 else 0
    size = int(argv[3]) if len(argv) > 3 else 0
    words = [int(v, 16) for v in (d / "ft_frame.hex").read_text().split()]
    src_w, src_h = 1024, len(words) // 512

    def px(x, y):
        w = words[y * 512 + (x >> 1)]
        p = (w >> 16) if x & 1 else (w & 0xFFFF)
        r5, g6, b5 = p >> 11, (p >> 5) & 63, p & 31
        return ((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2))

    x0, x1, y0, y1 = WIN[(asp, size)]
    nx, dx, ny, dy = (FRAC_1024 if src_w == 1024 else FRAC_640)[(asp, size)]
    rx, ry = recip(nx), recip(ny)
    ref = []
    sy, sya = 0, 0
    for v in range(720):
        row = []
        if y0 <= v < y1:
            if v > y0:
                st, sya = dda(sya, ny, dy)
                sy += st
            wy = weight(over_part(sya, ny, dy), ry)
            syb = sy if sy >= src_h - 1 else sy + 1
            sx, sxa = 0, 0
        for h in range(1280):
            if y0 <= v < y1 and x0 <= h < x1:
                if h > x0:
                    st, sxa = dda(sxa, nx, dx)
                    sx += st
                wx = weight(over_part(sxa, nx, dx), rx)
                sxr = (sx + 1) & 1023
                out = []
                for c in range(3):
                    tl, tr = px(sx, sy)[c], px(sxr, sy)[c]
                    bl, br = px(sx, syb)[c], px(sxr, syb)[c]
                    top = 16 * tl + (tr - tl) * wx
                    bot = 16 * bl + (br - bl) * wx
                    out.append(((16 * top + (bot - top) * wy + 128) >> 8) & 255)
                row.append(tuple(out))
            else:
                row.append((0, 0, 0))
        ref.append(row)
    rtl = [[(int(t[0:2], 16), int(t[2:4], 16), int(t[4:6], 16)) for t in line.split()]
           for line in (d / "scan_out.hex").read_text().split("\n") if line.strip()]
    bad = 0
    first = []
    for v in range(720):
        for h in range(1280):
            a = rtl[v][h] if v < len(rtl) and h < len(rtl[v]) else None
            if a != ref[v][h]:
                bad += 1
                if len(first) < 10:
                    first.append((h, v, a, ref[v][h]))
    try:
        from PIL import Image
        for name, img in (("scan_ref.png", ref), ("scan_rtl.png", rtl)):
            im = Image.new("RGB", (1280, 720))
            im.putdata([p for row in img[:720] for p in (row + [(0, 0, 0)] * 1280)[:1280]])
            im.save(d / name)
    except ImportError:
        pass
    print(f"строк RTL {len(rtl)}, точек расходится {bad}")
    for f in first:
        print("  ", f)
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
