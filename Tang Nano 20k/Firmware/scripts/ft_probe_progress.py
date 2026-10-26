"""Замер на bt8xxemu: слова дисплей-листа CMD_PROGRESS (x, y, w, h, опции, значение, предел) —
OPT_FLAT и объёмный вид, горизонтальная (w > h) и вертикальная полоса, края значений, цвета
CMD_FGCOLOR / CMD_BGCOLOR. Каждый случай — CMD_DLSTART, CMD_FGCOLOR, CMD_BGCOLOR, CMD_PROGRESS,
затем RAM_DL до REG_CMD_DL. Выход — JSON и OUT.txt (x y w h опции значение предел fg bg, число
слов и слова) для проверки сопроцессора BL616 (bl616/copro/test/host_test.c, t_progress).

    python ft_probe_progress.py OUT.json [--show]
"""
from __future__ import annotations

import json
import random
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
W2 = HERE.parents[2] / "VDAC4" / "ESP32 S3 Zero N8R8" / "Network" / "Weather screensaver VDAC2"
sys.path.insert(0, str(W2 / "tools"))

CMD_DLSTART, CMD_FGCOLOR, CMD_BGCOLOR, CMD_PROGRESS = 0xFFFFFF00, 0xFFFFFF0A, 0xFFFFFF09, 0xFFFFFF0F
REG_CMD_DL = 0x302100
OPT_FLAT = 256


def cases():
    out = []
    for opt in (OPT_FLAT, 0):
        for val, rng in ((0, 100), (1, 100), (50, 100), (99, 100), (100, 100), (150, 100), (0, 0),
                         (12345, 65535), (65535, 65535), (3, 7)):
            out.append((24, 708, 976, 12, opt, val, rng, 0x003870, 0x002040))     # ftview
        for x, y, w, h in ((10, 10, 200, 20), (10, 10, 20, 200), (100, 100, 2, 2), (0, 0, 1023, 767),
                           (600, 600, 300, 8), (-20, 5, 100, 10), (5, 500, 10, 40), (50, 50, 64, 64)):
            out.append((x, y, w, h, opt, 30, 100, 0xFF8000, 0x101010))
    rnd = random.Random(7)
    for _ in range(40):
        w, h = rnd.randint(2, 600), rnd.randint(2, 100)
        if rnd.random() < 0.3:
            w, h = h, w
        rng = rnd.randint(0, 2000)
        out.append((rnd.randint(-50, 900), rnd.randint(-50, 700), w, h, rnd.choice((0, OPT_FLAT)),
                    rnd.randint(0, rng + 50), rng, rnd.randint(0, 0xFFFFFF), rnd.randint(0, 0xFFFFFF)))
    return out


def main(argv: list[str]) -> int:
    import ft812emu
    res = []
    with ft812emu.FT812() as chip:
        chip.boot()
        for x, y, w, h, opt, val, rng, fg, bg in cases():
            chip.cmd(struct.pack("<IIIIIIhhhhHHHH", CMD_DLSTART, CMD_FGCOLOR, fg, CMD_BGCOLOR, bg,
                                 CMD_PROGRESS, x, y, w, h, opt, val, rng, 0))
            chip.wait_idle()
            k = chip.rd32(REG_CMD_DL)
            words = list(struct.unpack(f"<{k // 4}I", chip.read(0x300000, k))) if k else []
            res.append({"p": [x, y, w, h, opt, val, rng, fg, bg], "dl": words})
            if "--show" in argv:
                print(f"PROGRESS{(x, y, w, h, opt, val, rng)} fg {fg:06X} bg {bg:06X}: "
                      + " ".join(f"{v:08X}" for v in words))
    Path(argv[1]).write_text(json.dumps(res, indent=1), encoding="utf-8")
    with Path(argv[1]).with_suffix(".txt").open("w", encoding="ascii") as f:
        for c in res:
            f.write(" ".join(str(v) for v in c["p"]) + f" {len(c['dl'])} "
                    + " ".join(f"{w:08X}" for w in c["dl"]) + "\n")
    print(len(res), "cases")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
