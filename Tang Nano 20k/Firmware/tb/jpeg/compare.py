"""Сверка аппаратного декодера JPEG (модель Verilator tb_jpeg.exe) с libjpeg (Pillow).

Кадры: чанки 00dc ролика AVI и/или файлы .jpg. Для каждого — расхождение точек RGB с Pillow
(наибольшее, среднее, PSNR, доля точек с отличием больше 8) и такты декодера → мс на 60 и 75 МГц.

    python compare.py tb_jpeg.exe РАБОЧИЙ_КАТАЛОГ ролик.avi|кадр.jpg …
"""
import io
import math
import struct
import subprocess
import sys
from pathlib import Path

from PIL import Image


def frames(path):
    data = Path(path).read_bytes()
    if path.lower().endswith(".avi"):
        # чанки movi по порядку (поиск «00dc» по файлу цеплял бы и записи idx1)
        out, i = [], data.find(b"movi") + 4
        end = data.find(b"idx1", i)
        end = len(data) if end < 0 else end
        while i + 8 <= end:
            cid, n = data[i:i + 4], struct.unpack("<I", data[i + 4:i + 8])[0]
            if cid == b"LIST":
                i += 12
                continue
            if cid == b"00dc" and n:
                out.append(data[i + 8:i + 8 + n])
            i += 8 + n + (n & 1)
        return out
    return [data]


def main():
    tb, work = sys.argv[1], Path(sys.argv[2])
    work.mkdir(parents=True, exist_ok=True)
    for src in sys.argv[3:]:
        for k, jpg in enumerate(frames(src)):
            jf, rf = work / ("f%03d.jpg" % k), work / ("f%03d.rgb" % k)
            jf.write_bytes(jpg)
            r = subprocess.run([tb, str(jf), str(rf)], capture_output=True, text=True)
            parts = r.stdout.split()
            if r.returncode or len(parts) < 4:
                print("%s #%d: ДЕКОДЕР НЕ ДОШЁЛ (%s %s)" % (Path(src).name, k, r.stdout.strip(), r.stderr.strip()[:200]))
                continue
            w, h, cyc = int(parts[0]), int(parts[1]), int(parts[2])
            ref = Image.open(io.BytesIO(jpg)).convert("RGB")
            got = rf.read_bytes()
            exp = ref.tobytes()
            if ref.size != (w, h):
                print("%s #%d: размер %dx%d, у Pillow %dx%d" % (Path(src).name, k, w, h, *ref.size))
                continue
            d = [abs(a - b) for a, b in zip(got, exp)]
            mse = sum(x * x for x in d) / len(d)
            psnr = 99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)
            big = sum(1 for x in d if x > 8) / len(d)
            sub = {(1, 1): "4:4:4", (2, 1): "4:2:2", (2, 2): "4:2:0"}.get(
                tuple(Image.open(io.BytesIO(jpg)).layer[0][1:3]), "?")
            print("%s #%d %dx%d %s: такты %d (%.1f мс на 60 МГц, %.1f на 75), %.2f такта/точку; "
                  "расхождение макс %d, среднее %.2f, PSNR %.1f дБ, >8: %.2f%%"
                  % (Path(src).name, k, w, h, sub, cyc, cyc / 60e3, cyc / 75e3, cyc / (w * h),
                     max(d), sum(d) / len(d), psnr, 100 * big))


if __name__ == "__main__":
    main()
