"""Адреса в модуле ESP, которые загрузчик переведёт неверно.

Загрузчик (esp_elf_map_sym) переводит значение R_XTENSA_RELATIVE по секции, в которую оно попало, а .text
отображён на шину команд. Компилятор может хранить не адрес данных, а «адрес минус смещение» (оптимизация цикла):
такое значение численно лежит в .text и становится адресом на шине команд, запись по нему — Cache error
(AMIGA.ELF 0.8, take_sector, sector_words − 0x5000). Значение в .text допустимо, только если это начало функции
(инструкция entry); значение вне всех секций загрузчик превратит в 0.
"""
from pathlib import Path
import struct, sys

R_XTENSA_RELATIVE = 5


def sections(data):
    shoff = struct.unpack_from('<I', data, 32)[0]
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 46)
    raw = [struct.unpack_from('<10I', data, shoff + i * shentsize) for i in range(shnum)]
    names = data[raw[shstrndx][4]:raw[shstrndx][4] + raw[shstrndx][5]]
    out = {}
    for s in raw:
        out[names[s[0]:names.index(b'\0', s[0])].decode()] = dict(addr=s[3], off=s[4], size=s[5], type=s[1], flags=s[2])
    return out


def loop_end(data, text, literal):
    """Литерал читает l32r, и следом идёт wsr.lend: значение — конец цикла, адрес кода."""
    for at in range(text['off'], text['off'] + text['size'] - 3):
        if data[at] & 15 != 1:
            continue
        pc = at - text['off'] + text['addr']
        imm = data[at + 1] | data[at + 2] << 8
        if ((pc + 3) & ~3) + ((imm | ~0xFFFF) << 2) == literal and b'\xf0\x01\x13' in data[at + 3:at + 9]:
            return True
    return False


def check(path):
    data = Path(path).read_bytes()
    assert data[:4] == b'\x7fELF', path
    sec = sections(data)
    text = sec['.text']
    alloc = [s for s in sec.values() if s['flags'] & 2 and s['size']]

    def file_offset(addr):
        for s in alloc:
            if s['addr'] <= addr < s['addr'] + s['size'] and s['type'] != 8:
                return s['off'] + addr - s['addr']
        return None
    bad, total = [], 0
    rela = sec['.rela.dyn']
    for at in range(rela['off'], rela['off'] + rela['size'], 12):
        where, info, _ = struct.unpack_from('<IIi', data, at)
        if info & 255 != R_XTENSA_RELATIVE:
            continue
        off = file_offset(where)
        if off is None:
            continue
        value = struct.unpack_from('<I', data, off)[0]
        total += 1
        if text['addr'] <= value < text['addr'] + text['size']:
            code = data[text['off'] + value - text['addr']]
            # entry a1, N — начало функции; конец аппаратного цикла (l32r, затем wsr.lend) — тоже адрес кода.
            if code != 0x36 and not loop_end(data, text, where):
                bad.append((where, value, 'в .text, но не начало функции'))
        elif not any(s['addr'] <= value <= s['addr'] + s['size'] for s in alloc):
            bad.append((where, value, 'вне всех секций'))
    return total, bad


failed = 0
for path in sys.argv[1:]:
    total, bad = check(path)
    print(f'{Path(path).name}: адресов {total}, подозрительных {len(bad)}')
    for where, value, why in bad:
        print(f'  литерал {where:#x} = {value:#x}: {why}')
    failed += len(bad)
sys.exit(1 if failed else 0)
