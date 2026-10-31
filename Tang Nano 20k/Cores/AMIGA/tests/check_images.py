"""Проверка новых образов независимо от кода ELF: том AmigaDOS и RDB разбираются заново.

Разбор тома сначала проходит настоящую дискету (Test Kit), записанную не этим кодом: так
проверяется и сам разборщик: расположение полей, контрольные суммы, карта занятости.
"""
from datetime import date
from pathlib import Path
import struct, sys

BLOCK = 512


def longs(image, block, count=128):
    return struct.unpack_from(f'>{count}I', image, block * BLOCK)


def sum_is_zero(words):
    return sum(words) & 0xFFFFFFFF == 0


def read_volume(image, start, blocks):
    """Корень и карта занятости тома; множество свободных блоков."""
    root = (blocks + 1) // 2
    words = longs(image, start + root)
    assert words[0] == 2 and words[127] == 1, 'корень: тип'
    assert words[3] == 72, 'корень: таблица хешей'
    assert sum_is_zero(words), 'корень: сумма'
    assert words[78] == 0xFFFFFFFF, 'карта занятости помечена неверной'
    maps = [w for w in words[79:104] if w]
    ext = words[104]
    exts = []
    while ext:
        exts.append(ext)
        e = longs(image, start + ext)
        maps += [w for w in e[:127] if w]
        ext = e[127]
    free = set()
    for index, m in enumerate(maps):
        word = longs(image, start + m)
        assert sum_is_zero(word), f'блок карты {m}: сумма'
        for k in range(127):
            for bit in range(32):
                if word[k + 1] >> bit & 1:
                    free.add(2 + index * 127 * 32 + k * 32 + bit)
    name = bytes(image[(start + root) * BLOCK + 0x1B1:][:image[(start + root) * BLOCK + 0x1B0]]).decode('latin-1')
    return dict(root=root, maps=maps, exts=exts, free=free, name=name, words=words)


def check_empty_volume(image, start, blocks, dostype, name, days):
    boot = longs(image, start, 256)
    assert boot[0] == dostype, 'загрузочный блок: тип'
    total = 0
    for w in boot:
        total += w
        if total > 0xFFFFFFFF:
            total -= 0xFFFFFFFF
    assert total != 0xFFFFFFFF, 'пустой том не должен быть загрузочным'
    v = read_volume(image, start, blocks)
    assert v['name'] == name, v['name']
    used = {v['root'], *v['maps'], *v['exts']}
    assert v['free'] == set(range(2, blocks)) - used, 'заняты лишние блоки'
    w = v['words']
    for at in (105, 118, 121):
        assert w[at] == days and w[at + 1] == 3 * 60 + 4 and w[at + 2] == 5 * 50, f'дата в слове {at}'
    return v


def check_hdf(image, name, days):
    rdb = longs(image, 0, 64)
    assert image[:4] == b'RDSK' and rdb[1] == 64 and sum_is_zero(rdb), 'RDB'
    assert rdb[4] == BLOCK and rdb[7] == 1 and rdb[6] == 0xFFFFFFFF and rdb[8] == 0xFFFFFFFF
    cylinders, sectors, heads = rdb[16], rdb[17], rdb[18]
    assert cylinders * sectors * heads * BLOCK == len(image), 'геометрия не равна длине файла'
    assert cylinders < 65536 and heads <= 16 and sectors <= 255, 'геометрия вне IDE'
    assert rdb[34] == 2 and rdb[35] == cylinders - 1 and rdb[36] == sectors * heads
    assert rdb[33] == rdb[34] * rdb[36] - 1, 'конец области RDB'
    part = longs(image, 1, 64)
    assert image[BLOCK:BLOCK + 4] == b'PART' and part[1] == 64 and sum_is_zero(part), 'PART'
    assert part[4] == 0xFFFFFFFF and part[5] == 1
    assert image[BLOCK + 36:BLOCK + 40] == b'\x03DH0'
    env = part[32:49]
    assert env[0] == 16 and env[1] == 128 and env[3] == heads and env[4] == 1 and env[5] == sectors
    assert env[6] == 2, 'резерв тома'
    assert env[9] == rdb[34] and env[10] == cylinders - 1, 'цилиндры раздела'
    assert env[16] == 0x444F5301, 'файловая система'
    start = env[9] * heads * sectors
    blocks = (env[10] - env[9] + 1) * heads * sectors
    return check_empty_volume(image, start, blocks, 0x444F5301, name, days)


def main():
    real, adf, hdf = map(Path, sys.argv[1:4])
    disk = real.read_bytes()
    assert len(disk) == 901120
    v = read_volume(disk, 0, 1760)
    assert v['root'] not in v['free'] and v['maps'][0] not in v['free'], 'настоящая дискета: корень свободен'
    # Биты за концом тома у этой дискеты — единицы; у пустой от системы, как у нашей, — нули.
    assert 0 < len({n for n in v['free'] if n < 1760}) < 1758
    days = (date(2026, 10, 31) - date(1978, 1, 1)).days
    image = adf.read_bytes()
    assert len(image) == 901120
    check_empty_volume(image, 0, 1760, 0x444F5300, 'DISK01', days)
    # Известные числа пустой дискеты 880 КиБ: слово с корнем и картой и контрольная сумма карты.
    assert longs(image, 881)[28] == 0xFFFF3FFF
    assert longs(image, 881)[0] == 0xC000C037
    image = hdf.read_bytes()
    check_hdf(image, 'HD01', days)
    print(f'PASS images: parser read real {real.name} ({v["name"]}), blank ADF and {len(image)>>20} MB HDF')


main()
