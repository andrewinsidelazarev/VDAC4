"""Образ карты Evo для проверки тома EVO (AMIGA.ELF 0.18, esp/main/evo_vol.h): FAT16 или FAT32 за MBR, как карта WC.

В образе: длинные имена (UTF-16, кириллица), короткие только в CP866 и строчные по флагам NT, удалённая запись, метка тома,
каталог на несколько кластеров, вложенные каталоги, файл больше 72 блоков (блоки расширения FFS), файл вразбивку (кластеры
через один с другим файлом), пустой файл, имена, совпадающие после перевода латиницей и после укорочения до 30 знаков.
Рядом — manifest.json: путь на FAT -> sha256 и размер. Имена Amiga ожидаемые здесь не считаются: их независимо считает
check_evo.py.

    make_fat.py OUT.img [--fat16] [--tree boot DIR]   # DIR: файлы как есть (S/startup-sequence, C/Type) — загрузочный том
"""
from pathlib import Path
import argparse, hashlib, json, struct

PART_LBA = 2048


def lfn_checksum(sfn):
    s = 0
    for b in sfn:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s


def pattern(seed, n):
    return bytes((i * 131 + seed * 17 + (i >> 9) * 7) & 0xFF for i in range(n))


# ---- архивы LHA (каталоги-архивы тома EVO, evo_lha.h) ----
SEVEN_ZIP = Path(r'C:\Program Files\7-Zip\7z.exe')


def crc16(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ (0xA001 if c & 1 else 0)
    return c


def lha_ext(m):
    """Расширенные заголовки члена: [тип][данные][размер следующего]; и размер первого."""
    parts = []
    if 'ext_name' in m:
        parts.append(b'\x01' + m['ext_name'])
    if 'dir' in m:
        parts.append(b'\x02' + m['dir'])
    ext = b''
    for k, body in enumerate(parts):
        ext += body + struct.pack('<H', len(parts[k + 1]) + 2 if k + 1 < len(parts) else 0)
    return ext, (len(parts[0]) + 2 if parts else 0)


def lha_stored(members, level):
    """Архив без сжатия (-lh0-; каталог -lhd-; неизвестный метод — как есть). Член: name (уровень 0/1: байты, можно с
    путём и с комментарием Amiga после нуля), ext_name и dir (расширенные заголовки 1 и 2, части каталога через 0xFF),
    method, data, date и time (DOS, уровни 0/1) или unix (уровень 2)."""
    out = b''
    for m in members:
        data, method = m.get('data', b''), m.get('method', b'-lh0-')
        crc = crc16(data)
        ext, first = lha_ext(m) if level else (b'', 0)
        if level < 2:
            name = m.get('name', b'')
            base = (method + struct.pack('<IIHH', len(data) + len(ext), len(data), m['time'], m['date'])
                    + bytes([0x20, level, len(name)]) + name + struct.pack('<H', crc))
            if level == 1:
                base += b'A' + struct.pack('<H', first)
            out += bytes([len(base), sum(base) & 0xFF]) + base + ext + data
        else:
            total = 26 + len(ext)
            hdr = (struct.pack('<H', total) + method + struct.pack('<III', len(data), len(data), m['unix'])
                   + bytes([0x20, 2]) + struct.pack('<H', crc) + b'A' + struct.pack('<H', first) + ext)
            assert len(hdr) == total and total & 0xFF
            out += hdr + data
    return out + b'\0'


def lha_norm(path):
    out = []
    for part in path.replace(b'\\', b'/').replace(b'\xff', b'/').split(b'/'):
        if part and part != b'.':
            out.append(part)
    return b'/'.join(out)


def fat_stamp(date, time):
    """Дата и время FAT -> дни с 1.1.1978, минуты, тики (как ev_stamp)."""
    year, month, day = 1980 + (date >> 9), (date >> 5) & 15, date & 31
    if not 1 <= month <= 12 or day < 1:
        return [0, 0, 0]
    before = [0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334]
    days = (year - 1978) * 365 + before[month - 1] + day - 1 + len(range(1980, year, 4)) + int(year % 4 == 0 and month > 2)
    return [days, (time >> 11) * 60 + ((time >> 5) & 63), (time & 31) * 2 * 50]


def unix_fat(t):
    import datetime
    d = datetime.datetime.fromtimestamp(t, datetime.timezone.utc)
    if d.year < 1980:
        return (1 << 5) | 1, 0
    return ((min(d.year, 2107) - 1980) << 9) | (d.month << 5) | d.day, (d.hour << 11) | (d.minute << 5) | (d.second // 2)


def lha_members(data):
    """Свой разбор заголовков уровней 0–2 — независимо от evo_lha.h."""
    out, at = [], 0
    le16 = lambda o: struct.unpack_from('<H', data, o)[0]
    while at + 22 <= len(data) and data[at]:
        level, method = data[at + 20], data[at + 2:at + 7]
        packed, size = struct.unpack_from('<II', data, at + 7)
        name = dirn = b''
        if level < 2:
            hlen, nl = data[at] + 2, data[at + 21]
            name = data[at + 22:at + 22 + nl].split(b'\0')[0]
            crc = le16(at + 22 + nl)
            date, time = le16(at + 17), le16(at + 15)
            pos = at + hlen
            if level == 1:
                nxt = le16(at + hlen - 2)
                while nxt:
                    ext = data[pos:pos + nxt]
                    if ext[0] == 1:
                        name = ext[1:nxt - 2]
                    elif ext[0] == 2:
                        dirn = ext[1:nxt - 2]
                    packed -= nxt
                    pos += nxt
                    nxt = struct.unpack_from('<H', ext, nxt - 2)[0]
        else:
            hlen = le16(at)
            crc = le16(at + 21)
            date, time = unix_fat(struct.unpack_from('<I', data, at + 15)[0])
            off = 24
            while off + 2 <= hlen:
                n = le16(at + off)
                if not n:
                    break
                body = data[at + off + 3:at + off + n]
                if data[at + off + 2] == 1:
                    name = body
                elif data[at + off + 2] == 2:
                    dirn = body
                off += n
            pos = at + hlen
        body = data[pos:pos + packed]
        at = pos + packed
        path = lha_norm(dirn + (b'/' if dirn else b'') + name)
        kind = method[1:4]
        if not path or kind not in (b'lh0', b'lz4', b'lh1', b'lh4', b'lh5', b'lh6', b'lh7', b'lhd'):
            continue
        out.append(dict(path=path.decode('latin-1'), kind=kind, body=body, crc=crc, stamp=fat_stamp(date, time),
                        size=min(size, packed) if kind in (b'lh0', b'lz4') else size))
    return out


def lha_manifest(archive, base, manifest):
    """Записи каталога-архива: папки — на первом их члене, файлы — с sha256 (сжатое — через 7-Zip; -lh1- 7-Zip не
    умеет: тогда только размер, а содержимое проверяет CRC-16 в app_test)."""
    import subprocess, tempfile, contextlib, uuid
    members = lha_members(archive)
    unpacked = {}
    if any(m['kind'] not in (b'lh0', b'lz4', b'lhd') for m in members) and SEVEN_ZIP.exists():
        # Windows' mode-0700 tempfile ACL excludes a restricted test token.
        # Inherit the normal workspace ACL for these small reference fixtures.
        scratch=Path(tempfile.gettempdir())/('amiga-lha-'+uuid.uuid4().hex)
        scratch.mkdir()
        with contextlib.nullcontext(str(scratch)) as tmp:
            src = Path(tmp) / 'a.lha'
            src.write_bytes(archive)
            subprocess.run([str(SEVEN_ZIP), 'x', '-y', '-o' + str(Path(tmp) / 'x'), str(src)], capture_output=True)
            for f in (Path(tmp) / 'x').rglob('*'):
                if f.is_file() and f.stat().st_size:
                    unpacked[f.relative_to(Path(tmp) / 'x').as_posix()] = f.read_bytes()
    seen = set()
    for m in members:
        parts = m['path'].split('/')
        is_dir = m['kind'] == b'lhd'
        dirs = parts if is_dir else parts[:-1]
        for k in range(1, len(dirs) + 1):
            d = '/'.join(dirs[:k])
            if d not in seen:
                seen.add(d)
                manifest[f'{base}/{d}'] = {'dir': True, 'stamp': m['stamp']}
        if is_dir:
            continue
        body = m['body'][:m['size']] if m['kind'] in (b'lh0', b'lz4') else unpacked.get(m['path'])
        if body is not None:
            assert len(body) == m['size'] and crc16(body) == m['crc'], m['path']
        manifest[f'{base}/{m["path"]}'] = {'size': m['size'], 'stamp': m['stamp'],
                                           'sha256': hashlib.sha256(body).hexdigest() if body is not None else None}


def test_archives(samples=None):
    """Каталог Archives: архивы без сжатия уровней 0, 1, 2 и (если дан каталог) настоящие архивы со сжатием."""
    t = lambda y, mo, d, h, mi, s: (((y - 1980) << 9) | (mo << 5) | d, (h << 11) | (mi << 5) | (s // 2))
    d0, t0 = t(1995, 6, 15, 10, 20, 30)
    d1, t1 = t(2003, 2, 28, 23, 59, 58)
    a0 = lha_stored([dict(name=b'readme.txt', data=b'level 0 readme\n', date=d0, time=t0),
                     dict(name=b'docs\\manual.txt', data=b'MS-DOS separator\n', date=d1, time=t1),
                     dict(name=b'C/Tool\0an Amiga comment', data=pattern(7, 3000), date=d0, time=t0)], 0)
    a1 = lha_stored([dict(name=b'lib.txt', dir=b'Libs\xffSub\xff', data=b'level 1 with path\n', date=d1, time=t1),
                     dict(name=b'x', ext_name=b'Long name with spaces.txt', data=b'name from the extended header\n',
                          date=d0, time=t0)], 1)
    a2 = lha_stored([dict(method=b'-lhd-', dir=b'Empty\xff', unix=1000000000),
                     dict(dir=b'a\xffb\xff', ext_name=b'c.txt', data=b'level 2, nested\n', unix=1000000000),
                     dict(ext_name=b'big.dat', data=pattern(9, 150000), unix=1234567890),
                     dict(method=b'-lzs-', ext_name=b'hidden.txt', data=b'unsupported method\n', unix=1234567890),
                     dict(dir=b'.\xffdot\xff', ext_name=b'x.txt', data=b'dot part dropped\n', unix=1700000000)], 2)
    tree = {'stored0.lha': a0, 'Stored1.LZH': a1, 'stored2.lha': a2,
            'stored2': {'real folder.txt': b'a real folder named like the archive\n'}}
    if samples:
        for f in sorted(Path(samples).iterdir()):
            if f.is_file() and f.stat().st_size < 1 << 20:
                tree[f.name if f.suffix.lower() in ('.lha', '.lzh') else f.name + '.lha'] = f.read_bytes()
    return tree


def default_tree(samples=None):
    """Узел: dict — каталог, bytes — файл. Ключ str — длинное имя; ('sfn', b'11 байт', флаги NT) — только короткое имя."""
    games = {f'Game number {i:02d} with a long name.adf': pattern(100 + i, 300 + i) for i in range(40)}
    games['Игра.adf'] = b'cyrillic name\n'
    games['Igra.adf'] = b'latin name, same as the cyrillic after transliteration\n'
    games['Sub'] = {'Deep': {'file.txt': b'deep file\n'}, 'Щука и Ёж.txt': b'Shchuka\n'}
    return {
        'hello.txt': b'EVO VOLUME OK\n',
        ('sfn', b'README  TXT', 0x18): b'lower case by NT flags\n',
        ('sfn', bytes([0x94, 0x80, 0x89, 0x8B]) + b'    TXT', 0): b'cp866 short name\n',
        'Hello World.txt': b'Hello from the Evo card\n',
        'Длинное имя файла по-русски.txt': b'long russian name\n',
        'A very long file name that is longer than thirty.txt': b'first long\n',
        'A very long file name that is longer than thirty two.txt': b'second long, same 30 chars\n',
        'big.bin': pattern(1, 100 * 1024 + 77),
        'frag.bin': pattern(2, 20 * 1024 + 5),
        'frag2.bin': pattern(3, 20 * 1024 + 9),
        'empty.dat': b'',
        'Games': games,
        'Archives': test_archives(samples),
    }


class Image:
    def __init__(self, fat32, spc, clusters, label=b'EVOCARD    '):
        self.fat32, self.spc, self.clusters = fat32, spc, clusters
        self.rsv = 32 if fat32 else 1
        self.fats = 2
        self.root_ent = 0 if fat32 else 512
        self.root_secs = self.root_ent * 32 // 512
        per = 128 if fat32 else 256
        self.fatsz = (clusters + 2 + per - 1) // per
        self.system = self.rsv + self.fats * self.fatsz + self.root_secs
        self.total = self.system + clusters * spc
        self.fat = [0] * (clusters + 2)
        self.fat[0] = 0x0FFFFFF8 if fat32 else 0xFFF8
        self.fat[1] = 0x0FFFFFFF if fat32 else 0xFFFF
        self.data = bytearray(clusters * spc * 512)
        self.root = bytearray(self.root_secs * 512)
        self.next = 2
        self.label = label
        self.manifest = {}

    def eoc(self):
        return 0x0FFFFFFF if self.fat32 else 0xFFFF

    def alloc(self, count, skip=0):
        """count кластеров; skip — оставлять между ними столько свободных (файл вразбивку)."""
        out = []
        for _ in range(count):
            while self.fat[self.next] != 0:
                self.next += 1
            out.append(self.next)
            self.fat[self.next] = self.eoc()
            self.next += 1 + skip
        for a, b in zip(out, out[1:]):
            self.fat[a] = b
        if skip:
            self.next = out[0] + 1
        return out

    def write(self, clusters, payload):
        size = self.spc * 512
        for k, c in enumerate(clusters):
            chunk = payload[k * size:(k + 1) * size]
            at = (c - 2) * size
            self.data[at:at + len(chunk)] = chunk

    def clusters_for(self, n):
        return max(1, (n + self.spc * 512 - 1) // (self.spc * 512))


def sfn_alias(i, name):
    ext = name.rsplit('.', 1)[1].upper().encode('ascii', 'replace')[:3] if '.' in name[1:] else b''
    base = f'LFN{i:04X}'.encode()[:6] + b'~1'
    return base.ljust(8) + ext.ljust(3)


def sfn_label(raw, nt):
    """Короткое имя, как его покажет WC: CP866, строчные по флагам NT (бит 3 — имя, бит 4 — расширение)."""
    low = lambda s, on: ''.join(c.lower() if on and 'A' <= c <= 'Z' else c for c in s)
    base = low(raw[:8].rstrip(b' ').decode('cp866'), nt & 0x08)
    ext = low(raw[8:].rstrip(b' ').decode('cp866'), nt & 0x10)
    return base + ('.' + ext if ext else '')


def entries(name, attr, cluster, size, index, fat32):
    """Записи каталога: [LFN…] + SFN."""
    stamp = struct.pack('<HH', (12 << 11) | (34 << 5) | 28, ((2026 - 1980) << 9) | (11 << 5) | 2)   # 02.11.2026 12:34:56
    if isinstance(name, tuple):
        _, sfn, nt = name
        out = []
    else:
        sfn, nt = sfn_alias(index, name), 0
        units = list(name.encode('utf-16-le'))
        chars = [units[i] | units[i + 1] << 8 for i in range(0, len(units), 2)]
        chars.append(0)
        while len(chars) % 13:
            chars.append(0xFFFF)
        parts = [chars[i:i + 13] for i in range(0, len(chars), 13)]
        out = []
        for n in range(len(parts), 0, -1):
            e = bytearray(32)
            e[0] = n | (0x40 if n == len(parts) else 0)
            e[11], e[13] = 0x0F, lfn_checksum(sfn)
            p = parts[n - 1]
            for k, at in enumerate((1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)):
                struct.pack_into('<H', e, at, p[k])
            out.append(bytes(e))
    e = bytearray(32)
    e[:11] = sfn
    e[11], e[12] = attr, nt
    e[22:26] = stamp
    struct.pack_into('<H', e, 20, cluster >> 16 if fat32 else 0)
    struct.pack_into('<H', e, 26, cluster & 0xFFFF)
    struct.pack_into('<I', e, 28, size)
    out.append(bytes(e))
    return out


def build(img, tree, path, cluster, parent):
    """Каталог: файлы и подкаталоги получают кластеры, затем сам каталог пишется в свои кластеры (или в корень FAT16)."""
    raw = []
    if cluster:
        raw += entries(('sfn', b'.          ', 0), 0x10, cluster, 0, 0, img.fat32)
        raw += entries(('sfn', b'..         ', 0), 0x10, parent, 0, 0, img.fat32)
    else:
        raw += entries(('sfn', img.label, 0), 0x08, 0, 0, 0, img.fat32)
        raw += [b'\xe5' + bytes(31)]                      # удалённая запись
    frag = {}
    if 'frag.bin' in tree and 'frag2.bin' in tree:       # два файла кластерами через один
        a, b = tree['frag.bin'], tree['frag2.bin']
        ca = img.alloc(img.clusters_for(len(a)), skip=1)
        cb = img.alloc(img.clusters_for(len(b)), skip=1)
        assert all(y - x > 1 for x, y in zip(ca, ca[1:])), 'не вразбивку'
        frag = {'frag.bin': ca, 'frag2.bin': cb}
    for i, (name, node) in enumerate(tree.items()):
        label = name if isinstance(name, str) else sfn_label(name[1], name[2])
        full = f'{path}/{label}' if path else label
        if isinstance(node, dict):
            sub = img.alloc(1)[0]
            raw += entries(name, 0x10, sub, 0, i, img.fat32)
            build(img, node, full, sub, cluster if path else 0)   # «..» в подкаталоге корня — кластер 0
            img.manifest[full] = {'dir': True}
        else:
            cl = frag.get(name) or (img.alloc(img.clusters_for(len(node))) if node else [])
            img.write(cl, node)
            raw += entries(name, 0x20, cl[0] if cl else 0, len(node), i, img.fat32)
            img.manifest[full] = {'size': len(node), 'sha256': hashlib.sha256(node).hexdigest()}
            if label.lower().endswith(('.lha', '.lzh')) and len(node) >= 22:   # каталог-архив рядом, как в ESP
                # ключ — путь файла с «#»: рядом может быть настоящая папка с тем же именем
                img.manifest[full + '#'] = {'dir': True, 'name': label[:-4]}
                lha_manifest(node, full + '#', img.manifest)
    blob = b''.join(raw)
    if cluster == 0 and not img.fat32:
        assert len(blob) <= len(img.root), 'корень FAT16 мал'
        img.root[:len(blob)] = blob
        return
    size = img.spc * 512
    need = max(1, (len(blob) + 32 + size - 1) // size)       # нулевая запись в конце — по месту
    chain = [cluster]
    while len(chain) < need:
        chain += img.alloc(1)
        img.fat[chain[-2]] = chain[-1]
    img.write(chain, blob)


def finish(img, out, superfloppy=False):
    bpb = bytearray(512)
    bpb[0:3] = b'\xEB\x58\x90'
    bpb[3:11] = b'MSWIN4.1'
    struct.pack_into('<HBHBHHBHHHII', bpb, 11, 512, img.spc, img.rsv, img.fats, img.root_ent,
                     img.total if img.total < 65536 and not img.fat32 else 0, 0xF8,
                     0 if img.fat32 else img.fatsz, 63, 255, PART_LBA, img.total if img.total >= 65536 or img.fat32 else 0)
    if img.fat32:
        struct.pack_into('<IHHIHH', bpb, 36, img.fatsz, 0, 0, 2, 1, 6)
        bpb[0x42] = 0x29
        bpb[0x47:0x52] = b'EVOCARD    '
        bpb[0x52:0x5A] = b'FAT32   '
    else:
        bpb[0x26] = 0x29
        bpb[0x2B:0x36] = b'EVOCARD    '
        bpb[0x36:0x3E] = b'FAT16   '
    bpb[510:512] = b'\x55\xAA'
    fat = bytearray(img.fatsz * 512)
    for i, v in enumerate(img.fat):
        if img.fat32:
            struct.pack_into('<I', fat, i * 4, v)
        else:
            struct.pack_into('<H', fat, i * 2, v)
    reserved = bytearray(img.rsv * 512)
    reserved[:512] = bpb
    if img.fat32:                                        # FSInfo в секторе 1, копия загрузочного — в 6 и 7
        info = bytearray(512)
        info[0:4], info[484:488] = b'RRaA', b'rrAa'
        struct.pack_into('<II', info, 488, img.fat.count(0), img.next)
        struct.pack_into('<I', info, 508, 0xAA550000)
        reserved[512:1024] = info
        reserved[6 * 512:7 * 512] = bpb
        reserved[7 * 512:8 * 512] = info
    vol = bytearray(reserved)
    vol += fat * img.fats + img.root + img.data
    start = 0 if superfloppy else PART_LBA
    disk = bytearray(start * 512)
    if not superfloppy:
        disk[446:462] = struct.pack('<BBBBBBBBII', 0, 0, 2, 0, 0x0C if img.fat32 else 0x06, 0xFE, 0xFF, 0xFF,
                                    PART_LBA, img.total)
        disk[510:512] = b'\x55\xAA'
    disk += vol
    Path(out).write_bytes(disk)
    Path(out).with_suffix('.json').write_text(json.dumps(img.manifest, ensure_ascii=False, indent=1), encoding='utf-8')


def read_tree(base):
    out = {}
    for p in sorted(Path(base).iterdir()):
        out[p.name] = read_tree(p) if p.is_dir() else p.read_bytes()
    return out


def main():
    import sys
    sys.stdout.reconfigure(encoding='utf-8')
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--fat16', action='store_true')
    ap.add_argument('--superfloppy', action='store_true')
    ap.add_argument('--tree', type=Path, help='каталог с файлами вместо тестового набора')
    ap.add_argument('--lha', type=Path, help='каталог с настоящими архивами LHA (libxmp test-dev/data) для Archives')
    a = ap.parse_args()
    tree = read_tree(a.tree) if a.tree else default_tree(a.lha)
    img = Image(False, 4, 5000) if a.fat16 else Image(True, 1, 66000)
    if img.fat32:
        root = img.alloc(1)[0]
        assert root == 2
        build(img, tree, '', root, 0)
    else:
        build(img, tree, '', 0, 0)
    finish(img, a.out, a.superfloppy)
    print(f'{a.out}: FAT{32 if img.fat32 else 16}, {img.clusters} кластеров по {img.spc}, {len(img.manifest)} объектов')


if __name__ == '__main__':
    main()
