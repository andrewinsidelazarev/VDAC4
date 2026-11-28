"""Предел одной передачи флешки USB (0.1.179, 28.11.2026): паника 0.1.177 «block already marked as free».

27.11.2026 на плате прошивка 0.1.177 упала в задаче zm-coreload, когда Quake 2 принёс новое ядро:
assert tlsf_free «block already marked as free». Backtrace (по ELF той же сборки):
scsi_cmd_read10 → scsi_cmd_sense → bot_execute_command → msc_bulk_transfer (msc_host.c:688) →
usb_host_transfer_free → heap_caps_free. Причина: core_store::fileSha читал файл ядра на флешке по
8 КБ, а буфер драйвера флешки после старта приложения — 4 КБ. Драйвер для длинной передачи сначала
освобождает старый буфер, потом выделяет новый; у игры самый большой свободный кусок внутреннего ОЗУ
был ровно 8192 байта — нового не дали, и драйвер остался с освобождённым буфером. Чтение вернуло
ошибку, драйвер пошёл спросить причину (REQUEST SENSE) через тот же буфер и освободил его второй раз.

Исправление:
- components/fatfs/diskio/diskio.c — предел одной передачи диска (ff_diskio_set_max_transfer):
  ff_disk_read и ff_disk_write делят длинные чтения и записи на куски не длиннее предела;
- src/usb_drive.cpp — предел тома флешки = длина буфера драйвера: после регистрации тома и при
  каждой смене буфера (setTransferBytes, под замком тома). Драйвер свой буфер больше не растит;
- src/core_store.cpp — fileSha читает кусками kReadChunk = 4 КБ (не больше буфера).

Что проверяется:
- стенд MSVC (tests/fatfs_diskio_host.c): настоящий diskio.c с поддельным диском — куски не длиннее
  предела, секторы и места в буфере, данные, ошибка куска, смена предела, сброс при регистрации;
- по исходникам: где задаётся предел, чтение ядра кусками не больше буфера.
На плате это не проверено: стенд не повторяет драйвер USB.
"""
import subprocess
import unittest

from test_weather_parse import ROOT, build_exe
from test_wifi_power_save import function_body

BUILD = ROOT / '.test-build' / 'fatfs_diskio_host'
FATFS = ROOT / 'components' / 'fatfs'


def text(path) -> str:
    return path.read_text(encoding='utf-8').replace('\r\n', '\n')


class UsbTransferLimitTest(unittest.TestCase):
    def test_diskio_split_host(self) -> None:
        # заглушки (sdkconfig.h, esp_err.h, FreeRTOS) — раньше настоящих заголовков FatFs
        exe = build_exe(BUILD, 'fatfs_diskio_host.exe',
                        [ROOT / 'tests' / 'stubs_fatfs_diskio', FATFS / 'diskio', FATFS / 'src'],
                        [(FATFS / 'diskio' / 'diskio.c', 'diskio.obj'),
                         (ROOT / 'tests' / 'fatfs_diskio_host.c', 'host.obj')])
        if exe is None:
            self.skipTest('нет MSVC (vcvars64.bat)')
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), 'OK')

    def test_limit_set_by_usb_drive(self) -> None:
        drive = text(ROOT / 'src' / 'usb_drive.cpp')
        self.assertIn('#include "diskio_impl.h"', drive)
        limit = function_body(drive, 'void limitTransfers(msc_host_device_handle_t device, uint32_t bufferBytes)')
        self.assertIn('ff_diskio_set_max_transfer(pdrv, sectors > 0 ? sectors : 1, sector);', limit)
        opened = function_body(drive, 'uint8_t openDrive(uint8_t address)')
        # регистрация тома сбрасывает предел — задать его после неё
        self.assertLess(opened.index('msc_host_vfs_register('), opened.index('limitTransfers(device,'))
        self.assertIn('g_sectorBytes.store(info.sector_size);', opened)
        resize = function_body(drive, 'uint32_t UsbDrive::setTransferBytes(uint32_t bytes)')
        # новый предел — вместе с новым буфером, ещё под замком тома
        self.assertLess(resize.index('limitTransfers(device, now);'), resize.index('ff_mutex_give(pdrv);'))
        close = function_body(drive, 'void closeDrive()')
        self.assertIn('g_sectorBytes.store(0);', close)

    def test_register_resets_limit(self) -> None:
        diskio = text(FATFS / 'diskio' / 'diskio.c')
        register = function_body(diskio, 'void ff_diskio_register(BYTE pdrv, const ff_diskio_impl_t* discio_impl)')
        self.assertIn('s_max_sectors[pdrv] = 0;', register)
        header = text(FATFS / 'diskio' / 'diskio_impl.h')
        self.assertIn('void ff_diskio_set_max_transfer(BYTE pdrv, UINT max_sectors, UINT sector_bytes);', header)
        readme = text(FATFS / 'README.VDAC4.md')
        self.assertIn('ff_diskio_set_max_transfer', readme)

    def test_core_sha_reads_in_buffer_sized_pieces(self) -> None:
        store = text(ROOT / 'src' / 'core_store.cpp')
        self.assertIn('constexpr size_t kReadChunk = 4096;', store)
        self.assertIn('static_assert(kReadChunk <= UsbDrive::kTransferBytes,', store)
        sha = function_body(store, 'bool fileSha(const char* path, uint32_t* size, uint8_t digest[32])')
        self.assertIn('static uint8_t buf[kReadChunk];', sha)


if __name__ == '__main__':
    unittest.main()
