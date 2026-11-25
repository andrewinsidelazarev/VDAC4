"""Третья шара SMB «USB» — флешка в USB-C ESP32 (0.1.171, 25.11.2026).

Пользователь: «Нужно добавить в SMB сервер третью шару - USB OTG Flash ESP32. Шара должна быть
только если флешка в USB ESP32 вставлена».

Работу шары по сети (весь набор smb_reproduce_test по образу exFAT, перечень из трёх шар,
пропадание без флешки) проверяет стенд tools/host_smb/build_and_test.py (run_usb_share). Здесь — то,
чего на ПК не запустить: связка с usb_drive, с VFS («WC держит флешку как SD2») и поиск буквы FatFs
на ESP, а также обобщение сервера и моста с одной местной шары на несколько.
"""
import pathlib
import unittest

from test_wifi_power_save import function_body

ROOT = pathlib.Path(__file__).resolve().parents[1]


def read(*parts: str) -> str:
    return ROOT.joinpath(*parts).read_text(encoding="utf-8").replace("\r\n", "\n")


class UsbShareTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.share = read("src", "usb_share.cpp")
        cls.main = read("src", "main.cpp")
        cls.server = read("src", "smb_server.cpp")
        cls.server_header = read("include", "zifi", "smb_server.hpp")
        cls.bridge = read("src", "vfs_bridge.cpp")
        cls.bridge_header = read("include", "zifi", "vfs_bridge.hpp")
        cls.drive = read("src", "usb_drive.cpp")
        cls.vdisk = read("src", "vdisk_service.cpp")

    def test_wired_in_main(self) -> None:
        self.assertIn("vfsBridge_.attachLocal(SmbServer::kUsbRoot, &usb_share::vfs());", self.main)
        self.assertIn("smb_.setUsbShare([](void*) { return usb_share::shareReady(); }, nullptr);", self.main)
        # USB-шара не под ZIFI_HAS_SPI_LINK: флешка не зависит от Tang
        at = self.main.index("vfsBridge_.attachLocal(SmbServer::kUsbRoot")
        self.assertGreater(at, self.main.rindex("#if ZIFI_HAS_USB_DRIVE", 0, at))
        self.assertIn('static constexpr const char* kUsbRoot = "/\\x02USB";', self.server_header)
        self.assertIn('static constexpr const char* kUsbShare = "USB";', self.server_header)

    def test_plugin_learns_about_the_share(self) -> None:
        # 0.1.172 (пользователь: в окне плагина «писать UNC3 (если он есть)»): шестой байт ответа SMB_START
        start = function_body(self.main, "void Application::processSmbStart()")
        self.assertIn("exchange_->responseLength = 6;", start)
        self.assertIn("memset(exchange_->response, 0, 6);", start)
        self.assertIn("exchange_->response[5] = smb_.usbShareAvailable() ? 1 : 0;", start)

    def test_share_only_with_a_drive(self) -> None:
        # проба — без побочных действий (любая задача): флешка готова, WC её не держит, буква не потеряна
        probe = function_body(self.share, "bool shareReady()")
        self.assertIn("drive.isReady()", probe)
        self.assertIn("!vdisk_service::usbHeldByWc()", probe)
        self.assertIn("g_noLetterGeneration.load() != drive.generation()", probe)
        self.assertNotIn("ready()", probe.replace("isReady()", ""))
        # операции — на ядре 1 через ready() (там же usb_drive отмонтирует вынутую флешку)
        acquire = function_body(self.share, "const char* acquire() override")
        self.assertLess(acquire.index("usbDrive().ready()"), acquire.index("usbHeldByWc()"))
        # буква найдена — буфер драйвера растёт под серверы (0.1.174)
        self.assertLess(acquire.index("findLetter()"), acquire.index("growTransfer();"))

    def test_letter_search_never_touches_the_tang_card(self) -> None:
        find = function_body(self.share, "const char* findLetter()")
        # буква годна до нового монтирования
        self.assertIn("generation_ == generation && letter_[0] != 0", find)
        # размер /usb по VFS — образец, буква карты Tang — под её замком и пропускается
        self.assertLess(find.index("esp_vfs_fat_info(UsbDrive::kMountPoint"), find.index("tang_sd::Session session"))
        self.assertLess(find.index("tang_sd::Session session"), find.index("tang_sd::fatDrive()"))
        self.assertLess(find.index("strcmp(candidate, tang) == 0"), find.index("f_getfree(root"))
        self.assertIn("* fs->csize * sectorBytes == total", find)
        self.assertIn("TangVfs(*volume, TangVfs::kUsbSerial, \"USB\")", self.share)

    def test_drive_generation_and_wc_hold(self) -> None:
        self.assertIn("bool UsbDrive::isReady() const { return g_state.load() == kReady; }", self.drive)
        task = self.drive[self.drive.index("const uint8_t opened = openDrive(pending);"):]
        self.assertLess(task.index("++g_generation;"), task.index("g_state.store(opened);"))
        self.assertIn("g_usbHeld = true;", self.vdisk)
        self.assertIn("g_usbHeld = false;", function_body(self.vdisk, "void leaveUsb()"))

    def test_bridge_has_several_local_sources(self) -> None:
        self.assertIn("static constexpr size_t kMaxLocal = 2;", self.bridge_header)
        attach = function_body(self.bridge, "void VfsBridge::attachLocal(const char* root, VfsBackend* backend)")
        self.assertIn("strcmp(locals_[i].root, root) == 0", attach)
        # перенос между шарами отвергается, как и прежде
        self.assertIn('finish(false, "move-across-shares");', self.bridge)

    def test_server_shares_table(self) -> None:
        self.assertIn('{SmbServer::kUsbShare, SmbServer::kUsbRoot, 5, "USB flash ESP32", "USB:"}', self.server)
        self.assertIn('{SmbServer::kTangShare, SmbServer::kTangRoot, 6, "Tang microSD", "TANG:"}', self.server)
        # подключение по имени — только к шаре, которая есть сейчас
        self.assertIn("share = self->shareAvailable(candidate) ? candidate : kShareCount;", self.server)
        # список шар Windows — из тех, что есть сейчас
        self.assertEqual(self.server.count("smb2_dcerpc_srvsvc_reply3("), 2)
        self.assertNotIn("smb2_dcerpc_srvsvc_reply2(", self.server)
        self.assertIn("if (shareAvailable(share))", function_body(
            self.server, "int SmbServer::Impl::availableLocalShares(smb2_dcerpc_share* out, int capacity)"))
        # сведения о томе — у каждой шары свои
        self.assertIn("FsShareState savedFsState[kShareCount];", self.server)
        # пути WC не задевают местные шары и в кэше каталогов
        cache = read("src", "directory_cache.cpp")
        self.assertIn("!(value[1] >= '\\x01' && value[1] < ' ')", cache)

    def test_driver_buffer_follows_the_servers(self) -> None:
        # 0.1.173–0.1.174 (Metal Slug не получил code_ram на 0.1.172): шара передавала флешке по 16 КиБ,
        # драйвер usb_host_msc сам вырастил буфер (внутреннее ОЗУ, до вынимания флешки), и 15 872 байта
        # легли в резерв code_ram. Теперь буфер меняет прошивка: серверам — 16 КиБ, приложению — 4 КиБ.
        drive_header = read("include", "zifi", "usb_drive.hpp")
        self.assertIn("static constexpr uint32_t kTransferBytes = 4096;", drive_header)
        self.assertIn("static constexpr uint32_t kServerTransferBytes = 16384;", drive_header)
        drive = self.drive
        resize = function_body(drive, "uint32_t UsbDrive::setTransferBytes(uint32_t bytes)")
        # под замком тома FatFs флешки, новый буфер — до освобождения старого
        self.assertIn("ff_diskio_get_pdrv_disk(&device->disk)", resize)
        self.assertLess(resize.index("ff_mutex_take(pdrv)"), resize.index("usb_host_transfer_alloc("))
        self.assertLess(resize.index("usb_host_transfer_alloc(bytes, 0, &fresh) == ESP_OK"),
                        resize.index("device->xfer = fresh;"))
        self.assertLess(resize.index("device->xfer = fresh;"), resize.index("usb_host_transfer_free(old);"))
        self.assertLess(resize.index("usb_host_transfer_free(old);"), resize.index("ff_mutex_give(pdrv);"))
        self.assertIn("bytes = kTransferBytes;", resize)
        self.assertIn("g_transferBytes.store(static_cast<uint32_t>(device->xfer->data_buffer_size));", drive)
        self.assertIn("g_transferBytes.store(0);", function_body(drive, "void closeDrive()"))
        cmake = read("main", "CMakeLists.txt")
        self.assertIn('target_include_directories(${COMPONENT_LIB} PRIVATE "${ZIFI_MSC_DIR}/private_include")', cmake)
        # шара: куски не длиннее буфера драйвера, рост — после найденной буквы, не чаще kGrowRetryMs
        self.assertIn("size_t maxTransfer() const override { return usbDrive().transferBytes(); }", self.share)
        grow = function_body(self.share, "void growTransfer()")
        self.assertIn("drive.setTransferBytes(UsbDrive::kServerTransferBytes);", grow)
        self.assertIn("now - growTriedMs_ < kGrowRetryMs", grow)
        # перед стартом приложения (после остановки серверов) — назад kTransferBytes
        app = read("src", "apps", "module_app.cpp")
        start = function_body(app, "int ModuleApp::startApp(const char* name)")
        self.assertLess(start.index("host_.stopFileServers()"),
                        start.index("usbDrive().setTransferBytes(UsbDrive::kTransferBytes);"))
        self.assertLess(start.index("usbDrive().setTransferBytes(UsbDrive::kTransferBytes);"),
                        start.index("loadElf(name"))
        # TangVfs: кусок — по пределу тома на каждую операцию, после захвата тома
        vfs_header = read("include", "zifi", "tang_vfs.hpp")
        self.assertIn("virtual size_t maxTransfer() const { return kDefaultTransfer; }", vfs_header)
        vfs = read("src", "tang_vfs.cpp")
        for name in ("bool TangVfs::readFileWindow(", "bool TangVfs::writeFileWindow(",
                     "bool TangVfs::setFileSize("):
            body = vfs[vfs.index(name):]
            body = body[:body.index("\n}\n")]
            self.assertNotIn("kBufferSize", body, name)
            self.assertLess(body.index("Lock lock(*this);"), body.index("transferOf(port_)"), name)
        # стенд ПК: флешке 16 КиБ, контрольный прогон — 4 КиБ
        host = read("tools", "host_smb", "host_fatfs.cpp")
        self.assertIn("constexpr size_t kUsbTransferBytes = 16384;", host)
        self.assertIn('HostVolume g_usbVolume(1, "1:", "no USB drive", kUsbTransferBytes);', host)
        stand = read("tools", "host_smb", "build_and_test.py")
        self.assertIn("env['ZIFI_HOST_USB_TRANSFER'] = '4096'", stand)
        self.assertIn("if not limit // 2 < usb_largest <= limit:", stand)


if __name__ == "__main__":
    unittest.main()
