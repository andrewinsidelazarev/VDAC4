"""Загрузчик XM компонента tracker на ПК: файлы, на которых падала прошивка.

tests/tracker_files_host.cpp грузит файл так же, как AudioPlayer::startModule,
играет его и сообщает, сколько 16-битных отсчётов легло с нечётного адреса.
На x86 такие обращения проходят, а ESP32-S3 на них падает (LoadStoreAlignment):
XM вешал прошивку. Проверяется:
- 16-битный отсчёт после 8-битного нечётной длины — с чётного адреса;
- 16-битный отсчёт нечётной длины в байтах (FT2 такое пишет) не сдвигает
  разбор следующих инструментов: модуль грузится, а не отвергается;
- если есть каталог E:\\zx\\Music\\XM_1 — первые его файлы грузятся и ни один
  16-битный отсчёт не лежит с нечётного адреса.
Без Visual Studio Build Tools тесты пропускаются.
"""
import pathlib
import struct
import subprocess
import tempfile
import unittest

from sound_paths import FIRMWARE, ROOT
from test_weather_parse import find_vcvars, short

BUILD = ROOT / '.test-build' / 'tracker_files_host'
SOURCES = [
    ('tests/tracker_files_host.cpp', 'tracker_files_host.obj'),
    (FIRMWARE / 'components/tracker/tracker.cpp', 'tracker.obj'),
]
# Компонент tracker — в прошивке (Firmware/components/tracker).
INCLUDES = ['tests/stubs_tracker', FIRMWARE / 'components/tracker/include',
            FIRMWARE / 'components/tracker']
MUSIC = pathlib.Path(r'E:\zx\Music\XM_1')


def build() -> pathlib.Path | None:
    vcvars = find_vcvars()
    if vcvars is None:
        return None
    BUILD.mkdir(parents=True, exist_ok=True)
    flags = ('/nologo /c /EHsc /std:c++17 /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS '
             '/D_USE_MATH_DEFINES /w ' +
             ' '.join(f'/I "{short(ROOT / path)}"' for path in INCLUDES))
    lines = ['@echo off', f'call "{short(vcvars)}" >nul 2>nul || exit /b 1']
    lines += [f'cl {flags} "{short(ROOT / source)}" /Fo:{obj} || exit /b 1'
              for source, obj in SOURCES]
    lines.append('cl /nologo ' + ' '.join(obj for _, obj in SOURCES) +
                 ' /Fe:tracker_files_host.exe')
    script = BUILD / 'build_host.bat'
    script.write_text('\r\n'.join(lines) + '\r\n', encoding='ascii')
    result = subprocess.run(['cmd.exe', '/c', short(script)], capture_output=True,
                            cwd=short(BUILD))
    if result.returncode != 0:
        output = (result.stdout.decode('cp866', 'replace') +
                  result.stderr.decode('cp866', 'replace'))
        raise AssertionError(f'сборка tracker_files_host.exe не удалась:\n{output}')
    return BUILD / 'tracker_files_host.exe'


def instrument(samples: list[tuple[bytes, bool]]) -> bytes:
    """Инструмент XM: [(байты отсчёта, 16 бит)]. Заголовок 263 байта."""
    if not samples:
        return struct.pack('<I', 29) + bytes(25)
    header = bytearray(263)
    struct.pack_into('<I', header, 0, 263)
    struct.pack_into('<H', header, 27, len(samples))
    struct.pack_into('<I', header, 29, 40)
    body = bytearray(header)
    for data, sixteen in samples:
        sample = bytearray(40)
        struct.pack_into('<III', sample, 0, len(data), 0, 0)
        sample[12] = 64
        sample[14] = 0x10 if sixteen else 0
        sample[15] = 128
        body += sample
    for data, _ in samples:
        body += data
    return bytes(body)


def make_xm(instruments: list[bytes], channels: int = 2) -> bytes:
    header = bytearray(b'Extended Module: ' + b'vdac4 test'.ljust(20) + b'\x1a' +
                       b'FastTracker v2.00   ' + b'\x04\x01')
    module = bytearray(276)
    struct.pack_into('<IHHHHHHHH', module, 0, 276, 1, 0, channels, 1, len(instruments), 1, 6, 125)
    pattern = struct.pack('<IBHH', 9, 0, 64, 0)          # 64 пустые строки
    return bytes(header + module + pattern + b''.join(instruments))


class TrackerFilesHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.exe = build()
        if cls.exe is None:
            raise unittest.SkipTest('нет MSVC (vcvars64.bat): стенд трекера пропущен')
        cls.temp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temp.cleanup()

    def load(self, *paths: str, seconds: float = 0.2) -> list[list[str]]:
        result = subprocess.run([str(self.exe), str(seconds), *paths], capture_output=True,
                                text=True, timeout=600)
        self.assertEqual(result.returncode, 0, result.stderr)
        return [line.split('\t') for line in result.stdout.splitlines()]

    def test_16_bit_sample_after_odd_8_bit_one_is_aligned(self) -> None:
        xm = make_xm([instrument([(bytes([1, 2, 3]), False), (bytes(range(64)), True)]),
                      instrument([(bytes(7), False)]),
                      instrument([(bytes(32), True)])])
        path = pathlib.Path(self.temp.name) / 'aligned.xm'
        path.write_bytes(xm)
        [row] = self.load(str(path))
        self.assertEqual(row[1], 'xm')
        self.assertGreaterEqual(int(row[2]), 0, 'модуль загружен')
        self.assertEqual(row[8], '0', '16-битные отсчёты — с чётного адреса')

    def test_odd_length_16_bit_sample_does_not_shift_the_rest(self) -> None:
        xm = make_xm([instrument([(bytes(range(5)), True)]),     # 5 байт: 2 отсчёта и ещё байт
                      instrument([(bytes(9), False)]),
                      instrument([])])
        path = pathlib.Path(self.temp.name) / 'odd16.xm'
        path.write_bytes(xm)
        [row] = self.load(str(path))
        self.assertGreaterEqual(int(row[2]), 0, 'разбор не съехал: модуль загружен')
        self.assertEqual((row[3], row[4]), ('2', '1'), 'два канала, одна позиция')

    def test_envelope_with_too_many_points_is_switched_off(self) -> None:
        """Точек огибающей больше 12 (FT2 больше не бывает) и номера sustain/петли вне
        огибающей: прежде запись шла за массив points, теперь огибающая выключается."""
        instr = bytearray(instrument([(bytes(range(64)), False)]))
        instr[225] = 200                                   # точек громкости
        instr[226] = 13                                    # точек панорамы
        instr[227], instr[228], instr[229] = 150, 3, 250   # sustain и петля громкости
        instr[233] = 0x07                                  # огибающая, sustain, петля
        instr[234] = 0x07
        path = pathlib.Path(self.temp.name) / 'envelope.xm'
        path.write_bytes(make_xm([bytes(instr)]))
        [row] = self.load(str(path), seconds=1)
        self.assertGreaterEqual(int(row[2]), 0, 'модуль загружен и играет')

    def test_song_of_256_positions_loads(self) -> None:
        """Таблица порядка из 256 позиций: восьмибитный счётчик проверки прежде
        оборачивался, и загрузка не кончалась (сторожевой таймер на ESP); длина
        больше 256 — урезается до таблицы."""
        for length in (256, 1000):
            with self.subTest(length=length):
                xm = bytearray(make_xm([instrument([(bytes(range(64)), False)])]))
                struct.pack_into('<H', xm, 64, length)
                path = pathlib.Path(self.temp.name) / f'positions{length}.xm'
                path.write_bytes(bytes(xm))
                result = subprocess.run([str(self.exe), '0.1', str(path)], capture_output=True,
                                        text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stderr)
                row = result.stdout.splitlines()[0].split('	')
                self.assertGreaterEqual(int(row[2]), 0, 'загружен')
                self.assertEqual(row[4], '256', 'позиций — не больше таблицы')

    def test_real_modules(self) -> None:
        if not MUSIC.is_dir():
            self.skipTest(f'нет {MUSIC}')
        files = sorted(str(path) for path in MUSIC.rglob('*.xm'))[:40]
        rows = self.load(*files, seconds=0.1)
        self.assertEqual(len(rows), len(files))
        for row in rows:
            with self.subTest(file=row[0]):
                self.assertGreaterEqual(int(row[2]), 0, 'загружен')
                self.assertEqual(row[8], '0', 'нет 16-битных отсчётов с нечётного адреса')


if __name__ == '__main__':
    unittest.main()
