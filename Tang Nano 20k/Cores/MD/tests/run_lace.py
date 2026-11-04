# SPDX-License-Identifier: GPL-3.0-or-later
# Чересстрочка двойного разрешения на выходе HDMI (03.11.2026): тестовый ROM (make_lace_rom.py) в модели
# с md_video, кадры HDMI 20 и 21 сверяются построчно. Каждый должен целиком совпасть ровно с одним полем
# (строки плитки 2i+поле на месте, которое даёт масштаб 448 -> 720 по Брезенхэму, как в md_video 0.7+), поля
# соседних кадров — разные. Номер снимка VDP с номером кадра HDMI не сравнивается: они сдвинуты на поле.
# python run_lace.py [--no-build]
import subprocess, sys
from pathlib import Path
from PIL import Image

sys.stdout.reconfigure(encoding='utf-8', errors='replace')
HERE = Path(__file__).resolve().parent
STAGE = Path('E:/zx/cache/tmp/mdtang/lace')
STAGE.mkdir(parents=True, exist_ok=True)
rom = STAGE / 'lace.bin'
subprocess.run([sys.executable, '-I', str(HERE / 'make_lace_rom.py'), str(rom)], check=True)
subprocess.run([sys.executable, '-I', str(HERE / 'run_model.py'), str(rom), '--frames', '24', '--shots', '20,21',
                '--video', *sys.argv[1:]], check=True, stdout=subprocess.DEVNULL)
R = Path('E:/zx/cache/tmp/mdtang/model-video/run')
COLORS = [0x000, 0x00E, 0x0E0, 0xE00, 0x0EE, 0xE0E, 0xEE0, 0xEEE, 0x008, 0x080, 0x800, 0x088, 0x808, 0x880, 0x888, 0x04A]


def rgb(c):                     # CRAM 0BBB0GGG0RRR0 -> 4 бита ядра -> 8 бит md_video ({x, x})
    return tuple(((c >> s) & 7) * 2 * 17 for s in (1, 5, 9))


pal = {rgb(c): i for i, c in enumerate(COLORS)}


def rows_of(px):                # строки плитки с этим цветом (r -> цвет 1 + r, 15 — у строк 14 и 15)
    n = pal.get(px[:3])
    return [] if n is None else [r for r in range(16) if ((1 + r) & 15 or 15) == n]


def mismatches(img, f):
    Y0, H, SH = 0, 720, 448
    sy = vacc = bad = 0
    for vy in range(Y0, Y0 + H):
        rl = 0 if sy < f else (sy - f) >> 1
        if (2 * rl + f) % 16 not in rows_of(img.getpixel((640, vy))): bad += 1
        if vacc + SH >= H: vacc += SH - H; sy += 1
        else: vacc += SH
    return bad


fields = []
for n in (20, 21):
    img = Image.open(R / f'hdmi_{n:04d}.png').convert('RGB')
    bad = [mismatches(img, f) for f in (0, 1)]
    match = [f for f in (0, 1) if bad[f] == 0]
    print(f'кадр HDMI {n}: несовпадений с полем 0 — {bad[0]}, с полем 1 — {bad[1]}')
    fields.append(match[0] if len(match) == 1 else None)
ok = None not in fields and fields[0] != fields[1]
print('ВСЁ ВЕРНО' if ok else 'ОШИБКА')
sys.exit(0 if ok else 1)
