"""Машинные тесты запускателя ESP App SPG с файлами данных (build.bat ИМЯ.ELF … ДАННЫЕ).

Код запускателя исполняется в эмуляторе Z80 вместе с настоящим драйвером FAT32
(shared/fat32/build: fat32-work.bin в окне #0000, fat32.bin в окне #4000). Заменены только
секторный обмен с картой (таблица порта #3900 указывает на точки останова, сектор берётся из
образа карты fat32_disk.Disk) и всё, что за портами #57/#77: модель канала SPI
(spi_link_model) и «прошивка» ESP с командами модулей, MOD_QUERY с флагом 4, PATH_GET/PATH_SET.

Сборки для проверок тест делает сам — во временной папке (TEMP, переменная
ESP_APP_TEST_DIR): prep.py с --out и sjasmplus над копией esp_app.asm. Папка build
запускателя не трогается: test_esp_app_spg_z80.py по-прежнему проверяет последнюю сборку.

Проверяется: файл данных доходит до ESP байт в байт (фрагментированный файл, кластеры не по
порядку); у файлов больше 16 и 32 МБ смещения MOD_PUT 32-битные и идут по порядку; порции —
по 8 КБ одной передачей DMA (быстрый путь), без него — кадрами по 1016 байтов; ответ 0 —
передачи нет; ответы 2 и 4 — свой цвет бордюра, без APP_START и без экрана FT812; папка
берётся из PATH_GET без поиска, без записи (и с негодной записью) — поиском и PATH_SET;
порядок — ядро, ELF, файлы данных, APP_START; на карту ничего не пишется, матрица клавиатуры
ZX до запуска приложения не читается.

Прошивка 0.1.166 без флешки (EspAppSpgRemoteTest): в MOD_QUERY за именем приложения — номер файла,
ответ 5 — файл не передаётся; после APP_START «прошивка» просит куски EVT_DATA_READ, запускатель
отдаёт их CMD_DATA_PIECE байт в байт: DMA по 8 КБ и кадрами по 1016, кластеры 512 Б, 4 и 32 КБ,
фрагментированный файл, смещения за 32 МБ, кусок через границу кластера, хвост файла, запрос до
ответа на APP_START; нет файла и смещение за цепочкой — кусок без данных; раздроблен сильнее 896
кусков — красный бордюр без запуска.

Дополнительные модули ELF (build.bat ИМЯ.ELF … +МОДУЛЬ.ELF, 26.11.2026): уходят после главного ELF,
до файлов данных, байт в байт, с флагом 0; «+» и голый путь .ELF — одно и то же; годная копия у ESP —
не передаётся; ESP не взяла модуль — следующий модуль, файлы данных и запуск идут дальше, бордюр не
красный; сборка без файлов данных — так же; prep.py не берёт не-ELF, повтор имени и модуль с именем
главного ELF.

Запуск: E:\\zx\\esp\\pio-venv\\Scripts\\python.exe -m unittest test_esp_app_spg_data_z80
(пакет z80 1.2.0; sjasmplus — SJASMPLUS или E:\\zx\\z80\\zuma\\sjasmplus.exe).
"""
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import z80

import test_ftp_plugin_z80 as base
from fat32_disk import Disk, symbols
from spi_link_model import CMD_WRDMA, LinkCore
from test_spi_link_z80 import SpiLinkMixin

ROOT = base.ROOT                        # Network
SPG_SRC = ROOT / "ESP App SPG"
FAT32 = ROOT / "shared" / "fat32"
FAT32_BUILD = FAT32 / "build"
SJASM = os.environ.get("SJASMPLUS", r"E:\zx\z80\zuma\sjasmplus.exe")
WORK = Path(os.environ.get("ESP_APP_TEST_DIR", Path(tempfile.gettempdir()) / "esp_app_spg_data"))

CMD_MOD_QUERY, CMD_MOD_PUT, CMD_MOD_COMMIT, CMD_APP_START, CMD_APP_STOP = 0x69, 0x6A, 0x6B, 0x6C, 0x6D
CMD_PATH_GET, CMD_PATH_SET = 0x2A, 0x2B
VCONFIG, PAGE3, MEMCONFIG, PAGE0, PAGE1 = 0x00AF, 0x13AF, 0x21AF, 0x10AF, 0x11AF
INIT, READ, WRITE, SYNC = 0xC100, 0xC110, 0xC120, 0xC130   # «процедуры порта SD» — точки останова
SERIAL = 0x76543210                     # серийный номер тома у Disk (BPB +67)
APP = "Q2"
TIME, DATE = 0x6A2B, 0x5B77             # время и дата FAT у файлов образа
STAMP = DATE << 16 | TIME
BLUE, RED, MAGENTA, GREEN, CYAN, YELLOW, WHITE = 1, 2, 3, 4, 5, 6, 7

ELF_BYTES = b"\x7fELF" + bytes((i * 13 + 5) & 255 for i in range(30000))
CORE_BYTES = b"\xff" * 32 + b"\xa5\xc3" + bytes((i * 7 + 3) & 255 for i in range(20000))
HOT_BYTES = b"\x7fELF" + bytes((i * 29 + 1) & 255 for i in range(40000))     # три страницы SPG
SKY_BYTES = b"\x7fELF" + bytes((i * 3 + 7) & 255 for i in range(5000))


def frame(command: int, payload: bytes = b"") -> bytes:
    # Кадр ESP -> Z80 — не больше 256 байт payload: длиннее на плате теряется.
    assert len(payload) <= 256, (hex(command), len(payload))
    return base.frame(command, payload)


def run(args: list, cwd: Path | None = None, env: dict | None = None) -> None:
    result = subprocess.run([str(a) for a in args], cwd=cwd, env=env, capture_output=True)
    log = result.stdout.decode(errors="replace") + result.stderr.decode(errors="replace")
    assert result.returncode == 0, log


def build_fat32() -> None:
    """Драйвер FAT32 (shared/fat32/build.py), если его нет или исходники новее."""
    images = [FAT32_BUILD / "fat32.bin", FAT32_BUILD / "fat32-work.bin", FAT32_BUILD / "fat32.sym"]
    sources = [p for d in ("src", "include", "ports") for p in (FAT32 / d).rglob("*") if p.is_file()]
    if all(p.exists() for p in images) and \
            min(p.stat().st_mtime for p in images) >= max(p.stat().st_mtime for p in sources):
        return
    run([sys.executable, FAT32 / "build.py"], cwd=FAT32, env=dict(os.environ, SJASMPLUS=SJASM))


def build_variant(name: str, data: list[str], core: bool = False,
                  mods: tuple = ()) -> Path:
    """Сборка запускателя Q2.ELF [+ ядро] [+ модули (имя, байты, с «+»)] + файлы данных — в
    WORK/name/build."""
    folder = WORK / name
    shutil.rmtree(folder, ignore_errors=True)
    (folder / "build").mkdir(parents=True)
    elf = folder / "Q2.ELF"
    elf.write_bytes(ELF_BYTES)
    args = [sys.executable, SPG_SRC / "prep.py", elf]
    if core:
        bits = folder / "Q2CORE.BIN"
        bits.write_bytes(CORE_BYTES)
        args.append(bits)
    for mod_name, body, plus in mods:
        path = folder / mod_name
        path.write_bytes(body)
        args.append(f"+{path}" if plus else path)
    run(args + data + ["--out", folder / "build"])
    # Копия исходника: его INCLUDE "build/app_cfg.inc" ищется рядом с ним — в этой сборке.
    shutil.copyfile(SPG_SRC / "esp_app.asm", folder / "esp_app.asm")
    run([SJASM, "--nologo", f"--inc={ROOT / 'shared' / 'z80'}", f"--inc={FAT32 / 'include'}",
         f"--inc={SPG_SRC}", "--sym=build/esp_app.sym", "esp_app.asm"], cwd=folder)
    return folder


VARIANTS: dict[str, Path] = {}


def setUpModule() -> None:
    build_fat32()
    VARIANTS["data"] = build_variant("data", ["PAK0.PAK", "PAK1.PAK"])
    VARIANTS["core"] = build_variant("core", ["PAK0.PAK"], core=True)
    VARIANTS["mods"] = build_variant("mods", ["PAK0.PAK"], core=True,
                                     mods=(("Q2HOT.ELF", HOT_BYTES, True), ("Q2SKY.ELF", SKY_BYTES, False)))
    VARIANTS["mods_nodata"] = build_variant("mods_nodata", [], mods=(("Q2HOT.ELF", HOT_BYTES, True),))


# --- образ карты -----------------------------------------------------------------------

def entry(name: str, cluster: int, size: int = 0, directory: bool = False) -> bytes:
    """Запись каталога 8.3: атрибут, первый кластер (+20, +26), время и дата (+22), длина (+28)."""
    stem, _, ext = name.partition(".")
    e = bytearray(32)
    e[:11] = (stem.ljust(8) + ext.ljust(3)).encode("ascii")
    e[11] = 0x10 if directory else 0x20
    struct.pack_into("<HHHHI", e, 20, cluster >> 16, TIME, DATE, cluster & 0xFFFF, size)
    return bytes(e)


class Card:
    """SD-карта Evo: том FAT32 (Disk), каталоги и файлы с цепочками кластеров."""

    def __init__(self, spc: int = 1, clusters: int = 65536) -> None:
        self.disk = Disk(spc=spc, partition=2048, clusters=clusters)
        self.spc = spc
        self.cluster_bytes = 512 * spc
        self.next = 3                   # 2 — корень
        self.dirs: dict[int, list[bytes]] = {2: []}

    def alloc(self, count: int, fragmented: bool = False) -> list[int]:
        if not fragmented:
            chain = list(range(self.next, self.next + count))
            self.next += count
            return chain
        # через кластер, и вторая половина цепочки — раньше первой (ссылка назад)
        chain = list(range(self.next, self.next + 2 * count, 2))
        self.next += 2 * count + 1
        half = count // 2
        return chain[half:] + chain[:half]

    def link(self, chain: list[int]) -> None:
        for i, cluster in enumerate(chain):
            self.disk.set_fat(cluster, chain[i + 1] if i + 1 < len(chain) else 0x0FFFFFFF)

    def lba(self, cluster: int, sector: int = 0) -> int:
        return self.disk.data_start + (cluster - 2) * self.spc + sector

    def add(self, directory: int, raw: bytes) -> None:
        self.dirs[directory].append(raw)
        data = b"".join(self.dirs[directory]).ljust(self.cluster_bytes, b"\0")
        assert len(data) <= self.cluster_bytes, "каталог стенда — один кластер"
        for s in range(self.spc):
            self.disk.write(self.lba(directory, s), data[s * 512:(s + 1) * 512])

    def mkdir(self, parent: int, name: str) -> int:
        (cluster,) = self.alloc(1)
        self.link([cluster])
        self.dirs[cluster] = []
        self.add(cluster, b"")          # пустой каталог: нули
        self.add(parent, entry(name, cluster, directory=True))
        return cluster

    def put(self, directory: int, name: str, data: bytes, fragmented: bool = False) -> None:
        count = max(1, -(-len(data) // self.cluster_bytes))
        chain = self.alloc(count, fragmented)
        self.link(chain)
        for i, cluster in enumerate(chain):
            part = data[i * self.cluster_bytes:(i + 1) * self.cluster_bytes].ljust(self.cluster_bytes, b"\0")
            for s in range(self.spc):
                self.disk.write(self.lba(cluster, s), part[s * 512:(s + 1) * 512])
        self.add(directory, entry(name, chain[0], len(data)))

    def put_sparse(self, directory: int, name: str, size: int) -> None:
        """Большой файл: метки — в первом и последнем секторе каждого кластера, прочее — нули."""
        count = -(-size // self.cluster_bytes)
        chain = self.alloc(count)
        self.link(chain)
        for i, cluster in enumerate(chain):
            for s in (0, self.spc - 1):
                self.disk.write(self.lba(cluster, s),
                                struct.pack("<III", i, s, 0xC0DE5EC7).ljust(512, bytes([i & 255])))
        self.add(directory, entry(name, chain[0], size))

    def contents(self, name: str, directory: int) -> bytes:
        return self.disk.contents(name, directory)


PAK0 = bytes((i * 37 + (i >> 9)) & 255 for i in range(100_000))     # 12 порций и хвост
PAK1 = bytes((i * 11 + 7) & 255 for i in range(3000))


def standard_card(spc: int = 1, pak1: bool = True) -> tuple[Card, dict]:
    """/AAA (пустая), /GAMES/QUAKE2: Q2.ELF, PAK0.PAK (фрагментирован), PAK1.PAK."""
    card = Card(spc)
    where = {"AAA": card.mkdir(2, "AAA")}
    where["GAMES"] = card.mkdir(2, "GAMES")
    where["QUAKE2"] = folder = card.mkdir(where["GAMES"], "QUAKE2")
    card.put(folder, "Q2.SPG", b"SPG" * 100)
    card.put(folder, "Q2.ELF", ELF_BYTES)
    card.put(folder, "PAK0.PAK", PAK0, fragmented=True)
    if pak1:
        card.put(folder, "PAK1.PAK", PAK1)
    return card, where


# --- «прошивка» ESP ----------------------------------------------------------------------

class DataEsp:
    """Кэш модулей, файлы данных приложения (флаг 4), path.cfg, APP_START/APP_STOP."""

    def __init__(self, data_status: dict | None = None, paths: dict | None = None,
                 cached_names: tuple = (), refuse_put: tuple | None = None,
                 refuse_names: tuple = ()) -> None:
        self.data_status = data_status or {}        # имя -> ответ на MOD_QUERY флага 4 (иначе 1)
        self.paths = paths                          # (имя, серийный) -> кластер; None — нет команд
        self.cached_names = cached_names
        self.refuse_names = refuse_names            # флаг 0: эти — ответ 2 «нет места»
        self.refuse_put = refuse_put                # (имя, смещение): этот кусок не принять (EA 0)
        self.refused = False
        self.queries: list[tuple] = []              # (флаги, длина, время, имя, приложение)
        self.puts: list[tuple[str, int, int]] = []  # (имя, смещение, длина)
        self.files: dict[str, dict] = {}            # положенные: имя -> длина, sha256, байты
        self.current = None
        self.started = None
        self.stopped = False
        self.commands: list[int] = []
        self.path_gets: list[tuple] = []
        self.path_sets: list[tuple] = []

    def __call__(self, command: int, payload: bytes) -> bytes:
        self.commands.append(command)
        if command == CMD_MOD_QUERY:
            flags, length, stamp = payload[0], *struct.unpack("<II", payload[1:9])
            end = payload.index(0, 9)
            name = payload[9:end].decode()
            app = payload[end + 1:payload.index(0, end + 1)].decode() if flags & 4 else None
            self.queries.append((flags, length, stamp, name, app))
            status = self.data_status.get(name, 1) if flags & 4 else \
                (2 if name in self.refuse_names else 0 if name in self.cached_names else 1)
            self.current = None
            if status == 1:
                self.current = {"name": name, "size": length, "got": 0,
                                "sha": hashlib.sha256(), "data": bytearray() if length <= 4 << 20 else None}
            return frame(0xE9, bytes([status]) + struct.pack("<I", 1 << 30))
        if command == CMD_MOD_PUT:
            item = self.current
            assert item is not None, "MOD_PUT без MOD_QUERY «шли»"
            offset = struct.unpack("<I", payload[:4])[0]
            data = payload[4:]
            assert offset == item["got"], f"кусок с {offset}, ждали {item['got']}"
            assert 0 < len(data) <= 8192 and item["got"] + len(data) <= item["size"]
            assert not self.refused, "кусок после отказа: ответ EA не проверен"
            if self.refuse_put == (item["name"], offset):
                self.refused = True
                return frame(0xEA, b"\0" + struct.pack("<I", item["got"]))
            self.puts.append((item["name"], offset, len(data)))
            item["got"] += len(data)
            item["sha"].update(data)
            if item["data"] is not None:
                item["data"] += data
            return frame(0xEA, b"\1" + struct.pack("<I", item["got"]))
        if command == CMD_MOD_COMMIT:
            item = self.current
            ok = item is not None and item["got"] == item["size"]
            if ok:
                self.files[item["name"]] = item
            self.current = None
            return frame(0xEB, bytes([ok]))
        if command in (CMD_PATH_GET, CMD_PATH_SET):
            if self.paths is None:                  # прошивка до 0.1.83
                return frame(0xEE, b"unsupported\0")
            serial = struct.unpack("<I", payload[:4])[0]
            if command == CMD_PATH_GET:
                name = payload[4:payload.index(0, 4)].decode()
                self.path_gets.append((name, serial))
                cluster = self.paths.get((name, serial))
                return frame(0xAA, bytes([cluster is not None]) + struct.pack("<I", cluster or 0))
            cluster = struct.unpack("<I", payload[4:8])[0]
            name = payload[8:payload.index(0, 8)].decode()
            self.path_sets.append((name, serial, cluster))
            self.paths[(name, serial)] = cluster
            return frame(0xAB, b"\1")
        if command == CMD_APP_START:
            self.started = payload[:payload.index(0)].decode()
            return frame(0xEC, b"\0" + payload + b"1.0\0")
        if command == CMD_APP_STOP:
            self.stopped = True
            return frame(0xED, b"\1")
        return frame(0xEE, b"unsupported\0")

    def data_puts(self, name: str) -> list[tuple[int, int]]:
        return [(offset, length) for item, offset, length in self.puts if item == name]


# --- машина -------------------------------------------------------------------------------

class DataSpgMachine(SpiLinkMixin):
    """Запускатель с драйвером FAT32: окна #0000/#4000 — драйвер, #8000 — код, #C000 — страницы."""

    PLUGIN_PAGE = None

    def __init__(self, variant: Path, card: Card, esp: DataEsp, link_version: int = 3) -> None:
        build = variant / "build"
        self.symbols = symbols(build / "esp_app.sym")
        self.machine = z80.Z80Machine()
        self.memory = self.machine.memory
        self.machine.set_memory_block(0x0000, (FAT32_BUILD / "fat32-work.bin").read_bytes())
        self.machine.set_memory_block(0x4000, (FAT32_BUILD / "fat32.bin").read_bytes())
        self.machine.set_memory_block(0x8000, (build / "esp_app.bin").read_bytes())
        self.machine.set_memory_block(0x3900, struct.pack("<4H", INIT, READ, WRITE, SYNC))
        # Страницы из SPG (spgbld.ini): ELF и ядро — с #20; драйвер уже в окнах #0000/#4000.
        self.pages: dict[int, bytearray] = {}
        for line in (build / "spgbld.ini").read_text(encoding="ascii").splitlines():
            if line.startswith("Block") and "esp_app.bin" not in line and "fat32" not in line:
                page, path = line.split(",")[1].strip(), line.split(",")[2].strip()
                data = (build / Path(path).name).read_bytes()
                self.pages[int(page.lstrip("#"), 16)] = bytearray(data.ljust(16384, b"\0"))
        self.page3 = 0
        self.card = card
        self.reads: list[int] = []
        self.writes = 0
        self.ts: dict[int, list[int]] = {}
        self.border: list[int] = []
        self.vconfig: list[int] = []
        self.key_polls = 0
        self.fe_reads_before_start = 0
        self.rx = bytearray()
        self.tx = bytearray()
        self.rx_polls = 0
        self.esp = None
        self.attach_link(LinkCore(from_capacity=16384))
        self.link.core.version = link_version
        self.ports.windows[2] = 0x8000  # код SPG: страница 2 в окне #8000 (SpgPage2)
        self.app = esp
        self._fast_dma()
        for address in (INIT, READ, WRITE, SYNC, self.symbols["Halt"]):
            self.machine.set_breakpoint(address)
        self.machine.set_input_callback(self.port_in)
        self.machine.set_output_callback(self.port_out)

    def _fast_dma(self) -> None:
        """DMA «ОЗУ → SPI» в приём ESP — одним куском, а не по байту (32 МБ иначе — минуты)."""
        ports, slave, launch = self.ports, self.slave, self.ports._dma_launch

        def fast(mode: int) -> None:
            if mode == 0x82 and slave.selected and slave.command == CMD_WRDMA and slave.phase >= 3:
                count = 2 * (ports.dma[0x26] + 1) * (ports.dma[0x28] + 1)
                memory, address = ports._dma_place(0x1A, count)
                slave.dma_rx += memory[address:address + count]
                slave.phase += count
                ports.received = ports._miso(slave.idle)
                ports.dma_runs.append((mode, count))
                return
            launch(mode)
        ports._dma_launch = fast

    def map3(self, page: int) -> None:
        self.pages[self.page3] = bytearray(self.memory[0xC000:0x10000])
        self.page3 = page
        self.memory[0xC000:0x10000] = bytes(self.pages.get(page, bytes(16384)))

    def port_in(self, port: int) -> int:
        try:
            if port & 0xFF == 0xFE:
                if self.app.started is None:
                    self.fe_reads_before_start += 1
                self.key_polls += 1
                pressed = bool(self.vconfig) and self.vconfig[-1] == 0x04 and self.key_polls > 50
                return 0xFF & ~(0x01 if pressed else 0)
            return super().port_in(port)
        except Exception as error:      # noqa: BLE001 — исключение из обратного вызова эмулятора
            self.error = error
            return 0xFF

    def port_out(self, port: int, value: int) -> None:
        try:
            if port == PAGE3:
                self.map3(value)
                self.ports.write(port, value)   # модели DMA: какая страница в окне #C000
                return
            if port in (MEMCONFIG, PAGE0, PAGE1):
                self.ts.setdefault(port, []).append(value)
                return
            if port == VCONFIG:
                self.vconfig.append(value)
                return
            if port & 0xFF == 0xFE:
                self.border.append(value & 7)
                return
            super().port_out(port, value)
        except Exception as error:      # noqa: BLE001
            self.error = error

    def sd_call(self) -> None:
        m = self.machine
        status = 0
        if m.pc == READ:
            lba = m.de << 16 | m.hl
            assert m.bc + 512 <= 0x10000, hex(m.bc)     # буфер драйвера (окно #0000) или свой
            self.reads.append(lba)
            m.set_memory_block(m.bc, self.card.disk.read(lba))
        elif m.pc == WRITE:
            self.writes += 1
            status = 0xE1
        # процедура порта портит основные регистры — так и на плате
        m.bc, m.de, m.hl = 0xA55A, 0x1234, 0x5678
        m.a, m.f = status, 64 if status == 0 else 0
        m.pc = int.from_bytes(self.memory[m.sp:m.sp + 2], "little")
        m.sp += 2

    def run(self) -> "DataSpgMachine":
        m = self.machine
        m.pc = self.symbols["Start"]
        for _ in range(3_000_000):
            m.ticks_to_stop = 2_000_000
            event = m.run()
            if hasattr(self, "error"):
                raise self.error
            if not event & base.BREAKPOINT_HIT:
                continue
            if m.pc == self.symbols["Halt"]:
                return self
            if m.pc in (INIT, READ, WRITE, SYNC):
                self.sd_call()
                continue
            raise AssertionError(f"останов на #{m.pc:04X}")
        raise AssertionError("запускатель не дошёл до конца")


# --- проверки -----------------------------------------------------------------------------

class EspAppSpgDataTest(unittest.TestCase):
    def launch(self, card: Card, esp: DataEsp, variant: str = "data", **kw) -> DataSpgMachine:
        return DataSpgMachine(VARIANTS[variant], card, esp, **kw).run()

    def assert_started(self, h: DataSpgMachine, esp: DataEsp) -> None:
        self.assertEqual(esp.started, "Q2.ELF")
        self.assertTrue(esp.stopped)
        self.assertEqual(h.vconfig, [0x04, 0x00])
        self.assertEqual(h.border[-1], 0, "в конце — чёрный бордюр")
        self.assertEqual(h.writes, 0, "на карту ничего не пишется")
        self.assertEqual(h.fe_reads_before_start, 0, "матрица клавиатуры ZX до запуска не читается")

    def test_memory_layout_and_pinned_driver_addresses(self) -> None:
        sym = symbols(FAT32_BUILD / "fat32.sym")
        self.assertEqual(sym["WDOS.ENTRY"], 0x3868)
        self.assertEqual(sym["WDOS.ADDTOP"], 0x389E)
        app = symbols(VARIANTS["data"] / "build" / "esp_app.sym")
        self.assertLessEqual(app["CodeEnd"], 0xC000 - 256, "код и стек — в окне #8000")
        card, _ = standard_card()
        h = self.launch(card, DataEsp())
        self.assertEqual(h.ts[MEMCONFIG], [0x0E])
        self.assertEqual(h.ts[PAGE0], [0x0F])
        self.assertEqual(h.ts[PAGE1], [0x0E])

    def test_data_files_reach_esp_byte_exact_by_dma(self) -> None:
        card, where = standard_card()
        esp = DataEsp(paths={})
        h = self.launch(card, esp)
        self.assert_started(h, esp)
        self.assertEqual([(q[0], q[3], q[4]) for q in esp.queries],
                         [(0, "Q2.ELF", None), (4, "PAK0.PAK", APP), (4, "PAK1.PAK", APP)],
                         "ELF, затем файлы данных с флагом 4 и именем приложения")
        self.assertEqual(esp.queries[1][1:3], (len(PAK0), STAMP), "длина и время — из записи каталога")
        self.assertEqual(bytes(esp.files["PAK0.PAK"]["data"]), PAK0, "PAK0 — не байт в байт")
        self.assertEqual(bytes(esp.files["PAK1.PAK"]["data"]), PAK1, "PAK1 — не байт в байт")
        self.assertEqual(bytes(esp.files["Q2.ELF"]["data"]), ELF_BYTES)
        pieces = esp.data_puts("PAK0.PAK")
        self.assertTrue(all(length == 8192 for _, length in pieces[:-1]), "порции — по 8 КБ")
        self.assertIn((0x82, 8192), h.ports.dma_runs, "порция — одной передачей DMA")
        self.assertLess(esp.commands.index(CMD_MOD_COMMIT), esp.commands.index(CMD_APP_START))
        self.assertTrue({CYAN, WHITE} <= set(h.border), "ход передачи — голубой и белый")
        # папки в ESP не было: поиск, затем PATH_SET
        self.assertEqual(esp.path_gets, [(APP, SERIAL)])
        self.assertEqual(esp.path_sets, [(APP, SERIAL, where["QUAKE2"])])

    def test_big_files_have_32bit_offsets_in_order(self) -> None:
        card = Card(spc=64)             # кластер 32 КБ
        folder = card.mkdir(2, "Q2")
        card.put(folder, "Q2.ELF", ELF_BYTES)
        big0, big1 = (16 << 20) + 1000, (32 << 20) + 5000
        card.put_sparse(folder, "PAK0.PAK", big0)
        card.put_sparse(folder, "PAK1.PAK", big1)
        esp = DataEsp(paths={})
        h = self.launch(card, esp)
        self.assert_started(h, esp)
        for name, size in (("PAK0.PAK", big0), ("PAK1.PAK", big1)):
            pieces = esp.data_puts(name)
            offsets = [offset for offset, _ in pieces]
            self.assertEqual(offsets, sorted(offsets))
            self.assertEqual(sum(length for _, length in pieces), size)
            self.assertEqual(esp.files[name]["size"], size)
            self.assertEqual(esp.files[name]["sha"].hexdigest(),
                             hashlib.sha256(card.contents(name, folder)).hexdigest(), name)
        offsets = [offset for offset, _ in esp.data_puts("PAK1.PAK")]
        self.assertTrue(any(o >= 1 << 24 for o in offsets) and any(o >= 1 << 25 for o in offsets),
                        "смещения за 16 и 32 МБ")

    def test_frames_without_fast_path(self) -> None:
        card, _ = standard_card()
        esp = DataEsp(paths={})
        h = self.launch(card, esp, link_version=1)     # канал SPI версии 1: raw media нет
        self.assert_started(h, esp)
        self.assertEqual(bytes(esp.files["PAK0.PAK"]["data"]), PAK0)
        self.assertEqual(bytes(esp.files["PAK1.PAK"]["data"]), PAK1)
        self.assertTrue(all(length <= 1016 for _, length in esp.data_puts("PAK0.PAK")),
                        "без быстрого пути — кадры по 1016 байтов")

    def test_fresh_copy_on_drive_is_not_sent(self) -> None:
        card, _ = standard_card()
        esp = DataEsp(data_status={"PAK0.PAK": 0, "PAK1.PAK": 0}, paths={})
        h = self.launch(card, esp)
        self.assert_started(h, esp)
        self.assertEqual(esp.data_puts("PAK0.PAK") + esp.data_puts("PAK1.PAK"), [])
        self.assertEqual(sorted(esp.files), ["Q2.ELF"], "MOD_COMMIT — только у ELF")

    def test_no_room_shows_magenta_without_start(self) -> None:
        card, _ = standard_card()
        esp = DataEsp(data_status={"PAK1.PAK": 2}, paths={})
        h = self.launch(card, esp)
        self.assertIsNone(esp.started)
        self.assertNotIn(0x04, h.vconfig, "без приложения экран у FT812 не отбирается")
        self.assertEqual(h.border[-1], MAGENTA)
        self.assertIn("PAK0.PAK", esp.files, "первый файл успел лечь")

    def test_no_usb_drive_shows_yellow_without_start(self) -> None:
        card, _ = standard_card()
        esp = DataEsp(data_status={"PAK0.PAK": 4}, paths={})
        h = self.launch(card, esp)
        self.assertIsNone(esp.started)
        self.assertNotIn(0x04, h.vconfig)
        self.assertEqual(h.border[-1], YELLOW)
        self.assertEqual(esp.data_puts("PAK0.PAK"), [])

    def test_refused_piece_stops_with_red(self) -> None:
        card, _ = standard_card()
        esp = DataEsp(paths={}, refuse_put=("PAK0.PAK", 3 * 8192))
        h = self.launch(card, esp)
        self.assertIsNone(esp.started)
        self.assertNotIn("PAK0.PAK", esp.files)
        self.assertNotIn(CMD_MOD_COMMIT, esp.commands[esp.commands.index(CMD_MOD_COMMIT) + 1:])
        self.assertEqual(h.border[-1], RED)

    def test_bad_name_answer_is_red(self) -> None:
        card, _ = standard_card()
        esp = DataEsp(data_status={"PAK0.PAK": 3}, paths={})
        h = self.launch(card, esp)
        self.assertIsNone(esp.started)
        self.assertEqual(h.border[-1], RED)

    def test_folder_from_path_get_without_search(self) -> None:
        card, where = standard_card()
        esp = DataEsp(paths={(APP, SERIAL): where["QUAKE2"]})
        h = self.launch(card, esp)
        self.assert_started(h, esp)
        self.assertEqual(esp.path_sets, [], "папка из ESP подошла — PATH_SET не нужен")
        for name in ("AAA", "GAMES"):
            self.assertNotIn(card.lba(where[name]), h.reads, f"поиска не было: каталог {name} не читался")
        self.assertEqual(bytes(esp.files["PAK0.PAK"]["data"]), PAK0)

    def test_stale_path_is_searched_again(self) -> None:
        card, where = standard_card()
        esp = DataEsp(paths={(APP, SERIAL): where["AAA"]})     # там ничего нет
        h = self.launch(card, esp)
        self.assert_started(h, esp)
        self.assertEqual(esp.path_sets, [(APP, SERIAL, where["QUAKE2"])])

    def test_path_outside_volume_is_searched_again(self) -> None:
        card, where = standard_card()
        esp = DataEsp(paths={(APP, SERIAL): 0x0FFFFFF0})
        h = self.launch(card, esp)
        self.assert_started(h, esp)
        self.assertEqual(esp.path_sets, [(APP, SERIAL, where["QUAKE2"])])

    def test_firmware_without_path_commands_searches(self) -> None:
        card, _ = standard_card()
        esp = DataEsp(paths=None)
        h = self.launch(card, esp)
        self.assert_started(h, esp)
        self.assertEqual(esp.path_sets, [])

    def test_missing_data_file_stops_before_elf(self) -> None:
        card, _ = standard_card(pak1=False)
        esp = DataEsp(paths={})
        h = self.launch(card, esp)
        self.assertEqual(esp.queries, [], "нет файла данных — ничего не передаётся")
        self.assertIsNone(esp.started)
        self.assertEqual(h.border[-1], RED)

    def test_no_folder_on_card_is_red(self) -> None:
        card = Card()
        card.mkdir(2, "EMPTY")
        esp = DataEsp(paths={})
        h = self.launch(card, esp)
        self.assertEqual(esp.queries, [])
        self.assertEqual(h.border[-1], RED)

    def test_order_core_elf_data(self) -> None:
        card, _ = standard_card()
        esp = DataEsp(paths={})
        h = self.launch(card, esp, variant="core")
        self.assert_started(h, esp)
        self.assertEqual([(q[0], q[3]) for q in esp.queries],
                         [(2, "Q2CORE.BIN"), (0, "Q2.ELF"), (4, "PAK0.PAK")])
        self.assertEqual(bytes(esp.files["Q2CORE.BIN"]["data"]), CORE_BYTES)
        self.assertEqual(bytes(esp.files["PAK0.PAK"]["data"]), PAK0)

    # --- дополнительные модули ELF (26.11.2026) ---

    def test_order_core_elf_modules_data(self) -> None:
        card, _ = standard_card()
        esp = DataEsp(paths={})
        h = self.launch(card, esp, variant="mods")
        self.assert_started(h, esp)
        self.assertEqual([(q[0], q[3]) for q in esp.queries],
                         [(2, "Q2CORE.BIN"), (0, "Q2.ELF"), (0, "Q2HOT.ELF"), (0, "Q2SKY.ELF"), (4, "PAK0.PAK")])
        self.assertEqual(esp.queries[2][1], len(HOT_BYTES))
        self.assertEqual(bytes(esp.files["Q2HOT.ELF"]["data"]), HOT_BYTES, "модуль на три страницы")
        self.assertEqual(bytes(esp.files["Q2SKY.ELF"]["data"]), SKY_BYTES)
        self.assertEqual(bytes(esp.files["Q2.ELF"]["data"]), ELF_BYTES)
        self.assertEqual(bytes(esp.files["PAK0.PAK"]["data"]), PAK0)
        self.assertLess(esp.commands.index(CMD_MOD_QUERY), esp.commands.index(CMD_APP_START))

    def test_refused_module_does_not_stop_launch(self) -> None:
        card, _ = standard_card()
        esp = DataEsp(paths={}, refuse_names=("Q2HOT.ELF",))
        h = self.launch(card, esp, variant="mods")
        self.assert_started(h, esp)
        self.assertNotIn(RED, h.border, "отказ ESP в модуле — не ошибка запуска")
        self.assertNotIn("Q2HOT.ELF", esp.files)
        self.assertEqual(bytes(esp.files["Q2SKY.ELF"]["data"]), SKY_BYTES, "следующий модуль всё равно идёт")
        self.assertEqual(bytes(esp.files["PAK0.PAK"]["data"]), PAK0)

    def test_cached_module_is_not_sent_again(self) -> None:
        card, _ = standard_card()
        esp = DataEsp(paths={}, cached_names=("Q2HOT.ELF",))
        h = self.launch(card, esp, variant="mods")
        self.assert_started(h, esp)
        self.assertIn((0, "Q2HOT.ELF"), [(q[0], q[3]) for q in esp.queries])
        self.assertNotIn("Q2HOT.ELF", esp.files, "годная копия в кэше ESP — не передаётся")
        self.assertEqual(esp.data_puts("Q2HOT.ELF"), [])
        self.assertEqual(bytes(esp.files["Q2SKY.ELF"]["data"]), SKY_BYTES)

    def test_modules_without_data_files(self) -> None:
        esp = DataEsp(paths={})
        h = self.launch(Card(), esp, variant="mods_nodata")
        self.assert_started(h, esp)
        self.assertEqual([(q[0], q[3]) for q in esp.queries], [(0, "Q2.ELF"), (0, "Q2HOT.ELF")])
        self.assertEqual(bytes(esp.files["Q2HOT.ELF"]["data"]), HOT_BYTES)
        self.assertEqual(h.reads, [], "без файлов данных карта не читается")

    def test_prep_rejects_bad_modules(self) -> None:
        folder = WORK / "bad_mods"
        shutil.rmtree(folder, ignore_errors=True)
        folder.mkdir(parents=True)
        elf = folder / "Q2.ELF"
        elf.write_bytes(ELF_BYTES)
        (folder / "NOTELF.ELF").write_bytes(b"MZ" + bytes(100))
        (folder / "A").mkdir()
        (folder / "A" / "Q2HOT.ELF").write_bytes(HOT_BYTES)
        (folder / "Q2HOT.ELF").write_bytes(HOT_BYTES)
        (folder / "LONGNAME9.ELF").write_bytes(HOT_BYTES)
        for mods in ([f"+{folder / 'NOTELF.ELF'}"],
                     [f"+{folder / 'Q2HOT.ELF'}", f"+{folder / 'A' / 'Q2HOT.ELF'}"],
                     [f"+{elf}"],
                     [f"+{folder / 'LONGNAME9.ELF'}"],
                     [f"+{folder / 'MISSING.ELF'}"]):
            result = subprocess.run([sys.executable, str(SPG_SRC / "prep.py"), str(elf), *mods,
                                     "--out", str(folder / "build")], capture_output=True)
            self.assertNotEqual(result.returncode, 0, f"prep.py взял плохой модуль: {mods}")


# --- прошивка 0.1.166 без флешки: файлы данных — по запросу ----------------------------------------

CMD_DATA_PIECE, EVT_DATA_READ = 0x6F, 0x6E
DATA_REMOTE = 5


class RemoteEsp(DataEsp):
    """Флешки нет: на MOD_QUERY с номером файла — ответ 5; после APP_START — запросы кусков
    EVT_DATA_READ по одному (следующий — когда пришёл весь прежний или кусок без данных)."""

    def __init__(self, reads: list, early: bool = False, **kw) -> None:
        super().__init__(paths={}, **kw)
        self.reads = list(reads)        # (имя файла или номер, смещение, длина)
        self.early = early              # первый запрос — раньше ответа на APP_START
        self.indices: dict[str, int] = {}
        self.results: list[tuple] = []  # (имя или номер, смещение, байты или None — отказ)
        self.pieces: list[tuple[int, int]] = []
        self.seq = 0
        self.want: dict | None = None

    @property
    def busy(self) -> bool:
        return self.want is not None or bool(self.reads)

    def next_event(self) -> bytes:
        self.want = None
        if not self.reads:
            return b""
        what, offset, count = self.reads.pop(0)
        index = self.indices[what] if isinstance(what, str) else what
        self.seq = (self.seq + 1) & 255
        self.want = {"what": what, "offset": offset, "count": count, "got": bytearray(), "seq": self.seq}
        return frame(EVT_DATA_READ, struct.pack("<BBIH", self.seq, index, offset, count))

    def __call__(self, command: int, payload: bytes) -> bytes:
        if command == CMD_MOD_QUERY and payload[0] & 4:
            self.commands.append(command)
            end = payload.index(0, 9)
            app_end = payload.index(0, end + 1)
            name = payload[9:end].decode()
            self.queries.append((payload[0], *struct.unpack("<II", payload[1:9]), name,
                                 payload[end + 1:app_end].decode()))
            has_index = app_end + 1 < len(payload)
            if has_index:
                self.indices[name] = payload[app_end + 1]
            return frame(0xE9, bytes([DATA_REMOTE if has_index else 4]) + struct.pack("<I", 0))
        if command == CMD_APP_START:
            reply = super().__call__(command, payload)
            first = self.next_event()
            return first + reply if self.early else reply + first
        if command == CMD_DATA_PIECE:
            self.commands.append(command)
            seq, offset = payload[0], struct.unpack("<I", payload[1:5])[0]
            data = payload[5:]
            want = self.want
            assert want is not None and seq == want["seq"], "кусок без запроса"
            assert offset == want["offset"] + len(want["got"]), "кусок не по порядку"
            self.pieces.append((offset, len(data)))
            if not data:
                self.results.append((want["what"], want["offset"], None))
                return self.next_event()
            want["got"] += data
            assert len(want["got"]) <= want["count"], "кусок длиннее запроса"
            if len(want["got"]) == want["count"]:
                self.results.append((want["what"], want["offset"], bytes(want["got"])))
                return self.next_event()
            return b""
        return super().__call__(command, payload)


class RemoteMachine(DataSpgMachine):
    """Клавиша — только когда все запросы обслужены; зависание (больше IDLE_ROUNDS кругов по 2 млн
    тактов нет ни обращений к карте, ни новых кадров и кусков у «прошивки») — неудача, а не вечный
    тест. Передача большого ELF или ядра кадрами идёт без карты — её ход виден по кадрам."""

    IDLE_ROUNDS = 400

    def port_in(self, port: int) -> int:
        if port & 0xFF == 0xFE and getattr(self.app, "busy", False):
            self.key_polls = 0
        return super().port_in(port)

    def run(self) -> "RemoteMachine":
        m = self.machine
        m.pc = self.symbols["Start"]
        idle = 0
        seen = None
        while idle < self.IDLE_ROUNDS:
            m.ticks_to_stop = 2_000_000
            event = m.run()
            if hasattr(self, "error"):
                raise self.error
            if not event & base.BREAKPOINT_HIT:
                progress = (len(self.app.commands), len(self.app.pieces), len(self.app.puts))
                idle = 0 if progress != seen else idle + 1
                seen = progress
                continue
            idle = 0
            if m.pc == self.symbols["Halt"]:
                return self
            if m.pc in (INIT, READ, WRITE, SYNC):
                self.sd_call()
                continue
            raise AssertionError(f"останов на #{m.pc:04X}")
        raise AssertionError(f"запускатель стоит; запросы: {self.app.want} и ещё {len(self.app.reads)}")


class EspAppSpgRemoteTest(unittest.TestCase):
    """Прошивка 0.1.166 без флешки: файлы данных остаются на карте, запускатель отдаёт их куски
    по запросу ESP, пока приложение работает."""

    def launch(self, card: Card, esp: RemoteEsp, **kw) -> RemoteMachine:
        return RemoteMachine(VARIANTS["data"], card, esp, **kw).run()

    def assert_started(self, h: RemoteMachine, esp: RemoteEsp) -> None:
        self.assertEqual(esp.started, "Q2.ELF")
        self.assertTrue(esp.stopped)
        self.assertEqual(h.vconfig, [0x04, 0x00])
        self.assertEqual(h.border[-1], 0)
        self.assertEqual(h.writes, 0, "на карту ничего не пишется")
        self.assertFalse(esp.busy, "все запросы обслужены до клавиши")
        data_at = esp.commands.index(CMD_MOD_COMMIT) + 1   # MOD_PUT до него — сам ELF
        self.assertNotIn(CMD_MOD_PUT, esp.commands[data_at:], "файлы данных не передаются")
        self.assertEqual(sorted(esp.files), ["Q2.ELF"])
        self.assertEqual([q[3] for q in esp.queries if q[0] == 4], ["PAK0.PAK", "PAK1.PAK"])
        self.assertEqual(esp.indices, {"PAK0.PAK": 0, "PAK1.PAK": 1}, "номер файла — за приложением")

    def check_reads(self, esp: RemoteEsp, files: dict) -> None:
        for what, offset, data in esp.results:
            self.assertIsNotNone(data, (what, offset))
            want = files[what][offset:offset + len(data)]
            self.assertEqual(data, want, f"{what} с {offset}: не байт в байт")

    READS = [("PAK0.PAK", 0, 8192), ("PAK0.PAK", 512 * 3, 20000), ("PAK1.PAK", 0, 3000),
             ("PAK0.PAK", 99840, 160), ("PAK0.PAK", 4096, 32768), ("PAK0.PAK", 50176, 512)]

    def test_reads_on_request_by_dma(self) -> None:
        for spc in (1, 8):              # кластер 512 байтов (196 пробегов) и 4 КБ
            with self.subTest(spc=spc):
                card, _ = standard_card(spc)
                esp = RemoteEsp(self.READS)
                h = self.launch(card, esp)
                self.assert_started(h, esp)
                self.assertEqual(len(esp.results), len(self.READS))
                self.check_reads(esp, {"PAK0.PAK": PAK0, "PAK1.PAK": PAK1})
                self.assertIn((0x82, 8192), h.ports.dma_runs, "кусок 8 КБ — одной передачей DMA")
                self.assertTrue(all(length <= 8192 for _, length in esp.pieces))
                self.assertEqual(esp.pieces[-1], (50176, 512))

    def test_reads_by_frames_without_fast_path(self) -> None:
        card, _ = standard_card()
        esp = RemoteEsp(self.READS)
        h = self.launch(card, esp, link_version=1)
        self.assert_started(h, esp)
        self.check_reads(esp, {"PAK0.PAK": PAK0, "PAK1.PAK": PAK1})
        self.assertTrue(all(length <= 1016 for _, length in esp.pieces), "кадры по 1016 байтов")

    def test_request_before_start_reply(self) -> None:
        card, _ = standard_card()
        esp = RemoteEsp(self.READS[:2], early=True)
        h = self.launch(card, esp)
        self.assert_started(h, esp)
        self.check_reads(esp, {"PAK0.PAK": PAK0})
        self.assertEqual(len(esp.results), 2)

    def test_big_file_32bit_offsets_across_clusters(self) -> None:
        card = Card(spc=64)             # кластер 32 КБ
        folder = card.mkdir(2, "Q2")
        card.put(folder, "Q2.ELF", ELF_BYTES)
        big = (40 << 20) + 777
        card.put_sparse(folder, "PAK0.PAK", big)
        card.put(folder, "PAK1.PAK", PAK1)
        far = 1100 * 32768              # 34 МБ
        reads = [("PAK0.PAK", far, 512), ("PAK0.PAK", far + 63 * 512, 1024),
                 ("PAK0.PAK", (big // 512) * 512, big % 512), ("PAK1.PAK", 2560, 440)]
        esp = RemoteEsp(reads)
        h = self.launch(card, esp)
        self.assert_started(h, esp)
        self.check_reads(esp, {"PAK0.PAK": card.contents("PAK0.PAK", folder), "PAK1.PAK": PAK1})
        first, cross = esp.results[0][2], esp.results[1][2]
        self.assertEqual(struct.unpack("<III", first[:12]), (1100, 0, 0xC0DE5EC7))
        self.assertEqual(struct.unpack("<III", cross[:12]), (1100, 63, 0xC0DE5EC7))
        self.assertEqual(struct.unpack("<III", cross[512:524]), (1101, 0, 0xC0DE5EC7), "через кластер")

    def test_unreadable_requests_get_empty_piece(self) -> None:
        card, _ = standard_card()
        reads = [(9, 0, 512), ("PAK0.PAK", 100352, 512), ("PAK0.PAK", 512, 1000)]
        esp = RemoteEsp(reads)
        h = self.launch(card, esp)
        self.assert_started(h, esp)
        self.assertEqual([r[2] for r in esp.results[:2]], [None, None],
                         "нет такого файла, смещение за цепочкой — кусок без данных")
        self.assertEqual(esp.results[2][2], PAK0[512:1512], "после отказа — снова читает")

    def test_too_fragmented_file_is_red_without_start(self) -> None:
        card = Card()
        folder = card.mkdir(2, "Q2")
        card.put(folder, "Q2.ELF", ELF_BYTES)
        card.put(folder, "PAK0.PAK", bytes(1000 * 512), fragmented=True)     # 1000 пробегов > 896
        card.put(folder, "PAK1.PAK", PAK1)
        esp = RemoteEsp([])
        h = self.launch(card, esp)
        self.assertIsNone(esp.started)
        self.assertEqual(h.border[-1], RED)
        self.assertNotIn(0x04, h.vconfig)

    def test_old_firmware_without_index_support_is_yellow(self) -> None:
        # прошивка 0.1.165 на номер не смотрит: без флешки — ответ 4
        card, _ = standard_card()
        esp = DataEsp(data_status={"PAK0.PAK": 4}, paths={})
        h = DataSpgMachine(VARIANTS["data"], card, esp).run()
        self.assertIsNone(esp.started)
        self.assertEqual(h.border[-1], YELLOW)


if __name__ == "__main__":
    unittest.main()
