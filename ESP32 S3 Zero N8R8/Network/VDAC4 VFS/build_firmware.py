"""Сборка прошивки ESP платы VDAC4 во временном каталоге на E: (дерево платы как есть).

С 0.1.100 всё, что раньше накладывалось при сборке, лежит в самом дереве платы
(`ESP32 S3 Zero N8R8/Firmware`): служба VDAC4 VFS (vdisk*.cpp/hpp, команды VDISK 0x15…0x19 в
protocol.hpp и main.cpp), слой NES 0.1.96 Codex (module_app.cpp, zm_app.h, tang_files.hpp) и
захват карты Tang для WC (tang_sd.cpp, tang_sd_hold.cpp). Скрипт копирует дерево в каталог сборки,
при --version подменяет версию в CMakeLists.txt и собирает ESP-IDF. Дерево и устройства не трогаются.
"""
from pathlib import Path
from datetime import datetime, timezone
import argparse, hashlib, json, os, shutil, subprocess

HERE = Path(__file__).resolve().parent
VDAC = HERE.parents[1]                      # ESP32 S3 Zero N8R8
parser = argparse.ArgumentParser()
parser.add_argument('--stage-only', action='store_true')
parser.add_argument('--version', help='версия, например vdac4-0.1.101; без неё — из CMakeLists.txt')
parser.add_argument('--out', type=Path, default=HERE / 'release')
args = parser.parse_args()
cmake_text = (VDAC / 'Firmware/CMakeLists.txt').read_text(encoding='utf-8')
line = [l for l in cmake_text.splitlines() if 'set(ZIFI_VDAC4_VERSION' in l]
assert len(line) == 1, line
VERSION = args.version or line[0].split('"')[1]
assert VERSION.startswith('vdac4-0.1.'), VERSION
out = args.out
out.mkdir(parents=True, exist_ok=True)
stamp = datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%SZ')
# Каталог сборки — на E: (на C: может не хватить места: дерево и сборка — около 2 ГБ)
stage = Path(r'E:\zx\esp\stages') / ('claude-esp-' + VERSION.split('.')[-1] + '-' + stamp)
fw = stage / 'ESP32 S3 Zero N8R8/Firmware'
fw.mkdir(parents=True)
for name in ('components', 'data', 'include', 'lib', 'main', 'managed_components', 'modules', 'src', 'tools'):
    shutil.copytree(VDAC / 'Firmware' / name, fw / name,
                    ignore=shutil.ignore_patterns('.git', '__pycache__', '.pio', 'build', '.test-build', 'examples', 'test_apps'))
for name in ('CMakeLists.txt', 'dependencies.lock', 'partitions_vdac4.csv', 'sdkconfig', 'sdkconfig.defaults'):
    shutil.copyfile(VDAC / 'Firmware' / name, fw / name)
sound = stage / 'ESP32 S3 Zero N8R8/Sound/tools'
sound.mkdir(parents=True)
shutil.copyfile(VDAC / 'Sound/tools/check_module_symbols.py', sound / 'check_module_symbols.py')
shutil.copytree(VDAC / 'Sound/player_ui', sound.parent / 'player_ui')
if args.version:
    (fw / 'CMakeLists.txt').write_text(cmake_text.replace(
        line[0], f'set(ZIFI_VDAC4_VERSION "{VERSION}" CACHE STRING "Версия прошивки для платы VDAC4" FORCE)'),
        encoding='utf-8', newline='')
(out / 'firmware_stage.txt').write_text(str(stage))
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
print('Firmware stage:', stage, flush=True)
if args.stage_only:
    raise SystemExit(0)
command = ['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command',
           ". 'E:\\zx\\esp\\idf-env.ps1'; & (Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\\python.exe') (Join-Path $env:IDF_PATH 'tools\\idf.py') -B build build; exit $LASTEXITCODE"]
with (out / 'firmware_build.log').open('wb') as log:
    env = os.environ.copy()   # временные файлы компилятора — тоже на E:
    temp = stage / 'tmp'
    temp.mkdir()
    env['TMP'] = env['TEMP'] = str(temp)
    result = subprocess.run(command, cwd=fw, env=env, stdout=log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
record = {'version': VERSION, 'exit_code': result.returncode, 'uploaded': False, 'hardware_tested': False}
if result.returncode == 0:
    target = out / ('ESP-' + VERSION[len('vdac4-'):] + '.bin')
    shutil.copyfile(fw / 'build/zifi-vdac4.bin', target)
    record.update(sha256=sha(target), size=target.stat().st_size)
(out / 'firmware_result.json').write_text(json.dumps(record, indent=2))
print(json.dumps(record, indent=2), flush=True)
raise SystemExit(result.returncode)
