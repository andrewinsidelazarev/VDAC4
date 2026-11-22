"""Задачи приложений со стеком в PSRAM (0.1.152, 19.11.2026).

Задача приложения со стеком в PSRAM (ZM_TASK_STACK_PSRAM), кончившись, прежде удаляла себя через
vTaskDeleteWithCaps(nullptr): ESP-IDF для этого запускает задачу очистки во внутренней памяти, а
её при плеере нет — abort() (вероятный сбой PLAYER 1.34 при радио: свободно ~2 КБ, кусок 704 байта).
Теперь кончившаяся задача встаёт (vTaskSuspend), а снимает её ядро 1 (reapTasks из pollCore1 и
task_start) — внутренняя память на это не нужна. ABI minor 17: по нему PLAYER 1.48 берёт стек
поиска обложки из сети в PSRAM. Проверяется исходный текст: прошивка на ПК не собирается.
"""
import pathlib
import re
import unittest

from test_wifi_power_save import function_body

ROOT = pathlib.Path(__file__).resolve().parents[1]


def read(*parts: str) -> str:
    return ROOT.joinpath(*parts).read_text(encoding='utf-8')


class AppTasksTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = read('src', 'apps', 'module_app.cpp')
        cls.header = read('include', 'zifi', 'apps', 'module_app.hpp')
        cls.sdk = read('modules', 'sdk', 'include', 'zm_app.h')

    def test_psram_task_does_not_delete_itself(self) -> None:
        body = function_body(self.app, 'void ModuleApp::taskTrampoline(')
        body = re.sub(r'//.*', '', body)                      # комментарии не в счёт
        self.assertNotIn('vTaskDeleteWithCaps(nullptr)', body, 'задача очистки ESP-IDF — во внутренней памяти')
        self.assertIn('slot->finished = true', body)
        self.assertLess(body.index('slot->finished = true'), body.index('vTaskSuspend(nullptr)'))

    def test_core1_reaps_finished_tasks(self) -> None:
        reap = function_body(self.app, 'void ModuleApp::reapTasks()')
        self.assertIn('eTaskGetState', reap, 'только вставшую')
        self.assertIn('vTaskDeleteWithCaps(handle)', reap)
        self.assertIn('slot.running = true', reap, 'место занято, пока снимаем')
        self.assertIn('reapTasks();', function_body(self.app, 'void ModuleApp::pollCore1('))
        start = function_body(self.app, 'int ModuleApp::coreTaskStart(')
        self.assertLess(start.index('reapTasks()'), start.index('for (TaskSlot& slot'))
        self.assertIn('!slot.running && !slot.finished', start, 'кончившаяся, но не снятая — не занимать')
        self.assertIn('g_taskSlotLock', start)

    def test_stop_removes_finished_tasks_too(self) -> None:
        stop = function_body(self.app, 'void ModuleApp::stopApp()')
        self.assertIn('(slot.running || slot.finished) && slot.handle != nullptr', stop)

    def test_psram_first_skips_internal(self) -> None:
        start = function_body(self.app, 'int ModuleApp::coreTaskStart(')
        self.assertIn('ZM_TASK_STACK_PSRAM_FIRST', start)
        self.assertIn('if (!psramFirst) allocateAfterCleanup(', start, 'во внутренней не пробовать')
        self.assertIn('#define ZM_TASK_STACK_PSRAM_FIRST 0x40000000u', self.sdk)

    def test_abi_minor_17(self) -> None:
        self.assertRegex(self.sdk, r'#define ZM_APP_ABI_MINOR (1[789]|[2-9]\d)\b')
        self.assertIn('17: задачу со стеком в PSRAM', self.sdk)
        self.assertIn('volatile bool finished;', self.header)


if __name__ == '__main__':
    unittest.main()
