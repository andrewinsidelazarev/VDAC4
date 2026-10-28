"""Тестовый AVI для стенда ядра видео (tb_vpc.exe): все случаи разбора vp_avi в одном потоке.

    python make_vp_avi.py КАТАЛОГ_JPG КАТАЛОГ_ЭТАЛОНОВ вых.avi вых.txt [kino|hd]

Кадры — .jpg из КАТАЛОГ_JPG (кадры КИНО f000…f007, картинки q_*/o_*), эталоны RGB888 — .rgb
(tb_jpeg). вых.txt — ожидаемое: строка на слот темпа — «кадр ЭТАЛОН ШИРИНА ВЫСОТА» (объявленный
кадр; размер — из SOF) или «повтор» (пустой чанк); отменённый сторожем кадр слота не занимает.
Последняя строка — «отменено N». Рядом — вых.aud: байты звуковых чанков (01wb) подряд — что
должно прийти в FIFO звука ядра. kino — короткий поток для системного стенда (vvid): кадры КИНО
512×384 и повтор; hd — кадры больше 1024×768 (hd_*.jpg: ядро кладёт их вдвое меньше) и чанки
чужих дорожек (02wb, 01dc).
"""
import struct
import sys
from pathlib import Path


def chunk(fcc, data):
    out = fcc + struct.pack("<I", len(data)) + data
    return out + (b"\0" if len(data) & 1 else b"")


def sof_size(jpg):
    """Ширина и высота картинки из SOF0."""
    i = jpg.find(b"\xFF\xC0")
    return jpg[i + 7] << 8 | jpg[i + 8], jpg[i + 5] << 8 | jpg[i + 6]


def strip_dht(jpg):
    """Кадр без сегментов DHT (как MJPEG камер): декодер берёт стандартные таблицы K.3."""
    out, i = bytearray(jpg[:2]), 2
    while i < len(jpg):
        m = jpg[i + 1]
        if m == 0xDA:
            out += jpg[i:]
            break
        n = jpg[i + 2] << 8 | jpg[i + 3]
        if m != 0xC4:
            out += jpg[i:i + 2 + n]
        i += 2 + n
    return bytes(out)


def main(argv):
    src, ref, avi_out, exp_out = map(Path, argv[1:5])
    jpg = lambda n: (src / (n + ".jpg")).read_bytes()
    movi, exp, aud = [], [], []

    def audio(data):
        movi.append(chunk(b"01wb", data))
        aud.append(data)

    def frame(name, data=None, ref_name=None):
        data = jpg(name) if data is None else data
        movi.append(chunk(b"00dc", data))
        w, h = sof_size(data)
        exp.append("кадр %s %d %d" % (ref / ((ref_name or name) + ".rgb"), w, h))

    if len(argv) > 5 and argv[5] == "kino":
        # короткий поток для системного стенда (vvid): кадры КИНО 512×384 и повтор
        audio(bytes((i * 3) & 0xFF for i in range(1280)))
        frame("f000")
        audio(bytes((i * 5 + 1) & 0xFF for i in range(1281)))
        frame("f001")
        movi.append(chunk(b"00dc", b""))
        exp.append("повтор")
        frame("q_512x288_422_opt")                             # 16:9 — на холсте с полями
        frame("f002")
        frame("f003")
        return write(avi_out, exp_out, movi, exp, 0, aud)

    if len(argv) > 5 and argv[5] == "wide":
        # кадры FTViewConvert для широкоформатных исходников (1,85:1, 2,39:1, 16:10) и 16:9 —
        # с метками по краям (для системного стенда vvid --dump: виден ли край на выходе 720p)
        audio(bytes((i * 3) & 0xFF for i in range(1280)))
        for n in ("w_592x320_420_std", "w_608x256_420_std", "w_512x320_420_std", "w_512x288_420_std"):
            frame(n)
        return write(avi_out, exp_out, movi, exp, 0, aud)

    if len(argv) > 5 and argv[5] == "hd":
        # кадры больше 1024×768 — ядро кладёт их вдвое меньше (1.38); вторая звуковая дорожка
        # (02wb) и чужое видео (01dc) — мимо FIFO звука и декодера
        audio(bytes((i * 13) & 0xFF for i in range(2000)))
        movi.append(chunk(b"02wb", bytes((i * 3 + 7) & 0xFF for i in range(1500))))
        frame("hd_1280x720_420_std")
        movi.append(chunk(b"01dc", jpg("f000")))
        frame("hd_1296x730_422_std")
        audio(bytes((i * 17) & 0xFF for i in range(777)))
        frame("hd_1100x400_444_std")
        frame("f001")
        return write(avi_out, exp_out, movi, exp, 0, aud)

    for n in ("f000", "f001", "f002"):
        frame(n)
    movi.append(chunk(b"JUNK", b"\xFF\xD8\xFF" * 7))           # мусор с маркерами — пропуск
    frame("f003")
    movi.append(chunk(b"00dc", b""))                           # пустой — повтор кадра
    exp.append("повтор")
    audio(bytes(range(255)) * 3)                               # звук, нечётная длина (765)
    audio(b"\x01\x02\x03")                                     # 3 байта — слово собирается из чанков
    movi.append(chunk(b"02wb", bytes(range(200))))             # вторая дорожка — мимо (1.38)
    audio(bytes((i * 7) & 0xFF for i in range(1000)))
    frame("f004", jpg("f004") + b"\0")                         # чанк нечётной длины
    # кадр без DHT: эталон — тот же файл с таблицами (у _std они и есть стандартные K.3)
    frame("q_48x40_422_std", strip_dht(jpg("q_48x40_422_std")))
    movi.append(chunk(b"00dc", jpg("f005")[:6000]))            # обрыв: сторож отменит
    movi.append(chunk(b"00dc", b""))                           # повтор после отменённого
    exp.append("повтор")
    frame("f006")
    frame("o_50x30_420_opt")
    rec = b"rec " + chunk(b"00dc", jpg("f007"))                # кадр внутри LIST rec
    movi.append(chunk(b"LIST", rec))
    exp.append("кадр %s %d %d" % (ref / "f007.rgb", *sof_size(jpg("f007"))))
    audio(bytes((i * 11) & 0xFF for i in range(4097)))         # больше FIFO звука (4 КБ)
    frame("t_64x64_444_opt")
    write(avi_out, exp_out, movi, exp, 1, aud)


def write(avi_out, exp_out, movi, exp, aborts, aud):
    avih = struct.pack("<IIIIIIIIII", 40000, 0, 0, 0x10, len(exp), 0, 1, 0, 512, 384) + b"\0" * 16
    strh = b"vidsMJPG" + struct.pack("<IHHIIIIIIIIhhhh", 0, 0, 0, 0, 1, 25, 0, len(exp), 0, 0xFFFFFFFF, 0, 0, 0, 512, 384)
    strf = struct.pack("<IiiHH4sIiiII", 40, 512, 384, 1, 24, b"MJPG", 512 * 384 * 3, 0, 0, 0, 0)
    hdrl = chunk(b"LIST", b"hdrl" + chunk(b"avih", avih) + chunk(b"LIST", b"strl" + chunk(b"strh", strh) + chunk(b"strf", strf)))
    movi_l = chunk(b"LIST", b"movi" + b"".join(movi))
    idx1 = chunk(b"idx1", b"\0" * 64)
    body = b"AVI " + hdrl + movi_l + idx1
    avi_out.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)
    exp.append(f"отменено {aborts}")
    exp_out.write_text("\n".join(exp) + "\n", encoding="utf-8")
    exp_out.with_suffix(".aud").write_bytes(b"".join(aud))
    print(f"{avi_out}: {len(body) + 8} байтов, слотов {len(exp) - 1}")


if __name__ == "__main__":
    main(sys.argv)
