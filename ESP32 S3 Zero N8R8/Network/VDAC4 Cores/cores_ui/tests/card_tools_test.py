"""Boundary and deterministic tests for the Python authoring tools."""
import json
from pathlib import Path
from PIL import Image
from make_card import FIELDS, build
from add_card import add

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT/'build-pc'


def rejects(call):
    try:
        call()
    except (ValueError, UnicodeError):
        return
    raise AssertionError('Invalid authoring input accepted')


def test_tools():
    folder = ROOT/'cards/NES'
    metadata = json.loads((folder/'card.json').read_text(encoding='utf-8'))
    icon, image = folder/'icon.png', folder/'image.png'
    expected = (folder/'card.bin').read_bytes()
    assert build(metadata, icon, image) == build(metadata, icon, image) == expected
    for field, limit in FIELDS.items():
        for value in ('X'*(limit+1), 'A\0B', 'A\nB', '\u0410', 42):
            bad = dict(metadata, **{field:value})
            rejects(lambda:build(bad, icon, image))
    rejects(lambda:build(dict(metadata, title=''), icon, image))
    rejects(lambda:build(dict(metadata, extra='unsupported'), icon, image))
    for accent in ('#12345', '#zzzzzz', -1, 65536, True):
        rejects(lambda:build(dict(metadata, accent=accent), icon, image))
    path = BUILD/'invalid_image.png'
    for im, small in ((Image.new('RGBA',(57,28)),True), (Image.new('RGBA',(256,129)),False),
                      (Image.new('RGBA',(1,1),(255,0,0,127)),True)):
        im.save(path)
        rejects(lambda:build(metadata, path if small else icon, image if small else path))
    im = Image.new('RGBA',(256,1))
    im.putdata([((i%32)*8,(i//32)*4,0,255) for i in range(256)])
    im.save(path)
    rejects(lambda:build(metadata, icon, path))
    limits = {key:'W'*limit for key,limit in FIELDS.items()}
    limits['accent'] = 65535
    (BUILD/'limits.bin').write_bytes(build(limits, icon, image))
    # Palette index 0 is transparent; opaque black must get another index.
    im = Image.new('RGBA',(1,1),(0,0,0,255))
    im.save(path)
    one = build(metadata,path,path)
    assert one[-4:] == bytes((1,1,1,1))
    original = (BUILD/'elf/NES_original.ELF').read_bytes()
    assert add(original,expected) == add(original,expected)
    for bad in (b'',bytes(64),expected[:100],expected[:-1]+bytes((expected[-1]^1,))):
        rejects(lambda:add(original,bad))
    rejects(lambda:add(b'not an ELF',expected))
    # Папки вывода не ограничены: карточку вставляют сборки ядер в свои dist/ (2026-11-02)
    print('PASS authoring tools: deterministic output, strings, colors, PNG bounds/alpha, CRC, output scope',flush=True)
