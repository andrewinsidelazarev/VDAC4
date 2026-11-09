"""Проверка тома EVO (esp/main/evo_vol.h) независимо от кода ESP.

app_test.exe с EVO_FAT обходит том, как FFS, и пишет каждый прочитанный блок диска в дамп: [LBA LE32][512 байтов]. Здесь
дамп читается своим кодом по описанию FFS (ADF FAQ): RDB и PART, корень, карта занятости, заголовки, блоки расширения,
контрольные суммы, ключи, родители, ячейки хеша; имена Amiga считаются здесь же своим переводом и сверяются с FAT-именами
из manifest (make_fat.py), содержимое и размер каждого файла — с sha256 оттуда же, дата — 02.11.2026 12:34:56.

    check_evo.py DUMP MANIFEST.json
"""
from datetime import date
from pathlib import Path
import hashlib, json, struct, sys

CYL, CYLS, HEADS, SECS = 16 * 63, 3901, 16, 63
PART = (CYLS - 1) * CYL
ROOT = (PART - 1 + 2) // 2
MAPS = (PART - 2 + 4063) // 4064
EXTS = (MAPS - 25 + 126) // 127 if MAPS > 25 else 0
LAST = ROOT + MAPS + EXTS
CYR = ['A', 'B', 'V', 'G', 'D', 'E', 'ZH', 'Z', 'I', 'Y', 'K', 'L', 'M', 'N', 'O', 'P',
       'R', 'S', 'T', 'U', 'F', 'KH', 'TS', 'CH', 'SH', 'SCH', '', 'Y', '', 'E', 'YU', 'YA']
STAMP = ((date(2026, 11, 2) - date(1978, 1, 1)).days, 12 * 60 + 34, 56 * 50)
errors = []


def fail(text):
    errors.append(text)


def amiga_char(c):
    o = ord(c)
    if 0x20 <= o < 0x7F and c not in ':/':
        return c
    if o in (0x401, 0x451):
        k, lower = 5, o == 0x451
    elif 0x410 <= o < 0x450:
        k, lower = (o - 0x410) % 32, o >= 0x430
    else:
        return '_'
    t = CYR[k]
    return t.lower() if lower else t[:1] + t[1:].lower()


def fit(raw, suffix):
    tail = f'~{suffix}' if suffix else ''
    dot = raw.rfind('.', 1)
    if dot < 0 or len(raw) - dot > 8:
        dot = len(raw)
    ext = raw[dot:]
    room = 30 - len(tail) - len(ext)
    return raw[:min(dot, room)] + tail + ext


def ffs_hash(name):
    h = len(name)
    for c in name.encode('latin-1'):
        if ord('a') <= c <= ord('z'):
            c -= 32
        h = (h * 13 + c) & 0x7FF
    return h % 72


def expected_names(manifest):
    """FAT-путь -> путь Amiga: перевод, укорочение, повторы «~N» в порядке записей каталога."""
    taken, own = {}, {}
    for path in manifest:                    # порядок соседей — порядок записей каталога (каталог идёт после своих детей)
        parent, _, name = path.rpartition('/')
        name = manifest[path].get('name', name)          # каталог-архив: ключ «файл.lha#», имя без расширения
        raw = ''.join(amiga_char(c) for c in name)[:64]
        names = taken.setdefault(parent, set())
        for suffix in range(1000):
            cand = fit(raw, suffix)
            if cand.upper() not in names:
                break
        names.add(cand.upper())
        own[path] = cand
    full = lambda p: (full(p.rpartition('/')[0]) + '/' if '/' in p else '') + own[p]
    return {p: full(p) for p in manifest}


def main(dump_path, manifest_path):
    data = Path(dump_path).read_bytes()
    blocks = {}
    for at in range(0, len(data), 516):
        lba = struct.unpack_from('<I', data, at)[0]
        blk = data[at + 4:at + 516]
        if lba in blocks and blocks[lba] != blk:
            fail(f'блок {lba} прочитан дважды по-разному')
        blocks[lba] = blk
    manifest = json.loads(Path(manifest_path).read_text(encoding='utf-8'))

    def blk(p):
        lba = CYL + p
        if lba not in blocks:
            fail(f'блока {p} раздела нет в дампе')
            return bytes(512)
        return blocks[lba]

    L = lambda b, i: struct.unpack_from('>I', b, i * 4)[0]
    S = lambda b, n=128: sum(struct.unpack_from(f'>{n}I', b)) & 0xFFFFFFFF

    rdb, part = blocks.get(0, bytes(512)), blocks.get(1, bytes(512))
    if rdb[:4] != b'RDSK' or S(rdb, 64) or (L(rdb, 16), L(rdb, 17), L(rdb, 18)) != (CYLS, SECS, HEADS) or L(rdb, 7) != 1:
        fail('RDB')
    if L(rdb, 5) & 1:
        fail('RDB: флаг «последний диск»')
    if part[:4] != b'PART' or S(part, 64) or L(part, 5) & 1 or part[36:40] != b'\x03EVO':
        fail('PART: подпись, сумма, флаг загрузки или имя')
    if (L(part, 35), L(part, 37), L(part, 38), L(part, 41), L(part, 42), L(part, 48)) != (HEADS, SECS, 2, 1, CYLS - 1, 0x444F5301):
        fail('PART: геометрия или DOS\\1')
    if blk(0)[:4] != b'DOS\x01':
        fail('загрузочный блок')
    root = blk(ROOT)
    if S(root) or L(root, 0) != 2 or L(root, 127) != 1 or L(root, 3) != 72 or L(root, 78) != 0xFFFFFFFF:
        fail('корень: сумма, тип или карта занятости')
    if root[0x1B0] != 3 or root[0x1B1:0x1B4] != b'EVO':
        fail('имя тома')
    maps = [L(root, 79 + i) for i in range(25) if L(root, 79 + i)]
    ext = L(root, 104)
    while ext:
        e = blk(ext)
        maps += [L(e, i) for i in range(127) if L(e, i)]
        ext = L(e, 127)
    if maps != list(range(ROOT + 1, ROOT + 1 + MAPS)):
        fail('блоки карты занятости')
    if any(blk(m) != bytes(512) for m in maps):
        fail('карта занятости: не все блоки заняты')

    used = {}
    def own(b, what):
        if b < 2 or b >= PART or ROOT <= b <= LAST:
            fail(f'{what}: блок {b} вне места для выдачи')
        if b in used:
            fail(f'{what}: блок {b} уже занят под {used[b]}')
        used[b] = what

    found, stamps = {}, {}
    def walk(key, header, path):
        for h in range(72):
            x = L(header, 6 + h)
            while x:
                e = blk(x)
                name = e[0x1B1:0x1B1 + e[0x1B0]].decode('latin-1')
                full = path + '/' + name if path else name
                own(x, full)
                if S(e) or L(e, 0) != 2 or L(e, 1) != x or L(e, 125) != key or ffs_hash(name) != h or not 0 < len(name) <= 30:
                    fail(f'{full}: заголовок')
                if L(e, 80) != 5:
                    fail(f'{full}: защита {L(e, 80)}')
                stamps[full] = [L(e, 105), L(e, 106), L(e, 107)]
                if L(e, 127) == 2:
                    found[full] = None
                    walk(x, e, full)
                elif L(e, 127) == 0xFFFFFFFD:
                    size, content, lst, seq, parts = L(e, 81), bytearray(), e, 0, []
                    if L(e, 2) and L(e, 4) != L(e, 77):
                        fail(f'{full}: first_data')
                    while True:
                        n = L(lst, 2)
                        if n > 72:
                            fail(f'{full}: high_seq {n}')
                            break
                        for i in range(n):
                            d = L(lst, 77 - i)
                            own(d, f'{full} данные {seq}')
                            content += blk(d)
                            seq += 1
                        nxt = L(lst, 126)
                        if not nxt:
                            break
                        own(nxt, f'{full} расширение')
                        lst = blk(nxt)
                        if S(lst) or L(lst, 0) != 16 or L(lst, 1) != nxt or L(lst, 125) != x or L(lst, 127) != 0xFFFFFFFD:
                            fail(f'{full}: блок расширения')
                    if seq != (size + 511) // 512:
                        fail(f'{full}: блоков данных {seq}, надо {(size + 511) // 512}')
                    found[full] = bytes(content[:size])
                else:
                    fail(f'{full}: тип {L(e, 127)}')
                x = L(e, 124)

    walk(ROOT, root, '')
    want = expected_names(manifest)
    for path, amiga in want.items():
        info = manifest[path]
        if amiga not in found:
            fail(f'нет {amiga} (FAT {path})')
            continue
        if stamps[amiga] != list(info.get('stamp', STAMP)):
            fail(f'{amiga}: дата {stamps[amiga]}, надо {list(info.get("stamp", STAMP))}')
        if info.get('dir'):
            if found[amiga] is not None:
                fail(f'{amiga}: не каталог')
        elif found[amiga] is None or len(found[amiga]) != info['size'] or (
                info['sha256'] is not None and hashlib.sha256(found[amiga]).hexdigest() != info['sha256']):
            fail(f'{amiga}: содержимое не то')       # sha256 нет — член -lh1-: его сверяет CRC-16 в app_test
    extra = set(found) - set(want.values())
    if extra:
        fail(f'лишние: {sorted(extra)}')
    for line in errors[:40]:
        print('  ' + line)
    files = sum(1 for v in found.values() if v is not None)
    print(f'{Path(dump_path).name}: {files} файлов, {len(found) - files} каталогов, блоков {len(blocks)}; '
          + ('FAIL' if errors else 'PASS'))
    for p in ('Длинное имя файла по-русски.txt', 'Games/Игра.adf', 'Games/Igra.adf', 'ФАЙЛ.TXT', 'readme.txt',
              'A very long file name that is longer than thirty two.txt', 'Games/Sub/Щука и Ёж.txt',
              'Archives/stored2.lha#', 'Archives/stored2', 'Archives/stored0.lha#/C/Tool'):
        if p in want:
            print(f'  {p} -> {want[p]}')
    return 1 if errors else 0


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    sys.exit(main(sys.argv[1], sys.argv[2]))
