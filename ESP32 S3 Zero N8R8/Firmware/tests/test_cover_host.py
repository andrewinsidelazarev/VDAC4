"""Обложка MP3 на ПК: разбор метки ID3v2 (CoverParser) и миниатюра (CoverThumb).

src/cover_art.cpp собирается MSVC вместе со стендом tests/cover_host.cpp. Стенд
шлёт разбору куски файла с того места, которое тот просит, как это делает
плагин проигрывателя (AUDIO_COVER), а картинку «декодирует» моделью (JPEG на
ПК не разбирается: на ESP это TJpgDec из ПЗУ). Проверяется:
- метки 2.3 и 2.4, расширенный заголовок, описание в UTF-16, указатель длины
  данных; сжатая картинка и PNG пропускаются, берётся следующая JPEG;
- разбор просит только заголовки кадров и сам кадр APIC: большие кадры
  перед ним не пересылаются;
- отказы: нет картинки, только PNG, метка 2.2, рассинхронизация, файл короче
  метки;
- метки (название, исполнитель, альбом, год, номер) собираются по пути и
  после картинки тоже, в CP866: из UTF-8, UTF-16 с BOM и без, однобайтовой
  строки (CP1251); из даты 2.4 — только год; метки есть и без картинки;
- миниатюра 64×64: чистые цвета палитры WC остаются собой, чёрный — цвет 8
  (0 у спрайта прозрачен), из картинки берётся середина, маленькая
  растягивается.
Без Visual Studio Build Tools тесты пропускаются.
"""
import pathlib
import struct
import subprocess
import tempfile
import unittest

from test_weather_parse import ROOT, find_vcvars, short

BUILD = ROOT / '.test-build' / 'cover_host'
SOURCES = [
    ('tests/cover_host.cpp', 'cover_host.obj'),
    ('src/cover_art.cpp', 'cover_art.obj'),
]
INCLUDES = ['include']
_EXE: pathlib.Path | None = None


def build() -> pathlib.Path | None:
    global _EXE
    if _EXE is not None:
        return _EXE
    vcvars = find_vcvars()
    if vcvars is None:
        return None
    BUILD.mkdir(parents=True, exist_ok=True)
    flags = ('/nologo /c /EHsc /std:c++17 /O2 /utf-8 /W3 /D_CRT_SECURE_NO_WARNINGS ' +
             ' '.join(f'/I "{short(ROOT / path)}"' for path in INCLUDES))
    lines = ['@echo off', f'call "{short(vcvars)}" >nul 2>nul || exit /b 1']
    lines += [f'cl {flags} "{short(ROOT / source)}" /Fo:{obj} || exit /b 1'
              for source, obj in SOURCES]
    lines.append('cl /nologo ' + ' '.join(obj for _, obj in SOURCES) + ' /Fe:cover_host.exe')
    script = BUILD / 'build_host.bat'
    script.write_text('\r\n'.join(lines) + '\r\n', encoding='ascii')
    result = subprocess.run(['cmd.exe', '/c', short(script)], capture_output=True,
                            cwd=short(BUILD))
    if result.returncode != 0:
        output = (result.stdout.decode('cp866', 'replace') +
                  result.stderr.decode('cp866', 'replace'))
        raise AssertionError(f'сборка cover_host.exe не удалась:\n{output}')
    _EXE = BUILD / 'cover_host.exe'
    return _EXE


def syncsafe(value: int) -> bytes:
    return bytes([(value >> 21) & 0x7F, (value >> 14) & 0x7F, (value >> 7) & 0x7F, value & 0x7F])


def frame(name: bytes, body: bytes, version: int = 4, flags: int = 0) -> bytes:
    size = syncsafe(len(body)) if version == 4 else struct.pack('>I', len(body))
    return name + size + bytes([0, flags]) + body


def tag(frames: bytes, version: int = 4, padding: int = 300, extended: bytes = b'',
        flags: int = 0) -> bytes:
    body = extended + frames + bytes(padding)
    return b'ID3' + bytes([version, 0, flags]) + syncsafe(len(body)) + body


def fake_jpeg(width: int, height: int, pixel) -> bytes:
    """«JPEG» стенда: FF D8 FF FAKE, размеры и точки RGB888."""
    data = bytearray(b'\xFF\xD8\xFFFAKE' + struct.pack('<HH', width, height))
    for y in range(height):
        for x in range(width):
            data += bytes(pixel(x, y))
    return bytes(data)


def apic(image: bytes, encoding: int = 0, mime: bytes = b'image/jpeg',
         description: bytes = b'') -> bytes:
    if encoding in (1, 2):
        terminator = b'\x00\x00'
    else:
        terminator = b'\x00'
    return bytes([encoding]) + mime + b'\x00' + bytes([3]) + description + terminator + image


def png(size: int = 600) -> bytes:
    return b'\x89PNG\r\n\x1a\n' + bytes(size)


class CoverHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if build() is None:
            raise unittest.SkipTest('нет MSVC (vcvars64.bat): стенд обложки пропущен')
        cls.temp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temp.cleanup()

    def run_cover(self, content: bytes, chunk: int = 1020, mode: str = 'seq') -> dict[str, str]:
        path = pathlib.Path(self.temp.name) / 'cover.bin'
        path.write_bytes(content)
        result = subprocess.run([str(_EXE), str(path), str(chunk), mode],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        values = {}
        for line in result.stdout.splitlines():
            key, _, value = line.partition(' ')
            values[key] = value
        return values

    def thumb(self, values: dict[str, str]) -> list[list[int]]:
        data = bytes.fromhex(values['thumb'])
        self.assertEqual(len(data), 2048)
        rows = []
        for row in range(64):
            line = []
            for byte in data[row * 32:(row + 1) * 32]:
                line += [byte >> 4, byte & 15]
            rows.append(line)
        return rows

    def assert_image(self, values: dict[str, str], image: bytes) -> None:
        self.assertEqual(values['result'], '1', values)
        size, digest = values['image'].split()
        self.assertEqual(int(size), len(image))
        hash_value = 2166136261
        for byte in image:
            hash_value = ((hash_value ^ byte) * 16777619) & 0xFFFFFFFF
        self.assertEqual(digest, f'{hash_value:08x}')

    def test_v24_picture_is_found_and_only_it_is_sent(self) -> None:
        image = fake_jpeg(40, 40, lambda x, y: (255, 255, 255))
        big = frame(b'TXXX', bytes(200000))                 # большой кадр перед картинкой
        content = tag(frame(b'TIT2', b'\x00Song') + big + frame(b'APIC', apic(image))) + bytes(5000)
        for chunk, mode in ((1020, 'seq'), (4096, 'seq'), (100, 'seq'), (1020, 'overlap'),
                            (1020, 'late')):
            with self.subTest(chunk=chunk, mode=mode):
                values = self.run_cover(content, chunk, mode)
                self.assert_image(values, image)
                self.assertNotIn('late_moved', values, 'кусок дальше нужного не берётся')
                limit = (len(image) + 4 * chunk + 64) * (2 if mode == 'late' else 1)
                self.assertLess(int(values['fed']), limit,
                                'большой кадр перед картинкой не пересылался')
                self.assertEqual(values['decode'], '0')
                self.assertTrue(all(index == 15 for row in self.thumb(values) for index in row),
                                'белый — самый яркий белый WC (15)')
        wants = [int(value) for value in self.run_cover(content)['wants'].split(',')]
        # Заголовок метки и TIT2 — в первом куске; через TXXX — сразу к заголовку
        # APIC, дальше — его тело подряд.
        self.assertEqual(wants[:3], [0, 25 + len(big), 25 + len(big) + 1020])

    def test_v23_utf16_description_and_extended_header(self) -> None:
        image = fake_jpeg(20, 20, lambda x, y: (0, 0, 0))
        body = apic(image, encoding=1, description='﻿Cover'.encode('utf-16-le'))
        content = tag(frame(b'APIC', body, version=3), version=3,
                      extended=struct.pack('>I', 6) + bytes(6), flags=0x40)
        values = self.run_cover(content)
        self.assert_image(values, image)
        self.assertTrue(all(index == 8 for row in self.thumb(values) for index in row),
                        'чёрный — цвет 8: цвет 0 у спрайта прозрачен')
        # 2.4: размер расширенного заголовка семибитный и включает себя.
        content = tag(frame(b'APIC', apic(image)), extended=syncsafe(6) + bytes([1, 0]),
                      flags=0x40)
        self.assert_image(self.run_cover(content), image)

    def test_png_and_packed_pictures_are_skipped(self) -> None:
        image = fake_jpeg(16, 16, lambda x, y: (255, 0, 0))
        content = tag(frame(b'APIC', apic(png(), mime=b'image/png')) +
                      frame(b'APIC', apic(image, description=b'x'), flags=0x08) +   # сжата
                      frame(b'APIC', apic(image)))
        values = self.run_cover(content)
        self.assert_image(values, image)
        self.assertTrue(all(index == 10 for row in self.thumb(values) for index in row),
                        'чистый красный WC (24/31) остаётся собой')
        values = self.run_cover(tag(frame(b'APIC', apic(png(), mime=b'image/png'))))
        self.assertEqual((values['result'], values['reason']), ('2', 'the picture is PNG'))

    def test_data_length_indicator(self) -> None:
        image = fake_jpeg(8, 8, lambda x, y: (0, 197, 197))
        body = apic(image)
        content = tag(frame(b'APIC', syncsafe(len(body)) + body, flags=0x01))
        values = self.run_cover(content)
        self.assert_image(values, image)

    def tags(self, values: dict[str, str]) -> dict[str, str]:
        return {name: bytes.fromhex(values[name]).decode('cp866')
                for name in ('title', 'artist', 'album', 'year', 'track')}

    def test_tags_before_and_after_the_picture(self) -> None:
        image = fake_jpeg(8, 8, lambda x, y: (0, 0, 0))
        content = tag(frame(b'TIT2', b'\x03' + 'Песня №1 — ёлка'.encode('utf-8')) +
                      frame(b'TPE1', b'\x01' + 'Группа'.encode('utf-16')) +
                      frame(b'APIC', apic(image)) +
                      frame(b'TALB', b'\x00' + 'Альбом  '.encode('cp1251')) +
                      frame(b'TDRC', b'\x03' + b'1999-05-01') +
                      frame(b'TRCK', b'\x00' + b'3/12'))
        values = self.run_cover(content)
        self.assert_image(values, image)
        self.assertEqual(self.tags(values), {
            'title': 'Песня ?1 ? ёлка', 'artist': 'Группа', 'album': 'Альбом',
            'year': '1999', 'track': '3/12'})

    def test_tags_without_a_picture_and_utf16_big_endian(self) -> None:
        content = tag(frame(b'TIT2', b'\x02' + 'Track'.encode('utf-16-be'), version=3) +
                      frame(b'TYER', b'\x00' + b'2001', version=3) +
                      frame(b'TPE1', b'\x00' + b'A' * 400, version=3), version=3)
        values = self.run_cover(content)
        self.assertEqual((values['result'], values['reason']), ('2', 'no picture in the tag'))
        tags = self.tags(values)
        self.assertEqual((tags['title'], tags['year']), ('Track', '2001'))
        self.assertEqual(tags['artist'], 'A' * 63, 'длинный кадр обрезан по полю')

    def test_refusals(self) -> None:
        image = fake_jpeg(8, 8, lambda x, y: (1, 2, 3))
        cases = (
            (tag(frame(b'TIT2', b'\x00x')), 'no picture in the tag'),
            (b'ID3\x02\x00\x00' + syncsafe(100) + bytes(100), 'ID3v2 version is not 2.3 or 2.4'),
            (b'ID3\x03\x00\x80' + syncsafe(100) + bytes(100), 'unsynchronised or broken tag'),
            (tag(frame(b'APIC', apic(image)))[:60], 'the file ends inside the tag'),
            (tag(frame(b'APIC', apic(image)), padding=0)[:-1] + b'', 'the file ends inside the tag'),
        )
        for content, reason in cases:
            with self.subTest(reason=reason):
                values = self.run_cover(content)
                self.assertEqual((values['result'], values['reason']), ('2', reason))

    def test_thumb_takes_the_middle_square(self) -> None:
        # 100×60: середина — x 20..79; левая половина картинки (x < 50) чёрная.
        image = fake_jpeg(100, 60, lambda x, y: (0, 0, 0) if x < 50 else (255, 255, 255))
        rows = self.thumb(self.run_cover(tag(frame(b'APIC', apic(image)))))
        for row in rows:
            self.assertEqual(set(row[:30]), {8})
            self.assertEqual(set(row[34:]), {15})

    def test_small_picture_is_stretched(self) -> None:
        image = fake_jpeg(8, 8, lambda x, y: (0, 0, 255) if y < 4 else (0, 255, 0))
        rows = self.thumb(self.run_cover(tag(frame(b'APIC', apic(image)))))
        self.assertEqual({index for row in rows[:31] for index in row}, {9}, 'синий WC')
        self.assertEqual({index for row in rows[33:] for index in row}, {12}, 'зелёный WC')

    def test_gradient_is_dithered_without_transparent_points(self) -> None:
        image = fake_jpeg(128, 128, lambda x, y: (x * 2, y * 2, (x + y) & 255))
        rows = self.thumb(self.run_cover(tag(frame(b'APIC', apic(image)))))
        used = {index for row in rows for index in row}
        self.assertNotIn(0, used, 'цвет 0 у спрайта прозрачен')
        self.assertGreaterEqual(len(used), 8, 'плавный переход — смесь многих цветов')


if __name__ == '__main__':
    unittest.main()
