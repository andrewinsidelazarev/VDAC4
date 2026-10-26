"""Замер на bt8xxemu: текст ПЗУ-шрифтами на экране — кадр bt8xxemu против модели
(ft_render_model.py с настоящим образом ПЗУ, FT812_ROM). Лист собирает сопроцессор bt8xxemu
(CMD_TEXT разными шрифтами 16…31, в том числе как у Zuma Deluxe), он же сохраняется в OUT_DIR
(text_dl.bin, ramg.bin — нули) для сверки RTL: ft_rtl_check.py prep/check OUT_DIR ramg.bin text_dl.bin.

    FT812_ROM=…/ft812_rom_1E0000.bin python ft_probe_romtext.py OUT_DIR
"""
from __future__ import annotations

import struct
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ft_render_model as model  # noqa: E402
from ft_golden import golden_frame  # noqa: E402
from ft_probe_text import text_cmd, OPT_CENTERX, OPT_RIGHTX  # noqa: E402

CMD_DLSTART = 0xFFFFFF00
REG_CMD_DL = 0x302100

TEXTS = [(10 if f < 24 else 520, 10 + 46 * ((f - 16) % 8) * 2 % 740, f, 0, f"F{f} AaBb 0123 :;!?@#") for f in range(16, 32)]
TEXTS += [(45, 712, 26, 0, "12:34:56"), (998, 19, 28, OPT_RIGHTX, "GS 1 MB"),
          (256, 444, 29, 0, "Loading"), (512, 600, 31, OPT_CENTERX, "Zuma")]


def w(op, v=0):
    return (op << 24) | v


def main(argv: list[str]) -> int:
    import ft812emu
    out = Path(argv[1])
    out.mkdir(parents=True, exist_ok=True)
    s = struct.pack("<I", CMD_DLSTART)
    s += struct.pack("<3I", w(0x02, 0x102030), w(0x26, 7), w(0x04, 0xFFFFFF))   # CLEAR_COLOR, CLEAR, COLOR
    for x, y, font, opt, t in TEXTS:
        s += text_cmd(x, y, font, opt, t)
    s += struct.pack("<I", w(0x00))                                              # DISPLAY
    with ft812emu.FT812() as chip:
        chip.boot()
        chip.cmd(s)
        chip.wait_idle()
        n = chip.rd32(REG_CMD_DL)
        dl = list(struct.unpack(f"<{n // 4}I", chip.read(0x300000, n)))
    (out / "text_dl.bin").write_bytes(struct.pack(f"<{len(dl)}I", *dl))
    ram = bytes(1 << 16)
    (out / "ramg.bin").write_bytes(ram)
    time.sleep(0.1)
    from PIL import Image, ImageChops
    ref = golden_frame(ram, dl).convert("RGB")
    ref.save(out / "bt8xxemu.png")
    mine = Image.frombytes("RGB", (1024, 768), model.Renderer(ram, model.rom_area_with_ramp()).render(dl))
    mine.save(out / "model.png")
    diff = ImageChops.difference(ref, mine)
    diff.point(lambda v: min(255, v * 16)).save(out / "diff.png")
    px = list(diff.getdata())
    bad = sum(1 for p in px if max(p) > 0)
    lit = sum(1 for p in ref.getdata() if p != (0x10, 0x20, 0x30))
    print(f"лист {len(dl)} слов; точек текста у bt8xxemu {lit}; расходятся с моделью {bad}")
    return 0 if bad == 0 and lit > 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
