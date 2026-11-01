"""Запись на диск через настоящую цепочку ядра: 68000 -> Gayle/IDE -> nanomig -> окно передачи amiga_disk.

Вместо Kickstart в ROM лежит короткая программа: команда WRITE SECTORS на два сектора и узор, где все байты
разные. Сверяется каждый байт, который хост прочитал бы из окна. Так найдена ошибка ядра 0.6: байт 1 сектора
заменялся байтом 3 (на плате: HD01 has a checksum error, заголовок 00000002 на карте стал 00020002).
"""
from pathlib import Path
import struct, subprocess, sys

root = Path(__file__).resolve().parent
stage = root / 'boot-build'
hdf = root / 'build/new.hdf'
CYLINDER, COUNT = 5, 2


def pattern(sector):
    return bytes((i * 37 + 11 + (i >> 8) * 91 + sector * 59) & 255 for i in range(512))


code = bytes.fromhex(
    '41F900DA0000'      # lea     $DA0000,a0
    '1028001C'          # .ready: move.b  $1C(a0),d0
    '6BFA'              #         bmi.s   .ready
    '117C00A00018'      #         move.b  #$A0,$18(a0)        ; диск 0, головка 0
    '117C00020008'      #         move.b  #2,$08(a0)          ; два сектора
    '117C0001000C'      #         move.b  #1,$0C(a0)          ; сектор 1
    '117C00050010'      #         move.b  #5,$10(a0)          ; цилиндр 5
    '117C00000014'      #         move.b  #0,$14(a0)
    '117C0030001C'      #         move.b  #$30,$1C(a0)        ; WRITE SECTORS
    '43F900F80100'      #         lea     $F80100,a1          ; узор
    '7601'              #         moveq   #1,d3
    '1028001C'          # .drq:   move.b  $1C(a0),d0
    '08000003'          #         btst    #3,d0
    '67F6'              #         beq.s   .drq
    '343C00FF'          #         move.w  #255,d2
    '3099'              # .word:  move.w  (a1)+,(a0)
    '51CAFFFC'          #         dbra    d2,.word
    '1028001C'          # .busy:  move.b  $1C(a0),d0
    '6BFA'              #         bmi.s   .busy
    '51CBFFE4'          #         dbra    d3,.drq
    '60FE'              # .end:   bra.s   .end
)
rom = bytearray(b'\xff' * 524288)
rom[:8] = struct.pack('>II', 0x00080000, 0x00F80008)
rom[8:8 + len(code)] = code
assert 8 + len(code) <= 0x100
for s in range(COUNT):
    rom[0x100 + s * 512:0x100 + (s + 1) * 512] = pattern(s)
path = stage / 'ide_write.rom'
stage.mkdir(exist_ok=True)
path.write_bytes(rom)

# Узор идёт на шину словами 68000 и должен лечь в файл как есть: RDB тоже лежит в образе в порядке Amiga.
disk = hdf.open('rb').read(512)
assert disk[:4] == b'RDSK', hdf
sectors, heads = struct.unpack_from('>II', disk, 17 * 4)
first = CYLINDER * heads * sectors
subprocess.run([sys.executable, str(root / 'run_boot.py'), '--seconds', '0.3', '--rom', str(path), '--hdf', str(hdf)]
               + sys.argv[1:], check=True, stdout=subprocess.DEVNULL)
written = (stage / 'written.bin').read_bytes()
records = [written[n:n + 516] for n in range(0, len(written), 516)]
print(f'записано секторов: {len(records)}, ожидались {COUNT} начиная с {first} (головок {heads}, секторов {sectors})')
failed = len(records) != COUNT
for s, record in enumerate(records[:COUNT]):
    number = struct.unpack_from('<I', record)[0]
    data, want = record[4:], pattern(s)
    bad = [i for i in range(512) if data[i] != want[i]]
    print(f'сектор {number}: неверных байт {len(bad)}' + (f', первые {bad[:8]}' if bad else ''))
    if bad:
        print('  было ', data[:8].hex(), '\n  надо ', want[:8].hex())
    failed |= number != first + s or bool(bad)
print('FAIL' if failed else 'PASS')
sys.exit(1 if failed else 0)
