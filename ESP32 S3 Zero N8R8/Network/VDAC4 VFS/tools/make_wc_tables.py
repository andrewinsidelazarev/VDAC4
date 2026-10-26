"""Таблицы сверки WC Improved v1.11i для плагина VDAC4 VFS — из самого boot.$C.

Ядро CORE32 #4000..#5175 лежит в boot.$C HR-потоком (#CA88..#D733 образа, файл грузится с #6000),
упакованные драйверы DR1..DR4 — как есть (#C00C..). Распаковка — mhmt.exe. Остальной код WC
(#6011..#BFFF: страницы #05 и #02) лежит в файле как есть.

Выход:
  src/wc111i.inc   — адреса и эталонные байты: переходник и DOS_SWP, входы WC для перемонтирования,
                     контрольная сумма свободной после запуска области страницы #02;
  src/sdzc_state.bin — начальные значения общего блока драйвера #3848..#38CF из распакованного SD-ZC;
  build/wc/core.bin, build/wc/page05.bin, build/wc/page02.bin — образы для стенда.
"""
import hashlib, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
BOOT = Path(r'E:\zx\esp\wc-improved\exe\boot.$C')
MHMT = Path(r'E:\zx\FAT32 Driver\build\unreal\mhmt.exe')
BOOT_SHA = None   # заполняется при первом прогоне, проверяется дальше

data = BOOT.read_bytes()
assert data[:8] == b'WC110B  ' and len(data) == 30602, 'нужен boot.$C WC Improved v1.11i (30602 байта)'
sha = hashlib.sha256(data).hexdigest()
work = HERE / 'build' / 'wc'
work.mkdir(parents=True, exist_ok=True)


def at(addr, n):
    return data[addr - 0x6000:addr - 0x6000 + n]


def unpack(stream, name):
    (work / (name + '.hr')).write_bytes(stream)
    r = subprocess.run([str(MHMT), '-hst', '-zxh', '-d', str(work / (name + '.hr')), str(work / (name + '.bin'))],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    return (work / (name + '.bin')).read_bytes()


core = unpack(at(0xCA88, 0xD734 - 0xCA88), 'core')
assert len(core) == 0x5176 - 0x4000
drivers = at(0xC00C, 0xCA88 - 0xC00C)
dr2 = unpack(drivers[0x535D - 0x5176:0x5601 - 0x5176], 'dr2')
assert len(dr2) == 0x048C

# Страницы как в памяти после запуска (для стенда): #05 — ядро и код WC #6000..#7FFF, #02 — #8000..#BFFF
page05 = bytearray(0x4000)
page05[0:len(core)] = core
page05[0x2000:0x4000] = at(0x6000, 0x2000)
page02 = bytearray(at(0x8000, 0x4000))
(work / 'page05.bin').write_bytes(page05)
(work / 'page02.bin').write_bytes(page02)


def core_bytes(addr, n):
    return bytes(page05[addr - 0x4000:addr - 0x4000 + n])


TRAMP, DOS_SWP, LDHL, TAIL = 0x4FE8, 0x4FF8, 0x501C, 0x5028
swp = core_bytes(TRAMP, 0x502F - TRAMP)
assert swp[:16] == bytes(16), 'перед DOS_SWP нет 16 свободных байтов'
assert swp[16:18] == b'\x06\x00' and swp[LDHL - TRAMP:LDHL - TRAMP + 3] == b'\x21\x5D\x53'
assert swp[TAIL - TRAMP:] == b'\xCD\xE4\x5B\x00\xDD\xE1\xC9'
entries = {'WC_MNG8': 0x68B1, 'WC_MNGC': 0x68C1, 'WC_DRACT': 0x6868, 'WC_ORIGA': 0x63ED, 'WC_XPAN': 0x6149}
checks = {name: core_bytes(addr, 8) for name, addr in entries.items()}
STATUS = 0x8723
status = bytes(page02[STATUS - 0x8000:STATUS - 0x8000 + 8])
FREE, FREE_END = 0xAB4F, 0xB000
free = bytes(page02[FREE - 0x8000:FREE_END - 0x8000])
free_sum = sum(free) & 0xFFFF


def db(b):
    return ', '.join(f'#{x:02X}' for x in b)


lines = [
    '; Wild Commander Improved v1.11i — boot.$C «WC110B», 30602 байта,',
    f'; SHA-256 {sha}.',
    '; Создано tools/make_wc_tables.py из самого boot.$C: не править руками.',
    '',
    f'WC_TRAMP        equ #{TRAMP:04X}          ; 16 нулей перед DOS_SWP — переходник к резиденту',
    f'WC_DOS_SWP      equ #{DOS_SWP:04X}',
    f'WC_SD2_LDHL     equ #{LDHL:04X}          ; LD HL,DR2 ветки A=6 (второй SD ZC)',
    f'WC_SD2_TAIL     equ #{TAIL:04X}          ; CALL SELDEV_INVALIDATE / NOP / POP IX / RET',
    f'WC_SWP_LEN      equ {len(swp)}',
]
for name, addr in entries.items():
    lines.append(f'{name:<15} equ #{addr:04X}')
lines += [
    f'WC_STATUS       equ #{STATUS:04X}          ; страница #02 в окне #8000',
    'WC_WINDOW       equ #7EA0          ; описатели панелей',
    'WC_WINDO2       equ #7EC4',
    f'WC_FREE02       equ #{FREE:04X}          ; одноразовые данные запуска в странице #02, после запуска свободны',
    f'WC_FREE02_END   equ #{FREE_END:04X}',
    f'WC_FREE02_SUM   equ #{free_sum:04X}          ; их сумма байтов в boot.$C',
]
data = [
    '; Таблицы сверки WC Improved v1.11i (tools/make_wc_tables.py): включать внутрь кода плагина.',
    '; Эталон #4FE8..#502E: 16 нулей и DOS_SWP',
    'WC_SWP_BYTES:',
]
for i in range(0, len(swp), 16):
    data.append('        db ' + db(swp[i:i + 16]))
data.append('; Первые 8 байтов входов WC, которые зовёт плагин (сверка перед перемонтированием)')
data.append('WC_ENTRY_CHECKS:')
for name, addr in entries.items():
    data.append(f'        dw {name}')
    data.append('        db ' + db(checks[name]))
data.append('        dw 0')
data.append('WC_STATUS_CHECK: db ' + db(status))
(HERE / 'src' / 'wc111i.inc').write_text('\n'.join(lines) + '\n', encoding='utf-8')
(HERE / 'src' / 'wc111i_data.inc').write_text('\n'.join(data) + '\n', encoding='utf-8')
(HERE / 'src' / 'sdzc_state.bin').write_bytes(dr2[0x48:0xD0])
print('ok', sha, 'free sum', hex(free_sum))
