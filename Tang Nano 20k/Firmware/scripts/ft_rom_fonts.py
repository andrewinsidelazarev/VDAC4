"""Образ шрифтов ПЗУ FT812 для слота 2 flash Tang (VD4FONT1): из образа ПЗУ 0x1E0000…0x2FFFFF,
снятого с bt8xxemu, — только то, что нужно шрифтам 16…31: таблица шрифтов (ROM_FONTROOT, по 148
байтов на шрифт) и глифы с самого младшего адреса шрифта до конца ПЗУ (там же ROM_FONTROOT).
После загрузки ядра FT812 ESP переписывает куски в SDRAM Tang (Firmware/src/ft_rom.cpp).

Образ ПЗУ — данные Bridgetek: ни он, ни этот образ в репозиторий не кладутся.

    python ft_rom_fonts.py ROM_1E0000.bin OUT.bin            # ROM — с ft_probe_text.py / ft812emu
    python tools/tang_tool.py update --slot 2 <IP ESP> OUT.bin    # во flash Tang
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

ROM_BASE = 0x1E0000
ROM_END = 0x300000
FONTROOT = 0x2FFFFC
MAGIC = b"VD4FONT1"


def build(rom: bytes) -> bytes:
    if len(rom) != ROM_END - ROM_BASE:
        raise SystemExit(f"образ ПЗУ {len(rom)} байт, ждали {ROM_END - ROM_BASE}")

    def r32(a):
        return struct.unpack_from("<I", rom, a - ROM_BASE)[0]

    root = r32(FONTROOT)
    if not (0x200000 <= root < FONTROOT) or root & 3:
        raise SystemExit(f"ROM_FONTROOT {root:#x} — не таблица шрифтов")
    lowest = min(r32(root + 148 * i + 144) for i in range(16))     # адрес глифа 0 шрифтов 16…31
    spans = [(root, 148 * 16), (lowest & ~3, ROM_END - (lowest & ~3))]
    if spans[0][0] + spans[0][1] > spans[1][0] and spans[0][0] < ROM_END:
        spans = [(min(root, lowest) & ~3, ROM_END - (min(root, lowest) & ~3))]
    head = MAGIC + struct.pack("<I", len(spans))
    off = len(head) + 12 * len(spans)
    table, data = b"", b""
    for addr, n in spans:
        n = (n + 3) & ~3
        table += struct.pack("<3I", addr, n, off + len(data))
        data += rom[addr - ROM_BASE:addr - ROM_BASE + n]
    return head + table + data


def main(argv: list[str]) -> int:
    img = build(Path(argv[1]).read_bytes())
    Path(argv[2]).write_bytes(img)
    n = struct.unpack_from("<I", img, 8)[0]
    for i in range(n):
        a, ln, off = struct.unpack_from("<3I", img, 12 + 12 * i)
        print(f"кусок {i}: 0x{a:06X}…0x{a + ln - 1:06X} ({ln} байт), в образе с {off}")
    print(f"образ {len(img)} байт -> {argv[2]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
