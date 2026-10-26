"""Замер на bt8xxemu: арифметика BILINEAR. Тексели — PALETTED8 с палитрой «индекс → (i, i, i,
255)», то есть честные 8-битные каналы, BLEND(ONE, ZERO). Три вида отсчётов:
  - строки: пара текселей [a, b], BITMAP_TRANSFORM_A = 1 — доля fx проходит 0…255 по точке;
  - столбцы: пара [a; b], E = 1 — доля fy;
  - блоки 2×2 [a b; c d], A = E = 4 — fx, fy вместе (64×64 точки).
По отсчётам проверяются формулы-кандидаты; выводятся лучшие и расхождения.

    python ft_probe_bilinear_fit.py OUT_DIR
"""
from __future__ import annotations

import random
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import os  # noqa: E402
import time  # noqa: E402

W2 = HERE.parents[2] / "VDAC4" / "ESP32 S3 Zero N8R8" / "Network" / "Weather screensaver VDAC2"
sys.path.insert(0, str(W2 / "tools"))


def golden_frame(ram: bytes, dl: list[int], dither: int = 0):
    """Как ft_golden.golden_frame, но с REG_DITHER (0x302060): по умолчанию у FT812 он 1."""
    import ft812emu
    with ft812emu.FT812() as chip:
        chip.boot()
        chip.wr8(0x302060, dither)
        for i in range(0, len(ram), 4096):
            chip.write(i, ram[i:i + 4096])
        chip.write(0x300000, struct.pack(f"<{len(dl)}I", *dl))
        chip.wr8(0x302054, 2)
        time.sleep(0.2)
        return chip.frame()
from ft_probe_bilinear import SIZE_F, transform  # noqa: E402
from ft_probe_math import BEGIN, BLEND, CLEAR, CLEAR_COLOR_RGB, COLOR_A, COLOR_RGB, END, \
    HANDLE, LAYOUT, SOURCE, V2F, w  # noqa: E402

PAL = 0x8000                      # палитра ARGB8888: B, G, R, A


def PALETTE_SOURCE(a):
    return w(0x2A, a)


def candidates():
    def m_model(q, fx, fy):
        w11 = (fx * fy) >> 8
        wt = (256 - fx - fy + w11, fx - w11, fy - w11, w11)
        return sum((q[k] * wt[k]) >> 8 for k in range(4))

    def m_model_r(q, fx, fy):
        w11 = (fx * fy) >> 8
        wt = (256 - fx - fy + w11, fx - w11, fy - w11, w11)
        return (sum(q[k] * wt[k] for k in range(4))) >> 8

    def m_sep_floor(q, fx, fy):
        h0 = q[0] + (((q[1] - q[0]) * fx) >> 8)
        h1 = q[2] + (((q[3] - q[2]) * fx) >> 8)
        return h0 + (((h1 - h0) * fy) >> 8)

    def m_sep_w(q, fx, fy):
        h0 = (q[0] * (256 - fx) + q[1] * fx) >> 8
        h1 = (q[2] * (256 - fx) + q[3] * fx) >> 8
        return (h0 * (256 - fy) + h1 * fy) >> 8

    def m_sep_w_yx(q, fx, fy):
        v0 = (q[0] * (256 - fy) + q[2] * fy) >> 8
        v1 = (q[1] * (256 - fy) + q[3] * fy) >> 8
        return (v0 * (256 - fx) + v1 * fx) >> 8

    def m_full(q, fx, fy):
        return (q[0] * (256 - fx) * (256 - fy) + q[1] * fx * (256 - fy) +
                q[2] * (256 - fx) * fy + q[3] * fx * fy) >> 16

    def m_full_r(q, fx, fy):
        return (q[0] * (256 - fx) * (256 - fy) + q[1] * fx * (256 - fy) +
                q[2] * (256 - fx) * fy + q[3] * fx * fy + 32768) >> 16

    def m_sep255(q, fx, fy):
        h0 = (q[0] * (255 - fx) + q[1] * fx) // 255
        h1 = (q[2] * (255 - fx) + q[3] * fx) // 255
        return (h0 * (255 - fy) + h1 * fy) // 255

    base = {"model": m_model, "model_sum": m_model_r, "sep_floor": m_sep_floor, "sep_w": m_sep_w,
            "sep_w_yx": m_sep_w_yx, "full": m_full, "full_round": m_full_r, "sep255": m_sep255}
    out = dict(base)
    for b in (3, 4, 5, 6, 7):
        for name, f in base.items():
            def g(q, fx, fy, f=f, b=b):
                return f(q, (fx >> (8 - b)) << (8 - b), (fy >> (8 - b)) << (8 - b))
            out[f"{name}/{b}бит"] = g
    return out


def main(argv: list[str]) -> int:
    out = Path(argv[1])
    out.mkdir(parents=True, exist_ok=True)
    rnd = random.Random(11)
    ram = bytearray(0x9000)
    for i in range(256):
        ram[PAL + 4 * i:PAL + 4 * i + 4] = bytes((i, i, i, 255))
    dl = [CLEAR_COLOR_RGB(0, 0, 0), CLEAR(), BLEND(1, 0), COLOR_RGB(255, 255, 255), COLOR_A(255),
          PALETTE_SOURCE(PAL)]
    a = 0
    items = []                    # (вид, x, y, тексели)
    h = 0

    def bmp(tex, lw, lh, sw, sh, tf, x, y):
        nonlocal a, h
        ram[a:a + len(tex)] = bytes(tex)
        dl.extend([HANDLE(h % 15), SOURCE(a), LAYOUT(16, lw, lh), SIZE_F(sw, sh, 1)])
        dl.extend(transform(*tf))
        dl.extend([BEGIN(1), w(0x2E, h % 15), V2F(x * 16, y * 16), END()])
        a += (len(tex) + 3) & ~3
        h += 1

    for r in range(40):                                    # строки: 2 ряда по 20, ширина 256
        t = [rnd.randrange(256), rnd.randrange(256)]
        x, y = (r // 20) * 260, r % 20
        bmp(t, 2, 1, 256, 1, (1, 0, 0, 0, 256, 0), x, y)
        items.append(("row", x, y, t))
    for c in range(40):                                    # столбцы: высота 256
        t = [rnd.randrange(256), rnd.randrange(256)]
        x, y = 600 + c, 0
        bmp(t, 1, 2, 1, 256, (256, 0, 0, 0, 1, 0), x, y)
        items.append(("col", x, y, t))
    for b in range(24):                                    # блоки 2×2, 64×64
        t = [rnd.randrange(256) for _ in range(4)]
        x, y = (b % 12) * 66, 300 + (b // 12) * 66
        bmp(t, 2, 2, 64, 64, (4, 0, 0, 0, 4, 0), x, y)
        items.append(("blk", x, y, t))
    dl.append(w(0x00))
    ref = golden_frame(bytes(ram), dl).convert("RGB")
    ref.save(out / "fit8.png")
    px = ref.load()
    samples = []
    for kind, x, y, t in items:
        if kind == "row":
            for i in range(256):
                samples.append(((t[0], t[1], t[0], t[1]), i, 0, px[x + i, y][0], kind))
        elif kind == "col":
            for i in range(256):
                samples.append(((t[0], t[0], t[1], t[1]), 0, i, px[x, y + i][0], kind))
        else:
            for j in range(64):
                for i in range(64):
                    samples.append((tuple(t), 4 * i + 2, 4 * j + 2, px[x + i, y + j][0], kind))
    print(f"отсчётов {len(samples)}")
    res = []
    cand = candidates()
    for name, f in cand.items():
        bad = {k: 0 for k in ("row", "col", "blk")}
        for q, fx, fy, g, kind in samples:
            if f(q, fx, fy) != g:
                bad[kind] += 1
        res.append((sum(bad.values()), name, bad))
    for tot, name, bad in sorted(res)[:10]:
        print(f"{name}: не сходится {tot} (строки {bad['row']}, столбцы {bad['col']}, блоки {bad['blk']})")
    best = sorted(res)[0][1]
    shown = 0
    for q, fx, fy, g, kind in samples:
        if cand[best](q, fx, fy) != g and shown < 12:
            print("  ", best, kind, q, fx, fy, "чип", g, "формула", cand[best](q, fx, fy))
            shown += 1
    # сырые: первая строка и первый столбец
    k0 = [s for s in samples if s[4] == "row"][:256]
    print("строка", k0[0][0][:2], [s[3] for s in k0[::8]])
    c0 = [s for s in samples if s[4] == "col"][:256]
    print("столбец", (c0[0][0][0], c0[0][0][2]), [s[3] for s in c0[::8]])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
