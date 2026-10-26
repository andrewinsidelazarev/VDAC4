"""Растеризатор FT812 в RTL против модели (ft_render_model.py) на кадре Weather 2.

    python ft_rtl_check.py prep DIR     # weather2_dl.hex, weather2_ramg.hex для tb_ft_render
    python ft_rtl_check.py check DIR    # ft_lines.hex (RTL) против модели, построчно
    python ft_rtl_check.py frame DIR    # ft_frame.hex (RGB565 из SDRAM) → frame565.png
    python ft_rtl_check.py prep DIR RAMG.bin DL.bin    # любой кадр: frame_dl.hex, frame_ramg.hex
    python ft_rtl_check.py check DIR RAMG.bin DL.bin   # ft_lines.hex против модели на этом кадре

Сверка — точки RGBA буфера строки до дизеринга: должны совпасть с моделью бит в бит.
"""
from __future__ import annotations

import json
import os
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ft_render_model as model  # noqa: E402

W2 = HERE.parents[2] / "VDAC4" / "ESP32 S3 Zero N8R8" / "Network" / "Weather screensaver VDAC2"


FILES: tuple[Path, Path] | None = None       # (RAMG, DL) — кадр не Weather 2


def inputs():
    if FILES:
        ram = FILES[0].read_bytes()
        raw = FILES[1].read_bytes()[:8192]
        dl = list(struct.unpack(f"<{len(raw) // 4}I", raw)) + [0] * (2048 - len(raw) // 4)
        return ram, dl
    ram = (W2 / "build" / "ramg.bin").read_bytes()
    end = Path(os.environ.get("W2_END", HERE.parent / "sim" / "ft" / "weather2_end.json"))
    dl = list(struct.unpack("<2048I", bytes.fromhex(json.loads(end.read_text())["ram_dl"])))
    return ram, dl


def prep(out: Path) -> int:
    ram, dl = inputs()
    out.mkdir(parents=True, exist_ok=True)
    name = "frame" if FILES else "weather2"
    (out / f"{name}_dl.hex").write_text("".join(f"{w:08x}\n" for w in dl))
    pad = ram + bytes((-len(ram)) % 4)
    words = struct.unpack(f"<{len(pad) // 4}I", pad)
    text = "".join(f"{w:08x}\n" for w in words)
    rom = os.environ.get("FT812_ROM")
    if rom and Path(rom).is_file():                   # образ ПЗУ — в SDRAM с FONT_W (ft_core)
        rb = Path(rom).read_bytes()
        text += "@c0000\n" + "".join(f"{w:08x}\n" for w in struct.unpack(f"<{len(rb) // 4}I", rb))
    (out / f"{name}_ramg.hex").write_text(text)
    print(f"лист 2048 слов, RAM_G {len(words)} слов{', образ ПЗУ' if rom else ''} → {out}")
    return 0


def check(out: Path) -> int:
    ram, dl = inputs()
    r = model.Renderer(ram, model.rom_area_with_ramp())
    bad_lines = bad_px = n = 0
    first = []
    for row in (out / "ft_lines.hex").read_text().split("\n"):
        if not row.strip():
            continue
        f = row.split()
        y = int(f[0])
        px = [int(v, 16) for v in f[1:]]
        color, _ = r.line(dl, y)
        n += 1
        bad = 0
        for x in range(1024):
            a, rr, g, b = (px[x] >> 24) & 255, (px[x] >> 16) & 255, (px[x] >> 8) & 255, px[x] & 255
            m = tuple(color[4 * x:4 * x + 4])
            if (rr, g, b, a) != m:
                bad += 1
                if len(first) < 12:
                    first.append((x, y, (rr, g, b, a), m))
        if bad:
            bad_lines += 1
            bad_px += bad
    print(f"строк {n}: расходятся {bad_lines}, точек {bad_px}")
    for x, y, got, exp in first:
        print(f"  ({x}, {y}): RTL {got}, модель {exp}")
    return 0 if bad_px == 0 and n > 0 else 1


def frame(out: Path) -> int:
    from PIL import Image
    words = [int(v, 16) for v in (out / "ft_frame.hex").read_text().split()]
    h = len(words) // 512
    img = Image.new("RGB", (1024, h))
    pix = img.load()
    for y in range(h):
        for k in range(512):
            w = words[y * 512 + k]
            for j, p in enumerate((w & 0xFFFF, w >> 16)):
                r5, g6, b5 = p >> 11, (p >> 5) & 63, p & 31
                pix[2 * k + j, y] = ((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2))
    img.save(out / "frame565.png")
    print(f"кадр {h} строк → {out / 'frame565.png'}")
    return 0


if __name__ == "__main__":
    mode, d = sys.argv[1], Path(sys.argv[2])
    if len(sys.argv) > 4:
        FILES = (Path(sys.argv[3]), Path(sys.argv[4]))
    sys.exit({"prep": prep, "check": check, "frame": frame}[mode](d))
