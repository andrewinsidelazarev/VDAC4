"""Замер на bt8xxemu: какие слова дисплей-листа пишет CMD_NUMBER (база CMD_SETBASE, знак,
число цифр — младшие 5 разрядов опций, выравнивание, шрифты ПЗУ). Каждый случай —
CMD_DLSTART, CMD_SETBASE, CMD_NUMBER, затем RAM_DL от начала до REG_CMD_DL. Выход — JSON и то же
текстом (OUT.txt: x y шрифт опции база число, число слов листа и слова) для проверки
сопроцессора BL616 (bl616/copro/test/host_test.c, t_number). Это данные Bridgetek (ПЗУ):
в репозиторий не кладутся (E:/zx/ft812_rom).

    python ft_probe_number.py OUT.json [--show]
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
W2 = HERE.parents[2] / "VDAC4" / "ESP32 S3 Zero N8R8" / "Network" / "Weather screensaver VDAC2"
sys.path.insert(0, str(W2 / "tools"))

CMD_DLSTART, CMD_NUMBER, CMD_SETBASE = 0xFFFFFF00, 0xFFFFFF2E, 0xFFFFFF38
REG_CMD_DL = 0x302100
OPT_SIGNED, OPT_CENTERX, OPT_CENTERY, OPT_RIGHTX = 256, 512, 1024, 2048


def cases():
    out = []
    nums = [0, 1, 7, 42, 59, 100, 12345, 2147483647, 0x80000000, 0xFFFFFFFF, (-1) & 0xFFFFFFFF,
            (-12345) & 0xFFFFFFFF, 0xDEADBEEF]
    for n in nums:
        for opt in (0, OPT_SIGNED):
            out.append((10, 20, 26, opt, 10, n))
    for digits in (1, 2, 3, 5, 10, 12, 31):
        for n in (0, 7, 59, 12345, (-42) & 0xFFFFFFFF):
            out.append((100, 50, 28, digits, 10, n))
            out.append((100, 50, 28, digits | OPT_SIGNED, 10, n))
    for base in (2, 8, 16, 36):
        for n in (0, 5, 255, 0xDEADBEEF, (-1) & 0xFFFFFFFF):
            out.append((5, 5, 27, 0, base, n))
            out.append((5, 5, 27, OPT_SIGNED | 4, base, n))
    for opt in (OPT_RIGHTX, OPT_CENTERX, OPT_CENTERY, OPT_CENTERX | OPT_CENTERY, OPT_RIGHTX | 2):
        for n in (0, 42, 1234567):
            out.append((80, 730, 26, opt, 10, n))           # ftview: время у полосы
            out.append((988, 30, 26, opt, 10, n))
    for font in (16, 18, 20, 22, 23, 24, 25, 26, 28, 29, 30, 31):
        out.append((300, 400, font, 0, 10, 31415))
        out.append((600, 700, font, OPT_SIGNED, 10, (-271) & 0xFFFFFFFF))
    return out


def main(argv: list[str]) -> int:
    import ft812emu
    res = []
    with ft812emu.FT812() as chip:
        chip.boot()
        for x, y, font, opt, base, n in cases():
            chip.cmd(struct.pack("<III", CMD_DLSTART, CMD_SETBASE, base) +
                     struct.pack("<IhhHHI", CMD_NUMBER, x, y, font, opt, n))
            chip.wait_idle()
            k = chip.rd32(REG_CMD_DL)
            words = list(struct.unpack(f"<{k // 4}I", chip.read(0x300000, k))) if k else []
            res.append({"x": x, "y": y, "font": font, "opt": opt, "base": base, "n": n, "dl": words})
            if "--show" in argv:
                print(f"NUMBER({x},{y},{font},{opt:#x},base {base},{n:#x}): " + " ".join(f"{w:08X}" for w in words))
    Path(argv[1]).write_text(json.dumps(res, indent=1), encoding="utf-8")
    with Path(argv[1]).with_suffix(".txt").open("w", encoding="ascii") as f:
        for c in res:
            f.write(f"{c['x']} {c['y']} {c['font']} {c['opt']} {c['base']} {c['n']} {len(c['dl'])} "
                    + " ".join(f"{w:08X}" for w in c["dl"]) + "\n")
    print(len(res), "cases")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
