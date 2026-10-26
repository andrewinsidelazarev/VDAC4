"""Замер на bt8xxemu: как FT812 раскрывает каналы 1…6 бит в 8 бит. Каждый формат — строка
кадра, тексели — все значения канала по порядку, рисуются белым COLOR_RGB с COLOR_A 255
поверх чёрного (смешение SRC_ALPHA / ONE_MINUS_SRC_ALPHA). Выход — JSON и таблицы.

    python ft_probe_expand.py OUT.json
"""
from __future__ import annotations

import json
import struct
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from ft_probe_math import (BEGIN, BLEND, CLEAR, CLEAR_COLOR_RGB, COLOR_A, COLOR_RGB, END,  # noqa: E402
                           HANDLE, LAYOUT, SIZE, SOURCE, V2F, W2, w)

sys.path.insert(0, str(W2 / "tools"))


def pack_bits(values, bits):
    out = bytearray()
    acc, n = 0, 0
    for v in values:
        acc = (acc << bits) | v
        n += bits
        if n == 8:
            out.append(acc); acc, n = 0, 0
    if n:
        out.append(acc << (8 - n))
    return bytes(out)


def main(argv: list[str]) -> int:
    import ft812emu
    cases = []      # (имя, формат, байты, ширина, канал выхода 0=R 1=G 2=B, как читать)
    cases.append(("L1", 1, pack_bits([0, 1] * 4, 1), 8, 0))
    cases.append(("L2", 17, pack_bits(list(range(4)) * 2, 2), 8, 0))
    cases.append(("L4", 2, pack_bits(list(range(16)), 4), 16, 0))
    cases.append(("RGB565.R5", 7, b"".join(struct.pack("<H", (v << 11)) for v in range(32)), 32, 0))
    cases.append(("RGB565.G6", 7, b"".join(struct.pack("<H", (v << 5)) for v in range(64)), 64, 1))
    cases.append(("RGB565.B5", 7, b"".join(struct.pack("<H", v) for v in range(32)), 32, 2))
    cases.append(("ARGB1555.R5", 0, b"".join(struct.pack("<H", 0x8000 | (v << 10)) for v in range(32)), 32, 0))
    cases.append(("ARGB1555.A1", 0, b"".join(struct.pack("<H", (v << 15) | 0x7C00) for v in range(2)), 2, 0))
    cases.append(("RGB332.R3", 4, bytes(v << 5 for v in range(8)), 8, 0))
    cases.append(("RGB332.B2", 4, bytes(v for v in range(4)), 4, 2))
    cases.append(("ARGB2.R2", 5, bytes(0xC0 | (v << 4) for v in range(4)), 4, 0))
    cases.append(("ARGB2.A2", 5, bytes((v << 6) | 0x30 for v in range(4)), 4, 0))
    cases.append(("ARGB4.R4", 6, b"".join(struct.pack("<H", 0xF000 | (v << 8)) for v in range(16)), 16, 0))
    cases.append(("ARGB4.A4", 6, b"".join(struct.pack("<H", (v << 12) | 0x0F00) for v in range(16)), 16, 0))
    ram = {}
    dl = [CLEAR_COLOR_RGB(0, 0, 0), CLEAR(), w(0x27, 4)]
    addr = 0
    y = 2
    rows = []
    for name, fmt, data, width, ch in cases:
        ram[addr] = data
        bpp = {1: 1, 17: 2, 2: 4}.get(fmt, 16 if fmt in (0, 6, 7) else 8)
        stride = (width * bpp + 7) // 8
        dl += [COLOR_RGB(255, 255, 255), COLOR_A(255), BLEND(2, 4), HANDLE(1), SOURCE(addr),
               LAYOUT(fmt, stride, 1), SIZE(width, 1), BEGIN(1), V2F(0, y * 16), END()]
        rows.append((name, y, width, ch))
        addr += (len(data) + 63) & ~63
        y += 4
    with ft812emu.FT812() as chip:
        chip.boot()
        for a, d in ram.items():
            chip.write(a, d)
        dl.append(0)
        chip.write(0x300000, struct.pack(f"<{len(dl)}I", *dl))
        chip.wr8(0x302054, 2)
        time.sleep(0.2)
        img = chip.frame().convert("RGB")
    res = {}
    for name, ry, width, ch in rows:
        res[name] = [img.getpixel((x, ry))[ch] for x in range(width)]
        print(f"{name:12}", res[name])
    Path(argv[1]).write_text(json.dumps(res))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
