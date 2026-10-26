"""Замер на bt8xxemu: какие слова дисплей-листа пишет CMD_TEXT (шрифты ПЗУ 16…31, опции,
x за 511). Каждый случай — CMD_DLSTART, CMD_TEXT, затем RAM_DL от начала до REG_CMD_DL.
Выход — JSON со списком {параметры, слова листа} и то же текстом (OUT.txt) для проверки
сопроцессора BL616 (bl616/copro/test/host_test.c, t_text). Образ ПЗУ и эти файлы — данные
Bridgetek: в репозиторий не кладутся (E:/zx/ft812_rom).

    python ft_probe_text.py OUT.json [--show] [--random N]
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
W2 = HERE.parents[2] / "VDAC4" / "ESP32 S3 Zero N8R8" / "Network" / "Weather screensaver VDAC2"
sys.path.insert(0, str(W2 / "tools"))

CMD_DLSTART = 0xFFFFFF00
CMD_TEXT = 0xFFFFFF0C
REG_CMD_DL = 0x302100
OPT_CENTERX, OPT_CENTERY, OPT_RIGHTX = 512, 1024, 2048


def text_cmd(x, y, font, opt, s):
    b = s.encode("latin-1") + b"\0"
    b += b"\0" * (-len(b) % 4)
    return struct.pack("<IhhHH", CMD_TEXT, x, y, font, opt) + b


CASES = [
    (45, 712, 26, 0, "12:34:56"),
    (998, 19, 28, OPT_RIGHTX, "GS 1 MB"),
    (256, 444, 29, 0, "Loading"),
    (100, 100, 16, 0, "Ab"),
    (300, 200, 31, OPT_CENTERX, "Hi!"),
    (300, 200, 31, OPT_CENTERY, "Hi!"),
    (300, 200, 31, OPT_CENTERX | OPT_CENTERY, "Hi!"),
    (700, 600, 20, 0, "x>511"),
    (20, 20, 21, 0, ""),
    (-5, -3, 23, 0, "neg"),
    (510, 50, 18, 0, "edge"),
    (1000, 760, 30, OPT_RIGHTX | OPT_CENTERY, "R"),
]


def random_cases(n: int):
    import random
    rnd = random.Random(5)
    opts = [0, OPT_CENTERX, OPT_CENTERY, OPT_RIGHTX, OPT_CENTERX | OPT_CENTERY, OPT_RIGHTX | OPT_CENTERY]
    for _ in range(n):
        s = "".join(chr(rnd.randrange(32, 127)) for _ in range(rnd.randrange(0, 21)))
        yield (rnd.randrange(-60, 1100), rnd.randrange(-60, 820), rnd.randrange(16, 32), rnd.choice(opts), s)


def main(argv: list[str]) -> int:
    import ft812emu
    out = []
    cases = list(CASES)
    if "--random" in argv:
        cases += list(random_cases(int(argv[argv.index("--random") + 1])))
    with ft812emu.FT812() as chip:
        chip.boot()
        for x, y, font, opt, s in cases:
            chip.cmd(struct.pack("<I", CMD_DLSTART) + text_cmd(x, y, font, opt, s))
            chip.wait_idle()
            n = chip.rd32(REG_CMD_DL)
            words = list(struct.unpack(f"<{n // 4}I", chip.read(0x300000, n))) if n else []
            out.append({"x": x, "y": y, "font": font, "opt": opt, "s": s, "dl": words})
            if "--show" in argv:
                print(f"TEXT({x},{y},{font},{opt:#x},{s!r}): " + " ".join(f"{w:08X}" for w in words))
    Path(argv[1]).write_text(json.dumps(out, indent=1), encoding="utf-8")
    # те же случаи текстом для теста сопроцессора BL616 (bl616/copro/test): строка на случай —
    # x y шрифт опции, байты строки (hex, «-» — пустая), число слов листа и сами слова
    with Path(argv[1]).with_suffix(".txt").open("w", encoding="ascii") as f:
        for c in out:
            hx = c["s"].encode("latin-1").hex() or "-"
            f.write(f"{c['x']} {c['y']} {c['font']} {c['opt']} {hx} {len(c['dl'])} "
                    + " ".join(f"{w:08X}" for w in c["dl"]) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
