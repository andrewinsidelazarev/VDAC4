"""Host-тесты правил обмена по SPI платы VDAC4 (src/spi_link_core.cpp).

Тот же C++-код, что работает в прошивке, собирается MSVC вместе с
tests/spi_link_host.cpp и получает запросы Z80 по одному. Его ответы
сверяются с эталоном на Python (Network/tests/spi_link_model.py, класс LinkCore) —
тем самым, на котором проверяется драйвер Z80 в test_spi_link_z80.py. Так обе
стороны канала проверены одним описанием правил. Без Visual Studio Build Tools
тесты пропускаются.
"""
import pathlib
import random
import subprocess
import sys
import unittest

# Эталон канала лежит в тестах сетевой части, рядом с test_spi_link_z80.py.
sys.path.append(str(pathlib.Path(__file__).resolve().parents[2] / 'Network' / 'tests'))
from spi_link_model import (BULK_ATTEMPTS, BULK_ECHOED, BULK_FREE, BULK_LOADED, BULK_OFF,
                            BULK_SIZE, FLAG_BULK, STATUS_BULK, STATUS_BULK_STALE,
                            STATUS_ECHO_BAD, FLAG_PUT, FLAG_PUT_DONE, STATUS_PUT_READY,
                            STATUS_PUT_OK, STATUS_PUT_BAD, STATUS_PUT_CAPABLE,
                            CHUNK, FLAG_DROP, FLAG_SESSION, HEADER, MAGIC, REQUEST, RESPONSE,
                            STATE_EMPTY, STATE_PENDING, STATUS_MORE, TURN_SIZE, VERSION,
                            LinkCore, checksum, request)
from test_weather_parse import ROOT, build_exe

BUILD = ROOT / '.test-build' / 'spi_link'


class Host:
    """Одна запущенная программа — все команды через её stdin."""

    def __init__(self, exe: pathlib.Path) -> None:
        self.process = subprocess.Popen([str(exe)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, text=True)

    def ask(self, line: str) -> str:
        assert self.process.stdin and self.process.stdout
        self.process.stdin.write(line + '\n')
        self.process.stdin.flush()
        return self.process.stdout.readline().strip()

    def close(self) -> None:
        if self.process.stdin:
            self.process.stdin.close()
        self.process.wait(timeout=10)


class Core:
    """Ядро обмена прошивки с теми же методами, что у эталона LinkCore."""

    def __init__(self, host: Host, from_capacity: int = 4096, to_capacity: int = 4096) -> None:
        self.host = host
        assert host.ask(f'begin {from_capacity} {to_capacity}') == 'ok'

    def handle_request(self, data: bytes, bulk: int = BULK_OFF, *, put_available: bool = False,
                       put_received: bytes | None = None) -> bytes | None:
        assert self.host.ask(f'bulk {bulk}') == 'ok'
        assert self.host.ask(f'putavail {int(put_available)}') == 'ok'
        hex_data = put_received.hex() if put_received else '-'
        assert self.host.ask(f'putdata {hex_data}') == 'ok'
        answer = self.host.ask('request ' + data.hex())
        return None if answer == 'none' else bytes.fromhex(answer)

    @property
    def bulk_data(self) -> bytes:
        return bytes.fromhex(self.host.ask('bulkdata'))

    def write(self, data: bytes) -> int:
        return int(self.host.ask('write ' + data.hex()))

    def read(self, limit: int = 8192) -> bytes:
        answer = self.host.ask(f'read {limit}')
        return b'' if answer == '-' else bytes.fromhex(answer)

    def state_word(self, nonce: int = 0) -> bytes:
        return bytes.fromhex(self.host.ask(f'state {nonce}'))

    def stats(self) -> dict[str, int]:
        names = ('turns', 'repeats', 'bad', 'short', 'from', 'to', 'pending', 'space',
                 'blocks', 'bad_blocks', 'gave_up', 'puts', 'bad_puts')
        return dict(zip(names, map(int, self.host.ask('stats').split())))


_host_exe: pathlib.Path | None | bool = False


def host_exe() -> pathlib.Path:
    """Собрать программу один раз на все классы тестов этого файла."""
    global _host_exe
    if _host_exe is False:
        _host_exe = build_exe(BUILD, 'spi_link_host.exe', [ROOT / 'include'],
                              [(ROOT / 'src' / 'spi_link_core.cpp', 'core.obj'),
                               (ROOT / 'src' / 'spsc_ring.cpp', 'ring.obj'),
                               (ROOT / 'tests' / 'spi_link_host.cpp', 'host.obj')])
    if _host_exe is None:
        raise unittest.SkipTest('нет MSVC (vcvars64.bat): host-тест пропущен')
    return _host_exe


class SpiLinkCoreTest(unittest.TestCase):
    host: Host

    @classmethod
    def setUpClass(cls) -> None:
        cls.host = Host(host_exe())

    @classmethod
    def tearDownClass(cls) -> None:
        cls.host.close()

    def core(self, **capacities: int) -> Core:
        return Core(self.host, **capacities)

    def parse(self, response: bytes) -> tuple[int, int, bytes, int]:
        """Ответ -> (номер, статус, данные, принято); формат и сумма проверены."""
        self.assertEqual(response[0], RESPONSE)
        length = response[3]
        self.assertEqual(len(response), HEADER + length + 1)
        self.assertEqual(sum(response) & 0xFF, 0xFF, 'сумма сообщения с контрольным байтом — #FF')
        return response[1], response[2], response[HEADER:HEADER + length], response[4]

    def test_constants_match_the_model(self) -> None:
        core = self.core()
        self.assertEqual(core.state_word(0x42), bytes([STATE_EMPTY, MAGIC, VERSION, 0x42]))
        core.write(b'x')
        self.assertEqual(core.state_word(7), bytes([STATE_PENDING, MAGIC, VERSION, 7]))
        self.assertEqual(TURN_SIZE, 60)
        self.assertEqual(CHUNK, TURN_SIZE - HEADER - 1)

    def test_payload_goes_both_ways_in_one_turn(self) -> None:
        core = self.core()
        core.write(b'to the spectrum')
        tid, status, data, accepted = self.parse(
            core.handle_request(request(1, b'to the esp', room=CHUNK)))
        self.assertEqual((tid, status, data, accepted), (1, 0, b'to the spectrum', 10))
        self.assertEqual(core.read(), b'to the esp')
        self.assertEqual(core.state_word()[0], STATE_EMPTY)

    def test_room_limits_reply_and_more_flag_tells_the_rest(self) -> None:
        core = self.core()
        payload = bytes(range(100))
        core.write(payload)
        received = b''
        tid = 0
        for room in (10, 0, CHUNK, 200, CHUNK):
            tid += 1
            _, status, data, _ = self.parse(core.handle_request(request(tid, room=room)))
            self.assertLessEqual(len(data), min(room, CHUNK))
            received += data
            self.assertEqual(status, STATUS_MORE if len(received) < len(payload) else 0)
        self.assertEqual(received, payload)

    def test_repeated_request_returns_the_same_reply_once(self) -> None:
        core = self.core()
        core.write(b'answer')
        first = core.handle_request(request(9, b'question', room=CHUNK))
        # Z80 не дождалась ответа и прислала запрос снова.
        second = core.handle_request(request(9, b'question', room=CHUNK))
        self.assertEqual(first, second)
        self.assertEqual(core.read(), b'question', 'данные повторного запроса не удваиваются')
        self.assertEqual(self.parse(core.handle_request(request(10, room=CHUNK)))[2], b'',
                         'и ответ второй раз не отдаётся')
        stats = core.stats()
        self.assertEqual((stats['turns'], stats['repeats']), (2, 1))

    def test_session_request_is_never_a_repeat(self) -> None:
        core = self.core()
        core.write(b'stale')
        core.handle_request(request(5, room=0))
        # Новая программа случайно начала с того же номера.
        _, status, data, _ = self.parse(
            core.handle_request(request(5, room=CHUNK, flags=FLAG_SESSION | FLAG_DROP)))
        self.assertEqual((status, data), (0, b''), 'остаток прежнего сеанса выброшен')
        self.assertEqual(core.stats()['repeats'], 0)

    def test_drop_flag_clears_only_data_for_z80(self) -> None:
        core = self.core()
        core.write(b'old reply')
        _, _, data, accepted = self.parse(
            core.handle_request(request(1, b'new command', room=CHUNK, flags=FLAG_DROP)))
        self.assertEqual((data, accepted), (b'', 11))
        self.assertEqual(core.read(), b'new command')

    def test_full_queue_takes_part_and_reports_it(self) -> None:
        core = self.core(from_capacity=64, to_capacity=64)
        payload = bytes(range(CHUNK))
        self.assertEqual(self.parse(core.handle_request(request(1, payload)))[3], CHUNK)
        _, _, _, accepted = self.parse(core.handle_request(request(2, payload)))
        self.assertEqual(accepted, 64 - CHUNK, 'взято столько, сколько поместилось')
        self.assertEqual(self.parse(core.handle_request(request(3, payload)))[3], 0)
        self.assertEqual(core.read(), payload + payload[:64 - CHUNK])
        self.assertEqual(core.stats()['short'], 2)

    def test_damaged_or_foreign_buffer_is_not_a_request(self) -> None:
        core = self.core()
        good = request(1, b'data', room=CHUNK)
        for name, data in (
                ('нули после сброса', bytes(TURN_SIZE)),
                ('собственный ответ', bytes([RESPONSE]) + good[1:]),
                ('неверная сумма', good[:HEADER + 4] + bytes([good[HEADER + 4] ^ 1])),
                ('испорченный байт', good[:6] + bytes([good[6] ^ 0x80]) + good[7:]),
                ('длина больше области', bytes([REQUEST, 1, 0, CHUNK + 1, 0]) + bytes(55))):
            with self.subTest(name=name):
                self.assertIsNone(core.handle_request(data.ljust(TURN_SIZE, b'\0')))
        self.assertEqual(core.read(), b'')
        self.assertEqual(core.stats()['bad'], 3, 'нули и чужой признак запросом не считаются')
        self.assertIsNotNone(core.handle_request(good))

    def test_block_goes_through_dma_when_there_is_enough(self) -> None:
        core = self.core()
        data = bytes(index * 7 & 0xFF for index in range(BULK_SIZE + 100))
        core.write(data)
        # Z80 блок принять не может (флага нет) — данные идут в ответе.
        _, status, part, _ = self.parse(core.handle_request(request(1, room=CHUNK), BULK_FREE))
        self.assertEqual((status, part), (STATUS_MORE, data[:CHUNK]))
        core.write(data[:CHUNK])        # в очереди снова хватает на блок
        rest = data[CHUNK:] + data[:CHUNK]
        tid, status, part, _ = self.parse(
            core.handle_request(request(2, room=CHUNK, flags=FLAG_BULK), BULK_FREE))
        self.assertEqual((tid, status, part), (2, STATUS_MORE | STATUS_BULK, b''))
        self.assertEqual(core.bulk_data, rest[:BULK_SIZE])
        # Блок вернулся без искажений; остаток меньше блока уходит обычным ответом.
        _, status, part, _ = self.parse(
            core.handle_request(request(3, room=CHUNK, flags=FLAG_BULK), BULK_ECHOED))
        self.assertEqual((status, part), (STATUS_MORE, rest[BULK_SIZE:BULK_SIZE + CHUNK]))

    def test_queue_waits_behind_an_unconfirmed_block(self) -> None:
        core = self.core()
        data = bytes(index * 11 & 0xFF for index in range(BULK_SIZE + 200))
        core.write(data)
        _, status, _, _ = self.parse(
            core.handle_request(request(1, flags=FLAG_BULK), BULK_FREE))
        self.assertEqual(status, STATUS_MORE | STATUS_BULK, 'блок предлагается и без места под данные')
        # Блок прочитан, но ещё не подтверждён: то, что стоит за ним, не отдаётся
        # даже программе, которая про блоки не спрашивает.
        _, status, part, _ = self.parse(core.handle_request(request(2, room=CHUNK), BULK_FREE))
        self.assertEqual((status, part), (STATUS_MORE, b''))
        _, status, part, _ = self.parse(core.handle_request(request(3, room=CHUNK), BULK_ECHOED))
        self.assertEqual(part, data[BULK_SIZE:BULK_SIZE + CHUNK])
        self.assertEqual(core.stats()['bad_blocks'], 0)

    def test_block_that_came_back_wrong_is_offered_again(self) -> None:
        core = self.core()
        data = bytes(index * 13 & 0xFF for index in range(3 * BULK_SIZE))
        core.write(data)
        _, status, _, _ = self.parse(
            core.handle_request(request(1, flags=FLAG_BULK), BULK_FREE))
        self.assertEqual(status, STATUS_MORE | STATUS_BULK)
        # Обратная посылка не совпала (или не пришла): тот же блок ещё раз.
        _, status, part, _ = self.parse(
            core.handle_request(request(2, flags=FLAG_BULK), BULK_FREE))
        self.assertEqual((status, part), (STATUS_MORE | STATUS_BULK | STATUS_ECHO_BAD, b''))
        self.assertEqual(core.bulk_data, data[:BULK_SIZE])
        # Повторно выставленный блок Z80 ещё не прочитала.
        _, status, _, _ = self.parse(
            core.handle_request(request(3, flags=FLAG_BULK), BULK_LOADED))
        self.assertEqual(status, STATUS_MORE | STATUS_BULK)
        self.assertEqual(core.bulk_data, data[:BULK_SIZE])
        # Со второго раза вернулся верно: следующий блок — новый.
        _, status, _, _ = self.parse(
            core.handle_request(request(4, flags=FLAG_BULK), BULK_ECHOED))
        self.assertEqual(status, STATUS_MORE | STATUS_BULK)
        self.assertEqual(core.bulk_data, data[BULK_SIZE:2 * BULK_SIZE])
        stats = core.stats()
        self.assertEqual((stats['blocks'], stats['bad_blocks'], stats['gave_up']), (2, 1, 0))

    def test_dma_is_given_up_after_repeated_failures(self) -> None:
        core = self.core()
        data = bytes(index * 17 & 0xFF for index in range(2 * BULK_SIZE + 10))
        core.write(data)
        tid = 1
        self.parse(core.handle_request(request(tid, flags=FLAG_BULK), BULK_FREE))
        for attempt in range(1, BULK_ATTEMPTS):
            tid += 1
            _, status, _, _ = self.parse(
                core.handle_request(request(tid, flags=FLAG_BULK), BULK_FREE))
            self.assertEqual(status, STATUS_MORE | STATUS_BULK | STATUS_ECHO_BAD, attempt)
        # Последняя неудача: блоков больше не будет, копию выбросить.
        tid += 1
        _, status, part, _ = self.parse(
            core.handle_request(request(tid, flags=FLAG_BULK), BULK_FREE))
        self.assertEqual((status, part), (STATUS_MORE | STATUS_ECHO_BAD, b''))
        self.assertEqual(core.state_word()[0], 0xA5)
        # Те же байты и всё, что за ними, приходят обычными ответами, по порядку.
        received = b''
        while len(received) < len(data):
            tid += 1
            _, status, part, _ = self.parse(
                core.handle_request(request(tid, room=CHUNK, flags=FLAG_BULK), BULK_FREE))
            self.assertFalse(status & (STATUS_BULK | STATUS_ECHO_BAD))
            self.assertTrue(part)
            received += part
        self.assertEqual(received, data)
        stats = core.stats()
        self.assertEqual((stats['blocks'], stats['bad_blocks'], stats['gave_up']),
                         (1, BULK_ATTEMPTS, 1))
        # Новый сеанс пробует DMA заново.
        core.write(data)
        _, status, _, _ = self.parse(core.handle_request(
            request(200, flags=FLAG_BULK | FLAG_SESSION), BULK_FREE))
        self.assertEqual(status, STATUS_MORE | STATUS_BULK)

    def test_new_session_drops_the_unconfirmed_block(self) -> None:
        core = self.core()
        core.write(bytes(BULK_SIZE))
        self.parse(core.handle_request(request(1, flags=FLAG_BULK), BULK_FREE))
        fresh = bytes(index & 0xFF for index in range(BULK_SIZE))
        _, status, _, _ = self.parse(core.handle_request(
            request(1, flags=FLAG_SESSION | FLAG_DROP), BULK_FREE))
        self.assertEqual(status, 0)
        core.write(fresh)
        _, status, _, _ = self.parse(
            core.handle_request(request(2, flags=FLAG_BULK), BULK_FREE))
        self.assertEqual(status, STATUS_MORE | STATUS_BULK, 'это новый блок, а не повтор прежнего')
        self.assertEqual(core.bulk_data, fresh)
        self.assertEqual(core.stats()['bad_blocks'], 0)

    def test_block_is_not_offered_without_dma(self) -> None:
        core = self.core()
        data = bytes(range(256)) * 3
        core.write(data)
        _, status, part, _ = self.parse(
            core.handle_request(request(1, room=CHUNK, flags=FLAG_BULK), BULK_OFF))
        self.assertEqual((status, part), (STATUS_MORE, data[:CHUNK]))

    def test_unread_block_is_offered_as_stale(self) -> None:
        # Программу Z80 перезапустили, а блок остался выставленным: его нужно
        # прочитать и выбросить, новые данные при этом из очереди не берутся.
        core = self.core()
        data = bytes(range(256)) * 3
        core.write(data)
        _, status, part, _ = self.parse(core.handle_request(
            request(9, room=CHUNK, flags=FLAG_BULK | FLAG_SESSION), BULK_LOADED))
        self.assertEqual((status, part), (STATUS_MORE | STATUS_BULK | STATUS_BULK_STALE, b''))
        # Дальше — как обычно, с начала очереди.
        _, status, _, _ = self.parse(
            core.handle_request(request(10, room=CHUNK, flags=FLAG_BULK), BULK_FREE))
        self.assertEqual(status, STATUS_MORE | STATUS_BULK)
        self.assertEqual(core.bulk_data, data[:BULK_SIZE])

    def test_repeated_block_request_returns_the_same_block(self) -> None:
        core = self.core()
        data = bytes(index * 3 & 0xFF for index in range(2 * BULK_SIZE))
        core.write(data)
        first = core.handle_request(request(5, room=CHUNK, flags=FLAG_BULK), BULK_FREE)
        again = core.handle_request(request(5, room=CHUNK, flags=FLAG_BULK), BULK_LOADED)
        self.assertEqual(first, again)
        self.assertEqual(core.bulk_data, data[:BULK_SIZE], 'блок при повторе не меняется')
        self.assertEqual(core.stats()['repeats'], 1)

    @staticmethod
    def xor(data: bytes) -> int:
        value = 0
        for byte in data:
            value ^= byte
        return value

    def test_block_from_z80_goes_in_before_the_next_data(self) -> None:
        core = self.core()
        block = bytes(index * 5 & 0xFF for index in range(BULK_SIZE))
        # Отдаю блок: байт 4 — его XOR, данных в ответ не будет.
        answer = core.handle_request(request(1, b'ab', room=self.xor(block), flags=FLAG_PUT),
                                     put_available=True)
        _, status, part, accepted = self.parse(answer)
        self.assertEqual((status, part, accepted),
                         (STATUS_PUT_CAPABLE | STATUS_PUT_READY, b'', 2))
        # Блок записан; данные этого запроса встают за ним.
        _, status, _, _ = self.parse(core.handle_request(
            request(2, b'cd', flags=FLAG_PUT_DONE), put_available=True, put_received=block + bytes(4)))
        self.assertEqual(status, STATUS_PUT_CAPABLE | STATUS_PUT_OK)
        self.assertEqual(core.read(), b'ab' + block + b'cd')
        self.assertEqual((core.stats()['puts'], core.stats()['bad_puts']), (1, 0))

    def test_block_that_does_not_match_its_xor_is_dropped(self) -> None:
        core = self.core()
        block = bytes(range(256)) * 2
        core.handle_request(request(1, room=self.xor(block) ^ 1, flags=FLAG_PUT), put_available=True)
        _, status, _, _ = self.parse(core.handle_request(
            request(2, flags=FLAG_PUT_DONE), put_available=True, put_received=block))
        self.assertEqual(status, STATUS_PUT_CAPABLE | STATUS_PUT_BAD)
        # Не пришёл совсем или пришёл короче — тоже «не сошёлся».
        core.handle_request(request(3, room=self.xor(block), flags=FLAG_PUT), put_available=True)
        _, status, _, _ = self.parse(core.handle_request(
            request(4, flags=FLAG_PUT_DONE), put_available=True, put_received=block[:100]))
        self.assertEqual(status & STATUS_PUT_BAD, STATUS_PUT_BAD)
        # Без «готова» записанное не принимается.
        _, status, _, _ = self.parse(core.handle_request(
            request(5, flags=FLAG_PUT_DONE), put_available=True, put_received=block))
        self.assertEqual(status & STATUS_PUT_BAD, STATUS_PUT_BAD)
        self.assertEqual(core.read(), b'')
        self.assertEqual(core.stats()['bad_puts'], 3)

    def test_no_room_or_no_dma_means_not_ready(self) -> None:
        core = self.core(from_capacity=1024)
        # В очереди от Z80 меньше 512 свободных: «готова» нет.
        for tid in range(1, 12):
            core.handle_request(request(tid, bytes(54)), put_available=True)
        _, status, _, _ = self.parse(core.handle_request(
            request(20, room=0, flags=FLAG_PUT), put_available=True))
        self.assertEqual(status, STATUS_PUT_CAPABLE)
        core.read()
        _, status, _, _ = self.parse(core.handle_request(
            request(21, room=0, flags=FLAG_PUT), put_available=False))
        self.assertEqual(status, 0, 'без DMA прошивка о блоках молчит')

    def test_repeated_put_request_is_answered_the_same(self) -> None:
        core = self.core()
        block = bytes(BULK_SIZE)
        first = core.handle_request(request(7, room=0, flags=FLAG_PUT), put_available=True)
        again = core.handle_request(request(7, room=0, flags=FLAG_PUT), put_available=True)
        self.assertEqual(first, again)
        done = core.handle_request(request(8, flags=FLAG_PUT_DONE), put_available=True,
                                   put_received=block)
        repeat = core.handle_request(request(8, flags=FLAG_PUT_DONE), put_available=True,
                                     put_received=block)
        self.assertEqual(done, repeat)
        self.assertEqual(len(core.read()), BULK_SIZE, 'повтор не кладёт блок второй раз')

    def test_new_session_forgets_an_armed_block(self) -> None:
        core = self.core()
        block = bytes(BULK_SIZE)
        core.handle_request(request(1, room=0, flags=FLAG_PUT), put_available=True)
        core.handle_request(request(9, flags=FLAG_SESSION | FLAG_DROP), put_available=True)
        _, status, _, _ = self.parse(core.handle_request(
            request(10, flags=FLAG_PUT_DONE), put_available=True, put_received=block))
        self.assertEqual(status & STATUS_PUT_BAD, STATUS_PUT_BAD)
        self.assertEqual(core.read(), b'')

    def test_queue_to_z80_refuses_what_does_not_fit(self) -> None:
        core = self.core(to_capacity=64)
        self.assertEqual(core.write(bytes(100)), 64)
        self.assertEqual(core.write(b'x'), 0)
        self.assertEqual(core.stats()['space'], 0)

    def test_random_traffic_matches_the_model(self) -> None:
        rng = random.Random(20261001)
        put_block = bytes(rng.randrange(256) for _ in range(BULK_SIZE))
        put_xor = 0
        for value in put_block:
            put_xor ^= value
        for capacity in (64, 256, 4096):
            core = self.core(from_capacity=capacity, to_capacity=capacity)
            model = LinkCore(capacity, capacity)
            tid = 0
            for step in range(4000):
                action = rng.randrange(10)
                with self.subTest(capacity=capacity, step=step, action=action):
                    if action < 5:
                        repeat = tid and rng.randrange(8) == 0
                        if not repeat:
                            tid = (tid + rng.choice((1, 1, 1, 3))) & 0xFF
                        flags = rng.choice((0, 0, 0, 0, FLAG_DROP, FLAG_SESSION,
                                            FLAG_SESSION | FLAG_DROP))
                        flags |= rng.choice((0, FLAG_BULK, FLAG_BULK))
                        flags |= rng.choice((0, 0, FLAG_PUT, FLAG_PUT_DONE,
                                             FLAG_PUT | FLAG_PUT_DONE))
                        bulk = rng.choice((BULK_OFF, BULK_FREE, BULK_FREE, BULK_LOADED,
                                           BULK_ECHOED, BULK_ECHOED, BULK_ECHOED))
                        put_available = rng.randrange(4) != 0
                        received = rng.choice((None, put_block, put_block[:300],
                                               bytes(BULK_SIZE)))
                        data = request(tid, rng.randbytes(rng.choice((0, 1, 7, CHUNK))),
                                       room=rng.choice((0, 1, CHUNK, 255)), flags=flags)
                        if rng.randrange(20) == 0:
                            damaged = bytearray(data)
                            damaged[rng.randrange(HEADER + data[3] + 1)] ^= 1 << rng.randrange(8)
                            data = bytes(damaged)
                        if flags & FLAG_PUT and data[3] <= CHUNK and rng.randrange(2):
                            damaged_room = bytearray(data)
                            damaged_room[4] = put_xor
                            damaged_room[HEADER + data[3]] = 0
                            body = bytes(damaged_room[:HEADER + data[3]])
                            data = (body + bytes([(~sum(body)) & 0xFF])).ljust(TURN_SIZE, b'\0')
                        answer = model.handle_request(data, bulk, put_available=put_available,
                                                      put_received=received)
                        self.assertEqual(core.handle_request(data, bulk, put_available=put_available,
                                                             put_received=received), answer)
                        if answer and answer[2] & STATUS_BULK and not answer[2] & STATUS_BULK_STALE:
                            self.assertEqual(core.bulk_data, model.bulk_data)
                    elif action < 8:
                        data = rng.randbytes(rng.choice((1, 5, 60, 300, 700)))
                        self.assertEqual(core.write(data), model.write(data))
                    else:
                        limit = rng.choice((1, 16, 5000))
                        self.assertEqual(core.read(limit), model.read(limit))
                    self.assertEqual(core.state_word(), model.state_word())
            stats = core.stats()
            self.assertEqual((stats['turns'], stats['repeats'], stats['bad'], stats['short']),
                             (model.turns, model.repeats, model.bad_requests,
                              model.short_accepts))
            self.assertEqual((stats['blocks'], stats['bad_blocks'], stats['gave_up']),
                             (model.bulk_blocks, model.bulk_bad, model.bulk_gave_ups))
            self.assertEqual((stats['puts'], stats['bad_puts']), (model.put_blocks, model.put_bad))
            if capacity >= 1024:
                self.assertTrue(model.put_blocks and model.put_bad, 'случайный обмен прошёл ветви приёма')
            if capacity >= BULK_SIZE:
                self.assertTrue(model.bulk_blocks and model.bulk_bad and model.bulk_gave_ups,
                                'случайный обмен прошёл все ветви блока')


class StatusLedTest(unittest.TestCase):
    """Выбор цвета индикатора модуля (include/zifi/status_led.hpp)."""

    OFF, IDLE, WHITE, YELLOW, BLUE = (0, 0, 0), (4, 4, 4), (16, 16, 16), (24, 24, 0), (0, 0, 32)
    HOLD, FAST, SLOW = 1000, 100, 400
    host: Host

    @classmethod
    def setUpClass(cls) -> None:
        cls.host = Host(host_exe())

    @classmethod
    def tearDownClass(cls) -> None:
        cls.host.close()

    def setUp(self) -> None:
        self.assertEqual(self.host.ask('led_reset'), 'ok')

    def led(self, now: int, *, handled: int = 0, rejected: int = 0, reads: int = 0,
            clocks: int = 0, selects: int = 0, ready: bool = True) -> tuple:
        answer = self.host.ask(f'led {now & 0xFFFFFFFF} {int(ready)} {handled} {rejected} '
                               f'{reads} {clocks} {selects}')
        return tuple(map(int, answer.split()))

    def blink(self, now: int, half_period: int, colour: tuple) -> tuple:
        return colour if now // half_period % 2 == 0 else self.OFF

    def test_red_equals_green_in_every_colour(self) -> None:
        # У светодиодов модуля порядок красного и зелёного бывает разным.
        for colour in (self.OFF, self.IDLE, self.WHITE, self.YELLOW, self.BLUE):
            self.assertEqual(colour[0], colour[1])
        self.assertEqual(len({self.OFF, self.IDLE, self.WHITE, self.YELLOW, self.BLUE}), 5)

    def test_idle_until_the_counters_move(self) -> None:
        # Счётчики на момент запуска индикатора событием не считаются.
        start = dict(handled=55, rejected=7, reads=3, clocks=900, selects=40)
        self.assertEqual(self.led(1000, **start), self.IDLE)
        self.assertEqual(self.led(1020, **start), self.IDLE)

    def test_each_depth_has_its_own_sign(self) -> None:
        # От одной живой линии до полностью работающего обмена.
        cases = (
            ('только SCK', dict(clocks=8),
             lambda now: self.blink(now, self.SLOW, self.YELLOW)),
            ('только CS', dict(selects=1),
             lambda now: self.blink(now, self.SLOW, self.WHITE)),
            ('CS и SCK', dict(selects=1, clocks=8),
             lambda now: self.blink(now, self.FAST, self.YELLOW)),
            ('команды чтения', dict(selects=1, clocks=8, reads=1),
             lambda now: self.YELLOW),
            ('верные запросы', dict(selects=1, clocks=8, reads=1, handled=1),
             lambda now: self.BLUE),
            ('запись не по правилам', dict(selects=1, clocks=8, reads=1, handled=1, rejected=1),
             lambda now: self.blink(now, self.FAST, self.BLUE)),
        )
        for name, counters, expected in cases:
            with self.subTest(name=name):
                self.assertEqual(self.host.ask('led_reset'), 'ok')
                self.led(1000)
                seen = set()
                for now in range(1020, 1020 + self.HOLD, 20):
                    colour = self.led(now, **counters)
                    self.assertEqual(colour, expected(now), f'в момент {now}')
                    seen.add(colour)
                self.assertNotIn(self.IDLE, seen)
                self.assertEqual(self.led(1020 + self.HOLD, **counters), self.IDLE,
                                 'через секунду после события признак снят')

    def test_clocks_without_select_are_not_a_transfer_to_the_esp(self) -> None:
        # SCK и MOSI у ESP общие с SD-картой: такты без CS — это чужой обмен.
        self.led(1000)
        clocks = 0
        for now in range(1020, 1020 + 3 * self.HOLD, 20):
            clocks += 4096
            self.assertEqual(self.led(now, clocks=clocks),
                             self.blink(now, self.SLOW, self.YELLOW), f'в момент {now}')
        # Появился CS: теперь это обмен с ESP, в котором она ничего не разобрала.
        now = 1020 + 3 * self.HOLD
        self.assertEqual(self.led(now, clocks=clocks + 8, selects=1),
                         self.blink(now, self.FAST, self.YELLOW))

    def alternate(self, now: int) -> tuple:
        return self.YELLOW if now // self.SLOW % 2 == 0 else self.BLUE

    def test_line_sync_on_sck_shows_a_misplaced_socket(self) -> None:
        # Гнездо платы надето на X2 со сдвигом: на линии SCK строчная
        # синхронизация Evo — ровная частота без пауз, на линии CS бит видео.
        for rate, selects_per_ms in ((31250, 300), (15625, 0)):
            with self.subTest(rate=rate):
                self.assertEqual(self.host.ask('led_reset'), 'ok')
                self.led(1000)
                # Двадцать замеров по 100 мс: до этого — обычные признаки.
                for now in range(1020, 3000, 20):
                    colour = self.led(now, clocks=rate * (now - 1000) // 1000,
                                      selects=selects_per_ms * now)
                    self.assertIn(colour, (self.YELLOW, self.OFF), f'в момент {now}')
                seen = set()
                for now in range(3000, 5000, 20):
                    colour = self.led(now, clocks=rate * (now - 1000) // 1000,
                                      selects=selects_per_ms * now)
                    self.assertEqual(colour, self.alternate(now), f'в момент {now}')
                    seen.add(colour)
                self.assertEqual(seen, {self.YELLOW, self.BLUE})
                # Плату пересадили: такты пропали, признак снят за один замер.
                still = dict(clocks=rate * 4000 // 1000, selects=selects_per_ms * 4980)
                for now in range(5000, 5100, 20):
                    self.led(now, **still)
                for now in range(5100, 6000, 20):
                    self.assertIn(self.led(now, **still), (self.YELLOW, self.OFF),
                                  f'в момент {now}')
                self.assertEqual(self.led(6100, **still), self.IDLE)

    def test_line_sync_is_found_with_an_uneven_poll(self) -> None:
        # Задачу индикатора вытесняют: опросы идут неровно, а частота та же.
        rng = random.Random(7)
        now = 1000
        self.led(now)
        while now < 3600:
            now += rng.randint(17, 31)
            colour = self.led(now, clocks=31250 * (now - 1000) // 1000)
        self.assertEqual(colour, self.alternate(now))

    def test_spi_traffic_is_not_line_sync(self) -> None:
        cases = (
            # Столько же тактов в секунду, но пачкой.
            ('пачка раз в секунду', lambda now: 31250 * ((now - 1000) // 1000), {}),
            # Чтение карты: секторы по 512 байтов, в среднем те же 30 тысяч тактов.
            ('секторы SD-карты', lambda now: 4096 * ((now - 1000) // 140), {}),
            # Частота рядом со строчной, но не она.
            ('на 5 % выше строчной', lambda now: 32812 * (now - 1000) // 1000, {}),
            ('вдвое ниже строчной', lambda now: 7812 * (now - 1000) // 1000, {}),
            # Ровный обмен с ESP на той же частоте: команды разбираются.
            ('обмен с ESP', lambda now: 31250 * (now - 1000) // 1000,
             dict(reads=lambda now: now // 20)),
        )
        for name, clocks, extra in cases:
            with self.subTest(name=name):
                self.assertEqual(self.host.ask('led_reset'), 'ok')
                self.led(1000)
                for now in range(1020, 6000, 20):
                    counters = {key: value(now) for key, value in extra.items()}
                    colour = self.led(now, clocks=clocks(now), **counters)
                    self.assertNotEqual(colour, self.BLUE, f'в момент {now}')

    def test_falls_back_to_the_shallower_sign(self) -> None:
        # Опрос (чтения) идёт всё время, запрос был один — в начале.
        self.led(1000)
        reads = 0
        for now in range(1020, 1020 + self.HOLD, 20):
            reads += 1
            self.assertEqual(self.led(now, reads=reads, handled=1), self.BLUE)
        for now in range(1020 + self.HOLD, 2500, 20):
            reads += 1
            self.assertEqual(self.led(now, reads=reads, handled=1), self.YELLOW)
        self.assertEqual(self.led(2480 + self.HOLD, reads=reads, handled=1), self.IDLE)

    def test_rejected_write_is_shown_over_good_traffic(self) -> None:
        self.led(1000)
        self.assertEqual(self.led(1100, handled=1), self.BLUE)
        # Сбой среди верных запросов: синий начинает мигать.
        self.assertEqual(self.led(1200, handled=2, rejected=1), self.BLUE)
        self.assertEqual(self.led(1300, handled=3, rejected=1), self.OFF)
        self.assertEqual(self.led(1200 + self.HOLD - 100, handled=8, rejected=1), self.OFF)
        # Секунда после сбоя прошла, запросы продолжаются: снова ровный синий.
        self.assertEqual(self.led(1200 + self.HOLD, handled=9, rejected=1), self.BLUE)
        self.assertEqual(self.led(1300 + self.HOLD, handled=10, rejected=1), self.BLUE)

    def test_link_failure_blinks_whatever_the_counters_say(self) -> None:
        seen = [self.led(now, handled=now, rejected=now, reads=now, clocks=now, selects=now,
                         ready=False)
                for now in range(0, 1000, 20)]
        self.assertEqual(seen[:5], [self.WHITE] * 5)
        self.assertEqual(seen[5:10], [self.OFF] * 5)
        self.assertEqual(set(seen), {self.WHITE, self.OFF})

    def test_millisecond_counter_overflow(self) -> None:
        start = 0xFFFFFE00
        self.led(start)
        self.assertEqual(self.led(start + 20, handled=1), self.BLUE)
        self.assertEqual(self.led(start + 20 + self.HOLD - 1, handled=1), self.BLUE,
                         'счётчик миллисекунд перешёл через ноль')
        self.assertEqual(self.led(start + 20 + self.HOLD, handled=1), self.IDLE)

    def test_old_event_does_not_come_back_after_49_days(self) -> None:
        self.led(1000)
        self.assertEqual(self.led(1020, handled=1, selects=1), self.BLUE)
        self.assertEqual(self.led(1020 + self.HOLD, handled=1, selects=1), self.IDLE)
        # Счётчик миллисекунд сделал полный круг и снова рядом со временем событий.
        self.assertEqual(self.led((1 << 32) + 1030, handled=1, selects=1), self.IDLE)
        self.assertEqual(self.led((1 << 32) + 1500, handled=1, selects=1), self.IDLE)


class ChecksumTest(unittest.TestCase):
    def test_all_zero_and_all_ones_are_not_messages(self) -> None:
        # Пустой буфер после сброса ESP и «висящая» линия не должны сойти за ответ.
        for filler in (0x00, 0xFF):
            body = bytes([filler]) * (HEADER + 1)
            self.assertNotEqual(body[-1], checksum(body[:-1]))


if __name__ == '__main__':
    unittest.main()
