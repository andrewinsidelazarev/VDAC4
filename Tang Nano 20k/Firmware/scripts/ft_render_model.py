"""Эталонная модель растеризатора FT812 для RTL Tang (шаг 4, docs/ft812-emulation.md).

Кадр строится по строкам, как у FT812: на каждой строке дисплей-лист выполняется с начала,
каждый примитив отдаёт свой отрезок на этой строке, пиксели отрезка идут через конвейер
(проверка альфы, трафарет, смешение, маска каналов) в буфер строки RGBA + трафарет.
Арифметика — целая, та же, что будет в ПЛИС, и та же, что в eve-emu:

    eve-emu, Copyright (c) 2026 Ilia Sharin, MIT License
    (alfishe/unreal-ng, core/src/3rdparty/eve-emu: eve-raster.cpp, eve-pixel.cpp,
    eve-bitmap.cpp, eve-dl.cpp)

Где bt8xxemu считает иначе, чем eve-emu, — как bt8xxemu (замеры scripts/ft_probe_*.py):
умножение 8×8 / 255 с отбрасыванием остатка, раскрытие каналов 3…6 бит таблицами,
сглаживание краёв линейной рампой.

Сейчас — то, что нужно заставке Weather 2 и большей части TSLib: BITMAPS (все прямые
форматы, L1/L2/L4/L8, NEAREST и BILINEAR, BITMAP_TRANSFORM, BORDER/REPEAT, CELL), POINTS,
RECTS (LINE_WIDTH), LINES/LINE_STRIP; CLEAR, трафарет, смешение, ALPHA_FUNC, COLOR_MASK,
SCISSOR, SAVE/RESTORE_CONTEXT, CALL/JUMP/RETURN, MACRO. Без PALETTED, TEXT8X8/TEXTVGA,
EDGE_STRIP и тегов (по факту программ).

Запуск: python ft_render_model.py ramg.bin dl.bin out.png [--rom rom_area.bin]
"""
from __future__ import annotations

import os

import math
import struct
import sys
from pathlib import Path

SUB = 16                    # координаты — в 1/16 пикселя
PIXEL_CENTER = 0            # центр пикселя x — в x * 16 (eve-emu, kPixelCenter)
AA_REACH = 8                # покрытие нулевое дальше радиуса на 8/16 пикселя
HALF_RAMP = AA_REACH        # охват точки и линии за радиусом при поиске отрезка
RAM_G_SIZE = 3 * 1024 * 1024
ROM_BASE = 0x1E0000
ROM_END = 0x300000
# Ручки 16…31 после сброса — шрифты ПЗУ FT812 (таблица ROM_FONTROOT = 0x201EE0; снято с
# bt8xxemu): формат (L1, L4), шаг строки, ширина, высота, адрес глифа 0. Как в ft_rd_walk.sv.
ROM_FONT_HANDLES = {
    16: (1, 1, 8, 8, 0x2FF7FC), 17: (1, 1, 8, 8, 0x2FFBFC), 18: (1, 1, 8, 16, 0x2FE7FC),
    19: (1, 1, 8, 16, 0x2FEFFC), 20: (1, 2, 10, 13, 0x2FDAFC), 21: (1, 2, 13, 17, 0x2FCD3C),
    22: (1, 2, 14, 20, 0x2FBD7C), 23: (1, 3, 17, 22, 0x2FA17C), 24: (1, 3, 24, 29, 0x2F7E3C),
    25: (1, 4, 30, 38, 0x2F3D1C), 26: (2, 7, 14, 16, 0x2F181C), 27: (2, 8, 16, 20, 0x2ED61C),
    28: (2, 9, 18, 25, 0x2E799C), 29: (2, 11, 22, 28, 0x2DFBBC), 30: (2, 14, 28, 36, 0x2D263C),
    31: (2, 18, 36, 49, 0x2BAC3C),
}


def multiply(a: int, b: int) -> int:
    """Произведение двух 8-битных величин / 255 с отбрасыванием остатка. Так считает bt8xxemu
    и в умножении текселя на цвет, и в смешении (замеры scripts/ft_probe_math.py; eve-emu
    округляет — у него это «TO VERIFY»)."""
    return (a * b) // 255


# Три места умножения 8×8 — округление у каждого своё (подбирается по bt8xxemu)
def mul_tex(a: int, b: int) -> int:
    """Тексель × COLOR_RGB / COLOR_A."""
    return multiply(a, b)


def mul_texa(a: int, b: int) -> int:
    """Альфа текселя × COLOR_A."""
    return multiply(a, b)


def mul_cov(a: int, b: int) -> int:
    """COLOR_A × покрытие края."""
    return multiply(a, b)


def mul_blend(a: int, b: int) -> int:
    """Канал × множитель смешения."""
    return multiply(a, b)


def blend_channel(s: int, sf: int, d: int, df: int) -> int:
    """Канал после смешения: s × sf + d × df, с насыщением."""
    return min(255, mul_blend(s, sf) + mul_blend(d, df))


# Раскрытие n-битного канала в 8 бит — таблицы, снятые с bt8xxemu (scripts/ft_probe_expand.py):
# 1 и 2 бита — точное (255v, 85v), а 3…6 бит — не повтором разрядов, как в eve-emu
# (например, L4 15 -> 254, RGB565 R 31 -> 254).
EXPAND = {
    1: [0, 255],
    2: [0, 85, 170, 255],
    3: [0, 36, 71, 107, 143, 179, 215, 251],
    4: [0, 17, 33, 50, 67, 84, 101, 118, 135, 152, 169, 186, 203, 220, 237, 254],
    5: [0, 8, 16, 24, 32, 41, 49, 57, 65, 73, 82, 90, 98, 106, 115, 123, 131, 139, 147, 156, 164, 172, 180, 189, 197, 205, 213, 221, 230, 238, 246, 254],
    6: [0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 89, 93, 97, 101, 105, 109, 113, 117, 121, 125, 129, 133, 137, 141, 145, 149, 153, 157, 161, 165, 169, 174, 178, 182, 186, 190, 194, 198, 202, 206, 210, 214, 218, 222, 226, 230, 234, 238, 242, 246, 250, 254],
}


def expand(value: int, bits: int) -> int:
    """n-битный канал в 8 бит (таблица bt8xxemu); 8 бит — как есть."""
    return value if bits == 8 else EXPAND[bits][value]


def expand_alpha(value: int, bits: int) -> int:
    """Альфа в 8 бит: как expand, но 4-битная 15 — 255. У bt8xxemu 254 (как у цветов) — «глюк
    FT812» по решению 2026-10-19: фон Zuma на деле непрозрачный, и только с 255 точке не нужно
    смешение (растеризатор Tang рисует такие точки по две за такт)."""
    return 255 if bits == 4 and value == 15 else expand(value, bits)


def fine_distance(dx: int, dy: int) -> int:
    """Расстояние до точки (dx, dy в 1/16 пикселя) — в 1/256 пикселя, с отбрасыванием."""
    return isqrt((dx * dx + dy * dy) << 8)


def point_alpha(radius: int, dx: int, dy: int) -> int:
    """Покрытие пикселя точки (1/256): рампа в пиксель шириной вокруг радиуса, расстояние —
    в 1/256 пикселя (снято с bt8xxemu, scripts/ft_probe_aa.py, ft_probe_corner.py)."""
    if radius == 0:
        return 0
    return max(0, min(256, (radius + AA_REACH) * 16 - fine_distance(dx, dy)))


def axis_alpha(p: int, a: int, b: int, r: int) -> int:
    """Покрытие по одной оси прямоугольника (ядро [a, b], радиус r, центр пикселя p; всё в 1/16).

    Снято с bt8xxemu (scripts/ft_probe_rect.py): перекрытие полосы пикселя [p − 8, p + 8] с
    фигурой [a − r, b + r] × наклон min(r, 16), минус 1; если фигура целиком в полосе
    пикселя — её длина × 16 − 1.
    """
    lo, hi = a - r, b + r
    if lo >= p - 8 and hi <= p + 8:
        return max(0, min(256, (hi - lo) * 16))
    overlap = min(p + 8, hi) - max(p - 8, lo)
    if overlap <= 0:
        return 0
    return max(0, min(256, overlap * min(r, 16)))


def corner_alpha(radius: int, dx: int, dy: int) -> int:
    """Скруглённый угол прямоугольника (1/256): по расстоянию до угла ядра в 1/256 пикселя,
    наклон min(r, 16) (снято с bt8xxemu, scripts/ft_probe_corner.py)."""
    if radius == 0:
        return 0
    return max(0, min(256, ((radius + AA_REACH) * 16 - fine_distance(dx, dy)) * min(radius, 16) // 16))


def isqrt(v: int) -> int:
    return math.isqrt(v)


def field(w: int, hi: int, lo: int) -> int:
    return (w >> lo) & ((1 << (hi - lo + 1)) - 1)


def sfield(w: int, hi: int, lo: int) -> int:
    v = field(w, hi, lo)
    n = hi - lo + 1
    return v - (1 << n) if v & (1 << (n - 1)) else v


def floordiv(a: int, b: int) -> int:
    return a // b               # в Python // уже округляет вниз


# --- форматы битмапов ------------------------------------------------------------------------
ARGB1555, L1, L4, L8, RGB332, ARGB2, ARGB4, RGB565 = 0, 1, 2, 3, 4, 5, 6, 7
PALETTED565, PALETTED4444, PALETTED8 = 14, 15, 16
L2 = 17
BPP = {L1: 1, L2: 2, L4: 4, ARGB1555: 16, ARGB4: 16, RGB565: 16}
DIRECT = {  # (сдвиг, разряды) R, G, B, A; A с 0 разрядов — непрозрачно
    ARGB1555: ((10, 5), (5, 5), (0, 5), (15, 1)),
    RGB332: ((5, 3), (2, 3), (0, 2), (0, 0)),
    ARGB2: ((4, 2), (2, 2), (0, 2), (6, 2)),
    ARGB4: ((8, 4), (4, 4), (0, 4), (12, 4)),
    RGB565: ((11, 5), (5, 6), (0, 5), (0, 0)),
}


class Handle:
    __slots__ = ("source", "format", "stride_lo", "stride_hi", "lh_lo", "lh_hi", "filter",
                 "wrap_x", "wrap_y", "w_lo", "w_hi", "h_lo", "h_hi")

    def __init__(self):
        for k in self.__slots__:
            setattr(self, k, 0)

    def width(self):
        w = self.w_lo | (self.w_hi << 9)
        return w or 2048

    def height(self):
        h = self.h_lo | (self.h_hi << 9)
        return h or 2048

    def stride(self):
        return self.stride_lo | (self.stride_hi << 10)

    def layout_height(self):
        return self.lh_lo | (self.lh_hi << 9)


class Context:
    __slots__ = ("clear_rgb", "clear_a", "clear_stencil", "rgb", "a", "alpha_func", "alpha_ref",
                 "st_func", "st_ref", "st_mask", "st_wmask", "st_fail", "st_pass",
                 "blend_src", "blend_dst", "point_size", "line_width",
                 "sc_x", "sc_y", "sc_w", "sc_h", "color_mask", "transform", "vfmt",
                 "handle", "cell", "tr_x", "tr_y", "palette")

    def __init__(self):
        self.clear_rgb = 0
        self.clear_a = 0
        self.clear_stencil = 0
        self.rgb = 0xFFFFFF
        self.a = 255
        self.alpha_func = 7
        self.alpha_ref = 0
        self.st_func, self.st_ref, self.st_mask, self.st_wmask = 7, 0, 255, 255
        self.st_fail, self.st_pass = 1, 1
        self.blend_src, self.blend_dst = 2, 4
        self.point_size = 16
        self.line_width = 16
        self.sc_x, self.sc_y, self.sc_w, self.sc_h = 0, 0, 2048, 2048
        self.color_mask = 0xF
        self.transform = [256, 0, 0, 0, 256, 0]
        self.vfmt = 4
        self.handle = 0
        self.cell = 0
        self.tr_x = self.tr_y = 0
        self.palette = 0

    def copy(self):
        c = Context.__new__(Context)
        for k in self.__slots__:
            setattr(c, k, getattr(self, k))
        c.transform = list(self.transform)
        return c


def compare(func: int, v: int, ref: int) -> bool:
    return (False, v < ref, v <= ref, v > ref, v >= ref, v == ref, v != ref, True)[func]


def stencil_result(op: int, v: int, ref: int) -> int:
    if op == 0: return 0
    if op == 1: return v
    if op == 2: return ref
    if op == 3: return v if v == 255 else v + 1
    if op == 4: return v if v == 0 else v - 1
    if op == 5: return (~v) & 0xFF
    return v


def factor(f: int, sa: int, da: int) -> int:
    return (0, 255, sa, da, 255 - sa, 255 - da)[f] if f < 6 else 0


class Renderer:
    def __init__(self, ram_g: bytes, rom: bytes | None = None, hsize: int = 1024, vsize: int = 768,
                 macro0: int = 0, macro1: int = 0):
        self.ram = ram_g
        self.rom = rom              # образ 0x1E0000…0x2FFFFF или None (нули)
        self.hsize, self.vsize = hsize, vsize
        self.macro = (macro0, macro1)
        self.handles = [Handle() for _ in range(32)]
        for hnd, (fmt, stride, w, h, src) in ROM_FONT_HANDLES.items():   # как ft_rd_walk
            hd = self.handles[hnd]
            hd.source, hd.format, hd.stride_lo, hd.lh_lo, hd.w_lo, hd.h_lo = src, fmt, stride, h, w, h

    # --- память ------------------------------------------------------------------------------
    def byte(self, a: int) -> int:
        a &= 0x3FFFFF
        if a < len(self.ram):
            return self.ram[a]
        if ROM_BASE <= a < ROM_END and self.rom is not None:
            return self.rom[a - ROM_BASE]
        return 0

    def word16(self, a: int) -> int:
        return self.byte(a) | (self.byte(a + 1) << 8)

    # --- кадр --------------------------------------------------------------------------------
    def render(self, dl: list[int]) -> bytes:
        out = bytearray(self.hsize * self.vsize * 3)
        for y in range(self.vsize):
            color, _ = self.line(dl, y)
            for x in range(self.hsize):
                o = 3 * (y * self.hsize + x)
                out[o:o + 3] = bytes(color[4 * x:4 * x + 3])
        return bytes(out)

    def line(self, dl: list[int], y: int):
        self.y = y
        self.color = bytearray(4 * self.hsize)
        self.stencil = bytearray(self.hsize)
        ctx = self.ctx = Context()
        saved: list[Context] = []
        stack: list[int] = []
        prim = 0
        count = 0
        prev = None
        i = 0
        steps = 0
        while i < len(dl) and steps < 8 * 2048:
            w = dl[i]
            steps += 1
            nxt = i + 1
            kind = w >> 30
            if kind in (1, 2):
                if kind == 1:
                    frac = min(ctx.vfmt, 4)
                    vx = sfield(w, 29, 15) << (4 - frac)
                    vy = sfield(w, 14, 0) << (4 - frac)
                    vh, vc = ctx.handle, ctx.cell
                else:
                    vx = field(w, 29, 21) * SUB
                    vy = field(w, 20, 12) * SUB
                    vh, vc = field(w, 11, 7), field(w, 6, 0)
                v = (vx + ctx.tr_x, vy + ctx.tr_y, vh, vc)
                if prim == 1:
                    self.draw_bitmap(v)
                elif prim == 2:
                    self.draw_point(v)
                elif prim == 3 and count & 1:
                    self.draw_line(prev, v)
                elif prim == 4 and count > 0:
                    self.draw_line(prev, v)
                elif prim == 9 and count & 1:
                    self.draw_rect(prev, v)
                prev = v
                count += 1
                i = nxt
                continue
            op = w >> 24
            h = self.handles[ctx.handle]
            if op == 0x00:                                  # DISPLAY
                break
            elif op == 0x01: h.source = field(w, 21, 0)
            elif op == 0x02: ctx.clear_rgb = field(w, 23, 0)
            elif op == 0x04: ctx.rgb = field(w, 23, 0)
            elif op == 0x05: ctx.handle = field(w, 4, 0)
            elif op == 0x06: ctx.cell = field(w, 6, 0)
            elif op == 0x07:
                h.format, h.stride_lo, h.lh_lo = field(w, 23, 19), field(w, 18, 9), field(w, 8, 0)
            elif op == 0x08:
                h.filter, h.wrap_x, h.wrap_y = field(w, 20, 20), field(w, 19, 19), field(w, 18, 18)
                h.w_lo, h.h_lo = field(w, 17, 9), field(w, 8, 0)
            elif op == 0x09: ctx.alpha_func, ctx.alpha_ref = field(w, 10, 8), field(w, 7, 0)
            elif op == 0x0A: ctx.st_func, ctx.st_ref, ctx.st_mask = field(w, 19, 16), field(w, 15, 8), field(w, 7, 0)
            elif op == 0x0B: ctx.blend_src, ctx.blend_dst = field(w, 5, 3), field(w, 2, 0)
            elif op == 0x0C: ctx.st_fail, ctx.st_pass = field(w, 5, 3), field(w, 2, 0)
            elif op == 0x0D: ctx.point_size = field(w, 12, 0)
            elif op == 0x0E: ctx.line_width = field(w, 11, 0)
            elif op == 0x0F: ctx.clear_a = field(w, 7, 0)
            elif op == 0x10: ctx.a = field(w, 7, 0)
            elif op == 0x11: ctx.clear_stencil = field(w, 7, 0)
            elif op == 0x13: ctx.st_wmask = field(w, 7, 0)
            elif 0x15 <= op <= 0x1A:
                k = op - 0x15
                ctx.transform[k] = sfield(w, 23, 0) if k in (2, 5) else sfield(w, 16, 0)
            elif op == 0x1B: ctx.sc_x, ctx.sc_y = field(w, 21, 11), field(w, 10, 0)
            elif op == 0x1C: ctx.sc_w, ctx.sc_h = field(w, 23, 12), field(w, 11, 0)
            elif op == 0x1D:                                # CALL
                if len(stack) == 4:
                    break
                stack.append(i + 1)
                nxt = field(w, 15, 0) & 0x7FF
            elif op == 0x1E: nxt = field(w, 15, 0) & 0x7FF  # JUMP
            elif op == 0x1F: prim, count = field(w, 3, 0), 0
            elif op == 0x20: ctx.color_mask = field(w, 3, 0)
            elif op == 0x21: prim, count = 0, 0
            elif op == 0x22:
                if len(saved) == 4:
                    saved.pop(0)
                saved.append(ctx.copy())
            elif op == 0x23:
                ctx = self.ctx = saved.pop() if saved else Context()
            elif op == 0x24:                                # RETURN
                if not stack:
                    break
                nxt = stack.pop()
            elif op == 0x25:                                # MACRO: слово из REG_MACRO_0/1
                m = self.macro[field(w, 0, 0)]
                if (m >> 24) != 0x25:
                    dl = dl[:i] + [m] + dl[i + 1:]
                    continue
            elif op == 0x26: self.clear(field(w, 2, 0))
            elif op == 0x27: ctx.vfmt = field(w, 2, 0)
            elif op == 0x28: h.stride_hi, h.lh_hi = field(w, 3, 2), field(w, 1, 0)
            elif op == 0x29: h.w_hi, h.h_hi = field(w, 3, 2), field(w, 1, 0)
            elif op == 0x2A: ctx.palette = field(w, 21, 0)
            elif op == 0x2B: ctx.tr_x = sfield(w, 16, 0)
            elif op == 0x2C: ctx.tr_y = sfield(w, 16, 0)
            i = nxt
        return self.color, self.stencil

    # --- конвейер пикселя ------------------------------------------------------------------------
    def scissor(self, first: int, last: int):
        c = self.ctx
        if not (c.sc_y <= self.y < c.sc_y + c.sc_h):
            return None
        first = max(first, c.sc_x, 0)
        last = min(last, c.sc_x + c.sc_w, self.hsize)
        return (first, last) if first < last else None

    def shade(self, x: int, r: int, g: int, b: int, a: int):
        c = self.ctx
        if not compare(c.alpha_func, a, c.alpha_ref):
            return
        s = self.stencil[x]
        ok = compare(c.st_func, s & c.st_mask, c.st_ref & c.st_mask)
        upd = stencil_result(c.st_pass if ok else c.st_fail, s, c.st_ref)
        self.stencil[x] = (s & ~c.st_wmask & 0xFF) | (upd & c.st_wmask)
        if not ok:
            return
        o = 4 * x
        dst = self.color
        da = dst[o + 3]
        sf = factor(c.blend_src, a, da)
        df = factor(c.blend_dst, a, da)
        src = (r, g, b, a)
        for ch, bit in ((0, 8), (1, 4), (2, 2), (3, 1)):
            if c.color_mask & bit:
                dst[o + ch] = blend_channel(src[ch], sf, dst[o + ch], df)

    def clear(self, mask: int):
        span = self.scissor(0, self.hsize)
        if not span:
            return
        c = self.ctx
        vals = ((c.clear_rgb >> 16) & 255, (c.clear_rgb >> 8) & 255, c.clear_rgb & 255, c.clear_a)
        for x in range(*span):
            if mask & 4:
                for ch, bit in ((0, 8), (1, 4), (2, 2), (3, 1)):
                    if c.color_mask & bit:
                        self.color[4 * x + ch] = vals[ch]
            if mask & 2:
                s = self.stencil[x]
                self.stencil[x] = (s & ~c.st_wmask & 0xFF) | (c.clear_stencil & c.st_wmask)

    def shade_fragment(self, x: int, cov: int):
        """Фрагмент цвета контекста; cov — покрытие в 1/256 (0…256), альфа = COLOR_A × cov >> 8."""
        c = self.ctx
        self.shade(x, (c.rgb >> 16) & 255, (c.rgb >> 8) & 255, c.rgb & 255, (c.a * cov) >> 8)

    def shade_covered(self, x: int, cov: int):
        """Фрагмент только при ненулевом покрытии (точки и линии — пока не замерены)."""
        if cov:
            self.shade_fragment(x, cov)

    # --- примитивы -------------------------------------------------------------------------------
    def pixel_range(self, left: float, right: float):
        first = math.ceil((left - PIXEL_CENTER) / SUB)
        last = math.floor((right - PIXEL_CENTER) / SUB) + 1
        return self.scissor(first, last)

    def draw_point(self, v):
        """POINTS. Фрагмент (и с нулевым покрытием) — у пикселей не дальше r + 1 пиксель от
        центра; на осях граница r + 1 пиксель не входит (замер bt8xxemu, ft_probe_corner.py)."""
        r = self.ctx.point_size
        o = r + SUB
        y = self.y * SUB + PIXEL_CENTER
        dy = y - v[1]
        if abs(dy) >= o:
            return
        half = isqrt(o * o - dy * dy)
        span = self.pixel_range(v[0] - half, v[0] + half)
        if not span:
            return
        for x in range(*span):
            ix = x * SUB + PIXEL_CENTER - v[0]
            if dy == 0 and abs(ix) == o:
                continue
            self.shade_fragment(x, point_alpha(r, ix, dy))

    def draw_rect(self, a, b):
        """RECTS. Отрезок строки (замер bt8xxemu, scripts/ft_probe_corner.py): строки — пока
        расстояние до ядра по y меньше r + 8/16; в строках ядра — пиксели ближе r + 8/16 по x;
        в строках угла — пиксели не дальше r + 1 пиксель от угла ядра. Фрагмент получают все
        пиксели отрезка, и с нулевым покрытием (трафарет ставится и там)."""
        r = self.ctx.line_width
        x0, x1 = min(a[0], b[0]), max(a[0], b[0])
        y0, y1 = min(a[1], b[1]), max(a[1], b[1])
        y = self.y * SUB + PIXEL_CENTER
        if r < SUB:
            self.draw_thin_rect(x0, y0, x1, y1, r, y)
            return
        dy = y0 - y if y < y0 else (y - y1 if y > y1 else 0)
        if dy >= r + AA_REACH:
            return
        if dy > 0:
            o = r + SUB
            half = isqrt(o * o - dy * dy)
            span = self.pixel_range(x0 - half, x1 + half)
        else:
            reach = r + AA_REACH
            span = self.pixel_range(x0 - reach + 1, x1 + reach - 1)
        if not span:
            return
        cy = axis_alpha(y, y0, y1, r)
        for x in range(*span):
            px = x * SUB + PIXEL_CENTER
            if (px < x0 or px > x1) and dy > 0:      # угол: по расстоянию до угла ядра
                cov = corner_alpha(r, x0 - px if px < x0 else px - x1, dy)
            else:
                cov = min(axis_alpha(px, x0, x1, r), cy)
            self.shade_fragment(x, cov)

    def draw_thin_rect(self, x0, y0, x1, y1, r, y):
        """RECTS с LINE_WIDTH < 16 (замер bt8xxemu на случайных прямоугольниках, 199 168 точек
        без расхождений): углы не скругляются — фигура [x0 − r, x1 + r] × [y0 − r, y1 + r].
        По оси: перекрытие o полосы пикселя [p − 8, p + 8] с фигурой; фигура целиком в полосе —
        её длина L. Если по какой-то оси фигура целиком в полосе — альфа ⌊A·X·Y/256⌋ (X, Y — L
        или o), иначе ⌊A·ox·oy·r/4096⌋; A — COLOR_A (альфа из точной дроби, без покрытия 1/256)."""
        def axis(p, lo, hi):
            ent = lo >= p - 8 and hi <= p + 8
            return (hi - lo if ent else max(0, min(p + 8, hi) - max(p - 8, lo))), ent
        oy, ey = axis(y, y0 - r, y1 + r)
        if oy == 0:
            return
        span = self.scissor(floordiv(x0 - r - 8, SUB) + 1, floordiv(x1 + r + 8 - 1, SUB) + 1)
        if not span:
            return
        c = self.ctx
        cr, cg, cb = (c.rgb >> 16) & 255, (c.rgb >> 8) & 255, c.rgb & 255
        for x in range(*span):
            ox, ex = axis(x * SUB + PIXEL_CENTER, x0 - r, x1 + r)
            if ex or ey:
                a = c.a * ox * oy // 256
            else:
                a = c.a * ox * oy * r // 4096
            self.shade(x, cr, cg, cb, min(c.a, a))

    def draw_line(self, a, b):
        # отрезок вдоль оси (ненулевой длины) у bt8xxemu — ровно RECTS с теми же вершинами и
        # LINE_WIDTH (замер: случайные отрезки, r 1…40, ни одной расходящейся точки); отрезок
        # нулевой длины рисуется иначе (ни POINTS, ни RECTS) — не замерен, как и наклонные
        if (a[0] == b[0]) != (a[1] == b[1]):
            self.draw_rect(a, b)
            return
        r = self.ctx.line_width
        y = self.y * SUB + PIXEL_CENTER
        span = capsule(y, a[0], a[1], b[0], b[1], r + HALF_RAMP)
        if span is None:
            return
        span = self.pixel_range(*span)
        if not span:
            return
        for x in range(*span):
            d = segment_distance(x * SUB + PIXEL_CENTER, y, a[0], a[1], b[0], b[1])
            self.shade_covered(x, corner_alpha(r, d, 0))    # линии: не замерены, как углы

    def texel(self, h: Handle, base: int, stride: int, tx: int, ty: int):
        fmt = h.format
        row = base + ty * stride
        if fmt in (L1, L2, L4, L8):
            bits = BPP.get(fmt, 8)
            per = 8 // bits
            shift = 8 - bits * (1 + tx % per)
            l = expand_alpha((self.byte(row + tx // per) >> shift) & ((1 << bits) - 1), bits)
            return 255, 255, 255, l
        if fmt in DIRECT:
            p = self.word16(row + 2 * tx) if BPP.get(fmt, 8) == 16 else self.byte(row + tx)
            return self.direct(fmt, p)
        if fmt in (PALETTED565, PALETTED4444, PALETTED8):
            # индекс — байт на точку; запись палитры с PALETTE_SOURCE: RGB565 / ARGB4444 — 2 байта,
            # PALETTED8 — 4 байта ARGB8888 (B, G, R, A в памяти)
            i = self.byte(row + tx)
            pal = self.ctx.palette
            if fmt == PALETTED8:
                a = pal + 4 * i
                return self.byte(a + 2), self.byte(a + 1), self.byte(a), self.byte(a + 3)
            return self.direct(RGB565 if fmt == PALETTED565 else ARGB4, self.word16(pal + 2 * i))
        return 0, 0, 0, 0

    @staticmethod
    def direct(fmt: int, p: int):
        ch = []
        for k, (sh, n) in enumerate(DIRECT[fmt]):
            v = (p >> sh) & ((1 << n) - 1)
            ch.append((expand_alpha(v, n) if k == 3 else expand(v, n)) if n else 255)
        return tuple(ch)

    def wrapped(self, h: Handle, base, stride, lw, lh, tx, ty):
        if lw == 0 or lh == 0:
            return 0, 0, 0, 0
        # REPEAT — маска (ширина − 1), а не остаток: замер bt8xxemu на раскладке 6×5 (ARGB4)
        # даёт тексели 0, 1, 0, 1, 4, 5, 4, 5…; для степени двойки это то же, что остаток
        if h.wrap_x:
            tx &= lw - 1
        elif tx < 0 or tx >= lw:
            return 0, 0, 0, 0
        if h.wrap_y:
            ty &= lh - 1
        elif ty < 0 or ty >= lh:
            return 0, 0, 0, 0
        return self.texel(h, base, stride, tx, ty)

    def draw_bitmap(self, v):
        h = self.handles[v[2]]
        width, height = h.width(), h.height()
        rely = self.y * SUB + PIXEL_CENTER - v[1]
        if rely < 0 or rely >= height * SUB:
            return
        first = floordiv(v[0] - PIXEL_CENTER + SUB - 1, SUB)
        last = floordiv(v[0] + width * SUB - PIXEL_CENTER + SUB - 1, SUB)
        span = self.scissor(first, last)
        if not span:
            return
        first, last = span
        c = self.ctx
        stride = h.stride()
        lh = h.layout_height()
        bpp = BPP.get(h.format, 8)
        lw = stride * 8 // bpp
        base = h.source + v[3] * stride * lh
        A, B, C, D, E, F = c.transform
        cr, cg, cb = (c.rgb >> 16) & 255, (c.rgb >> 8) & 255, c.rgb & 255
        rel = first * SUB + PIXEL_CENTER - v[0]
        sx = ((A * rel + B * rely) >> 4) + C
        sy = ((D * rel + E * rely) >> 4) + F
        for x in range(first, last):
            if not h.filter:
                t = self.wrapped(h, base, stride, lw, lh, sx >> 8, sy >> 8)
            else:
                tx, ty = sx >> 8, sy >> 8
                fx, fy = sx - tx * 256, sy - ty * 256
                w11 = (fx * fy) >> 8
                w = (256 - fx - fy + w11, fx - w11, fy - w11, w11)
                q = (self.wrapped(h, base, stride, lw, lh, tx, ty),
                     self.wrapped(h, base, stride, lw, lh, tx + 1, ty),
                     self.wrapped(h, base, stride, lw, lh, tx, ty + 1),
                     self.wrapped(h, base, stride, lw, lh, tx + 1, ty + 1))
                t = tuple(sum((q[k][ch] * w[k]) >> 8 for k in range(4)) for ch in range(4))
            self.shade(x, mul_tex(t[0], cr), mul_tex(t[1], cg), mul_tex(t[2], cb), mul_texa(t[3], c.a))
            sx += A
            sy += D


def segment_distance(px, py, ax, ay, bx, by) -> int:
    qx, qy, dx, dy = px - ax, py - ay, bx - ax, by - ay
    l2 = dx * dx + dy * dy
    fx = fy = 0
    if l2 > 0:
        dot = max(0, min(l2, qx * dx + qy * dy))
        fx, fy = (dot * dx) // l2, (dot * dy) // l2
    ex, ey = qx - fx, qy - fy
    return isqrt(ex * ex + ey * ey)


def capsule(y, ax, ay, bx, by, r):
    xs = []
    for ex, ey in ((ax, ay), (bx, by)):
        if abs(y - ey) <= r:
            half = math.sqrt(r * r - (y - ey) ** 2)
            xs += [ex - half, ex + half]
    dx, dy = bx - ax, by - ay
    length = math.hypot(dx, dy)
    if length > 0:
        ux, uy = dx / length, dy / length
        nx, ny = -uy, ux
        lo, hi = -math.inf, math.inf
        for coef, const, low, high in ((nx, ny * (y - ay) - nx * ax, -r, r),
                                       (ux, uy * (y - ay) - ux * ax, 0, length)):
            if coef == 0:
                if const < low or const > high:
                    lo = math.inf
                continue
            p, q = (low - const) / coef, (high - const) / coef
            lo, hi = max(lo, min(p, q)), min(hi, max(p, q))
        if lo <= hi:
            xs += [lo, hi]
    return (min(xs), max(xs)) if xs else None


def gradient_ramp() -> bytes:
    """Рампа CMD_GRADIENT по 0x200065 (как в ПЗУ FT81x): clamp(i - 128, 0, 255)."""
    return bytes(min(255, max(0, i - 128)) for i in range(512))


def rom_area_with_ramp() -> bytes:
    """Образ ПЗУ для модели: настоящий (FT812_ROM — файл 0x1E0000…0x2FFFFF, снятый с bt8xxemu:
    данные Bridgetek, не в репозитории) или нули с одной рампой CMD_GRADIENT."""
    path = os.environ.get("FT812_ROM")
    if path and Path(path).is_file():
        return Path(path).read_bytes()
    rom = bytearray(ROM_END - ROM_BASE)
    rom[0x200065 - ROM_BASE:0x200065 - ROM_BASE + 512] = gradient_ramp()
    return bytes(rom)


def main(argv: list[str]) -> int:
    ram = Path(argv[1]).read_bytes()
    dl = list(struct.unpack(f"<{len(Path(argv[2]).read_bytes()) // 4}I", Path(argv[2]).read_bytes()))
    rgb = Renderer(ram, rom_area_with_ramp()).render(dl)
    from PIL import Image
    Image.frombytes("RGB", (1024, 768), rgb).save(argv[3])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
