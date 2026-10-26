"""Тракт «две точки за такт»: по ручкам — точки, годные без смешения (альфа текселя 0 или
непрозрачная; 4-битная 15, у bt8xxemu 254, считается 255), и такты, если пара соседних точек
(x чётный, x + 1) обе годны и их тексели — в одном 32-битном слове. Состояние —
по умолчанию (COLOR_RGB белый, COLOR_A 255, смешение по умолчанию, без трафарета и
ALPHA_FUNC, COLOR_MASK 1111). python scripts/ft_speed_pairs.py ramg dl [шаг строк]
(разбор — docs/ft812-render-speed.md)"""
import collections
import struct
import sys
from pathlib import Path

sys.path.insert(0, r"E:\zx\Tang Nano 20k VDAC\fpga\vdac2\scripts")
import ft_render_model as m  # noqa: E402

ram = Path(sys.argv[1]).read_bytes()
raw = Path(sys.argv[2]).read_bytes()[:8192]
dl = list(struct.unpack(f"<{len(raw)//4}I", raw)) + [0] * (2048 - len(raw) // 4)
step = int(sys.argv[3]) if len(sys.argv) > 3 else 4

px = collections.Counter()      # точек по ручке
fast = collections.Counter()    # годных
cyc2 = collections.Counter()    # тактов при двух точках за такт
rot = {}
cur = {"h": None, "ok": False, "span": []}
pend = []                       # слово текселя текущей точки


def flush():
    s = cur["span"]
    if not s:
        return
    h = cur["h"]
    i = 0
    n = 0
    while i < len(s):
        x, w, f = s[i]
        if (f and x % 2 == 0 and i + 1 < len(s) and s[i + 1][0] == x + 1 and s[i + 1][2]
                and s[i + 1][1] == w):
            i += 2
        else:
            i += 1
        n += 1
    cyc2[h] += n
    cur["span"] = []


orig_draw = m.Renderer.draw_bitmap


def draw_bitmap(self, v):
    c = self.ctx
    hd = self.handles[v[2]]
    ok = (not hd.filter and c.rgb == 0xFFFFFF and c.a == 255 and (c.blend_src, c.blend_dst) == (2, 4)
          and c.alpha_func == 7 and c.color_mask == 15
          and ((c.st_func == 7 and c.st_pass == 1 and c.st_fail == 1) or c.st_wmask == 0) and c.st_func == 7)
    cur["h"] = v[2]
    cur["ok"] = ok
    rot[v[2]] = rot.get(v[2], False) or c.transform[1] != 0 or c.transform[3] != 0
    orig_draw(self, v)
    flush()
    cur["h"] = None


orig_texel = m.Renderer.texel


def texel(self, h, base, stride, tx, ty):
    t = orig_texel(self, h, base, stride, tx, ty)
    a = base + ty * stride + (tx * m.BPP.get(h.format, 8)) // 8
    pend.append(((a & 0x3FFFFF) >> 2, t[3]))
    return t


def shade(self, x, r, g, b, a):
    if cur["h"] is not None and pend:
        w, ta = pend[-1]
        f = cur["ok"] and ta in (0, 254, 255)
        px[cur["h"]] += 1
        fast[cur["h"]] += f
        cur["span"].append((x, w, f))
    pend.clear()


m.Renderer.draw_bitmap = draw_bitmap
m.Renderer.texel = texel
m.Renderer.shade = shade
r = m.Renderer(ram, m.rom_area_with_ramp())
for y in range(0, 768, step):
    r.line(dl, y)
    flush()
tp, tf, tc = sum(px.values()), sum(fast.values()), sum(cyc2.values())
print(f"точек битмапов {tp * step}, годны {100 * tf / max(1, tp):.1f} %, тактов при 2 точках за такт "
      f"{tc * step} ({100 * tc / max(1, tp):.1f} % от точек)")
for h, n in px.most_common(12):
    print(f"  ручка {h:2d}{' поворот' if rot.get(h) else '        '}: точек {n * step:7d}, годны "
          f"{100 * fast[h] / n:5.1f} %, тактов {cyc2[h] * step:7d} ({100 * cyc2[h] / n:5.1f} %)")
