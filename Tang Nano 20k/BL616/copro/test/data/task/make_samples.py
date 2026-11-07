"""Образцы картинок для задания task_bl616_images.md: все виды PNG и прогрессивные JPEG.

Настоящие картинки сайтов (vtrd.in, google.com) лежат в samples/real/ готовыми; этот сценарий
делает samples/gen/ — то, что надо уметь разбирать, но на сайтах попадается реже:

- PNG всех глубин (1, 2, 4, 8, 16 бит) и видов цвета (0 серый, 2 RGB, 3 палитра, 4 серый с
  альфой, 6 RGBA), tRNS у серого, RGB и палитры, «через строку» (Adam7). Пишет их свой
  писатель PNG (Pillow не умеет 16-битный RGB и Adam7), фильтры строк — все пять по очереди;
- JPEG прогрессивные (4:2:0, 4:4:4, серый), большие (baseline и прогрессивный), CMYK (Adobe).

Запуск: python make_samples.py (нужен Pillow). Эталон точек — то, что даёт Pillow
(Image.open(...).convert("RGBA")): у прогрессивного JPEG — libjpeg-turbo, как у baseline в
copro/test/image_test.c.
"""
import io
import math
import pathlib
import sys
import struct
import zlib

from PIL import Image, ImageDraw

OUT = pathlib.Path(__file__).resolve().parent / "samples" / "gen"

# Проходы Adam7: (x0, y0, шаг x, шаг y)
ADAM7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]
CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def pack_row(values: list[int], depth: int) -> bytes:
    """Значения каналов строки подряд — в байты PNG (меньше 8 бит — старшие биты первыми)."""
    if depth == 16:
        return b"".join(struct.pack(">H", v) for v in values)
    if depth == 8:
        return bytes(values)
    out, acc, bits = bytearray(), 0, 0
    for v in values:
        acc = acc << depth | v
        bits += depth
        if bits == 8:
            out.append(acc)
            acc, bits = 0, 0
    if bits:
        out.append(acc << (8 - bits))
    return bytes(out)


def paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c


def filter_row(kind: int, row: bytes, prev: bytes, bpp: int) -> bytes:
    out = bytearray([kind])
    for i, x in enumerate(row):
        a = row[i - bpp] if i >= bpp else 0
        b = prev[i] if prev else 0
        c = prev[i - bpp] if prev and i >= bpp else 0
        pred = (0, a, b, (a + b) // 2, paeth(a, b, c))[kind]
        out.append((x - pred) & 255)
    return bytes(out)


def write_png(name: str, w: int, h: int, ctype: int, depth: int, pixel, plte=None, trns=None,
              interlace=False) -> None:
    """pixel(x, y) — значения каналов точки (столько, сколько у вида цвета) при этой глубине."""
    ch = CHANNELS[ctype]
    bpp = max(1, ch * depth // 8)
    raw = bytearray()
    passes = ADAM7 if interlace else [(0, 0, 1, 1)]
    k = 0
    for x0, y0, dx, dy in passes:
        xs = list(range(x0, w, dx))
        if not xs:
            continue
        prev = b""
        for y in range(y0, h, dy):
            row = pack_row([v for x in xs for v in pixel(x, y)], depth)
            raw += filter_row(k % 5, row, prev, bpp)
            prev = row
            k += 1
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0,
                                                              1 if interlace else 0))
    if plte:
        data += chunk(b"PLTE", bytes(v for c in plte for v in c))
    if trns is not None:
        data += chunk(b"tRNS", trns)
    data += chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    (OUT / name).write_bytes(data)


def wave(x: int, y: int, w: int, h: int, k: int) -> float:
    """Плавный узор 0…1 — видно и сдвиг строк, и ошибки фильтров."""
    return 0.5 + 0.5 * math.sin((x * (3 + k)) / w * math.pi + (y * (2 + k)) / h * math.pi * 1.3)


def make_png() -> None:
    w, h = 61, 37                     # нечётные — неполные байты строк и неполные проходы Adam7
    for depth in (1, 2, 4, 8, 16):   # серый всех глубин
        top = (1 << depth) - 1
        write_png(f"g{depth}.png", w, h, 0, depth, lambda x, y, t=top: [round(wave(x, y, w, h, 0) * t)])
    write_png("g8_trns.png", w, h, 0, 8, lambda x, y: [0 if (x // 8 + y // 8) % 2 else round(wave(x, y, w, h, 1) * 255) | 1],
              trns=struct.pack(">H", 0))
    write_png("g2_trns.png", w, h, 0, 2, lambda x, y: [(x // 6 + y // 6) % 4], trns=struct.pack(">H", 3))
    for depth in (8, 16):             # RGB, RGBA, серый с альфой — 8 и 16 бит
        top = (1 << depth) - 1
        rgb = lambda x, y, t=top: [round(wave(x, y, w, h, i) * t) for i in range(3)]
        write_png(f"rgb{depth}.png", w, h, 2, depth, rgb)
        write_png(f"rgba{depth}.png", w, h, 6, depth, lambda x, y, t=top: rgb(x, y) + [round(x / (w - 1) * t)])
        write_png(f"ga{depth}.png", w, h, 4, depth,
                  lambda x, y, t=top: [round(wave(x, y, w, h, 2) * t), round(y / (h - 1) * t)])
    write_png("rgb8_trns.png", w, h, 2, 8, lambda x, y: [255, 0, 255] if (x - 30) ** 2 + (y - 18) ** 2 < 120
              else [round(wave(x, y, w, h, i) * 254) for i in range(3)], trns=struct.pack(">HHH", 255, 0, 255))
    for depth in (1, 2, 4, 8):        # палитра всех глубин, с tRNS и без
        n = 1 << depth
        plte = [(i * 255 // (n - 1), 255 - i * 255 // (n - 1), (i * 97) % 256) for i in range(n)]
        idx = lambda x, y, n=n: [min(n - 1, int(wave(x, y, w, h, 3) * n))]
        write_png(f"p{depth}.png", w, h, 3, depth, idx, plte=plte)
        write_png(f"p{depth}_trns.png", w, h, 3, depth, idx, plte=plte, trns=bytes(i * 255 // (n - 1) for i in range(n)))
    # «через строку» (Adam7) — разные виды и глубины
    write_png("rgba8_adam7.png", w, h, 6, 8, lambda x, y: [round(wave(x, y, w, h, i) * 255) for i in range(3)] + [round(x / (w - 1) * 255)],
              interlace=True)
    write_png("rgb16_adam7.png", w, h, 2, 16, lambda x, y: [round(wave(x, y, w, h, i) * 65535) for i in range(3)], interlace=True)
    write_png("p4_adam7.png", w, h, 3, 4, lambda x, y: [min(15, int(wave(x, y, w, h, 3) * 16))],
              plte=[(i * 17, 255 - i * 17, 128) for i in range(16)], interlace=True)
    write_png("g1_adam7.png", w, h, 0, 1, lambda x, y: [(x + y) % 3 == 0], interlace=True)
    # большая палитра с прозрачностью (как telegram-logo.png: 1000×1000, место 32×32) — только «в рамку»
    big = Image.new("RGBA", (2000, 1500), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    d.ellipse((100, 50, 1900, 1450), fill=(40, 160, 220, 255))
    d.polygon([(600, 750), (1500, 350), (1300, 1150)], fill=(255, 255, 255, 255))
    big.quantize(64, dither=Image.Dither.NONE).save(OUT / "big_pal_2000x1500.png", transparency=0)


def photo(w: int, h: int) -> Image.Image:
    """Фото-подобная картинка: плавные переходы, мелкие детали, резкие края."""
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            r = int(128 + 127 * math.sin(x / w * 7 + y / h * 3))
            g = int(128 + 127 * math.sin(y / h * 5 + 1.3))
            b = int(128 + 127 * math.cos((x + y) / (w + h) * 9))
            if (x // 3 + y // 3) % 2 and x > w * 0.7:
                r, g, b = 255 - r, 255 - g, 255 - b
            px[x, y] = (r, g, b)
    d = ImageDraw.Draw(im)
    for i in range(12):
        d.line((i * w // 12, 0, w - i * w // 12, h), fill=(255, 255, 255), width=max(1, w // 400))
    d.text((w // 20, h // 20), "VDAC4 TSafari", fill=(0, 0, 0))
    return im


def make_jpeg() -> None:
    small = photo(203, 131)          # неровные размеры — неполные MCU
    small.save(OUT / "prog_420.jpg", quality=85, progressive=True, subsampling=2)
    small.save(OUT / "prog_422.jpg", quality=85, progressive=True, subsampling=1)
    small.save(OUT / "prog_444.jpg", quality=90, progressive=True, subsampling=0)
    small.convert("L").save(OUT / "prog_gray.jpg", quality=85, progressive=True)
    small.save(OUT / "prog_420_q30.jpg", quality=30, progressive=True, subsampling=2)
    small.convert("CMYK").save(OUT / "cmyk.jpg", quality=85)
    big = photo(1920, 1080)          # большие — только «в рамку»
    big.save(OUT / "big_base_1920x1080.jpg", quality=85, subsampling=2)
    big.resize((1600, 1200)).save(OUT / "big_prog_1600x1200.jpg", quality=85, progressive=True, subsampling=2)
    big.resize((4000, 3000)).save(OUT / "huge_base_4000x3000.jpg", quality=80, subsampling=2)


def riff_chunks(data: bytes) -> list:
    """Блоки WebP за «RIFF длина WEBP»: [(тег, блок с заголовком и выравниванием)]."""
    at, out = 12, []
    while at + 8 <= len(data):
        n = struct.unpack("<I", data[at + 4:at + 8])[0]
        out.append((data[at:at + 4], data[at:at + 8 + n + (n & 1)]))
        at += 8 + n + (n & 1)
    return out


def riff_chunk(tag: bytes, payload: bytes) -> bytes:
    return tag + struct.pack("<I", len(payload)) + payload + (bytes(1) if len(payload) & 1 else b"")


def u24(v: int) -> bytes:
    return struct.pack("<I", v)[:3]


def webp_bytes(im: Image.Image, **kw) -> bytes:
    b = io.BytesIO()
    im.save(b, "WEBP", **kw)
    return b.getvalue()


def anim_webp(cw: int, ch: int, frames: list) -> bytes:
    """Анимация из готовых WebP: [(x, y, файл кадра)], x и y чётные. Pillow (WebPAnimDecoder)
    показывает первый кадр на прозрачном холсте — на своём месте."""
    body = riff_chunk(b"VP8X", bytes([0x12, 0, 0, 0]) + u24(cw - 1) + u24(ch - 1))
    body += riff_chunk(b"ANIM", bytes([255, 255, 255, 255, 0, 0]))
    for x, y, data in frames:
        w, h = Image.open(io.BytesIO(data)).size
        sub = b"".join(c for t, c in riff_chunks(data) if t in (b"ALPH", b"VP8 ", b"VP8L"))
        body += riff_chunk(b"ANMF", u24(x // 2) + u24(y // 2) + u24(w - 1) + u24(h - 1) + u24(100) + bytes([2]) + sub)
    return b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WEBP" + body


def rgba_icon(w: int, h: int) -> Image.Image:
    """Значок с прозрачностью: круг с мягким краем на прозрачном, полупрозрачная полоса."""
    im = Image.new("RGBA", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            r = math.hypot(x - w / 2, y - h / 2) / (min(w, h) / 2)
            a = 255 if r < 0.8 else max(0, round((1 - r) / 0.2 * 255))
            if abs(y - h * 0.7) < h * 0.08:
                a = 128
            px[x, y] = (round(wave(x, y, w, h, 0) * 255), round(wave(x, y, w, h, 1) * 255), 40, a)
    return im


def make_webp(out: pathlib.Path) -> None:
    """WebP (прошивка 40): с потерями, с потерями и альфой, без потерь (и палитрой), анимация
    (первый кадр со сдвигом на холсте), большие — для рамки и предела памяти BL616."""
    small = photo(203, 131)          # неровные размеры — неполные макроблоки, нечётная цветность
    (out / "webp_lossy.webp").write_bytes(webp_bytes(small, quality=80))
    (out / "webp_lossy_q30.webp").write_bytes(webp_bytes(small, quality=30))
    icon = rgba_icon(161, 99)
    (out / "webp_lossy_alpha.webp").write_bytes(webp_bytes(icon, quality=80))
    (out / "webp_lossless.webp").write_bytes(webp_bytes(small, lossless=True))
    (out / "webp_lossless_alpha.webp").write_bytes(webp_bytes(rgba_icon(96, 96), lossless=True))
    (out / "webp_lossless_pal.webp").write_bytes(webp_bytes(small.quantize(16, dither=Image.Dither.NONE).convert("RGB"),
                                                            lossless=True))
    first = webp_bytes(rgba_icon(100, 80), lossless=True)
    (out / "webp_anim.webp").write_bytes(anim_webp(200, 150, [(40, 20, first),
                                                              (0, 0, webp_bytes(photo(200, 150), quality=70))]))
    (out / "webp_anim_lossy.webp").write_bytes(anim_webp(160, 120, [(20, 10, webp_bytes(rgba_icon(90, 70), quality=75))]))
    big = photo(1920, 1080)
    (out / "webp_1280x720.webp").write_bytes(webp_bytes(big.resize((1280, 720)), quality=80))
    (out / "webp_1920x1080.webp").write_bytes(webp_bytes(big, quality=80))
    (out / "webp_lossless_640x480.webp").write_bytes(webp_bytes(big.resize((640, 480)), lossless=True))
    # ошибки: длина блока VP8 больше файла (повреждён), RIFF не WebP (не поддерживается)
    bad = bytearray((out / "webp_lossy.webp").read_bytes())
    struct.pack_into("<I", bad, 16, len(bad))
    (out / "webp_err_chunk.webp").write_bytes(bytes(bad))
    (out / "webp_err_riff.webp").write_bytes(b"RIFF" + struct.pack("<I", 4 + 24) + b"AVI " + riff_chunk(b"LIST", bytes(16)))


if __name__ == "__main__":
    if sys.argv[1:] == ["webp"]:     # только WebP — сразу сюда, к остальным образцам задания
        OUT = pathlib.Path(__file__).resolve().parent
        make_webp(OUT)
        sys.exit(0)
    OUT.mkdir(parents=True, exist_ok=True)
    make_png()
    make_jpeg()
    make_webp(OUT)
    for f in sorted(OUT.iterdir()):
        try:
            with Image.open(f) as im:   # Pillow должен их читать — иначе образец неверен
                im.load()
                print(f"{f.name:28} {im.format:5} {im.mode:6} {im.size}")
        except Exception as e:
            raise SystemExit(f"{f.name}: Pillow не читает — {e}")
