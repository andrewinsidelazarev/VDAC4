"""Проверка образа ELF модуля (src/elf_image.cpp) на ПК.

tests/elf_image_host.cpp: собранные модули modules/out/*.ELF обязаны пройти
проверку; испорченные (обрезка, таблица секций за концом файла, переполнение
смещения, индекс таблицы имён, чужая машина, точка входа вне .text, секция за
концом, имя секции, символ и место релокации, ссылка на таблицу символов) —
отвергнуты; на тысячах случайных порч проверка не читает за образом (он лежит
вплотную к странице без доступа). Без Visual Studio Build Tools тест пропускается.
"""
import subprocess
import unittest

from test_weather_parse import ROOT, build_exe

BUILD = ROOT / '.test-build' / 'elf_image_host'
# Собранные модули — в части Sound (Sound/modules/out).
MODULES = sorted((ROOT.parent / 'Sound' / 'modules' / 'out').glob('*.ELF'))


class ElfImageHostTest(unittest.TestCase):
    def test_images(self) -> None:
        if not MODULES:
            self.skipTest('нет собранных модулей в modules/out')
        exe = build_exe(BUILD, 'elf_image_host.exe', [ROOT / 'include'],
                        [(ROOT / 'src' / 'elf_image.cpp', 'image.obj'),
                         (ROOT / 'tests' / 'elf_image_host.cpp', 'host.obj')])
        if exe is None:
            self.skipTest('нет MSVC (vcvars64.bat): стенд проверки ELF пропущен')
        result = subprocess.run([str(exe), *map(str, MODULES)], capture_output=True, text=True, timeout=300)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.split(), ['ok', 'real_modules_are_valid',
                                                 'ok', 'damaged_images_are_refused_within_bounds'])


if __name__ == '__main__':
    unittest.main()
