"""Сборка плагинов VDAC4 VFS: build/VFSRES.WMF (резидент) и build/VDAC4VFS.WMF (переключатель F10).
Таблицы сверки WC (src/wc111i.inc, src/sdzc_state.bin) — tools/make_wc_tables.py из boot.$C."""
import subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SJASM = Path(r'E:\zx\z80\zuma\sjasmplus.exe')
SHARED = HERE.parent / 'shared' / 'z80'
(HERE / 'build').mkdir(exist_ok=True)
ok = True
for name, wmf in (('resident', 'VFSRES'), ('toggle', 'VDAC4VFS')):
    r = subprocess.run([str(SJASM), '--nologo', '-DZIFI_SPI', '--inc=' + str(SHARED), '--inc=' + str(HERE / 'src'),
                        f'--sym=../build/{wmf}.sym', f'--lst=../build/{wmf}.lst', name + '.asm'],
                       cwd=HERE / 'src', capture_output=True, text=True, encoding='utf-8', errors='replace')
    text = (r.stdout + r.stderr).strip()
    print(f'{name}: rc={r.returncode}')
    if r.returncode or 'error:' in text.lower():
        print(text[-3000:])
        ok = False
    out = HERE / 'build' / (wmf + '.WMF')
    if out.exists():
        print(f'  {out.name}: {out.stat().st_size} bytes')
sys.exit(0 if ok else 1)
