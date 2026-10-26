#!/usr/bin/env python3
"""Подготовка SPG-запускателя для приложения ESP: build/app_cfg.inc, куски ELF по 16 КБ
и build/spgbld.ini.

    python prep.py путь/к/BOING.ELF   (из папки ESP App SPG)

Время для кэша ESP (MOD_QUERY) — время изменения файла ELF в формате FAT: новый ELF той же
длины ESP не спутает со старым.
"""
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
PAGE = 16384
APP_PAGE0 = 0x20


def fat_time(path: Path) -> int:
    t = time.localtime(path.stat().st_mtime)
    date = ((t.tm_year - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday
    tod = (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2)
    return (date << 16) | tod


def main() -> int:
    elf = Path(sys.argv[1]).resolve()
    data = elf.read_bytes()
    if data[:4] != b"\x7fELF":
        print(f"{elf}: не ELF")
        return 1
    name = elf.name.upper()
    stem = elf.stem.upper()
    if len(elf.stem) > 8 or len(elf.suffix) > 4:
        print(f"{name}: имя не 8.3")
        return 1
    build = HERE / "build"
    build.mkdir(exist_ok=True)
    pages = (len(data) + PAGE - 1) // PAGE
    blocks = []
    for i in range(pages):
        part = build / f"elf{i}.bin"
        part.write_bytes(data[i * PAGE:(i + 1) * PAGE])
        blocks.append(f"Block = #c000, #{APP_PAGE0 + i:02x}, ../build/{part.name}")
    (build / "app_cfg.inc").write_text(
        "; created by prep.py, do not edit\n"   # только ASCII: sjasmplus и spgbld
        f'        DEFINE APP_NAME db "{name}",0\n'
        f"APP_LEN         equ {len(data)}\n"
        f"APP_TIME        equ #{fat_time(elf):08X}\n"
        f"APP_PAGE0       equ #{APP_PAGE0:02X}\n",
        encoding="ascii")
    ini = [
        f"Desc = {stem}",
        "Start = #8000",
        "Stack = #5fff",
        "Resident = #5B00",
        "Page3 = 0",
        "Clock = 2",
        "INT = 0",
        "Pager = 0",
        "",
        "Block = #c000,  2,../build/esp_app.bin",
    ] + blocks
    (build / "spgbld.ini").write_text("\n".join(ini) + "\n", encoding="ascii")
    print(f"{name}: {len(data)} байт, {pages} стр. с #{APP_PAGE0:02X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
