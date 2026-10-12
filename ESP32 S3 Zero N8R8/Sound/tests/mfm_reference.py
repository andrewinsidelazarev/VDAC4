"""Эталонные записи в регистры OPL4 от оригинального проигрывателя MFM.

Z80-код проигрывателя (mfm_reference.asm: mfm_player.asm из ZXM-MoonSound как
есть) исполняется в эмуляторе Z80: MBPlayer_init, потом MBPlayer_play раз в
тик. OUT в порты OPL4 пишутся в журнал по тикам. Адрес FM защёлкивается
записью в C4 (банк 0) или C6 (банк 1), данные идут в C5 или C7 (у YMF262 и
YMF278B порт данных один); IN из C5/C7 отдаёт последнее записанное в
защёлкнутый регистр (OPL4 регистры FM читать умеет), IN из 7F — то же для
волн, статус C4 — «не занят».

Формат журнала (его же печатает tests/mfm_check.cpp): строка init, строка на
тик, «Frrr=vv» и «Wrr=vv» через пробел, в конце «END n» (песня без повтора
остановилась), «LOOP n» (пошла на повтор) или «LIMIT n».

Запуск вручную: mfm_reference.py SONG.MFM OUT.txt
"""
import pathlib
import subprocess
import sys

import z80

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / '.test-build' / 'mfm'
SOURCES = pathlib.Path(r'E:\zx\esp\downloads\zxm-moonsound\demo-disks\mfm_sample_02')
SJASM = pathlib.Path(r'E:\zx\z80\zuma\sjasmplus.exe')
BREAKPOINT_HIT = getattr(z80.Z80Machine, '_BREAKPOINT_HIT', 1 << 1)
SENTINEL = 0x5FF0
STACK = 0x5FE0
SONG = 0xC000
SETTINGS = 812
INIT, PLAY = 0x6200, 0x6203


def available() -> bool:
    return SJASM.exists() and (SOURCES / 'mfm_player.asm').exists()


def assemble() -> tuple[bytes, dict]:
    BUILD.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(SJASM), f'--inc={SOURCES}', f'--inc={ROOT / "tests"}',
                    '--sym=mfm_reference.sym', str(ROOT / 'tests' / 'mfm_reference.asm')],
                   cwd=BUILD, check=True, capture_output=True)
    symbols = {}
    for line in (BUILD / 'mfm_reference.sym').read_text().splitlines():
        if ': EQU ' in line:
            name, value = line.split(': EQU ')
            symbols[name.strip()] = int(value, 16)
    return (BUILD / 'mfm_reference.bin').read_bytes(), symbols


def song_bank(data: bytes) -> bytes:
    """Как mbload демо-диска: заголовок прочь, 812 байт настроек, позиции,
    адреса узоров и первый кусок узоров."""
    assert data[:6] == b'MBMS\x10\x01', data[:6]
    pos = 6
    bank = bytearray(data[pos:pos + SETTINGS])
    pos += SETTINGS
    count = bank[0] + 1
    positions = data[pos:pos + count]
    pos += count
    bank += positions
    patterns = (max(positions) + 1) * 2
    bank += data[pos:pos + patterns]
    pos += patterns
    head = data[pos:pos + 3]
    pos += 3
    if len(head) == 3 and head[2] != 0:
        bank += data[pos:pos + (head[0] | head[1] << 8)]
    assert len(bank) <= 0x4000, 'песня больше банка'
    return bytes(bank)


class Reference:
    def __init__(self, code: bytes, symbols: dict, song: bytes) -> None:
        m = self.machine = z80.Z80Machine()
        self.memory = m.memory
        m.set_memory_block(INIT, code)
        m.set_memory_block(SONG, song_bank(song))
        address = symbols['MBPlayer_songdata_addres']
        self.memory[address] = SONG & 0xFF
        self.memory[address + 1] = SONG >> 8
        m.set_input_callback(self.port_in)
        m.set_output_callback(self.port_out)
        m.set_breakpoint(SENTINEL)
        self.fm_address = 0
        self.fm = bytearray(512)
        self.wave_address = 0
        self.wave = bytearray(256)
        self.log: list[str] = []

    def port_in(self, port: int) -> int:
        p = port & 0xFF
        if p in (0xC4, 0xC6):
            return 0x00
        if p in (0xC5, 0xC7):
            return self.fm[self.fm_address]
        if p == 0x7F:
            return self.wave[self.wave_address]
        return 0xFF

    def port_out(self, port: int, value: int) -> None:
        p = port & 0xFF
        if p == 0xC4:
            self.fm_address = value
        elif p == 0xC6:
            self.fm_address = 0x100 | value
        elif p in (0xC5, 0xC7):
            self.fm[self.fm_address] = value
            self.log.append(f'F{self.fm_address:03X}={value:02X}')
        elif p == 0x7E:
            self.wave_address = value
        elif p == 0x7F:
            self.wave[self.wave_address] = value
            self.log.append(f'W{self.wave_address:02X}={value:02X}')

    def call(self, address: int) -> str:
        m = self.machine
        sp = STACK - 2
        self.memory[sp] = SENTINEL & 0xFF
        self.memory[sp + 1] = SENTINEL >> 8
        m.sp, m.pc = sp, address
        self.log = []
        for _ in range(100):
            m.ticks_to_stop = 100_000
            event = m.run()
            if event & BREAKPOINT_HIT and m.pc == SENTINEL:
                return ' '.join(self.log)
        raise AssertionError(f'проигрыватель зациклился: PC=#{m.pc:04X}')


def journal(code: bytes, symbols: dict, song: bytes, limit: int = 60 * 60 * 15) -> list[str]:
    ref = Reference(code, symbols, song)
    lines = [ref.call(INIT)]
    busy = symbols['MBPlayer_play_busy']
    pos = symbols['MBPlayer_play_pos']
    last = None
    for tick in range(limit):
        lines.append(ref.call(PLAY))
        if ref.memory[busy] == 0:
            lines.append(f'END {tick}')
            return lines
        now = ref.memory[pos]
        if last is not None and last != 0xFF and now < last:
            lines.append(f'LOOP {tick}')
            return lines
        last = now
    lines.append(f'LIMIT {limit}')
    return lines


def main() -> None:
    code, symbols = assemble()
    song = pathlib.Path(sys.argv[1]).read_bytes()
    lines = journal(code, symbols, song)
    pathlib.Path(sys.argv[2]).write_text('\n'.join(lines) + '\n')
    print(lines[-1])


if __name__ == '__main__':
    main()
