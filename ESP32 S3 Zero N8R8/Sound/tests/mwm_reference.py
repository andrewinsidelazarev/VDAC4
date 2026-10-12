"""Эталонные записи в регистры OPL4 от оригинального проигрывателя MWM.

Z80-код проигрывателя (mwm_reference.asm: mwm_player.asm из ZXM-MoonSound как
есть) исполняется в эмуляторе Z80: MBPlayer_init, потом MBPlayer_play раз в
тик. Все OUT в порты OPL4 (FM C4..C7, волны 7E/7F) пишутся в журнал по тикам;
IN из 7F отдаёт последнее записанное в выбранный регистр, статус C4 — «не
занят». Набор волн (MWK) даёт таблицы MBPlayer_waves и MBPlayer_tones_data.

Формат журнала (его же печатает tests/mwm_check.cpp): строка init, строка на
тик, «Wrr=vv» (волны) и «Frrr=vv» (FM) через пробел, в конце «END n» (тик, на
котором песня кончилась: у порта ZX повтора нет) или «LIMIT n».

Запуск вручную: mwm_reference.py SONG.MWM KIT.MWK|- OUT.txt
"""
import pathlib
import subprocess
import sys

import z80

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / '.test-build' / 'mwm'
SOURCES = pathlib.Path(r'E:\zx\esp\downloads\zxm-moonsound\demo-disks\moonmusic_01')
SJASM = pathlib.Path(r'E:\zx\z80\zuma\sjasmplus.exe')
BREAKPOINT_HIT = getattr(z80.Z80Machine, '_BREAKPOINT_HIT', 1 << 1)
SENTINEL = 0x5FF0
STACK = 0x5FE0
SONG = 0xC000
INIT, PLAY = 0x6200, 0x6203


def available() -> bool:
    return SJASM.exists() and (SOURCES / 'mwm_player.asm').exists()


def assemble() -> tuple[bytes, dict]:
    BUILD.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(SJASM), f'--inc={SOURCES}', f'--inc={ROOT / "tests"}',
                    '--sym=mwm_reference.sym', str(ROOT / 'tests' / 'mwm_reference.asm')],
                   cwd=BUILD, check=True, capture_output=True)
    symbols = {}
    for line in (BUILD / 'mwm_reference.sym').read_text().splitlines():
        if ': EQU ' in line:
            name, value = line.split(': EQU ')
            symbols[name.strip()] = int(value, 16)
    return (BUILD / 'mwm_reference.bin').read_bytes(), symbols


def song_bank(data: bytes) -> bytes:
    """Как mbload: заголовок прочь, 278 байт настроек, позиции, адреса узоров,
    потом первый кусок [длина LE16][флаг] — сразу за ними. У порта ZX банков
    нет: песня должна уложиться в один."""
    assert data[:6] == b'MBMS\x10\x08', data[:6]
    pos = 6
    bank = bytearray(data[pos:pos + 278])
    pos += 278
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


def kit_tables(kit: bytes | None) -> tuple[bytes, bytes]:
    """MBPlayer_waves (48 записей по 25 байт) и MBPlayer_tones_data (64)."""
    if kit is None:
        return bytes(1200), bytes(64)
    assert kit[:6] == b'MBMS\x10\x0d', kit[:6]
    waves = kit[74:74 + 25 * kit[9]]
    return waves + bytes(1200 - len(waves)), kit[10:74]


class Reference:
    def __init__(self, code: bytes, symbols: dict, song: bytes, kit: bytes | None) -> None:
        self.symbols = symbols
        m = self.machine = z80.Z80Machine()
        self.memory = m.memory
        m.set_memory_block(INIT, code)
        waves, tones = kit_tables(kit)
        m.set_memory_block(symbols['MBPlayer_waves'], waves)
        m.set_memory_block(symbols['MBPlayer_tones_data'], tones)
        m.set_memory_block(SONG, song_bank(song))
        address = symbols['MBPlayer_songdata_addres']
        self.memory[address] = SONG & 0xFF
        self.memory[address + 1] = SONG >> 8
        m.set_input_callback(self.port_in)
        m.set_output_callback(self.port_out)
        m.set_breakpoint(SENTINEL)
        self.fm_address = [0, 0]
        self.wave_address = 0
        self.wave = bytearray(256)
        self.log: list[str] = []

    def port_in(self, port: int) -> int:
        p = port & 0xFF
        if p == 0xC4:
            return 0x00
        if p == 0x7F:
            return self.wave[self.wave_address]
        return 0xFF

    def port_out(self, port: int, value: int) -> None:
        p = port & 0xFF
        if p == 0xC4:
            self.fm_address[0] = value
        elif p == 0xC6:
            self.fm_address[1] = value
        elif p == 0xC5:
            self.log.append(f'F{self.fm_address[0]:03X}={value:02X}')
        elif p == 0xC7:
            self.log.append(f'F{0x100 | self.fm_address[1]:03X}={value:02X}')
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


def journal(code: bytes, symbols: dict, song: bytes, kit: bytes | None,
            limit: int = 60 * 60 * 15) -> list[str]:
    ref = Reference(code, symbols, song, kit)
    lines = [ref.call(INIT)]
    flag = symbols['MoonSound_flg_next']
    for tick in range(limit):
        lines.append(ref.call(PLAY))
        if ref.memory[flag]:
            lines.append(f'END {tick}')
            return lines
    lines.append(f'LIMIT {limit}')
    return lines


def main() -> None:
    code, symbols = assemble()
    song = pathlib.Path(sys.argv[1]).read_bytes()
    kit = None if sys.argv[2] == '-' else pathlib.Path(sys.argv[2]).read_bytes()
    lines = journal(code, symbols, song, kit)
    pathlib.Path(sys.argv[3]).write_text('\n'.join(lines) + '\n')
    print(lines[-1])


if __name__ == '__main__':
    main()
