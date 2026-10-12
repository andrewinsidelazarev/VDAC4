"""Трекер (components/tracker) на ПК: загрузка MOD, звук, конец песни, перемотка,
эффекты XM.

Компонент собирается MSVC с заглушками ESP-IDF (tests/stubs_tracker) вместе с
tests/tracker_host.cpp: синтетический модуль M.K. из двух паттернов играет до
конца песни так же, как его играет AudioPlayer. Эффекты XM — на однo-канальных
XM, собранных стендом: глиссандо (E3y) ведёт тон-портаменто полутонами, без
него — плавно; Y (панбрелло OpenMPT) качает панораму, а Y и Z не меняют
скорость и темп (их коды внутри трекера заняты командами S3M); Zxx —
резонансный фильтр IT (низкий срез глушит обертоны, резонанс не уводит в
бесконечность, Z7F без резонанса — без фильтра). Без Visual Studio Build Tools
тесты пропускаются.
"""
import math
import pathlib
import subprocess
import unittest

from sound_paths import FIRMWARE, ROOT
from test_weather_parse import find_vcvars, short

BUILD = ROOT / '.test-build' / 'tracker_host'
SOURCES = [
    ('tests/tracker_host.cpp', 'tracker_host.obj'),
    (FIRMWARE / 'components/tracker/tracker.cpp', 'tracker.obj'),
]
# Компонент tracker — в прошивке (Firmware/components/tracker).
INCLUDES = ['tests/stubs_tracker', FIRMWARE / 'components/tracker/include',
            FIRMWARE / 'components/tracker']


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
                 ' /Fe:tracker_host.exe')
    script = BUILD / 'build_host.bat'
    script.write_text('\r\n'.join(lines) + '\r\n', encoding='ascii')
    result = subprocess.run(['cmd.exe', '/c', short(script)], capture_output=True,
                            cwd=short(BUILD))
    if result.returncode != 0:
        output = (result.stdout.decode('cp866', 'replace') +
                  result.stderr.decode('cp866', 'replace'))
        raise AssertionError(f'сборка tracker_host.exe не удалась:\n{output}')
    return BUILD / 'tracker_host.exe'


class TrackerHostTest(unittest.TestCase):
    values: dict[str, str]

    @classmethod
    def setUpClass(cls) -> None:
        exe = build()
        if exe is None:
            raise unittest.SkipTest('нет MSVC (vcvars64.bat): стенд трекера пропущен')
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        cls.values = {}
        for line in result.stdout.splitlines():
            key, _, value = line.partition(' ')
            cls.values[key] = value
        cls.returncode = result.returncode

    def test_module_loads(self) -> None:
        self.assertEqual(self.returncode, 0, self.values)
        self.assertEqual(self.values['sane'], '1')
        self.assertGreaterEqual(int(self.values['result']), 0)
        self.assertEqual(self.values['name'], 'vdac4 test tune')
        self.assertEqual(self.values['length'], '2')
        self.assertEqual(self.values['channels'], '4')

    def test_song_plays_once_and_ends(self) -> None:
        # Две позиции по 64 строки, темп 6 тиков, 125 BPM (тик — 20 мс):
        # 128 * 6 * 0,02 = 15,36 с. Конец — по счётчику повторов.
        frames = int(self.values['frames'])
        self.assertAlmostEqual(frames / 44100, 15.36, delta=0.1)
        # Номер позиции libxm меняет на последней строке паттерна, за
        # несколько тиков до конца строки.
        self.assertAlmostEqual(int(self.values['second']) / 44100, 7.68, delta=0.15)
        self.assertGreater(float(self.values['rms']), 0.001, 'звук есть')
        self.assertLessEqual(float(self.values['peak']), 1.5)

    def test_scan_finds_the_length(self) -> None:
        # Прогон без звука: те же 15,36 с; вторая позиция — с 7,68 с; песня
        # после прогона всё равно играет целиком с начала (test_song_plays_once).
        self.assertAlmostEqual(int(self.values['scanned']) / 1000, 15.36, delta=0.05)
        self.assertAlmostEqual(int(self.values['order1']) / 1000, 7.68, delta=0.05)
        self.assertAlmostEqual(int(self.values['frames']) / 44100, 15.36, delta=0.1)

    def test_seek_moves_to_the_position(self) -> None:
        index, row = self.values['seek'].split()
        self.assertEqual(index, '1')
        self.assertLessEqual(int(row), 1)

    def test_junk_is_not_a_module(self) -> None:
        self.assertEqual(self.values['junk'], '0')

    def semitones(self, key: str) -> list[float]:
        """Частоты по тикам — в полутонах от первой (C-4)."""
        values = [float(value) for value in self.values[key].split()]
        return [12 * math.log2(value / values[0]) for value in values]

    def test_glissando_moves_by_semitones(self) -> None:
        notes = self.semitones('gliss_on_frequency')
        self.assertAlmostEqual(notes[-1], 12, delta=0.01, msg='дошло до C-5')
        self.assertTrue(all(abs(note - round(note)) < 0.01 for note in notes),
                        f'только полутона: {notes}')
        self.assertGreaterEqual(len({round(note) for note in notes}), 10, 'шаг за шагом')

    def test_portamento_without_glissando_slides(self) -> None:
        notes = self.semitones('gliss_off_frequency')
        self.assertAlmostEqual(notes[-1], 12, delta=0.01)
        self.assertTrue(any(abs(note - round(note)) > 0.2 for note in notes),
                        f'между полутонами: {notes}')

    def test_y_is_panbrello_and_z_does_nothing(self) -> None:
        self.assertEqual(self.values['ymz_speed'], '125 6', 'ни скорость, ни темп не сменились')
        panning = [float(value) for value in self.values['ymz_panning'].split()]
        self.assertGreater(max(panning) - min(panning), 0.05, 'панорама качается')

    def test_z_is_the_resonant_filter(self) -> None:
        level = {}
        for name in ('filter_off', 'filter_low', 'filter_resonant', 'filter_open'):
            rms, finite = self.values[name].split()
            self.assertEqual(finite, '1', f'{name}: конечные числа')
            level[name] = float(rms)
        self.assertGreater(level['filter_off'], 0.05, 'звук есть')
        self.assertLess(level['filter_low'], level['filter_off'] * 0.8, 'низкий срез глушит обертоны')
        self.assertGreater(level['filter_resonant'], 0.0)
        self.assertAlmostEqual(level['filter_open'], level['filter_off'], delta=1e-4,
                               msg='Z7F без резонанса — фильтра нет')


if __name__ == '__main__':
    unittest.main()
