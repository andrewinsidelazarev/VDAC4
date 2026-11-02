"""Add/replace non-ALLOC PROGBITS: add_card.py ELF card.bin [output.ELF].

Default output replaces ELF. Existing allocated sections, section indices and program headers are preserved.
"""
import argparse
from pathlib import Path
import struct
import zlib


def destination(path):
    # Без make_card: тот тянет Pillow, а сборки ядер зовут этот файл питоном ESP-IDF, где его нет
    return Path(path).resolve()


def sections(data):
    if len(data) < 52 or data[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('Expected ELF32 little-endian')
    if struct.unpack_from('<HHI', data, 16) not in ((2, 94, 1), (3, 94, 1)):
        raise ValueError('Expected Xtensa executable/shared object')
    table = struct.unpack_from('<I', data, 32)[0]
    size, count, index = struct.unpack_from('<HHH', data, 46)
    if size != 40 or not 0 < index < count <= 1024 or table < 52 or table+count*40 > len(data):
        raise ValueError('Bad section table')
    rows = [list(struct.unpack_from('<10I', data, table+i*40)) for i in range(count)]
    shstr = rows[index]
    if shstr[1] != 3 or shstr[4]+shstr[5] > len(data):
        raise ValueError('Bad section name table')
    names = data[shstr[4]:shstr[4]+shstr[5]]
    decoded = []
    for row in rows:
        if row[1] != 8 and row[4]+row[5] > len(data):
            raise ValueError('Section outside file')
        if row[0] >= len(names) or b'\0' not in names[row[0]:]:
            raise ValueError('Invalid section name')
        decoded.append(names[row[0]:].split(b'\0', 1)[0])
    return rows, index, names, decoded


def add(elf, card):
    if not 64 <= len(card) <= 24576 or card[:8] != b'VD4CARD\0':
        raise ValueError('Bad card header')
    if struct.unpack_from('<HHI', card, 8) != (1, 64, len(card)):
        raise ValueError('Bad card version/size')
    if zlib.crc32(card[:16]+bytes(4)+card[20:]) != struct.unpack_from('<I', card, 16)[0]:
        raise ValueError('Bad card CRC32')
    rows, index, names, decoded = sections(elf)
    matches = [i for i, name in enumerate(decoded) if name == b'.vdac4.card']
    if len(matches) > 1:
        raise ValueError('Duplicate card sections')
    result = bytearray(elf)
    # Append-only deliberately keeps all original ELF offsets and program bytes intact.
    result.extend(bytes((-len(result)) % 4))
    offset = len(result)
    result.extend(card)
    if matches:
        slot = matches[0]
        if rows[slot][2] != 0 or rows[slot][1] != 1:
            raise ValueError('Refuse to replace a loaded or non-PROGBITS card section')
        name_offset = rows[slot][0]
    else:
        if len(rows) == 1024:
            raise ValueError('Too many sections')
        name_offset = len(names)
        names += b'.vdac4.card\0'
        slot = len(rows)
        rows.append([0]*10)
    rows[slot] = [name_offset, 1, 0, 0, offset, len(card), 0, 0, 1, 0]
    rows[index][4:6] = [len(result), len(names)]
    result.extend(names)
    result.extend(bytes((-len(result)) % 4))
    struct.pack_into('<I', result, 32, len(result))
    struct.pack_into('<H', result, 48, len(rows))
    for row in rows:
        result.extend(struct.pack('<10I', *row))
    return bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('card', type=Path)
    parser.add_argument('output', type=Path, nargs='?')
    args = parser.parse_args()
    out = destination(args.output or args.elf)
    result = add(args.elf.read_bytes(), args.card.read_bytes())
    out.write_bytes(result)
    print(f'{out.name}: {len(result)} bytes; .vdac4.card PROGBITS flags=0')


if __name__ == '__main__':
    main()
