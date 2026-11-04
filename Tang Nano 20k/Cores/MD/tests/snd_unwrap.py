# SPDX-License-Identifier: GPL-3.0-or-later
# События звука модели с паузами длиннее оборота счётчика (03.11.2026). Время в слове события — 20 бит
# тактов синтеза YM (MCLK/42), оборот — 0,815 с; snd_render разворачивает его по разностям соседних
# событий и паузу длиннее оборота укорачивает. Здесь время каждого события восстанавливается по кадру
# модели (stats.json: накопленный счётчик событий и такт конца кадра), а в длинные паузы вставляются
# пустые слова вида 3 (движок их пропускает) каждые 600 000 тактов YM. На ESP этой беды нет: события
# сверяются с временем ядра (LINK 0x2C) и потребляются через 25 мс.
# python snd_unwrap.py events.bin stats.json выход.bin
import json, struct, sys

ev_path, stats_path, out_path = sys.argv[1:4]
data = open(ev_path, 'rb').read()
words = struct.unpack('<%dI' % (len(data) // 4), data)
frames = json.load(open(stats_path, encoding='utf-8'))['frames']
M = 1 << 20
abs_t, i = [], 0
for f in frames:
    hi = f['cycle'] // 42 + 1                     # событие кадра — не позже его конца
    while i < f['events']:
        s = (words[i] >> 8) & 0xFFFFF
        abs_t.append(hi - ((hi - s) % M))
        i += 1
if len(abs_t) != len(words):
    raise SystemExit(f'событий в stats.json {len(abs_t)}, в файле {len(words)}')
out = []
for k, (w, a) in enumerate(zip(words, abs_t)):
    if k:
        t = abs_t[k - 1] + 600000
        while t < a:
            out.append((3 << 30) | ((t % M) << 8))
            t += 600000
    out.append(w)
open(out_path, 'wb').write(struct.pack('<%dI' % len(out), *out))
print(f'событий {len(words)}, с пустыми {len(out)}, от {abs_t[0] * 42 / 54e6:.3f} до {abs_t[-1] * 42 / 54e6:.3f} с')
