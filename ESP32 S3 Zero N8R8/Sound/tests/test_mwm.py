"""Проигрыватель MoonBlaster Wave (modules/opl/main/mwm.cpp) против оригинала.

Каждая песня MWM из ZXM-MoonSound (E:\\zx\\esp\\downloads\\zxm-moonsound,
202 песни, наборы POWERKIT и TALL_ONE) играется дважды: оригинальным
Z80-проигрывателем в эмуляторе Z80 (mwm_reference.py) и портом
(tests/mwm_check.cpp). Журналы записей в регистры OPL4 по тикам должны
совпасть байт в байт до конца песни. Кроме песен без набора («NONE»), которые
всё же зовут свои волны: оригинал ищет их части за концом пустой таблицы — в
памяти индикаторов и указателях патчей, — и это не повторить (у порта эти
волны молчат).

Образ ОЗУ OPL4 из набора (mwm::load_kit) сверяется с образами, которые
сделаны для демо-дисков ZXM-MoonSound (powerkit.bin, tall_one.bin): заголовки
тонов со звуком и сам звук.

Без репозитория ZXM-MoonSound, sjasmplus или Visual Studio — пропуск.
"""
import concurrent.futures
import pathlib
import subprocess
import unittest

import mwm_reference
from sound_paths import ROOT
from test_weather_parse import build_exe

DISKS = mwm_reference.SOURCES.parent
BUILD = ROOT / '.test-build' / 'mwm'


def build() -> pathlib.Path | None:
    main = ROOT / 'modules' / 'opl' / 'main'
    return build_exe(BUILD, 'mwm_check.exe', [main],
                     [(main / 'mwm.cpp', 'mwm.obj'), (ROOT / 'tests' / 'mwm_check.cpp', 'check.obj')])


def kit_of(song: pathlib.Path) -> pathlib.Path | None:
    """Набор из папки песни по имени из песни (байты 276..283)."""
    name = song.read_bytes()[276:284].decode('latin-1').strip().upper()
    for item in song.parent.iterdir():
        if item.suffix.upper() == '.MWK' and item.stem.upper() == name:
            return item
    return None


def own_waves(song: bytes) -> bool:
    """Песня зовёт свои волны набора (номера волн от 176)."""
    info = song[6:6 + 220]
    return any(number >= 176 for number in info[124:172])


@unittest.skipUnless(mwm_reference.available(), 'нет ZXM-MoonSound или sjasmplus')
class MwmTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.exe = build()
        if cls.exe is None:
            raise unittest.SkipTest('нет Visual Studio Build Tools')
        cls.code, cls.symbols = mwm_reference.assemble()

    def port(self, song: pathlib.Path, kit: pathlib.Path | None) -> list[str]:
        result = subprocess.run([str(self.exe), str(song), str(kit) if kit else '-'],
                                capture_output=True, check=True)
        return result.stdout.decode('ascii').replace('\r\n', '\n').rstrip('\n').split('\n')

    def compare(self, song: pathlib.Path) -> str | None:
        kit = kit_of(song)
        data = song.read_bytes()
        if kit is None and own_waves(data):
            return None
        expected = mwm_reference.journal(self.code, self.symbols, data,
                                         kit.read_bytes() if kit else None)
        got = self.port(song, kit)
        for index, (want, have) in enumerate(zip(expected, got)):
            if want != have:
                return f'{song.parent.name}/{song.name}: тик {index - 1}:\n  Z80  {want[:300]}\n  порт {have[:300]}'
        if len(expected) != len(got):
            return f'{song.parent.name}/{song.name}: {len(got)} строк вместо {len(expected)}'
        self.assertTrue(expected[-1].startswith('END'), f'{song.name}: {expected[-1]}')
        return ''

    def test_all_songs_match_the_original(self) -> None:
        songs = sorted(p for p in DISKS.rglob('*') if p.suffix.upper() == '.MWM')
        self.assertGreater(len(songs), 0)
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            verdicts = list(pool.map(self.compare, songs))
        failures = [v for v in verdicts if v]
        compared = sum(1 for v in verdicts if v is not None)
        self.assertEqual(failures, [], f'{len(failures)} из {compared} песен расходятся')
        self.assertGreaterEqual(compared, len(songs) - 5)

    def test_kit_image_matches_the_demo_disks(self) -> None:
        for kit_path, image_path in ((DISKS / 'moonmusic_01' / 'POWERKIT.MWK', DISKS / 'moonmusic_01' / 'powerkit.bin'),
                                     (DISKS / 'moonsound_14' / 'TALL_ONE.MWK', DISKS / 'moonsound_14' / 'tall_one.bin')):
            if not kit_path.exists() or not image_path.exists():
                continue
            out = BUILD / (kit_path.stem + '.img')
            subprocess.run([str(self.exe), '--image', str(kit_path), str(out)], check=True)
            ours = out.read_bytes()
            theirs = image_path.read_bytes()
            kit = kit_path.read_bytes()
            total = kit[6] | kit[7] << 8 | kit[8] << 16
            self.assertEqual(len(ours), 0x300 + total + 4)
            tones = [k for k in range(64) if kit[10 + k] & 1]
            for k in tones:
                self.assertEqual(ours[12 * k:12 * k + 12], theirs[12 * k:12 * k + 12], f'{kit_path.name}: тон {k}')
            self.assertEqual(ours[0x300:0x300 + total], theirs[0x300:0x300 + total], kit_path.name)
            # Тоны без звука — на тишину в конце образа: начало там, конец через отсчёт.
            silent = [k for k in range(64) if not kit[10 + k] & 1]
            address = 0x200000 + 0x300 + total
            for k in silent[:1]:
                header = ours[12 * k:12 * k + 12]
                self.assertEqual(header[:3], bytes([(address >> 16) & 0x3F, (address >> 8) & 0xFF, address & 0xFF]))
                self.assertEqual(header[5:7], b'\xff\xff')
            self.assertEqual(ours[-4:], bytes(4))

    def test_no_kit_image_is_silence(self) -> None:
        out = BUILD / 'none.img'
        subprocess.run([str(self.exe), '--image', '-', str(out)], check=True)
        image = out.read_bytes()
        self.assertEqual(len(image), 0x300 + 4)
        for k in range(64):
            self.assertEqual(image[12 * k:12 * k + 3], b'\x20\x03\x00')


if __name__ == '__main__':
    unittest.main()
