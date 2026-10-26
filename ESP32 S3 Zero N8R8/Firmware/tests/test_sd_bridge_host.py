"""Host-тест драйвера microSD Tang через мост ядра TS-Config 1.30 (src/sd_bridge_card.cpp).

tests/sd_bridge_host.cpp — модель моста (fpga/vdac2/rtl/fb_sdspi.sv) и карты в режиме SPI:
инициализация SDHC и SDSC, чтение и запись блоков, повторы по CRC16, отказ записи, долгий ACMD41,
карты нет, ядра без моста. Собирается MSVC; без Visual Studio Build Tools тест пропускается.
"""
import subprocess
import unittest

from test_weather_parse import ROOT, build_exe

BUILD = ROOT / '.test-build' / 'sd_bridge'


class SdBridgeHostTest(unittest.TestCase):
    def test_scenarios(self):
        exe = build_exe(BUILD, 'sd_bridge_host.exe', [ROOT / 'include'],
                        [(ROOT / 'src' / 'sd_bridge_card.cpp', 'card.obj'),
                         (ROOT / 'tests' / 'sd_bridge_host.cpp', 'host.obj')])
        if exe is None:
            raise unittest.SkipTest('нет Visual Studio Build Tools')
        result = subprocess.run([str(exe)], capture_output=True, timeout=120)
        out = result.stdout.decode('utf-8', 'replace')
        print(out)
        self.assertEqual(result.returncode, 0, out)
        self.assertIn('ТЕСТ ПРОЙДЕН', out)


if __name__ == '__main__':
    unittest.main()
