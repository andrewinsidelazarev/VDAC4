"""Сравнение входной картинки и кадра, декодированного тестбенчем из HDMI (TMDS).

    py -3 scripts/fb_compare.py sim/hib.hex sim/r0/fb_out_07.ppm итог.png [подпись]

Эталон строится так же, как его подаёт модель Evo в tb_ts_fb.sv: картинка из .hex,
номер кадра в первых 8 точках строки 0 и маркер кадра внизу по бордюру; номер кадра
берётся из имени файла. Итог: слева вход, справа выход, внизу карта расхождений
(красное — отличающиеся точки) и их число.
"""
import re
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

W, H = 720, 576


def expand(pal, v):
    if pal:
        return v << 3
    return 255 if v >= 24 else (v * 255 + 12) // 24


def reference(hex_path, f):
    words = [int(x, 16) for x in Path(hex_path).read_text().split()]
    out = []
    mx = 16 + (f * 16) % 656
    for y in range(H):
        for x in range(W):
            if y == 0 and x < 8:
                r, g, b, pal = f & 31, (f >> 5) & 31, (~f) & 31, 1
            elif 540 <= y < 564 and mx <= x < mx + 24:
                w = 0x7FFF if f % 2 else (24 << 5) | 24
                r, g, b, pal = w & 31, (w >> 5) & 31, (w >> 10) & 31, w >> 15
            else:
                w = words[y * W + x]
                r, g, b, pal = w & 31, (w >> 5) & 31, (w >> 10) & 31, w >> 15
            out.append((expand(pal, r), expand(pal, g), expand(pal, b)))
    img = Image.new("RGB", (W, H))
    img.putdata(out)
    return img


def main():
    hex_path, ppm_path, out_path = sys.argv[1:4]
    title = sys.argv[4] if len(sys.argv) > 4 else ""
    f = int(re.search(r"(\d+)\.ppm$", ppm_path).group(1))
    ref = reference(hex_path, f)
    got = Image.open(ppm_path).convert("RGB")
    diff = Image.new("RGB", (W, H))
    bad = 0
    dp = []
    for a, b in zip(ref.getdata(), got.getdata()):
        if a != b:
            bad += 1
            dp.append((255, 0, 0))
        else:
            dp.append(tuple(c // 4 for c in b))
    diff.putdata(dp)

    pad = 28
    sheet = Image.new("RGB", (W * 2 + 20, H * 2 + 3 * pad + 20), (32, 32, 32))
    d = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("arial.ttf", 16)       # кириллица есть в шрифтах Windows
    except OSError:
        font = ImageFont.load_default()
    d.text((10, 8), f"Вход: картинка Evo (кадр {f})", fill=(255, 255, 255), font=font)
    d.text((W + 30, 8), "Выход: HDMI 720x576p50, декодировано из TMDS", fill=(255, 255, 255), font=font)
    sheet.paste(ref, (10, pad))
    sheet.paste(got, (W + 30, pad))
    d.text((10, H + pad + 8), f"Расхождения: {bad} точек из {W * H}" + (f" — {title}" if title else ""),
           fill=(255, 120, 120) if bad else (120, 255, 120), font=font)
    sheet.paste(diff, (10, H + 2 * pad))
    sheet.save(out_path)
    print(f"кадр {f}: расхождений {bad}")


if __name__ == "__main__":
    main()
