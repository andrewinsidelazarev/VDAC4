"""Перемещение R_XTENSA_RELATIVE загрузчика ELF на ПК.

tests/elf_relocate_host.cpp вызывает настоящий esp_elf_arch_relocate
(components/elf_loader/src/arch/esp_elf_xtensa.c); esp_elf_map_sym и elf_remap_text
тест вырезает из esp_elf.c и esp_elf_adapter.c (остальное в них — ESP-IDF). Указатель
на конец секции (конец цикла по таблице в конце .rodata, MIDI.ELF 1.7..1.7.1)
обязан перейти в конец её копии на плате, а не в 0; конец .text — на шину команд;
значение на стыке .data и .bss — начало .bss; адрес между секциями — 0.
Без Visual Studio Build Tools тест пропускается.
"""
import re
import subprocess
import unittest

from test_weather_parse import ROOT, build_exe

BUILD = ROOT / '.test-build' / 'elf_relocate_host'
LOADER = ROOT / 'components' / 'elf_loader'


def function_source(path, name: str) -> str:
    """Определение функции name из исходника загрузчика (до закрывающей скобки в начале строки)."""
    text = path.read_text(encoding='utf-8').replace('\r\n', '\n')
    match = re.search(r'^uintptr_t ' + name + r'\(.*?^\}\n', text, re.S | re.M)
    if not match:
        raise AssertionError(f'{name} не найдена в {path}')
    return match.group(0)


class ElfRelocateHostTest(unittest.TestCase):
    def test_relative_section_end(self) -> None:
        BUILD.mkdir(parents=True, exist_ok=True)
        extracted = BUILD / 'extracted.c'
        extracted.write_text(
            '#include "esp_elf.h"\n#include "private/elf_platform.h"\n'
            '#define OFFSET_TEXT_VALUE (0x42000000 - 0x3C000000) /* ESP32-S3: SOC_IROM_LOW - SOC_DROM_LOW */\n'
            + function_source(LOADER / 'src' / 'esp_elf.c', 'esp_elf_map_sym')
            + function_source(LOADER / 'src' / 'esp_elf_adapter.c', 'elf_remap_text'),
            encoding='utf-8')
        exe = build_exe(BUILD, 'elf_relocate_host.exe',
                        [ROOT / 'tests' / 'stubs_elf_loader', LOADER / 'include'],
                        [(LOADER / 'src' / 'arch' / 'esp_elf_xtensa.c', 'xtensa.obj'),
                         (extracted, 'extracted.obj'),
                         (ROOT / 'tests' / 'elf_relocate_host.cpp', 'host.obj')])
        if exe is None:
            self.skipTest('нет MSVC (vcvars64.bat): стенд загрузчика ELF пропущен')
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.split(), ['ok', 'relative_section_end'])


if __name__ == '__main__':
    unittest.main()
