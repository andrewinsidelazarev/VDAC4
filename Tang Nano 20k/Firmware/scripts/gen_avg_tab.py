"""Таблица смешения двух кадров Evo для FRAME BLEND (fb_core, AVG) → rtl/fb_avg_tab.svh.

Запуск из fpga/vdac2:  python scripts/gen_avg_tab.py

Вход — канал точки в двух кадрах, каждый {PAL_SEL, значение 5 бит} (как в кадровом буфере:
точка {PAL_SEL, B5, G5, R5}). Цвет канала на выходе — как у legacy_palette:
    PAL_SEL = 1: v × 8;  PAL_SEL = 0: v ≥ 24 → 255, иначе round(v × 255 / 24).
Среднее берётся по яркости, как глаз усредняет два кадра на 50 Гц: L = (c / 255) ^ G,
c_avg = 255 × ((L1 + L2) / 2) ^ (1 / G), G = 2,2 (гамма телевизора). Простое среднее кодов
делало бы мерцание чёрного с белым серым 128 — заметно темнее, чем на ЭЛТ (~186).

Выход — 5 бит q для точки с PAL_SEL = 1: на выводе q × 8 (ближайшее к c_avg, не больше 31).
Точки, одинаковые в обоих кадрах, fb_core не смешивает — для них таблица не нужна.
"""
import sys
from pathlib import Path

GAMMA = 2.2
HERE = Path(__file__).resolve().parents[1]
OUT = HERE / 'rtl' / 'fb_avg_tab.svh'


def level(pal, v):
    if pal:
        return v * 8
    return 255 if v >= 24 else (v * 255 + 12) // 24


def lin(c):
    return (c / 255.0) ** GAMMA


def avg_q(i):
    a, b = i >> 6, i & 63
    la = lin(level(a >> 5, a & 31))
    lb = lin(level(b >> 5, b & 31))
    c = 255.0 * ((la + lb) / 2.0) ** (1.0 / GAMMA)
    return min(31, int(c / 8.0 + 0.5))


def main():
    tab = [avg_q(i) for i in range(4096)]
    # Самопроверка: симметрия, чёрный с белым, совпадающие входы с PAL_SEL = 1
    assert all(tab[(a << 6) | b] == tab[(b << 6) | a] for a in range(64) for b in range(64))
    black, white0, white1 = 0, 24, 32 | 31
    assert tab[(black << 6) | white0] == 23, tab[(black << 6) | white0]   # 255 и 0 → 186 → 23 × 8 = 184
    assert all(tab[((32 | v) << 6) | (32 | v)] == v for v in range(32))
    lines = [
        '// SPDX-License-Identifier: Apache-2.0',
        '//',
        '// Таблица смешения двух кадров (FRAME BLEND, fb_core). СГЕНЕРИРОВАНО scripts/gen_avg_tab.py —',
        '// не править руками. i = {канал кадра 1, канал кадра 2}, канал — {PAL_SEL, значение 5 бит};',
        f'// результат — значение для PAL_SEL = 1 (вывод q × 8): среднее по яркости, гамма {GAMMA}.',
        '//',
        'function automatic [4:0] fb_avg_q(input [11:0] i);',
        '    case (i)',
    ]
    for i, q in enumerate(tab):
        lines.append(f"        12'h{i:03X}: fb_avg_q = 5'd{q};")
    lines += ['        default: fb_avg_q = 5\'d0;', '    endcase', 'endfunction', '']
    OUT.write_bytes('\n'.join(lines).encode('utf-8'))
    print(f'{OUT}: 4096 значений, чёрный+белый = {tab[(black << 6) | white0] * 8}, '
          f'белый(PAL 1)+чёрный = {tab[(white1 << 6) | black] * 8}')


if __name__ == '__main__':
    sys.exit(main())
