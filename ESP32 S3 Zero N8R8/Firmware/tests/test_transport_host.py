"""Кадры Z80Transport на ПК: дополнение до чётной длины на канале SPI.

Настоящий src/z80_transport.cpp с src/protocol.cpp собирается MSVC вместе с
tests/transport_host.cpp: кадр уходит в поддельный канал. На канале SPI
(padsFrames) кадр нечётной длины дополняется нулём, и место в очереди
просится сразу с ним; на канале без дополнения кадр прежний. Без Visual Studio Build Tools
тесты пропускаются.
"""
import pathlib
import subprocess
import unittest

from test_weather_parse import ROOT, find_vcvars, short

BUILD = ROOT / '.test-build' / 'transport_host'
SOURCES = [
    ('tests/transport_host.cpp', 'transport_host.obj'),
    ('src/z80_transport.cpp', 'z80_transport.obj'),
    ('src/protocol.cpp', 'protocol.obj'),
]
INCLUDES = ['tests/stubs_host', 'include']


def build() -> pathlib.Path | None:
    vcvars = find_vcvars()
    if vcvars is None:
        return None
    BUILD.mkdir(parents=True, exist_ok=True)
    flags = ('/nologo /c /EHsc /std:c++17 /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS '
             '/DZIFI_HOST_BUILD /D_WINDOWS /FIzifi_msvc_prelude.h ' +
             ' '.join(f'/I "{short(ROOT / path)}"' for path in INCLUDES))
    lines = ['@echo off', f'call "{short(vcvars)}" >nul 2>nul || exit /b 1']
    lines += [f'cl {flags} "{short(ROOT / source)}" /Fo:{obj} || exit /b 1'
              for source, obj in SOURCES]
    lines.append('cl /nologo ' + ' '.join(obj for _, obj in SOURCES) +
                 ' /Fe:transport_host.exe')
    script = BUILD / 'build_host.bat'
    script.write_text('\r\n'.join(lines) + '\r\n', encoding='ascii')
    result = subprocess.run(['cmd.exe', '/c', short(script)], capture_output=True,
                            cwd=short(BUILD))
    if result.returncode != 0:
        output = (result.stdout.decode('cp866', 'replace') +
                  result.stderr.decode('cp866', 'replace'))
        raise AssertionError(f'сборка transport_host.exe не удалась:\n{output}')
    return BUILD / 'transport_host.exe'


def frame(command: int, payload: bytes) -> bytes:
    check = command ^ (len(payload) & 0xFF) ^ (len(payload) >> 8)
    for byte in payload:
        check ^= byte
    return bytes([0x5A, command, len(payload) & 0xFF, len(payload) >> 8]) + payload + bytes([check])


class TransportPaddingTest(unittest.TestCase):
    exe: pathlib.Path

    @classmethod
    def setUpClass(cls) -> None:
        exe = build()
        if exe is None:
            raise unittest.SkipTest('нет MSVC (vcvars64.bat): стенд транспорта пропущен')
        cls.exe = exe

    def send(self, pads: bool, length: int) -> tuple[bool, int, bytes]:
        result = subprocess.run([str(self.exe), '1' if pads else '0', str(length)],
                                capture_output=True, text=True, check=True)
        sent, reserved, data = result.stdout.split()
        return sent == '1', int(reserved), bytes.fromhex(data)

    def test_spi_frames_have_even_length(self) -> None:
        for length in (0, 1, 2, 3, 54, 1015, 1016, 1023, 1024):
            payload = bytes((i * 7 + 1) & 0xFF for i in range(length))
            expected = frame(0x42, payload)
            with self.subTest(length=length):
                sent, reserved, data = self.send(True, length)
                self.assertTrue(sent)
                self.assertEqual(len(data) % 2, 0, 'кадр с дополнением — чётной длины')
                self.assertEqual(data[:len(expected)], expected, 'сам кадр прежний')
                self.assertEqual(data[len(expected):], bytes(len(expected) & 1), 'дополнение — ноль')
                self.assertEqual(reserved, len(data), 'место в очереди — под кадр с дополнением')

    def test_unpadded_frames_stay_as_they_were(self) -> None:
        for length in (0, 1, 1024):
            payload = bytes((i * 7 + 1) & 0xFF for i in range(length))
            with self.subTest(length=length):
                sent, reserved, data = self.send(False, length)
                self.assertTrue(sent)
                self.assertEqual(data, frame(0x42, payload))
                self.assertEqual(reserved, len(data))


if __name__ == '__main__':
    unittest.main()
