"""Ядро ПЛИС с карты Evo с кэшем на флешке USB (0.1.164, 23.11.2026).

Пользователь: «Api проброса ядра FPGA с SD card с кэшем в флешке usb-c, как мы это делали для elf.
Имею в виду с SD card Evo». Программа Z80 шлёт ИМЯ.BIN теми же командами, что ELF: MOD_QUERY с
флагом 2, MOD_PUT, MOD_COMMIT. ESP:
- годная копия (та же длина, не старее карты) на флешке /usb/cores или образ в PSRAM — передачи нет;
- принятое ядро проверяется как битстрим VDAC4 (то же, что проверяет BL616) и ложится на флешку;
  флешки нет — образ остаётся в PSRAM;
- ядро получает отметку «с карты Evo»: core_load берёт его оттуда, а не с SD Tang, и кладёт в кэш
  BL616, если там его нет; ядро уже в кэше — образ в PSRAM освобождается.
Проверяется исходный текст: сама прошивка на ПК не собирается.
"""
import pathlib
import unittest

from test_wifi_power_save import function_body

ROOT = pathlib.Path(__file__).resolve().parents[1]


def read(*parts: str) -> str:
    return (ROOT.joinpath(*parts)).read_text(encoding="utf-8")


class EvoCoresTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.drive = read("src", "usb_drive.cpp")
        cls.drive_header = read("include", "zifi", "usb_drive.hpp")
        cls.modules = read("src", "apps", "module_app.cpp")
        cls.store = read("src", "core_store.cpp")
        cls.store_header = read("include", "zifi", "core_store.hpp")
        cls.host = read("tools", "unreal", "vdac4_esp_host.cpp")

    def test_core_folder_on_drive(self) -> None:
        self.assertIn('static constexpr const char* kCoreFolder = "/usb/cores";', self.drive_header)
        opened = function_body(self.drive, "uint8_t openDrive(uint8_t address)")
        self.assertIn("mkdir(UsbDrive::kCoreFolder, 0777);", opened)
        self.assertIn("isDirectory(UsbDrive::kCoreFolder)", opened)

    def test_query_flag_routes_cores(self) -> None:
        query = function_body(self.modules, "void ModuleApp::handleQuery(")
        # флаг ядра — до кэша модулей ESP и до доставки: ядро в кэш ESP не ложится
        self.assertLess(query.index("(packet.data[0] & 0x02) != 0"), query.index("cache_->has(name, size, stamp)"))
        self.assertLess(query.index("handleCoreQuery(name, size, stamp);"),
                        query.index("allocateDelivery(name, size, resource)"))

    def test_core_query_order(self) -> None:
        q = function_body(self.modules, "void ModuleApp::handleCoreQuery(")
        self.assertIn('strcmp(dot, ".BIN") != 0', q)
        self.assertIn("size > kMaxCoreBytes", q)
        # образ в PSRAM, потом флешка, и только потом передача с карты
        self.assertLess(q.index("core_store::evoHeld(core, size, stamp)"),
                        q.index("ResourceDiskCache(UsbDrive::kCoreFolder).fresh(name, size, stamp)"))
        self.assertLess(q.index("ResourceDiskCache(UsbDrive::kCoreFolder).fresh(name, size, stamp)"),
                        q.index("allocateDelivery(name, size, false)"))
        self.assertIn("core_store::evoOffer(core, nullptr, size, stamp);", q)
        self.assertIn("deliveryCore_ = true;", q)
        self.assertIn("constexpr uint32_t kMaxCoreBytes = 1024 * 1024;", self.modules)

    def test_commit_checks_then_stores(self) -> None:
        commit = function_body(self.modules, "void ModuleApp::handleCommit()")
        self.assertLess(commit.index("if (deliveryCore_)"), commit.index("if (deliveryResource_)"))
        self.assertIn("where = commitCore();", commit)
        core = function_body(self.modules, "const char* ModuleApp::commitCore()")
        self.assertLess(core.index("core_store::checkEvoImage(delivery_, deliverySize_)"),
                        core.index("mkdir(UsbDrive::kCoreFolder, 0777);"))
        self.assertLess(core.index(".write(deliveryName_, delivery_, deliverySize_, deliveryStamp_)"),
                        core.index("core_store::evoOffer(core, nullptr, deliverySize_, deliveryStamp_);"))
        # без флешки образ уходит к core_store (кэш BL616 или PSRAM), и доставка его уже не освобождает
        tail = core[core.index("core_store::evoStoreToCache(core, delivery_, deliverySize_, deliveryStamp_)"):]
        self.assertIn("delivery_ = nullptr;", tail)
        drop = function_body(self.modules, "void ModuleApp::dropDelivery()")
        self.assertIn("deliveryCore_ = false;", drop)

    def test_delivery_room_releases_core_images(self) -> None:
        room = function_body(self.modules, "uint8_t* ModuleApp::allocateDelivery(")
        self.assertLess(room.index("core_store::evoReleaseMemory()"), room.index("stopApp();"))

    def test_load_prefers_evo_card(self) -> None:
        load = function_body(self.store, "const char* load(const char* name, uint32_t* version,")
        # ядро слота flash Tang (VIDEO) — как прежде; ядро с карты Evo — до SD Tang
        self.assertLess(load.index("for (const FlashCore& core : kFlashCores)"), load.index("syncEvo(n, progress, ctx)"))
        self.assertLess(load.index("syncEvo(n, progress, ctx)"), load.index("tang_sd::mount() == nullptr"))
        self.assertIn("evo != kNotEvo", load)

    def test_sync_evo(self) -> None:
        sync = function_body(self.store, "const char* syncEvo(const char* name, Progress progress, void* ctx)")
        self.assertIn("evoFind(name, &mark, true, true)", sync)
        self.assertLess(sync.index("cache.find(name, digest, &found)"), sync.index("cache.put(name, mark.size"))
        # образ из PSRAM, иначе копия на флешке
        self.assertLess(sync.index("ImageReader{&image, memRead}"), sync.index("ImageReader{f, fileRead}"))
        # ядро легло в кэш BL616 — образ в PSRAM освобождается
        self.assertIn("if (problem == nullptr && mark.data != nullptr) {\n    heap_caps_free(mark.data);", sync)
        self.assertIn("evoDone(name, mark, problem == nullptr ? digest : nullptr, mark.data);", sync)

    def test_image_check_matches_bl616(self) -> None:
        check = function_body(self.store, "const char* checkEvoImage(const uint8_t* data, uint32_t size)")
        self.assertIn("size > tang::kSlotBytes", check)
        self.assertIn("core_cache::checkImage(data, 128, data + size - 64, 64, &usercode)", check)
        self.assertIn("uint32_t evoReleaseMemory();", self.store_header)

    def test_core_app_for_evo_core(self) -> None:
        # CORE_APP ядра с карты Evo: SD Tang не читается, слой — присланный ELF или общий
        app = function_body(self.modules, "void ModuleApp::handleCoreApp(")
        self.assertLess(app.index("core_store::evoMarked(name)"), app.index("core_store::readElf("))
        layer = function_body(self.modules, "void ModuleApp::startEvoCoreLayer(")
        self.assertIn("driveModuleSize(module, &driveSize)", layer)
        self.assertIn("cache_->find(module)", layer)
        self.assertLess(layer.index('startBuiltin(coreBuiltinApp(), "CORE")'), layer.index("startApp(module)"))
        self.assertIn("bool evoMarked(const char* name);", self.store_header)

    def test_works_without_drive(self) -> None:
        # без флешки: на MOD_QUERY — кэш BL616 по отметке с SHA (NVS), на MOD_COMMIT — сразу в кэш BL616
        q = function_body(self.modules, "void ModuleApp::handleCoreQuery(")
        self.assertLess(q.index("ResourceDiskCache(UsbDrive::kCoreFolder).fresh(name, size, stamp)"),
                        q.index("core_store::evoClaimCached(core, size, stamp)"))
        self.assertLess(q.index("core_store::evoClaimCached(core, size, stamp)"),
                        q.index("allocateDelivery(name, size, false)"))
        core = function_body(self.modules, "const char* ModuleApp::commitCore()")
        self.assertIn("core_store::evoStoreToCache(core, delivery_, deliverySize_, deliveryStamp_)", core)
        store = function_body(self.store, "const char* evoStoreToCache(const char* name, uint8_t* data,")
        self.assertLess(store.index("cache.find(n, digest, &found)"), store.index("cache.put(n, size, digest"))
        self.assertIn("evoOffer(n, data, size, stamp);", store)       # не вышло — образ в PSRAM
        claim = function_body(self.store, "bool evoClaimCached(const char* name, uint32_t size, uint32_t stamp)")
        self.assertIn("tang_sd::Session session(tang_sd::kQuickMs);", claim)
        self.assertIn("found != core_cache::Found::kFound", claim)
        # SHA — в NVS; заявки — только из MOD_QUERY этого запуска; список SD Tang их снимает
        self.assertIn("nvs_set_blob(handle, kEvoKey, saved, sizeof saved)", self.store)
        self.assertIn("evoLoadSaved(table);", function_body(self.store, "EvoCore* evoTable()"))
        self.assertIn("evoFind(name, &mark, true, true)", function_body(
            self.store, "const char* syncEvo(const char* name, Progress progress, void* ctx)"))
        self.assertIn("core_store::evoReleaseClaims();", function_body(self.modules, "void ModuleApp::handleCoreList()"))

    def test_nvs_only_from_internal_stack(self) -> None:
        # 24.11.2026, 0.1.169: загрузчик ядер zm-coreload (стек в PSRAM) упал в NVS при первой загрузке
        # нового Q2.BIN — syncEvo → evoDone → evoSave → nvs_set_blob, assert cache_utils.c:126
        # (esp_task_stack_is_sane_cache_disabled). С 0.1.170 NVS пишет только evoFlush из задачи
        # обмена с Z80 (loopTask, стек во внутренней памяти).
        done = function_body(self.store, "void evoDone(const char* name, const EvoCore& was,")
        self.assertNotIn("evoSave()", done)
        self.assertIn("g_evoSavePending = true;", done)
        flush = function_body(self.store, "void evoFlush()")
        self.assertLess(flush.index("esp_ptr_internal(&probe)"), flush.index("evoSave();"))
        self.assertLess(flush.index("g_evoSavePending = false;"), flush.index("evoSave();"))
        # evoSave зовёт только evoFlush; таблицу (с чтением NVS) заводят только evoOffer и evoClaimCached
        self.assertEqual(self.store.count("evoSave();"), 1)
        self.assertEqual(self.store.count("evoTable()"), 3)      # определение и два вызова
        for name in ("void evoOffer(", "bool evoClaimCached("):
            self.assertIn("evoTable()", function_body(self.store, name))
        self.assertIn("void evoFlush();", self.store_header)
        main = read("src", "main.cpp")
        self.assertIn("core_store::evoFlush();", function_body(main, "void Application::pollLink()"))
        # загрузчик ядер по-прежнему со стеком в PSRAM — значит, флешь ESP ему по-прежнему нельзя
        self.assertIn('"zm-coreload", 8192, this, 2, &coreLoader_, 1,', self.modules)

    def test_settings_copy_not_from_psram_stack(self) -> None:
        # Тот же род ошибки (найден в коде 24.11.2026, на плате не проявлялся): перед сменой ядра
        # core_store зовёт tang_flash::flush() из zm-coreload, а сохранение настроек пишет их копию в
        # LittleFS (флешь ESP). С 0.1.170 копия из задачи со стеком в PSRAM откладывается до poll().
        self.assertIn("tang_flash::flush();", self.store)
        flash = read("src", "tang_flash.cpp")
        remember = function_body(flash, "void rememberRecord(const uint32_t* record)")
        self.assertLess(remember.index("esp_ptr_internal(&probe)"), remember.index("LittleFS.open(kSettingsFile"))
        self.assertLess(remember.index("g_copyPending = true;"), remember.index("LittleFS.open(kSettingsFile"))
        poll = function_body(flash, "void poll()")
        self.assertIn("if (g_copyPending.load() && g_haveSaved)", poll)
        self.assertIn("rememberRecord(g_saved);", poll)
        # poll зовёт только задача LINK (стек во внутренней памяти)
        link = read("src", "tang_link.cpp")
        self.assertIn('xTaskCreatePinnedToCore(taskMain, "zifi-tang", 6144, nullptr, 1, nullptr, 0)', link)
        self.assertIn("tang_flash::poll();", link)

    def test_unreal_model_knows_cores(self) -> None:
        query = function_body(self.host, "void handleQuery(const PacketView& packet)")
        self.assertIn("handleCoreQuery(name, size, stamp);", query)
        commit = function_body(self.host, "void handleCommit()")
        self.assertIn("where = commitCore();", commit)


if __name__ == "__main__":
    unittest.main()
