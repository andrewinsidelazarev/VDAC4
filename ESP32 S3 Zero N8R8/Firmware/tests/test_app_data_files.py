"""Файлы данных приложений с карты Evo с кэшем на флешке USB (0.1.165, ABI 1.20, 23.11.2026).

Пользователь (Quake 2 на VDAC4): pak — «с SD-Card Evo. ELF — глобальный механизм», «Как ELF: кэш на
флешке», папка — «/SYSTEM/CACHE/Q2». Общий механизм для любого приложения:
- программа Z80 шлёт файл тем же MOD_QUERY / MOD_PUT / MOD_COMMIT, что ELF и ядро, с флагом 0x04 и
  именем приложения за именем файла;
- ESP кладёт его на флешку в /usb/SYSTEM/CACHE/<приложение>/ потоком (куски MOD_PUT сразу в WRITING.TMP —
  PSRAM под 50–180 МБ не берётся), COMMIT — переименование и время файла с карты; годная копия (та же
  длина, не старее) — передачи нет; флешки нет — ответ 4;
- приложение читает файл функциями zm_core_t data_open / data_read / data_close (папка — имя его ELF);
  при выгрузке приложения дескрипторы закрываются.
0.1.166 (пользователь 23.11: «Игра должна работать и без флешки otg esp32»): флешки нет, а программа Z80
назвала номер файла (байт за именем приложения) — ответ 5, файл на учёте; data_read просит куски у Z80
событием 0x6E, она отвечает кусками 0x6F (быстрым путём DMA или кадрами); выгрузка приложения обрывает
ожидание до stop(); объект файлов данных — в PSRAM.
Проверяется исходный текст прошивки; сам класс AppDataFiles — на ПК (test_app_data_files_host.py).
"""
import pathlib
import unittest

from test_wifi_power_save import function_body

ROOT = pathlib.Path(__file__).resolve().parents[1]


def read(*parts: str) -> str:
    return (ROOT.joinpath(*parts)).read_text(encoding="utf-8").replace("\r\n", "\n")


class AppDataFilesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.drive_header = read("include", "zifi", "usb_drive.hpp")
        cls.drive = read("src", "usb_drive.cpp")
        cls.modules = read("src", "apps", "module_app.cpp")
        cls.header = read("include", "zifi", "apps", "module_app.hpp")
        cls.sdk = read("modules", "sdk", "include", "zm_app.h")
        cls.files = read("src", "app_data_files.cpp")

    def test_folder_next_to_music_cache(self) -> None:
        self.assertIn('static constexpr const char* kAppDataRoot = "/usb/SYSTEM/CACHE";', self.drive_header)
        opened = function_body(self.drive, "uint8_t openDrive(uint8_t address)")
        self.assertIn('mkdir("/usb/SYSTEM/CACHE", 0777);', opened, "корень папок приложений есть всегда")

    def test_query_flag_4_goes_first(self) -> None:
        query = function_body(self.modules, "void ModuleApp::handleQuery(const PacketView& packet)")
        self.assertIn("(packet.data[0] & 0x04) != 0", query)
        self.assertLess(query.index("& 0x04"), query.index("& 0x02"), "до ядер и модулей")
        self.assertIn("handleDataQuery(name, size, stamp, app, index);", query)
        self.assertIn("const size_t indexAt = appAt + appLength + 1;", query, "номер — за приложением")
        self.assertIn("indexAt < packet.length ? packet.data[indexAt] : -1", query, "старый запрос — без")
        self.assertIn("const size_t appAt = 9 + nameLength + 1;", query, "приложение — за именем файла")

    def test_data_query_never_copies_to_drive(self) -> None:
        # 0.1.175 (пользователь 26.11.2026: «Сама игра эти файлы туда не копирует»): на флешку ничего не
        # пишется и для запускателей без номера файла — есть на флешке (поиск) — 0, нет — 4
        query = function_body(self.modules, "void ModuleApp::handleDataQuery(")
        self.assertIn("usbDrive().ready()", query)
        self.assertIn("replyQuery(AppDataFiles::kNoDrive);", query)
        self.assertNotIn("appData().begin(", query)
        self.assertNotIn("deliveryData_ = true;", query)
        self.assertNotIn("kSend", query)
        self.assertIn("appData().findOnDrive(app, name, path, sizeof(path), &length, nullptr)", query)
        self.assertIn("replyQuery(found ? AppDataFiles::kHave : AppDataFiles::kNoDrive);", query)
        self.assertNotIn("allocateDelivery", query, "PSRAM под весь файл не берётся")

    def test_data_open_searches_drive(self) -> None:
        # 0.1.175: data_open — сначала флешка (номер монтирования; без флешки — 0, не ищется), журнал —
        # строкой на файл из AppDataFiles
        opened = function_body(self.modules, "int ModuleApp::coreDataOpen(")
        self.assertIn("appData().setVolume(usbDrive().isReady() ? usbDrive().generation() : 0);", opened)
        self.assertLess(opened.index("setVolume("), opened.index("appData().open(app, name, file, size)"))
        self.assertNotIn("neither on the USB drive", opened, "строку «нет нигде» пишет AppDataFiles, раз")
        self.assertIn('appData().setLog([](void*, const char* line) { bringupLog(kTag, "%s", line); }, nullptr);',
                      self.modules)
        files = read("src", "app_data_files.cpp")
        opener = function_body(files, "int AppDataFiles::open(")
        self.assertLess(opener.index("findOnDrive(upperApp, upperName"), opener.index("remotes_"),
                        "флешка — раньше учёта с карты Evo")
        finder = function_body(files, "bool AppDataFiles::findOnDrive(")
        self.assertLess(finder.index("tryDir(static_cast<size_t>(home_));"), finder.index("listDirs();"),
                        "есть домашняя папка — только в ней")
        self.assertIn("resetSearch();", function_body(files, "void AppDataFiles::closeAll()"))
        self.assertIn("std::try_to_lock", function_body(files, "void AppDataFiles::closeAll()"),
                      "выгрузка не ждёт замок поиска")

    def test_put_and_commit_route_data(self) -> None:
        put = function_body(self.modules, "void ModuleApp::handlePut(const PacketView& packet)")
        self.assertLess(put.index("if (deliveryData_)"), put.index("delivery_ != nullptr"))
        self.assertIn("appData().put(readLe32(packet.data), packet.data + 4, packet.length - 4)", put)
        commit = function_body(self.modules, "void ModuleApp::handleCommit()")
        self.assertLess(commit.index("if (deliveryData_)"), commit.index("delivery_ != nullptr"))
        self.assertIn("appData().commit()", commit)
        drop = function_body(self.modules, "void ModuleApp::dropDelivery()")
        self.assertIn("appData().drop();", drop, "брошенная доставка: WRITING.TMP прочь")

    def test_app_functions(self) -> None:
        self.assertRegex(self.sdk, r"#define ZM_APP_ABI_MINOR (2\d|[3-9]\d)\b")
        core = self.sdk[self.sdk.index("struct zm_core {"):]
        core = core[:core.index("\n};")]
        # поля 1.20 стоят подряд; за data_close — только поля следующих версий (1.21 — link_read_checked)
        tail = core[core.index("void (*data_close)(void* file);"):]
        self.assertNotIn("data_open", tail)
        self.assertNotIn("data_read", tail)
        self.assertLess(core.index("int (*data_open)"), core.index("int (*data_read)"))
        self.assertIn("int (*data_open)(const char* name, void** file, uint32_t* size);", core)
        self.assertIn("int (*data_read)(void* file, uint32_t offset, void* data, uint32_t count, uint32_t* got);",
                      core)
        for name in ("data_open = coreDataOpen", "data_read = coreDataRead", "data_close = coreDataClose"):
            self.assertIn(f"core_.{name};", self.modules)
        opened = function_body(self.modules, "int ModuleApp::coreDataOpen(")
        self.assertIn("instance_->appName_", opened, "папка — имя ELF приложения, которое зовёт")
        self.assertIn("elf[i] != '.'", opened, "без «.ELF»")
        stop = function_body(self.modules, "void ModuleApp::stopApp()")
        self.assertIn("appData().closeAll();", stop)

    def test_no_drive_registers_remote(self) -> None:
        query = function_body(self.modules, "void ModuleApp::handleDataQuery(")
        self.assertIn("if (index >= 0) {", query)
        self.assertIn("if (appData().addRemote(app, name, size, stamp, static_cast<uint8_t>(index))) {", query)
        self.assertIn("replyQuery(AppDataFiles::kRemote);", query)
        self.assertLess(query.index("kRemote"), query.index("kNoDrive"), "умеет Z80 — ответ 5, а не 4")
        # 0.1.167 (пользователь 24.11.2026: «кэш флешки OTG не используем»): умеет Z80 — на учёт и при
        # вставленной флешке; флешку проверяют только запросы без номера файла (запускатели 0.1.165)
        self.assertLess(query.index("if (index >= 0) {"), query.index("if (!drive) {"), "учёт — до флешки")
        self.assertLess(query.index("replyQuery(AppDataFiles::kBadName);"), query.index("if (!drive) {"),
                        "не встал на учёт — 3, без копии на флешку")
        self.assertIn("appData().forgetRemote(app, name);", query, "флешка есть — с неё")
        header = read("include", "zifi", "app_data_files.hpp")
        self.assertIn("kNoDrive = 4, kRemote = 5", header)
        opened = function_body(self.modules, "int ModuleApp::coreDataOpen(")
        self.assertNotIn("usbDrive().ready()", opened, "файл на учёте открывается и без флешки")

    def test_protocol_codes_and_raw_media(self) -> None:
        protocol = read("include", "zifi", "protocol.hpp")
        self.assertIn("kEventDataRead = 0x6E,", protocol)
        self.assertIn("kDataPiece = 0x6F,", protocol)
        self.assertIn("constexpr size_t kDataPiecePrefix = 5;", protocol)
        parser = read("src", "protocol.cpp")
        self.assertIn("byte != kDataPiece", parser, "кусок принимается быстрым путём")
        self.assertIn("command_ == kDataPiece ? kDataPiecePrefix", parser, "кусок без данных быстрым путём — нет")
        link = read("src", "spi_link_core.cpp")
        self.assertIn("const bool piece = command == kDataPiece && length == 3 + kDataPiecePrefix;", link)
        self.assertIn(": piece  ? kDataPiecePrefix", link, "управление — в начало тела кадра")
        command = function_body(self.modules, "bool ModuleApp::handleCommand(const PacketView& packet)")
        self.assertLess(command.index("case kDataPiece:"), command.index("app_->command_first"),
                        "кусок — прошивке, не приложению")

    def test_remote_fetch_waits_and_repeats(self) -> None:
        fetch = function_body(self.modules, "bool ModuleApp::remoteFetch(")
        self.assertIn("host_.sendEvent(kEventDataRead, event, sizeof(event), 1000)", fetch)
        self.assertIn("xSemaphoreTake(r.done, pdMS_TO_TICKS(kRemoteWaitMs))", fetch)
        self.assertIn("for (uint32_t attempt = 0; attempt < kRemoteAttempts; ++attempt)", fetch)
        self.assertIn("const uint8_t seq = ++r.seq;", fetch, "новый номер на каждый повтор")
        self.assertIn("r.request.end();", fetch, "после срока куски этого номера — мимо")
        piece = function_body(self.modules, "void ModuleApp::handleDataPiece(")
        self.assertIn("r.request.piece(packet.data, packet.length)", piece)
        self.assertNotIn("transport().send", piece, "на кусок ответа нет")

    def test_stop_cancels_before_app_stop(self) -> None:
        stop = function_body(self.modules, "void ModuleApp::stopApp()")
        self.assertLess(stop.index("remoteCancel(true);"), stop.index("app->stop();"),
                        "задача приложения не должна ждать Z80, пока stop() ждёт её")
        self.assertNotIn("clearRemote", stop, "учёт живёт: startApp зовёт stopApp после MOD_QUERY")
        start = function_body(self.modules, "int ModuleApp::startApp(const char* name)")
        self.assertLess(start.index("stopApp();"), start.index("remoteCancel(false);"))
        self.assertLess(start.index("remoteCancel(false);"), start.index("app->start(&core_)"))

    def test_app_data_object_in_psram(self) -> None:
        body = self.modules[self.modules.index("zifi::AppDataFiles& appData() {"):]
        body = body[:body.index("\n}\n")]
        self.assertIn("heap_caps_malloc(sizeof(zifi::AppDataFiles), MALLOC_CAP_SPIRAM", body)
        self.assertNotIn("static zifi::AppDataFiles files(", body, "не во внутренней .bss")

    def test_realloc_shrinks_in_place(self) -> None:
        # Quake 2 Hunk_End урезает кусок, в который уже разложена модель: адрес не должен меняться
        body = function_body(self.modules, "void* ModuleApp::coreRealloc(void* pointer, size_t size)")
        self.assertLess(body.index("if (size <= old)"), body.index("coreAlloc(size, header->caps)"))
        self.assertIn("heap_caps_realloc(header, size + sizeof(AllocHeader), kind)", body)
        self.assertIn("esp_ptr_external_ram(header)", body, "вид памяти — где блок лежит")
        self.assertIn("kept = header;", body, "отказ realloc — блок прежний")

    def test_files_never_whole_in_memory(self) -> None:
        self.assertNotIn("malloc", self.files)
        self.assertIn("kChunkBytes", self.files, "флешка — кусками не больше 4 КБ")
        self.assertIn('static constexpr uint32_t kChunkBytes = 4096;',
                      read("include", "zifi", "app_data_files.hpp"))


if __name__ == "__main__":
    unittest.main()
