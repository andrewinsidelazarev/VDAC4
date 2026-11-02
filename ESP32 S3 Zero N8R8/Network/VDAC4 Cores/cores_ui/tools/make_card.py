"""Build v1 card: make_card.py card.json icon.png image.png [card.bin]."""
import argparse
import json
from pathlib import Path
import struct
import zlib
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
TEMP = Path(r'E:\zx\cache\tmp\codex_cores_ui')
FIELDS = dict(title=48, full_name=96, year=8, manufacturer=48, line1=32,
              line2=32, category=16, manage_key=8)


def destination(path):
    # Пишет куда скажут: карточку в ELF вставляют сборки самих ядер (их dist/), не только cores_ui
    return Path(path).resolve()


def rgb565(color):
    if isinstance(color, str):
        if len(color) != 7 or color[0] != '#':
            raise ValueError('accent must be #RRGGBB or a uint16 RGB565')
        r, g, b = bytes.fromhex(color[1:])
        return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3
    if type(color) is not int or not 0 <= color <= 65535:
        raise ValueError('Invalid RGB565 accent')
    return color


def build(metadata, icon, image):
    if set(metadata) - set(FIELDS) - {'accent'}:
        raise ValueError('Unknown JSON field')
    strings = []
    for key, limit in FIELDS.items():
        value = metadata.get(key, '')
        if not isinstance(value, str):
            raise ValueError(f'{key}: expected string')
        raw = value.encode('ascii')
        if len(raw) > limit or any(c < 32 or c > 126 for c in raw) or (key == 'title' and not raw):
            raise ValueError(f'{key}: invalid ASCII or length (max {limit})')
        strings.append(raw)
    palette, indices = [0], {}
    frames = []
    dimensions = []
    for path, limits in ((icon, (56, 28)), (image, (256, 128))):
        with Image.open(path) as source:
            if not (1 <= source.width <= limits[0] and 1 <= source.height <= limits[1]):
                raise ValueError(f'{path}: dimensions exceed {limits}')
            im = source.convert('RGBA')
        dimensions.extend(im.size)
        pixels = []
        for r, g, b, alpha in im.getdata():
            if alpha not in (0, 255):
                raise ValueError('PNG alpha must be 0 or 255')
            if not alpha:
                pixels.append(0)
                continue
            color = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3
            if color not in indices:
                if len(palette) == 256:
                    raise ValueError('More than 255 opaque RGB565 colors')
                indices[color] = len(palette)
                palette.append(color)
            pixels.append(indices[color])
        rle = bytearray()
        pos = 0
        while pos < len(pixels):
            end = pos + 1
            while end < len(pixels) and end-pos < 255 and pixels[end] == pixels[pos]:
                end += 1
            rle.extend((end-pos, pixels[pos]))
            pos = end
        frames.append(rle)
    header = bytearray(64)
    struct.pack_into('<8sHH', header, 0, b'VD4CARD\0', 1, 64)
    struct.pack_into('<6H2I8H', header, 20, rgb565(metadata['accent']), len(palette),
                     *dimensions, *(len(f) for f in frames), *(len(s) for s in strings))
    result = header + b''.join(strings) + struct.pack('<'+'H'*len(palette), *palette) + b''.join(frames)
    if len(result) > 24576:
        raise ValueError('Card exceeds 24 KiB')
    struct.pack_into('<I', result, 12, len(result))
    struct.pack_into('<I', result, 16, zlib.crc32(result))
    return bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('json', type=Path)
    parser.add_argument('icon', type=Path)
    parser.add_argument('image', type=Path)
    parser.add_argument('output', nargs='?', type=Path)
    args = parser.parse_args()
    out = destination(args.output or args.json.with_name('card.bin'))
    data = build(json.loads(args.json.read_text(encoding='utf-8')), args.icon, args.image)
    out.write_bytes(data)
    print(f'{out.name}: {len(data)} bytes')


if __name__ == '__main__':
    main()
