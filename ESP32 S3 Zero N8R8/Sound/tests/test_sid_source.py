"""SID.ELF на ПК (clang, cRSID целиком): синтетический PSID — программа 6502 включает треугольник
голоса 1 с частотой из таблицы по номеру подмелодии. Тон и его частота, метки заголовка, длина по
умолчанию (3 минуты), длины из SIDLEN.BIN (tools/make_sidlen.py), конец основной подмелодии, перемотка по времени вперёд и назад."""
import hashlib
import math
import pathlib
import struct
import subprocess
import sys
import unittest
import wave

from sound_paths import FIRMWARE, ROOT

sys.path.insert(0, str(ROOT / 'tools'))
import make_sidlen  # noqa: E402

CLANG = pathlib.Path(r'E:\zx\llvm\bin\clang.exe')
BUILD = ROOT / '.test-build' / 'sid_source'
PAL_CLOCK = 985248
RATE = 44100


def sid_register(hz: float) -> int:
    return round(hz * (1 << 24) / PAL_CLOCK)


def make_sid(freqs=(440.0, 660.0), start=1, title=b'Test Tone', author=b'VDAC4',
             released=b'1987 Test Group', model=1) -> bytes:
    """PSID v2: загрузка с $1000, init — $1006 (A — номер подмелодии с нуля), play — $1003 (RTS), таблица частот — с $1030."""
    regs = [sid_register(f) for f in freqs]
    lo, hi = 0x1030, 0x1030 + len(freqs)          # таблица частот — за кодом
    code = bytes([0x4C, 0x06, 0x10,               # $1000 JMP init
                  0x60, 0x00, 0x00,               # $1003 play: RTS
                  0xAA,                           # $1006 TAX
                  0xA9, 0x0F, 0x8D, 0x18, 0xD4,   # LDA #$0F, STA $D418 — громкость
                  0xBD, lo & 0xFF, lo >> 8, 0x8D, 0x00, 0xD4,   # частота голоса 1, младший байт
                  0xBD, hi & 0xFF, hi >> 8, 0x8D, 0x01, 0xD4,   # ... старший
                  0xA9, 0x00, 0x8D, 0x05, 0xD4,   # AD = 0
                  0xA9, 0xF0, 0x8D, 0x06, 0xD4,   # SR: держать 15
                  0xA9, 0x11, 0x8D, 0x04, 0xD4,   # треугольник, gate
                  0x60])
    assert len(code) <= 0x30, len(code)
    code = code.ljust(0x30, b'\xEA')
    code += bytes(r & 0xFF for r in regs) + bytes(r >> 8 for r in regs)
    header = b'PSID' + struct.pack('>HHHHHHHI', 2, 0x7C, 0x1000, 0x1000, 0x1003, len(freqs), start, 0)
    header += title.ljust(32, b'\0') + author.ljust(32, b'\0') + released.ljust(32, b'\0')
    header += struct.pack('>HBBBB', 0x0004 | model << 4, 0, 0, 0, 0)
    assert len(header) == 0x7C
    return header + code


def base_for(data: bytes, times: str) -> bytes:
    md5 = hashlib.md5(data).hexdigest()
    return make_sidlen.build(make_sidlen.parse(f'[Database]\n; /TEST/Test.sid\n{md5}={times}\n'))


def build() -> pathlib.Path | None:
    if not CLANG.exists():
        return None
    BUILD.mkdir(parents=True, exist_ok=True)
    exe = BUILD / 'sid_host.exe'
    result = subprocess.run(
        [str(CLANG), '-std=gnu11', '-O2', '-w', '-D_CRT_SECURE_NO_WARNINGS',
         '-I', str(ROOT / 'tests' / 'stubs_sid'), '-I', str(ROOT / 'components' / 'crsid'),
         '-I', str(FIRMWARE / 'modules' / 'sdk' / 'include'),
         str(ROOT / 'tests' / 'sid_host.c'), '-o', str(exe)],
        capture_output=True, text=True)
    if result.returncode != 0:
        raise AssertionError('сборка sid_host.exe не удалась:\n' + result.stdout + result.stderr)
    return exe


class SidSourceTest(unittest.TestCase):
    exe: pathlib.Path

    @classmethod
    def setUpClass(cls) -> None:
        exe = build()
        if exe is None:
            raise unittest.SkipTest('нужен clang (E:\\zx\\llvm)')
        cls.exe = exe

    def run_host(self, data: bytes, name: str, **keys) -> dict[str, str]:
        path = BUILD / name
        path.write_bytes(data)
        args = [str(self.exe), str(path)]
        for key, value in keys.items():
            args += [f'--{key}', str(value)]
        result = subprocess.run(args, capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        values = {}
        for line in result.stdout.splitlines():
            if '=' in line and not line.startswith('log='):
                key, value = line.split('=', 1)
                values[key] = value
        values['log'] = '\n'.join(line for line in result.stdout.splitlines() if line.startswith('log='))
        self.assertEqual(values['entry'], '1')
        self.assertEqual(values['open'], '1', result.stdout)
        return values

    def tone(self, wav: pathlib.Path) -> float:
        """Частота по переходам левого канала через ноль (без первой половины секунды)."""
        with wave.open(str(wav)) as w:
            frames = w.readframes(w.getnframes())
        left = struct.unpack(f'<{len(frames) // 2}h', frames)[0::2][RATE // 2:]
        mean = sum(left) / len(left)
        crossings = sum(1 for a, b in zip(left, left[1:]) if (a - mean) < 0 <= (b - mean))
        return crossings * RATE / len(left)

    def test_tone_tags_and_default_length(self):
        r = self.run_host(make_sid(author=b'Chris H\xfclsbeck'), 'tone.sid', seconds=2,
                          wav=BUILD / 'tone.wav')
        self.assertEqual(r['probe'], '100')
        self.assertEqual(r['format'], 'SID 6581 PSID')
        self.assertEqual((r['title'], r['artist'], r['album'], r['year']),
                         ('Test Tone', 'Chris Hulsbeck', 'Test Group', '1987'), 'Latin-1 — без диакритики')
        self.assertEqual((r['song'], r['songs'], r['voices']), ('1', '2', '3'))
        self.assertEqual(r['duration_ms'], '180000', 'нет базы — 3 минуты')
        self.assertEqual(r['ended'], '0')
        self.assertGreater(float(r['rms']), 0.02)
        self.assertEqual(float(r['stereo_diff']), 0.0, 'одна SID — моно')
        self.assertGreater(int(r['max_level']), 200, 'огибающая голоса 1 — в уровнях')
        self.assertAlmostEqual(self.tone(BUILD / 'tone.wav'), 440.0, delta=4.4)
        self.assertIn('default 3 min', r['log'])

    def test_normalization_is_postprocessing_and_seek_preserves_gain(self):
        data = make_sid()
        raw = self.run_host(data, 'level.sid', seconds=20, normalize=0)
        level = self.run_host(data, 'level.sid', seconds=20, normalize=1)
        self.assertEqual(raw['level_frames'], '0')
        self.assertEqual(level['level_frames'], level['frames'])
        self.assertGreater(float(level['rms']), float(raw['rms']))
        self.assertLessEqual(float(level['peak']), 1.0)
        self.assertGreater(float(level['level_gain']), 1.0)
        # Максимальная скорость усиления — 0,5 дБ/с, включая начальный прогрев.
        self.assertLessEqual(float(level['level_gain']), 10.1)
        for delta in (-10000, 10000):
            with self.subTest(delta=delta):
                r = self.run_host(data, 'level_seek.sid', before=20, seek=delta, seconds=0.1)
                self.assertEqual(r['gain_before_seek'], r['gain_after_seek'])
                self.assertEqual(r['blocks_before_seek'], r['blocks_after_seek'])
                self.assertGreater(float(r['gain_after_seek']), 1.0)
                # Пропущенные секунды не попадают в измеритель громкости.
                self.assertEqual(int(r['level_frames']), 20 * RATE + int(r['frames']))

    def test_lengths_from_base_and_end(self):
        data = make_sid()
        base = BUILD / 'SIDLEN.BIN'
        base.write_bytes(base_for(data, '0:02 0:03.500'))
        r = self.run_host(data, 'base.sid', base=base)
        self.assertEqual(r['duration_ms'], '2000')
        self.assertEqual((r['ended'], r['frames']), ('1', str(2 * RATE)), 'конец подмелодии — по базе')
        self.assertIn('from SIDLEN.BIN', r['log'])
        data = make_sid(start=2)
        base.write_bytes(base_for(data, '0:02 0:03.500'))
        r = self.run_host(data, 'base.sid', base=base, wav=BUILD / 'song2.wav')
        self.assertEqual((r['song'], r['duration_ms']), ('2', '4000'), 'доли секунды — вверх')
        self.assertEqual(r['frames'], str(4 * RATE))
        self.assertAlmostEqual(self.tone(BUILD / 'song2.wav'), 660.0, delta=6.6)

    def test_other_tune_not_in_base(self):
        base = BUILD / 'SIDLEN_other.BIN'
        base.write_bytes(base_for(make_sid(freqs=(330.0,)), '0:01'))
        r = self.run_host(make_sid(), 'other.sid', base=base, seconds=0.1)
        self.assertEqual(r['duration_ms'], '180000')

    def test_time_seek_keeps_default_song_and_clamps(self):
        data = make_sid(start=2)
        for before, delta, position in ((0, 10000, 10000), (12, -10000, 2000),
                                        (2, -10000, 0), (0, 2147483647, 180000)):
            with self.subTest(before=before, delta=delta):
                r = self.run_host(data, 'seek.sid', before=before, seek=delta, seconds=1,
                                  wav=BUILD / 'seek.wav')
                self.assertEqual(r['song'], '2')
                self.assertEqual((r['order'], r['order_length']), ('0', '0'))
                self.assertEqual(int(r['seek_position_ms']), position)
                if position == 180000:
                    self.assertEqual((r['ended'], r['frames'], r['seek_frames']), ('1', '0', '0'))
                else:
                    self.assertGreaterEqual(int(r['position_ms']), position + 1000)
                    self.assertAlmostEqual(self.tone(BUILD / 'seek.wav'), 660, delta=7)

    def test_seek_near_end_plays_only_the_remainder(self):
        data = make_sid(start=2)
        base = BUILD / 'seek_lengths.bin'
        base.write_bytes(base_for(data, '0:03 0:12'))
        r = self.run_host(data, 'end.sid', base=base, seek=10000)
        self.assertEqual((r['duration_ms'], r['ended'], r['frames']), ('12000', '1', str(2 * RATE)))

    def test_seek_yields_without_sound_and_can_be_changed_or_closed(self):
        data = make_sid()
        r = self.run_host(data, 'slice.sid', seek=10000, seconds=0.1, load=2000)
        self.assertEqual(r['seek_frames'], str(10 * RATE))
        self.assertEqual(r['seek_sound_frames'], '0')
        self.assertGreater(int(r['prep_calls']), 100)
        r = self.run_host(data, 'repeat.sid', seek=20000, seconds=0.1,
                          **{'repeat-seek': -10000})
        self.assertEqual((r['pending_result'], r['pending_frames'], r['pending_seek']), ('1', '0', '1'))
        self.assertEqual(r['seek_position_ms'], '10000')
        r = self.run_host(data, 'close.sid', seek=10000, **{'close-pending': 1})
        self.assertEqual(r['pending_frames'], '0')
        self.assertLess(float(r['close_ms']), 100)

    def test_not_sid(self):
        path = BUILD / 'junk.sid'
        path.write_bytes(b'NOTSID' + bytes(200))
        result = subprocess.run([str(self.exe), str(path)], capture_output=True, text=True, timeout=60)
        self.assertIn('probe=0', result.stdout)
        self.assertIn('open=0', result.stdout)

    def test_overload_does_not_end_song(self):
        # Время ESP здесь искусственное: проверка только отсутствия обрыва по нагрузке.
        for load in (1, 2000):
            r = self.run_host(make_sid(), 'load.sid', seconds=10, load=load)
            self.assertEqual(r['ended'], '0')
            self.assertGreaterEqual(int(r['frames']), 10 * RATE)
            self.assertNotIn('does not keep up', r['log'])

    def test_long_init_yields_and_seek_to_end_cancels_it(self):
        # Подмелодия 1 зацикливается в init, подмелодия 2 сразу возвращается.
        data = make_sid()
        code = bytes([0xC9, 0, 0xD0, 3, 0x4C, 4, 0x10, 0x60])
        data = data[:0x7C] + code + data[0x7C + len(code):]
        r = self.run_host(data, 'long_init.sid', seconds=1, **{'cancel-init': 1})
        self.assertEqual(r['init_result'], '1')
        self.assertEqual(r['init_frames'], '0')
        self.assertLessEqual(int(r['init_steps']), 4096)
        self.assertEqual(r['song'], '1')
        self.assertEqual((r['ended'], r['frames']), ('1', '0'))

    def pcm(self, path: pathlib.Path) -> bytes:
        with wave.open(str(path)) as w:
            return w.readframes(w.getnframes())

    def test_two_cores_bit_exact_with_crsid(self):
        # SID.ELF 1.1: такты C64 (ядро 0) и звуковая часть по снимкам (ядро 1 — поток-помощник, как у
        # PLAYER), свои волны и огибающие пакетом — бит в бит то же, что cRSID одним потоком
        # (cRSID_generateSample), и на одном «ядре», и на двух. Мелодии — тестовые автора cRSID: ADSR,
        # ошибка задержки, комбинированные волны, фильтр, шум; часть читает OSC3 и ENV3.
        data = ROOT / 'tests' / 'data' / 'sid'
        tunes = sorted(data.glob('*.sid'))
        self.assertEqual(len(tunes), 17)
        for tune in tunes:
            with self.subTest(tune=tune.name):
                waves = {}
                for mode, keys in (('ref', {'reference': 1}), ('one', {'worker': 0}), ('two', {'worker': 1})):
                    wav = BUILD / f'exact_{mode}.wav'
                    self.run_host(tune.read_bytes(), 'exact.sid', seconds=3, wav=wav, normalize=0, **keys)
                    waves[mode] = self.pcm(wav)
                n = min(len(w) for w in waves.values())
                self.assertGreater(n, 3 * RATE * 4 - 4096 * 4)
                self.assertEqual(waves['one'][:n], waves['ref'][:n], 'одно ядро')
                self.assertEqual(waves['two'][:n], waves['ref'][:n], 'два ядра')


if __name__ == '__main__':
    unittest.main()
