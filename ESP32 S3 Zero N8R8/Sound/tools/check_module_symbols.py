"""Все ли внешние символы модуля ESP даёт ядро и все ли его секции загрузчик кладёт.

Модуль (ELF из /WC/ESP/) связывается на плате загрузчиком прошивки
(Firmware/components/elf_loader): символ ищется в его таблицах libc и ESP-IDF
(src/esp_elf_symbol.c, ESP_ELFSYM_EXPORT) и в таблице ядра (Firmware/src/zm_symbols.cpp,
ZM_SYM). Символа нет ни там, ни там —
модуль на плате не загрузится. Проверка на ПК ловит это сразу после сборки.

Загрузчик кладёт в память только .text, .rodata, .data, .data.rel.ro и .bss.
Код или данные в другой секции (.iram1.* от IRAM_ATTR, .dram1.* и т. п.) на
плате не окажутся там, куда на них ссылаются, — так TRACKER.ELF ронял ESP при
связывании. Такая секция — ошибка проверки.

Указатель с перемещением R_XTENSA_RELATIVE загрузчик переводит в адрес на плате по
секции, в которую он попадает (esp_elf_map_sym, полуоткрытый интервал); вне загружаемых
секций — 0. Так бывает с указателем «за последний элемент» массива в конце секции:
компилятор делает из цикла по таблице цикл до её конца. В MIDI.ELF 1.7 таблица
модуляторов по умолчанию лежала в конце .rodata, конец её стал нулём, цикл бежал по
памяти до ошибки кэша (ESP сбрасывалась при загрузке банка). Такой указатель — ошибка
проверки: модуль должен работать и с прошивками, где загрузчик это ещё не прощает.

    python tools/check_module_symbols.py modules/out/MP3.ELF ...
Выход 0 — всё есть; 1 — список недостающих.
"""
import pathlib
import re
import subprocess
import sys

# Таблицы символов — в прошивке (папка Firmware рядом с Sound).
FIRMWARE = pathlib.Path(__file__).resolve().parents[2] / "Firmware"


def readelf() -> str:
    tools = pathlib.Path(r"E:\zx\esp\.espressif\tools\xtensa-esp-elf")
    found = sorted(tools.glob("*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-readelf.exe"))
    if not found:
        raise SystemExit("xtensa-esp32s3-elf-readelf not found")
    return str(found[-1])


def allowed() -> set[str]:
    names = set()
    loader = (FIRMWARE / "components/elf_loader/src/esp_elf_symbol.c").read_text(encoding="utf-8")
    names.update(re.findall(r"ESP_ELFSYM_EXPORT\((\w+)\)", loader))
    core = (FIRMWARE / "src/zm_symbols.cpp").read_text(encoding="utf-8")
    names.update(re.findall(r"ZM_SYM\((\w+)\)", core))
    return names


def undefined(path: str, tool: str) -> list[str]:
    output = subprocess.run([tool, "-W", "--dyn-syms", path], capture_output=True,
                            text=True).stdout
    result = []
    for line in output.splitlines():
        parts = line.split()
        if len(parts) >= 8 and parts[6] == "UND" and parts[7]:
            result.append(parts[7])
    return result


# Секции, которые загрузчик кладёт, и служебные секции динамической связи.
LOADED = {".text", ".rodata", ".data", ".data.rel.ro", ".bss"}
DYNAMIC = {".hash", ".gnu.hash", ".dynsym", ".dynstr", ".rela.dyn", ".rela.plt", ".dynamic",
           ".got", ".got.plt", ".plt", ".interp"}


def unloaded_sections(path: str, tool: str) -> list[str]:
    """Выделяемые (флаг A) секции, которых загрузчик не кладёт."""
    output = subprocess.run([tool, "-S", "-W", path], capture_output=True, text=True).stdout
    result = []
    for line in output.splitlines():
        match = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+[0-9a-f]+\s+[0-9a-f]+\s+([0-9a-f]+)"
                         r"\s+\S+\s+(\S*)", line)
        if match and "A" in match.group(3) and int(match.group(2), 16) != 0:
            name = match.group(1)
            if name not in LOADED and name not in DYNAMIC:
                result.append(name)
    return result


def relative_outside(path: str, tool: str) -> list[str]:
    """Указатели R_XTENSA_RELATIVE, которые не попадают внутрь загружаемых секций."""
    output = subprocess.run([tool, "-S", "-W", path], capture_output=True, text=True).stdout
    sections = []                                      # (имя, адрес, место в файле, длина, есть ли байты)
    for line in output.splitlines():
        match = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", line)
        if match:
            name, kind = match.group(1), match.group(2)
            sections.append((name, int(match.group(3), 16), int(match.group(4), 16), int(match.group(5), 16), kind != "NOBITS"))
    loaded = [(name, addr, size) for name, addr, _, size, _ in sections if name in LOADED and size]
    data = pathlib.Path(path).read_bytes()
    relocations = subprocess.run([tool, "-r", "-W", path], capture_output=True, text=True).stdout
    result = []
    for line in relocations.splitlines():
        parts = line.split()
        if len(parts) < 3 or parts[2] != "R_XTENSA_RELATIVE":
            continue
        where = int(parts[0], 16)
        holder = next(((addr, offset) for name, addr, offset, size, bits in sections
                       if bits and addr <= where < addr + size), None)
        if holder is None:
            result.append(f"0x{where:x} (place outside sections)")
            continue
        at = holder[1] + (where - holder[0])
        value = int.from_bytes(data[at:at + 4], "little")
        if not any(addr <= value < addr + size for _, addr, size in loaded):
            edge = next((name for name, addr, size in loaded if value == addr + size), None)
            result.append(f"0x{value:x} at 0x{where:x}" + (f" (end of {edge})" if edge else ""))
    return result


def main() -> int:
    tool = readelf()
    known = allowed()
    status = 0
    for path in sys.argv[1:]:
        needed = undefined(path, tool)
        missing = [name for name in needed if name not in known]
        if missing:
            status = 1
            print(f"{path}: the core does not give {', '.join(sorted(missing))}")
        else:
            print(f"{path}: {len(needed)} external symbols, all given by the core")
        foreign = unloaded_sections(path, tool)
        if foreign:
            status = 1
            shown = ", ".join(foreign[:6]) + (f" and {len(foreign) - 6} more" if len(foreign) > 6 else "")
            print(f"{path}: sections the ELF loader does not load: {shown}")
        outside = relative_outside(path, tool)
        if outside:
            status = 1
            shown = ", ".join(outside[:6]) + (f" and {len(outside) - 6} more" if len(outside) > 6 else "")
            print(f"{path}: pointers the ELF loader turns into 0 (outside loaded sections): {shown}")
    return status


if __name__ == "__main__":
    sys.exit(main())
