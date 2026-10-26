"""Сверка модели растеризатора (ft_render_model.py) с bt8xxemu (Bridgetek) на одном кадре.

bt8xxemu получает образ RAM_G и дисплей-лист, рисует кадр 1024×768; модель рисует тот же
кадр. Отчёт — сколько пикселей расходится и на сколько; кадры и разница — в PNG.

    python ft_golden.py RAMG.bin DL.bin OUT_DIR
    python ft_golden.py --weather2 OUT_DIR      # образ и лист заставки Weather 2

Нужны bt8xxemu.dll (BT8XXEMU_DIR) и tools/ft812emu.py заставки Weather 2 (VDAC4).
"""
from __future__ import annotations

import json
import os
import struct
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ft_render_model as model  # noqa: E402

W2 = HERE.parents[2] / "VDAC4" / "ESP32 S3 Zero N8R8" / "Network" / "Weather screensaver VDAC2"
sys.path.insert(0, str(W2 / "tools"))


def golden_frame(ram: bytes, dl: list[int]):
    import ft812emu
    with ft812emu.FT812() as chip:
        chip.boot()
        for i in range(0, len(ram), 4096):
            chip.write(i, ram[i:i + 4096])
        chip.write(0x300000, struct.pack(f"<{len(dl)}I", *dl))
        chip.wr8(0x302054, 2)                       # DLSWAP_FRAME
        time.sleep(0.2)
        return chip.frame()


def main(argv: list[str]) -> int:
    if argv[1] == "--weather2":
        ram = (W2 / "build" / "ramg.bin").read_bytes()
        end = Path(os.environ.get("W2_END", HERE.parent / "sim" / "ft" / "weather2_end.json"))
        dl = list(struct.unpack("<2048I", bytes.fromhex(json.loads(end.read_text())["ram_dl"])))
        out = Path(argv[2])
    else:
        ram = Path(argv[1]).read_bytes()
        raw = Path(argv[2]).read_bytes()
        dl = list(struct.unpack(f"<{len(raw) // 4}I", raw))
        out = Path(argv[3])
    out.mkdir(parents=True, exist_ok=True)
    from PIL import Image, ImageChops

    ref = golden_frame(ram, dl).convert("RGB")
    ref.save(out / "bt8xxemu.png")
    t = time.time()
    rgb = model.Renderer(ram, model.rom_area_with_ramp()).render(dl)
    mine = Image.frombytes("RGB", (1024, 768), rgb)
    mine.save(out / "model.png")
    diff = ImageChops.difference(ref, mine)
    diff.point(lambda v: min(255, v * 16)).save(out / "diff.png")
    px = list(diff.getdata())
    bad = sum(1 for p in px if max(p) > 0)
    worst = max(max(p) for p in px)
    big = sum(1 for p in px if max(p) > 8)
    print(f"модель: {time.time() - t:.0f} с; расходятся {bad} из {len(px)} пикселей, "
          f"больше чем на 8 — {big}, наибольшая разница {worst}")
    return 0 if big == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
