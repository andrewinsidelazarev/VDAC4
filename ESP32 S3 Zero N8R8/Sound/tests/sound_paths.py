"""Пути частей проекта для тестов звука.

ROOT — папка Sound (модули ESP, MP3 Player, библиотеки кодеков), FIRMWARE —
прошивка (src/, include/, modules/sdk, components/tracker, managed_components),
NETWORK — сетевые программы Z80. Тесты звука берут общие помощники из тестов
прошивки (test_weather_parse: сборка MSVC) и основу эмулятора Z80 из сетевых
(test_ftp_plugin_z80, test_spi_link_z80, spi_link_model), поэтому их папки
добавляются в sys.path.
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
FIRMWARE = ROOT.parent / 'Firmware'
NETWORK = ROOT.parent / 'Network'

for _tests in (FIRMWARE / 'tests', NETWORK / 'tests'):
    if str(_tests) not in sys.path:
        sys.path.append(str(_tests))
