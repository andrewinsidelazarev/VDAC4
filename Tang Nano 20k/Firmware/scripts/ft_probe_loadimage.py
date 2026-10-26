"""Замер на bt8xxemu: что сопроцессор FT812 делает на CMD_LOADIMAGE — эталон для BL616.

Картинки — свои, из Pillow (воспроизводимые): JPEG baseline (оттенки серого, 4:4:4, 4:2:2,
4:2:0, размеры не кратные блоку, оптимизированные таблицы Хаффмана) и PNG 8 бит (типы 0, 2,
3 — с tRNS и без, 6). Для каждой: CMD_DLSTART, CMD_LOADIMAGE(ptr, 0) и данные (до кратного
4), CMD_GETPROPS; затем слова RAM_DL до REG_CMD_DL, ответ GETPROPS и RAM_G от ptr до конца
картинки (с палитрой). Выход — каталог: NAME.jpg|png (вход), NAME.json (слова листа,
GETPROPS, формат из BITMAP_LAYOUT), NAME.ramg (байты RAM_G).

    python ft_probe_loadimage.py OUT_DIR
"""
from __future__ import annotations

import io
import json
import random
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from ft_probe_math import W2  # noqa: E402

sys.path.insert(0, str(W2 / "tools"))

CMD_DLSTART, CMD_LOADIMAGE, CMD_GETPROPS = 0xFFFFFF00, 0xFFFFFF24, 0xFFFFFF25
REG_CMD_DL, REG_CMD_READ = 0x302100, 0x3020F8
RAM_CMD = 0x308000
PTR = 0x1000


def picture(w, h, seed, mode="RGB"):
    """Плавные цвета, полосы и шум: и низкие, и высокие частоты блоков 8×8."""
    from PIL import Image
    rnd = random.Random(seed)
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            r = (x * 255) // max(1, w - 1)
            g = (y * 255) // max(1, h - 1)
            b = ((x ^ y) * 37) & 255 if (x // 4 + y // 3) % 5 else 255 - r
            n = rnd.randint(-24, 24)
            px[x, y] = (max(0, min(255, r + n)), max(0, min(255, g - n)), max(0, min(255, b + n // 2)))
    return img.convert(mode) if mode != "RGB" else img


def cases():
    from PIL import Image
    out = []
    for name, (w, h), sub, q, mode, opt in (
            ("j444", (48, 32), 0, 90, "RGB", False),
            ("j422", (48, 32), 1, 85, "RGB", False),
            ("j420", (48, 32), 2, 75, "RGB", False),
            ("j420odd", (37, 23), 2, 80, "RGB", True),
            ("j444odd", (19, 13), 0, 95, "RGB", True),
            ("jgray", (40, 24), 0, 85, "L", False),
            ("jgrayodd", (21, 11), 0, 70, "L", True),
            ("j420big", (128, 96), 2, 60, "RGB", True)):
        bio = io.BytesIO()
        picture(w, h, len(out), mode).save(bio, "JPEG", quality=q, subsampling=sub, optimize=opt)
        out.append((name + ".jpg", bio.getvalue()))
    for name, (w, h), mode in (("pgray", (29, 17), "L"), ("prgb", (31, 19), "RGB"),
                               ("prgba", (23, 15), "RGBA"), ("ppal", (27, 13), "P"),
                               ("ppaltrns", (25, 11), "Ptrns")):
        img = picture(w, h, 100 + len(out))
        bio = io.BytesIO()
        if mode == "RGBA":
            img = img.convert("RGBA")
            a = img.split()[0].point(lambda v: (v * 7) & 255)
            img.putalpha(a)
            img.save(bio, "PNG")
        elif mode.startswith("P"):
            pal = img.quantize(colors=200 if mode == "P" else 64)
            if mode == "Ptrns":
                pal.info["transparency"] = bytes((i * 4) & 255 for i in range(64))
                pal.save(bio, "PNG", transparency=pal.info["transparency"])
            else:
                pal.save(bio, "PNG")
        else:
            img.convert(mode).save(bio, "PNG")
        out.append((name + ".png", bio.getvalue()))
    return out


def fmt_of(words):
    """Формат, шаг и высота — из последнего BITMAP_LAYOUT (и _H) листа."""
    fmt = stride = height = None
    sh = lh = 0
    for wd in words:
        op = wd >> 24
        if op == 0x07:
            fmt, stride, height = (wd >> 19) & 31, (wd >> 9) & 1023, wd & 511
        elif op == 0x28:
            sh, lh = (wd >> 2) & 3, wd & 3
    if fmt is None:
        return None
    return {"fmt": fmt, "stride": stride | (sh << 10), "height": height | (lh << 9)}


def main(argv: list[str]) -> int:
    import ft812emu
    out = Path(argv[1])
    out.mkdir(parents=True, exist_ok=True)
    with ft812emu.FT812() as chip:
        chip.boot()
        for name, data in cases():
            chip.write(PTR - 0x400, bytes(0x400 + 0x30000))           # RAM_G чистая
            pad = data + bytes(-len(data) % 4)
            rd0 = chip.rd32(REG_CMD_READ)
            chip.cmd(struct.pack("<3I", CMD_DLSTART, CMD_LOADIMAGE, PTR) + struct.pack("<I", 0) + pad +
                     struct.pack("<4I", CMD_GETPROPS, 0, 0, 0))
            chip.wait_idle()
            rd = chip.rd32(REG_CMD_READ)
            props = list(struct.unpack("<3I", chip.read(RAM_CMD + ((rd - 12) & 0xFFF), 12)))
            n = chip.rd32(REG_CMD_DL) // 4
            words = list(struct.unpack(f"<{n}I", chip.read(0x300000, 4 * n)))
            f = fmt_of(words)
            size = (f["stride"] * f["height"]) if f else 0
            pal = 0
            for wd in words:
                if wd >> 24 == 0x2A:                                  # PALETTE_SOURCE
                    pal = wd & 0x3FFFFF
            end = max(PTR + size, pal + 1024 if pal else 0)
            ramg = chip.read(PTR, end - PTR)
            (out / name).write_bytes(data)
            stem = name.rsplit(".", 1)[0]
            (out / (stem + ".ramg")).write_bytes(ramg)
            (out / (stem + ".json")).write_text(json.dumps({
                "file": name, "ptr": PTR, "dl": words, "getprops": props, "layout": f,
                "palette": pal, "ramg_bytes": len(ramg), "cmd_read_start": rd0}, indent=1))
            print(f"{name:14s} dl {len(words):2d} props {props} layout {f} pal {pal:#x}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
