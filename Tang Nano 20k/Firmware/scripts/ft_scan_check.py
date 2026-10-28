"""Эталон вывода кадра FT812 через масштаб 720p (fb_scanout, ft = 1) против tb_ft_scan.

    python ft_scan_check.py DIR [ASPECT SIZE [SRC_W [video|768]]]   # DIR: ft_frame.hex (вход) и scan_out.hex (выход RTL)

SRC_W — ширина кадра FT (по умолчанию 1024); 512 — кадр видео 512×384 (fb_scanout с FT_W512).
Высота — по числу строк ft_frame.hex (строка — 512 слов). video — ядро видео (FT_W512): холст
16:9 (высота меньше 3/4 ширины: 512×288, 640×360, 1024×576). С 1.39 окно у всех холстов — по
ASPECT и SIZE, холст растянут на него: шаг по x — как у холста 4:3 той же ширины, по y у холста
16:9 — по его высоте (в 1.36–1.38 холст 16:9 шёл окном 16:9 при любом ASPECT).
768 — вывод ядра FT812 с 1.41: 1366×768, окна и шаги OUT768.

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
FRAC_512 = {(0, 0): (8, 15, 8, 15), (0, 1): (32, 57, 32, 57), (0, 2): (16, 27, 16, 27),
            (1, 0): (2, 5, 8, 15), (1, 1): (8, 19, 32, 57), (1, 2): (4, 9, 16, 27)}
# вывод 1366×768 (fb_scanout OUT768, ядро FT812 с 1.41)
WIN768 = {(0, 0): (171, 1195, 0, 768), (0, 1): (195, 1171, 18, 750), (0, 2): (219, 1147, 36, 732),
          (1, 0): (3, 1363, 0, 768), (1, 1): (35, 1331, 18, 750), (1, 2): (67, 1299, 36, 732)}
FRAC768_1024 = {(0, 0): (1, 1, 1, 1), (0, 1): (64, 61, 64, 61), (0, 2): (32, 29, 32, 29),
                (1, 0): (64, 85, 1, 1), (1, 1): (64, 81, 64, 61), (1, 2): (64, 77, 32, 29)}
FRAC768_640 = {(0, 0): (5, 8, 5, 8), (0, 1): (40, 61, 40, 61), (0, 2): (20, 29, 20, 29),
               (1, 0): (8, 17, 5, 8), (1, 1): (40, 81, 40, 61), (1, 2): (40, 77, 20, 29)}
# холсты 16:9 ядра видео: ширина → (size → шаг по y; по x — как у холста 4:3 той же ширины)
FRAC_WIDE = {512: {0: (2, 5), 1: (8, 19), 2: (4, 9)}, 640: {0: (1, 2), 1: (10, 19), 2: (5, 9)},
             1024: {0: (4, 5), 1: (16, 19), 2: (8, 9)}}


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
    src_w, src_h = (int(argv[4]) if len(argv) > 4 else 1024), len(words) // 512

    def px(x, y):
        w = words[y * 512 + (x >> 1)]
        p = (w >> 16) if x & 1 else (w & 0xFFFF)
        r5, g6, b5 = p >> 11, (p >> 5) & 63, p & 31
        return ((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2))

    video = len(argv) > 5 and argv[5] == "video"
    o768 = len(argv) > 5 and argv[5] == "768"
    ow, oh = (1366, 768) if o768 else (1280, 720)
    wide = video and src_h * 4 < src_w * 3
    if o768:
        x0, x1, y0, y1 = WIN768[(asp, size)]
        nx, dx, ny, dy = (FRAC768_640 if src_w <= 640 else FRAC768_1024)[(asp, size)]
    else:
        x0, x1, y0, y1 = WIN[(asp, size)]
        nx, dx, ny, dy = (FRAC_512 if src_w <= 512 else FRAC_640 if src_w <= 640 else FRAC_1024)[(asp, size)]
        if wide:
            ny, dy = FRAC_WIDE[512 if src_w <= 512 else 640 if src_w <= 640 else 1024][size]
    rx, ry = recip(nx), recip(ny)
    ref = []
    sy, sya = 0, 0
    for v in range(oh):
        row = []
        if y0 <= v < y1:
            if v > y0:
                st, sya = dda(sya, ny, dy)
                sy += st
            wy = weight(over_part(sya, ny, dy), ry)
            syb = sy if sy >= src_h - 1 else sy + 1
            sx, sxa = 0, 0
        for h in range(ow):
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
    for v in range(oh):
        for h in range(ow):
            a = rtl[v][h] if v < len(rtl) and h < len(rtl[v]) else None
            if a != ref[v][h]:
                bad += 1
                if len(first) < 10:
                    first.append((h, v, a, ref[v][h]))
    try:
        from PIL import Image
        for name, img in (("scan_ref.png", ref), ("scan_rtl.png", rtl)):
            im = Image.new("RGB", (ow, oh))
            im.putdata([p for row in img[:oh] for p in (row + [(0, 0, 0)] * ow)[:ow]])
            im.save(d / name)
    except ImportError:
        pass
    print(f"строк RTL {len(rtl)}, точек расходится {bad}")
    for f in first:
        print("  ", f)
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
