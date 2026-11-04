"""Данные проверки видео сопроцессора (CMD_MEDIAFIFO, CMD_VIDEOSTART, CMD_VIDEOFRAME).

Ролики — MJPEG AVI генератора ftview (tests/avigen.py). Кадры — эталон libjpeg (решение
пользователя 2026-10-20, как у CMD_LOADIMAGE): декод Pillow, каналы усечены в RGB565. Разбор
потока — как у FT812 (bt8xxemu, docs/ft812-mjpeg-video.md): после каждого VIDEOFRAME слово по
ptr — длина RIFF из заголовка минус позиция за кадром (за чанком с выравниванием), 0 — файл
кончился; REG_MEDIAFIFO_READ — позиция, округлённая вверх до 4; за концом файла без кадра —
слово 0 и кадра нет; данных нет — VIDEOFRAME ждёт.

Ожидания считаются здесь по этим правилам; с BT8XXEMU_DIR — сверяются с bt8xxemu (случаи,
где bt8xxemu сбивается, помечены и не сверяются).

    python make_video_data.py   → data/video/*.avi, *.exp (кадры RGB565 подряд), list.txt

list.txt: файл, ожидание, ширина, высота, затем по VIDEOFRAME: «слово:READ» (кадр) или
«end:READ» (конец без кадра) или «wait» (ждёт данных).
"""
import io
import os
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
OUT = HERE / "data" / "video"
sys.path.insert(0, r"E:\zx\Tang Nano 20k VDAC\VDAC4\Tang Nano 20k\FTVIEW4\tests")   # avigen FTVIEW4
import avigen  # noqa: E402


def picture(n, w, h, sub=2, q=70, dht=True):
    img = Image.new("RGB", (w, h), (30 + n * 20 % 200, 60, 120))
    d = ImageDraw.Draw(img)
    for k in range(6):
        d.rectangle((k * w // 8 + n, k * h // 9, k * w // 8 + w // 6 + n, k * h // 9 + h // 5),
                    fill=((k * 50 + n * 9) % 256, (k * 90) % 256, (255 - k * 40) % 256))
    d.line((0, n, w - 1, h - 1 - n), fill=(255, 255, 0), width=3)
    out = io.BytesIO()
    img.save(out, "JPEG", quality=q, subsampling=sub)
    data = out.getvalue()
    if not dht:                       # MJPEG без таблиц Хаффмана: все DHT вырезаны
        o, res = 2, bytearray(data[:2])
        while o < len(data):
            m = data[o + 1]
            if m == 0xDA:
                res += data[o:]
                break
            ln = struct.unpack(">H", data[o + 2:o + 4])[0]
            if m != 0xC4:
                res += data[o:o + 2 + ln]
            o += 2 + ln
        data = bytes(res)
    return data


def rgb565(jpeg):
    raw = Image.open(io.BytesIO(jpeg)).convert("RGB").tobytes()
    out = bytearray(len(raw) // 3 * 2)
    for i in range(len(raw) // 3):
        v = ((raw[3 * i] >> 3) << 11) | ((raw[3 * i + 1] >> 2) << 5) | (raw[3 * i + 2] >> 3)
        out[2 * i] = v & 255
        out[2 * i + 1] = v >> 8
    return bytes(out)


def walk(avi, extra=1):
    """Ожидания по правилам FT812: список (kind, слово, READ) на каждый VIDEOFRAME."""
    end = struct.unpack("<I", avi[4:8])[0] + 8
    pos, res = 12, []
    # VIDEOSTART: чанки до avih включительно
    while True:
        cid, size = avi[pos:pos + 4], struct.unpack("<I", avi[pos + 4:pos + 8])[0]
        if cid == b"LIST":
            pos += 12
            continue
        pos += 8 + size + (size & 1)
        if cid == b"avih":
            break
    frames = 0
    while True:
        frame = ended = False
        while pos + 8 <= len(avi):
            cid, size = avi[pos:pos + 4], struct.unpack("<I", avi[pos + 4:pos + 8])[0]
            if cid == b"LIST":
                pos += 12
                continue
            pos += 8 + size + (size & 1)
            if cid[2:] in (b"dc", b"db") and size:   # кадр длины 0 — пропуск (bt8xxemu)
                frame = True
                break
            if pos >= end:                # конец файла — только на пропуске чанка
                ended = True
                break
        rd = (pos + 3) & ~3
        if frame:
            res.append(("%d:%d" % (end - pos if pos < end else 0, rd)))
            frames += 1
            continue
        res.append("end:%d" % rd if ended else "wait")
        break
    return res, frames


def cases():
    w, h = 64, 48
    fr = [picture(n, w, h) for n in range(4)]
    out = []
    avi, _, info = avigen.make_avi(4, width=w, height=h, audio=False, frame=lambda k: fr[k])
    out.append(("base420", avi, fr, w, h, True))
    avi, _, info = avigen.make_avi(4, width=w, height=h, audio=True, frame=lambda k: fr[k])
    out.append(("audio", avi, fr, w, h, True))
    ms = info["movi_start"]
    seek = [o for t, o, n in info["chunks"] if t == b"00dc"][2]
    fill = -(ms + 8) & 3
    junk = b"JUNK" + struct.pack("<I", fill + (seek & 3)) + bytes(fill)
    out.append(("seek2", avi[:ms] + junk + avi[seek & ~3:], fr[2:], w, h, True))
    recavi = bytearray(avi[:ms])
    for k in range(4):
        recavi += avigen.list_chunk(b"rec ", avigen.chunk(b"00dc", fr[k]))
    recavi[ms - 8:ms - 4] = struct.pack("<I", len(recavi) - ms + 4)
    recavi[4:8] = struct.pack("<I", len(recavi) - 8)
    out.append(("rec", bytes(recavi), fr, w, h, True))
    fr422 = [picture(n, w, h, sub=1) for n in range(3)]
    avi, _, _ = avigen.make_avi(3, width=w, height=h, audio=False, frame=lambda k: fr422[k])
    out.append(("s422", avi, fr422, w, h, True))
    frno = [picture(n, w, h, sub=1, dht=False) for n in range(3)]
    avi, _, _ = avigen.make_avi(3, width=w, height=h, audio=False, frame=lambda k: frno[k])
    out.append(("nodht422", avi, frno, w, h, True))
    frno0 = [picture(n, w, h, sub=0, dht=False) for n in range(2)]
    avi, _, _ = avigen.make_avi(2, width=w, height=h, audio=False, frame=lambda k: frno0[k])
    out.append(("nodht444", avi, frno0, w, h, True))
    avi, _, _ = avigen.make_avi(4, width=w, height=h, audio=False, frame=lambda k: fr[k], index=False)
    out.append(("noidx", avi, fr, w, h, True))
    frodd = [p + (b"\x00" if len(p) % 2 == 0 else b"") for p in (picture(n, w, h) for n in range(3))]
    avi, _, _ = avigen.make_avi(3, width=w, height=h, audio=False, frame=lambda k: frodd[k])
    out.append(("oddlen", avi, frodd, w, h, False))   # bt8xxemu сбивается на втором кадре
    # пустые чанки 00dc (повтор кадра у кодировщика; КИНО «Мама, мы все тяжело больны» — кадр 1):
    # VIDEOFRAME проходит их и декодирует следующий кадр
    seq = [fr[0], b"", fr[1], b"", b"", fr[2]]
    avi, _, _ = avigen.make_avi(len(seq), width=w, height=h, audio=True, frame=lambda k: seq[k])
    out.append(("empty", avi, fr[:3], w, h, True))
    # размер ftview: 512×384, 4:2:0 и 4:2:2 без DHT (как у ffmpeg mjpeg)
    big = [picture(n, 512, 384, q=75) for n in range(3)]
    avi, _, _ = avigen.make_avi(3, width=512, height=384, audio=True, frame=lambda k: big[k], rate=11025)
    out.append(("ftview512", avi, big, 512, 384, True))
    big2 = [picture(n, 512, 384, sub=1, q=80, dht=False) for n in range(2)]
    avi, _, _ = avigen.make_avi(2, width=512, height=384, audio=False, frame=lambda k: big2[k])
    out.append(("mjpg512", avi, big2, 512, 384, True))
    return out


def bt8xx_walk(avi, w, h, calls):
    """bt8xxemu: (слово, READ) после каждого VIDEOFRAME; 'wait' — не закончил."""
    sys.path.insert(0, str(Path(r"E:\zx\Tang Nano 20k VDAC\VDAC4\ESP32 S3 Zero N8R8\Network\Weather screensaver VDAC2") / "tools"))
    import ft812emu
    data = avi + bytes(-len(avi) % 4)
    res = []
    with ft812emu.FT812() as chip:
        chip.boot()
        chip.cmd(struct.pack("<IIII", 0xFFFFFF00, 0xFFFFFF39, 0xE0000, 0x20000))
        chip.wait_idle()
        chip.write(0xE0000, data)
        chip.wr32(0x309018, len(data))
        chip.cmd(struct.pack("<I", 0xFFFFFF40))
        chip.wait_idle()
        for _ in range(calls):
            chip.wr32(0xDFFF0, 0xDEADBEEF)
            chip.cmd(struct.pack("<IIII", 0xFFFFFF00, 0xFFFFFF41, 0, 0xDFFF0))
            try:
                chip.wait_idle(timeout=3.0)
            except Exception:  # noqa: BLE001
                res.append("wait")
                break
            res.append((chip.rd32(0xDFFF0), chip.rd32(0x309014)))
    return res


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    check = bool(os.environ.get("BT8XXEMU_DIR"))
    lines = []
    for name, avi, frames, w, h, bt_ok in cases():
        exp, n = walk(avi)
        assert n == len(frames), (name, n, len(frames))
        if check and bt_ok:
            bt = bt8xx_walk(avi, w, h, len(exp))
            mine = [e if e == "wait" else tuple(int(v) if v != "end" else 0 for v in e.split(":")) for e in exp]
            assert bt == mine, (name, bt, mine)
        (OUT / (name + ".avi")).write_bytes(avi)
        (OUT / (name + ".exp")).write_bytes(b"".join(rgb565(f) for f in frames))
        lines.append(f"{name}.avi {name}.exp {w} {h} " + " ".join(exp))
        print(lines[-1], "(bt8xxemu сверен)" if check and bt_ok else "")
    (OUT / "list.txt").write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
