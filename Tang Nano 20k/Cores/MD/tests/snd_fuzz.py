"""Случайные записи в YM2612 и PSG для сверки синтеза звука MD (09.11.2026): старый движок и новый рендерят один и
тот же файл событий (tests/snd_render.cpp), WAV обязаны совпасть байт в байт.
Слова — как md_snd_events: [31:30] вид (0 — порт YM, 1 — PSG, 2 — сброс YM), [29:28] порт, [27:8] время в тактах
синтеза YM (MCLK/42, 24 такта на отсчёт), [7:0] байт.
    python snd_fuzz.py ЗЕРНО ФАЙЛ СЕКУНД   (зёрна 1, 2, 3 по 60 с — сверка MD.ELF 1.6)
Охват: все регистры операторов и каналов обеих частей (и «пустой» канал 3), SSG-EG, LFO (0x22), режим канала 3 и
CSM (0x27), частоты канала 3 (A8–AE), нажатия с частью операторов, ЦАП (2A/2B/2C) пачками, сброс YM, PSG; паузы
от нуля тактов (несколько записей в один отсчёт) до 300 000 тактов (больше 4096 отсчётов — периодический prepare)."""
import random, struct, sys

seed, path, seconds = int(sys.argv[1]), sys.argv[2], float(sys.argv[3])
rnd = random.Random(seed)
TICKS = int(seconds * 54e6 / 42)
ev = []
t = 0

def put(kind, port, byte):
    ev.append((kind << 30) | (port << 28) | ((t & 0xFFFFF) << 8) | (byte & 0xFF))

def ym(part, reg, val):
    put(0, 2 * part, reg)
    put(0, 2 * part + 1, val)

def gap(lo, hi):
    global t
    t += rnd.randint(lo, hi)

def voice(ch):
    part, c = ch // 3, ch % 3
    for op in range(4):
        o = c + 4 * op
        carrier = op == 3 or rnd.random() < 0.3
        ym(part, 0x30 + o, rnd.randint(0, 0x7F))                                   # DT, MUL
        ym(part, 0x40 + o, rnd.randint(0, 0x30) if carrier else rnd.randint(0, 0x7F))  # TL
        ym(part, 0x50 + o, rnd.choice([0x1F, 0x1F, 0x5F, 0xDF, rnd.randint(0, 0xFF)]))  # KS, AR
        ym(part, 0x60 + o, rnd.randint(0, 0xFF))                                   # AM, D1R
        ym(part, 0x70 + o, rnd.randint(0, 0x1F))                                   # D2R
        ym(part, 0x80 + o, rnd.randint(0, 0xFF))                                   # SL, RR
        ym(part, 0x90 + o, rnd.choice([0, 0, 0, rnd.randint(8, 15), rnd.randint(0, 15)]))  # SSG-EG
    ym(part, 0xB0 + c, rnd.randint(0, 0x3F))                                       # FB, ALG
    ym(part, 0xB4 + c, rnd.choice([0xC0, 0xC0, 0x80, 0x40, 0x00]) | rnd.randint(0, 0x37))  # L R AMS PMS

def freq(ch):
    part, c = ch // 3, ch % 3
    ym(part, 0xA4 + c, rnd.randint(0, 0x3F))
    ym(part, 0xA0 + c, rnd.randint(0, 0xFF))

def key(ch, mask):
    ym(0, 0x28, (mask << 4) | (ch % 3) | (4 if ch >= 3 else 0))

for ch in range(6):
    voice(ch); freq(ch)
dac_on = False
while t < TICKS:
    r = rnd.random()
    if r < 0.30:                                       # нота: частота, нажатие
        ch = rnd.randrange(6)
        freq(ch)
        key(ch, rnd.choice([0xF, 0xF, 0xF, rnd.randint(0, 15)]))
        gap(0, 3000)
    elif r < 0.45:                                     # отпускание
        key(rnd.randrange(6), rnd.choice([0, 0, rnd.randint(0, 15)]))
        gap(0, 3000)
    elif r < 0.55:                                     # новый голос
        voice(rnd.randrange(6))
        gap(0, 500)
    elif r < 0.68:                                     # драйвер: TL/частота по ходу ноты
        ch = rnd.randrange(6); part, c = ch // 3, ch % 3
        ym(part, 0x40 + c + 4 * rnd.randrange(4), rnd.randint(0, 0x7F))
        if rnd.random() < 0.5: freq(ch)
        gap(0, 400)
    elif r < 0.72:                                     # LFO
        ym(0, 0x22, rnd.choice([0, 8 | rnd.randint(0, 7), rnd.randint(0, 15)]))
        gap(0, 2000)
    elif r < 0.75:                                     # режим канала 3 / CSM и таймеры; частоты канала 3
        ym(0, 0x27, rnd.choice([0x00, 0x40, 0x80, 0xC0, 0x15, 0x55, rnd.randint(0, 255)]))
        for reg in rnd.sample([0xA8, 0xA9, 0xAA, 0xAC, 0xAD, 0xAE], rnd.randint(1, 6)):
            ym(0, reg, rnd.randint(0, 0xFF))
        gap(0, 2000)
    elif r < 0.80:                                     # ЦАП: включить/выключить и пачка отсчётов ~16 кГц
        dac_on = rnd.random() < 0.7
        ym(0, 0x2B, 0x80 if dac_on else 0x00)
        n = rnd.randint(10, 600)
        v = rnd.randint(0, 255)
        for i in range(n):
            ym(0, 0x2A, v)
            if rnd.random() < 0.3: ym(0, 0x27, rnd.choice([0x15, 0x15, 0x55]))  # сброс флага таймера, как драйверы
            if rnd.random() < 0.02: ym(0, 0x2C, rnd.choice([0, 8]))
            v = (v + rnd.randint(-40, 40)) & 0xFF
            gap(60, 100)
    elif r < 0.83:                                     # «пустые» адреса: канал 3 частей, B8–BF, A3, неизвестные
        ym(rnd.randrange(2), rnd.choice([0x33, 0x43, 0x9F, 0xA3, 0xA7, 0xB3, 0xB7, 0xB8, 0xBC, 0x21, 0x29, 0x2D, 0x24, 0x25, 0x26]),
           rnd.randint(0, 255))
        gap(0, 500)
    elif r < 0.90:                                     # PSG
        put(1, 0, rnd.choice([0x80 | rnd.randint(0, 0x7F), rnd.randint(0, 0x3F)]))
        gap(0, 500)
    elif r < 0.905:                                    # сброс YM
        put(2, 0, rnd.randint(0, 1))
        gap(0, 2000)
        for ch in range(6):
            voice(ch); freq(ch)
    elif r < 0.93:                                     # долгая пауза: больше 4096 отсчётов
        gap(20000, 300000)
    else:
        gap(0, 6000)

with open(path, 'wb') as f:
    f.write(struct.pack('<%dI' % len(ev), *ev))
print('событий', len(ev), 'тактов', t)
