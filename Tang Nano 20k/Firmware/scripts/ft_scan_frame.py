"""Проверочный кадр FT (RGB565) для tb_ft_scan: ft_frame.hex — строки по 512 слов, точка x —
слово x/2 (младшая половина — чётная точка), как у fb_line_ram.

    python ft_scan_frame.py W H OUT.hex

Рисунок различает соседние точки и строки: градиенты по x и y, шахматка 1 точка у краёв,
рамка в крайних точках и строках, диагональ — по ним видно и сдвиг окна, и шаг масштаба.
"""
from __future__ import annotations

import sys
from pathlib import Path


def pixel(x: int, y: int, w: int, h: int) -> int:
    if x in (0, w - 1) or y in (0, h - 1):
        return 0xFFFF                                   # рамка
    if x < 16 or y < 16:
        return 0xF800 if (x + y) & 1 else 0x001F        # шахматка у краёв
    if abs(x * h - y * w) < w:
        return 0x07E0                                   # диагональ
    r = (x * 31) // w
    g = (y * 63) // h
    b = ((x ^ y) >> 3) & 31
    return (r << 11) | (g << 5) | b


def main(argv: list[str]) -> int:
    w, h, out = int(argv[1]), int(argv[2]), Path(argv[3])
    lines = []
    for y in range(h):
        words = []
        for xw in range(512):
            lo = pixel(2 * xw, y, w, h) if 2 * xw < w else 0
            hi = pixel(2 * xw + 1, y, w, h) if 2 * xw + 1 < w else 0
            words.append(f"{(hi << 16) | lo:08x}")
        lines.append(" ".join(words))
    out.write_text("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
