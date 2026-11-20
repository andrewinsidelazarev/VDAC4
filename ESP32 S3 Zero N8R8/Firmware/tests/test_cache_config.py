"""Кэш ESP32-S3 (0.1.155, 20.11.2026).

Строка кэша данных — 64 Б: замер Metal Slug на плате дал простой данных 20,2 → 18,5 % тактов,
внутренняя RAM при этом не меняется (размер кэша данных прежний, 32 КБ).

Кэш команд — 16 КБ, и тест это сторожит: 32 КБ заняли бы всю SRAM0, код IRAM переехал бы в SRAM1
и сдвинул данные — внутренней RAM стало бы на 16 КБ меньше. При плеере её остаётся ~9 КБ, при старте
Metal Slug ~19 КБ: без этих 16 КБ откажут стеки приложений, буферы Wi-Fi, USB-хост и TLS.
"""
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def lines(name: str) -> set[str]:
    return {line.strip() for line in ROOT.joinpath(name).read_text(encoding='utf-8').splitlines()}


class CacheConfigTest(unittest.TestCase):
    def test_data_cache_line_64(self) -> None:
        self.assertIn('CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y', lines('sdkconfig.defaults'))
        config = lines('sdkconfig')
        self.assertIn('CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE=64', config)
        self.assertIn('CONFIG_ESP32S3_DATA_CACHE_SIZE=0x8000', config)

    def test_instruction_cache_keeps_internal_ram(self) -> None:
        config = lines('sdkconfig')
        self.assertIn('CONFIG_ESP32S3_INSTRUCTION_CACHE_16KB=y', config)
        self.assertIn('CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE=0x4000', config)
        self.assertNotIn('CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y', config)


if __name__ == '__main__':
    unittest.main()
