"""Площадь, которую рисует лист FT812, по ручкам: модель растеризатора (scripts/ft_render_model.py)
без закраски — только длины отрезков BITMAPS по строкам. python scripts/ft_speed_area.py ramg.bin dl.bin"""
import collections
import struct
import sys
from pathlib import Path

sys.path.insert(0, r"E:\zx\Tang Nano 20k VDAC\fpga\vdac2\scripts")
import ft_render_model as m  # noqa: E402

area = collections.Counter()
calls = collections.Counter()
info = {}


class Counting(m.Renderer):
    def draw_bitmap(self, v):
        h = self.handles[v[2]]
        width, height = h.width(), h.height()
        rely = self.y * m.SUB + m.PIXEL_CENTER - v[1]
        if rely < 0 or rely >= height * m.SUB:
            return
        first = m.floordiv(v[0] - m.PIXEL_CENTER + m.SUB - 1, m.SUB)
        last = m.floordiv(v[0] + width * m.SUB - m.PIXEL_CENTER + m.SUB - 1, m.SUB)
        span = self.scissor(first, last)
        if not span:
            return
        key = (v[2], h.format, width, height, h.wrap_x, h.wrap_y)
        info[key] = (h.source, h.stride(), h.layout_height())
        area[key] += span[1] - span[0]
        calls[key] += 1

    def shade(self, x, r, g, b, a):
        pass

    def clear(self, mask):
        pass


ram = Path(sys.argv[1]).read_bytes()
raw = Path(sys.argv[2]).read_bytes()
dl = list(struct.unpack(f"<{len(raw) // 4}I", raw))
Counting(ram).render(dl)
tot = sum(area.values())
print(f"всего точек BITMAPS: {tot} ({tot / (1024 * 768):.1f} экранов)")
for key, a in area.most_common(15):
    hnd, fmt, w, hgt, wx, wy = key
    src, stride, lh = info[key]
    print(f"  ручка {hnd:2d} формат {fmt:2d} {w}x{hgt} wrap {wx}{wy} src {src:#07x} stride {stride} lh {lh}: "
          f"точек {a}, отрезков {calls[key]}")
