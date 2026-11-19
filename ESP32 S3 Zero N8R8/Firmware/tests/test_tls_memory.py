"""TLS без внутренней памяти на каждую запись (0.1.154, 19.11.2026).

Аппаратный AES ESP-IDF 6.0 шифрует запись TLS через DMA, а буфер TLS в PSRAM
(CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC) не выровнен по строке кэша: на каждую запись он берёт буфер
до 1,6 КБ из внутренней памяти. С флешкой в ESP при плеере её было ~5 КБ кусками — «esp-aes:
Failed to allocate memory», соединение с Deezer оборвалось на 8 КБ картинки из 70, обложки нет.
Поэтому AES — программный, а его таблицы — во флеш (не в .bss внутренней памяти).
"""
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def lines(name: str) -> set[str]:
    return {line.strip() for line in ROOT.joinpath(name).read_text(encoding='utf-8').splitlines()}


class TlsMemoryTest(unittest.TestCase):
    def test_hardware_aes_off(self) -> None:
        for name in ('sdkconfig.defaults', 'sdkconfig'):
            with self.subTest(name=name):
                config = lines(name)
                self.assertIn('# CONFIG_MBEDTLS_HARDWARE_AES is not set', config)
                self.assertNotIn('CONFIG_MBEDTLS_HARDWARE_AES=y', config)

    def test_tls_buffers_in_psram_tables_in_flash(self) -> None:
        self.assertIn('CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y', lines('sdkconfig.defaults'))
        self.assertIn('CONFIG_MBEDTLS_AES_ROM_TABLES=y', lines('sdkconfig'))


if __name__ == '__main__':
    unittest.main()
