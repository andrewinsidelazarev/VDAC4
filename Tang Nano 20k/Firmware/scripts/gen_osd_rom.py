#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Генератор ПЗУ экранного меню: rtl/fb_osd_rom.sv (шрифт 5×7 и шаблон текста).

Запуск из fpga/vdac2:  py -3 scripts/gen_osd_rom.py
Результат детерминирован; файл rtl/fb_osd_rom.sv не править руками.

Код шаблона — 9 бит:
  0x000…0x03F  постоянный символ (ASCII − 0x20);
  0x100 | поле << 3 | разряд   число: поле 0…15 (fb_osd, BCD), разряд 0 — единицы;
  0x180 | поле << 3 | индекс   символ поля состояния 0…15, индекс 0…7.
В шаблоне строки: {nF:W} — число поля F шириной W; {cF:W} — символы 0…W−1 поля F;
{cF.I} — символ I поля F.
"""
import re
import sys
from pathlib import Path

COLS = 40

TEMPLATE = [
    "VDAC4  ZX EVOLUTION -> HDMI 1280X720P50",
    "",
    "INPUT  {c0:8}  LOCK {c1.0}  M_SEL {c1.7}",
    "PCLK {n0:5} KHZ  LINE {n1:4}  LINES {n2:4}",
    "HSYNC {n3:4} {c1.1}  VSYNC {c1.2}   FRAMES {n4:5}",
    "WINDOW {n6:4} {n7:4} {c1.3}{c1.4}  BAD FRAMES {n5:5}",
    "PICTURE X {n8:4}-{n9:4}  Y {n10:4}-{n11:4}",
    "SDRAM {c6:4} CHECK {n12:5} LATE {c1.5} OVF {c1.6}",
    "",
    "{c4.0} MUX PHASE   {c5.0}",
    "{c4.1} MUX DELAY   {c5.1}",
    "{c4.2} MUX CLOCK   {c5.5}",
    "{c4.3} H ADJUST  {c5.3}{n13:2}",
    "{c4.4} V ADJUST  {c5.4}{n14:2}",
    "{c4.5} AUTO ALIGN  {c3:3}",
    "{c4.6} SD READ     {c5.2}   MAP {c8:4}",
    "{c4.7} SD CALIBRATE    {c8.4}{c8.5}{c8.6}{c8.7}",
    "{c9.0} ASPECT      {c10:4}",
    "{c9.1} SIZE        {c11:4}",
    "{c9.2} SOURCE      {c2:4}   NOW {c7:4}",
    "S1 NEXT/HOLD HIDE   S2 +1/HOLD -1",
]

# Шрифт 5×7: столбцы 0…4 кладутся в биты 6…2 строки ячейки 8×8, строка 7 пустая.
FONT = {
    " ": [".....", ".....", ".....", ".....", ".....", ".....", "....."],
    "!": ["..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."],
    "(": ["...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."],
    ")": [".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."],
    "*": [".....", "#.#.#", ".###.", "#####", ".###.", "#.#.#", "....."],
    "+": [".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."],
    ",": [".....", ".....", ".....", ".....", ".##..", "..#..", ".#..."],
    "-": [".....", ".....", ".....", "#####", ".....", ".....", "....."],
    ".": [".....", ".....", ".....", ".....", ".....", ".##..", ".##.."],
    "/": [".....", "....#", "...#.", "..#..", ".#...", "#....", "....."],
    "0": [".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."],
    "1": ["..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "2": [".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"],
    "3": ["#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."],
    "4": ["...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."],
    "5": ["#####", "#....", "####.", "....#", "....#", "#...#", ".###."],
    "6": ["..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."],
    "7": ["#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."],
    "8": [".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."],
    "9": [".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."],
    ":": [".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."],
    "<": ["...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#."],
    "=": [".....", ".....", "#####", ".....", "#####", ".....", "....."],
    ">": [".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#..."],
    "?": [".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."],
    "A": [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    "C": [".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."],
    "D": ["###..", "#..#.", "#...#", "#...#", "#...#", "#..#.", "###.."],
    "E": ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
    "F": ["#####", "#....", "#....", "####.", "#....", "#....", "#...."],
    "G": [".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"],
    "H": ["#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "I": [".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "J": ["..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."],
    "K": ["#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"],
    "L": ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    "M": ["#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"],
    "N": ["#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#"],
    "O": [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "P": ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."],
    "Q": [".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"],
    "R": ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"],
    "S": [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
    "T": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "U": ["#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "V": ["#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."],
    "W": ["#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#."],
    "X": ["#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"],
    "Y": ["#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."],
    "Z": ["#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"],
    "_": [".....", ".....", ".....", ".....", ".....", ".....", "#####"],
}

TOKEN = re.compile(r"\{([nc])(\d+)(?::(\d+)|\.(\d+))\}")


def glyph(ch: str) -> int:
    code = ord(ch)
    if not 0x20 <= code <= 0x5F:
        raise ValueError(f"символ {ch!r} вне набора 0x20…0x5F")
    if ch not in FONT:
        raise ValueError(f"нет рисунка символа {ch!r}")
    return code - 0x20


def parse_row(text: str) -> list[int]:
    codes: list[int] = []
    pos = 0
    for m in TOKEN.finditer(text):
        codes += [glyph(c) for c in text[pos:m.start()]]
        kind, field = m.group(1), int(m.group(2))
        if not 0 <= field <= 15:
            raise ValueError(f"поле {field} вне 0…15: {text!r}")
        base = 0x100 | (0x80 if kind == "c" else 0) | (field << 3)
        if m.group(3) is not None:
            width = int(m.group(3))
            if kind == "n":
                if not 1 <= width <= 5:
                    raise ValueError(f"ширина числа {width}: {text!r}")
                codes += [base | d for d in range(width - 1, -1, -1)]
            else:
                if not 1 <= width <= 8:
                    raise ValueError(f"ширина поля {width}: {text!r}")
                codes += [base | i for i in range(width)]
        else:
            codes.append(base | int(m.group(4)))
        pos = m.end()
    codes += [glyph(c) for c in text[pos:]]
    if len(codes) > COLS:
        raise ValueError(f"строка длиннее {COLS}: {text!r} ({len(codes)})")
    return codes + [0] * (COLS - len(codes))


def font_rows() -> dict[int, int]:
    rom: dict[int, int] = {}
    for ch, rows in FONT.items():
        if len(rows) != 7 or any(len(r) != 5 for r in rows):
            raise ValueError(f"рисунок {ch!r} не 5×7")
        g = glyph(ch)
        for y, row in enumerate(rows):
            bits = 0
            for x, px in enumerate(row):
                if px == "#":
                    bits |= 1 << (6 - x)
            if bits:
                rom[g * 8 + y] = bits
    return rom


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    rows = [parse_row(t) for t in TEMPLATE]
    if len(rows) != 21 or COLS != 40:
        raise ValueError("размер панели задан и в rtl/fb_osd.sv (ROWS = 21, COLS = 40)")
    text_rom = {r * COLS + c: code for r, row in enumerate(rows)
                for c, code in enumerate(row) if code}
    font = font_rows()

    out = []
    out.append("// SPDX-License-Identifier: Apache-2.0")
    out.append("//")
    out.append("// ПЗУ экранного меню: шаблон текста и шрифт 5×7. СГЕНЕРИРОВАНО scripts/gen_osd_rom.py —")
    out.append("// не править руками. Формат кодов шаблона описан в скрипте и в fb_osd.sv.")
    out.append("//")
    out.append(f"// Шаблон: {len(rows)} строк × {COLS} символов.")
    for r, t in enumerate(TEMPLATE):
        out.append(f"//   {r:2d} | {t}")
    out.append("//")
    out.append("")
    out.append("module fb_osd_text_rom (")
    out.append("    input  wire       clk,")
    out.append("    input  wire [9:0] addr,          // строка × 40 + столбец")
    out.append("    output reg  [8:0] q")
    out.append(");")
    out.append("    always_ff @(posedge clk) begin")
    out.append("        case (addr)")
    for a in sorted(text_rom):
        out.append(f"            10'd{a}: q <= 9'h{text_rom[a]:03X};")
    out.append("            default: q <= 9'h000;")
    out.append("        endcase")
    out.append("    end")
    out.append("endmodule")
    out.append("")
    out.append("module fb_osd_font_rom (")
    out.append("    input  wire       clk,")
    out.append("    input  wire [8:0] addr,          // {символ[5:0], строка[2:0]}")
    out.append("    output reg  [7:0] q              // бит 7 — левая точка ячейки")
    out.append(");")
    out.append("    always_ff @(posedge clk) begin")
    out.append("        case (addr)")
    for a in sorted(font):
        out.append(f"            9'd{a}: q <= 8'h{font[a]:02X};")
    out.append("            default: q <= 8'h00;")
    out.append("        endcase")
    out.append("    end")
    out.append("endmodule")
    out.append("")
    (root / "rtl" / "fb_osd_rom.sv").write_text("\n".join(out), encoding="utf-8", newline="\n")
    print(f"rtl/fb_osd_rom.sv: шаблон {len(text_rom)} непустых ячеек, шрифт {len(FONT)} символов")
    return 0


if __name__ == "__main__":
    sys.exit(main())
