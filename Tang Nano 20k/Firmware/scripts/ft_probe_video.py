"""Замер на bt8xxemu: покадровое видео FT812 — CMD_MEDIAFIFO, CMD_VIDEOSTART, CMD_VIDEOFRAME
(как в ftview: MJPEG AVI из медиа-FIFO). AVI — генератор ftview (tests/avigen.py): кадры —
настоящие JPEG (Pillow). Для каждого кадра: что легло в RAM_G по dst (RGB565), слово
завершения по ptr, REG_MEDIAFIFO_READ, слова листа. Выход — каталог: in.avi, frameN.rgb,
info.json.

    python ft_probe_video.py OUT_DIR [кадров] [ширина высота]
"""
from __future__ import annotations

import io
import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from ft_probe_math import W2  # noqa: E402

sys.path.insert(0, str(W2 / "tools"))
FTVIEW_TESTS = Path(r"E:\zx\esp\wc-improved\source\plugins\ftview\tests")
sys.path.insert(0, str(FTVIEW_TESTS))

CMD_DLSTART, CMD_MEDIAFIFO, CMD_VIDEOSTART, CMD_VIDEOFRAME = 0xFFFFFF00, 0xFFFFFF39, 0xFFFFFF40, 0xFFFFFF41
REG_CMD_DL, REG_CMD_READ = 0x302100, 0x3020F8
REG_MEDIAFIFO_READ, REG_MEDIAFIFO_WRITE = 0x309014, 0x309018
FIFO, FIFO_SIZE, DST, RESULT = 0xE0000, 0x20000, 0x0, 0xDFFF0


def frames_jpeg(n, w, h):
    from PIL import Image, ImageDraw
    img = Image.new("RGB", (w, h), (30 + n * 20 % 200, 60, 120))
    d = ImageDraw.Draw(img)
    for k in range(6):
        d.rectangle((k * w // 8 + n, k * h // 9, k * w // 8 + w // 6 + n, k * h // 9 + h // 5),
                    fill=((k * 50 + n * 9) % 256, (k * 90) % 256, (255 - k * 40) % 256))
    out = io.BytesIO()
    img.save(out, "JPEG", quality=70)
    return out.getvalue()


def main(argv: list[str]) -> int:
    import ft812emu
    import avigen
    out = Path(argv[1])
    out.mkdir(parents=True, exist_ok=True)
    nfr = int(argv[2]) if len(argv) > 2 else 4
    w, h = (int(argv[3]), int(argv[4])) if len(argv) > 4 else (64, 48)
    avi, _pcm, _info = avigen.make_avi(nfr, width=w, height=h, audio=False, frame=lambda k: frames_jpeg(k, w, h))
    (out / "in.avi").write_bytes(avi)
    data = avi + bytes(-len(avi) % 4)
    assert len(data) < FIFO_SIZE
    info = {"frames": [], "avi_bytes": len(avi), "fifo": FIFO, "fifo_size": FIFO_SIZE, "dst": DST, "result": RESULT}
    with ft812emu.FT812() as chip:
        chip.boot()
        chip.write(FIFO, data)
        chip.wr32(REG_MEDIAFIFO_WRITE, len(data))
        chip.cmd(struct.pack("<I", CMD_DLSTART) + struct.pack("<III", CMD_MEDIAFIFO, FIFO, FIFO_SIZE))
        chip.wait_idle()
        chip.wr32(REG_MEDIAFIFO_READ, 0)
        chip.wr32(REG_MEDIAFIFO_WRITE, len(data))
        chip.cmd(struct.pack("<I", CMD_VIDEOSTART))
        chip.wait_idle()
        info["after_start_read"] = chip.rd32(REG_MEDIAFIFO_READ)
        for k in range(nfr + 1):
            chip.wr32(RESULT, 0xDEADBEEF)
            chip.cmd(struct.pack("<I", CMD_DLSTART) + struct.pack("<III", CMD_VIDEOFRAME, DST, RESULT))
            try:
                chip.wait_idle(timeout=3.0)
                done = True
            except TimeoutError:
                done = False
            n = chip.rd32(REG_CMD_DL)
            words = list(struct.unpack(f"<{n // 4}I", chip.read(0x300000, n))) if n else []
            fr = {"k": k, "done": done, "result": chip.rd32(RESULT), "fifo_read": chip.rd32(REG_MEDIAFIFO_READ),
                  "dl": words}
            if done:
                (out / f"frame{k}.rgb").write_bytes(chip.read(DST, w * h * 2))
            info["frames"].append(fr)
            print(k, {x: v for x, v in fr.items() if x != "dl"}, "dl", " ".join(f"{v:08X}" for v in words))
            if not done:
                break
    (out / "info.json").write_text(json.dumps(info, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
