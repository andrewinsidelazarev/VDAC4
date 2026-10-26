"""Этап 2: локальные сборки и проверки. Все записи — только в player_ui/build-pc.

Запуск из player_ui: E:\\zx\\esp\\pio-venv\\Scripts\\python.exe tests/run_stage2.py
Никаких установок, загрузок, подключения к плате или изменений Firmware.
"""
from pathlib import Path
import argparse
import ast
import csv
import hashlib
import json
import os
import shutil
import subprocess
import sys
import types
import unittest

sys.dont_write_bytecode = True
sys.stdout.reconfigure(encoding='utf-8')
ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build-pc'
FIRMWARE = ROOT.parent.parent / 'Firmware'
CLANG = Path(r'E:\zx\llvm\bin\clang.exe')
XTENSA = Path(r'E:\zx\esp\.espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin')
ENV = os.environ.copy()
ENV.update(TEMP=str(BUILD), TMP=str(BUILD), PYTHONDONTWRITEBYTECODE='1')
ENV['PATH'] = str(Path(r'E:\zx\llvm\lib\clang\23\lib\windows')) + os.pathsep + ENV.get('PATH', '')
ENV['ASAN_OPTIONS'] = 'halt_on_error=1:detect_leaks=0'
FLAGS = ['-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-fno-exceptions', '-fno-rtti', '-D_CRT_SECURE_NO_WARNINGS']
INCLUDES = ['-I', str(ROOT), '-I', str(FIRMWARE / 'include'), '-I', str(FIRMWARE / 'modules/sdk/include')]


def run(command, log, cwd=BUILD):
    command = [str(x) for x in command]
    result = subprocess.run(command, cwd=cwd, env=ENV, capture_output=True, timeout=240)
    output = (result.stdout + result.stderr).decode('utf-8', errors='replace').replace('\r\n', '\n').replace('\r', '')
    (BUILD / log).write_text(subprocess.list2cmdline(command) + '\n' + output, encoding='utf-8')
    if result.returncode:
        raise RuntimeError(f'{log}: exit {result.returncode}\n{output}')
    return output


def host(sanitize=False):
    suffix = '_asan' if sanitize else ''
    extra = ['-fsanitize=address', '-g'] if sanitize else []
    unit = BUILD / f'pui_unit{suffix}.exe'
    run([CLANG, *FLAGS, *extra, *INCLUDES, ROOT / 'tests/pui_unit.cpp', ROOT / 'fonts/pui_font.cpp', '-o', unit], f'build_unit{suffix}.log')
    print(run([unit], f'unit{suffix}.log').strip(), flush=True)
    objects = []
    for namespace in ('player_view', 'pui_reference', 'pui_repeat'):
        obj = BUILD / f'{namespace}{suffix}.obj'
        run([CLANG, *FLAGS, *extra, *INCLUDES, f'-Dplayer_view={namespace}', '-c', ROOT / 'player_ui.cpp', '-o', obj], f'build_{namespace}{suffix}.log')
        objects.append(obj)
    exe = BUILD / f'pui_stage2{suffix}.exe'
    run([CLANG, *FLAGS, *extra, *INCLUDES, *objects, ROOT / 'fonts/pui_font.cpp', ROOT / 'tests/pui_stage2.cpp', '-o', exe], f'build_stage2{suffix}.log')
    print(run([exe], f'stage2{suffix}.log').strip(), flush=True)
    if not sanitize:
        print(run([exe, '--benchmark'], 'benchmark.log').strip(), flush=True)


def overlay():
    exe = BUILD / 'player_overlay_stage2.exe'
    run([CLANG, *FLAGS, *INCLUDES, FIRMWARE / 'tests/player_overlay_host.cpp', FIRMWARE / 'src/player_overlay.cpp', ROOT / 'player_ui.cpp', ROOT / 'fonts/pui_font.cpp', '-o', exe], 'build_overlay.log')
    output = run([exe], 'overlay.log')
    values = dict(line.split('=', 1) for line in output.splitlines() if '=' in line)
    # Берём все настоящие assertions Claude, исключая его сборщик, пишущий в Firmware.
    # Только определения класса и константы: импорт test_weather_parse не исполняется.
    source = FIRMWARE / 'tests/test_player_overlay_host.py'
    tree = ast.parse(source.read_text(encoding='utf-8-sig'), filename=str(source))
    cls = next(node for node in tree.body if isinstance(node, ast.ClassDef) and node.name == 'PlayerOverlayTest')
    cls.body = [node for node in cls.body if not isinstance(node, ast.FunctionDef) or node.name != 'setUpClass']
    module = types.ModuleType('claude_overlay_assertions')
    module.__dict__.update(unittest=unittest, OFF=0, FADE_IN=1, SHOWN=2, FADE_OUT=3, CANVAS_WORDS=320*360)
    exec(compile(ast.Module(body=[cls], type_ignores=[]), str(source), 'exec'), module.__dict__)
    module.PlayerOverlayTest.r = values
    with (BUILD / 'overlay_assertions.log').open('w', encoding='utf-8') as log:
        result = unittest.TextTestRunner(stream=log, verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(module.PlayerOverlayTest))
    if not result.wasSuccessful():
        raise RuntimeError((BUILD / 'overlay_assertions.log').read_text(encoding='utf-8'))
    # Уточняем исходный общий лимит теста до бюджета задания.
    assert int(values['frames_mean_words']) <= 6000 and int(values['frames_max_words']) <= 6000
    assert int(values['random_max_words']) <= 6000
    print(f"PASS Claude: {result.testsRun} tests; mean={values['frames_mean_words']} max={values['frames_max_words']} random_max={values['random_max_words']} words", flush=True)


def xtensa():
    compiler = XTENSA / 'xtensa-esp32s3-elf-g++.exe'
    flags = ['-O2', '-Wall', '-Wextra', '-Werror', '-std=gnu++17', '-mlongcalls', '-ffunction-sections', '-fdata-sections', '-fno-exceptions', '-fno-rtti']
    run([compiler, '--version'], 'xtensa_version.log')
    objects = []
    for source, name in [('player_ui.cpp', 'player_ui_xtensa.o'), ('fonts/pui_font.cpp', 'pui_font_xtensa.o')]:
        obj = BUILD / name
        run([compiler, *flags, *INCLUDES, '-fstack-usage', '-c', ROOT / source, '-o', obj], f'build_{name}.log')
        objects.append(obj)
    size = XTENSA / 'xtensa-esp32s3-elf-size.exe'
    print(run([size, *objects], 'xtensa_size.log').strip(), flush=True)
    detail = run([size, '-A', *objects], 'xtensa_sections.log')
    run([XTENSA / 'xtensa-esp32s3-elf-objdump.exe', '-d', '-C', objects[0]], 'xtensa_disassembly.log')
    run([XTENSA / 'xtensa-esp32s3-elf-nm.exe', '-S', '-C', '--size-sort', *objects], 'xtensa_symbols.log')
    run([XTENSA / 'xtensa-esp32s3-elf-nm.exe', '-u', *objects], 'xtensa_undefined.log')
    totals = {key: 0 for key in ('text', 'literal', 'rodata', 'data', 'bss')}
    for line in detail.splitlines():
        parts = line.split()
        if len(parts) == 3:
            for key in totals:
                if parts[0] == '.' + key or parts[0].startswith('.' + key + '.'):
                    totals[key] += int(parts[1])
    (BUILD / 'xtensa_totals.json').write_text(json.dumps(totals, indent=2)+'\n', encoding='utf-8')
    print('Xtensa sections:', totals, flush=True)


def previews():
    target = BUILD / 'stage2-preview'
    (target / 'raw').mkdir(parents=True, exist_ok=True)
    for name in ('cover.rgb565', 'spectrum.bin'):
        shutil.copyfile(BUILD / name, target / name)
    exe = BUILD / 'pui_preview_stage2.exe'
    run([CLANG, *FLAGS, *INCLUDES, '-I', BUILD, ROOT / 'player_ui.cpp', ROOT / 'fonts/pui_font.cpp', ROOT / 'preview/pui_preview.cpp', '-o', exe], 'build_preview_stage2.log')
    run([exe], 'preview_stage2.log', target)
    hashes = {}
    for source in sorted((BUILD / 'raw').glob('*.rgb565')):
        generated = target / 'raw' / source.name
        assert source.read_bytes() == generated.read_bytes(), f'Preview changed: {source.name}'
        hashes[source.name] = hashlib.sha256(generated.read_bytes()).hexdigest()
    assert len(hashes) == 125
    (BUILD / 'preview_stage2_hashes.json').write_text(json.dumps(hashes, indent=2)+'\n', encoding='utf-8')
    print(f'PASS previews: {len(hashes)} RGB565 frames byte-identical to stage 1', flush=True)


def profile():
    source = (ROOT / 'player_ui.cpp').read_text(encoding='utf-8')
    substitutions = {
        'void put(int x, int y, uint16_t color) {': 'void put(int x, int y, uint16_t color) { ++pui_profile::counts.put;',
        '  p = color;': '  ++pui_profile::counts.stores; p = color;',
        'uint16_t dither(Color c, int x, int y) {': 'uint16_t dither(Color c, int x, int y) { ++pui_profile::counts.dither;',
        '    const int light=': '    ++pui_profile::counts.background; const int light=',
        '      const int glow=': '      ++pui_profile::counts.placeholder; const int glow=',
        'bool insideRound(int x, int y, int w, int h, int radius) {': 'bool insideRound(int x, int y, int w, int h, int radius) { ++pui_profile::counts.round;',
        'void restore(int x, int y, int w, int h) {': 'void restore(int x, int y, int w, int h) { pui_profile::counts.restore += w*h;',
        '      const int index=yy*g.width+xx;': '      ++pui_profile::counts.glyph; const int index=yy*g.width+xx;',
        '        Color bg=unpack(': '        ++pui_profile::counts.alpha; Color bg=unpack(',
        'h=(h^b[i])*16777619U;': '{ ++pui_profile::counts.hash; h=(h^b[i])*16777619U; }',
        'h=(h^static_cast<unsigned char>(str[i]))*16777619U;': '{ ++pui_profile::counts.hash; h=(h^static_cast<unsigned char>(str[i]))*16777619U; }',
        '    memcpy(s.base,canvas,kWidth*kHeight*sizeof(uint16_t));': '    pui_profile::counts.copied += kWidth*kHeight; memcpy(s.base,canvas,kWidth*kHeight*sizeof(uint16_t));',
    }
    for old, new in substitutions.items():
        assert source.count(old) == 1, f'Profile insertion drift: {old}'
        source = source.replace(old, new)
    source = '#include "tests/pui_profile.h"\n' + source.replace('snprintf(', 'pui_profile::format(')
    generated = BUILD / 'player_ui_profile.cpp'
    generated.write_text(source, encoding='utf-8')
    exe = BUILD / 'pui_profile.exe'
    run([CLANG, *FLAGS, *INCLUDES, generated, ROOT / 'fonts/pui_font.cpp', ROOT / 'tests/pui_profile.cpp', '-o', exe], 'build_profile.log')
    output = run([exe], 'profile.log')
    (BUILD / 'profile.csv').write_text(output, encoding='utf-8')
    print('PASS operation counts: build-pc/profile.csv', flush=True)
    # Коэффициенты — оценка по дизассемблеру GCC 15.2, НЕ измеренные циклы ESP.
    # put: адрес/чтение/сравнение; stores: дополнительная запись и изменение dirty.
    # Остальные цены не включают put. background — отдельная прямая запись.
    costs = dict(put=18, stores=20, dither=65, background=14, placeholder=40,
                 round=15, restore=6, glyph=12, alpha=45, hash=6, copied=2, formats=1500)
    rows = list(csv.DictReader(output.splitlines()))
    estimates = {}
    for mode in dict.fromkeys(row['mode'] for row in rows):
        selected = [row for row in rows if row['mode'] == mode]
        cycles = [10000 + sum(int(row[key]) * cost for key, cost in costs.items()) for row in selected]
        estimates[mode] = dict(frames=len(cycles), mean_ms=sum(cycles)/len(cycles)/240000,
                               max_ms=max(cycles)/240000,
                               mean_counts={key: sum(int(r[key]) for r in selected)/len(selected) for key in costs})
    summary = dict(method='sum(operation_count * assumed_cycles) + 10000 cycles/frame',
                   clock_mhz=240, assumed_cycles=costs,
                   uncertainty='Roughly x0.5..x2, not a confidence interval or hard bound; PSRAM/cache/interrupts unmeasured',
                   estimates=estimates)
    (BUILD / 'timing_estimate.json').write_text(json.dumps(summary, indent=2)+'\n', encoding='utf-8')
    print('ESP estimate ms:', {key: round(value['mean_ms'], 3) for key, value in estimates.items()}, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--only', choices=('host', 'asan', 'overlay', 'xtensa', 'previews', 'profile'))
    args = parser.parse_args()
    BUILD.mkdir(exist_ok=True)
    actions = {'host': host, 'asan': lambda: host(True), 'overlay': overlay, 'xtensa': xtensa, 'previews': previews, 'profile': profile}
    for name, action in actions.items():
        if args.only is None or args.only == name:
            print(f'Running {name}...', flush=True)
            action()
    sources = [ROOT / 'player_ui.cpp', ROOT / 'player_ui.h', ROOT / 'fonts/pui_font.cpp',
               ROOT / 'tests/pui_stage2.cpp', ROOT / 'tests/pui_unit.cpp', ROOT / 'tests/pui_profile.cpp',
               ROOT / 'tests/pui_profile.h', Path(__file__),
               FIRMWARE / 'tests/player_overlay_host.cpp', FIRMWARE / 'tests/test_player_overlay_host.py',
               FIRMWARE / 'src/player_overlay.cpp', FIRMWARE / 'include/zifi/player_overlay.hpp',
               FIRMWARE / 'modules/sdk/include/zm_app.h']
    (BUILD / 'stage2_inputs.json').write_text(json.dumps({str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}, indent=2)+'\n', encoding='utf-8')


if __name__ == '__main__':
    main()
