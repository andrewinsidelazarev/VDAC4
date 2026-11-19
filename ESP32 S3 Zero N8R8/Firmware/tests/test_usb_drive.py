"""Флешка в USB-C модуля: подключение при запуске и кэш ELF (0.1.146, 19.11.2026).

На плате (0.1.145) флешка не подключилась: хост включался при первом запросе ресурса
музыки, когда плеер и декодер уже заняли внутреннюю память, — «USBH: HCD Pipe alloc
error: ESP_ERR_NO_MEM» при 16 КБ свободных. Теперь:
- хост и флешку включает задача usb-drive при запуске ESP, если к разъёму не подключён
  ПК; стеки задач хоста — в PSRAM; буфер обмена драйвера выращен до kTransferBytes сразу
  после монтирования, и кэш ресурсов и том VFS читают кусками не больше его;
- не записанный на флешку ресурс — с причиной в журнале;
- кэш ELF (слово пользователя): модуль в кэше ESP — как раньше; иначе годная копия в
  /ELF флешки (та же длина, не старее карты Evo) — передачи нет, запуск с неё; иначе
  модуль приходит с карты, ложится в /ELF (папки нет — создать) и запускается оттуда.
Проверяется исходный текст: сама прошивка на ПК не собирается.
"""
import pathlib
import re
import unittest

from test_wifi_power_save import function_body

ROOT = pathlib.Path(__file__).resolve().parents[1]


def read(*parts: str) -> str:
    return (ROOT.joinpath(*parts)).read_text(encoding="utf-8")


class UsbDriveTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.main = read("src", "main.cpp")
        cls.drive = read("src", "usb_drive.cpp")
        cls.drive_header = read("include", "zifi", "usb_drive.hpp")
        cls.cache = read("src", "resource_disk_cache.cpp")
        cls.cache_header = read("include", "zifi", "resource_disk_cache.hpp")
        cls.sources = read("src", "vdisk_sources.cpp")
        cls.modules = read("src", "apps", "module_app.cpp")

    def test_drive_starts_at_boot(self) -> None:
        begin = function_body(self.main, "void Application::begin()")
        self.assertIn("usbDrive().begin();", begin)
        poll = function_body(self.main, "void Application::pollLink()")
        self.assertIn("usbDrive().poll();", poll)
        # ready() больше не включает хост и не ждёт флешку
        ready = function_body(self.drive, "bool UsbDrive::ready()")
        self.assertNotIn("startHost", ready)
        self.assertNotIn("xSemaphoreTake", ready)

    def test_pc_on_port_keeps_console(self) -> None:
        task = function_body(self.drive, "void driveTask(void*)")
        self.assertLess(task.index("usb_serial_jtag_is_connected()"), task.index("startHost()"))

    def test_host_tasks_in_psram(self) -> None:
        self.assertIn("msc.create_backround_task = false;", self.drive)
        for task in ('"usb-host"', '"usb-msc"', '"usb-drive"'):
            call = re.search(r"xTaskCreatePinnedToCoreWithCaps\(\w+, " + task + r"[^;]*;", self.drive)
            self.assertIsNotNone(call, task)
            self.assertIn("MALLOC_CAP_SPIRAM", call.group(0), task)
        self.assertNotIn("xTaskCreatePinnedToCore(", self.drive)

    def test_transfer_buffer_bounded(self) -> None:
        self.assertIn("static constexpr uint32_t kTransferBytes = 4096;", self.drive_header)
        self.assertIn("static constexpr uint32_t kChunkBytes = 4096;", self.cache_header)
        self.assertIn("constexpr uint32_t kChunk = ResourceDiskCache::kChunkBytes;", self.cache)
        self.assertIn("ResourceDiskCache::kChunkBytes <= UsbDrive::kTransferBytes", self.drive)
        opened = function_body(self.drive, "uint8_t openDrive(uint8_t address)")
        # буфер — до монтирования: FatFs пишет том с первых команд (0.1.147)
        self.assertLess(opened.index("growTransferBuffer(device, info.sector_size)"),
                        opened.index("msc_host_vfs_register("))
        # новая папка обнуляется записями по MAX_MALLOC: не больше передачи (паника 0.1.146)
        ff = (ROOT / "components" / "fatfs" / "src" / "ff.c").read_text(encoding="utf-8")
        self.assertIn("#define MAX_MALLOC\t0x1000", ff)
        self.assertIn("mkdir(UsbDrive::kElfFolder, 0777);", opened)
        read_usb = function_body(self.sources, "bool UsbSource::read(")
        self.assertIn("UsbDrive::kTransferBytes", read_usb)

    def test_skipped_resource_copy_is_logged(self) -> None:
        copy = function_body(self.modules, "void ModuleApp::copyResourceToDrive(")
        self.assertIn("usbDrive().status()", copy)

    def test_elf_cache_order_in_query(self) -> None:
        query = function_body(self.modules, "void ModuleApp::handleQuery(")
        # кэш ESP — первым (как раньше), потом флешка, потом передача с карты
        self.assertLess(query.index("cache_->has(name, size, stamp)"),
                        query.index("moduleFromDrive(name, size, stamp)"))
        self.assertLess(query.index("moduleFromDrive(name, size, stamp)"),
                        query.index("allocateDelivery(name, size, resource)"))
        fresh = function_body(self.modules, "bool ModuleApp::moduleFromDrive(")
        self.assertIn("ResourceDiskCache(UsbDrive::kElfFolder).fresh(name, size, stamp)", fresh)

    def test_elf_written_to_drive_and_started_from_it(self) -> None:
        commit = function_body(self.modules, "void ModuleApp::handleCommit()")
        self.assertLess(commit.index("storeModuleOnDrive(deliveryName_"),
                        commit.index("cache_->store(deliveryName_"))
        store = function_body(self.modules, "bool ModuleApp::storeModuleOnDrive(")
        self.assertLess(store.index("mkdir(UsbDrive::kElfFolder, 0777);"), store.index(".write(name"))
        self.assertIn("markDriveModule(name, size, stamp);", store)
        load = function_body(self.modules, "int ModuleApp::loadElf(")
        self.assertLess(load.index("driveModuleSize(name, &driveSize)"), load.index("cache_->find(name)"))
        self.assertIn("ResourceDiskCache(UsbDrive::kElfFolder).read(name, file, size)", load)


if __name__ == "__main__":
    unittest.main()
