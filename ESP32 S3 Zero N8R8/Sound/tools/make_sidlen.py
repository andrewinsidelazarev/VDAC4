"""База длин мелодий SID для SID.ELF: Songlengths.md5 из HVSC (C64Music/DOCUMENTS) -> SIDLEN.BIN.

Запуск: python make_sidlen.py Songlengths.md5 SIDLEN.BIN

SIDLEN.BIN кладётся в /WC/ESP на карте Evo; плагин присылает его в ESP ресурсом до файла SID, и
SID.ELF ищет в нём длины подмелодий по MD5 всего файла (так их считает HVSC с выпуска 68).

Формат (числа — LE):
  0   "SIDL"
  4   версия 1, байт 0, слово 0
  8   число мелодий
  12  4097 смещений корзин от начала файла: корзина — старшие 12 бит MD5, её записи — от смещения
      корзины до смещения следующей, по возрастанию ключа
  записи: байты 2..6 MD5 (ключ), число длин (0 — 256), длины в секундах — слова (до 65535;
          доли секунды — вверх, как cRSID)
"""
import re
import struct
import sys
from pathlib import Path

BUCKETS = 4096
TIME = re.compile(r'(\d+):(\d+)(?:\.(\d+))?')


def parse(text: str) -> dict[bytes, list[int]]:
    tunes: dict[bytes, list[int]] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line[0] in ';[' or '=' not in line:
            continue
        key, times = line.split('=', 1)
        if len(key) != 32:
            continue
        seconds = []
        for minutes, secs, fraction in TIME.findall(times):
            total = int(minutes) * 60 + int(secs) + (1 if fraction and int(fraction) > 0 else 0)
            seconds.append(min(total, 0xFFFF))
        if seconds:
            tunes[bytes.fromhex(key)] = seconds[:256]
    return tunes


def build(tunes: dict[bytes, list[int]]) -> bytes:
    buckets: list[list[bytes]] = [[] for _ in range(BUCKETS)]
    for md5 in sorted(tunes, key=lambda m: m[2:7]):
        seconds = tunes[md5]
        record = md5[2:7] + bytes([len(seconds) & 0xFF]) + struct.pack(f'<{len(seconds)}H', *seconds)
        buckets[md5[0] << 4 | md5[1] >> 4].append(record)
    head = b'SIDL' + bytes([1, 0, 0, 0]) + struct.pack('<I', len(tunes))
    at = len(head) + 4 * (BUCKETS + 1)
    offsets, body = [], bytearray()
    for records in buckets:
        offsets.append(at + len(body))
        for record in records:
            body += record
    offsets.append(at + len(body))
    return head + struct.pack(f'<{BUCKETS + 1}I', *offsets) + bytes(body)


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    tunes = parse(Path(sys.argv[1]).read_text(encoding='latin-1'))
    data = build(tunes)
    Path(sys.argv[2]).write_bytes(data)
    songs = sum(len(s) for s in tunes.values())
    print(f'{len(tunes)} tunes, {songs} songs, {len(data)} bytes')


if __name__ == '__main__':
    main()
