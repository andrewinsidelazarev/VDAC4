"""VIDEO.ELF (приложение ESP ядра видео VDAC4) на ПК: test/video_host.cpp — заглушка ядра ESP,
модель Tang с ядром видео (темп vp_pace, FIFO звука с эпохой, оверлеи fb_ovl), модель ЦАП и
сценарий плагина FTVIEW4; приложение и MP3.ELF (Helix) — те же исходники, что в ELF. Сборка MSVC
x86 (Helix на MSVC — только x86), как у стенда PLAYER (Sound/tests/test_player_host.py).

Ролики — AVI с «пиками» звука: 1 кГц по 40 мс в начале каждой секунды ролика, между ними тишина.
Пик, зазвучавший на ЦАП, сравнивается с выходным кадром, с которого виден кадр той же секунды:
расхождение — ошибка синхронности. Проверяется:
- PCM 16 бит моно 32 кГц, 8 бит стерео 22,05 кГц, 16 бит стерео 44,1 кГц, MP3 стерео 128 кбит/с;
- пауза 1,5 с (бит паузы ядра и VS_PAUSED): ЦАП стоит, после — синхронно;
- перемотка (сброс потока, новая эпоха звука): звук с нового места, синхронно;
- звук в файле на 1 с раньше кадров (копится в кольце) и на 4 с (кольцо 512 КБ переполнено:
  самое старое выбрасывается, картинка не встаёт), звук на 10 кадров позже кадров;
- MP3.ELF нет: картинка идёт, звука нет (состояние 3);
- ядро загружено (core_load), EXIT — назад в слот 0, задачи и модули закрыты;
- панель, окно списка, сообщение — снимки вывода в E:\\zx\\cache\\tmp\\video_host\\shots_*.
Без Visual Studio Build Tools или ffmpeg (MP3) тесты пропускаются.

    E:\\zx\\esp\\pio-venv\\Scripts\\python.exe -m unittest test_video_host -v
"""
import array
import math
import pathlib
import struct
import subprocess
import sys
import unittest
import wave

HERE = pathlib.Path(__file__).resolve().parent
APP = HERE.parent / 'main'
VDAC4 = HERE.parents[4]
FIRMWARE = VDAC4 / 'ESP32 S3 Zero N8R8' / 'Firmware'
SOUND = VDAC4 / 'ESP32 S3 Zero N8R8' / 'Sound'
HELIX = FIRMWARE / 'managed_components' / 'chmorgan__esp-libhelix-mp3' / 'libhelix-mp3'
HELIX_SOURCES = ['mp3dec.c', 'mp3tabs.c'] + [
    f'real/{name}.c' for name in ('bitstream', 'buffers', 'dct32', 'dequant', 'dqchan', 'huffman',
                                  'hufftabs', 'imdct', 'polyphase', 'scalfact', 'stproc',
                                  'subband', 'trigtabs')]
BUILD = pathlib.Path(r'E:\zx\cache\tmp\video_host')
FFMPEG = pathlib.Path(r'C:\Users\andre\AppData\Local\FTViewConvert\ffmpeg-9.0.2.exe')

sys.path.append(str(FIRMWARE / 'tests'))
from test_weather_parse import find_vcvars, short  # noqa: E402

US_FRAME = 40000
BEEP_MS = 40


def build() -> pathlib.Path | None:
    vcvars = find_vcvars()
    if vcvars is None:
        return None
    vcvars = vcvars.with_name('vcvars32.bat')
    BUILD.mkdir(parents=True, exist_ok=True)
    includes = [SOUND / 'tests' / 'stubs_player', FIRMWARE / 'modules' / 'sdk' / 'include', APP,
                SOUND / 'player_ui' / 'fonts']
    common = ('/nologo /c /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS ' +
              ' '.join(f'/I "{short(path)}"' for path in includes))
    c11 = f'{common} /std:c11 /FI zm_compat.h /DVIDEO_APP_HOST_TEST /DVIDEO_AUDIO_TRACE /I "{short(HELIX / "pub")}"'
    cpp = f'{common} /EHsc /std:c++17 /DVIDEO_APP_HOST_TEST'
    units = [
        (cpp, HERE / 'video_host.cpp', 'host.obj'),
        (c11, APP / 'app.c', 'app.obj'),
        (c11, APP / 'audio.c', 'audio.obj'),
        (c11, APP / 'avi_info.c', 'avi_info.obj'),
        (cpp, APP / 'hud.cpp', 'hud.obj'),
        (cpp, SOUND / 'player_ui' / 'fonts' / 'pui_font.cpp', 'pui_font.obj'),
        (f'{c11} /Dmain=mp3_main', SOUND / 'modules' / 'mp3' / 'main' / 'mp3_source.c', 'mp3_source.obj'),
    ]
    helix = f'{common} /w /I "{short(HELIX / "pub")}" /I "{short(HELIX / "real")}"'
    units += [(helix, HELIX / name, 'helix_' + pathlib.Path(name).stem + '.obj') for name in HELIX_SOURCES]
    lines = ['@echo off', f'call "{short(vcvars)}" >nul 2>nul || exit /b 1']
    lines += [f'cl {flags} "{short(source)}" /Fo:{obj} || exit /b 1' for flags, source, obj in units]
    lines.append('cl /nologo ' + ' '.join(obj for _, _, obj in units) + ' winmm.lib /Fe:video_host.exe')
    script = BUILD / 'build_host.bat'
    script.write_text('\r\n'.join(lines) + '\r\n', encoding='ascii')
    result = subprocess.run(['cmd.exe', '/c', short(script)], capture_output=True, cwd=short(BUILD))
    if result.returncode != 0:
        output = result.stdout.decode('cp866', 'replace') + result.stderr.decode('cp866', 'replace')
        raise AssertionError(f'сборка video_host.exe не удалась:\n{output}')
    return BUILD / 'video_host.exe'


# --- звук с пиками -------------------------------------------------------------------------------------
def beeps(seconds: float, rate: int, channels: int) -> list[list[float]]:
    """Каналы: пик 1 кГц BEEP_MS мс в начале каждой секунды с первой (в правом канале — 1,5 кГц)."""
    n = int(seconds * rate)
    out = [[0.0] * n for _ in range(channels)]
    for s in range(1, int(seconds) + 1):
        start = s * rate
        for i in range(start, min(n, start + rate * BEEP_MS // 1000)):
            for c in range(channels):
                out[c][i] = 0.5 * math.sin(2 * math.pi * (1000 + 500 * c) * (i - start) / rate)
    return out


def pcm_bytes(chans: list[list[float]], bits: int) -> bytes:
    n = len(chans[0])
    if bits == 8:
        return bytes(max(0, min(255, int(128 + 127 * chans[c][i]))) for i in range(n) for c in range(len(chans)))
    a = array.array('h', (int(32767 * chans[c][i]) for i in range(n) for c in range(len(chans))))
    return a.tobytes()


def wfx(tag: int, channels: int, rate: int, bits: int, avg: int, align: int) -> bytes:
    return struct.pack('<HHIIHHH', tag, channels, rate, avg, align, bits, 0)


def mp3_bytes(seconds: float, rate: int = 44100) -> bytes:
    path = BUILD / 'media' / f'beeps_{seconds:g}.mp3'
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        src = path.with_suffix('.wav')
        with wave.open(str(src), 'wb') as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(pcm_bytes(beeps(seconds, rate, 2), 16))
        r = subprocess.run([str(FFMPEG), '-hide_banner', '-loglevel', 'error', '-y', '-i', str(src),
                            '-c:a', 'libmp3lame', '-b:a', '128k', '-write_xing', '0', '-id3v2_version', '0',
                            str(path)], capture_output=True)
        if r.returncode:
            raise AssertionError('ffmpeg: ' + r.stderr.decode('utf-8', 'replace'))
    return path.read_bytes()


# --- AVI -----------------------------------------------------------------------------------------------------
def chunk(fcc: bytes, data: bytes) -> bytes:
    return fcc + struct.pack('<I', len(data)) + data + (b'\0' if len(data) & 1 else b'')


def lst(kind: bytes, body: bytes) -> bytes:
    return b'LIST' + struct.pack('<I', len(body) + 4) + kind + body


def make_avi(path: pathlib.Path, frames: int, fmt: bytes | None, audio: bytes, skew: int = 0) -> None:
    """Кадры — пустышки по 64 байта; звук поровну на кадр; skew > 0 — звук на skew кадров раньше
    своих кадров в файле, < 0 — позже."""
    avih = struct.pack('<IIIIIIIIII', US_FRAME, 0, 0, 0x10, frames, 0, 2 if fmt else 1, 0, 512, 384) + bytes(16)
    vstrh = b'vids' + b'MJPG' + struct.pack('<IHHIIIIIIIIhhhh', 0, 0, 0, 0, 1, 25, 0, frames, 0, 0, 0, 0, 0, 512, 384)
    vstrf = struct.pack('<IiiHHIIiiII', 40, 512, 384, 1, 24, 0x47504A4D, 512 * 384 * 3, 0, 0, 0, 0)
    hdrl = chunk(b'avih', avih) + lst(b'strl', chunk(b'strh', vstrh) + chunk(b'strf', vstrf))
    parts: list[bytes] = []
    if fmt:
        rate = struct.unpack_from('<I', fmt, 4)[0]
        astrh = b'auds' + bytes(4) + struct.pack('<IHHIIIIIIIIhhhh', 0, 0, 0, 0, 1, rate, 0, 0, 0, 0, 0, 0, 0, 0, 0)
        hdrl += lst(b'strl', chunk(b'strh', astrh) + chunk(b'strf', fmt))
        align = struct.unpack_from('<H', fmt, 12)[0] or 1
        per = len(audio) / frames
        cuts = [min(len(audio), int(round(i * per / align)) * align) for i in range(frames + 1)]
        cuts[-1] = len(audio)
        a_chunks = [audio[cuts[i]:cuts[i + 1]] for i in range(frames)]
    else:
        a_chunks = []
    video = [chunk(b'00dc', bytes([0xFF, 0xD8]) + bytes(60) + bytes([0xFF, 0xD9])) for _ in range(frames)]
    # порядок в movi: аудиочанк i стоит перед кадром i − skew
    order: list[tuple[float, int, bytes]] = []
    for i, v in enumerate(video):
        order.append((i, 1, v))
    for i, a in enumerate(a_chunks):
        order.append((i - skew - 0.5, 0, chunk(b'01wb', a)))
    order.sort(key=lambda t: (t[0], t[1]))
    movi = b''.join(p for _, _, p in order)
    body = b'AVI ' + lst(b'hdrl', hdrl) + lst(b'movi', movi)
    path.write_bytes(b'RIFF' + struct.pack('<I', len(body)) + body)


def run(avi: pathlib.Path, *args: str, timeout: int = 180) -> dict[str, str]:
    r = subprocess.run([str(BUILD / 'video_host.exe'), str(avi), *args], capture_output=True, timeout=timeout)
    out = r.stdout.decode('utf-8', 'replace')
    log = r.stderr.decode('utf-8', 'replace')
    (BUILD / (avi.stem + '.log')).write_text(out + '\n' + log, encoding='utf-8')
    if r.returncode != 0:
        raise AssertionError(f'video_host {avi.name}: код {r.returncode}\n{out}\n{log[-3000:]}')
    return dict(line.split('=', 1) for line in out.splitlines() if '=' in line)


def offsets(res: dict[str, str]) -> list[tuple[int, float]]:
    return [(int(a), float(b)) for a, b in (x.split(':') for x in res['beeps'].split(',') if x)]


def to_png(directory: pathlib.Path) -> None:
    try:
        from PIL import Image
    except ImportError:
        return
    for ppm in directory.glob('*.ppm'):
        Image.open(ppm).save(ppm.with_suffix('.png'))
        ppm.unlink()


class VideoHostTest(unittest.TestCase):
    exe: pathlib.Path | None = None

    @classmethod
    def setUpClass(cls):
        cls.exe = build()

    def setUp(self):
        if self.exe is None:
            self.skipTest('нет Visual Studio Build Tools')

    def common(self, res: dict[str, str], rate: int, ch: int, audio: bool = True):
        self.assertEqual(res['state_loaded'], '2')
        self.assertEqual(res['state_exit'], '3')
        self.assertEqual(res['core_loads'], '1')
        self.assertEqual(res['core_backs'], '1')
        self.assertEqual(res['tasks_left'], '0')
        self.assertEqual(res['modules_open'], '0')
        self.assertEqual(res['audio_bytes_skipped'], '0')   # ядро не пропускает звук: приём включён всегда
        if audio:
            self.assertEqual(res['audio_state_max'], '2')
            self.assertEqual((int(res['pcm_rate']), int(res['pcm_ch'])), (rate, ch))
            # ЦАП не пустеет посреди звука (после конца ролика — можно)
            end = float(res['stream_end'])
            gaps = [u for u in map(float, filter(None, res['pcm_underruns'].split(','))) if u < end - 250]   # хвост: звук кончается чуть раньше последнего кадра
            self.assertEqual(gaps, [], f'ЦАП опустел посреди звука: {res["pcm_underruns"]}')

    def check_sync(self, res, first: int, last: int, lo: float, hi: float, skip=()):
        got = dict(offsets(res))
        for s in range(first, last + 1):
            if s in skip:
                continue
            self.assertIn(s, got, f'пик {s} с не прозвучал: {res["beeps"]}')
            self.assertGreaterEqual(got[s], lo, f'пик {s} с раньше кадра: {res["beeps"]}')
            self.assertLessEqual(got[s], hi, f'пик {s} с позже кадра: {res["beeps"]}')

    def test_pcm16_mono_pause_seek(self):
        rate, frames = 32000, 300
        audio = pcm_bytes(beeps(frames * US_FRAME / 1e6, rate, 1), 16)
        avi = BUILD / 'pcm16_mono.avi'
        make_avi(avi, frames, wfx(1, 1, rate, 16, rate * 2, 2), audio)
        shots = BUILD / 'shots_pcm16'
        shots.mkdir(exist_ok=True)
        res = run(avi, '--pause-at', '4000', '--pause-ms', '1500', '--seek-at', '6500', '--seek-frame', '250',
                  '--shots', str(shots))
        to_png(shots)
        self.common(res, rate, 1)
        # перемотка с ~5 с ролика на 10 с: пропущены кадры до 249, остальные показаны
        missing = res['frames_missing'].split(',')
        self.assertEqual(len(missing), 1, res['frames_missing'])
        self.assertTrue(missing[0].endswith('-249'), res['frames_missing'])
        lo_skip = (int(missing[0].split("-")[0]) * US_FRAME // 1000 - 200) // 1000 + 1   # пик у места перемотки — неясен
        self.check_sync(res, 1, 11, -40, 40, skip=[4, *range(lo_skip, 11)])   # пик 4 — на месте паузы

    def test_pcm8_stereo_16x9(self):
        rate, frames = 22050, 200
        audio = pcm_bytes(beeps(frames * US_FRAME / 1e6, rate, 2), 8)
        avi = BUILD / 'pcm8_stereo.avi'
        make_avi(avi, frames, wfx(1, 2, rate, 8, rate * 2, 2), audio)
        shots = BUILD / 'shots_pcm8'
        shots.mkdir(exist_ok=True)
        res = run(avi, '--cfg', str(1 << 14 | 1 << 15), '--shots', str(shots))     # 16:9, 95 %
        to_png(shots)
        self.common(res, rate, 2)
        self.assertEqual(res['frames_missing'], '')
        self.check_sync(res, 1, 7, -40, 40)

    def test_pcm16_stereo_audio_ahead_1s(self):
        rate, frames = 44100, 250
        audio = pcm_bytes(beeps(frames * US_FRAME / 1e6, rate, 2), 16)
        avi = BUILD / 'pcm16_ahead1.avi'
        make_avi(avi, frames, wfx(1, 2, rate, 16, rate * 4, 4), audio, skew=25)
        res = run(avi)
        self.common(res, rate, 2)
        self.assertEqual(res['frames_missing'], '')
        self.check_sync(res, 1, 9, -40, 40)

    def test_pcm16_stereo_audio_ahead_4s(self):
        """Кольцо 2 МБ (12 с такого звука) держит перекос 4 с целиком."""
        rate, frames = 44100, 300
        audio = pcm_bytes(beeps(frames * US_FRAME / 1e6, rate, 2), 16)
        avi = BUILD / 'pcm16_ahead4.avi'
        make_avi(avi, frames, wfx(1, 2, rate, 16, rate * 4, 4), audio, skew=100)
        res = run(avi)
        self.common(res, rate, 2)
        self.assertEqual(res['frames_missing'], '')
        self.check_sync(res, 1, 11, -40, 40)

    def test_pcm16_stereo_ring_overflow(self):
        """PSRAM дала только 512 КБ (3 с), перекос 4 с: картинка не встаёт, часть звука теряется,
        а то, что звучит, — синхронно (с паузами тишины на месте выброшенного)."""
        rate, frames = 44100, 300
        audio = pcm_bytes(beeps(frames * US_FRAME / 1e6, rate, 2), 16)
        avi = BUILD / 'pcm16_ahead4.avi'
        make_avi(avi, frames, wfx(1, 2, rate, 16, rate * 4, 4), audio, skew=100)
        res = run(avi, '--psram-max', str(512 * 1024))
        self.assertEqual(res['state_exit'], '3')
        self.assertEqual(res['frames_missing'], '', 'картинка встала из-за звука')
        got = offsets(res)
        self.assertGreaterEqual(len(got), 3, res['beeps'])
        for s, off in got:
            self.assertTrue(-40 <= off <= 40, f'пик {s} с: {off} мс ({res["beeps"]})')

    def test_pcm16_audio_one_frame_behind(self):
        """Звук на кадр позже своих кадров в файле — в пределах опережения разбора: синхронно."""
        rate, frames = 32000, 200
        audio = pcm_bytes(beeps(frames * US_FRAME / 1e6, rate, 1), 16)
        avi = BUILD / 'pcm16_behind1.avi'
        make_avi(avi, frames, wfx(1, 1, rate, 16, rate * 2, 2), audio, skew=-1)
        res = run(avi)
        self.common(res, rate, 1)
        self.assertEqual(res['frames_missing'], '')
        self.check_sync(res, 1, 7, -40, 40)

    def test_pcm16_audio_far_behind(self):
        """Звук на 10 кадров позже своих кадров: догнать нечем (приходит в реальном времени) —
        играет с опозданием, но целиком, без дыр."""
        rate, frames = 32000, 200
        audio = pcm_bytes(beeps(frames * US_FRAME / 1e6, rate, 1), 16)
        avi = BUILD / 'pcm16_behind.avi'
        make_avi(avi, frames, wfx(1, 1, rate, 16, rate * 2, 2), audio, skew=-10)
        res = run(avi)
        self.common(res, rate, 1)
        self.assertEqual(res['frames_missing'], '')
        self.check_sync(res, 1, 7, 250, 450)

    def test_mp3_stereo_pause_seek(self):
        if not FFMPEG.exists():
            self.skipTest('нет ffmpeg')
        frames = 300
        mp3 = mp3_bytes(frames * US_FRAME / 1e6)
        avi = BUILD / 'mp3_stereo.avi'
        make_avi(avi, frames, wfx(0x55, 2, 44100, 0, 16000, 1), mp3)
        res = run(avi, '--pause-at', '3000', '--pause-ms', '1500', '--seek-at', '6000', '--seek-frame', '225')
        self.common(res, 44100, 2)
        missing = res['frames_missing'].split(',')
        self.assertEqual(len(missing), 1, res['frames_missing'])
        lo_skip = (int(missing[0].split("-")[0]) * US_FRAME // 1000 - 200) // 1000 + 1   # пик у места перемотки — неясен
        # MP3: задержка кодера и декодера (~1600 отсчётов, ~37 мс) лежит в самом файле
        self.check_sync(res, 1, 11, -40, 80, skip=[3, *range(lo_skip, 10)])   # пик 3 — на месте паузы

    def test_mp3_without_decoder(self):
        if not FFMPEG.exists():
            self.skipTest('нет ffmpeg')
        frames = 150
        mp3 = mp3_bytes(frames * US_FRAME / 1e6)
        avi = BUILD / 'mp3_nodec.avi'
        make_avi(avi, frames, wfx(0x55, 2, 44100, 0, 16000, 1), mp3)
        res = run(avi, '--no-mp3', '1')
        self.common(res, 0, 0, audio=False)
        self.assertEqual(res['audio_state_max'], '3')
        self.assertEqual(res['frames_missing'], '')
        self.assertNotEqual(res['audio_bytes_in'], '0')   # ядро звук отдаёт, ESP его выбрасывает


if __name__ == '__main__':
    unittest.main()
