"""Исполняемое внутреннее ОЗУ для кода модулей по запросу приложения (0.1.157, ABI 1.19).

Защита памяти (PMS) включена, снят только её замок: прошивка открывает исполнение в области данных
внутреннего ОЗУ (IRAM0_3) лишь для приложения, позвавшего core->code_ram, и закрывает при его выгрузке.
Проверки — по исходникам (железо защиты на ПК не проверить; это — на плате).
"""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def text(*parts: str) -> str:
    return ROOT.joinpath(*parts).read_text(encoding='utf-8').replace('\r\n', '\n')


def lines(name: str) -> set[str]:
    return {line.strip() for line in text(name).splitlines()}


def body(source: str, head: str) -> str:
    start = source.index(head)
    end = source.index('\n}\n', start)
    return source[start:end]


class CodeRamTest(unittest.TestCase):
    def test_protection_on_lock_off(self) -> None:
        for name in ('sdkconfig.defaults', 'sdkconfig'):
            with self.subTest(name=name):
                config = lines(name)
                self.assertIn('CONFIG_ESP_SYSTEM_MEMPROT=y', config)
                self.assertIn('# CONFIG_ESP_SYSTEM_MEMPROT_PMS_LOCK is not set', config)
                self.assertNotIn('CONFIG_ESP_SYSTEM_MEMPROT_PMS_LOCK=y', config)

    def test_api_documented(self) -> None:
        sdk = text('modules', 'sdk', 'include', 'zm_app.h')
        self.assertIn('#define ZM_APP_ABI_MINOR 19', sdk)
        self.assertIn('#define ZM_MODULE_TEXT_INTERNAL 0x01u', sdk)
        self.assertIn('int (*code_ram)(uint32_t bytes, uint32_t* granted);', sdk)
        self.assertIn('int (*module_open_ex)(const char* name, uint32_t flags,', sdk)
        # новые поля — в конце таблицы ядра: старые приложения видят прежние смещения
        core = sdk[sdk.index('struct zm_core {'):]
        core = core[:core.index('\n};')]
        self.assertTrue(core.rstrip().endswith('int* entry_result, uint32_t* placed);'))

    def test_exec_opened_and_closed(self) -> None:
        app = text('src', 'apps', 'module_app.cpp')
        grant = body(app, 'int ModuleApp::coreCodeRam(')
        self.assertIn('setDataExec(true)', grant)
        self.assertIn('esp_ptr_in_diram_dram(data + size - 1)', grant, 'кусок — только в SRAM1 с зеркалом')
        self.assertIn('size + kCodeRamKeep', grant, 'запас внутреннего ОЗУ ядру')
        release = body(app, 'void ModuleApp::releaseCodeRam(')
        self.assertIn('setDataExec(false)', release)
        stop = app[app.index('void ModuleApp::stopApp('):]
        stop = stop[:stop.index('\n}\n')]
        self.assertLess(stop.index('unloadElf(module.elf)'), stop.index('releaseCodeRam();'),
                        'сначала снять код модулей, потом закрыть исполнение')
        self.assertIn('MEMPROT_PMS_AREA_IRAM0_3', app)
        self.assertNotIn('MEMPROT_PMS_AREA_DRAM0_0', app, 'запрет записи в код прошивки не трогаем')

    def test_reserve_made_first_and_used_first(self) -> None:
        # 0.1.158: к старту приложения SRAM1 раздроблена (Metal Slug: 13568 байт не нашлось при 37527
        # свободных) — резерв делается при старте прошивки, до всего остального.
        main = text('src', 'main.cpp')
        setup = main[main.index('void setup() {'):]
        setup = setup[:setup.index('\n}\n')]
        self.assertLess(setup.index('ModuleApp::reserveCodeRam();'), setup.index('application.begin();'))
        app = text('src', 'apps', 'module_app.cpp')
        self.assertIn('constexpr uint32_t kCodeRamReserveCap = MALLOC_CAP_PID7;', app)
        reserve = body(app, 'void ModuleApp::reserveCodeRam(')
        self.assertIn('heap_caps_malloc(kCodeRamReserve, kCodeRamSram1Caps)', reserve)
        self.assertIn('inSram1(block, kCodeRamReserve)', reserve)
        self.assertIn('const uint32_t caps[3] = {0, 0, kCodeRamReserveCaps};', reserve,
                      'резерв — только в последнем столбце приоритета: память остаётся общей')
        grant = body(app, 'int ModuleApp::coreCodeRam(')
        self.assertLess(grant.index('heap_caps_malloc(size, kCodeRamReserveCap)'),
                        grant.index('heap_caps_malloc(size, kCodeRamSram1Caps)'))
        sdk = text('modules', 'sdk', 'include', 'zm_app.h')
        self.assertIn('#define ZM_CODE_RAM_RESERVED (16u * 1024u)', sdk)
        # резерв вмещает ZM_CODE_RAM_RESERVED: 17408 − 20 − TLSF(36 + 4·10 + 4·10·16) − 8 ≥ 16384
        self.assertIn('constexpr uint32_t kCodeRamReserve = 17 * 1024;', app)
        self.assertGreaterEqual(17 * 1024 - 20 - (36 + 4 * 10 + 4 * 10 * 16) - 8, 16 * 1024)

    def test_eight_module_slots(self) -> None:
        # 0.1.159: Metal Slug открывает 4 больших модуля и MSCODE4 (code_ram) — на 0.1.158 пятому
        # не нашлось места (ZM_BUSY).
        header = text('include', 'zifi', 'apps', 'module_app.hpp')
        self.assertIn('static constexpr size_t kMaxModules = 8;', header)
        self.assertIn('LoadedModule* modules_ = nullptr;', header, 'места — в PSRAM, не в статической RAM')
        app = text('src', 'apps', 'module_app.cpp')
        self.assertIn('heap_caps_calloc(kMaxModules, sizeof(LoadedModule), MALLOC_CAP_SPIRAM', app)
        self.assertNotIn(': modules_)', app)
        sdk = text('modules', 'sdk', 'include', 'zm_app.h')
        self.assertIn('Открытых модулей у приложения сразу — до 8', sdk)

    def test_elf_record_in_psram(self) -> None:
        # 0.1.160: esp_elf_t с одним MALLOC_CAP_8BIT ложилась во внутреннее ОЗУ.
        app = text('src', 'apps', 'module_app.cpp')
        self.assertIn('heap_caps_calloc(1, sizeof(esp_elf_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)', app)
        self.assertNotIn('heap_caps_calloc(1, sizeof(esp_elf_t), MALLOC_CAP_8BIT)', app)

    def test_loader_keeps_borrowed_text(self) -> None:
        loader = text('components', 'elf_loader', 'src', 'esp_elf.c')
        self.assertIn('elf->text_room_size', loader)
        self.assertRegex(loader, re.compile(r'if \(!elf->text_borrowed\)[^\n]*\n\s*esp_elf_free\(elf->ptext\)'))
        adapter = text('components', 'elf_loader', 'src', 'esp_elf_adapter.c')
        self.assertIn('elf->text_bus_off ? elf->text_bus_off : OFFSET_TEXT_VALUE', adapter)

    def test_crash_record_maps_internal_code(self) -> None:
        crash = text('src', 'crash_record.cpp')
        self.assertIn('kDiramBusOffset = 0x40378000 - 0x3FC88000', crash)
        self.assertIn('codeToData(record.pc)', crash)


if __name__ == '__main__':
    unittest.main()
