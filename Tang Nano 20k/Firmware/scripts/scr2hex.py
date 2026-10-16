"""Экран ZX Spectrum (.scr, 6912 байт) → входная картинка тестбенча кадрового буфера.

Видимая область TS-Config/Pentagon — 360×288 точек ТВ-растра: экран 256×192 с бордюром
52 точки слева/справа и 48 строк сверху/снизу. После VGA-удвоения это окно 720×576,
которое и захватывает прошивка. Каждый пиксель — слово {PAL_SEL, B5, G5, R5}, как его
собирает ts_mux_capture. Цвета ZX: обычная яркость — 18 (≈191 после палитры TS),
яркие — 24 (255); PAL_SEL = 0.

    py -3 scripts/scr2hex.py экран.scr картинка.hex [цвет_бордюра 0..7] [эталон.png]
"""
import sys
from pathlib import Path

W, H = 720, 576
PAPER_X, PAPER_Y = 52, 48          # в точках ТВ-растра


def zx_rgb5(color, bright):
    level = 24 if bright else 18
    b = level if color & 1 else 0
    r = level if color & 2 else 0
    g = level if color & 4 else 0
    return r, g, b


def word(r, g, b, pal=0):
    return (pal << 15) | (b << 10) | (g << 5) | r


def expand(v):                     # как legacy_palette при PAL_SEL = 0
    return 255 if v >= 24 else (v * 255 + 12) // 24


def render(scr, border):
    tv = [[zx_rgb5(border, False)] * 360 for _ in range(288)]
    for y in range(192):
        # адрес строки экрана ZX: 010T TSSS LLLC CCCC
        row = ((y & 0xC0) << 5) | ((y & 0x07) << 8) | ((y & 0x38) << 2)
        for cx in range(32):
            bits = scr[row + cx]
            attr = scr[6144 + (y // 8) * 32 + cx]
            ink, paper, bright = attr & 7, (attr >> 3) & 7, bool(attr & 0x40)
            for bit in range(8):
                on = bits & (0x80 >> bit)
                tv[PAPER_Y + y][PAPER_X + cx * 8 + bit] = zx_rgb5(ink if on else paper, bright)
    # VGA-удвоение по обеим осям
    return [[tv[y // 2][x // 2] for x in range(W)] for y in range(H)]


def main():
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    border = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    scr = src.read_bytes()
    if len(scr) != 6912:
        raise SystemExit("ожидался .scr на 6912 байт")
    img = render(scr, border)
    dst.write_text("\n".join(f"{word(*img[y][x]):04x}" for y in range(H) for x in range(W)) + "\n")
    if len(sys.argv) > 4:
        from PIL import Image
        png = Image.new("RGB", (W, H))
        png.putdata([tuple(expand(c) for c in img[y][x]) for y in range(H) for x in range(W)])
        png.save(sys.argv[4])


if __name__ == "__main__":
    main()
