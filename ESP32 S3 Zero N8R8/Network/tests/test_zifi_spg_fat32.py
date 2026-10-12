"""zifi.spg: сохранение загрузок на SD через FAT32-Driver-ZX-Evolution.

Исполняется собранный Z80-код ZiFi для VDAC4 (ZiFi SPG/build: zifi.bin и zifi_win.bin) вместе
с собранным драйвером и его портом SD-ZC (fat32.bin, fat32-work.bin) на модели
SD-карты из репозитория драйвера (tests/harness.py, tests/test_sdzc.py). Подменены
только отрисовка интерфейса, окно ошибки и часы Evo (CMOS). Том после каждого
сценария проверяется независимым разбором (fsck ниже): обход всех каталогов, порядок
и полнота записей LFN, «.» и «..» в каждом подкаталоге, цепочки, перекрёстные
ссылки, потерянные кластеры, размеры, зеркала FAT и FSInfo.

Драйвер ищется в FAT32_DRIVER_DIR, по умолчанию — там же, где его ищет
ZiFi SPG/build.bat. Перед тестами — свежая сборка ZiFi SPG/build.bat. Нужен
пакет z80 (requirements-test.txt драйвера).
"""
from datetime import datetime, timedelta
import os
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPG = ROOT / "ZiFi SPG"
BUILD = SPG / "build"
DRIVER = Path(os.environ.get("FAT32_DRIVER_DIR", ROOT.parent / "FAT32 Driver"))
sys.path.insert(0, str(DRIVER / "tests"))
try:
    import z80
    from harness import Disk, Driver, symbols
    from test_sdzc import Card, ZController
except ImportError as error:  # нет драйвера или пакета z80
    raise unittest.SkipTest(f"FAT32 driver tests unavailable: {error}")

# Стек — как в главном цикле ZiFi (SP=#BFFF, сохранение вызывается из него); вектор
# прерываний IM2 лежит в #BEFF..#BF00, и стек не должен до него дорастать.
STOP, STACK = 0xBE10, 0xBFFD
VECTOR = 0xBEFF
FAT32_APPEND = 0x4000 + 76 * 3
STATUS_EXISTS, STATUS_READ_ONLY, STATUS_FAT = 0x1A, 0x18, 0x20
# После отказа носителя или обрыва питания посреди операции FAT32 без журнала
# допускает только то, что chkdsk чинит без потери данных: потерянные кластеры,
# устаревший FSInfo и отставшую вторую копию FAT (сбой между записями копий;
# цепочки берутся из активной, первой).
AFTER_FAULT = ("lost", "FSInfo free", "FAT mirror")
NOW = datetime(2026, 10, 7, 14, 35, 27)


def fat_stamp(when):
    """Время и дата FAT, сотые доли секунды создания."""
    return ((when.hour << 11) | (when.minute << 5) | (when.second // 2),
            ((when.year - 1980) << 9) | (when.month << 5) | when.day,
            100 if when.second & 1 else 0)


def stamps(entry):
    raw = entry["raw"]
    return dict(tenth=raw[13], ctime=struct.unpack_from("<H", raw, 14)[0],
                cdate=struct.unpack_from("<H", raw, 16)[0], adate=struct.unpack_from("<H", raw, 18)[0],
                wtime=struct.unpack_from("<H", raw, 22)[0], wdate=struct.unpack_from("<H", raw, 24)[0])


def lfn_checksum(short):
    checksum = 0
    for x in short:
        checksum = (((checksum & 1) << 7) + (checksum >> 1) + x) & 255
    return checksum


def raw_directory_problems(disk, directory, path):
    """Сырые записи каталога: «.» и «..» первыми в подкаталоге; LFN — с флагом последней
    записи, номерами подряд до 1, одной суммой, равной сумме следующего короткого имени,
    нулями в поле типа и кластера; висящих LFN нет."""
    problems = []
    raw = b"".join(disk.cluster_data(c) for c in disk.chain(directory))
    entries = [raw[i:i + 32] for i in range(0, len(raw), 32)]
    if directory != 2:
        for index, name in ((0, b".          "), (1, b"..         ")):
            if index >= len(entries) or entries[index][:11] != name or not entries[index][11] & 0x10:
                problems.append(("dot entry missing", path, name.decode().strip()))
    expect, checksum = None, None
    for e in entries:
        if e[0] == 0:
            break
        if e[0] == 0xE5:
            if expect is not None:
                problems.append(("LFN orphan", path))
            expect = None
            continue
        if e[11] & 0x3F == 0x0F:
            order = e[0] & 0x1F
            if e[12] != 0 or e[26:28] != b"\0\0" or not 1 <= order <= 20:
                problems.append(("LFN fields", path, e[0]))
            if e[0] & 0x40:
                if expect is not None:
                    problems.append(("LFN orphan", path))
                expect, checksum = order - 1, e[13]
            elif expect is None or order != expect or e[13] != checksum:
                problems.append(("LFN sequence", path, e[0]))
                expect = None
            else:
                expect -= 1
            continue
        if expect is not None:
            if expect != 0 or checksum != lfn_checksum(e[:11]):
                problems.append(("LFN incomplete", path, e[:11]))
            expect = None
    if expect is not None:
        problems.append(("LFN orphan", path))
    return problems


def fsck(disk):
    """Независимая проверка тома. Список неисправностей; пусто — том цел."""
    problems, owner = [], {}
    cluster_bytes = disk.spc * 512

    def claim(chain, path):
        for cluster in chain:
            if cluster in owner:
                problems.append(("cross-link", owner[cluster], path, cluster))
            else:
                owner[cluster] = path

    def walk(directory, path, parent):
        try:
            problems.extend(raw_directory_problems(disk, directory, path))
            entries = disk.entries(directory)
        except (AssertionError, KeyError, UnicodeDecodeError) as error:
            problems.append(("directory", path, str(error)))
            return
        names = set()
        for e in entries:
            name = e["name"]
            if name in (".", ".."):
                want = directory if name == "." else (0 if parent == 2 else parent)
                if directory == 2 or e["cluster"] != want or not e["attr"] & 0x10:
                    problems.append(("dot entry", path, name, e["cluster"], want))
                continue
            if e["attr"] & 0x08:
                continue
            full = f"{path}/{name}"
            if name.casefold() in names:
                problems.append(("duplicate", full))
            names.add(name.casefold())
            try:
                chain = disk.chain(e["cluster"]) if e["cluster"] else []
            except AssertionError as error:
                problems.append(("chain", full, str(error)))
                continue
            claim(chain, full)
            if e["attr"] & 0x10:
                if not chain:
                    problems.append(("directory without cluster", full))
                else:
                    walk(e["cluster"], full, directory)
            elif len(chain) != (e["size"] + cluster_bytes - 1) // cluster_bytes:
                problems.append(("size", full, e["size"], len(chain)))

    claim(disk.chain(2), "/")
    walk(2, "", 0)
    free = 0
    for cluster in range(2, disk.clusters + 2):
        value = disk.fat(cluster)
        if value == 0:
            free += 1
        elif cluster not in owner:
            problems.append(("lost", cluster))
    for copy in range(1, disk.fats):
        for i in range(disk.fat_sectors):
            if disk.read(disk.start + disk.reserved + i) != disk.read(disk.start + disk.reserved + copy * disk.fat_sectors + i):
                problems.append(("FAT mirror", copy, i))
                break
    info = disk.read(disk.start + 1)
    count = struct.unpack_from("<I", info, 488)[0]
    if count != 0xFFFFFFFF and count != free:
        problems.append(("FSInfo free", count, free))
    return problems


class Rtc:
    """Часы Evo (CMOS, как DS12887): #EFF7 бит 7 — доступ, #DFF7 — регистр, #BFF7 — данные."""

    def __init__(self, when=NOW, binary=False, hour12=False):
        self.when, self.binary, self.hour12 = when, binary, hour12
        self.enabled, self.address, self.reads = False, 0, 0
        self.raw, self.on_read, self.written = None, None, {}

    def value(self, reg):
        if self.raw is not None and reg in self.raw:
            return self.raw[reg]
        if reg == 0x0B:
            return (0x04 if self.binary else 0) | (0 if self.hour12 else 0x02)
        w = self.when
        v = {0: w.second, 2: w.minute, 4: w.hour, 7: w.day, 8: w.month, 9: w.year % 100}.get(reg)
        if v is None:
            return self.written.get(reg, 0)
        pm = 0
        if reg == 4 and self.hour12:                 # 1..12 и бит 7 — PM
            pm = 0x80 if v >= 12 else 0
            v = v % 12 or 12
        return pm | (v if self.binary else (v // 10) << 4 | v % 10)

    def output(self, port, value):
        if port == 0xEFF7:
            self.enabled = bool(value & 0x80)
        elif port == 0xDFF7:
            self.address = value
        else:
            assert self.enabled
            self.written[self.address] = value

    def input(self, port):
        assert self.enabled, "CMOS read without #EFF7 bit 7"
        self.reads += 1
        if self.on_read:
            self.on_read(self)
        return self.value(self.address)


class Zifi:
    """Страницы, порты и вызовы ZiFi; интерфейс подменён, остальное — собранный код."""

    def __init__(self, disk=None, rtc=None):
        self.disk = disk or Disk(spc=8)
        self.card = Card(self.disk)
        self.bus = ZController(self.card)
        self.rtc = rtc or Rtc()
        self.cut = False
        self.cpu = z80.Z80Machine()
        self.ram = self.cpu.memory
        self.sym = symbols(BUILD / "zifi.sym")
        self.pages = [bytearray(16384) for _ in range(256)]
        self.mapping = [0, 31, 2, 0]
        for page, path in [(2, BUILD / "zifi.bin"), (0x11, BUILD / "zifi_win.bin"),
                           (14, SPG / "build" / "fat32.bin"), (15, SPG / "build" / "fat32-work.bin")]:
            data = path.read_bytes()
            self.pages[page][:len(data)] = data
        for window, page in enumerate(self.mapping):
            self.cpu.set_memory_block(window * 16384, self.pages[page])
        for window in (0, 1, 3):
            self.ram[self.sym[f"restore_page{window}"] + 1] = self.mapping[window]
        self.cpu.set_output_callback(self.output)
        self.cpu.set_input_callback(self.input)
        self.logs, self.errors = [], []
        self.music_ticks = 0
        # Прерывание стенда: после irq_after байтов, переданных в SPI внутри APPEND, оно
        # «приходит» и подаётся, только когда IFF1=1 — как у настоящего Z80.
        self.irq_after, self.irq_outs, self.irq_pending, self.irq_taken = None, 0, False, None
        self.in_append, self.min_sp = False, 0xFFFF
        self.ui = {self.sym[x]: x for x in ("set_ports", "zifi_log", "win_error_call")}
        for addr in [STOP, FAT32_APPEND, *self.ui]:
            self.cpu.set_breakpoint(addr)
        assert self.call("sd_init") == (0, True, False)
        assert self.mapping == [0, 31, 2, 0]

    def output(self, port, value):
        self.min_sp = min(self.min_sp, self.cpu.sp)
        if self.irq_after is not None and self.in_append and port & 255 == 0x57 and self.irq_taken is None:
            self.irq_outs += 1
            if self.irq_outs == self.irq_after:
                self.irq_pending = True
                self.cpu.ticks_to_stop = 0
        if port in (0x10AF, 0x11AF, 0x13AF):
            window = (port >> 8) - 16
            start = window * 16384
            self.pages[self.mapping[window]][:] = self.ram[start:start + 16384]
            self.mapping[window] = value
            self.cpu.set_memory_block(start, self.pages[value])
        elif port & 255 in (0x57, 0x77):
            self.bus.output(port, value)
        elif port in (0xEFF7, 0xDFF7, 0xBFF7):
            self.rtc.output(port, value)
        elif port & 255 not in (0xFE, 0xAF):
            raise AssertionError(hex(port))

    def input(self, port):
        self.min_sp = min(self.min_sp, self.cpu.sp)
        if port == 0xBFF7:
            return self.rtc.input(port)
        return self.bus.input(port)

    def page_byte(self, page, address):
        """Байт страницы page по адресу окна #C000 (страница может быть не отображена)."""
        if self.mapping[3] == page:
            return self.ram[address]
        return self.pages[page][address - 0xC000]

    def power_cut_after(self, writes):
        """Питание пропадает после writes записанных секторов: дальше ничего не ложится."""
        count = [0]

        def write(lba, data):
            count[0] += 1
            if count[0] > writes:
                self.cut = True
                self.card.program_error = lambda lba, data: True
                return False
            return False
        self.card.fail_write = write

    def call(self, label, stop_at=None, **regs):
        stop = self.sym[stop_at] if stop_at else STOP
        if stop_at:
            self.cpu.set_breakpoint(stop)
        self.cpu.pc, self.cpu.sp = self.sym[label], STACK
        self.ram[STACK:STACK + 2] = STOP.to_bytes(2, "little")
        for key, value in regs.items():
            setattr(self.cpu, key, value)
        for _ in range(20000):
            # Ждущее прерывание подаётся, как только IFF1=1: стенд проверяет это часто.
            self.cpu.ticks_to_stop = 50 if self.irq_pending else 200_000
            event = self.cpu.run()
            if self.cut:
                return "power cut"
            if self.irq_pending and self.cpu._Z80State__iff1[0]:
                # Настоящий обработчик ZiFi сохраняет регистры и возвращает банки; как
                # у Z80, на входе IFF1=0, а разрешает его сам обработчик.
                self.irq_pending = False
                self.irq_taken = self.cpu.pc
                self.cpu._Z80State__iff1[0] = 0
                self.cpu._Z80State__iff2[0] = 0
                self.cpu.sp -= 2
                self.ram[self.cpu.sp:self.cpu.sp + 2] = self.cpu.pc.to_bytes(2, "little")
                self.cpu.pc = self.sym["int_text_off"]
                continue
            if not event & 2:
                continue
            if self.cpu.pc in (STOP, stop):
                if stop_at:
                    self.cpu.clear_breakpoint(stop)
                return self.cpu.a, bool(self.cpu.f & 64), bool(self.cpu.f & 1)
            if self.cpu.pc == FAT32_APPEND:
                self.in_append = True             # дальше — внутри дозаписи
                self.cpu.clear_breakpoint(FAT32_APPEND)
                continue
            if self.cpu.pc not in self.ui:
                continue
            kind = self.ui[self.cpu.pc]
            if kind == "music_player_play":
                self.music_ticks += 1
            elif kind == "zifi_log":
                self.logs.append(bytes(self.ram[self.cpu.hl:self.cpu.hl + 80]).split(b"\0")[0])
            elif kind == "win_error_call":
                self.errors.append(bytes(self.ram[self.cpu.hl:self.cpu.hl + 80]).split(b"\0")[0])
            self.cpu.pc = int.from_bytes(self.ram[self.cpu.sp:self.cpu.sp + 2], "little")
            self.cpu.sp += 2
        raise AssertionError(("ZiFi did not return", label, hex(self.cpu.pc)))

    def put_body(self, data, offset=0, page=0x20):
        payload = bytes(offset) + data
        for pos in range(0, len(payload), 16384):
            chunk = payload[pos:pos + 16384]
            self.pages[page + pos // 16384][:len(chunk)] = chunk
        for window, mapped in enumerate(self.mapping):
            if page <= mapped < page + (len(payload) + 16383) // 16384:
                self.cpu.set_memory_block(window * 16384, self.pages[mapped])
        thread = self.sym["read_threads"]
        struct.pack_into("<H", self.ram, thread + self.sym["thread.adress"], self.sym["get_buffer"] + offset)
        self.ram[thread + self.sym["thread.page"]] = page
        self.ram[thread + self.sym["thread.full_len"]:thread + self.sym["thread.full_len"] + 3] = len(data).to_bytes(3, "little")

    def thread(self, field, size=2):
        at = self.sym["read_threads"] + self.sym[f"thread.{field}"]
        return int.from_bytes(self.ram[at:at + size], "little")

    def save(self, name, data, offset=0, page=0x20):
        """Как готовая загрузка: имя уже в FILE_NAME, тело в страницах."""
        self.put_body(data, offset, page)
        self.cpu.set_memory_block(self.sym["FILE_NAME"], name.encode("cp866") + b"\0")
        return self.call("save_downloaded_file")

    def file_name(self, path, body=b"x", page=0x20):
        """create_filename по пути запроса; выход — FILE_NAME."""
        self.cpu.set_memory_block(self.sym["request_path"], path + b"\0")
        self.put_body(body, 0, page)
        assert self.call("create_filename", ix=self.sym["read_threads"])[0] is not None
        return bytes(self.ram[self.sym["FILE_NAME"]:self.sym["FILE_NAME"] + 49]).split(b"\0")[0]

    def download(self, path, body, page=0x20):
        """Путь главного цикла: create_filename, затем save_downloaded_file."""
        self.file_name(path, body, page)
        return self.call("save_downloaded_file", ix=self.sym["read_threads"])

    def enter_download_dir(self, date=b"2026-10-07"):
        self.cpu.set_memory_block(self.sym["DIR_date"] + 1, date + b"\0")
        return self.call("set_download_dir")


def names(disk, directory=2):
    """Имена каталога без учёта регистра: имя 8.3 драйвер пишет без LFN, заглавными."""
    return sorted(e["name"].casefold() for e in disk.entries(directory) if e["name"] not in (".", ".."))


class ZifiSaveTests(unittest.TestCase):
    def assertClean(self, disk):
        self.assertEqual(fsck(disk), [])

    def assertAfterMediaError(self, disk):
        """После отказа носителя посреди APPEND звенья FAT уже легли, а запись каталога
        нет: кластеры потеряны (их освободит chkdsk), FSInfo не обновлён. Иного — нет."""
        self.assertEqual([p for p in fsck(disk) if p[0] not in ("lost", "FSInfo free")], [])

    def test_new_file_is_complete_and_stamped_by_evo_clock(self):
        h = Zifi()
        data = bytes((i * 7 + 3) % 251 for i in range(20000))
        self.assertEqual(h.save("game.trd", data), (0, True, False))
        self.assertEqual(h.disk.contents("game.trd"), data)
        self.assertEqual(names(h.disk), ["game.trd"])
        time, date, tenth = fat_stamp(NOW)
        self.assertEqual(stamps(h.disk.get("game.trd")),
                         dict(tenth=tenth, ctime=time, cdate=date, adate=date, wtime=time, wdate=date))
        self.assertFalse(h.rtc.enabled)            # доступ к часам закрыт
        self.assertEqual(h.mapping, [0, 31, 2, 0])
        self.assertEqual(h.ram[h.sym["fat_active"]], 0)
        self.assertFalse(h.errors)
        self.assertClean(h.disk)

    def test_sizes_and_replace_keep_one_entry(self):
        h = Zifi()
        for size, offset in [(0, 0), (1, 0), (4096, 0), (16385, 37), (640 * 1024, 0)]:
            with self.subTest(size=size, offset=offset):
                data = bytes((i * 31 + size) % 251 for i in range(size))
                name = f"data{size}.bin"
                h.rtc.when = NOW
                self.assertEqual(h.save(name, data, offset), (0, True, False))
                self.assertEqual(h.disk.contents(name), data)
                later = NOW + timedelta(minutes=3, seconds=10)
                h.rtc.when = later
                self.assertEqual(h.save(name, b"replace"), (0, True, False))
                self.assertEqual(h.disk.contents(name), b"replace")
                time, date, tenth = fat_stamp(later)
                self.assertEqual(stamps(h.disk.get(name))["wtime"], time)
                self.assertEqual(h.mapping, [0, 31, 2, 0])
                self.assertFalse([n for n in names(h.disk) if n.endswith(".$$$")])
                self.assertClean(h.disk)
        self.assertFalse(h.errors)

    def test_failed_new_download_never_appears_under_its_name(self):
        h = Zifi()
        data = bytes(i % 251 for i in range(33000))
        written = []
        h.card.fail_write = lambda lba, block: (written.append(lba), len(written) > 12)[1]
        result = h.save("fresh.bin", data)
        self.assertNotEqual(result[0], 0)
        self.assertEqual(len(h.errors), 1)
        self.assertNotIn("fresh.bin", names(h.disk))
        self.assertIn("fresh.bin.$$$", names(h.disk))
        self.assertAfterMediaError(h.disk)
        # Повтор той же загрузки убирает остаток и кладёт файл целиком.
        h.card.fail_write = None
        self.assertEqual(h.save("fresh.bin", data), (0, True, False))
        self.assertEqual(h.disk.contents("fresh.bin"), data)
        self.assertEqual(names(h.disk), ["fresh.bin"])
        self.assertAfterMediaError(h.disk)

    def test_failed_replace_keeps_previous_file(self):
        h = Zifi()
        first = bytes(i % 251 for i in range(20000))
        self.assertEqual(h.save("keep.bin", first), (0, True, False))
        second = bytes((i * 7) % 251 for i in range(33000))
        written = []
        h.card.fail_write = lambda lba, block: (written.append(lba), len(written) > 40)[1]
        self.assertNotEqual(h.save("keep.bin", second)[0], 0)
        h.card.fail_write = None
        self.assertEqual(h.disk.contents("keep.bin"), first)
        self.assertEqual(h.ram[h.sym["fat_active"]], 0)
        self.assertAfterMediaError(h.disk)
        self.assertEqual(h.save("keep.bin", second), (0, True, False))
        self.assertEqual(h.disk.contents("keep.bin"), second)
        self.assertEqual(names(h.disk), ["keep.bin"])
        self.assertAfterMediaError(h.disk)

    def test_tmp_sharing_the_target_chain_is_left_alone(self):
        """Обрыв посреди подмены: временный и цель на одной цепочке — DELETE запрещён."""
        h = Zifi()
        data = bytes(i % 251 for i in range(9000))
        self.assertEqual(h.save("shared.bin", data), (0, True, False))
        seed = Driver(h.disk)
        self.assertEqual(seed.create("shared.bin.$$$"), (0, True, False))
        target = h.disk.get("shared.bin")
        for e in h.disk.entries():
            if e["name"] == "shared.bin.$$$":
                stale = e
        # Запись временного переводится на цепочку цели, как после обрыва.
        root = b"".join(h.disk.cluster_data(c) for c in h.disk.chain(2))
        at = root.index(stale["raw"])
        raw = bytearray(stale["raw"])
        raw[20:22] = target["raw"][20:22]
        raw[26:32] = target["raw"][26:32]
        lba = h.disk.data_start + at // 512
        sector = bytearray(h.disk.read(lba))
        sector[at % 512:at % 512 + 32] = raw
        h.disk.write(lba, bytes(sector))
        before = dict(h.disk.blocks)
        h = Zifi(h.disk)
        self.assertEqual(h.save("shared.bin", b"new data")[0], STATUS_FAT)
        self.assertEqual(h.disk.blocks, before)
        self.assertEqual(h.disk.contents("shared.bin"), data)

    def test_directory_or_read_only_target_is_refused_before_writing(self):
        h = Zifi()
        seed = Driver(h.disk)
        self.assertEqual(seed.mkdir("thing.bin"), (0, True, False))
        self.assertEqual(seed.create("locked.bin"), (0, True, False))
        self.assertEqual(seed.append(b"old"), (0, True, False))
        root = b"".join(h.disk.cluster_data(c) for c in h.disk.chain(2))
        locked = h.disk.get("locked.bin")
        at = root.index(locked["raw"])
        lba = h.disk.data_start + at // 512
        sector = bytearray(h.disk.read(lba))
        sector[at % 512 + 11] |= 1
        h.disk.write(lba, bytes(sector))
        h = Zifi(h.disk)
        before = dict(h.disk.blocks)
        self.assertEqual(h.save("thing.bin", b"data")[0], STATUS_EXISTS)
        self.assertEqual(h.save("locked.bin", b"data")[0], STATUS_READ_ONLY)
        self.assertEqual(h.disk.blocks, before)
        self.assertEqual(h.disk.contents("locked.bin"), b"old")

    def test_download_directory_created_once_with_time(self):
        h = Zifi()
        for _ in range(2):
            self.assertEqual(h.enter_download_dir(b"2026-10-07"), (0, True, False))
            self.assertEqual(h.mapping, [0, 31, 2, 0])
        zifi = h.disk.get("zifi")
        downloads = h.disk.get("downloads", zifi["cluster"])
        date = h.disk.get("2026-10-07", downloads["cluster"])
        time, day, tenth = fat_stamp(NOW)
        for entry in (zifi, downloads, date):
            self.assertEqual(stamps(entry)["cdate"], day)
            self.assertEqual(stamps(entry)["wtime"], time)
        before = dict(h.disk.blocks)
        h.rtc.when = NOW + timedelta(hours=1)
        self.assertEqual(h.enter_download_dir(b"2026-10-07"), (0, True, False))
        self.assertEqual(h.disk.blocks, before)     # прежние каталоги не трогаются
        self.assertEqual(h.save("in.bin", b"in directory"), (0, True, False))
        self.assertEqual(h.disk.contents("in.bin", date["cluster"]), b"in directory")
        self.assertClean(h.disk)

    def test_clock_fallbacks(self):
        cases = [
            # (часы, двоичные, сырые регистры, дата каталога, ожидаемое время)
            (datetime(2028, 2, 29, 23, 59, 58), False, None, b"2026-10-07", datetime(2028, 2, 29, 23, 59, 58)),
            (datetime(2026, 12, 31, 9, 5, 1), True, None, b"2026-10-07", datetime(2026, 12, 31, 9, 5, 1)),
            (NOW, False, {7: 0x4A}, b"2026-09-08", datetime(2026, 9, 8)),       # день не BCD
            (datetime(2000, 1, 1, 0, 0, 5), False, None, b"2026-09-08", datetime(2026, 9, 8)),  # севшая батарейка
            (NOW, False, {8: 0x13}, b"0000-00-00", datetime(2026, 1, 1)),       # нет и даты каталога
            (NOW, False, {7: 0x31, 8: 0x04}, b"2026-09-08", datetime(2026, 1, 1)),  # 31 апреля
        ]
        for clock, binary, raw, folder, expected in cases:
            with self.subTest(clock=clock, raw=raw, folder=folder):
                h = Zifi(rtc=Rtc(clock, binary))
                h.rtc.raw = raw
                h.cpu.set_memory_block(h.sym["DIR_date"] + 1, folder + b"\0")
                self.assertEqual(h.save("clock.bin", b"t"), (0, True, False))
                time, date, tenth = fat_stamp(expected)
                got = stamps(h.disk.get("clock.bin"))
                self.assertEqual((got["wtime"], got["wdate"], got["tenth"]), (time, date, tenth))

    def test_clock_in_12_hour_mode(self):
        for binary in (False, True):
            for clock in (datetime(2026, 10, 7, 0, 30, 10), datetime(2026, 10, 7, 12, 30, 11),
                          datetime(2026, 10, 7, 13, 5, 0), datetime(2026, 10, 7, 23, 59, 58),
                          datetime(2026, 10, 7, 11, 0, 0)):
                with self.subTest(binary=binary, clock=clock):
                    h = Zifi(rtc=Rtc(clock, binary, hour12=True))
                    self.assertEqual(h.save("half.bin", b"t"), (0, True, False))
                    time, date, tenth = fat_stamp(clock)
                    got = stamps(h.disk.get("half.bin"))
                    self.assertEqual((got["wtime"], got["wdate"], got["tenth"]), (time, date, tenth))
        # Часы 0 и 13 в 12-часовом режиме — сбитые: дата каталога, 00:00.
        for raw in ({4: 0x00}, {4: 0x13}, {4: 0x80}):
            with self.subTest(raw=raw):
                h = Zifi(rtc=Rtc(NOW, hour12=True))
                h.rtc.raw = raw
                h.cpu.set_memory_block(h.sym["DIR_date"] + 1, b"2026-09-08\0")
                self.assertEqual(h.save("half.bin", b"t"), (0, True, False))
                time, date, _ = fat_stamp(datetime(2026, 9, 8))
                got = stamps(h.disk.get("half.bin"))
                self.assertEqual((got["wtime"], got["wdate"]), (time, date))

    def test_clock_step_between_reads_gives_consistent_snapshot(self):
        before, after = datetime(2026, 10, 7, 23, 59, 59), datetime(2026, 10, 8, 0, 0, 0)
        h = Zifi(rtc=Rtc(before))

        def tick(rtc):
            if rtc.reads == 4:                      # посреди первого снимка
                rtc.when = after
        h.rtc.on_read = tick
        self.assertEqual(h.save("midnight.bin", b"t"), (0, True, False))
        time, date, _ = fat_stamp(after)
        got = stamps(h.disk.get("midnight.bin"))
        self.assertEqual((got["wtime"], got["wdate"]), (time, date))

    def test_file_names_follow_driver_rules(self):
        long_stem = b"a" * 100
        cases = [
            (b"/files/game.trd", b"game.trd"),
            (b"/zxart/file/id:123/filename:Some%20Game.scl", b"Some%20Game.scl"),
            (b"/get.php?f=demo.zip", b"f=demo.zip"),
            (b'/x/a<b>c|d"e.txt', b"a_b_c_d_e.txt"),
            (b"/x/\xd0\x9f\xd1\x80\xd0\xb8.txt", b"_\x9f_\x80__.txt"),     # UTF-8 из URL; #B8 — псевдографика
            (b"/x/\x01ctl\x7f.bin", b"_ctl\x7f.bin"),
            (b"/x/name...", b"name"),
            (b"/x/...", b"download"),
            (b"/x/", b"download"),
            (b"", b"download"),
            (b"/x/two words.txt", b"two"),
            (b"/x/" + long_stem + b".trd", b"a" * 44 + b".trd"),
            (b"/x/" + long_stem, b"a" * 48),
            (b"/x/" + b"b" * 50 + b".verylongextension", b"b" * 48),
            (b"/x/" + b"." * 50 + b"tail.longextension", b"download"),
            (b"/x/" + b"c" * 47 + b".d.", b"c" * 46 + b".d"),             # 49 знаков после среза точки
            (b"/x/demo.trd.$$$", b"demo.trd_$$$"),                      # «.$$$» — только временные
            (b"/x/.$$$", b"_$$$"),
            (b"/x/$$$", b"$$$"),
        ]
        h = Zifi()
        for path, expected in cases:
            with self.subTest(path=path):
                name = h.file_name(path)
                self.assertEqual(name, expected)
                ext = h.thread("file_ext")
                dot = name.rfind(b".")
                self.assertEqual(ext - h.sym["FILE_NAME"], dot + 1 if dot >= 0 else 0)
        # Каждое такое имя сохраняется и находится под ним же.
        for path, expected in cases:
            with self.subTest(saved=path):
                body = path + b" body"
                self.assertEqual(h.download(path, body), (0, True, False))
                self.assertEqual(h.disk.contents(expected.decode("cp866")), body)
        self.assertClean(h.disk)

    def test_downloaded_dollar_name_is_not_taken_for_a_temporary(self):
        """Скачанный «demo.trd.$$$» не удаляется последующей загрузкой «demo.trd»."""
        h = Zifi()
        self.assertEqual(h.download(b"/x/demo.trd.$$$", b"first file"), (0, True, False))
        self.assertEqual(h.download(b"/x/demo.trd", b"second file"), (0, True, False))
        self.assertEqual(names(h.disk), ["demo.trd", "demo.trd_$$$"])
        self.assertEqual(h.disk.contents("demo.trd_$$$"), b"first file")
        self.assertEqual(h.disk.contents("demo.trd"), b"second file")
        self.assertClean(h.disk)

    def test_online_unzip_prefix_replaces_extension(self):
        h = Zifi()
        for path, name in [(b"/unzipremote.php?f=game.zip", b"f=game.scl"), (b"/files/game", b"game.scl")]:
            with self.subTest(path=path):
                self.assertEqual(h.file_name(path, b".sclSINCLAIR"), name)
                self.assertEqual(h.thread("adress"), h.sym["get_buffer"] + 4)
                self.assertEqual(h.thread("full_len", 3), len(b"SINCLAIR"))

    def test_load_ini_through_driver_and_restore_banks(self):
        for spc in (1, 8):
            for size in (37, 511, 512, 3000):
                with self.subTest(spc=spc, size=size):
                    disk = Disk(spc=spc)
                    seed = Driver(disk)
                    self.assertEqual(seed.mkdir("zifi"), (0, True, False))
                    seed.find("zifi", 16)
                    seed.call(31)
                    seed.create("zifi.ini")
                    data = (b"SSID:test\r\nPASSWORD:test\r\ntime:none\r\n" + b";" * size)[:size]
                    seed.append(data)
                    h = Zifi(disk)
                    h.call("load_ini", stop_at="parse_ini")
                    self.assertEqual(h.cpu.pc, h.sym["parse_ini"])
                    length = min(size, 511)
                    self.assertEqual(h.pages[0x20][:length + 1], data[:length] + b"\0")
                    self.assertEqual(h.mapping, [0, 31, 2, 0])
                    self.assertEqual(h.ram[h.sym["fat_active"]], 0)

    def test_invalid_source_range_touches_nothing(self):
        cases = [(640 * 1024 + 1, 0, 0x20), (640 * 1024, 1, 0x20),        # буфер загрузки
                 (32769, 0, 0x1D), (32768, 1, 0x1D), (1, 0, 0x1F), (1, 0, 0x1C), (1, 0, 0x48)]
        for size, offset, page in cases:
            with self.subTest(size=size, offset=offset, page=page):
                h = Zifi()
                before = dict(h.disk.blocks)
                self.assertEqual(h.save("invalid.bin", bytes(size), offset, page=page), (0xF2, False, True))
                self.assertEqual(h.disk.blocks, before)
                self.assertEqual(h.mapping, [0, 31, 2, 0])
        for size, offset in ((1, 0), (32768, 0), (32764, 4)):       # музыкальный буфер #1D..#1E
            with self.subTest(music=size, offset=offset):
                h = Zifi()
                data = bytes((i * 17 + 3) % 251 for i in range(size))
                self.assertEqual(h.save("track.pt3", data, offset, page=0x1D), (0, True, False))
                self.assertEqual(h.disk.contents("track.pt3"), data)

    def test_music_interrupt_inside_sd_transfer_restores_driver_banks(self):
        """Прерывание музыки посреди передачи сектора в APPEND (там IFF1=1; в коде
        страницы окон — DI): банки, данные и вектор IM2 целы, стек до вектора не дорос."""
        for after in (40, 600, 5000):
            with self.subTest(spi_bytes=after):
                h = Zifi()
                for label in ("music_player_play", "set_256c_mode"):
                    h.ui[h.sym[label]] = label
                    h.cpu.set_breakpoint(h.sym[label])
                h.ram[h.sym["music_sw"] + 1] = 1
                h.ram[h.sym["load_sw"] + 1] = 0
                h.pages[3][h.sym["music_setup_vars"] & 0x3FFF] = 0x80
                h.ram[VECTOR:VECTOR + 2] = h.sym["int_text_off"].to_bytes(2, "little")
                h.irq_after = after
                data = bytes(i % 251 for i in range(17000))
                self.assertEqual(h.save("music.bin", data), (0, True, False))
                self.assertIsNotNone(h.irq_taken, "прерывание не подано")
                self.assertEqual(h.music_ticks, 1)
                self.assertEqual(h.ram[h.sym["load_sw"] + 1], 0)
                self.assertEqual(h.mapping, [0, 31, 2, 0])
                # Обработчик сам переводит вектор на следующий в цепочке растровых
                # прерываний; стек его не затёр — там адрес одного из обработчиков.
                handlers = {h.sym[x] for x in ("int_main", "int_text_off", "int_text_on", "int_text_cor", "int_gfx_view")}
                self.assertIn(int.from_bytes(h.ram[VECTOR:VECTOR + 2], "little"), handlers)
                self.assertGreaterEqual(h.min_sp, VECTOR + 2)
                self.assertEqual(h.disk.contents("music.bin"), data)
                self.assertClean(h.disk)

    def test_many_downloads_in_one_directory(self):
        """Сотни сохранений и замен в каталоге даты; модель содержимого и fsck."""
        disk = Disk(spc=1, clusters=8192)
        h = Zifi(disk)
        self.assertEqual(h.enter_download_dir(), (0, True, False))
        directory = h.disk.get("2026-10-07", h.disk.get("downloads", h.disk.get("zifi")["cluster"])["cluster"])["cluster"]
        model = {}
        for n in range(240):
            name = f"file{n % 70:03d}{'.trd' if n % 3 else '.scl'}"
            size = (n * 977) % 9000
            data = bytes((i + n) % 253 for i in range(size))
            self.assertEqual(h.save(name, data), (0, True, False), name)
            model[name] = data
            if n % 40 == 39:
                self.assertEqual(sorted(model), names(h.disk, directory))
                for key, value in model.items():
                    self.assertEqual(h.disk.contents(key, directory), value, key)
                self.assertClean(h.disk)
        self.assertFalse(h.errors)

    def test_full_volume_fails_cleanly(self):
        disk = Disk(spc=1, clusters=300)
        h = Zifi(disk)
        kept = {}
        n = 0
        while True:
            data = bytes((i * 3 + n) % 251 for i in range(20000))
            result = h.save(f"fill{n}.bin", data)
            if result[0]:
                break
            kept[f"fill{n}.bin"] = data
            n += 1
        self.assertEqual(result[0], 16)              # нет места
        self.assertNotIn(f"fill{n}.bin", names(h.disk))
        for key, value in kept.items():
            self.assertEqual(h.disk.contents(key), value)
        problems = fsck(h.disk)
        self.assertEqual(problems, [])
        # Замена при нехватке места: прежний файл цел.
        first = next(iter(kept))
        self.assertNotEqual(h.save(first, bytes(60000))[0], 0)
        self.assertEqual(h.disk.contents(first), kept[first])
        self.assertEqual(fsck(h.disk), [])


class FsckSelfTests(unittest.TestCase):
    """fsck обязан видеть порчу: иначе «том цел» в остальных тестах ничего не значит."""

    def damaged(self):
        h = Zifi(Disk(spc=1, clusters=512))
        self.assertEqual(h.enter_download_dir(), (0, True, False))
        self.assertEqual(h.save("long file name.bin", bytes(3000)), (0, True, False))
        self.assertEqual(h.save("short.bin", bytes(1000)), (0, True, False))
        self.assertEqual(fsck(h.disk), [])
        downloads = h.disk.get("downloads", h.disk.get("zifi")["cluster"])["cluster"]
        date = h.disk.get("2026-10-07", downloads)["cluster"]
        return h.disk, date

    def patch(self, disk, directory, find, change):
        """Переписать первую сырую запись каталога, для которой find(e) истинно."""
        lba = disk.data_start + (directory - 2) * disk.spc
        sector = bytearray(disk.read(lba))
        for at in range(0, 512, 32):
            if find(bytes(sector[at:at + 32])):
                sector[at:at + 32] = change(bytearray(sector[at:at + 32]))
                disk.write(lba, bytes(sector))
                return
        raise AssertionError("запись не найдена")

    def test_each_damage_is_reported(self):
        def kinds(disk):
            return {p[0] for p in fsck(disk)}

        disk, date = self.damaged()
        self.patch(disk, date, lambda e: e[:11] == b"..         ", lambda e: bytes([0xE5]) + e[1:])
        self.assertIn("dot entry missing", kinds(disk))
        disk, date = self.damaged()
        self.patch(disk, date, lambda e: e[11] == 0x0F and e[0] != 0xE5 and e[0] & 0x40, lambda e: bytes([e[0] & 0x1F]) + e[1:])
        self.assertTrue(kinds(disk) & {"LFN sequence", "LFN orphan", "LFN incomplete"}, kinds(disk))
        disk, date = self.damaged()
        self.patch(disk, date, lambda e: e[11] == 0x0F and e[0] != 0xE5, lambda e: e[:13] + bytes([e[13] ^ 1]) + e[14:])
        self.assertTrue(kinds(disk) & {"LFN sequence", "LFN incomplete", "directory"}, kinds(disk))
        disk, date = self.damaged()
        short = disk.get("short.bin", date)
        self.patch(disk, date, lambda e: e[:11] == short["short"], lambda e: e[:28] + struct.pack("<I", 9000))
        self.assertIn("size", kinds(disk))
        disk, date = self.damaged()
        other = disk.get("long file name.bin", date)
        mine = disk.get("short.bin", date)["short"]
        self.patch(disk, date, lambda e: e[:11] == mine,
                   lambda e: e[:20] + other["raw"][20:22] + e[22:26] + other["raw"][26:28] + e[28:])
        self.assertTrue(kinds(disk) & {"cross-link"}, kinds(disk))
        disk, date = self.damaged()
        disk.set_fat(400, 0x0FFFFFFF)
        self.assertIn("lost", kinds(disk))


class ZifiFaultSweepTests(unittest.TestCase):
    """Отказ носителя или обрыв питания на каждой записи сектора: новый файл и замена,
    корень и каталог даты, файл в несколько кластеров по 4 КиБ."""

    OLD = bytes(i % 251 for i in range(6000))
    SCENARIOS = [  # (секторов в кластере, размер новой загрузки, каталог даты)
        (1, 9000, False),
        (1, 9000, True),
        (8, 40000, False),
    ]

    def prepared(self, spc, directory, replace):
        h = Zifi(Disk(spc=spc, clusters=2048))
        if directory:
            self.assertEqual(h.enter_download_dir(), (0, True, False))
        if replace:
            self.assertEqual(h.save("target.bin", self.OLD), (0, True, False))
        return h

    def rebooted(self, disk, directory):
        """Новый запуск ZiFi на том же томе: ОЗУ заново, тот же каталог загрузок."""
        h = Zifi(disk)
        if directory:
            self.assertEqual(h.enter_download_dir(), (0, True, False))
        return h

    def cwd(self, disk, directory):
        if not directory:
            return 2
        downloads = disk.get("downloads", disk.get("zifi")["cluster"])["cluster"]
        return disk.get("2026-10-07", downloads)["cluster"]

    def scenarios(self):
        for spc, size, directory in self.SCENARIOS:
            new = bytes((i * 13 + 5) % 251 for i in range(size))
            for replace in (False, True):
                h = self.prepared(spc, directory, replace)
                count = [0]
                h.card.fail_write = lambda lba, block: (count.__setitem__(0, count[0] + 1), False)[1]
                self.assertEqual(h.save("target.bin", new), (0, True, False))
                yield spc, directory, replace, new, count[0]

    def check_after(self, disk, directory, replace, new, allow):
        """Цель — прежняя или новая целиком (нового файла может и не быть);
        размер не сходится с цепочкой только у недописанного временного."""
        problems = fsck(disk)
        unexpected = [p for p in problems if p[0] not in allow and not (
            p[0] == "size" and p[1].casefold().endswith("/target.bin.$$$"))]
        self.assertEqual(unexpected, [], problems)
        cwd = self.cwd(disk, directory)
        if "target.bin" in names(disk, cwd):
            self.assertIn(disk.contents("target.bin", cwd), (self.OLD, new) if replace else (new,))
        else:
            self.assertFalse(replace, "прежний файл пропал")
        return problems

    def assertRetryCleans(self, h, directory, data):
        """Следующая загрузка того же файла кладёт его целиком и убирает временный;
        на томе остаётся только то, что чинит chkdsk без потери данных."""
        self.assertEqual(h.save("target.bin", data), (0, True, False))
        cwd = self.cwd(h.disk, directory)
        self.assertEqual(h.disk.contents("target.bin", cwd), data)
        self.assertNotIn("target.bin.$$$", names(h.disk, cwd))
        self.assertEqual([p for p in fsck(h.disk) if p[0] not in AFTER_FAULT], [])

    def test_media_error_on_each_write(self):
        """Отказ записи сектора: карта его не приняла (rejected) либо сектор лёг, а карта
        ответила ошибкой (landed) — исход, неизвестный драйверу."""
        for spc, directory, replace, new, total in self.scenarios():
            for k in range(total):
                for landed in (False, True):
                    with self.subTest(spc=spc, directory=directory, replace=replace, failed_write=k, landed=landed):
                        self.one_write_fault(spc, directory, replace, new, k, landed)

    def one_write_fault(self, spc, directory, replace, new, k, landed):
                    h = self.prepared(spc, directory, replace)
                    count = [0]

                    def fail(lba, block):
                        count[0] += 1
                        if count[0] != k + 1:
                            return False
                        if landed:
                            h.disk.write(lba, block)
                        return True
                    h.card.fail_write = fail
                    result = h.save("target.bin", new)
                    h.card.fail_write = None
                    self.check_after(h.disk, directory, replace, new, AFTER_FAULT)
                    if result[0] == 0:
                        self.assertEqual(h.disk.contents("target.bin", self.cwd(h.disk, directory)), new)
                    else:
                        self.assertEqual(len(h.errors), 1)
                    self.assertRetryCleans(self.rebooted(h.disk, directory), directory, new)

    def test_read_error_on_each_read(self):
        """Ошибка чтения (CRC блока) на каждом чтении сохранения: каталог даты, новый файл
        и замена, кластер 512 Б."""
        new = bytes((i * 13 + 5) % 251 for i in range(9000))
        for replace in (False, True):
            h = self.prepared(1, True, replace)
            count = [0]
            h.card.garble = lambda lba, data: (count.__setitem__(0, count[0] + 1), data)[1]
            self.assertEqual(h.save("target.bin", new), (0, True, False))
            for k in range(count[0]):
                with self.subTest(replace=replace, failed_read=k):
                    h = self.prepared(1, True, replace)
                    reads = [0]

                    def garble(lba, data):
                        reads[0] += 1
                        return bytes([data[0] ^ 0xFF]) + data[1:] if reads[0] == k + 1 else data
                    h.card.garble = garble
                    result = h.save("target.bin", new)
                    h.card.garble = None
                    self.check_after(h.disk, True, replace, new, AFTER_FAULT)
                    if result[0] == 0:
                        self.assertEqual(h.disk.contents("target.bin", self.cwd(h.disk, True)), new)
                    self.assertRetryCleans(self.rebooted(h.disk, True), True, new)

    def test_power_cut_after_each_write(self):
        for spc, directory, replace, new, total in self.scenarios():
            for k in range(total + 1):
                with self.subTest(spc=spc, directory=directory, replace=replace, writes_done=k):
                    h = self.prepared(spc, directory, replace)
                    h.power_cut_after(k)
                    h.save("target.bin", new)
                    # Обрыв в окне подмены оставляет цель и её временный на одной
                    # цепочке — иных перекрёстных ссылок не бывает.
                    problems = self.check_after(h.disk, directory, replace, new, AFTER_FAULT + ("cross-link",))
                    shared = [p for p in problems if p[0] == "cross-link"]
                    for p in shared:
                        self.assertEqual({p[1].casefold().rsplit("/", 1)[1], p[2].casefold().rsplit("/", 1)[1]},
                                         {"target.bin", "target.bin.$$$"}, problems)
                    h = self.rebooted(h.disk, directory)
                    if shared:
                        # Такой временный не удаляется: DELETE освободил бы цепочку цели.
                        before = dict(h.disk.blocks)
                        self.assertEqual(h.save("target.bin", self.OLD)[0], STATUS_FAT)
                        self.assertEqual(h.disk.blocks, before)
                        self.assertEqual(h.disk.contents("target.bin", self.cwd(h.disk, directory)), new)
                    else:
                        self.assertRetryCleans(h, directory, self.OLD)


if __name__ == "__main__":
    unittest.main()
