"""Правка ROM NextZXOS 2.09 для ядра Next на VDAC4: меню, F8 и NMI работают с общим выбором частоты
ядра (регистр F1, Next 0.17+), и выбор переживает F4 и повторный запуск (CORES/next.cfg).

    python patch_rom.py enNextZX.patch.json enNextZX.rom enNextZX.new.rom
    python patch_rom.py enNextMf.patch.json enNextMf.rom enNextMf.new.rom

Исходный ROM — из дистрибутива NextZXOS (sn-complete-24.11, machines/next). Правка проверяет SHA-256
исходного файла, старые байты в каждом месте и SHA-256 результата; при любом расхождении ничего не
пишет. Готовый файл кладётся на карту Tang вместо исходного (machines/next/)."""
import hashlib
import json
import sys
from pathlib import Path


def main(patch_path, rom_path, out_path):
    patch = json.loads(Path(patch_path).read_text(encoding='utf-8'))
    rom = bytearray(Path(rom_path).read_bytes())
    if hashlib.sha256(rom).hexdigest() != patch['base_sha256']:
        sys.exit(f'{rom_path}: это не тот ROM (нужен SHA-256 {patch["base_sha256"]})')
    for change in patch['changes']:
        at = int(change['offset'], 16)
        old, new = bytes.fromhex(change['old']), bytes.fromhex(change['new'])
        if len(old) != len(new) or rom[at:at + len(old)] != old:
            sys.exit(f'{rom_path}: по смещению {change["offset"]} не те байты ({change["purpose"]})')
        rom[at:at + len(new)] = new
    if hashlib.sha256(rom).hexdigest() != patch['candidate_sha256']:
        sys.exit('результат не совпал с ожидаемым SHA-256')
    Path(out_path).write_bytes(rom)
    print(f'{out_path}: готово, {len(patch["changes"])} правок, SHA-256 {patch["candidate_sha256"]}')


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
