#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Шрифт меню Tang для ESP: include/zifi/tang_menu_font.hpp из шрифта 5×7 прежнего экранного
меню ПЛИС (fpga/vdac2/scripts/gen_osd_rom.py, словарь FONT) — меню выглядит, как раньше.

Запуск из Firmware:  python tools/gen_menu_font.py
Знаки 0x20…0x5F, ячейка 8×8: столбцы 0…4 — биты 6…2 строки, строка 7 пустая.
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]          # корень рабочей копии: Firmware/tools → ../../../..
sys.path.insert(0, str(ROOT / "fpga" / "vdac2" / "scripts"))
import gen_osd_rom  # noqa: E402


def main() -> int:
    rows = gen_osd_rom.font_rows()
    out = [
        "// Создан tools/gen_menu_font.py из шрифта 5×7 fpga/vdac2/scripts/gen_osd_rom.py — не править.",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "namespace zifi {",
        "",
        "// Знак c (0x20…0x5F): kMenuFont[c − 0x20][строка 0…7], столбцы 0…4 — биты 6…2.",
        "constexpr uint8_t kMenuFont[64][8] = {",
    ]
    for g in range(64):
        vals = ", ".join(f"0x{rows.get(g * 8 + y, 0):02X}" for y in range(8))
        out.append(f"    {{{vals}}},  // {chr(0x20 + g)!r}")
    out += ["};", "", "}  // namespace zifi", ""]
    path = HERE.parent / "include" / "zifi" / "tang_menu_font.hpp"
    path.write_text("\n".join(out), encoding="utf-8")
    print(f"{path}: 64 знака")
    return 0


if __name__ == "__main__":
    sys.exit(main())
