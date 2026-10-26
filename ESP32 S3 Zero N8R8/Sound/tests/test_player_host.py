"""Приложение PLAYER.ELF с декодерами MP3.ELF, AAC.ELF, OGG.ELF, TRACKER.ELF и MIDI.ELF на ПК.

tests/player_host.cpp — заглушка ядра (zm_core_t) с моделью вывода
AudioPlayer::pcmWrite; приложение и декодеры — те же исходники, что
собираются в ELF (modules/player, modules/mp3, modules/tracker), Helix и
tracker — настоящие. Сборка 32-битная: Helix на MSVC умеет только x86.
Проверяется:
- MP3: метка ID3 и мусор перед первым кадром пропущены, каждый кадр дошёл до
  вывода, формат, битрейт, частота и каналы в записи AUDIO_STATUS; после
  доигрывания — состояние «доиграл»;
- пауза: пока она стоит, вывод не получает ни кадра, а после неё ни один
  кадр не потерян;
- AUDIO_FLUSH (перемотка MP3): время идёт от нового места, файл подаётся оттуда;
- модуль: AUDIO_INFO (название, формат, позиций), AUDIO_SEEK, песня до конца
  один раз; мусор вместо модуля — пропуск: 1,5 с «играет» с причиной вместо
  названия, без звука, потом «доиграл» (плагин уходит к следующему файлу);
- IT идёт потоком (PLAYER подаёт кольцо, TRACKER читает в open): как есть;
  не хватает PSRAM стенда (--psram) — стерео в моно, затем 8 бит; не влезает и
  так — пропуск; стоп посреди приёма не ждёт декодер; не IT в потоке IT —
  целиком, как обычный файл;
- MOD с испорченным тегом при точной длине — как M.K.; обрезанный до первого
  сэмпла — пропуск;
- декодера нет в кэше — AUDIO_OPEN отказывает;
- выгрузка посреди звука: задача остановлена, декодер закрыт, приложение
  ответов вне команд не шлёт; тон (#34) — не команда приложения;
- спектр (view.c): синус поднимает свою полосу до верха шкалы, полосы вдали —
  на нуле; снимки следуют позиции PCM с учётом очереди, паузы и сбросов;
  MIDI-нота после тишины синхронна при очередях 93, 500 и 1486 мс;
  AUDIO_VIEW у модуля — уровни каналов (по числу каналов),
  AUDIO_TAGS — название модуля;
- обложка MP3: метка досылается с того места, что просит приложение, большой
  кадр перед картинкой не пересылается, миниатюра собирается из четырёх частей,
  название — «исполнитель - название» из меток в CP866; без картинки — отказ,
  а название есть;
- если есть E:\\zx\\Music\\XM_1 — настоящие XM играют;
- MIDI (если есть банк E:\\zx\\esp\\soundfonts\\TimGM6mb.sf2): песня звучит без
  перегрузки до конца и не дольше хвоста отпускания, название из Track Name,
  каналы, инструменты и дорожки, вид — спектр, длительность известна;
  перемотка на 10 с — время с нового места; без банка — отказ без зависания;
- OPL (OPL.ELF): VGM с метками GD3 — формат 7, название, автор, игра, год,
  микросхема за метками, уровни каналов с нотами, длительность, звук до конца;
  VGZ — то же, испорченный gzip — отказ; DRO 2 — длительность из заголовка;
  IMF — только по расширению из AUDIO_OPEN, 560 Гц, у WLF — 700; перемотка
  на 10 с; VGM для OPL4 без ПЗУ YRW801.ROM — флаг 16 в AUDIO_STATUS, с ним —
  нет; AUDIO_OPEN [#20] освобождает банк MIDI и ПЗУ;
  эффекты по выходному звуку: реверберация (CC91) тянет хвост, эхо (CC94)
  повторяет удар, хорус (CC93) расходит каналы, портаменто (CC65, CC5) ведёт
  высоту от прежней ноты, колесо модуляции (CC1) качает высоту.
- AAC (AAC.ELF) и Ogg Vorbis (OGG.ELF) с карты — файлы делает ffmpeg (нет его —
  тесты пропускаются): звук до конца, частота, каналы, длительность, метки
  Vorbis; моно, 5.1 — в стерео; Opus — отказ с надписью; комментарий в 200 КБ
  (обложка) — буферы растут; перемотка (AUDIO_FLUSH) — с сохранённых
  заголовков; декодера нет в кэше — AUDIO_OPEN отказывает; записи настоящих
  станций из E:\\zx\\esp\\samples (если есть: HE-AAC, Ogg) — без ошибок;
- радио: кодек станции по началу потока (Content-Type врёт — всё равно по
  байтам), AAC и Ogg; у Ogg песня — из комментариев Vorbis, цепочка потоков
  меняет её (флаг меток); Ogg Opus и нет AAC.ELF — отказ с надписью.
Без Visual Studio Build Tools тесты пропускаются.
"""
import array
import gzip
import hashlib
import pathlib
import random
import re
import shutil
import struct
import subprocess
import tempfile
import unittest

from test_tracker_files_host import instrument, make_xm
from sound_paths import FIRMWARE, ROOT
from test_weather_parse import find_vcvars, short

BUILD = ROOT / '.test-build' / 'player_host'
HELIX = FIRMWARE / 'managed_components' / 'chmorgan__esp-libhelix-mp3' / 'libhelix-mp3'
HELIX_SOURCES = ['mp3dec.c', 'mp3tabs.c'] + [
    f'real/{name}.c' for name in ('bitstream', 'buffers', 'dct32', 'dequant', 'dqchan', 'huffman',
                                  'hufftabs', 'imdct', 'polyphase', 'scalfact', 'stproc',
                                  'subband', 'trigtabs')]
MUSIC = pathlib.Path(r'E:\zx\Music\XM_1')
SAMPLES = pathlib.Path(r'E:\zx\esp\samples')          # записи настоящих станций (не в git)
FFMPEG = next((path for path in (shutil.which('ffmpeg'),
                                 r'C:\Users\andre\AppData\Local\FTViewConvert\ffmpeg-9.0.2.exe')
               if path and pathlib.Path(path).exists()), None)
BANK = pathlib.Path(r'E:\zx\esp\soundfonts\TimGM6mb.sf2')
FORMAT_MIDI = 6
FRAME_BYTES, FRAME_SAMPLES, RATE = 417, 1152, 44100
# TSFM.ELF выводит песни из одних AY на 96 кГц (гармоники AY выше 20 кГц, как у
# настоящего TSFM), а песни с YM2203 (TFC, TFD, MTC) — на 44,1 кГц (с 1.5).
TSFM_RATE = 96000
TSFM_FM_RATE = 44100
STATE_IDLE, STATE_PLAYING = 0, 1
FORMAT_MP3, FORMAT_XM = 1, 3


def build() -> pathlib.Path | None:
    vcvars = find_vcvars()
    if vcvars is None:
        return None
    vcvars = vcvars.with_name('vcvars32.bat')
    BUILD.mkdir(parents=True, exist_ok=True)
    # Пути от ROOT (Sound); заголовки ядра и компонент tracker — из прошивки (абсолютные пути).
    includes = ['tests/stubs_player', 'tests/stubs_tracker', FIRMWARE / 'modules/sdk/include',
                FIRMWARE / 'components/tracker/include', FIRMWARE / 'components/tracker',
                FIRMWARE / 'include', 'modules/player/main', 'components/tsf/include', 'player_ui']
    common = ('/nologo /c /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /D_USE_MATH_DEFINES ' +
              ' '.join(f'/I "{short(ROOT / path)}"' for path in includes))
    module = (f'{common} /std:c11 /FI zm_compat.h /I "{short(HELIX / "pub")}" /DLIBXMP_STATIC '
              f'/I "{short(ROOT / "components" / "libxmp" / "include")}" '
              f'/I "{short(ROOT / "components" / "libxmp" / "esp")}"')
    ymfm = (f'{common} /EHsc /std:c++17 /DNDEBUG '
            f'/I "{short(ROOT / "components" / "ymfm" / "include")}"')
    saa = f'{common} /EHsc /std:c++17 /I "{short(ROOT / "components" / "saasound")}"'
    units = [
        (f'{common} /EHsc /std:c++17', 'tests/player_host.cpp', 'host.obj'),
        (f'{module} /Dmain=player_main', 'modules/player/main/player.c', 'player.obj'),
        (module, 'modules/player/main/view.c', 'view.obj'),
        (module, 'modules/player/main/radio_list.c', 'radio_list.obj'),
        (f'{module} /Dmain=mp3_main', 'modules/mp3/main/mp3_source.c', 'mp3_source.obj'),
        (f'{module} /Dmain=aac_main /I "{short(ROOT / "components" / "helix_aac")}"',
         'modules/aac/main/aac_source.c', 'aac_source.obj'),
        (f'{module} /Dmain=ogg_main /I "{short(ROOT / "components" / "stb_vorbis")}"',
         'modules/ogg/main/ogg_source.c', 'ogg_source.obj'),
        (f'{common} /std:c11 /w /DNDEBUG', 'components/stb_vorbis/stb_vorbis_vdac4.c',
         'stb_vorbis.obj'),
        (f'{module} /Dmain=flac_main /I "{short(ROOT / "components" / "foxen_flac")}"',
         'modules/flac/main/flac_source.c', 'flac_source.obj'),
        (f'{common} /std:c11 /DNDEBUG', 'components/foxen_flac/foxen-flac.c', 'foxen_flac.obj'),
        (f'{module} /Dmain=tracker_main', 'modules/tracker/main/tracker_source.c',
         'tracker_source.obj'),
        (module, 'modules/tracker/main/it_stream.c', 'it_stream.obj'),
        (f'{common} /EHsc /std:c++17 /w', FIRMWARE / 'components/tracker/tracker.cpp', 'tracker.obj'),
        (f'{module} /Dmain=midi_main', 'modules/midi/main/midi_source.c', 'midi_source.obj'),
        (f'{common} /std:c11 /w', 'components/tsf/tsf.c', 'tsf.obj'),
        (f'{ymfm} /Dmain=opl_main', 'modules/opl/main/opl_source.cpp', 'opl_source.obj'),
        (f'{ymfm}', 'modules/opl/main/mwm.cpp', 'mwm.obj'),
        (f'{ymfm}', 'modules/opl/main/mfm.cpp', 'mfm.obj'),
        (f'{common} /std:c11 /w', 'modules/opl/main/em_inflate.c', 'em_inflate.obj'),
        (f'{ymfm} /w', 'components/ymfm/src/ymfm_opl.cpp', 'ymfm_opl.obj'),
        (f'{ymfm} /w', 'components/ymfm/src/ymfm_pcm.cpp', 'ymfm_pcm.obj'),
        (f'{ymfm} /w', 'components/ymfm/src/ymfm_adpcm.cpp', 'ymfm_adpcm.obj'),
        (f'{ymfm} /w', 'components/ymfm/src/fast_opl.cpp', 'fast_opl.obj'),
        (f'{common} /EHsc /std:c++17', 'modules/player/main/cover_glue.cpp', 'cover_glue.obj'),
        (module, 'modules/player/main/online_cover.c', 'online_cover.obj'),
        # Экран PLAYER на Tang (screen.cpp, 1.26): логика — PlayerOverlay прошивки, отрисовка — Codex
        (f'{common} /EHsc /std:c++17', 'modules/player/main/screen.cpp', 'screen.obj'),
        (f'{common} /EHsc /std:c++17', FIRMWARE / 'src/player_overlay.cpp', 'player_overlay.obj'),
        (f'{common} /EHsc /std:c++17', 'player_ui/player_ui.cpp', 'player_view.obj'),
        (f'{common} /EHsc /std:c++17', 'player_ui/fonts/pui_font.cpp', 'pui_font.obj'),
        (f'{common} /EHsc /std:c++17', FIRMWARE / 'src/cover_art.cpp', 'cover_art.obj'),
    ]
    units += [(f'{saa} /Dmain=saa_main', 'modules/saa/main/saa_source.cpp', 'saa_source.obj'),
              (saa, 'modules/saa/main/etracker.cpp', 'etracker.obj')]
    units += [(saa, f'components/saasound/{name}.cpp', name + '.obj')
              for name in ('SAAAmp', 'SAADevice', 'SAAEnv', 'SAAFreq', 'SAANoise')]
    tsfm = ymfm + f' /I "{short(ROOT / "modules/tsfm/main/ymfm")}"'
    units += [(f'{tsfm} /Dmain=tsfm_main', 'modules/tsfm/main/tsfm_source.cpp', 'tsfm_source.obj')]
    units += [(tsfm, f'modules/tsfm/main/{name}.cpp', 'tsfm_' + name + '.obj')
              for name in ('ay_players', 'tfc', 'tfd', 'ymfm/ymfm_opn', 'ymfm/ymfm_ssg')]
    units = [(flags, path, obj.replace('tsfm_ymfm/', 'tsfm_')) for flags, path, obj in units]
    units += [(f'{common} /std:c11', 'modules/tsfm/main/ayumi/ayumi.c', 'ayumi.obj')]
    # libxmp полная (components/libxmp): все загрузчики и ProWizard, без распаковщиков.
    xmp = ROOT / 'components' / 'libxmp'
    # Как в модуле ESP: файлы — в памяти (esp/xmp_esp_shim.c, те же имена xmp_*).
    renames = ' '.join(f'/D{name}=xmp_{name}' for name in
                       ('fread', 'fseek', 'ftell', 'fclose', 'feof', 'ferror', 'fgetc', 'fwrite',
                        'fputc', 'fflush', 'fopen', 'malloc', 'calloc', 'realloc', 'free',
                        'getenv'))
    # Память libxmp — в счёт PSRAM стенда (--psram): XMP_ESP_HOST_BUDGET в xmp_esp_shim.c.
    xmp_flags = (f'{common} /std:c11 /w /DLIBXMP_STATIC /DLIBXMP_NO_DEPACKERS /DLIBXMP_ESP_MODULE '
                 f'/DXMP_ESP_HOST_BUDGET {renames} /I "{short(xmp / "include")}" '
                 f'/I "{short(xmp / "esp")}" /I "{short(xmp / "src")}"')
    xmp_sources = [xmp / name for name in (xmp / 'sources.txt').read_text(encoding='ascii').split()]
    xmp_sources.append(xmp / 'esp' / 'xmp_esp_shim.c')
    units += [(xmp_flags, str(source.relative_to(ROOT)), 'xmp_' + source.stem + '.obj')
              for source in xmp_sources]
    # Helix AAC (components/helix_aac): все исходники компонента.
    aac_dir = ROOT / 'components' / 'helix_aac'
    units += [(f'{common} /w /DNDEBUG /I "{short(aac_dir)}"', str(source.relative_to(ROOT)),
               'aac_' + source.stem + '.obj') for source in sorted(aac_dir.glob('*.c'))]
    helix_flags = f'{common} /w /I "{short(HELIX / "pub")}" /I "{short(HELIX / "real")}"'
    units += [(helix_flags, HELIX / source,
               'helix_' + pathlib.Path(source).stem + '.obj') for source in HELIX_SOURCES]
    lines = ['@echo off', f'call "{short(vcvars)}" >nul 2>nul || exit /b 1']
    lines += [f'cl {flags} "{short(ROOT / source)}" /Fo:{obj} || exit /b 1'
              for flags, source, obj in units]
    lines.append('cl /nologo ' + ' '.join(obj for _, _, obj in units) +
                 ' winmm.lib /Fe:player_host.exe')
    script = BUILD / 'build_host.bat'
    script.write_text('\r\n'.join(lines) + '\r\n', encoding='ascii')
    result = subprocess.run(['cmd.exe', '/c', short(script)], capture_output=True,
                            cwd=short(BUILD))
    if result.returncode != 0:
        output = (result.stdout.decode('cp866', 'replace') +
                  result.stderr.decode('cp866', 'replace'))
        raise AssertionError(f'сборка player_host.exe не удалась:\n{output}')
    return BUILD / 'player_host.exe'


def encoded(name: str, *args: str, inputs: tuple[str, ...] = ()) -> pathlib.Path:
    """Файл от ffmpeg (кэш в .test-build/player_host/media по ключам): inputs —
    входы (-f lavfi -i … или файлы), args — фильтры, кодировщик, метки."""
    key = hashlib.sha1('|'.join(inputs + args).encode()).hexdigest()[:10]
    path = BUILD / 'media' / f'{pathlib.Path(name).stem}_{key}{pathlib.Path(name).suffix}'
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        result = subprocess.run([FFMPEG, '-hide_banner', '-loglevel', 'error', '-y', *inputs, *args,
                                 str(path)], capture_output=True)
        if result.returncode != 0:
            raise AssertionError(f'ffmpeg {name}: ' + result.stderr.decode('utf-8', 'replace'))
    return path


def tone(seconds: float, hz: int = 1000, rate: int = 44100) -> tuple[str, ...]:
    return ('-f', 'lavfi', '-i', f'sine=frequency={hz}:sample_rate={rate}:duration={seconds}')


def silent_mp3(frames: int, tag: int = 0, junk: bytes = b'') -> bytes:
    """MPEG-1 Layer III 128 кбит/с 44,1 кГц стерео: побочная информация и
    главные данные нулевые — каждый кадр декодируется в 1152 кадра тишины."""
    head = b''
    if tag:
        size = bytes((tag >> shift) & 0x7F for shift in (21, 14, 7, 0))
        head = b'ID3\x03\x00\x00' + size + bytes(tag)
    frame = bytes([0xFF, 0xFB, 0x90, 0x00]) + bytes(FRAME_BYTES - 4)
    return head + junk + frame * frames


def vbr_mp3(frames: int) -> tuple[bytes, int]:
    """VBR как у LAME: первый кадр — Xing (64 кбит/с, без звука), за ним кадры
    тишины 256 и 320 кбит/с через один. Возвращает файл и средний битрейт."""
    sizes = [144 * kbps * 1000 // RATE for kbps in (256, 320)]
    audio = b''.join(bytes([0xFF, 0xFB, 0xD0 if i % 2 == 0 else 0xE0, 0x00]) +
                     bytes(sizes[i % 2] - 4) for i in range(frames))
    xing_length = 144 * 64000 // RATE
    total = xing_length + len(audio)
    toc = bytes(i * 256 // 100 for i in range(100))
    xing = (bytes([0xFF, 0xFB, 0x50, 0x00]) + bytes(32) + b'Xing' +
            struct.pack('>III', 0x0F, frames, total) + toc + struct.pack('>I', 0))
    xing += bytes(xing_length - len(xing))
    duration_ms = frames * FRAME_SAMPLES * 1000 // RATE
    return xing + audio, total * 8 // duration_ms


def id3_frame(name: bytes, body: bytes) -> bytes:
    size = len(body)
    return name + bytes([(size >> 21) & 0x7F, (size >> 14) & 0x7F, (size >> 7) & 0x7F,
                         size & 0x7F]) + bytes(2) + body


def id3_tag(frames: bytes) -> bytes:
    body = frames + bytes(200)
    size = len(body)
    return b'ID3\x04\x00\x00' + bytes([(size >> 21) & 0x7F, (size >> 14) & 0x7F,
                                        (size >> 7) & 0x7F, size & 0x7F]) + body


def song_xm(orders: int) -> bytes:
    """XM: два канала, один инструмент, позиций orders (все — пустой
    узор в 64 строки), темп 6, 125 BPM: 7,68 с на позицию."""
    xm = bytearray(make_xm([instrument([(bytes(range(64)), False)])]))
    struct.pack_into('<H', xm, 64, orders)
    return bytes(xm)


def song_it(frames: int, stereo: bool = False, title: bytes = b'vdac4 it', count: int = 1) -> bytes:
    """IT 2.14 без инструментов: count одинаковых 16-битных сэмплов (меандр
    220 Гц, у стерео каналы подряд, как пишет IT), один узор в 64 строки — нота
    C-5 сэмпла 1 в канале 1 на первой строке; отсчёты — за узором, как у
    Impulse Tracker."""
    orders = bytes([0, 255])
    header = bytearray(0xC0)
    header[0:4] = b'IMPM'
    header[4:4 + len(title)] = title
    struct.pack_into('<HHHHHHHH', header, 0x20, len(orders), 0, count, 1, 0x0214, 0x0214, 0x09, 0)
    header[0x30:0x35] = bytes([128, 48, 6, 125, 128])   # громкости, скорость 6, темп 125
    header[0x40:0x80] = bytes([32] * 64)                # каналы по центру
    header[0x80:0xC0] = bytes([64] * 64)
    sample_at = 0xC0 + len(orders) + 4 * (count + 1)
    pattern_at = sample_at + 0x50 * count
    rows = b'\x81\x03\x3C\x01\x00' + bytes(63)
    pattern = struct.pack('<HH4x', len(rows), 64) + rows
    wave = array.array('h', [12000 if (i // 50) % 2 else -12000 for i in range(frames)])
    data = wave.tobytes() * (2 if stereo else 1)
    data_at = pattern_at + len(pattern)
    samples = b''
    for index in range(count):
        sample = bytearray(0x50)
        sample[0:4] = b'IMPS'
        sample[0x11:0x14] = bytes([64, 1 | 2 | (4 if stereo else 0), 64])
        sample[0x2E] = 1                                 # отсчёты со знаком
        struct.pack_into('<IIIIIII', sample, 0x30, frames, 0, 0, 22050, 0, 0,
                         data_at + index * len(data))
        samples += bytes(sample)
    pointers = struct.pack(f'<{count + 1}I', *[sample_at + 0x50 * i for i in range(count)], pattern_at)
    return bytes(header) + orders + pointers + samples + pattern + data * count


def song_mod() -> bytes:
    """MOD «M.K.»: один сэмпл (меандр, 8 бит, 4000 отсчётов), один узор — нота
    C-2 в канале 1 на первой строке."""
    header = bytearray(1084)
    header[0:8] = b'vdac4 md'
    struct.pack_into('>H', header, 20 + 22, 2000)       # длина в словах
    header[20 + 25] = 64                                  # громкость
    struct.pack_into('>HH', header, 20 + 26, 0, 1)
    header[950] = 1                                       # длина песни
    header[1080:1084] = b'M.K.'
    pattern = bytearray(64 * 4 * 4)
    pattern[0:4] = bytes([0x01, 0xAC, 0x10, 0x00])        # сэмпл 1, период 428 (C-2)
    data = bytes((40 if (i // 20) % 2 else 216) for i in range(4000))
    return bytes(header) + bytes(pattern) + data


def vlq(value: int) -> bytes:
    out = [value & 0x7F]
    value >>= 7
    while value:
        out.insert(0, 0x80 | (value & 0x7F))
        value >>= 7
    return bytes(out)


def song_midi(seconds: int, title: bytes = b'VDAC4 MIDI test') -> bytes:
    """SMF 1: дорожка 0 — темп 120 и Track Name, дорожка 1 — аккорд из трёх
    нот (фортепиано, канал 0) и бочка (канал ударных 9) каждую секунду.
    Деление 480: секунда — 960 тиков."""
    head = vlq(0) + b'\xFF\x51\x03\x07\xA1\x20' + vlq(0) + b'\xFF\x03' + vlq(len(title)) + title
    track0 = head + vlq(0) + b'\xFF\x2F\x00'
    events = vlq(0) + b'\xC0\x00'
    for second in range(seconds):
        events += vlq(0) + b'\x90\x3C\x64' + vlq(0) + b'\x90\x40\x64' + vlq(0) + b'\x90\x43\x64'
        events += vlq(0) + b'\x99\x24\x7F'
        events += vlq(720) + b'\x80\x3C\x00' + vlq(0) + b'\x80\x40\x00' + vlq(0) + b'\x80\x43\x00'
        events += vlq(0) + b'\x89\x24\x00' + vlq(240) + b'\xB0\x07\x64'
    track1 = events + vlq(0) + b'\xFF\x2F\x00'
    chunk = lambda body: b'MTrk' + struct.pack('>I', len(body)) + body
    return b'MThd' + struct.pack('>IHHH', 6, 1, 2, 480) + chunk(track0) + chunk(track1)


def midi_events(events: list[tuple[int, bytes]]) -> bytes:
    """SMF 0 из событий (тик, байты): деление 480, темп 120 — секунда = 960 тиков."""
    body = vlq(0) + b'\xFF\x51\x03\x07\xA1\x20'
    last = 0
    for tick, data in sorted(events, key=lambda event: event[0]):
        body += vlq(tick - last) + data
        last = tick
    body += vlq(0) + b'\xFF\x2F\x00'
    return b'MThd' + struct.pack('>IHHH', 6, 0, 1, 480) + b'MTrk' + struct.pack('>I', len(body)) + body


def energy(samples, start: float, end: float) -> float:
    part = samples[int(start * RATE):int(end * RATE)]
    return sum(value * value for value in part) / max(1, len(part))


def pitch(samples, start: float, end: float, low: float, high: float) -> float:
    """Основной тон окна автокорреляцией в полосе low..high Гц: наименьший сдвиг
    с корреляцией не ниже 90 % наибольшей (кратные периоды не берутся), доля
    отсчёта — по параболе через соседей."""
    part = samples[int(start * RATE):int(end * RATE)]
    count = len(part)
    scores = {}
    for lag in range(int(RATE / high) - 1, int(RATE / low) + 2):
        scores[lag] = sum(a * b for a, b in zip(part, part[lag:])) / (count - lag)
    best = max(scores.values())
    lags = sorted(scores)
    lag = next(lag for lag in lags[1:-1] if scores[lag] >= 0.9 * best and
               scores[lag] >= scores[lag - 1] and scores[lag] >= scores[lag + 1])
    y0, y1, y2 = scores[lag - 1], scores[lag], scores[lag + 1]
    bend = y0 - 2 * y1 + y2
    return RATE / (lag + (0.5 * (y0 - y2) / bend if bend else 0.0))


OPL_RATE = 49715
FORMAT_OPL = 7


def gd3(*fields: str) -> bytes:
    """GD3: 11 строк UTF-16 (название, игра, система, автор — англ. и япон., дата…)."""
    body = b''.join(text.encode('utf-16-le') + b'\x00\x00' for text in fields)
    return b'Gd3 ' + struct.pack('<II', 0x100, len(body)) + body


def opl2_notes(seconds: float, write) -> list[tuple[int, int]]:
    """Голоса 0 и 1 OPL2: нота каждые 0,5 с (0,4 звучит). write(регистр, значение);
    возвращает [(отсчётов 44100 до следующего, ...)] через write(None, ожидание)."""
    for ch, (tl, fb) in enumerate([(0x10, 0x0E), (0x18, 0x00)]):
        for reg, value in ((0x20, 0x21), (0x23, 0x01), (0x40, tl), (0x43, 0x00), (0x60, 0xF4),
                           (0x63, 0xF4), (0x80, 0x35), (0x83, 0x35), (0xC0, fb)):
            write(reg + ch, value)
    steps = int(seconds * 2)
    for step in range(steps):
        fnum = 0x200 + 40 * (step % 8)
        for ch in range(2):
            write(0xA0 + ch, fnum & 0xFF)
            write(0xB0 + ch, 0x20 | (4 << 2) | (fnum >> 8))
        write(None, 17640)
        for ch in range(2):
            write(0xB0 + ch, (4 << 2) | (fnum >> 8))
        write(None, 4410)


def opl_vgm(seconds: float, tags: bytes = b'', opl4: bool = False) -> bytes:
    """VGM 1.71: OPL2 (или OPL4 — та же FM, команды 0xD0) с нотами opl2_notes."""
    data = bytearray()
    total = 0

    def write(reg, value):
        nonlocal total
        if reg is None:
            data.extend(b'\x61' + struct.pack('<H', value))
            total += value
        elif opl4:
            data.extend(bytes([0xD0, 0, reg, value]))
        else:
            data.extend(bytes([0x5A, reg, value]))

    if opl4:
        data.extend(bytes([0xD0, 1, 0x05, 0x03]))
    opl2_notes(seconds, write)
    data.extend(b'\x66')
    head = bytearray(0x100)
    head[0:4] = b'Vgm '
    struct.pack_into('<I', head, 0x08, 0x171)
    struct.pack_into('<I', head, 0x18, total)
    struct.pack_into('<I', head, 0x34, 0x100 - 0x34)
    struct.pack_into('<I', head, 0x60 if opl4 else 0x50, 33868800 if opl4 else 3579545)
    body = bytes(head) + bytes(data)
    if tags:
        struct.pack_into('<I', head, 0x14, len(body) - 0x14)
        body = bytes(head) + bytes(data) + tags
    out = bytearray(body)
    struct.pack_into('<I', out, 0x04, len(out) - 4)
    return bytes(out)


def opl_dro(seconds: float) -> bytes:
    """DRO 2 (DOSBox): OPL2, задержки в мс кодами 0xFE (короткая) и 0xFF (длинная)."""
    codes = []
    pairs = bytearray()

    def write(reg, value):
        if reg is None:
            ms = round(value * 1000 / 44100)
            while ms > 0:
                if ms >= 256:
                    part = min(ms // 256, 256)
                    pairs.extend(bytes([0xFF, part - 1]))
                    ms -= part * 256
                else:
                    pairs.extend(bytes([0xFE, ms - 1]))
                    ms = 0
            return
        if reg not in codes:
            codes.append(reg)
        pairs.extend(bytes([codes.index(reg), value]))

    opl2_notes(seconds, write)
    length_ms = int(seconds * 2) * 500
    head = b'DBRAWOPL' + struct.pack('<HHII', 2, 0, len(pairs) // 2, length_ms)
    head += bytes([0, 0, 0, 0xFE, 0xFF, len(codes)]) + bytes(codes)
    return head + bytes(pairs)


def opl_imf(seconds: float, rate: int = 560) -> bytes:
    """IMF тип 0: записи [регистр][значение][задержка LE16] в тиках rate Гц."""
    records = bytearray(b'\x00\x00\x00\x00')
    pending = []

    def write(reg, value):
        if reg is None:
            ticks = round(value * rate / 44100)
            if pending:
                last = pending[-1]
                pending[-1] = (last[0], last[1], last[2] + ticks)
            return
        pending.append((reg, value, 0))

    opl2_notes(seconds, write)
    for reg, value, delay in pending:
        records.extend(bytes([reg, value]) + struct.pack('<H', delay))
    return bytes(records)


def make_wav(data: bytes, rate: int, channels: int, bits: int, fmt: int = 1,
             extensible: bool = False, info: dict | None = None) -> bytes:
    """RIFF WAVE: fmt (или WAVE_FORMAT_EXTENSIBLE с подформатом fmt), LIST INFO, data."""
    block = channels * bits // 8
    head = struct.pack('<HHIIHH', 0xFFFE if extensible else fmt, channels, rate, rate * block, block, bits)
    if extensible:
        head += struct.pack('<HHI', 22, bits, 0) + struct.pack('<H', fmt) + bytes.fromhex('000000001000800000aa00389b71')
    chunks = b'fmt ' + struct.pack('<I', len(head)) + head
    if info:
        body = b'INFO'
        for key, value in info.items():
            text = value.encode('ascii') + b'\0'
            body += key.encode('ascii') + struct.pack('<I', len(text)) + text + (b'\0' if len(text) & 1 else b'')
        chunks += b'LIST' + struct.pack('<I', len(body)) + body
    chunks += b'JUNK' + struct.pack('<I', 6) + bytes(6)   # блок, который надо пропустить
    chunks += b'data' + struct.pack('<I', len(data)) + data
    return b'RIFF' + struct.pack('<I', 4 + len(chunks)) + b'WAVE' + chunks


def wav_square(frames: int, channels: int, bits: int, fmt: int, high: bytes, low: bytes) -> bytes:
    """Меандр: кадры по 32 отсчёта high и low во всех каналах (байты отсчёта готовы)."""
    out = bytearray()
    for i in range(frames):
        out += (high if (i // 32) % 2 == 0 else low) * channels
    return bytes(out)


def mwm_song(positions: int = 2, kit: str = 'TESTKIT', loop: int = 255,
             name: str = 'VDAC4 MWM test') -> bytes:
    """MoonBlaster Wave: канал 0 — своя волна набора (волна 1 → 176), канал 1 —
    патч ПЗУ 0 (волна 2); темп 6, 50 Гц, все позиции — узор 0."""
    info = bytearray(220)
    info[0] = positions - 1                       # xleng
    info[1] = loop                                # xloop
    info[26] = 6                                  # xtempo
    info[27] = 1                                  # xhzequal: 50 Гц
    for t in range(24):
        info[100 + t] = 1                         # xbegwav
    info[124] = 176                               # волна 1 — своя волна 0 набора
    info[125] = 0                                 # волна 2 — патч ПЗУ 0
    settings = bytes(info) + name.encode('ascii').ljust(50) + kit.encode('ascii').ljust(8)

    def step(ch0: int, ch1: int = 0) -> bytes:
        if ch0 == 0 and ch1 == 0:
            return b'\xff'                        # шаг пуст
        return bytes([ch0, 0x80 if ch1 else 0, 0, 0]) + (bytes([ch1]) if ch1 else b'')

    pattern = (step(37, 99) + step(0, 49) + b'\xff\xff' + step(97) + b'\xff\xff\xff' +
               step(49) + b'\xff' * 7)
    positions_table = bytes(positions)
    pattern_table = struct.pack('<H', 278 + positions + 2)
    chunk = struct.pack('<H', len(pattern)) + b'\x01' + pattern
    return b'MBMS\x10\x08' + settings + positions_table + pattern_table + chunk + b'\x00\x00\x00'


def mwm_kit() -> bytes:
    """Набор MWK: одна запись волны (вся высота — тон 0) и тон 0 — меандр 8 бит."""
    sample = bytes([100] * 16 + [156] * 16) * 64
    end = (0x10000 - len(sample)) & 0xFFFF       # конец — отрицательный
    record = bytes([0, 96, 0, 36]) + bytes(18) + bytes([96, 0, 0])
    tone = (bytes([0, 0, 0, 0, end >> 8, end & 0xFF, 0x00, 0xF0, 0x00, 0x08, 0x00]) +
            struct.pack('<H', len(sample)))
    total = len(sample)
    return (b'MBMS\x10\x0d' + bytes([total & 0xFF, (total >> 8) & 0xFF, total >> 16, 1]) +
            b'\x01' + bytes(63) + record + tone + sample)


def radio_list(stations: list[tuple[str, str]], encoding: str = 'ascii') -> bytes:
    """Плейлист M3U, как у станций: #EXTM3U, пустая строка, #EXTINF и адрес на станцию."""
    lines = ['#EXTM3U', '']
    for title, url in stations:
        lines += [f'#EXTINF:-1,{title}', url]
    return ('\r\n'.join(lines) + '\r\n').encode(encoding)


STATIONS = [('First Station', 'http://one.example/live'),
            ('Second Station', 'http://two.example/stream.mp3'),
            ('Third Station', 'https://three.example/radio')]


class PlayerHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.exe = build()
        if cls.exe is None:
            raise unittest.SkipTest('нет MSVC (vcvars32.bat): стенд PLAYER.ELF пропущен')
        cls.temp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temp.cleanup()

    def run_host(self, scenario: str, content: bytes | None = None,
                 path: pathlib.Path | None = None, **keys) -> dict[str, str]:
        args = [str(self.exe), scenario]
        for key, value in keys.items():
            args += [f'--{key}', str(value)]
        if content is not None:
            path = pathlib.Path(self.temp.name) / f'{scenario}.bin'
            path.write_bytes(content)
        if path is not None:
            args.append(str(path))
        result = subprocess.run(args, capture_output=True, text=True, timeout=300)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.log = result.stderr                  # журнал ядра: строки «log: ...»
        values = dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)
        self.assertEqual(values['entry'], '1', 'main вернул ZM_APP_ABI_MAJOR')
        self.assertEqual(values['start'], '0')
        self.assertEqual(values['tone_own'], '0', 'тон делает ядро')
        self.assertEqual(values['tasks_after_stop'], '0', 'задача плеера кончилась')
        self.assertEqual(values['modules_after_stop'], '0', 'декодер закрыт')
        self.assertEqual(values.get('replies_outside', '0'), '0')
        return values

    def test_starts_and_plays_in_board_internal_memory(self) -> None:
        # Внутренняя память ESP на плате (0.1.86 без приложения: свободно 49 503, самый большой
        # кусок 29 696). Плееру с задачами декодера, помощника и обложек из меток — не больше
        # 36 КБ: 1.21…1.24 держали ещё задачу обложек из сети (и 1.21/1.22 — таблицы спектра)
        # и брали 41…61 КБ — на плате start −2, плеер не запускался. Здесь — с запасом 40 000.
        r = self.run_host('mp3', silent_mp3(300, tag=2000), internal=40000, **{'internal-block': 29696})
        self.assertEqual((r['state'], r['finished'], r['decoded']), ('0', '1', '1'))
        self.assertEqual(int(r['pcm_frames']), 300 * FRAME_SAMPLES)
        self.assertLessEqual(int(r['internal_peak']), 36000, 'внутренней памяти ESP плееру нужно больше')
        self.assertNotIn('not allocated', self.log)

    def test_every_mp3_frame_reaches_the_output(self) -> None:
        r = self.run_host('mp3', silent_mp3(300, tag=2000, junk=b'\x00\xFF\x00junk'))
        self.assertEqual(r['open'], '1')
        self.assertEqual((r['state'], r['finished'], r['decoded']), ('0', '1', '1'))
        self.assertEqual(int(r['pcm_frames']), 300 * FRAME_SAMPLES)
        # Принятые — ещё не проигранные: при закрытии в очереди ничего не выброшено.
        self.assertEqual(r['pcm_discarded'], '0', 'конец песни доигран, а не выброшен')
        self.assertAlmostEqual(int(r['pcm_played']), 300 * FRAME_SAMPLES, delta=1)
        self.assertEqual((r['rate'], r['channels'], r['bitrate']), (str(RATE), '2', '128'))
        self.assertEqual(r['format'], str(FORMAT_MP3))
        self.assertEqual((r['errors'], r['underruns']), ('0', '0'))
        self.assertEqual(r['seconds'], str(300 * FRAME_SAMPLES // RATE))
        self.assertEqual(r['pcm_opens'], '1')
        self.assertEqual(r['allocations_after_stop'], '6',
                         'осталась память приложения: P, кольцо, кадры, миниатюра, спектр и его история')
        self.assertEqual(r['temperature'], '48', 'кристалл 47,5 °C — в AUDIO_STATUS целыми')

    def test_input_rate_is_logged(self) -> None:
        # PLAYER 1.27: при остановке — сколько потока пришло от Z80, за сколько и сколько раз
        # очередь была полна (0 — звук ждал данных: это предел передачи Evo → ESP).
        mp3 = silent_mp3(300, tag=2000)
        self.run_host('mp3', mp3)
        found = re.search(r'player: input (\d+) KB in (\d+) ms \((\d+) KB/s\), queue full (\d+) times', self.log)
        self.assertIsNotNone(found, self.log)
        kb, ms, rate, full = (int(x) for x in found.groups())
        self.assertAlmostEqual(kb, (len(mp3) - 2000) // 1024, delta=2, msg='весь поток за меткой')
        # На стенде поток приходит разом (0 мс — скорость 0); на плате — за секунды
        if ms > 0:
            # КБ в строке округлены вниз (до 1 КБ): за ms это до 1000/ms КБ/с расхождения
            self.assertAlmostEqual(rate, kb * 1000 // ms, delta=1000 // ms + 2,
                                   msg='КБ/с — из байтов и времени')

    def test_vbr_xing_survives_a_tiny_first_block(self) -> None:
        # Длинную метку ID3 плагин пропускает сам; первым блоком уходит хвост
        # сектора за ней — 52 байта кадра Xing у «Балтики». Длительность — по
        # Xing и не скачет за битрейтом кадров; сам кадр Xing тишины не даёт.
        mp3, average = vbr_mp3(300)
        for first in (52, 0):
            with self.subTest(first=first):
                r = self.run_host('mp3', mp3, first=first, sized=1, block=512)
                seconds = 300 * FRAME_SAMPLES // RATE
                self.assertEqual(r['stream_durations'], f'{seconds},{seconds},{seconds}')
                self.assertEqual(int(r['pcm_frames']), 300 * FRAME_SAMPLES, 'кадр Xing — мимо')
                self.assertEqual(r['bitrate'], str(average), 'средний битрейт VBR')
                self.assertEqual(r['finished'], '1')

    def test_vbr_duration_after_a_flush(self) -> None:
        # После перемотки первого кадра в потоке нет: Xing — от начала файла.
        mp3, average = vbr_mp3(400)
        resume = mp3.index(bytes([0xFF, 0xFB, 0xD0, 0x00]), 200 * 900)
        r = self.run_host('flush', mp3, cut=100 * 900, resume=resume, base=5000, sized=1)
        seconds = 400 * FRAME_SAMPLES // RATE
        self.assertEqual(r['flush'], '1')
        self.assertEqual(r['stream_durations'], f'{seconds},{seconds},{seconds}')
        self.assertEqual(r['bitrate'], str(average))

    def test_pause_holds_the_output_and_loses_nothing(self) -> None:
        # Каждая пауза дольше двух ожиданий записи (по 500 мс).
        r = self.run_host('pause', silent_mp3(200), speed=5, hold=1100, pauses=5)
        self.assertEqual(r['frames_during_pause'], '0', 'на паузе в вывод ничего не идёт')
        self.assertEqual(r['paused_paused'], '1')
        self.assertEqual(r['pcm_pauses'], '5')
        self.assertEqual(r['finished'], '1')
        self.assertEqual(int(r['pcm_frames']), 200 * FRAME_SAMPLES, 'после паузы — без пропуска')

    def test_flush_restarts_time_and_stream(self) -> None:
        mp3 = silent_mp3(400)
        r = self.run_host('flush', mp3, cut=100 * FRAME_BYTES, resume=200 * FRAME_BYTES,
                          base=60000)
        self.assertEqual(r['flush'], '1')
        self.assertEqual(r['seconds_after_flush'], '60')
        self.assertEqual(r['finished'], '1')
        self.assertEqual(r['seconds'], str(60 + 200 * FRAME_SAMPLES // RATE))
        self.assertGreaterEqual(int(r['pcm_flushes']), 1)

    def test_seek_while_draining_reopens_the_stream(self) -> None:
        """Плагин всё отдал и закрыл поток для доигрывания, а пользователь мотает
        назад: прежде FLUSH отвергался (поток закрыт), теперь поток открывается снова."""
        mp3 = silent_mp3(400)
        r = self.run_host('drainflush', mp3, resume=100 * FRAME_BYTES, base=2000, speed=1)
        self.assertEqual(r['state_draining'], '2', 'поток закрыт, звук ещё идёт')
        self.assertEqual(r['flush'], '1', 'перемотка во время доигрывания принята')
        self.assertEqual(r['state_after_flush'], '1', 'снова играет')
        self.assertEqual(r['finished'], '1')
        self.assertEqual(r['seconds'], str(2 + 300 * FRAME_SAMPLES // RATE))

    def test_wav_chunk_of_4_gb_does_not_overrun_the_header(self) -> None:
        """Блок LIST длиной 0xFFFFFFFF: прежде сумма с выравниванием давала ноль, и
        разбор меток читал за буфер. Теперь блок пропускается до конца файла."""
        fmt = struct.pack('<HHIIHH', 1, 2, 44100, 44100 * 4, 4, 16)
        wav = (b'RIFF' + struct.pack('<I', 0) + b'WAVE' + b'fmt ' + struct.pack('<I', 16) + fmt +
               b'LIST' + struct.pack('<I', 0xFFFFFFFF) + b'INFO' + b'INAM' + struct.pack('<I', 0x7FFFFFF0) +
               bytes(64))
        r = self.run_host('mp3', wav, sized=1)
        self.assertEqual(r['finished'], '1')
        self.assertEqual(r['decoded'], '0', 'звука нет, но и падения нет')

    def test_wav_with_a_long_block_before_data_in_small_pieces(self) -> None:
        """Между fmt и data — блок в 100 КБ, файл приходит кусками по 512 байт: PLAYER
        спрашивает сведения у декодера, пока частоты ещё нет (прежде — деление на ноль)."""
        frames = 22050
        data = wav_square(frames, 2, 16, 1, struct.pack('<h', 8192), struct.pack('<h', -8192))
        fmt = struct.pack('<HHIIHH', 1, 2, 44100, 44100 * 4, 4, 16)
        chunks = (b'fmt ' + struct.pack('<I', 16) + fmt + b'JUNK' + struct.pack('<I', 100000) + bytes(100000) +
                  b'data' + struct.pack('<I', len(data)) + data)
        wav = b'RIFF' + struct.pack('<I', 4 + len(chunks)) + b'WAVE' + chunks
        r = self.run_host('mp3', wav, block=512)
        self.assertEqual((r['finished'], r['decoded']), ('1', '1'))
        self.assertEqual(int(r['pcm_frames']), frames)

    def test_stop_in_the_middle(self) -> None:
        r = self.run_host('stop', silent_mp3(3000), speed=1)
        self.assertLess(int(r['pcm_frames']), 3000 * FRAME_SAMPLES)

    def test_cover_and_tags(self) -> None:
        picture = b'\xFF\xD8\xFF' + bytes([255, 255, 255]) + bytes(3000)
        apic = b'\x00image/jpeg\x00\x03\x00' + picture
        tag = id3_tag(id3_frame(b'TPE1', b'\x03' + 'Группа'.encode('utf-8')) +
                      id3_frame(b'TXXX', bytes(100000)) +
                      id3_frame(b'APIC', apic) +
                      id3_frame(b'TIT2', b'\x00Song'))
        r = self.run_host('cover', tag + silent_mp3(20))
        self.assertEqual(r['cover_state'], '1')
        self.assertLess(int(r['cover_feeds']), 12, 'большой кадр TXXX не пересылался')
        self.assertEqual(r['thumb_state'], '1')
        self.assertEqual((r['thumb_bytes'], r['thumb_uniform'], r['thumb_byte']),
                         ('2048', '1', str(0xFF)), 'белый — цвет 15 во всех точках')
        self.assertEqual(bytes.fromhex(r['title_hex']).decode('cp866'), 'Группа - Song')

    def test_picture_from_the_folder(self) -> None:
        picture = b'\xFF\xD8\xFF' + bytes([255, 255, 255]) + bytes(20000)    # белый — цвет 15
        r = self.run_host('picture', picture)
        self.assertEqual(r['picture_state'], '1')
        self.assertEqual(int(r['picture_frames']), (len(picture) + 1015) // 1016 + 1,
                         'кусок не с того места не взят и прислан снова')
        self.assertEqual((r['thumb_state'], r['thumb_byte']), ('1', str(0xFF)))
        r = self.run_host('picture', b'GIF89a' + bytes(5000))
        self.assertEqual((r['picture_state'], r['thumb_state']), ('2', '2'), 'не JPEG — отказ')

    def test_tags_without_a_picture(self) -> None:
        tag = id3_tag(id3_frame(b'TIT2', b'\x00Only title'))
        r = self.run_host('cover', tag + silent_mp3(5))
        self.assertEqual((r['cover_state'], r['thumb_state']), ('2', '2'))
        self.assertEqual(bytes.fromhex(r['title_hex']).decode('cp866'), 'Only title')

    # Ответ поиска Deezer (как у api.deezer.com/search): первая запись — чужой исполнитель.
    DEEZER = (r'{"data":[{"id":1,"title":"Children","artist":{"id":5,"name":"Other Band",'
              r'"picture_big":"https:\/\/cdn-images.dzcdn.net\/images\/artist\/aa\/500x500.jpg"},'
              r'"album":{"id":7,"title":"X","cover_big":"https:\/\/cdn-images.dzcdn.net\/images'
              r'\/cover\/bad\/500x500-000000-80-0-0.jpg"}},{"id":2,"title":"Children (Dream '
              r'Version)","artist":{"id":6,"name":"Robert Miles"},"album":{"id":8,"title":'
              r'"Dreamland","cover_big":"https:\/\/cdn-images.dzcdn.net\/images\/cover\/55140f8e'
              r'\/500x500-000000-80-0-0.jpg"}}],"total":2}')
    NET_JPEG = b'\xFF\xD8\xFF' + bytes([0x20, 0x80, 0xE0]) + bytes(2000)   # «картинка» одного цвета

    def ui_run(self, tag: bytes, json: str | None = None, block: bytes | None = None) -> dict[str, str]:
        folder = pathlib.Path(self.temp.name)
        (folder / 'deezer.json').write_text(json if json is not None else self.DEEZER, encoding='utf-8')
        (folder / 'net.jpg').write_bytes(self.NET_JPEG)
        keys = {'cover-json': folder / 'deezer.json', 'cover-jpeg': folder / 'net.jpg'}
        if block is not None:
            (folder / 'block.bin').write_bytes(block)
            keys['ui-block'] = folder / 'block.bin'
        return self.run_host('ui', tag + silent_mp3(800), ui=1, speed=4, **keys)

    def test_screen_whole_chain(self) -> None:
        # Вся цепочка на ПК: тело AUDIO_STATUS, которое собрал настоящий плагин VDAC4MP3.WMF на
        # эмуляторе Z80, → PLAYER.ELF (этот стенд) → его экран (screen.cpp, 1.26) → LINK → Tang
        # (модель): оверлей 1 — холст 640×360 ×1 в середине кадра 1280×720, целиком в Tang до
        # включения, проявляется по ступени прозрачности за кадр; кадры идут; плагину — флаги
        # экрана; флаг ухода экран гасит.
        import test_mp3_plugin_z80 as plugin
        h = plugin.Mp3Harness(bytes(40 * 1024), name=b'CHILDREN.MP3',
                              esp=plugin.AudioEsp(played_per_status=2048))
        h.esc_after = 120
        h.start()
        block = h.audio.ui_raw[0]
        self.assertEqual(block[0:2], b'\x01\x01', 'тело плагина: версия 1, окно открыто')
        tag = id3_tag(id3_frame(b'TPE1', b'\x00Robert Miles') + id3_frame(b'TIT2', b'\x00Children'))
        r = self.ui_run(tag, block=block)
        self.assertEqual(r['ui_visible'], '1')
        self.assertEqual(r['ui_file'], 'CHILDREN.MP3', 'имя — то, что прислал плагин')
        self.assertEqual(r['open_screen'], '2', 'AUDIO_OPEN: Tang может показать экран')
        ctrl = int(r['chain_ctrl'])
        self.assertEqual((ctrl & 1, (ctrl >> 1) & 1, (ctrl >> 4) & 15), (1, 0, 15),
                         'оверлей включён, ×1, непрозрачный')
        self.assertEqual(int(r['chain_pos']), 320 | 180 << 16, 'место: середина кадра 1280×720')
        self.assertEqual(int(r['chain_size']), 640 | 360 << 16, 'холст 640×360')
        self.assertEqual(int(r['chain_base']), 131072 - 320 * 360, 'холст — в конце региона')
        self.assertEqual(r['chain_written_before_on'], str(320 * 360), 'весь холст — до включения')
        self.assertEqual((r['chain_written'], r['chain_outside']), (str(320 * 360), '0'),
                         'пишется только холст')
        alphas = [int(a) for a in r['chain_alphas'].split(',')]
        self.assertEqual(alphas[:16], list(range(16)), 'появление: 0…15 по ступени')
        self.assertGreater(int(r['chain_frame_words']), 0, 'кадры идут')
        self.assertEqual(r['chain_bad'], '0')
        self.assertEqual(r['chain_flags'], '3', 'плагину: Tang есть, экран показан')
        self.assertEqual(r['chain_app_dones'], '1', 'плагин ушёл — PLAYER просит выгрузить себя')
        left = [int(a) for a in r['chain_left_alphas'].split(',')]
        self.assertEqual(left, list(range(14, -1, -1)) + [-1], 'уходит плавно 15 → 0, затем выкл.')
        self.assertEqual(r['chain_left_on'], '0')

    def test_screen_model_and_online_cover(self) -> None:
        # Экран оверлеем Tang (PLAYER 1.21): что прислал плагин в теле AUDIO_STATUS, метки,
        # спектр и формат — в снимке; обложки нет ни в метке, ни в папке — из сети, с флагом 64.
        tag = id3_tag(id3_frame(b'TPE1', b'\x00Robert Miles') + id3_frame(b'TIT2', b'\x00Children'))
        r = self.ui_run(tag)
        self.assertEqual(r['ui_table'], '0', 'экран рисует сам PLAYER: прошивке снимка нет')
        self.assertEqual(r['ui_before_visible'], '0', 'плагин тела не прислал — экрана нет')
        self.assertEqual((r['ui_visible'], r['ui_page'], r['ui_track'], r['ui_tracks']),
                         ('1', '2', '3', '10'))
        self.assertEqual(r['ui_sent_kb'], str(123456 // 1024))
        self.assertEqual((r['ui_file'], r['ui_status']), ('CHILDREN.MP3', 'Buffering'))
        self.assertEqual(r['tag_cover_state'], '2', 'в метке картинки нет')
        self.assertEqual((r['ui_view'], r['ui_count']), ('1', '64'), 'спектр: 64 полосы')
        self.assertIn(r['ui_state'], ('3', '5'), 'играет или доигрывает')
        self.assertEqual((r['ui_title'], r['ui_artist']), ('Children', 'Robert Miles'))
        self.assertEqual(r['ui_format'], 'MP3  128 kbps  44.1 kHz  stereo')
        self.assertEqual(r['ui_temperature'], '48')
        self.assertGreaterEqual(int(r['ui_played_ms']), 10000)
        self.assertEqual(r['cover_searches'], '1')
        self.assertEqual(r['cover_search_url'],
                         'https://api.deezer.com/search?limit=5&q=Robert%20Miles%20Children')
        self.assertEqual(r['cover_pictures'], '1')
        self.assertIn('/cover/55140f8e/', r['cover_picture_url'], 'запись своего исполнителя')
        self.assertEqual(r['cover_flag'], '1', 'окно Evo спросит миниатюру')
        self.assertNotEqual(r['ui_cover_serial'], '0')
        self.assertEqual(r['ui_cover'], '0')
        rgb565 = (0x20 >> 3) << 11 | (0x80 >> 2) << 5 | 0xE0 >> 3
        self.assertEqual((r['ui_cover_pixel'], r['ui_cover_corner']), (str(rgb565), str(rgb565)))
        self.assertEqual(r['ui_left_visible'], '0', 'плагин уходит — экран сразу прочь')

    def test_own_cover_is_not_replaced_from_the_net(self) -> None:
        picture = b'\xFF\xD8\xFF' + bytes([255, 255, 255]) + bytes(3000)
        tag = id3_tag(id3_frame(b'TPE1', b'\x00Robert Miles') + id3_frame(b'TIT2', b'\x00Children') +
                      id3_frame(b'APIC', b'\x00image/jpeg\x00\x03\x00' + picture))
        r = self.ui_run(tag)
        self.assertEqual(r['tag_cover_state'], '1')
        self.assertEqual(r['cover_searches'], '0', 'своя обложка есть — в сеть не ходить')
        self.assertEqual(r['ui_cover'], '0')
        self.assertEqual(r['ui_cover_pixel'], str(0xFFFF), 'обложка из метки — белая')

    def test_online_cover_cyrillic_and_foreign_artist(self) -> None:
        tag = id3_tag(id3_frame(b'TPE1', b'\x03' + 'Кино'.encode('utf-8')) +
                      id3_frame(b'TIT2', b'\x03' + 'Группа крови'.encode('utf-8')))
        r = self.ui_run(tag)
        query = '%D0%9A%D0%B8%D0%BD%D0%BE%20%D0%93%D1%80%D1%83%D0%BF%D0%BF%D0%B0%20' \
                '%D0%BA%D1%80%D0%BE%D0%B2%D0%B8'
        self.assertEqual(r['cover_search_url'], 'https://api.deezer.com/search?limit=5&q=' + query)
        self.assertEqual(r['cover_pictures'], '0', 'чужой исполнитель — обложки нет')
        self.assertEqual((r['cover_flag'], r['ui_cover_serial']), ('0', '0'))
        # Имя в ответе — экранированное \\uXXXX, регистр другой: тот же исполнитель
        json = ('{"data":[{"id":3,"artist":{"id":9,"name":"\\u041a\\u0418\\u041d\\u041e"},"album":'
                '{"id":4,"cover_big":"https:\\/\\/cdn-images.dzcdn.net\\/images\\/cover\\/kino'
                '\\/500x500-000000-80-0-0.jpg"}}],"total":1}')
        r = self.ui_run(tag, json)
        self.assertEqual(r['cover_pictures'], '1')
        self.assertIn('/cover/kino/', r['cover_picture_url'])
        self.assertEqual(r['cover_flag'], '1')

    def test_missing_decoder(self) -> None:
        r = self.run_host('missing')
        self.assertEqual(r['open'], '0')
        self.assertEqual(r['state'], str(STATE_IDLE))

    def test_module_plays_once_to_the_end(self) -> None:
        r = self.run_host('module', song_xm(4))
        self.assertEqual(r['open'], '1')
        self.assertEqual(r['loaded_state'], str(STATE_PLAYING))
        self.assertEqual(r['title'], 'vdac4 test')
        self.assertEqual((r['info_format'], r['info_voices'], r['info_length']),
                         (str(FORMAT_XM), '2', '4'))
        self.assertEqual((r['state'], r['finished']), ('0', '1'))
        self.assertAlmostEqual(int(r['pcm_frames']), int(4 * 7.68 * RATE), delta=2048)
        # PLAYER 1.20: вывод открывается, когда накоплено 200 мс начала — запись во
        # flash в начале трека (оба ядра стоят) лишь задерживает старт.
        self.assertGreaterEqual(int(r['first_write']), RATE // 5)
        # Файл модуля живёт с декодером (отсчёты XM распакованы в нём на месте):
        # на одну память больше, чем у MP3; её освобождает ядро при выгрузке.
        self.assertEqual(r['allocations_after_stop'], '7')

    def test_spectrum(self) -> None:
        for hz, band in ((1000, None), (5000, None)):
            with self.subTest(hz=hz):
                bands = [int(v) for v in self.run_host('spectrum', hz=hz)['bands'].split(',')]
                self.assertEqual(len(bands), 32)
                top = bands.index(max(bands))
                self.assertGreaterEqual(bands[top], 10, 'синус 0,8 — почти верх шкалы')
                self.assertTrue(all(level == 0 for index, level in enumerate(bands)
                                    if abs(index - top) > 5), bands)
                self.assertTrue(all(v <= 12 for v in bands))

    def test_spectrum_follows_playback_pause_reset_and_drain(self) -> None:
        r = self.run_host('spectrum_timeline')
        for key in ('before_note', 'after_note', 'drained', 'after_reset',
                    'new_stream_queued', 'rate_changed', 'wrapped_future'):
            self.assertEqual(r[key], '0', key)
        for key in ('during_note', 'before_reset', 'new_stream_played', 'wrapped_played'):
            self.assertGreaterEqual(int(r[key]), 10, key)
        self.assertEqual(r['paused_note'], r['during_note'])

    def test_midi_spectrum_is_synced_with_different_pcm_queues(self) -> None:
        if not BANK.is_file():
            self.skipTest(f'нет {BANK}')
        song = midi_events([(0, b'\xC0\x00'), (0, b'\xB0\x5B\x00'),
                            (2880, b'\x90\x45\x7F'), (3840, b'\x80\x45\x00'),
                            (5760, b'\xB0\x07\x64')])
        for queue_ms in (93, 500, 1486):
            with self.subTest(queue_ms=queue_ms):
                r = self.run_host('module', song, midi=1, bank=BANK, speed=1,
                                  **{'pcm-ms': queue_ms, 'view-sync': 1})
                self.assertAlmostEqual(float(r['sync_pcm_onset_ms']), 3000, delta=20)
                self.assertGreater(float(r['sync_view_at_ms']), 0)
                # 50 мс между снимками + один блок PCM + опрос стенда; секунды очереди
                # больше не должны превращаться в опережение экрана.
                self.assertLessEqual(abs(float(r['sync_lead_ms'])), 100, r)
                self.assertEqual(r['finished'], '1')

    def test_module_view_and_tags(self) -> None:
        r = self.run_host('module', song_xm(2))
        self.assertEqual((r['view_kind'], r['view_count']), ('2', '2'), 'уровни двух каналов')
        self.assertEqual(r['view_bytes'], str(6 + 2))
        self.assertEqual(r['tags_title'], 'vdac4 test')
        self.assertEqual(int(r['tags_bytes']), len('vdac4 test') + 5 + 3,
                         'у модуля за метками — сэмплы LE16 и эффекты')

    def test_module_seek(self) -> None:
        r = self.run_host('module', song_xm(4), seek=2)
        self.assertEqual((r['seek'], r['seek_order']), ('1', '2'))
        self.assertEqual(r['finished'], '1')
        frames = int(r['pcm_frames'])
        self.assertGreaterEqual(frames, int(2 * 7.68 * RATE) - 2048)
        self.assertLess(frames, int(2.5 * 7.68 * RATE), 'две позиции пропущены')

    def assert_skipped(self, r: dict[str, str]) -> None:
        """Файл не взят: «играет» с причиной вместо названия, без звука и без
        «распознан», потом доиграл — плагин уходит к следующему файлу."""
        self.assertEqual(r['open'], '1')
        self.assertEqual((r['loaded_state'], r['loaded_decoded'], r['loaded_errors']),
                         (str(STATE_PLAYING), '0', '1'), r)
        self.assertEqual(r['tags_title'], 'Cannot play this file, skipping')
        self.assertEqual((r['state'], r['finished']), (str(STATE_IDLE), '1'))
        self.assertEqual(r['pcm_frames'], '0')

    def test_garbage_module_is_skipped(self) -> None:
        rng = random.Random(5)
        r = self.run_host('module', bytes(rng.randrange(256) for _ in range(5000)))
        self.assert_skipped(r)
        self.assertIn('was not loaded: skipped', self.log)

    def test_it_streams_through_the_player(self) -> None:
        r = self.run_host('module', song_it(20000), ext='IT', seconds=2)
        self.assertIn('IT of', self.log)
        self.assertIn('comes as a stream', self.log)
        self.assertIn('streamed: as is', self.log)
        self.assertEqual(r['loaded_state'], str(STATE_PLAYING))
        self.assertEqual(r['tags_title'], 'vdac4 it')
        self.assertGreater(int(r['pcm_peak_permille']), 20, 'звук есть')
        self.assertNotEqual(r['xmp_peak'], '0', 'память — у libxmp')

    def it_plans(self, song: bytes) -> tuple[int, int, int]:
        """Оценки памяти TRACKER для IT потоком: как есть, моно, моно 8 бит."""
        self.run_host('module', song, ext='IT', seconds=0.2)
        line = next(l for l in self.log.splitlines() if 'streamed:' in l)
        plans = line.split('(')[1].split(' by plan')[0].split(' / ')
        return int(plans[0]), int(plans[1]), int(plans[2])

    def test_big_it_is_reduced_to_fit(self) -> None:
        song = song_it(150_000, stereo=True, count=8)
        full, mono, eight = self.it_plans(song)
        self.assertLess(mono, full)
        self.assertLess(eight, mono)
        ring = 250_000                       # кольцо и прочее PLAYER — тоже в PSRAM стенда
        r = self.run_host('module', song, ext='IT', seconds=1, psram=mono + ring)
        self.assertIn('streamed: stereo -> mono', self.log)
        self.assertEqual(r['loaded_state'], str(STATE_PLAYING))
        self.assertGreater(int(r['pcm_peak_permille']), 20)
        self.assertLessEqual(int(r['xmp_peak']), mono, 'libxmp не больше оценки')
        r = self.run_host('module', song, ext='IT', seconds=1, psram=eight + ring)
        self.assertIn('streamed: mono, 16 -> 8 bit', self.log)
        self.assertGreater(int(r['pcm_peak_permille']), 20)
        r = self.run_host('module', song, ext='IT', psram=eight // 2)
        self.assertIn('streamed: does not fit', self.log)
        self.assert_skipped(r)
        self.assertLess(int(r['refused_at']), len(song) // 2,
                        'отвергнутый файл ESP дальше не берёт (ответ 2): плагин не шлёт впустую')

    def test_stop_while_it_streams(self) -> None:
        song = song_it(400_000)
        r = self.run_host('module', song, ext='IT', **{'stop-at': len(song) // 2})
        self.assertLess(int(r['stop_ms']), 500, 'стоп не ждёт конца файла')
        self.assertEqual(r['stopped_state'], str(STATE_IDLE))
        self.assertEqual(r['xmp_after_stop'], '0', 'память libxmp отдана')

    def test_not_an_it_in_an_it_stream(self) -> None:
        r = self.run_host('module', song_xm(2), ext='IT', seconds=1)
        self.assertIn('comes as a stream', self.log)
        self.assertEqual(r['loaded_state'], str(STATE_PLAYING))
        self.assertEqual(r['info_format'], str(FORMAT_XM), 'XM в файле .it — как XM')

    def test_mod_with_a_broken_tag(self) -> None:
        mod = bytearray(song_mod())
        mod[1080:1084] = b'WN\x00&'
        r = self.run_host('module', bytes(mod), ext='MOD', seconds=1)
        self.assertIn('tag -> M.K.', self.log)
        self.assertEqual(r['loaded_state'], str(STATE_PLAYING))
        self.assertGreater(int(r['pcm_peak_permille']), 20)

    def test_mod_cut_before_its_samples_is_skipped(self) -> None:
        mod = song_mod()
        r = self.run_host('module', mod[:1084 + 1024], ext='MOD')
        self.assertIn('no sample left', self.log)
        self.assert_skipped(r)

    def test_mo_is_a_mod(self) -> None:
        r = self.run_host('module', song_mod(), ext='MO', seconds=1)
        self.assertEqual(r['loaded_state'], str(STATE_PLAYING))
        self.assertGreater(int(r['pcm_peak_permille']), 20)

    def test_midi_title_in_any_cyrillic(self) -> None:
        # Track Name пишут в CP1251 (aida_vedischeva-pesenka_o_medvedyah.mid), CP866 и
        # UTF-8: MIDI.ELF 1.7.3 отдаёт байты файла, PLAYER 1.9 различает кодировку (до них
        # кириллица была вопросами); UTF-8, обрезанный посреди знака, — без обрывка.
        if not BANK.is_file():
            self.skipTest(f'нет {BANK}')
        title = 'Песенка о медведях'
        cases = [(title.encode('cp1251'), title), (title.encode('cp866'), title),
                 (title.encode('utf-8'), title), ('мама'.encode('cp866'), 'мама'),
                 ('мама'.encode('cp1251'), 'мама')]      # поровну букв обеих — CP1251
        for raw, want in cases:
            r = self.run_host('module', song_midi(1, title=raw), midi=1, bank=BANK, seconds=1)
            self.assertEqual(bytes.fromhex(r['tags_title_hex']).decode('cp866'), want, raw.hex())
        long = ('Ж' * 40).encode('utf-8')       # 80 байтов: декодер обрежет на 63
        r = self.run_host('module', song_midi(1, title=long), midi=1, bank=BANK, seconds=1)
        self.assertEqual(bytes.fromhex(r['tags_title_hex']).decode('cp866'), 'Ж' * 31)

    def test_midi_plays_to_the_end(self) -> None:
        if not BANK.is_file():
            self.skipTest(f'нет {BANK}')
        r = self.run_host('module', song_midi(4), midi=1, bank=BANK)
        self.assertEqual(r['open'], '1')
        self.assertEqual((r['loaded_state'], r['loaded_format']), (str(STATE_PLAYING),
                                                                   str(FORMAT_MIDI)))
        self.assertEqual(r['tags_title'], 'VDAC4 MIDI test')
        self.assertEqual(r['info_voices'], '2', 'каналы с нотами: 0 и 9')
        self.assertEqual((r['tags_samples'], r['tags_effects']), ('2', '2'),
                         'инструменты: фортепиано и набор ударных; дорожек две')
        self.assertEqual((r['view_kind'], r['view_duration']), ('1', '4'), 'спектр, 4 с')
        self.assertEqual((r['state'], r['finished']), ('0', '1'))
        peak = int(r['pcm_peak_permille'])
        self.assertGreater(peak, 50, 'не тишина')
        self.assertLessEqual(peak, 1000, 'без перегрузки')
        frames = int(r['pcm_frames'])
        self.assertGreaterEqual(frames, int(3.5 * RATE))
        self.assertLess(frames, int((4 + 3) * RATE), 'хвост отпускания не дольше 3 с')
        self.assertEqual(r['bank_releases'], '0')

    def test_midi_seek(self) -> None:
        if not BANK.is_file():
            self.skipTest(f'нет {BANK}')
        r = self.run_host('module', song_midi(30), midi=1, bank=BANK, seek=1)
        self.assertEqual(r['seek'], '1')
        self.assertGreaterEqual(int(r['seconds_after_seek']), 10, 'время — с нового места')
        self.assertEqual(r['finished'], '1')
        frames = int(r['pcm_frames'])
        self.assertLess(frames, int(24 * RATE), '10 с пропущены')

    def midi_pcm(self, events: list[tuple[int, bytes]]) -> tuple[list[int], list[int]]:
        """Звук песни (вывод стенда, 16 бит стерео): левый и правый каналы."""
        raw = pathlib.Path(self.temp.name) / 'midi.raw'
        self.run_host('module', midi_events(events), midi=1, bank=BANK, raw=raw)
        data = array.array('h')
        data.frombytes(raw.read_bytes())
        return list(data[0::2]), list(data[1::2])

    def test_midi_reverb_tail(self) -> None:
        if not BANK.is_file():
            self.skipTest(f'нет {BANK}')
        note = [(0, b'\xC0\x00'), (0, b'\x90\x3C\x64'), (192, b'\x80\x3C\x00'),
                (1920, b'\xB0\x07\x64')]
        tails = {}
        for label, send in (('dry', b'\xB0\x5B\x00'), ('default', b''), ('wet', b'\xB0\x5B\x7F')):
            left, right = self.midi_pcm(([(0, send)] if send else []) + note)
            tails[label] = energy(left, 1.0, 1.8) + energy(right, 1.0, 1.8)
        self.assertLess(tails['dry'] * 5, tails['wet'], tails)
        self.assertLess(tails['dry'], tails['default'], 'CC91 по умолчанию 40 — немного реверберации')
        self.assertLess(tails['default'], tails['wet'])

    def test_midi_echo(self) -> None:
        if not BANK.is_file():
            self.skipTest(f'нет {BANK}')
        hit = [(0, b'\xB9\x5B\x00'), (0, b'\x99\x4C\x7F'), (48, b'\x89\x4C\x00'),
               (1920, b'\xB9\x07\x64')]                      # деревянная коробочка, без реверберации
        late = {}
        for label, send in (('dry', b'\xB9\x5E\x00'), ('echo', b'\xB9\x5E\x7F')):
            left, right = self.midi_pcm([(0, send)] + hit)
            late[label] = energy(left, 0.28, 0.40)               # левое эхо — через 300 мс
        self.assertLess(late['dry'] * 10, late['echo'], late)

    def test_midi_chorus_spreads_the_channels(self) -> None:
        if not BANK.is_file():
            self.skipTest(f'нет {BANK}')
        tone = [(0, b'\xB0\x5B\x00'), (0, b'\xC0\x13'), (0, b'\x90\x3C\x64'),
                (960, b'\x80\x3C\x00'), (1440, b'\xB0\x07\x64')]   # орган, по центру
        side = {}
        for label, send in (('dry', b'\xB0\x5D\x00'), ('chorus', b'\xB0\x5D\x7F')):
            left, right = self.midi_pcm([(0, send)] + tone)
            difference = [a - b for a, b in zip(left, right)]
            side[label] = energy(difference, 0.2, 0.8) / max(1.0, energy(left, 0.2, 0.8))
        self.assertGreater(side['chorus'], side['dry'] * 3 + 0.01, side)

    def test_midi_portamento_glides(self) -> None:
        if not BANK.is_file():
            self.skipTest(f'нет {BANK}')
        result = {}
        for label, switch in (('off', b'\xB0\x41\x00'), ('on', b'\xB0\x41\x7F')):
            events = [(0, b'\xB0\x5B\x00'), (0, b'\xC0\x4F'), (0, switch), (0, b'\xB0\x05\x40'),
                      (0, b'\x90\x3C\x64'), (280, b'\xB0\x78\x00'), (288, b'\x90\x48\x64'),
                      (960, b'\x80\x48\x00'), (1440, b'\xB0\x07\x64')]   # окарина: C-4, затем C-5
            left, right = self.midi_pcm(events)
            mono = [(a + b) / 2 for a, b in zip(left, right)]
            result[label] = (pitch(mono, 0.33, 0.36, 240, 560), pitch(mono, 0.6, 0.65, 240, 560))
        early, final = result['off']
        self.assertGreater(early, final * 0.9, f'без портаменто — сразу C-5: {result}')
        early, final = result['on']
        self.assertAlmostEqual(final / 261.6, 2.0, delta=0.1, msg=f'дошло до C-5: {result}')
        self.assertLess(early, final * 0.8, f'с портаменто — ещё в пути: {result}')

    def test_midi_mod_wheel_vibrato(self) -> None:
        if not BANK.is_file():
            self.skipTest(f'нет {BANK}')
        spread = {}
        for label, wheel in (('still', b'\xB0\x01\x00'), ('vibrato', b'\xB0\x01\x7F')):
            events = [(0, b'\xB0\x5B\x00'), (0, b'\xC0\x4F'), (0, wheel), (0, b'\x90\x45\x64'),
                      (1152, b'\x80\x45\x00'), (1440, b'\xB0\x07\x64')]   # окарина A-4
            left, right = self.midi_pcm(events)
            mono = [(a + b) / 2 for a, b in zip(left, right)]
            values = [pitch(mono, 0.3 + k * 0.02, 0.33 + k * 0.02, 400, 480) for k in range(35)]
            spread[label] = (max(values) - min(values)) / (sum(values) / len(values))
        self.assertLess(spread['still'], 0.015, spread)
        self.assertGreater(spread['vibrato'], 0.03, spread)

    def test_midi_voices_on_two_cores_sound_as_on_one(self) -> None:
        # MIDI.ELF 1.3: голоса куска делят два ядра (помощник — часть голосов и
        # эффекты). Сухой звук — тот же, что когда всё считает ядро 0 (--no-worker):
        # разница только в порядке сложения float. Эффекты выключены — с помощником они
        # ложатся на кусок позже. С 1.7 зоны банка посылают в реверберацию и сами
        # (генераторы SF2 15, 16 — у большинства инструментов TimGM6mb), поэтому здесь
        # инструменты без них: гобой (68) аккордом и музыкальная шкатулка (10) дробью.
        if not BANK.is_file():
            self.skipTest(f'нет {BANK}')
        events = [(0, b'\xB0\x5B\x00'), (0, b'\xB1\x5B\x00'), (0, b'\xC0\x44'), (0, b'\xC1\x0A')]
        events += [(0, bytes([0x90, 36 + k, 0x50])) for k in range(0, 36, 3)]       # аккорд гобоя
        events += [(48 * k, bytes([0x91, 72 + k % 12, 0x7F])) for k in range(1, 30)]   # дробь шкатулки
        events += [(1500, b'\xB0\x7B\x00'), (2400, b'\xB0\x07\x64')]
        song = midi_events(events)
        pcm = {}
        for label, keys in (('two', {}), ('one', {'no-worker': 1})):
            raw = pathlib.Path(self.temp.name) / f'midi_{label}.raw'
            self.run_host('module', song, midi=1, bank=BANK, raw=raw, **keys)
            data = array.array('h')
            data.frombytes(raw.read_bytes())
            pcm[label] = data
            if label == 'two':
                self.assertIn('midi: core 1: effects', self.log)
                self.assertRegex(self.log, r'voices \d+ cycles per frame \(([1-9]\d*|0\.[1-9])')
            else:
                self.assertNotIn('midi: core 1', self.log)
        self.assertEqual(len(pcm['two']), len(pcm['one']))
        worst = max(abs(a - b) for a, b in zip(pcm['two'], pcm['one']))
        self.assertLessEqual(worst, 2, 'отсчёты 16 бит: разница не больше 2')
        self.assertGreater(max(abs(v) for v in pcm['two']), 1000, 'не тишина')

    def test_midi_notes_without_a_voice_are_counted(self) -> None:
        # 60 нот сразу при 48 голосах: всех голосов не хватает, гаснущих нет —
        # журнал говорит, сколько нот не прозвучало и когда были заняты все голоса.
        if not BANK.is_file():
            self.skipTest(f'нет {BANK}')
        events = [(0, b'\xC0\x13')] + [(0, bytes([0x90, 30 + k, 0x50])) for k in range(60)]
        events += [(960, b'\xB0\x7B\x00'), (1440, b'\xB0\x07\x64')]
        self.run_host('module', midi_events(events), midi=1, bank=BANK)
        self.assertRegex(self.log, r'midi: all 48 voices busy \d+ ms \(first at 0:00\), '
                                   r'([1-9]\d*) notes without a voice')

    def test_midi_pedal_voices_give_way_to_new_notes(self) -> None:
        # Под педалью 48 нот фортепиано (голос на ноту) отпущены, но звучат (sustain);
        # 5 новых нот берут самые тихие из них, а не теряются.
        if not BANK.is_file():
            self.skipTest(f'нет {BANK}')
        events = [(0, b'\xB0\x5B\x00'), (0, b'\xC0\x00'), (0, b'\xB0\x40\x7F')]
        events += [(0, bytes([0x90, 30 + k, 0x50])) for k in range(48)]
        events += [(96, bytes([0x80, 30 + k, 0x00])) for k in range(48)]
        events += [(192, bytes([0x90, 80 + k, 0x60])) for k in range(5)]
        events += [(960, b'\xB0\x7B\x00'), (960, b'\xB0\x40\x00'), (1440, b'\xB0\x07\x64')]
        self.run_host('module', midi_events(events), midi=1, bank=BANK)
        self.assertIn('midi: all 48 voices busy', self.log)
        self.assertIn(', 0 notes without a voice', self.log)

    def test_midi_sysex_does_not_shift_the_song(self) -> None:
        # Сброс GS и настройка XG (sysex) в начале: нота на 0,5 с звучит на 0,5 с. До
        # правки tml.h на стенде (MSVC) после каждого sysex время уезжало на секунды.
        if not BANK.is_file():
            self.skipTest(f'нет {BANK}')
        gs_reset = b'\xF0\x0A\x41\x10\x42\x12\x40\x00\x7F\x00\x41\xF7'
        xg_on = b'\xF0\x08\x43\x10\x4C\x00\x00\x7E\x00\xF7'
        events = [(0, gs_reset), (96, xg_on), (192, xg_on), (0, b'\xB0\x5B\x00'),
                  (0, b'\xC0\x00'), (480, b'\x90\x3C\x64'), (960, b'\x80\x3C\x00'),
                  (1920, b'\xB0\x07\x64')]
        left, right = self.midi_pcm(events)
        mono = [abs(a) + abs(b) for a, b in zip(left, right)]
        start = next(i for i, v in enumerate(mono) if v > 200) / RATE
        self.assertAlmostEqual(start, 0.5, delta=0.05)

    def test_midi_note_starts_at_its_own_frame(self) -> None:
        # MIDI.ELF 1.6: кусок (64 кадра) кончается на ноте внутри него, и нота звучит со
        # своего места (с точностью до 16 кадров), а не с начала куска. Тик 7 при PPQN 480 и
        # темпе 120 — 7,29 мс, в мс — 7: кадр 308,7, вниз до кратного 16 — 304. До 1.6 нота
        # применялась перед куском 256..320 и звучала с кадра 256.
        if not BANK.is_file():
            self.skipTest(f'нет {BANK}')
        onsets = []
        for tick in (0, 7):
            left, right = self.midi_pcm([(0, b'\xB9\x5B\x00'), (tick, b'\x99\x25\x7F'),
                                         (tick + 96, b'\x89\x25\x00'), (960, b'\xB9\x07\x64')])
            onsets.append(next(i for i, (a, b) in enumerate(zip(left, right)) if abs(a) + abs(b) > 30))
        self.assertEqual(onsets[1] - onsets[0], 304, onsets)

    def midi_raw(self, events: list[tuple[int, bytes]], **keys) -> tuple[dict[str, str], array.array]:
        """Песня из событий: итоги стенда и звук (16 бит стерео подряд)."""
        raw = pathlib.Path(self.temp.name) / 'midi17.raw'
        r = self.run_host('module', midi_events(events), midi=1, bank=BANK, raw=raw, **keys)
        data = array.array('h')
        data.frombytes(raw.read_bytes())
        return r, data

    def test_midi_end_releases_forgotten_notes(self) -> None:
        # MIDI.ELF 1.7: в конце событий клавиши и педали отпускаются — забытая нота (без
        # отпускания, под педалью) гаснет по своему отпусканию, а не держится до предела
        # хвоста и не обрывается на полном звуке.
        if not BANK.is_file():
            self.skipTest(f'нет {BANK}')
        events = [(0, b'\xB0\x5B\x00'), (0, b'\xC0\x44'), (0, b'\xB0\x40\x7F'), (0, b'\x90\x45\x64'),
                  (960, b'\xB0\x07\x64')]                                    # гобой, конец событий — 1 с
        r, data = self.midi_raw(events)
        frames = len(data) // 2
        self.assertEqual(r['finished'], '1')
        self.assertLess(frames, 4 * RATE, 'хвост — по отпусканию, а не 4 с до предела')
        self.assertGreater(max(abs(v) for v in data[: RATE * 2]), 1000, 'нота звучала')
        self.assertLess(max(abs(v) for v in data[-2048:]), 100, 'конец — тишина, не обрыв')

    def test_midi_seek_restores_held_notes(self) -> None:
        # MIDI.ELF 1.7: перемотка внутрь долгой ноты берёт её заново (до 1.7 после перемотки
        # было тихо до следующей ноты).
        if not BANK.is_file():
            self.skipTest(f'нет {BANK}')
        events = [(0, b'\xB0\x5B\x00'), (0, b'\xC0\x44'), (0, b'\x90\x45\x64'),
                  (960 * 25, b'\x80\x45\x00')]                               # гобой, 25 с
        r, data = self.midi_raw(events, seek=1)
        self.assertEqual(r['seek'], '1')
        at = int(r['frames_at_seek'])
        after = data[2 * (at + RATE): 2 * (at + 5 * RATE)]
        self.assertGreater(max(abs(v) for v in after), 1000, 'после перемотки нота звучит')

    def test_midi_master_volume_sysex(self) -> None:
        # Общая громкость GS (SysEx 40 00 04): 64 из 127 — вдвое тише.
        if not BANK.is_file():
            self.skipTest(f'нет {BANK}')
        gs_reset = b'\xF0\x0A\x41\x10\x42\x12\x40\x00\x7F\x00\x41\xF7'
        peaks = {}
        for label, volume in (('full', 0x7F), ('half', 0x40)):
            checksum = (128 - (0x40 + 0x00 + 0x04 + volume) % 128) % 128
            master = bytes([0xF0, 0x0A, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x04, volume, checksum, 0xF7])
            events = [(0, gs_reset), (1, master), (2, b'\xB0\x5B\x00'), (2, b'\xC0\x44'),
                      (96, b'\x90\x45\x50'), (960, b'\x80\x45\x00'), (1440, b'\xB0\x07\x64')]
            left, right = self.midi_pcm(events)
            peaks[label] = max(abs(v) for v in left)
        self.assertAlmostEqual(peaks['half'] / peaks['full'], 64 / 127, delta=0.03, msg=peaks)

    def test_midi_gs_rhythm_part_and_xg_drums(self) -> None:
        # Ударные не на канале 9: партия GS, назначенная ударной (SysEx 40 11 15), и канал XG
        # с банком 127 звучат тем же набором, что канал 9; без этого — фортепиано.
        if not BANK.is_file():
            self.skipTest(f'нет {BANK}')
        gs_reset = b'\xF0\x0A\x41\x10\x42\x12\x40\x00\x7F\x00\x41\xF7'
        xg_on = b'\xF0\x08\x43\x10\x4C\x00\x00\x7E\x00\xF7'
        rhythm = b'\xF0\x0A\x41\x10\x42\x12\x40\x11\x15\x01\x19\xF7'   # партия 1 (канал 0) — ударные
        tail = [(960, b'\xB0\x07\x64')]
        def song(reset: bytes, setup: list[tuple[int, bytes]], channel: int) -> list[int]:
            events = [(0, reset)] + setup + [(10, bytes([0xC0 | channel, 0])), (10, bytes([0xB0 | channel, 0x5B, 0])),
                                             (96, bytes([0x90 | channel, 38, 0x64]))] + tail
            left, right = self.midi_pcm(events)
            return left + right
        drums = song(gs_reset, [], 9)
        self.assertEqual(song(gs_reset, [(1, rhythm)], 0), drums, 'партия GS, назначенная ударной')
        self.assertNotEqual(song(gs_reset, [], 0), drums, 'без назначения канал 0 — не ударные')
        self.assertEqual(song(xg_on, [(1, b'\xB0\x00\x7F')], 0), song(xg_on, [], 9), 'XG: банк 127 — набор ударных')

    def test_midi_limiter_is_linked(self) -> None:
        # Ограничитель 1.7 общий для обоих каналов: громкий аккорд слева убавляет и тихую
        # ноту справа (стереобаланс не плывёт); прежде каждый канал сжимался сам.
        if not BANK.is_file():
            self.skipTest(f'нет {BANK}')
        quiet = [(0, b'\xB1\x5B\x00'), (0, b'\xC1\x44'), (0, b'\xB1\x0A\x7F'), (0, b'\x91\x51\x40'),
                 (1920, b'\x81\x51\x00'), (2400, b'\xB0\x07\x64')]                # гобой справа
        loud = [(0, b'\xB0\x5B\x00'), (0, b'\xC0\x44'), (0, b'\xB0\x0A\x00'), (0, b'\xB0\x07\x7F')]
        loud += [(0, bytes([0x90, 48 + k, 0x7F])) for k in range(16)]               # аккорд гобоя слева
        loud += [(1920, bytes([0x80, 48 + k, 0x00])) for k in range(16)]
        left_alone, right_alone = self.midi_pcm(quiet)
        left_both, right_both = self.midi_pcm(quiet + loud)
        self.assertGreater(max(abs(v) for v in left_both), int(0.85 * 32768), 'слева ограничитель работает')
        ratio = energy(right_both, 0.5, 1.5) / energy(right_alone, 0.5, 1.5)
        self.assertLess(ratio, 0.8, f'справа убавлено тем же усилением: {ratio}')

    def test_midi_without_bank(self) -> None:
        self.assert_skipped(self.run_host('module', song_midi(2), midi=1))

    def check_opl(self, r: dict[str, str], seconds: int) -> None:
        self.assertEqual(r['open'], '1')
        self.assertEqual(r['loaded_state'], str(STATE_PLAYING), r)
        self.assertEqual(r['info_format'], str(FORMAT_OPL))
        self.assertEqual(r['tags_extra'], 'YM3812 (OPL2)')
        self.assertEqual(r['view_kind'], '2', 'у OPL — уровни каналов')
        self.assertEqual(r['view_count'], '2', 'каналы с нотами: 0 и 1')
        self.assertEqual(r['view_duration'], str(seconds))
        self.assertEqual((r['state'], r['finished']), (str(STATE_IDLE), '1'))
        self.assertGreater(int(r['pcm_peak_permille']), 30)


    def test_tsfm_tfc_format_view_and_end(self):
        from test_tsfm_source import make_tfc
        for rate in (50, 60):
            with self.subTest(rate=rate):
                r = self.run_host('module', make_tfc(rate), ext='TFC')
                self.assertEqual(r['info_format'], '9', r)
                self.assertEqual(r['tags_title'], 'TSFM test')
                self.assertEqual(r['tags_artist'], 'Test author')
                self.assertEqual(r['view_kind'], '2')
                self.assertEqual(r['view_count'], '6')
                self.assertEqual(r['finished'], '1')
                self.assertEqual(int(r['pcm_frames']), 97 * TSFM_FM_RATE // rate)

    def test_tsfm_truncated_input_is_skipped(self):
        from test_tsfm_source import make_tfc
        self.assert_skipped(self.run_host('module', make_tfc()[:-1], ext='TFC'))

    def test_tsfm_seek_in_seconds(self):
        from test_tsfm_source import make_tfc
        r = self.run_host('module', make_tfc(blocks=30), ext='TFC', seek=1)
        self.assertEqual(r['seek'], '1')
        self.assertGreaterEqual(int(r['seconds_after_seek']), 10)
        self.assertEqual(r['finished'], '1')

    def test_tsfm_tfd_frames_tags_and_end(self):
        # TurboFM Dump: кадры по 20 мс, название и автор из строк заголовка.
        from test_tsfm_source import make_tfd
        r = self.run_host('module', make_tfd(frames=200), ext='TFD')
        self.assertEqual(r['info_format'], '9', r)
        self.assertEqual(r['tags_title'], 'TFD test')
        self.assertEqual(r['tags_artist'], 'TFD author')
        self.assertEqual(r['view_count'], '6')
        self.assertEqual(r['finished'], '1')
        self.assertEqual(int(r['pcm_frames']), 200 * TSFM_FM_RATE // 50)
        self.assertGreater(int(r['pcm_peak_permille']), 30, 'голос FM звучит')

    def test_tsfm_mtc_tracks_play_together(self):
        # MTC: дорожки TFC и TFD одновременно — четыре YM2203, 12 каналов; длина —
        # по самой длинной; название и автор — из блоков контейнера.
        from test_tsfm_source import make_mtc, make_tfc, make_tfd
        mtc = make_mtc([(make_tfc(blocks=3), b'a.tfc'), (make_tfd(frames=300), b'b.tfd')])
        r = self.run_host('module', mtc, ext='MTC')
        self.assertEqual(r['info_format'], '9', r)
        self.assertEqual(r['tags_title'], 'MTC test')
        self.assertEqual(r['tags_artist'], 'MTC author')
        self.assertEqual(r['view_count'], '12')
        self.assertEqual(r['finished'], '1')
        self.assertEqual(int(r['pcm_frames']), 300 * TSFM_FM_RATE // 50)

    def test_tsfm_mtc_without_playable_tracks_is_skipped(self):
        from test_tsfm_source import make_mtc
        self.assert_skipped(self.run_host('module', make_mtc([(b'junk' * 100, b'x.bin')]), ext='MTC'))
        self.assert_skipped(self.run_host('module', b'MTC1' + bytes(200), ext='MTC'))

    def test_saa_formats_view_and_end(self):
        from test_saa_source import make_saa
        for extension in ('SAA','COP','MOD'):
            with self.subTest(extension=extension):
                r=self.run_host('module',make_saa(),ext=extension)
                self.assertEqual(r['info_format'],'8',r)
                self.assertEqual(r['tags_extra'],'SAA1099 E-Tracker')
                self.assertEqual(r['view_kind'],'2')
                self.assertEqual(r['view_count'],'6')
                self.assertEqual(r['finished'],'1')
                self.assertEqual(int(r['pcm_frames']),19200*RATE//1000)

    def test_saa_seek(self):
        from test_saa_source import make_saa
        r=self.run_host('module',make_saa(),ext='SAA',seek=1)
        self.assertEqual(r['seek'],'1')
        self.assertGreaterEqual(int(r['seconds_after_seek']),10)
        self.assertEqual(r['finished'],'1')

    def test_saa_invalid_input_is_skipped(self):
        self.assert_skipped(self.run_host('module',b'ETracker'+bytes(100),ext='COP'))

    def test_opl_vgm_tags_view_and_end(self) -> None:
        tags = gd3('VDAC4 OPL test', '', 'Test Game', '', 'IBM PC', '', 'Tester', '',
                   '1994/05/01', 'ripper', 'notes')
        # Окно посреди звука — в реальном времени (стенд по умолчанию в 100 раз быстрее).
        r = self.run_host('module', opl_vgm(3, tags), ext='VGM', late=300, speed=1)
        self.check_opl(r, 3)
        self.assertEqual(r['tags_title'], 'VDAC4 OPL test')
        self.assertEqual(r['tags_artist'], 'Tester')
        self.assertEqual(r['tags_album'], 'Test Game')
        self.assertEqual(r['tags_year'], '1994')
        self.assertTrue(any(int(v) > 0 for v in r['view_late'].split(',')), r['view_late'])
        self.assertEqual(r['loaded_wants_rom'], '0')

    def test_opl_vgm_with_a_1_hz_clock_is_refused(self) -> None:
        """Такт микросхемы 1 Гц: её частота вывода — ноль, и длительность делилась бы на
        ноль. Такая микросхема отвергается, файл не играет, но и не роняет PLAYER."""
        vgm = bytearray(opl_vgm(2))
        struct.pack_into('<I', vgm, 0x50, 1)
        r = self.run_host('module', bytes(vgm), ext='VGM')
        self.assertEqual(r['finished'], '1')
        self.assertIn('refused', self.log)

    def test_opl_vgz_and_damaged_gzip(self) -> None:
        r = self.run_host('module', gzip.compress(opl_vgm(3)), ext='VGZ')
        self.check_opl(r, 3)
        damaged = bytearray(gzip.compress(opl_vgm(3)))
        damaged[len(damaged) // 2] ^= 0xFF
        self.assert_skipped(self.run_host('module', bytes(damaged), ext='VGZ'))

    def test_opl_dro(self) -> None:
        r = self.run_host('module', opl_dro(3), ext='DRO')
        self.check_opl(r, 3)

    def test_opl_key_toggle_in_one_millisecond_restarts_the_note(self) -> None:
        # Как удары в doofus.dro: клавишу отпустили и сразу нажали — в DRO в одну
        # миллисекунду. Настоящая микросхема видит отпускание (между записями
        # не меньше 23 мкс), и звук берётся заново; движок, что смотрит на клавишу
        # раз в такт, видел только «нажата» и ударов не давал (OPL.ELF до 1.3).
        codes = [0x20, 0x23, 0x40, 0x43, 0x60, 0x63, 0x80, 0x83, 0xA0, 0xB0]
        pairs = bytearray()

        def put(reg, value):
            pairs.extend(bytes([codes.index(reg), value]))

        # Ударный голос: несущая без удержания (EG-TYP 0), быстрая атака, спад.
        for reg, value in ((0x20, 0x01), (0x23, 0x01), (0x40, 0x3F), (0x43, 0x00),
                           (0x60, 0xF5), (0x63, 0xF5), (0x80, 0xF5), (0x83, 0xF5), (0xA0, 0x44)):
            put(reg, value)
        put(0xB0, 0x20 | (4 << 2) | 1)
        hits = 4
        for _ in range(hits):
            pairs.extend(bytes([0xFF, 1]))           # 512 мс
            put(0xB0, (4 << 2) | 1)                  # отпустить
            put(0xB0, 0x20 | (4 << 2) | 1)           # и в ту же миллисекунду нажать
        pairs.extend(bytes([0xFF, 1]))
        length_ms = (hits + 1) * 512
        head = b'DBRAWOPL' + struct.pack('<HHII', 2, 0, len(pairs) // 2, length_ms)
        head += bytes([0, 0, 0, 0xFE, 0xFF, len(codes)]) + bytes(codes)
        raw = pathlib.Path(self.temp.name) / 'toggle.raw'
        r = self.run_host('module', head + bytes(pairs), ext='DRO', raw=raw)
        self.assertEqual(r['loaded_state'], str(STATE_PLAYING), r)
        left = array.array('h', raw.read_bytes())[0::2]
        rate = 49715                                 # OPL2: 3 579 545 / 72

        def level(ms_from, ms_to):
            part = left[int(ms_from * rate / 1000):int(ms_to * rate / 1000)]
            return max(abs(v) for v in part)

        first = level(2, 30)
        self.assertGreater(first, 3000, 'первый удар звучит')
        for hit in range(1, hits + 1):
            at = hit * 512
            before = level(at - 30, at - 2)
            after = level(at + 2, at + 30)
            self.assertLess(before, first // 4, f'к удару {hit} прежний затих')
            self.assertGreater(after, first // 2, f'удар {hit} взят заново')

    def test_opl_imf_by_extension(self) -> None:
        r = self.run_host('module', opl_imf(3), ext='IMF')
        self.check_opl(r, 3)
        # Тот же файл как .WLF — 700 Гц: короче в 560/700 раз.
        r = self.run_host('module', opl_imf(3), ext='WLF')
        self.assertEqual(r['loaded_state'], str(STATE_PLAYING))
        self.assertEqual(r['view_duration'], '2')
        # Без расширения IMF не узнать: его берёт трекер и отвергает.
        self.assert_skipped(self.run_host('module', opl_imf(3)))

    def test_opl_seek(self) -> None:
        r = self.run_host('module', opl_vgm(30), ext='VGM', seek=1, seconds=12)
        self.assertEqual(r['seek'], '1')
        self.assertGreaterEqual(int(r['seconds_after_seek']), 9)
        self.assertLessEqual(int(r['seconds_after_seek']), 11)

    def test_opl4_asks_for_the_rom(self) -> None:
        r = self.run_host('module', opl_vgm(2, opl4=True), ext='VGM')
        self.assertEqual(r['loaded_state'], str(STATE_PLAYING))
        self.assertEqual(r['tags_extra'], 'YMF278B (OPL4)')
        self.assertEqual(r['loaded_wants_rom'], '1', 'ПЗУ нет — флаг для плагина')
        rom = pathlib.Path(self.temp.name) / 'rom.bin'
        rom.write_bytes(bytes(2 * 1024 * 1024))
        r = self.run_host('module', opl_vgm(2, opl4=True), ext='VGM', rom=rom)
        self.assertEqual(r['loaded_wants_rom'], '0')

    def test_wav_formats(self) -> None:
        # (бит, формат, каналов, частота, байты отсчёта «вверх» и «вниз», пик в тысячных, каналов на выходе)
        cases = [
            (16, 1, 2, 44100, struct.pack('<h', 16384), struct.pack('<h', -16384), 500, 2),
            (8, 1, 1, 22050, bytes([192]), bytes([64]), 500, 1),
            (24, 1, 2, 48000, (4194304).to_bytes(3, 'little', signed=True), (-4194304).to_bytes(3, 'little', signed=True), 500, 2),
            (32, 1, 2, 44100, struct.pack('<i', 1 << 30), struct.pack('<i', -(1 << 30)), 500, 2),
            (32, 3, 2, 96000, struct.pack('<f', 0.25), struct.pack('<f', -0.25), 250, 2),   # прорежено до 48 кГц
            (64, 3, 1, 32000, struct.pack('<d', 0.75), struct.pack('<d', -0.75), 750, 1),
            (8, 7, 1, 8000, bytes([0x80]), bytes([0x00]), 980, 1),      # µ-law ±32124
            (8, 6, 1, 8000, bytes([0xAA]), bytes([0x2A]), 984, 1),      # A-law ±32256
        ]
        for bits, fmt, channels, rate, high, low, peak, out_channels in cases:
            with self.subTest(bits=bits, fmt=fmt, channels=channels):
                frames = rate // 2
                wav = make_wav(wav_square(frames, channels, bits, fmt, high, low), rate, channels, bits, fmt)
                r = self.run_host('mp3', wav)
                factor = 1
                while rate // factor > 48000:         # выше 48 кГц — прореживание целым кратным
                    factor *= 2
                self.assertEqual((r['state'], r['finished'], r['decoded']), ('0', '1', '1'), r)
                self.assertEqual(int(r['pcm_frames']), frames // factor, 'все кадры, без хвоста за data')
                self.assertEqual(r['pcm_discarded'], '0', 'и все проиграны: при закрытии не выброшено')
                self.assertEqual((r['rate'], r['channels']), (str(rate // factor), str(out_channels)))
                self.assertEqual(r['bitrate'], str(rate * channels * bits // 1000))
                self.assertEqual(r['format'], str(FORMAT_MP3), 'для Z80 поток — как MP3')
                self.assertAlmostEqual(int(r['pcm_peak_permille']), peak, delta=2)

    def test_wav_extensible_six_channels_downmix(self) -> None:
        frames = 24000
        wav = make_wav(wav_square(frames, 6, 16, 1, struct.pack('<h', 8192), struct.pack('<h', -8192)),
                       48000, 6, 16, 1, extensible=True)
        r = self.run_host('mp3', wav)
        self.assertEqual((r['finished'], r['decoded']), ('1', '1'))
        self.assertEqual((r['rate'], r['channels']), ('48000', '2'), '5.1 — в стерео')
        self.assertEqual(int(r['pcm_frames']), frames)
        self.assertAlmostEqual(int(r['pcm_peak_permille']), 250, delta=2, msg='сведение не громче отсчёта')

    def test_wav_tags_from_list_info(self) -> None:
        wav = make_wav(bytes(44100 * 4 * 4), 44100, 2, 16,
                       info={'INAM': 'Test Take', 'IART': 'VDAC4', 'IPRD': 'Demo Album', 'ICRD': '2026-10-05'})
        r = self.run_host('mp3', wav, speed=1)
        self.assertEqual((r['stream_title'], r['stream_artist'], r['stream_album'], r['stream_year']),
                         ('Test Take', 'VDAC4', 'Demo Album', '2026'))

    def test_wav_flush_lands_on_a_frame(self) -> None:
        # 24 бита стерео: левый отсчёт кадра i — i (в 16 битах вывода), правый — минус
        # (до 16000 кадров: дальше i в 16 битах округляется вниз).
        frames = 12000
        data = bytearray()
        for i in range(frames):
            value = i * 256 + 128
            data += value.to_bytes(3, 'little', signed=True) + (-value).to_bytes(3, 'little', signed=True)
        wav = make_wav(bytes(data), 8000, 2, 24)
        start = wav.index(b'data') + 8
        raw = pathlib.Path(self.temp.name) / 'wav.raw'
        resume = start + 1000 * 6 + 4                 # не на границе кадра
        r = self.run_host('flush', wav, cut=start + 500 * 6, resume=resume, base=125000, raw=raw)
        self.assertEqual(r['flush'], '1')
        left = array.array('h', raw.read_bytes())[0::2]
        jumps = [k for k in range(1, len(left)) if left[k] != left[k - 1] + 1]
        self.assertEqual(len(jumps), 1, 'один разрыв — перемотка')
        k = jumps[0]
        self.assertEqual(left[k], 1001, 'с начала следующего целого кадра')
        self.assertEqual(list(left[k:k + 5]), [1001, 1002, 1003, 1004, 1005])
        self.assertEqual(r['seconds_after_flush'], '125')

    def kit_file(self) -> pathlib.Path:
        kit = pathlib.Path(self.temp.name) / 'kit.mwk'
        kit.write_bytes(mwm_kit())
        return kit

    def test_mwm_with_its_kit(self) -> None:
        # 2 позиции по 16 шагов, темп 6, 50 Гц: 192 тика по 248 отсчётов таймера.
        r = self.run_host('module', mwm_song(), ext='MWM', resource=f'TESTKIT.MWK={self.kit_file()}')
        self.assertEqual(r['open'], '1')
        self.assertEqual(r['loaded_state'], str(STATE_PLAYING), r)
        self.assertEqual(r['info_format'], str(FORMAT_OPL))
        self.assertEqual(r['tags_title'], 'VDAC4 MWM test')
        self.assertEqual(r['tags_album'], 'Kit TESTKIT.MWK')
        self.assertEqual(r['tags_extra'], 'YMF278B (OPL4)')
        self.assertEqual(r['loaded_wants_rom'], '1', 'патч ПЗУ — плагину флаг')
        self.assertEqual(r['view_count'], '2', 'каналы с нотами: 0 и 1')
        self.assertEqual(r['view_duration'], str(192 * 248 * 57 // 16 // RATE))
        self.assertEqual((r['state'], r['finished']), (str(STATE_IDLE), '1'))
        self.assertGreater(int(r['pcm_peak_permille']), 30, 'своя волна звучит и без ПЗУ')

    def test_mwm_without_its_kit_is_silent(self) -> None:
        r = self.run_host('module', mwm_song(), ext='MWM')
        self.assertEqual(r['loaded_state'], str(STATE_PLAYING), r)
        self.assertEqual(r['tags_album'], 'Kit TESTKIT.MWK missing')
        self.assertEqual(r['pcm_peak_permille'], '0', 'нет набора и ПЗУ — тишина, не шум')
        damaged = pathlib.Path(self.temp.name) / 'damaged.mwk'
        damaged.write_bytes(mwm_kit()[:200])
        r = self.run_host('module', mwm_song(), ext='MWM', resource=f'TESTKIT.MWK={damaged}')
        self.assertEqual(r['tags_album'], 'Kit TESTKIT.MWK damaged')
        self.assertEqual(r['pcm_peak_permille'], '0')

    def test_mwm_author_after_the_padding(self) -> None:
        name = 'Darkness'.ljust(36) + 'Near Dark 1997'
        r = self.run_host('module', mwm_song(name=name, kit='NONE'), ext='MWM')
        self.assertEqual((r['tags_title'], r['tags_artist']), ('Darkness', 'Near Dark 1997'))
        self.assertEqual(r['tags_album'], '', 'без набора — ни слова о нём')

    def test_mwm_loop_fades(self) -> None:
        r = self.run_host('module', mwm_song(loop=0), ext='MWM', resource=f'TESTKIT.MWK={self.kit_file()}')
        self.assertEqual(r['view_duration'], str((192 * 248 * 57 // 16 + 8 * RATE) // RATE),
                         'повтор: ещё 8 с с затуханием')

    def test_real_mfm_plays_fm_without_the_rom(self) -> None:
        song = pathlib.Path(r'E:\zx\esp\downloads\zxm-moonsound\demo-disks\mfm_sample_02\DJINGLE1.MFM')
        if not song.exists():
            self.skipTest(f'нет {song}')
        r = self.run_host('module', path=song, ext='MFM')
        self.assertEqual(r['loaded_state'], str(STATE_PLAYING), r)
        self.assertEqual(r['info_format'], str(FORMAT_OPL))
        self.assertEqual(r['tags_extra'], 'YMF278B (OPL4)')
        self.assertEqual(r['loaded_wants_rom'], '1', 'дорожки волн — патчи ПЗУ')
        self.assertGreater(int(r['view_count']), 3, 'каналы FM с нотами')
        self.assertEqual((r['state'], r['finished']), (str(STATE_IDLE), '1'))
        self.assertGreater(int(r['pcm_peak_permille']), 30, 'FM звучит и без ПЗУ')

    def test_release_frees_the_kit_too(self) -> None:
        r = self.run_host('module', mwm_song(), ext='MWM', resource=f'TESTKIT.MWK={self.kit_file()}',
                          release=1)
        self.assertEqual(r['release_open'], '1')
        self.assertEqual(r['resource_releases'], '1', 'AUDIO_OPEN [#20] освобождает и набор')

    def test_release_frees_the_bank_and_the_rom(self) -> None:
        rom = pathlib.Path(self.temp.name) / 'rom.bin'
        rom.write_bytes(bytes(1024))
        keys = {'rom': rom}
        if BANK.exists():
            keys['bank'] = BANK
        r = self.run_host('release', **keys)
        self.assertEqual(r['open'], '1')
        self.assertEqual(r['rom_releases'], '1')
        self.assertEqual(r['bank_releases'], '1' if BANK.exists() else '0')

    def test_real_xm(self) -> None:
        if not MUSIC.is_dir():
            self.skipTest(f'нет {MUSIC}')
        small = [path for path in sorted(MUSIC.rglob('*.xm')) if path.stat().st_size < 400_000]
        for path in small[:3]:
            with self.subTest(file=path.name):
                r = self.run_host('module', path=path, seconds=3)
                self.assertEqual(r['loaded_state'], str(STATE_PLAYING))
                self.assertEqual(r['loaded_format'], str(FORMAT_XM))
                self.assertGreater(int(r['pcm_peak_permille']), 0, 'не тишина')
                self.assertGreaterEqual(int(r['pcm_frames']), 3 * RATE)


class CodecFileTest(unittest.TestCase):
    """AAC.ELF и OGG.ELF: файлы с карты потоком, как MP3 (расширение — в AUDIO_OPEN)."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.exe = build()
        if cls.exe is None:
            raise unittest.SkipTest('нет MSVC (vcvars32.bat): стенд PLAYER.ELF пропущен')
        if FFMPEG is None:
            raise unittest.SkipTest('нет ffmpeg: файлы AAC и Ogg не сделать')
        cls.temp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temp.cleanup()

    run_host = PlayerHostTest.run_host

    def play(self, path: pathlib.Path, ext: str, **keys) -> dict[str, str]:
        keys.setdefault('speed', 20)
        r = self.run_host('mp3', path=path, ext=ext, sized=1, **keys)
        self.assertEqual(r['open'], '1')
        return r

    def assert_length(self, r: dict[str, str], seconds: float, rate: int = 44100) -> None:
        frames = int(r['pcm_frames'])
        self.assertLess(abs(frames - seconds * rate), 4096, f'{frames} кадров на {seconds} с')

    def test_aac_file_plays_to_the_end(self) -> None:
        path = encoded('tone.aac', '-ac', '2', '-c:a', 'aac', '-b:a', '128k', '-f', 'adts',
                       inputs=tone(3))
        r = self.play(path, 'AAC')
        self.assertEqual((r['finished'], r['decoded'], r['errors']), ('1', '1', '0'))
        self.assertEqual((r['rate'], r['channels'], r['format']), ('44100', '2', '1'))
        self.assert_length(r, 3)
        durations = r['stream_durations'].split(',')
        self.assertEqual(durations[1:], ['3', '3'], 'длительность — по битрейту кадров')
        self.assertIn(durations[0], ('0', '3'), 'до первого кадра — неизвестна')
        self.assertIn('(AAC.ELF)', self.log)
        self.assertRegex(self.log, r'played to the end: AAC \d+ kbps')

    def test_aac_file_with_id3_tag(self) -> None:
        path = encoded('tone.aac', '-ac', '2', '-c:a', 'aac', '-b:a', '128k', '-f', 'adts',
                       inputs=tone(3))
        tagged = pathlib.Path(self.temp.name) / 'tagged.aac'
        tagged.write_bytes(id3_tag(id3_frame(b'TIT2', b'\x00Tagged')) + path.read_bytes())
        r = self.play(tagged, 'AAC')
        self.assertEqual((r['finished'], r['errors']), ('1', '0'))
        self.assert_length(r, 3)
        # Название из ID3 плагин даёт кадрами AUDIO_COVER (cover_glue), здесь
        # проверяется только, что метка в потоке пропущена.

    def test_ogg_file_plays_with_vorbis_comments(self) -> None:
        path = encoded('tagged.ogg', '-ac', '2', '-c:a', 'libvorbis', '-q:a', '4',
                       '-metadata', 'title=Song One', '-metadata', 'artist=Band',
                       '-metadata', 'album=Album X', '-metadata', 'date=1999-05-01',
                       inputs=tone(3, 440))
        r = self.play(path, 'OGG')
        self.assertEqual((r['finished'], r['decoded'], r['errors']), ('1', '1', '0'))
        self.assertEqual((r['rate'], r['channels'], r['format']), ('44100', '2', '1'))
        self.assert_length(r, 3)
        self.assertEqual((r['stream_title'], r['stream_artist'], r['stream_album'],
                          r['stream_year']), ('Song One', 'Band', 'Album X', '1999'))
        self.assertIn('(OGG.ELF)', self.log)

    def test_ogg_mono_and_surround(self) -> None:
        mono = encoded('mono.ogg', '-ac', '1', '-c:a', 'libvorbis', inputs=tone(2, 500, 22050))
        r = self.play(mono, 'OGG')
        self.assertEqual((r['rate'], r['channels'], r['errors']), ('22050', '1', '0'))
        self.assert_length(r, 2, 22050)
        surround = encoded('six.ogg', '-af', 'pan=5.1|c0=c0|c1=c0|c2=c0|c3=c0|c4=c0|c5=c0',
                           '-c:a', 'libvorbis', inputs=tone(2, 500))
        r = self.play(surround, 'OGA')
        self.assertEqual((r['channels'], r['errors'], r['finished']), ('2', '0', '1'),
                         '5.1 — в стерео')
        self.assert_length(r, 2)

    def test_ogg_cover_from_vorbis_comments(self) -> None:
        # METADATA_BLOCK_PICTURE: base64 блока картинки FLAC; «JPEG» стенда — FF D8 FF и
        # цвет (белый — цвет 15 миниатюры). Сначала задняя обложка, лицевая важнее.
        def block(kind: int, picture: bytes) -> str:
            mime = b'image/jpeg'
            data = (struct.pack('>II', kind, len(mime)) + mime + struct.pack('>I', 0) +
                    struct.pack('>IIIII', 500, 500, 24, 0, len(picture)) + picture)
            return __import__('base64').b64encode(data).decode('ascii')
        white = b'\xFF\xD8\xFF' + bytes([255, 255, 255]) + bytes(3000)
        black = b'\xFF\xD8\xFF' + bytes([0, 0, 0]) + bytes(3000)
        meta = pathlib.Path(self.temp.name) / 'cover_meta.txt'
        meta.write_text(';FFMETADATA1\nmetadata_block_picture=' + block(4, black) +
                        '\nMETADATA_BLOCK_PICTURE=' + block(3, white) + '\ntitle=Covered\n',
                        encoding='ascii')
        path = encoded('covered.ogg', '-map_metadata', '1', '-ac', '2', '-c:a', 'libvorbis',
                       inputs=tone(2) + ('-f', 'ffmetadata', '-i', str(meta)))
        r = self.play(path, 'OGG', **{'cover-wait': 1})
        self.assertEqual(r['cover_flag'], '1', 'флаг 64: обложку нашёл декодер')
        self.assertEqual(r['thumb_state'], '1')
        self.assertEqual((r['thumb_uniform'], r['thumb_byte']), ('1', str(0xFF)),
                         'лицевая (белая), не задняя')
        self.assertEqual(r['cover_flag_after_thumb'], '0', 'AUDIO_THUMB снимает флаг')
        self.assertIn('cover from the decoder', self.log)
        self.assertEqual((r['finished'], r['errors'], r['stream_title']), ('1', '0', 'Covered'))

    def test_ogg_without_cover_raises_no_flag(self) -> None:
        path = encoded('tagged.ogg', '-ac', '2', '-c:a', 'libvorbis', '-q:a', '4',
                       '-metadata', 'title=Song One', '-metadata', 'artist=Band',
                       '-metadata', 'album=Album X', '-metadata', 'date=1999-05-01',
                       inputs=tone(3, 440))
        r = self.play(path, 'OGG', **{'cover-wait': 1})
        self.assertEqual(r['cover_flag'], '0')

    def test_ogg_opus_file_is_refused(self) -> None:
        path = encoded('tone.opus', '-c:a', 'libopus', '-b:a', '64k', inputs=tone(2))
        r = self.play(path, 'OGG')
        self.assertEqual((r['finished'], r['decoded'], r['pcm_frames']), ('1', '0', '0'))
        self.assertEqual(r['stream_title'], 'Opus is not supported')

    def test_huge_comment_header_grows_the_buffers(self) -> None:
        # Комментарий в 200 КБ (так бывает с обложкой METADATA_BLOCK_PICTURE).
        meta = pathlib.Path(self.temp.name) / 'meta.txt'
        meta.write_text(';FFMETADATA1\ncomment=' + 'x' * 200000 + '\ntitle=Big\n', encoding='ascii')
        path = encoded('big.ogg', '-map_metadata', '1', '-ac', '2', '-c:a', 'libvorbis',
                       inputs=tone(2) + ('-f', 'ffmetadata', '-i', str(meta)))
        self.assertGreater(path.stat().st_size, 200000)
        r = self.play(path, 'OGG')
        self.assertEqual((r['finished'], r['errors'], r['stream_title']), ('1', '0', 'Big'))
        self.assert_length(r, 2)

    def test_ogg_seek_restarts_from_saved_headers(self) -> None:
        # Битрейт постоянный: длительность Ogg — оценка по нему (последней страницы
        # с числом отсчётов поток с начала не видит).
        path = encoded('long.ogg', '-ac', '2', '-c:a', 'libvorbis', '-b:a', '128k',
                       '-minrate', '128k', '-maxrate', '128k', '-metadata', 'title=Long',
                       inputs=tone(10, 440))
        size = path.stat().st_size
        r = self.run_host('flush', path=path, ext='OGG', sized=1, speed=20,
                          cut=size * 3 // 10, resume=size * 6 // 10, base=6000)
        self.assertEqual((r['flush'], r['seconds_after_flush']), ('1', '6'))
        self.assertEqual((r['finished'], r['errors']), ('1', '0'))
        after = int(r['pcm_frames']) - int(r['frames_before_flush'])
        self.assertGreater(after, 3 * 44100, 'после перемотки звук пошёл с заголовков начала')
        self.assertEqual(r['stream_title'], 'Long', 'метки — тоже с начала файла')
        durations = {int(d) for d in r['stream_durations'].split(',')}
        self.assertTrue(durations <= {9, 10, 11}, f'{durations}: длительность — по всему '
                        'файлу (оценка по битрейту), а не от нового места')

    def test_aac_seek_finds_the_next_frame(self) -> None:
        path = encoded('long.aac', '-ac', '2', '-c:a', 'aac', '-b:a', '128k', '-f', 'adts',
                       inputs=tone(10))
        size = path.stat().st_size
        r = self.run_host('flush', path=path, ext='AAC', sized=1, speed=20,
                          cut=size * 3 // 10, resume=size * 6 // 10 + 3, base=6000)
        self.assertEqual((r['flush'], r['seconds_after_flush']), ('1', '6'))
        self.assertEqual((r['finished'], r['errors']), ('1', '0'))
        after = int(r['pcm_frames']) - int(r['frames_before_flush'])
        self.assertGreater(after, 3 * 44100)
        durations = {int(d) for d in r['stream_durations'].split(',')}
        self.assertTrue(durations <= {9, 10, 11}, f'{durations}: длительность — по всему '
                        'файлу (оценка по битрейту), а не от нового места')

    def test_missing_decoder_refuses_the_file(self) -> None:
        for ext in ('AAC', 'OGG'):
            with self.subTest(ext=ext):
                r = self.run_host('missing', ext=ext)
                self.assertEqual(r['open'], '0')
                self.assertIn(f'decoder {ext}.ELF not loaded', self.log)

    def test_real_station_recordings(self) -> None:
        # Записи настоящих станций (scratchpad record_streams.py): HE-AAC v1 и v2, Ogg.
        found = {name: SAMPLES / name for name in ('rp_aac128.aac', 'rp_aac64.aac', 'rp_aac32.aac',
                                                   'soma_aac64.aac', 'dancewave.ogg')
                 if (SAMPLES / name).exists()}
        if not found:
            self.skipTest(f'нет записей в {SAMPLES}')
        expect = {'rp_aac128.aac': ('AAC', '2'), 'rp_aac64.aac': ('HE-AAC', '1'),
                  'rp_aac32.aac': ('HE-AAC', '1'), 'soma_aac64.aac': ('HE-AAC', '2'),
                  'dancewave.ogg': ('Vorbis', '2')}
        for name, path in found.items():
            with self.subTest(name=name):
                r = self.play(path, path.suffix[1:].upper(), speed=40)
                codec, channels = expect[name]
                self.assertEqual((r['finished'], r['errors'], r['rate'], r['channels']),
                                 ('1', '0', '44100', channels))
                self.assertRegex(self.log, rf'played to the end: {codec} \d+ kbps')
                self.assertGreater(int(r['pcm_frames']), 15 * 44100)


class RadioTest(unittest.TestCase):
    """Интернет-радио: плейлист M3U, станции — сеть стенда (net_* ядра)."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.exe = build()
        if cls.exe is None:
            raise unittest.SkipTest('нет MSVC (vcvars32.bat): стенд PLAYER.ELF пропущен')
        cls.temp = tempfile.TemporaryDirectory()
        cls.stream = pathlib.Path(cls.temp.name) / 'stream.mp3'
        cls.stream.write_bytes(silent_mp3(600))

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temp.cleanup()

    def radio(self, playlist: bytes, **keys) -> dict[str, str]:
        path = pathlib.Path(self.temp.name) / 'radio.m3u'
        path.write_bytes(playlist)
        args = [str(self.exe), 'radio']
        keys.setdefault('net-file', self.stream)
        keys.setdefault('seconds', 2)
        for key, value in keys.items():
            args += [f'--{key}', str(value)]
        args.append(str(path))
        result = subprocess.run(args, capture_output=True, timeout=300)
        output = result.stdout.decode('cp866', 'replace')
        self.assertEqual(result.returncode, 0, output + result.stderr.decode('cp866', 'replace'))
        self.log = result.stderr.decode('cp866', 'replace')
        values = dict(line.split('=', 1) for line in output.splitlines() if '=' in line)
        self.assertEqual(values['tasks_after_stop'], '0', 'задачи плеера (и радио) вышли')
        self.assertEqual(values['modules_after_stop'], '0')
        return values

    def test_first_station_plays_with_the_song_title(self) -> None:
        r = self.radio(radio_list(STATIONS))
        self.assertEqual(r['loaded_state'], '1')
        self.assertEqual(r['loaded_decoded'], '1', 'строка формата сразу: формат известен')
        self.assertEqual(r['first_tags_flag'], '1', 'песня пришла: перечитать метки')
        self.assertEqual(r['first_flag_after_tags'], '0', 'метки прочитаны — флаг снят')
        self.assertEqual(r['first_title'], 'First Station')
        self.assertEqual(r['first_artist'], 'Test Artist - Test Song')
        self.assertEqual(r['first_album'], 'Station 1 of 3')
        self.assertEqual(r['first_extra'], 'MP3  128 kbps  44.1 kHz  stereo')
        self.assertEqual((r['first_format'], r['first_order'], r['first_length']), ('1', '0', '3'))
        self.assertEqual(r['info_title'], 'First Station')
        self.assertEqual(r['net_url_1'], 'http://one.example/live')
        self.assertEqual(r['net_opens'], '1')
        self.assertGreaterEqual(int(r['pcm_frames']), 2 * RATE)
        self.assertIn('player: radio 1/3: http://one.example/live, MP3.ELF, 128 kbps, '
                      'metaint 16000', self.log)

    def test_left_and_right_switch_stations_round(self) -> None:
        r = self.radio(radio_list(STATIONS), seek=-1)
        self.assertEqual((r['seek'], r['seek_station']), ('1', '2'), 'влево с первой — последняя')
        self.assertEqual(r['switch_artist'], 'Connecting...', 'сразу — соединение, не прежняя песня')
        self.assertEqual(r['second_title'], 'Third Station')
        self.assertEqual(r['second_album'], 'Station 3 of 3')
        self.assertEqual(r['second_artist'], 'Test Artist - Test Song')
        self.assertEqual(r['net_opens'], '2')
        self.assertEqual(r['net_url_2'], 'https://three.example/radio')

    def test_station_list_while_switching(self) -> None:
        # Листают станции — 10 с вместо столбиков список (вид 3): 7 строк, текущая
        # с «>», у начала списка — с первой станции; потом снова спектр.
        many = [(f'Station {i}', f'http://s{i}.example/live') for i in range(1, 12)]
        r = self.radio(radio_list(many), seek=1, **{'list-wait': 1, 'seconds': 1})
        self.assertEqual(r['list_kind'], '3')
        self.assertEqual(r['list_rows'], '7')
        self.assertEqual(r['list_0'], '   1. Station 1')
        self.assertEqual(r['list_1'], '>  2. Station 2')
        self.assertEqual(r['list_6'], '   7. Station 7')
        self.assertEqual(r['after_list_kind'], '1', 'через 10 с — снова спектр')

    def test_station_list_sticks_to_the_end_of_the_list(self) -> None:
        # Текущая — посередине окна станций, а у конца списка окно прижато к краю.
        many = [(f'Station {i}', f'http://s{i}.example/live') for i in range(1, 12)]
        r = self.radio(radio_list(many), seek=-3, seconds=1)
        self.assertEqual(r['seek_station'], '8')
        self.assertEqual([r[f'list_{i}'] for i in range(7)],
                         ['   5. Station 5', '   6. Station 6', '   7. Station 7',
                          '   8. Station 8', '>  9. Station 9', '  10. Station 10',
                          '  11. Station 11'])

    def test_station_without_a_song_shows_what_it_is(self) -> None:
        # Станции TAVR шлют StreamTitle='' до смены песни: вместо неё — icy-description.
        r = self.radio(radio_list(STATIONS), **{'net-title': ''})
        self.assertEqual(r['first_artist'], 'Only test songs!')

    def test_a_new_song_title_comes_from_the_stream(self) -> None:
        r = self.radio(radio_list(STATIONS), **{'net-title2': 'Next Band - Next Song',
                                                 'net-metaint': 8000, 'seconds': 3})
        self.assertEqual(r['last_artist'], 'Next Band - Next Song')

    def test_lost_connection_reconnects(self) -> None:
        r = self.radio(radio_list(STATIONS), **{'net-cut': 60000, 'seconds': 8})
        self.assertGreaterEqual(int(r['net_opens']), 2, 'обрыв — новое соединение')
        self.assertEqual(r['net_url_2'], 'http://one.example/live', 'та же станция')
        self.assertIn('player: radio 1/3: connection lost', self.log)
        self.assertGreaterEqual(int(r['pcm_frames']), 4 * RATE, 'звук идёт и после обрыва')

    def test_failed_connection_is_retried(self) -> None:
        r = self.radio(radio_list(STATIONS), **{'net-fail': 1, 'seconds': 2})
        self.assertGreaterEqual(int(r['net_opens']), 2)
        self.assertIn('No connection, retrying', self.log)
        self.assertGreaterEqual(int(r['pcm_frames']), 2 * RATE)

    # Буфер радио (PLAYER 1.6): станция шлёт звук вровень со звуком (128 кбит/с),
    # очередь ядра — как у платы (1486 мс); --speed 4 — звук и сеть вчетверо быстрее.
    REAL_TIME = {'net-kbps': 128, 'pcm-ms': 1486, 'speed': 4}

    def test_stream_at_real_time_plays_without_gaps(self) -> None:
        # Без порции при подключении, пачками раз в 400 мс (Wi-Fi, окно TCP): звук —
        # когда пришло 4 с звука; дальше пустое кольцо при звуке в очереди ядра — не
        # опустошение (до 1.6 — провал за провалом).
        r = self.radio(radio_list(STATIONS), **self.REAL_TIME,
                       **{'net-burst': 0, 'net-jitter': 400, 'seconds': 12})
        self.assertIn('player: radio buffer 512 KB', self.log)
        self.assertGreaterEqual(int(r['pcm_frames']), 12 * RATE)
        self.assertEqual((r['pcm_gaps'], r['last_underruns']), ('0', '0'), self.log)

    def test_stall_shorter_than_the_buffer_is_not_heard(self) -> None:
        # Станция шлёт 2 с сразу, после 100 000 байтов молчит 3 с, потом догоняет.
        r = self.radio(radio_list(STATIONS), **self.REAL_TIME,
                       **{'net-burst': 32000, 'net-stall-at': 100000, 'net-stall-ms': 3000,
                          'seconds': 14})
        self.assertGreaterEqual(int(r['pcm_frames']), 14 * RATE)
        self.assertEqual((r['pcm_gaps'], r['last_underruns']), ('0', '0'), self.log)

    def test_long_stall_is_one_pause_then_a_bigger_buffer(self) -> None:
        # Молчит 9 с — дольше запаса (4 с): одна пауза, после неё набрать 6 с звука
        # (96 000 байтов при 128 кбит/с), и дальше без провалов.
        r = self.radio(radio_list(STATIONS), **self.REAL_TIME,
                       **{'net-burst': 32000, 'net-stall-at': 100000, 'net-stall-ms': 9000,
                          'seconds': 16})
        self.assertEqual((r['pcm_gaps'], r['last_underruns']), ('1', '1'), self.log)
        self.assertIn('player: radio stream ran dry (1), waiting for 93 KB', self.log)
        self.assertGreaterEqual(int(r['pcm_frames']), 16 * RATE)

    def test_no_psram_for_the_radio_buffer_plays_with_the_small_one(self) -> None:
        r = self.radio(radio_list(STATIONS), **{'psram': 600000})
        self.assertIn('player: no PSRAM for the 512 KB radio buffer: 128 KB', self.log)
        self.assertIn('player: radio buffer 128 KB', self.log)
        self.assertGreaterEqual(int(r['pcm_frames']), 2 * RATE)

    def test_codec_comes_from_the_stream_start(self) -> None:
        # Content-Type врёт (audio/aacp), а в потоке — кадры MPEG: играет MP3.ELF.
        r = self.radio(radio_list(STATIONS), **{'net-type': 'audio/aacp'})
        self.assertEqual(r['first_extra'], 'MP3  128 kbps  44.1 kHz  stereo')
        self.assertIn('MP3.ELF, 128 kbps', self.log)
        self.assertGreaterEqual(int(r['pcm_frames']), 2 * RATE)

    def test_playlist_in_a_playlist_is_refused_with_a_reason(self) -> None:
        r = self.radio(radio_list(STATIONS), **{'net-type': 'audio/x-mpegurl', 'seconds': 0.5,
                                                 'wait-ms': 1500})
        self.assertEqual(r['first_artist'], 'audio/x-mpegurl is not supported')
        self.assertEqual(r['pcm_frames'], '0')

    def need_ffmpeg(self) -> None:
        if FFMPEG is None:
            self.skipTest('нет ffmpeg: станций AAC и Ogg не сделать')

    def test_aac_station(self) -> None:
        self.need_ffmpeg()
        stream = encoded('station.aac', '-ac', '2', '-c:a', 'aac', '-b:a', '128k', '-f', 'adts',
                         inputs=tone(4))
        r = self.radio(radio_list(STATIONS), **{'net-file': stream, 'net-type': 'audio/aac'})
        self.assertEqual(r['first_artist'], 'Test Artist - Test Song', 'песня — из ICY, как у MP3')
        self.assertRegex(r['first_extra'], r'^AAC  \d+ kbps  44\.1 kHz  stereo$')
        self.assertIn('AAC.ELF', self.log)
        self.assertGreaterEqual(int(r['pcm_frames']), 2 * RATE)
        self.assertEqual(r['errors'], '0')

    def test_aac_station_named_mpeg_plays_as_aac(self) -> None:
        self.need_ffmpeg()
        stream = encoded('station.aac', '-ac', '2', '-c:a', 'aac', '-b:a', '128k', '-f', 'adts',
                         inputs=tone(4))
        r = self.radio(radio_list(STATIONS), **{'net-file': stream, 'net-type': 'audio/mpeg'})
        self.assertIn('AAC.ELF', self.log)
        self.assertGreaterEqual(int(r['pcm_frames']), 2 * RATE)

    def test_ogg_station_song_titles_from_the_chain(self) -> None:
        # Цепочка: песня 3 с, за ней другая 6 с, по кругу; ICY у Ogg нет.
        self.need_ffmpeg()
        one = encoded('one.ogg', '-ac', '2', '-c:a', 'libvorbis', '-q:a', '3', '-metadata',
                      'title=Song One', '-metadata', 'artist=Band', inputs=tone(3, 440))
        two = encoded('two.ogg', '-ac', '2', '-c:a', 'libvorbis', '-q:a', '3', '-metadata',
                      'title=Song Two', '-metadata', 'artist=Other Band', inputs=tone(6, 660))
        chain = pathlib.Path(self.temp.name) / 'chain.ogg'
        chain.write_bytes(one.read_bytes() + two.read_bytes())
        r = self.radio(radio_list(STATIONS), **{'net-file': chain, 'net-type': 'application/ogg',
                                                 'net-metaint': 0, 'seconds': 4.5})
        self.assertEqual(r['first_tags_flag'], '1')
        self.assertEqual(r['first_artist'], 'Band - Song One')
        self.assertRegex(r['first_extra'], r'^Vorbis  \d+ kbps  44\.1 kHz  stereo$')
        self.assertEqual(r['last_artist'], 'Other Band - Song Two', 'новая песня цепочки')
        self.assertIn('OGG.ELF', self.log)
        self.assertGreaterEqual(int(r['pcm_frames']), 4 * RATE)
        self.assertEqual(r['errors'], '0')

    def test_ogg_opus_station_is_refused_with_a_reason(self) -> None:
        self.need_ffmpeg()
        stream = encoded('station.opus', '-c:a', 'libopus', '-b:a', '64k', inputs=tone(2))
        r = self.radio(radio_list(STATIONS), **{'net-file': stream, 'net-type': 'audio/ogg',
                                                 'net-metaint': 0, 'seconds': 0.5,
                                                 'wait-ms': 1500})
        self.assertEqual(r['first_artist'], 'Ogg Opus is not supported')
        self.assertEqual(r['pcm_frames'], '0')

    def test_station_without_its_decoder_says_so(self) -> None:
        self.need_ffmpeg()
        stream = encoded('station.aac', '-ac', '2', '-c:a', 'aac', '-b:a', '128k', '-f', 'adts',
                         inputs=tone(4))
        r = self.radio(radio_list(STATIONS), **{'net-file': stream, 'net-type': 'audio/aac',
                                                 'no-aac': 1, 'seconds': 0.5, 'wait-ms': 1500})
        self.assertEqual(r['first_artist'], 'No AAC.ELF in /WC/ESP')
        self.assertEqual(r['pcm_frames'], '0')

    def test_official_playlist_in_cp1251(self) -> None:
        # Как у станций TAVR: название в скобках с адресом сайта, кириллица в CP1251;
        # украинские і и ї в окне — латинской i (в CP866 их нет).
        title = 'Хіт FM Українські хіти'
        playlist = ('#EXTM3U\r\n\r\n#EXTINF:-1, (' + title + ' - www.hitfm.ua)\r\n'
                    'http://online.hitfm.ua/HitFM_Ukr\r\n').encode('cp1251')
        r = self.radio(playlist)
        self.assertEqual(r['first_title'], title.replace('і', 'i').replace('ї', 'i'))
        self.assertEqual(r['net_url_1'], 'http://online.hitfm.ua/HitFM_Ukr')

    def test_utf8_song_title(self) -> None:
        r = self.radio(radio_list(STATIONS), **{'net-title': 'Океан Ельзи - Обійми'})
        self.assertEqual(r['first_artist'], 'Океан Ельзи - Обiйми')

    def test_playlist_without_stations_is_skipped(self) -> None:
        r = self.radio(b'#EXTM3U\r\nmusic/local.mp3\r\n', seconds=0)
        self.assertEqual(r['first_title'] if 'first_title' in r else '', '')
        self.assertIn('has no stations', self.log)
        self.assertEqual(r['net_opens'], '0')

    def test_old_firmware_without_network_skips_with_a_reason(self) -> None:
        r = self.radio(radio_list(STATIONS), **{'no-net': 1, 'seconds': 0})
        self.assertIn('radio needs firmware 0.1.32', self.log)
        self.assertEqual(r['net_opens'], '0')


if __name__ == '__main__':
    unittest.main()
