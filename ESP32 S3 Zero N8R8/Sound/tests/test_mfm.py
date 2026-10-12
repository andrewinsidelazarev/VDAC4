"""Проигрыватель MoonBlaster FM (modules/opl/main/mfm.cpp) против оригинала.

Каждая песня MFM из ZXM-MoonSound (E:\\zx\\esp\\downloads\\zxm-moonsound, 38
песен) играется дважды: оригинальным Z80-проигрывателем в эмуляторе Z80
(mfm_reference.py) и портом (tests/mfm_check.cpp). Журналы записей в регистры
OPL4 по тикам — FM (с чтением регистров обратно) и волны — должны совпасть
байт в байт до конца песни или до ухода на повтор.

Без репозитория ZXM-MoonSound, sjasmplus или Visual Studio — пропуск.
"""
import concurrent.futures
import pathlib
import subprocess
import unittest

import mfm_reference
from sound_paths import ROOT
from test_weather_parse import build_exe

DISKS = mfm_reference.SOURCES.parent
BUILD = ROOT / '.test-build' / 'mfm'


def build() -> pathlib.Path | None:
    main = ROOT / 'modules' / 'opl' / 'main'
    return build_exe(BUILD, 'mfm_check.exe', [main],
                     [(main / 'mfm.cpp', 'mfm.obj'), (ROOT / 'tests' / 'mfm_check.cpp', 'check.obj')])


@unittest.skipUnless(mfm_reference.available(), 'нет ZXM-MoonSound или sjasmplus')
class MfmTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.exe = build()
        if cls.exe is None:
            raise unittest.SkipTest('нет Visual Studio Build Tools')
        cls.code, cls.symbols = mfm_reference.assemble()

    def compare(self, song: pathlib.Path) -> str:
        expected = mfm_reference.journal(self.code, self.symbols, song.read_bytes())
        result = subprocess.run([str(self.exe), str(song)], capture_output=True, check=True)
        got = result.stdout.decode('ascii').replace('\r\n', '\n').rstrip('\n').split('\n')
        for index, (want, have) in enumerate(zip(expected, got)):
            if want != have:
                return f'{song.parent.name}/{song.name}: тик {index - 1}:\n  Z80  {want[:300]}\n  порт {have[:300]}'
        if len(expected) != len(got):
            return f'{song.parent.name}/{song.name}: {len(got)} строк вместо {len(expected)}'
        if expected[-1].startswith('LIMIT'):
            return f'{song.name}: ни конца, ни повтора'
        return ''

    def test_all_songs_match_the_original(self) -> None:
        songs = sorted(p for p in DISKS.rglob('*') if p.suffix.upper() == '.MFM')
        self.assertGreater(len(songs), 0)
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            failures = [v for v in pool.map(self.compare, songs) if v]
        self.assertEqual(failures, [], f'{len(failures)} из {len(songs)} песен расходятся')


if __name__ == '__main__':
    unittest.main()
