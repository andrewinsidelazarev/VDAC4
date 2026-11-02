"""Стенд ПК COREMENU.ELF: карта Tang из ELF ядер с карточками, сборка clang (обычная и с ASan), прогон.

Карта: ELF ядер из Tang Nano 20k/Cores/SD/CORES с карточками cores_ui/cards (add_card.py) и
esp/dist/COREMENU.ELF (сначала esp/tools/build.ps1). Всё временное — в E:/zx/cache/tmp/coremenu/host:
там же PNG кадров экрана (01…06), чтобы посмотреть глазами.
"""
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ESP = HERE.parent
CORES = ESP.parent
UI = CORES / 'cores_ui'
V4 = CORES.parents[2]
FW = V4 / 'ESP32 S3 Zero N8R8' / 'Firmware'
SD_CORES = V4 / 'Tang Nano 20k' / 'Cores' / 'SD' / 'CORES'
OUT = Path(r'E:/zx/cache/tmp/coremenu/host')
CLANG = Path(r'E:/zx/llvm/bin/clang.exe')

ENV = os.environ.copy()
ENV.update(TEMP=str(OUT), TMP=str(OUT), PYTHONDONTWRITEBYTECODE='1', ASAN_OPTIONS='detect_leaks=0:halt_on_error=1')
ENV['PATH'] = str(CLANG.parent.parent / 'lib/clang/23/lib/windows') + os.pathsep + ENV.get('PATH', '')


def run(cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], env=ENV, capture_output=True, **kw)
    out = (r.stdout + r.stderr).decode('utf-8', 'replace')
    if r.returncode:
        print(out)
        sys.exit(f'exit {r.returncode}: {cmd[0]}')
    return out


def main():
    sd = OUT / 'sd' / 'CORES'
    sd.mkdir(parents=True, exist_ok=True)
    for name in ('NES', 'NEXT', 'AMIGA'):
        run([sys.executable, '-B', UI / 'tools' / 'add_card.py', SD_CORES / f'{name}.ELF',
             UI / 'cards' / name / 'card.bin', sd / f'{name}.ELF'])
    (sd / 'COREMENU.ELF').write_bytes((ESP / 'dist' / 'COREMENU.ELF').read_bytes())
    inc = ['-I', FW / 'modules/sdk/include', '-I', FW / 'include', '-I', UI, '-I', UI / 'assets', '-I', UI / 'fonts']
    for variant, extra in (('asan', ['-fsanitize=address', '-g', '-fno-omit-frame-pointer']), ('host', [])):
        objs = []
        for src, lang in ((ESP / 'main' / 'app.c', 'c'), (UI / 'cores_ui.cpp', 'c++'), (UI / 'cui_card.cpp', 'c++'),
                          (UI / 'assets' / 'cui_art.cpp', 'c++'), (UI / 'fonts' / 'pui_font.cpp', 'c++'),
                          (FW / 'src' / 'elf_image.cpp', 'c++'), (HERE / 'coremenu_host.cpp', 'c++')):
            obj = OUT / f'{variant}_{src.stem}.obj'
            std = ['-std=c11'] if lang == 'c' else ['-std=c++17', '-fno-exceptions', '-fno-rtti']
            run([CLANG, '-x', lang, *std, '-O2', '-Wall', '-Wextra', '-Werror', '-D_CRT_SECURE_NO_WARNINGS',
                 '-DCOREMENU_HOST_TEST', '-Dmain=coremenu_main' if src.stem == 'app' else '-DHOST', *extra, *inc,
                 '-c', src, '-o', obj])
            objs.append(obj)
        exe = OUT / f'coremenu_{variant}.exe'
        run([CLANG, *extra, *objs, '-o', exe])
        print(f'--- {variant}')
        print(run([exe, OUT / 'sd', OUT]).strip())
    # кадры — в PNG (PIL есть в pio-venv)
    try:
        from PIL import Image
        for ppm in sorted(OUT.glob('*.ppm')):
            Image.open(ppm).resize((1280, 720), Image.NEAREST).save(ppm.with_suffix('.png'))
    except ImportError:
        pass


if __name__ == '__main__':
    main()
