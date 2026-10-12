"""Сборка vdac4_esp_host.exe — ESP платы VDAC4 для Unreal (TS_ZIFI32=VDAC4).

Те же исходники, что у прошивки и модулей (PLAYER.ELF, MP3.ELF, AAC.ELF, OGG.ELF, FLAC.ELF,
TRACKER.ELF, MIDI.ELF, OPL.ELF, SAA.ELF, TSFM.ELF):
MSVC, 32 бита (Helix на MSVC умеет только x86), как tests/test_player_host.py.
Обновлятор WC (tools/unreal/wcu_glue.cpp, src/wc_updater.cpp, мост VFS, транспорт) —
отдельно, на заглушках Arduino и FreeRTOS стендов ПК (tests/stubs_host), как tools/host_wcu,
и с ZIFI_BOARD_VDAC4=1, как прошивка платы.
Запуск: python tools/unreal/build_vdac4_esp_host.py [каталог для exe]
Пути исходников — от корня своей части (функция at): модули (modules/, кроме
modules/sdk), их заглушки и библиотеки кодеков — в Sound, остальное — в Firmware.
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]       # Firmware
SOUND = ROOT.parent / 'Sound'
sys.path.insert(0, str(ROOT / 'tests'))
from test_weather_parse import find_vcvars, short  # noqa: E402

BUILD = ROOT / '.test-build' / 'vdac4_esp_host'
HELIX = ROOT / 'managed_components' / 'chmorgan__esp-libhelix-mp3' / 'libhelix-mp3'
SOUND_PARTS = ('modules/', 'tests/stubs_player', 'tests/stubs_tracker') + tuple(
    f'components/{name}/' for name in ('foxen_flac', 'helix_aac', 'libxmp', 'libxmp_lite', 'saasound', 'stb_vorbis', 'tsf', 'ymfm'))


def at(path) -> pathlib.Path:
    """Путь от корня своей части: модули, их заглушки и кодеки — Sound, остальное — Firmware."""
    text = str(path).replace('\\', '/')
    if (text + '/').startswith(SOUND_PARTS) and not text.startswith('modules/sdk/'):
        return SOUND / text
    return ROOT / path


HELIX_SOURCES = ['mp3dec.c', 'mp3tabs.c'] + [
    f'real/{name}.c' for name in ('bitstream', 'buffers', 'dct32', 'dequant', 'dqchan', 'huffman',
                                  'hufftabs', 'imdct', 'polyphase', 'scalfact', 'stproc',
                                  'subband', 'trigtabs')]


def build(out_dir: pathlib.Path) -> pathlib.Path:
    vcvars = find_vcvars()
    if vcvars is None:
        raise SystemExit('нет Visual Studio Build Tools')
    vcvars = vcvars.with_name('vcvars32.bat')
    BUILD.mkdir(parents=True, exist_ok=True)
    includes = ['tests/stubs_player', 'tests/stubs_tracker', 'modules/sdk/include',
                'components/tracker/include', 'components/tracker', 'include',
                'modules/player/main', 'components/tsf/include']
    common = ('/nologo /c /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /D_USE_MATH_DEFINES ' +
              ' '.join(f'/I "{short(at(path))}"' for path in includes))
    module = (f'{common} /std:c11 /FI zm_compat.h /I "{short(HELIX / "pub")}" /DLIBXMP_STATIC '
              f'/I "{short(SOUND / "components" / "libxmp" / "include")}" '
              f'/I "{short(SOUND / "components" / "libxmp" / "esp")}"')
    cpp = f'{common} /EHsc /std:c++17'
    # Код прошивки на заглушках ПК: свои пути включения, без заглушек модулей.
    firmware = ('/nologo /c /O2 /utf-8 /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS '
                '/DZIFI_HOST_BUILD /D_WINDOWS /DZIFI_BOARD_VDAC4=1 /FIzifi_msvc_prelude.h '
                f'/I "{short(ROOT / "tests" / "stubs_host")}" /I "{short(ROOT / "include")}"')
    ymfm = f'{cpp} /DNDEBUG /I "{short(SOUND / "components" / "ymfm" / "include")}"'
    units = [
        (cpp, 'tools/unreal/vdac4_esp_host.cpp', 'host.obj'),
        (cpp, 'src/protocol.cpp', 'protocol.obj'),
        (cpp, 'src/zm_cache.cpp', 'zm_cache.obj'),
        (cpp, 'src/config.cpp', 'config.obj'),             # zifi.ini (WIFI_INI)
        (cpp, 'src/weather_parse.cpp', 'weather_parse.obj'),   # WEATHER_GET
        (cpp, 'src/resource_disk_cache.cpp', 'resource_disk_cache.obj'),   # флешка (VDAC4_USB)
        (f'{module} /Dmain=player_main', 'modules/player/main/player.c', 'player.obj'),
        (module, 'modules/player/main/view.c', 'view.obj'),
        (module, 'modules/player/main/radio_list.c', 'radio_list.obj'),
        (f'{module} /Dmain=mp3_main', 'modules/mp3/main/mp3_source.c', 'mp3_source.obj'),
        (f'{module} /Dmain=aac_main /I "{short(SOUND / "components" / "helix_aac")}"',
         'modules/aac/main/aac_source.c', 'aac_source.obj'),
        (f'{module} /Dmain=ogg_main /I "{short(SOUND / "components" / "stb_vorbis")}"',
         'modules/ogg/main/ogg_source.c', 'ogg_source.obj'),
        (f'{common} /std:c11 /w /DNDEBUG', 'components/stb_vorbis/stb_vorbis_vdac4.c',
         'stb_vorbis.obj'),
        (f'{module} /Dmain=flac_main /I "{short(SOUND / "components" / "foxen_flac")}"',
         'modules/flac/main/flac_source.c', 'flac_source.obj'),
        (f'{common} /std:c11 /DNDEBUG', 'components/foxen_flac/foxen-flac.c', 'foxen_flac.obj'),
        (f'{module} /Dmain=tracker_main', 'modules/tracker/main/tracker_source.c',
         'tracker_source.obj'),
        (module, 'modules/tracker/main/it_stream.c', 'it_stream.obj'),
        (f'{cpp} /w', 'components/tracker/tracker.cpp', 'tracker.obj'),
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
        (cpp, 'modules/player/main/cover_glue.cpp', 'cover_glue.obj'),
        (cpp, 'src/cover_art.cpp', 'cover_art.obj'),
        # Обновлятор WC (WCU_*): связка и код прошивки.
        (f'{firmware} /I "{short(ROOT / "tools" / "unreal")}"', 'tools/unreal/wcu_glue.cpp',
         'wcu_glue.obj'),
        (firmware, 'src/wc_updater.cpp', 'wc_updater.obj'),
        (firmware, 'src/github_tree.cpp', 'github_tree.obj'),
        (firmware, 'src/git_sha1.cpp', 'git_sha1.obj'),
        (firmware, 'src/vfs_bridge.cpp', 'vfs_bridge.obj'),
        (firmware, 'src/vfs_client.cpp', 'vfs_client.obj'),
        (firmware, 'src/fat_allocation_cache.cpp', 'fat_allocation_cache.obj'),
        (firmware, 'src/spsc_ring.cpp', 'spsc_ring.obj'),
        (firmware, 'src/z80_transport.cpp', 'z80_transport.obj'),
        (firmware, 'src/diagnostic_log.cpp', 'diagnostic_log.obj'),
    ]
    # SAA.ELF (E-Tracker на SAASound) и TSFM.ELF (AY/TurboSound на AYumi, TFC на
    # ymfm OPN) — те же исходники и ключи, что у стенда плеера (tests/test_player_host.py).
    saa = f'{cpp} /I "{short(SOUND / "components" / "saasound")}"'
    units += [(f'{saa} /Dmain=saa_main', 'modules/saa/main/saa_source.cpp', 'saa_source.obj'),
              (saa, 'modules/saa/main/etracker.cpp', 'etracker.obj')]
    units += [(saa, f'components/saasound/{name}.cpp', name + '.obj')
              for name in ('SAAAmp', 'SAADevice', 'SAAEnv', 'SAAFreq', 'SAANoise')]
    tsfm = ymfm + f' /I "{short(SOUND / "modules/tsfm/main/ymfm")}"'
    units += [(f'{tsfm} /Dmain=tsfm_main', 'modules/tsfm/main/tsfm_source.cpp', 'tsfm_source.obj')]
    units += [(tsfm, f'modules/tsfm/main/{name}.cpp',
               'tsfm_' + name.replace('ymfm/', '') + '.obj')
              for name in ('ay_players', 'tfc', 'tfd', 'ymfm/ymfm_opn', 'ymfm/ymfm_ssg')]
    units += [(f'{common} /std:c11', 'modules/tsfm/main/ayumi/ayumi.c', 'ayumi.obj')]
    # libxmp полная (components/libxmp): все загрузчики и ProWizard, без распаковщиков.
    xmp = SOUND / 'components' / 'libxmp'
    # Как в модуле ESP: файлы — в памяти (esp/xmp_esp_shim.c, те же имена xmp_*).
    renames = ' '.join(f'/D{name}=xmp_{name}' for name in
                       ('fread', 'fseek', 'ftell', 'fclose', 'feof', 'ferror', 'fgetc', 'fwrite',
                        'fputc', 'fflush', 'fopen', 'malloc', 'calloc', 'realloc', 'free',
                        'getenv'))
    xmp_flags = (f'{common} /std:c11 /w /DLIBXMP_STATIC /DLIBXMP_NO_DEPACKERS /DLIBXMP_ESP_MODULE '
                 f'{renames} /I "{short(xmp / "include")}" /I "{short(xmp / "esp")}" '
                 f'/I "{short(xmp / "src")}"')
    xmp_sources = [xmp / name for name in (xmp / 'sources.txt').read_text(encoding='ascii').split()]
    xmp_sources.append(xmp / 'esp' / 'xmp_esp_shim.c')
    units += [(xmp_flags, source, 'xmp_' + source.stem + '.obj')
              for source in xmp_sources]
    # Helix AAC (components/helix_aac): все исходники компонента.
    aac_dir = SOUND / 'components' / 'helix_aac'
    units += [(f'{common} /w /DNDEBUG /I "{short(aac_dir)}"', source,
               'aac_' + source.stem + '.obj') for source in sorted(aac_dir.glob('*.c'))]
    helix_flags = f'{common} /w /I "{short(HELIX / "pub")}" /I "{short(HELIX / "real")}"'
    units += [(helix_flags, HELIX / source,
               'helix_' + pathlib.Path(source).stem + '.obj') for source in HELIX_SOURCES]
    lines = ['@echo off', f'call "{short(vcvars)}" >nul 2>nul || exit /b 1']
    lines += [f'cl {flags} "{short(at(source))}" /Fo:{obj} || exit /b 1'
              for flags, source, obj in units]
    lines.append('cl /nologo ' + ' '.join(obj for _, _, obj in units) +
                 ' winmm.lib ole32.lib windowscodecs.lib winhttp.lib ws2_32.lib /Fe:vdac4_esp_host.exe || exit /b 1')
    script = BUILD / 'build.bat'
    script.write_text('\r\n'.join(lines) + '\r\n', encoding='ascii')
    result = subprocess.run(['cmd.exe', '/c', short(script)], capture_output=True, cwd=short(BUILD))
    if result.returncode != 0:
        output = result.stdout.decode('cp866', 'replace') + result.stderr.decode('cp866', 'replace')
        raise SystemExit(f'сборка не удалась:\n{output}')
    exe = BUILD / 'vdac4_esp_host.exe'
    out_dir.mkdir(parents=True, exist_ok=True)
    target = out_dir / exe.name
    target.write_bytes(exe.read_bytes())
    return target


if __name__ == '__main__':
    out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else BUILD
    print(build(out))
