"""Таблицы проигрывателя MoonBlaster Wave (MWM) для OPL.ELF: modules/opl/main/mwm_tables.h.

Источник — таблицы оригинального Z80-проигрывателя из ZXM-MoonSound (порт Mick
2015 проигрывателя Moonsoft 1995): freq_table_0..4.inc (деление нот на октавы,
частоты волн ПЗУ и своих волн) и patch_table.inc (патчи ПЗУ YRW801: 176
наборов «нота → тон, частота, байты заголовка», ударные GM). Они собираются
sjasmplus с адреса 0 — указатели внутри становятся смещениями от начала
таблиц — и ещё раз с адреса #4000: все различия должны быть старшими байтами
указателей на сам блок (иначе отказ).

Запуск: python tools/make_mwm_tables.py [папка moonmusic_01]
"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else
                      r"E:\zx\esp\downloads\zxm-moonsound\demo-disks\moonmusic_01")
SJASM = pathlib.Path(r"E:\zx\z80\zuma\sjasmplus.exe")
OUT = ROOT / "modules" / "opl" / "main" / "mwm_tables.h"
NAMES = {
    "MBPlayer_tabdiv12": "MWM_TABDIV12",
    "MBPlayer_frqtab_441khz": "MWM_FRQ_441KHZ",
    "MBPlayer_frqtab_amiga": "MWM_FRQ_AMIGA",
    "MBPlayer_frqtab_turbo": "MWM_FRQ_TURBO",
    "MBPlayer_patch_table": "MWM_PATCH_TABLE",
    "MBPlayer_gmdrm_c4": "MWM_GMDRM_C4",
}


def assemble(work: pathlib.Path, origin: int) -> tuple[bytes, dict]:
    name = f"tables_{origin:04x}"
    (work / f"{name}.asm").write_text(
        "\t\tDEVICE NOSLOT64K\n"
        f"\t\tORG {origin:#06x}\n"
        "tab_start:\n"
        + "".join(f'\t\tINCLUDE "freq_table_{i}.inc"\n' for i in range(4))
        + '\t\tINCLUDE "patch_table.inc"\n'
        '\t\tINCLUDE "freq_table_4.inc"\n'
        "tab_end:\n"
        f'\t\tSAVEBIN "{name}.bin", tab_start, tab_end - tab_start\n')
    subprocess.run([str(SJASM), f"--inc={SOURCE}", f"--sym={name}.sym", f"{name}.asm"],
                   cwd=work, check=True, capture_output=True)
    symbols = {}
    for line in (work / f"{name}.sym").read_text().splitlines():
        if ": EQU " in line:
            label, value = line.split(": EQU ")
            symbols[label.strip()] = int(value, 16) - origin
    return (work / f"{name}.bin").read_bytes(), symbols


def main() -> None:
    with tempfile.TemporaryDirectory() as temp:
        work = pathlib.Path(temp)
        data, symbols = assemble(work, 0)
        moved, _ = assemble(work, 0x4000)
    assert len(data) == len(moved)
    pointers = 0
    for i, (a, b) in enumerate(zip(data, moved)):
        if a != b:
            assert b - a == 0x40 and i > 0, f"не указатель в {i:#x}"
            target = data[i - 1] | a << 8
            assert target < len(data), f"указатель {target:#x} мимо таблиц"
            pointers += 1
    lines = [
        "/* mwm_tables.h — таблицы проигрывателя MoonBlaster Wave (MWM).",
        " * Сделано tools/make_mwm_tables.py из freq_table_0..4.inc и patch_table.inc",
        " * Z80-проигрывателя ZXM-MoonSound (порт Mick 2015 проигрывателя Moonsoft",
        " * 1995). Указатели внутри (dw) — смещения от начала mwm_tables. Не править. */",
        "#ifndef MWM_TABLES_H",
        "#define MWM_TABLES_H",
        "",
        "#include <stdint.h>",
        "",
    ]
    for label, name in NAMES.items():
        lines.append(f"#define {name} {symbols[label]:#06x}")
    lines.append(f"#define MWM_TABLES_SIZE {len(data)}")
    lines.append("")
    lines.append(f"/* {pointers} указателей. */")
    lines.append("static const uint8_t mwm_tables[MWM_TABLES_SIZE] = {")
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("#endif  /* MWM_TABLES_H */")
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(OUT, len(data), "bytes,", pointers, "pointers")


if __name__ == "__main__":
    main()
