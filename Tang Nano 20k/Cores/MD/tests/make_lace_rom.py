# SPDX-License-Identifier: GPL-3.0-or-later
# Тестовый ROM чересстрочки двойного разрешения (03.11.2026): VDP — H40, LSM 11 (320 x 448), плоскость A
# вся из плитки 1 (8 x 16), строка r плитки — цвет палитры 1 + r (16 цветов, все разные). В поле 0 VDP
# выводит строки 0, 2, 4… плитки, в поле 1 — 1, 3, 5… (run_lace.py сверяет кадры HDMI построчно).
# Код 68000 собран здесь же — несколько команд, ассемблер не нужен.
# python make_lace_rom.py выход.bin
import struct, sys

CTRL, DATA = 0xC00004, 0xC00000
code = bytearray()


def move_w(imm, addr):          # move.w #imm, (addr).l
    code.extend(struct.pack('>HHI', 0x33FC, imm, addr))


def move_l(imm, addr):          # move.l #imm, (addr).l
    code.extend(struct.pack('>HII', 0x23FC, imm, addr))


def vram_cmd(addr, kind=0x40000000):
    return kind | (addr & 0x3FFF) << 16 | addr >> 14


def fill(cmd, words, value):    # move.l #cmd,CTRL / move.w #n-1,d0 / цикл: move.w #value,DATA / dbra d0
    move_l(cmd, CTRL)
    code.extend(struct.pack('>HH', 0x303C, words - 1))
    loop = len(code)
    move_w(value, DATA)
    code.extend(struct.pack('>Hh', 0x51C8, loop - (len(code) + 2)))


# R G B по 3 бита (0BBB0GGG0RRR0): 16 разных цветов
COLORS = [0x000, 0x00E, 0x0E0, 0xE00, 0x0EE, 0xE0E, 0xEE0, 0xEEE, 0x008, 0x080, 0x800, 0x088, 0x808, 0x880, 0x888, 0x04A]
code.extend(struct.pack('>HH', 0x46FC, 0x2700))                       # move.w #$2700, sr
for reg, val in ((0, 0x04), (1, 0x04), (2, 0x30), (3, 0x3C), (4, 0x07), (5, 0x78), (7, 0x00),
                 (10, 0xFF), (11, 0x00), (12, 0x87), (13, 0x3F), (15, 0x02), (16, 0x01), (17, 0x00), (18, 0x00)):
    move_w(0x8000 | reg << 8 | val, CTRL)
move_l(0xC0000000, CTRL)                                               # CRAM 0
for c in COLORS:
    move_w(c, DATA)
move_l(vram_cmd(0x0040), CTRL)                                         # плитка 1: 16 строк по 4 байта
for r in range(16):
    n = (1 + r) & 15 or 15
    word = n * 0x1111
    move_w(word, DATA)
    move_w(word, DATA)
fill(vram_cmd(0xC000), 64 * 32, 0x0001)                                # плоскость A: плитка 1 везде
fill(vram_cmd(0xE000), 64 * 32, 0x0000)                                # плоскость B: пусто
fill(vram_cmd(0xFC00), 512, 0x0000)                                    # горизонтальный сдвиг
fill(vram_cmd(0x0000, 0x40000010), 40, 0x0000)                         # VSRAM
fill(vram_cmd(0xF000), 320, 0x0000)                                    # спрайты
move_w(0x8144, CTRL)                                                   # экран вкл, режим 5
code.extend(struct.pack('>H', 0x60FE))                                 # bra.s *

rom = bytearray(65536)
struct.pack_into('>II', rom, 0, 0x00FFFE00, 0x200)                     # SSP, PC
rom[0x100:0x110] = b'SEGA GENESIS    '
rom[0x120:0x150] = b'VDAC4 LACE TEST'.ljust(48)
rom[0x150:0x180] = b'VDAC4 LACE TEST'.ljust(48)
rom[0x1F0:0x1F3] = b'JUE'
rom[0x200:0x200 + len(code)] = code
open(sys.argv[1], 'wb').write(rom)
print(f'код {len(code)} байт')
