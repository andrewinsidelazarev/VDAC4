"""Данные проверки CMD_LOADIMAGE (ft_image.c): картинки и ожидаемые байты RAM_G с ptr.

JPEG — эталон libjpeg (решение пользователя 2026-10-20): декод Pillow (libjpeg-turbo, ДКП
islow, «гладкое» восстановление цветности), каналы усечены в RGB565. PNG — правила FT812
(bt8xxemu, fpga/vdac2/scripts/ft_probe_loadimage.py: 100 % совпадения): серый — L8, RGB —
RGB565, RGBA — ARGB4, палитра — PALETTED565 (с tRNS — PALETTED4444) с ptr, индексы за ней.

    python make_image_data.py          → data/image/*.jpg|png, *.exp, list.txt
    python make_image_data.py fit      → data/fit/*.jpg|png, list.txt (CRC32 ожидания)

Вписывание (OPT_FULLSCREEN, расширение VDAC4) — эталон Pillow (решение пользователя
2026-10-26, «автомасштаб до 1024*768 в bl616»): JPEG — Image.draft("RGB", вписанный размер)
(уменьшение при ДКП в 2/4/8 раз, libjpeg-turbo), затем resize(BILINEAR); PNG — resize(BILINEAR),
прозрачность — наложение на чёрный (paste с маской: DIV255(c·a)). Картинка — по центру холста
RGB565 1024×768 с чёрными полями. Холст — 1,5 МБ, поэтому ожидание — CRC32 вывода.
Случаи «draft» (флаг стенда OPT_TEST_DRAFT) — только уменьшение ДКП, вывод как есть.
Нужен Pillow с libjpeg-turbo 3.1.4.1 (E:/zx/esp/pio-venv: Pillow 12.3.0).
"""
import io
import random
import struct
import sys
import zlib
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
OUT = HERE / "data" / "image"
OUT_FIT = HERE / "data" / "fit"
OPT_FULLSCREEN = 8
OPT_TEST_DRAFT = 0x80000000


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


def fit_size(w, h):
    """Вписанный размер: широкая — во всю ширину 1024, узкая — во всю высоту 768."""
    if w * 768 >= h * 1024:
        ow, oh = 1024, (h * 1024 + w // 2) // w
    else:
        ow, oh = (w * 768 + h // 2) // h, 768
    return max(ow, 1), max(oh, 1)


def pack565(img):
    raw = img.tobytes()
    out = bytearray(len(raw) // 3 * 2)
    for i in range(len(raw) // 3):
        r, g, b = raw[3 * i], raw[3 * i + 1], raw[3 * i + 2]
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        out[2 * i] = v & 255
        out[2 * i + 1] = v >> 8
    return bytes(out)


def canvas(img):
    """RGB ow×oh → холст RGB565 1024×768, картинка по центру, поля чёрные."""
    ow, oh = img.size
    x0, y0 = (1024 - ow) // 2, (768 - oh) // 2
    body = pack565(img)
    out = bytearray(1024 * 768 * 2)
    for y in range(oh):
        o = ((y0 + y) * 1024 + x0) * 2
        out[o:o + 2 * ow] = body[2 * ow * y:2 * ow * (y + 1)]
    return bytes(out)


def jpeg_fit(data, draft_only=False):
    im = Image.open(io.BytesIO(data))
    ow, oh = fit_size(*im.size)
    im.draft("RGB", (ow, oh))
    im = im.convert("RGB")
    if draft_only:
        return pack565(im), (7, im.size[0], im.size[1], 2 * im.size[0], 0)
    if im.size != (ow, oh):
        im = im.resize((ow, oh), Image.BILINEAR)
    return canvas(im), (7, 1024, 768, 2048, 0)


def png_fit(data):
    im = Image.open(io.BytesIO(data))
    if im.mode in ("RGBA", "P"):
        rgba = im.convert("RGBA")
        rgb = Image.new("RGB", im.size, 0)
        rgb.paste(rgba.convert("RGB"), mask=rgba.getchannel("A"))
    else:
        rgb = im.convert("RGB")
    ow, oh = fit_size(*im.size)
    if rgb.size != (ow, oh):
        rgb = rgb.resize((ow, oh), Image.BILINEAR)
    return canvas(rgb), (7, 1024, 768, 2048, 0)


def picture_big(w, h, seed, noise=True):
    """Крупные картинки быстро: блоки малой picture() и шум."""
    big = picture(max(2, w // 23), max(2, h // 17), seed).resize((w, h), Image.NEAREST)
    if noise:
        big = Image.blend(big, Image.effect_noise((w, h), 60).convert("RGB"), 0.2)
    return big


def fit_cases():
    out = []
    jpegs = [
        # имя, размер, выборка, качество, вид, только ДКП
        ("d420s2", (2100, 1600), 2, 80, "RGB", True),
        ("d422s2", (2101, 1577), 1, 80, "RGB", True),
        ("d444s2", (2099, 1601), 0, 80, "RGB", True),
        ("dgrays2", (2050, 1700), 0, 80, "L", True),
        ("d420s4", (4300, 3300), 2, 70, "RGB", True),
        ("d422s4", (4301, 3301), 1, 70, "RGB", True),
        ("d420s8", (8300, 6300), 2, 60, "RGB", True),
        ("d422s8", (8299, 6301), 1, 60, "RGB", True),
        ("d444s8", (8200, 6200), 0, 60, "RGB", True),
        ("f420up", (64, 48), 2, 85, "RGB", False),
        ("f420odd", (37, 23), 2, 85, "RGB", False),
        ("f422tall", (100, 200), 1, 85, "RGB", False),
        ("fgraywide", (300, 100), 0, 85, "L", False),
        ("f444exact", (1024, 768), 0, 85, "RGB", False),
        ("f420hd", (1920, 1080), 2, 80, "RGB", False),
        ("f420max", (2047, 1535), 2, 80, "RGB", False),
        ("f420s2", (2100, 1600), 2, 80, "RGB", False),
        ("f422s4", (4300, 3000), 1, 70, "RGB", False),
        ("f420s8", (8400, 6300), 2, 60, "RGB", False),
        ("f1x1", (1, 1), 2, 75, "RGB", False),
        ("fstripe", (2047, 3), 2, 75, "RGB", False),
        ("fpole", (3, 2047), 2, 75, "RGB", False),
        ("frst", (200, 150), 2, 75, "RGB", False),
    ]
    for name, (w, h), sub, q, mode, draft in jpegs:
        img = picture(w, h, len(out)) if w * h <= 100000 else picture_big(w, h, len(out), w * h < 20_000_000)
        if mode == "L":
            img = img.convert("L")
        bio = io.BytesIO()
        extra = {"restart_marker_blocks": 5} if name == "frst" else {}
        img.save(bio, "JPEG", quality=q, subsampling=sub, **extra)
        data = bio.getvalue()
        exp, info = jpeg_fit(data, draft)
        out.append((name + ".jpg", data, exp, info, OPT_FULLSCREEN | (OPT_TEST_DRAFT if draft else 0)))
    for name, (w, h), mode in (("pfgray", (29, 17), "L"), ("pfrgb", (31, 19), "RGB"),
                               ("pfrgba", (23, 15), "RGBA"), ("pfpal", (27, 13), "P"),
                               ("pfpaltrns", (25, 11), "Ptrns"), ("pf640", (640, 480), "RGB"),
                               ("pfwide", (1500, 300), "RGB"), ("pfrgbabig", (2047, 900), "RGBA")):
        img = picture(w, h, 200 + len(out)) if w * h <= 100000 else picture_big(w, h, 200 + len(out))
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
        exp, info = png_fit(data)
        out.append((name + ".png", data, exp, info, OPT_FULLSCREEN))
    return out


def fit_main():
    OUT_FIT.mkdir(parents=True, exist_ok=True)
    lines = []
    for name, data, exp, info, opts in fit_cases():
        (OUT_FIT / name).write_bytes(data)
        crc = zlib.crc32(exp) & 0xFFFFFFFF
        lines.append(f"{name} crc:{crc:08x}:{len(exp)} " + " ".join(str(v) for v in info) + f" {opts:x}")
        print(lines[-1], flush=True)
    (OUT_FIT / "list.txt").write_text("\n".join(lines) + "\n")
    print(len(lines), "cases")


def main():
    if sys.argv[1:] == ["fit"]:
        fit_main()
        return
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
