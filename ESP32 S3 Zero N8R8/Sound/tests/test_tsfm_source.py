"""Small compiled TFC fixture; no external music or SoundFont required."""
import struct


def make_tfc(rate=50, blocks=3):
    header = bytearray(34)
    header[:6] = b'TFMcom'
    header[9] = rate
    metadata = b'TSFM test\0Test author\0\0'
    pairs = [(0xB0, 7)]
    for op in (0, 4, 8, 12):
        pairs += [(0x30 + op, 1), (0x40 + op, 20), (0x50 + op, 31),
                  (0x60 + op, 0), (0x70 + op, 0), (0x80 + op, 15)]
    frame = bytes([0x81 | len(pairs) * 2, 0x24, 0x60])
    frame += bytes(v for pair in pairs for v in pair)
    stream = frame + b'\xe0' * blocks + b'\x7f'
    offset = len(header) + len(metadata)
    struct.pack_into('<6H', header, 10, offset, *([offset + len(stream) - 1] * 5))
    return bytes(header) + metadata + stream


def make_tfd(frames=200, title=b'TFD test', author=b'TFD author'):
    """TurboFM Dump: на первом кадре — голос FM на канале 0 обоих чипов и нажатие,
    дальше пустые кадры (FF и группы FE n). Кадров ровно frames (последняя граница
    закрывает последний кадр)."""
    stream = bytearray(b'TFMD' + title + b'\0' + author + b'\0' + b'\0')
    voice = [(0xB0, 7)]
    for op in (0, 4, 8, 12):
        voice += [(0x30 + op, 1), (0x40 + op, 20), (0x50 + op, 31),
                  (0x60 + op, 0), (0x70 + op, 0), (0x80 + op, 15)]
    stream.append(0xFF)                         # граница 0: начало кадра 0
    for chip in (0, 1):
        stream.append(0xFC if chip == 0 else 0xFD)
        for reg, val in voice + [(0xA4, 0x22), (0xA0, 0x69), (0x28, 0xF0)]:
            stream += bytes((reg, val))
    left = frames                               # каждая граница закрывает кадр
    while left >= 3 + 255:
        stream += b'\xFE\xFF'; left -= 3 + 255
    while left >= 3:
        n = min(left, 3 + 200)
        stream += bytes((0xFE, n - 3)); left -= n
    stream += b'\xFF' * left
    stream.append(0xFB)
    return bytes(stream)


def make_mtc(tracks, name=b'MTC test', author=b'MTC author'):
    """Multitrack Container ZXTune: блоки [имя][длина BE32][данные], выравнивание 2;
    tracks — список (данные, имя файла)."""
    def chunk(tag, body):
        return tag + len(body).to_bytes(4, 'big') + body + (b'\0' if len(body) & 1 else b'')
    tune = chunk(b'NAME', name) + chunk(b'AUTH', author)
    for data, filename in tracks:
        tune += chunk(b'TRCK', chunk(b'DATA', data) + chunk(b'PROP', b'Filename=' + filename))
    return chunk(b'MTC1', tune)
