"""Досборка прошивки 0.1.97 в уже подготовленном каталоге (build_firmware.py --stage-only или прежний
прогон): временные файлы — в каталоге стадии, кэш ccache — E:/zx/cache/ccache (задаёт idf-env.ps1)."""
import hashlib, json, os, shutil, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
out = HERE / 'release'
stage = Path((out / 'firmware_stage.txt').read_text().strip())
assert stage.is_dir() and stage.parent == Path(r'E:\zx\esp\stages'), stage
fw = stage / 'ESP32 S3 Zero N8R8/Firmware'
# свежие файлы VFS поверх (могли поменяться после подготовки каталога)
for source in (HERE / 'firmware').rglob('*'):
    if source.is_file():
        shutil.copyfile(source, fw / source.relative_to(HERE / 'firmware'))
env = os.environ.copy()
temp = stage / 'tmp'
temp.mkdir(exist_ok=True)
env.update(TMP=str(temp), TEMP=str(temp))
command = ['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command',
           ". 'E:\\zx\\esp\\idf-env.ps1'; "
           "& (Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\\python.exe') (Join-Path $env:IDF_PATH 'tools\\idf.py') -B build build; exit $LASTEXITCODE"]
with (out / 'firmware_build.log').open('wb') as log:
    result = subprocess.run(command, cwd=fw, env=env, stdout=log, stderr=subprocess.STDOUT,
                            creationflags=subprocess.CREATE_NO_WINDOW)
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
record = {'version': 'vdac4-0.1.97', 'exit_code': result.returncode, 'uploaded': False, 'hardware_tested': False,
          'stage': str(stage),
          'sources': {str(p.relative_to(HERE)): sha(p) for p in (HERE / 'firmware').rglob('*') if p.is_file()}}
if result.returncode == 0:
    target = out / 'ESP-0.1.97.bin'
    shutil.copyfile(fw / 'build/zifi-vdac4.bin', target)
    record.update(sha256=sha(target), size=target.stat().st_size)
(out / 'firmware_result.json').write_text(json.dumps(record, indent=2))
print(json.dumps(record, indent=2), flush=True)
sys.exit(result.returncode)
