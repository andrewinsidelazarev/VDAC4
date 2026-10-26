"""Данные проверки CMD_LOADIMAGE (ft_image.c): картинки и ожидаемые байты RAM_G с ptr.

JPEG — эталон libjpeg (решение пользователя 2026-10-20): декод Pillow (libjpeg-turbo, ДКП
islow, «гладкое» восстановление цветности), каналы усечены в RGB565. PNG — правила FT812
(bt8xxemu, fpga/vdac2/scripts/ft_probe_loadimage.py: 100 % совпадения): серый — L8, RGB —
RGB565, RGBA — ARGB4, палитра — PALETTED565 (с tRNS — PALETTED4444) с ptr, индексы за ней.

    python make_image_data.py          → data/image/*.jpg|png, *.exp, list.txt
"""
import io
import random
import struct
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
OUT = HERE / "data" / "image"


def picture(w, h, seed):
    """Плавные цвета, полосы, шум и резкие края — всё, на чём ДКП и цветность ошибаются."""
    rnd = random.Random(seed)
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            r = (x * 255) // max(1, w - 1)
            g = (y * 255) // max(1, h - 1)
            b = ((x ^ y) * 37) & 255 if (x // 4 + y // 3) % 5 else 255 - r
            if (x // 11 + y // 7) % 9 == 0:
                r, g, b = 255, 255, 0
            n = rnd.randint(-30, 30)
            px[x, y] = (max(0, min(255, r + n)), max(0, min(255, g - n)), max(0, min(255, b + n // 2)))
    return img


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def argb4(a, r, g, b):
    return ((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)


def jpeg_cases():
    out = []
    specs = [
        ("j444", (48, 32), 0, 90, "RGB", {}),
        ("j422", (48, 32), 1, 85, "RGB", {}),
        ("j420", (48, 32), 2, 75, "RGB", {}),
        ("j420odd", (37, 23), 2, 80, "RGB", {"optimize": True}),
        ("j422odd", (35, 21), 1, 80, "RGB", {}),
        ("j444odd", (19, 13), 0, 95, "RGB", {"optimize": True}),
        ("jgray", (40, 24), 0, 85, "L", {}),
        ("jgrayodd", (21, 11), 0, 70, "L", {"optimize": True}),
        ("j420big", (160, 120), 2, 60, "RGB", {"optimize": True}),
        ("j420q100", (64, 48), 2, 100, "RGB", {}),
        ("j420q10", (64, 48), 2, 10, "RGB", {}),
        ("j420rst", (80, 48), 2, 75, "RGB", {"restart_marker_blocks": 3}),
        ("j444rst", (40, 40), 0, 75, "RGB", {"restart_marker_rows": 1}),
        ("j1x1", (1, 1), 2, 75, "RGB", {}),
        ("j2x17", (2, 17), 2, 75, "RGB", {}),
        ("j17x2", (17, 2), 2, 75, "RGB", {}),
        ("j640", (640, 48), 2, 85, "RGB", {}),
    ]
    for name, (w, h), sub, q, mode, extra in specs:
        img = picture(w, h, len(out))
        if mode == "L":
            img = img.convert("L")
        bio = io.BytesIO()
        try:
            img.save(bio, "JPEG", quality=q, subsampling=sub, **extra)
        except TypeError:
            continue                    # старый Pillow без restart_marker_*
        data = bio.getvalue()
        dec = Image.open(io.BytesIO(data)).convert("RGB")
        exp = b"".join(struct.pack("<H", rgb565(*dec.getpixel((x, y)))) for y in range(h) for x in range(w))
        out.append((name + ".jpg", data, exp, (7, w, h, 2 * w, 0)))
    return out


def png_cases():
    out = []
    for name, (w, h), mode in (("pgray", (29, 17), "L"), ("prgb", (31, 19), "RGB"),
                               ("prgba", (23, 15), "RGBA"), ("ppal", (27, 13), "P"),
                               ("ppaltrns", (25, 11), "Ptrns"), ("prgbbig", (300, 40), "RGB")):
        img = picture(w, h, 100 + len(out))
        bio = io.BytesIO()
        if mode == "RGBA":
            img = img.convert("RGBA")
            img.putalpha(img.split()[0].point(lambda v: (v * 7) & 255))
            img.save(bio, "PNG")
        elif mode.startswith("P"):
            pal = img.quantize(colors=200 if mode == "P" else 64)
            if mode == "Ptrns":
                pal.save(bio, "PNG", transparency=bytes((i * 4) & 255 for i in range(64)))
            else:
                pal.save(bio, "PNG")
        else:
            img.convert(mode).save(bio, "PNG")
        data = bio.getvalue()
        dec = Image.open(io.BytesIO(data))
        if dec.mode == "L":
            exp = bytes(dec.getpixel((x, y)) for y in range(h) for x in range(w))
            info = (3, w, h, w, 0)
        elif dec.mode == "RGB":
            exp = b"".join(struct.pack("<H", rgb565(*dec.getpixel((x, y)))) for y in range(h) for x in range(w))
            info = (7, w, h, 2 * w, 0)
        elif dec.mode == "RGBA":
            exp = b"".join(struct.pack("<H", argb4(p[3], p[0], p[1], p[2]))
                           for p in (dec.getpixel((x, y)) for y in range(h) for x in range(w)))
            info = (6, w, h, 2 * w, 0)
        else:                            # P
            pal = dec.getpalette()
            n = len(dec.palette.palette) // 3
            trns = dec.info.get("transparency")
            pb = b""
            for i in range(n):
                r, g, b = pal[3 * i:3 * i + 3]
                if trns is not None:
                    a = trns[i] if isinstance(trns, (bytes, bytearray)) and i < len(trns) else 255
                    pb += struct.pack("<H", argb4(a, r, g, b))
                else:
                    pb += struct.pack("<H", rgb565(r, g, b))
            exp = pb + bytes(dec.getpixel((x, y)) for y in range(h) for x in range(w))
            info = (15 if trns is not None else 14, w, h, w, len(pb))
        out.append((name + ".png", data, exp, info))
    return out


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    lines = []
    for name, data, exp, info in jpeg_cases() + png_cases():
        (OUT / name).write_bytes(data)
        stem = name.rsplit(".", 1)[0]
        (OUT / (stem + ".exp")).write_bytes(exp)
        lines.append(f"{name} {stem}.exp " + " ".join(str(v) for v in info))
    (OUT / "list.txt").write_text("\n".join(lines) + "\n")
    print(len(lines), "cases")


if __name__ == "__main__":
    main()
