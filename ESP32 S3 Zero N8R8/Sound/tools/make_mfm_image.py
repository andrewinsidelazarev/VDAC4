"""Образ памяти проигрывателя MoonBlaster FM (MFM) для OPL.ELF: modules/opl/main/mfm_image.h.

Источник — Z80-проигрыватель из ZXM-MoonSound (mfm_player.asm, порт Mick 2015
проигрывателя Moonsoft 1995) со своими таблицами (freq_table_0/1.inc,
patch_table.inc). Порт на C (mfm.cpp) работает с той же раскладкой памяти,
что у Z80: проигрыватель собирается sjasmplus с адреса 0, и всё от первой
таблицы данных до конца — массив; адреса внутри (dw таблиц, указатели в
таблицах каналов) становятся смещениями в нём. Код в массив не идёт.

Запуск: python tools/make_mfm_image.py [папка mfm_sample_02]
"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else
                      r"E:\zx\esp\downloads\zxm-moonsound\demo-disks\mfm_sample_02")
SJASM = pathlib.Path(r"E:\zx\z80\zuma\sjasmplus.exe")
OUT = ROOT / "modules" / "opl" / "main" / "mfm_image.h"
FIRST = "unk_0_4BFB"                 # первая таблица данных: дальше — массив
NAMES = {
    "unk_0_4BFB": "OPS2",             # регистры 2-операторных каналов: 4 байта на канал
    "unk_0_4C43": "OPS4",             # регистры 4-операторных: 22 байта на канал
    "unk_0_4CC8": "FNUM",             # F-номера и блоки нот FM
    "MBPlayer_play_table_wav_2": "TABLE_2OP",
    "MBPlayer_play_table_wav_1": "TABLE_4OP",
    "MBPlayer_play_table_wav_3": "TABLE_WAVE",
    "unk_0_4F29": "INSTR_4OP",        # смещение голоса 4-операторного (x2)
    "unk_0_4F2A": "INSTR_2OP",        # смещение голоса 2-операторного
    "unk_0_4F2B": "TIMES_11",
    "unk_0_4F43": "MASK_4OP",
    "MBPlayer_play_busy": "BUSY",
    "MBPlayer_play_pos": "POS",
    "MBPlayer_play_step": "STEP",
    "MBPlayer_step_buffer": "STEP_BUFFER",
    "MBPlayer_step_buffer_1": "STEP_WAVE",
    "MBPlayer_play_speed": "SPEED",
    "MBPlayer_play_tspval": "TRANSPOSE",
    "MBPlayer_play_timercnt": "TIMER",
    "MBPlayer_play_chvol_2": "COUNT_2OP",
    "byte_0_4F50": "PATTERN",
    "MBPlayer_xleng": "XLENG",
    "MBPlayer_xloop": "XLOOP",
    "unk_0_50F4": "VOICES_2OP",
    "unk_0_51FC": "VOICES_4OP",
    "unk_0_5318": "XMODTAB",
    "MBPlayer_xtempo": "XTEMPO",
    "MBPlayer_xhzequal": "XHZEQUAL",
    "byte_0_531E": "XBD",
    "unk_0_5328": "XMODTAB_FM",
    "MBPlayer_play_chvol_1": "COUNT_4OP",
    "unk_0_5338": "XMODTAB_FM2",
    "unk_0_5368": "XBEGIN",
    "unk_0_537A": "XBEGIN_WAVE",
    "MBPlayer_xwavnrs": "XWAVNRS",
    "MBPlayer_xwavvols": "XWAVVOLS",
    "tabdiv12": "TABDIV12",
    "MBPlayer_patch_table": "PATCH_TABLE",
    "gmdrm_c4": "GMDRM_C4",
}


def main() -> None:
    with tempfile.TemporaryDirectory() as temp:
        work = pathlib.Path(temp)
        (work / "image.asm").write_text(
            "\t\tDEVICE NOSLOT64K\n\t\tORG 0\n"
            '\t\tINCLUDE "mfm_player.asm"\n'
            "image_end:\n"
            '\t\tSAVEBIN "image.bin", 0, image_end\n')
        subprocess.run([str(SJASM), f"--inc={SOURCE}", "--sym=image.sym", "image.asm"],
                       cwd=work, check=True, capture_output=True)
        data = (work / "image.bin").read_bytes()
        symbols = {}
        for line in (work / "image.sym").read_text().splitlines():
            if ": EQU " in line:
                label, value = line.split(": EQU ")
                symbols[label.strip()] = int(value, 16)
    base = symbols[FIRST]
    lines = [
        "/* mfm_image.h — память проигрывателя MoonBlaster FM (MFM): таблицы и",
        " * переменные с той же раскладкой, что у Z80 (смещения — адреса сборки с 0).",
        " * Сделано tools/make_mfm_image.py из mfm_player.asm ZXM-MoonSound (порт Mick",
        " * 2015 проигрывателя Moonsoft 1995). Не править. */",
        "#ifndef MFM_IMAGE_H",
        "#define MFM_IMAGE_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define MFM_BASE {base:#06x}",
        f"#define MFM_END {len(data):#06x}",
    ]
    for label, name in NAMES.items():
        lines.append(f"#define MFM_{name} {symbols[label]:#06x}")
    lines.append("")
    lines.append("static const uint8_t mfm_image[MFM_END - MFM_BASE] = {")
    tail = data[base:]
    for i in range(0, len(tail), 16):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in tail[i:i + 16]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("#endif  /* MFM_IMAGE_H */")
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(OUT, len(tail), "bytes from", hex(base))


if __name__ == "__main__":
    main()
