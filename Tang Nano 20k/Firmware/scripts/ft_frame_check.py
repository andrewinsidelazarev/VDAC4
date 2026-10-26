"""Кадр FT в RGB565 (снимок стенда tb/vsys --dump или ft_frame.hex tb_ft_render) против модели
(ft_render_model.py): точки модели (RGBA 8 бит) переводятся в RGB565 так же, как буфер плитки
ft_rd_pix при выгрузке (упорядоченный дизеринг 4×4 по координатам экрана, REG_DITHER = 1).
Быстрая сверка RTL с моделью без долгого tb_ft_render в iverilog.

    python ft_frame_check.py FRAME.hex RAMG.bin DL.bin [--no-dither] [--lines N]

FRAME.hex — 768 строк × 512 слов, слово на строку (младшая половина — левая точка).
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ft_render_model as model  # noqa: E402

BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]   # [y][x], как bayer() ft_rd_pix


def to565(r: int, g: int, b: int, t: int, d: bool) -> int:
    r9 = r + ((t >> 1) if d else 0)
    g9 = g + ((t >> 2) if d else 0)
    b9 = b + ((t >> 1) if d else 0)
    r5 = 31 if r9 > 255 else r9 >> 3
    g6 = 63 if g9 > 255 else g9 >> 2
    b5 = 31 if b9 > 255 else b9 >> 3
    return (r5 << 11) | (g6 << 5) | b5


def main(argv: list[str]) -> int:
    args = [a for a in argv[1:] if not a.startswith("--")]
    dither = "--no-dither" not in argv
    n_lines = 768
    if "--lines" in argv:
        n_lines = int(argv[argv.index("--lines") + 1])
        args = [a for a in args if a != str(n_lines)]
    frame, ramg, dlp = Path(args[0]), Path(args[1]), Path(args[2])
    words = [int(v, 16) for v in frame.read_text().split() if v.strip() and not v.startswith("@")]
    if len(words) < 768 * 512:
        print(f"{frame}: слов {len(words)}, нужно 393216")
        return 2
    ram = ramg.read_bytes()
    raw = dlp.read_bytes()[:8192]
    dl = list(struct.unpack(f"<{len(raw) // 4}I", raw)) + [0] * (2048 - len(raw) // 4)
    r = model.Renderer(ram, model.rom_area_with_ramp())
    bad_lines = bad_px = 0
    first = []
    for y in range(n_lines):
        color, _ = r.line(dl, y)
        bad = 0
        for x in range(1024):
            w = words[y * 512 + x // 2]
            got = (w >> 16) & 0xFFFF if x & 1 else w & 0xFFFF
            c = color[4 * x:4 * x + 4]
            exp = to565(c[0], c[1], c[2], BAYER[y & 3][x & 3], dither)
            if got != exp:
                bad += 1
                if len(first) < 12:
                    first.append((x, y, got, exp, tuple(c)))
        if bad:
            bad_lines += 1
            bad_px += bad
    print(f"строк {n_lines}: расходятся {bad_lines}, точек {bad_px}")
    for x, y, got, exp, c in first:
        print(f"  ({x}, {y}): кадр {got:04x}, модель {exp:04x} (RGBA {c})")
    return 0 if bad_px == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
