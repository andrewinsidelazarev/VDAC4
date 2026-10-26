"""Промахи кэша текселей по ручкам, в порядке RTL (плитки 64×16; в плитке — битмапы по листу,
у битмапа — его строки сверху вниз, точки слева направо). Кэш как в ft_rd_exec: 256 строк по
32 байта, индекс ln[7:0] ^ ln[15:8], очистка в начале кадра; предвыборка строки L + 1 при входе
в строку L и первой строки задания (считаются без остановки). Варианты: промахи битмапов без
поворота спрятаны (предвыборка на строку вперёд); предвыборка строки кэша той же точки строкой
плитки ниже; кэш 512 строк.
python scripts/ft_speed_cache.py ramg dl
(разбор — docs/ft812-render-speed.md)"""
import array
import collections
import struct
import sys
from pathlib import Path

sys.path.insert(0, r"E:\zx\Tang Nano 20k VDAC\fpga\vdac2\scripts")
import ft_render_model as m  # noqa: E402

ram = Path(sys.argv[1]).read_bytes()
raw = Path(sys.argv[2]).read_bytes()[:8192]
dl = list(struct.unpack(f"<{len(raw)//4}I", raw)) + [0] * (2048 - len(raw) // 4)

K = array.array("Q")            # ключ сортировки: плитка, задание, строка, x
LN = array.array("I")           # строка кэша (байт >> 5)
HD = array.array("B")           # ручка
RT = array.array("B")           # поворот
cur = {"seq": 0, "h": 0, "rot": 0}
pend = []

orig_draw = m.Renderer.draw_bitmap


def draw_bitmap(self, v):
    c = self.ctx
    cur["seq"] += 1
    cur["h"] = v[2]
    cur["rot"] = 1 if (c.transform[1] != 0 or c.transform[3] != 0) else 0
    orig_draw(self, v)


orig_texel = m.Renderer.texel


def texel(self, h, base, stride, tx, ty):
    a = base + ty * stride + (tx * m.BPP.get(h.format, 8)) // 8
    pend.append((a & 0x3FFFFF) >> 5)
    return orig_texel(self, h, base, stride, tx, ty)


def shade(self, x, r, g, b, a):
    if pend:
        y = self.y
        tile = (y >> 4) * 16 + (x >> 6)
        K.append((tile << 44) | (cur["seq"] << 24) | (y << 12) | x)
        LN.append(pend[-1])
        HD.append(cur["h"])
        RT.append(cur["rot"])
    pend.clear()


m.Renderer.draw_bitmap = draw_bitmap
m.Renderer.texel = texel
m.Renderer.shade = shade
r = m.Renderer(ram, m.rom_area_with_ramp())
for y in range(768):
    cur["seq"] = 0
    r.line(dl, y)
order = sorted(range(len(K)), key=K.__getitem__)
print(f"чтений текселей {len(order)}")


# строка кэша точки (задание, y + 1, x) — для предвыборки «на строку плитки вперёд» (оракул:
# в RTL — адрес + сдвиг строки преобразования (B, E), постоянный у задания)
nxt = {}
for i in range(len(K)):
    nxt[K[i] & ~((1 << 44) - 1) | (K[i] & ((1 << 44) - 1))] = LN[i]
def below(k):
    tile, seq, y, x = k >> 44, (k >> 24) & 0xFFFFF, (k >> 12) & 0xFFF, k & 0xFFF
    if (y + 1) >> 4 != y >> 4:
        return None                                     # строка плитки — последняя
    return nxt.get((tile << 44) | (seq << 24) | ((y + 1) << 12) | x)


def idx_of(n, nlines, mask):
    return ((n & 255) ^ ((n >> 8) & 255)) & mask if nlines == 256 else ((n & mask) ^ ((n >> 9) & mask))


def run_row(nlines):
    tags = [None] * nlines
    mask = nlines - 1
    miss = 0
    fills = 0
    last = None
    for i in order:
        ln = LN[i]
        j = idx_of(ln, nlines, mask)
        if tags[j] != ln:
            tags[j] = ln
            fills += 1
            miss += 1
        if ln != last:
            for n in (ln + 1, below(K[i])):
                if n is None:
                    continue
                q = idx_of(n, nlines, mask)
                if tags[q] != n:
                    tags[q] = n
                    fills += 1
            last = ln
    return miss, fills


def run(nlines, hide_unrot):
    tags = [None] * nlines
    mask = nlines - 1
    miss = collections.Counter()
    fills = 0
    last = None
    for i in order:
        ln = LN[i]
        idx = ((ln & 255) ^ ((ln >> 8) & 255)) & mask if nlines == 256 else ((ln & mask) ^ ((ln >> 9) & mask))
        if tags[idx] != ln:
            tags[idx] = ln
            fills += 1
            if not (hide_unrot and not RT[i]):
                miss[HD[i]] += 1
        if ln != last:
            n = ln + 1                                  # предвыборка L + 1
            j = ((n & 255) ^ ((n >> 8) & 255)) & mask if nlines == 256 else ((n & mask) ^ ((n >> 9) & mask))
            if tags[j] != n:
                tags[j] = n
                fills += 1
            last = ln
    return miss, fills


for nl in (256, 512):
    mi, fi = run_row(nl)
    print(f"предвыборка L + 1 и строки плитки ниже ({nl} строк): остановок на промахе {mi}, заполнений {fi}")
px = collections.Counter(HD[i] for i in range(len(HD)))
rot = {HD[i]: RT[i] for i in range(len(HD))}
for name, nl, hide in (("сейчас (256 строк)", 256, False), ("Б: без поворота спрятаны", 256, True),
                       ("512 строк", 512, False), ("512 строк + Б", 512, True)):
    miss, fills = run(nl, hide)
    print(f"{name}: остановок на промахе {sum(miss.values())}, заполнений {fills}")
    if name.startswith("сейчас"):
        for h, n in miss.most_common(10):
            print(f"    ручка {h:2d}{' поворот' if rot[h] else '        '}: точек {px[h]:7d}, промахов {n:6d}")
