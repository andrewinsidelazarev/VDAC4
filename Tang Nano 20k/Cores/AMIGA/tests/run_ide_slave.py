"""Второй диск IDE (unit 5, ядро 0.11): у AMIGA.ELF 0.18 это карта Evo. Настоящая цепочка ядра: 68000 -> Gayle/IDE ->
nanomig -> ящик amiga_disk.

Вместо Kickstart в ROM лежит короткая программа: чтение с диска 1 — три сектора через конец дорожки (сектор 62 из 63),
затем WRITE SECTORS этих же данных на диск 0, где их видно в окне передачи (written.bin). Геометрия дисков разная (диск 1:
16 головок, 63 сектора; диск 0: из RDB tests/build/new.hdf, 32 сектора). Чтение дважды: READ MULTIPLE (c4, так читает
Kickstart) и READ SECTORS (20). До 0.11: ящик не подключал unit 5; nanomig переходил к следующему сектору чтения по геометрии
диска 0 (вместо секторов 61, 62, 63 диска 1 — 61, 63, 64); продолжение READ SECTORS после первого сектора шло записью.
"""
from pathlib import Path
import struct, subprocess, sys

root = Path(__file__).resolve().parent
stage = root / 'boot-build'
hdf = root / 'build/new.hdf'
SLAVE_HEADS, SLAVE_SECS, SLAVE_CYLS = 16, 63, 4
START_SECTOR, COUNT, CYLINDER = 62, 3, 5


def pattern(lba):
    return bytes((i * 29 + 7 + (i >> 8) * 113 + lba * 71) & 255 for i in range(512))


code = bytes.fromhex(
    '41F900DA0000'      # lea     $DA0000,a0
    '1028001C'          # .ready: move.b  $1C(a0),d0
    '6BFA'              #         bmi.s   .ready
    '117C00B00018'      #         move.b  #$B0,$18(a0)        ; диск 1, головка 0
    '117C00030008'      #         move.b  #3,$08(a0)          ; три сектора
    '117C003E000C'      #         move.b  #62,$0C(a0)         ; сектор 62: третий уже на головке 1
    '117C00000010'      #         move.b  #0,$10(a0)          ; цилиндр 0
    '117C00000014'      #         move.b  #0,$14(a0)
    '117C00CC001C'      #         move.b  #$CC,$1C(a0)        ; команда чтения: CC — место для c4 или 20
    '43F900100000'      #         lea     $100000,a1          ; буфер в Chip RAM выше перекрытия ROM
    '7602'              #         moveq   #2,d3
    '1028001C'          # .drq:   move.b  $1C(a0),d0
    '6BFA'              #         bmi.s   .drq
    '08000003'          #         btst    #3,d0
    '67F4'              #         beq.s   .drq
    '343C00FF'          #         move.w  #255,d2
    '32D0'              # .word:  move.w  (a0),(a1)+
    '51CAFFFC'          #         dbra    d2,.word
    '51CBFFE8'          #         dbra    d3,.drq
    '1028001C'          # .idle:  move.b  $1C(a0),d0
    '6BFA'              #         bmi.s   .idle
    '117C00A00018'      #         move.b  #$A0,$18(a0)        ; диск 0, головка 0
    '117C00030008'      #         move.b  #3,$08(a0)
    '117C0001000C'      #         move.b  #1,$0C(a0)          ; сектор 1
    '117C00050010'      #         move.b  #5,$10(a0)          ; цилиндр 5
    '117C00000014'      #         move.b  #0,$14(a0)
    '117C0030001C'      #         move.b  #$30,$1C(a0)        ; WRITE SECTORS
    '43F900100000'      #         lea     $100000,a1
    '7602'              #         moveq   #2,d3
    '1028001C'          # .wdrq:  move.b  $1C(a0),d0
    '08000003'          #         btst    #3,d0
    '67F6'              #         beq.s   .wdrq
    '343C00FF'          #         move.w  #255,d2
    '3099'              # .wword: move.w  (a1)+,(a0)
    '51CAFFFC'          #         dbra    d2,.wword
    '1028001C'          # .busy:  move.b  $1C(a0),d0
    '6BFA'              #         bmi.s   .busy
    '51CBFFE4'          #         dbra    d3,.wdrq
    '60FE'              # .end:   bra.s   .end
)
rom = bytearray(b'\xff' * 524288)
rom[:8] = struct.pack('>II', 0x00080000, 0x00F80008)
rom[8:8 + len(code)] = code
stage.mkdir(exist_ok=True)
at = rom.index(bytes.fromhex('117C00CC001C')) + 3

# Диск 1: RDB с геометрией (ядро берёт её из блока 0), остальные сектора — узор своего номера.
slave = bytearray()
for lba in range(SLAVE_CYLS * SLAVE_HEADS * SLAVE_SECS):
    slave += pattern(lba)
rdb = bytearray(512)
rdb[:4] = b'RDSK'
struct.pack_into('>III', rdb, 16 * 4, SLAVE_CYLS, SLAVE_SECS, SLAVE_HEADS)
slave[:512] = rdb
slave_path = stage / 'ide_slave.hdf'
slave_path.write_bytes(slave)

disk = hdf.open('rb').read(512)
assert disk[:4] == b'RDSK', hdf
sectors, heads = struct.unpack_from('>II', disk, 17 * 4)
assert sectors != SLAVE_SECS, 'геометрия дисков должна различаться'
first = CYLINDER * heads * sectors
wanted = [START_SECTOR - 1 + k for k in range(COUNT)]          # LBA диска 1: 61, 62, 63
failed = False
for n, (command, name) in enumerate(((0xC4, 'READ MULTIPLE'), (0x20, 'READ SECTORS'))):
    rom[at] = command
    path = stage / 'ide_slave.rom'
    path.write_bytes(rom)
    run = subprocess.run([sys.executable, str(root / 'run_boot.py'), '--seconds', '0.3', '--rom', str(path), '--hdf', str(hdf),
                          '--hdf1', str(slave_path)] + (['--no-build'] if n else []) + sys.argv[1:], stdout=subprocess.DEVNULL)
    written = (stage / 'written.bin').read_bytes()
    records = [written[k:k + 516] for k in range(0, len(written), 516)]
    print(f'{name}: записано секторов {len(records)}, ожидались {COUNT} начиная с {first}; на диске 1 — LBA {wanted}'
          + ('' if run.returncode == 0 else f'; модель вышла с кодом {run.returncode}'))
    failed |= run.returncode != 0 or len(records) != COUNT
    for k, record in enumerate(records[:COUNT]):
        number = struct.unpack_from('<I', record)[0]
        data = record[4:]
        got = next((lba for lba in range(len(slave) // 512) if slave[lba * 512:(lba + 1) * 512] == data), None)
        print(f'  сектор {number}: данные сектора {got} диска 1, надо {wanted[k]}')
        failed |= number != first + k or got != wanted[k]
print('FAIL' if failed else 'PASS')
sys.exit(1 if failed else 0)
