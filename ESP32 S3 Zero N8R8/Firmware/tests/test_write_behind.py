"""Накопитель записи тома USB Flash (0.1.149, 19.11.2026).

Пользователь: «Файл копируется медленно». WC пишет файл по сектору на команду, и ESP 0.1.148
отдавал флешке каждый сектор своей командой USB. Теперь секторы подряд копятся в
zifi::WriteBehind (include/zifi/write_behind.hpp) и уходят кусками по 4 КБ:
- стенд tests/write_behind_host.cpp (MSVC) — порядок и содержимое записи, размеры кусков, отказы;
- исходный текст UsbSource — накопитель подключён, выталкивается перед чтением этого файла и
  при закрытии, удаление файла его забывает, отказ без вызывающего отдаётся следующей записи,
  большой файл пишет строку скорости в журнал.
Без Visual Studio Build Tools стенд пропускается.
"""
import pathlib
import subprocess
import unittest

from test_weather_parse import ROOT, build_exe
from test_wifi_power_save import function_body

BUILD = ROOT / '.test-build' / 'write_behind'


def read(*parts: str) -> str:
    return ROOT.joinpath(*parts).read_text(encoding='utf-8')


class WriteBehindHostTest(unittest.TestCase):
    def test_host_bench(self) -> None:
        exe = build_exe(BUILD, 'write_behind_host.exe', [ROOT / 'include'],
                        [(ROOT / 'tests' / 'write_behind_host.cpp', 'host.obj')])
        if exe is None:
            raise unittest.SkipTest('нет MSVC (vcvars64.bat): host-тест пропущен')
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(result.stdout.startswith('PASS '), result.stdout)


class UsbSourceWriteTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.sources = read('src', 'vdisk_sources.cpp')
        cls.header = read('include', 'zifi', 'vdisk_sources.hpp')

    def test_buffer_is_transfer_sized_in_psram(self) -> None:
        body = function_body(self.sources, 'bool UsbSource::write(')
        self.assertIn('heap_caps_malloc(UsbDrive::kTransferBytes, MALLOC_CAP_SPIRAM', body)
        self.assertIn('pending_.attach(buffer, UsbDrive::kTransferBytes)', body)
        self.assertIn('pending_.put(offset, data, n, sink, this)', body)
        self.assertNotIn('::write(fd_', body, 'сектор не должен идти на флешку мимо накопителя')

    def test_read_of_open_file_drains_first(self) -> None:
        body = function_body(self.sources, 'bool UsbSource::read(')
        drain = body.index('pending_.drain(sink, this)')
        self.assertLess(drain, body.index('fopen('))

    def test_close_drains_and_logs_speed(self) -> None:
        body = function_body(self.sources, 'bool UsbSource::closeOpen()')
        self.assertLess(body.index('pending_.drain(sink, this)'), body.index('close(fd_)'))
        self.assertIn('kSpeedLogBytes', body)
        self.assertIn('failed_ = true', body)

    def test_remove_forgets_pending(self) -> None:
        body = function_body(self.sources, 'bool UsbSource::remove(')
        self.assertLess(body.index('pending_.drop()'), body.index('closeOpen()'))

    def test_deferred_failure_reaches_next_write(self) -> None:
        body = function_body(self.sources, 'bool UsbSource::write(')
        self.assertLess(body.index('if (failed_)'), body.index('full(path'))

    def test_every_operation_on_open_file_closes_it(self) -> None:
        for name in ('create', 'resize', 'rename', 'setTime'):
            with self.subTest(name=name):
                self.assertIn('closeOpen()', function_body(self.sources, f'bool UsbSource::{name}('))


class UsbWriterTaskTest(unittest.TestCase):
    """0.1.150: куски WriteBehind пишет на флешку задача usb-write через кольцо в PSRAM."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.sources = read('src', 'vdisk_sources.cpp')
        cls.header = read('include', 'zifi', 'vdisk_sources.hpp')

    def test_sink_queues_into_ring(self) -> None:
        sink = function_body(self.sources, 'bool UsbSource::sink(')
        self.assertIn('->queue(offset, data, n)', sink)
        queue = function_body(self.sources, 'bool UsbSource::queue(')
        self.assertIn('return writeNow(offset, data, n)', queue, 'без задачи — сразу на флешку')
        self.assertIn('++ringFull_', queue)
        self.assertLess(queue.index('memcpy(ring_'), queue.index('xQueueSend('))

    def test_ring_and_slots_in_psram(self) -> None:
        start = function_body(self.sources, 'bool UsbSource::startWriter()')
        self.assertIn('kSlots * (UsbDrive::kTransferBytes + sizeof(Slot))', start)
        self.assertIn('MALLOC_CAP_SPIRAM', start)
        self.assertIn('"usb-write"', start)
        self.assertIn('Slot* slots_', self.header)

    def test_writer_reports_failure_and_frees_slot(self) -> None:
        task = function_body(self.sources, 'void UsbSource::writerTask(')
        self.assertIn('asyncFailed_ = true', task)
        self.assertLess(task.index('writeNow('), task.index('xSemaphoreGive('))

    def test_close_and_read_wait_for_ring(self) -> None:
        close = function_body(self.sources, 'bool UsbSource::closeOpen()')
        self.assertLess(close.index('pending_.drain(sink, this)'), close.index('sync()'))
        self.assertLess(close.index('sync()'), close.index('close(fd_)'))
        self.assertIn('ring full', close)
        read_ = function_body(self.sources, 'bool UsbSource::read(')
        self.assertLess(read_.index('sync()'), read_.index('fopen('))
        write = function_body(self.sources, 'bool UsbSource::write(')
        self.assertLess(write.index('if (asyncFailed_)'), write.index('full(path'))

    def test_state_is_locked(self) -> None:
        for name in ('write', 'create', 'resize', 'remove', 'setTime'):
            with self.subTest(name=name):
                self.assertIn('Hold hold(lock_)', function_body(self.sources, f'bool UsbSource::{name}('))
        self.assertIn('Hold hold(lock_)', function_body(self.sources, 'bool UsbSource::closeOpen()'))


class VdiskSeriesTest(unittest.TestCase):
    """0.1.150: серия WRITE — кадр [lba][флаги][сектор], ответ только на WRITE_LAST."""

    def test_series_flag_and_single_reply(self) -> None:
        header = read('include', 'zifi', 'vdisk_service.hpp')
        self.assertIn('constexpr uint8_t kFlagWriteN = 0x80;', header)
        service = read('src', 'vdisk_service.cpp')
        self.assertIn('kFlagReadN | kFlagWriteN', service)
        body = service[service.index('case kVdiskWrite: {'):service.index('case kVdiskMode: {')]
        self.assertIn('packet.length == 5 + vdisk::kSectorSize', body)
        self.assertIn('packet.length == 4 + vdisk::kSectorSize', body, 'прежний кадр — как раньше')
        self.assertLess(body.index('(flags & kWriteLast) == 0) break;'), body.index('transport.send('))
        self.assertIn('s.seriesStatus != kOk', body, 'после отказа серия не пишется')


if __name__ == '__main__':
    unittest.main()
