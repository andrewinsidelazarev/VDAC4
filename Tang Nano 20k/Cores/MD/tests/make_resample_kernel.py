# SPDX-License-Identifier: GPL-3.0-or-later
# Таблица фильтра пересчёта частоты звука MD (MD.ELF 1.7, 09.11.2026): YM 53 098 / 53 336 Гц -> 48 000 Гц.
# sinc с окном Блэкмана, 32 точки, 64 фазы (+1 — для интерполяции между фазами), срез 0,40 частоты входа
# (21,2 кГц при 53 098 Гц; полоса ровная до ~16,7 кГц, подавление от ~25,8 кГц — выше частоты Найквиста
# вывода 24 кГц складывается только неслышимое). Как у TSFM (там 32 точки, срез 19,8 кГц), но фазы
# интерполируются: ошибка времени не 1/128 отсчёта, а на порядки меньше.
#     python make_resample_kernel.py ../esp/main/md_resample_kernel.h
import math, sys
from pathlib import Path

TAPS, PHASES, CUT = 32, 64, 0.40
rows = []
for p in range(PHASES + 1):
    f = p / PHASES
    k = []
    for j in range(TAPS):
        x = f - (j - 15)                       # от входного отсчёта j до точки выхода, в отсчётах входа
        a = 2 * CUT * x
        s = 1.0 if abs(a) < 1e-9 else math.sin(math.pi * a) / (math.pi * a)
        w = (x + 16) / 32
        win = 0.0 if w <= 0 or w >= 1 else 0.42 - 0.5 * math.cos(2 * math.pi * w) + 0.08 * math.cos(4 * math.pi * w)
        k.append(2 * CUT * s * win)
    total = sum(k)
    rows.append([v / total for v in k])

out = ['/* SPDX-License-Identifier: GPL-3.0-or-later */',
       '/* Создано tests/make_resample_kernel.py — не править руками. Фильтр пересчёта частоты (md_resample.h):',
       ' * sinc с окном Блэкмана; точек %d, фаз %d + 1, срез %.2f частоты входа. Строка p — доля p/%d отсчёта. */'
       % (TAPS, PHASES, CUT, PHASES),
       '#define MD_RS_TAPS %d' % TAPS, '#define MD_RS_PHASES %d' % PHASES,
       'static const float md_rs_kernel[MD_RS_PHASES + 1][MD_RS_TAPS] = {']
for r in rows:
    out.append('    {' + ', '.join('%.9ef' % v for v in r) + '},')
out.append('};')
Path(sys.argv[1]).write_bytes(('\r\n'.join(out) + '\r\n').encode('utf-8'))
print('ok', len(rows), 'x', TAPS)
