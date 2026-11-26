#!/usr/bin/env python3
"""Подготовка SPG-запускателя для приложения ESP: build/app_cfg.inc, куски ELF по 16 КБ
и build/spgbld.ini.

    python prep.py путь/к/BOING.ELF [путь/к/ЯДРО.BIN] [+путь/к/МОДУЛЬ.ELF …] [ДАННЫЕ.EXT …] [--out ПАПКА]
    (из папки ESP App SPG; --out — куда класть вместо build, для тестов)

Ядро ПЛИС (необязательно, ESP 0.1.164+) кладётся в SPG вслед за ELF, тоже кусками по 16 КБ:
запускатель шлёт его первым с флагом 2 в MOD_QUERY, ESP держит копию на флешке USB. Второй
аргумент — ядро, если у него расширение .BIN и это путь к файлу (файл есть или в аргументе
есть папка); иначе это уже имя файла данных.

Файлы данных (необязательно, ESP 0.1.165+) — только имена 8.3: сами файлы в SPG не кладутся,
они лежат на карте Evo в папке приложения рядом с копией ИМЯ.ELF. Запускатель находит папку,
читает файлы драйвером FAT32 и шлёт их ESP после ELF с флагом 4 (esp_app.asm, app_data.asm).
Для этого в SPG идут ещё две страницы драйвера FAT32 (shared/fat32/build: сначала
shared/fat32/build.py — так делает build.bat), а запускателю нужна рабочая страница
SCRATCH_PAGE — первая за ELF и ядром.

Дополнительные модули ELF (необязательно; 26.11.2026, для Quake 2: горячий код — в отдельном
модуле, его приложение грузит module_open_ex с ZM_MODULE_TEXT_INTERNAL) — пути к файлам ИМЯ.ELF,
можно с «+» впереди (+Q2HOT.ELF). Кладутся в SPG вслед за ELF и ядром, каждый — со своей страницы,
и уходят в ESP после главного ELF, до файлов данных и APP_START, тем же MOD_QUERY / MOD_PUT /
MOD_COMMIT с флагом 0. Годная копия в кэше ESP — не передаётся; ESP модуль не взяла — запуск идёт
дальше без него (приложение само решает, как жить без модуля).

Время для кэша ESP (MOD_QUERY) — время изменения файла ELF в формате FAT: новый ELF той же
длины ESP не спутает со старым. У файлов данных время и длину берёт сам запускатель — из
записи каталога на карте.
"""
import os
import re
import shutil
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
FAT32_BUILD = HERE.parent / "shared" / "fat32" / "build"
PAGE = 16384
APP_PAGE0 = 0x20
FAT32_PAGE = 0x0E                       # код драйвера FAT32 — в окне #4000 (как у DOOM VDAC4)
FAT32_WORK_PAGE = 0x0F                  # его рабочая страница — в окне #0000
MAX_DATA = 16                           # файлов данных у приложения — не больше
MAX_MODS = 8                            # дополнительных модулей ELF — не больше
MAX_MODULE = 2 * 1024 * 1024            # больше ESP не берёт (module_app.cpp, kMaxModuleBytes)
NAME_CHAR = r"[A-Z0-9_-]"               # знаки имён, которые берёт ESP (app_data_files.cpp)


def fat_time(path: Path) -> int:
    t = time.localtime(path.stat().st_mtime)
    date = ((t.tm_year - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday
    tod = (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2)
    return (date << 16) | tod


def is_core(arg: str) -> bool:
    """Второй аргумент — ядро ПЛИС: .BIN и путь к файлу, а не голое имя файла данных."""
    path = Path(arg)
    return path.suffix.upper() == ".BIN" and (path.is_file() or path.name != arg)


def is_module(arg: str) -> bool:
    """Дополнительный модуль: «+путь» или путь к существующему файлу .ELF."""
    return arg.startswith("+") or (Path(arg).suffix.upper() == ".ELF" and Path(arg).is_file())


def name83(name: str) -> str:
    """ИМЯ.EXT — как в записи каталога FAT: 8 знаков имени и 3 расширения, с пробелами."""
    stem, _, ext = name.partition(".")
    return stem.ljust(8) + ext.ljust(3)


def main() -> int:
    args = sys.argv[1:]
    out = HERE / "build"
    if "--out" in args:
        at = args.index("--out")
        out = Path(args[at + 1]).resolve()
        del args[at:at + 2]
    if not args:
        print(__doc__)
        return 1
    elf = Path(args.pop(0)).resolve()
    core = Path(args.pop(0)).resolve() if args and is_core(args[0]) else None
    mods = [Path(a[1:] if a.startswith("+") else a).resolve() for a in args if is_module(a)]
    names = [a.upper() for a in args if not is_module(a)]
    data = elf.read_bytes()
    if data[:4] != b"\x7fELF":
        print(f"{elf}: не ELF")
        return 1
    name = elf.name.upper()
    stem = elf.stem.upper()
    if len(elf.stem) > 8 or len(elf.suffix) > 4:
        print(f"{name}: имя не 8.3")
        return 1
    build = out
    build.mkdir(parents=True, exist_ok=True)

    def rel(path: Path) -> str:
        """Путь блока для spgbld: от папки _spg, где build.bat его запускает."""
        try:
            return os.path.relpath(path, HERE / "_spg").replace("\\", "/")
        except ValueError:              # другой диск
            return path.as_posix()

    for old in list(build.glob("elf*.bin")) + list(build.glob("core*.bin")) + \
            list(build.glob("mod*.bin")) + list(build.glob("fat32*.bin")):
        old.unlink()                    # куски прошлой сборки (другой длины) не нужны
    pages = (len(data) + PAGE - 1) // PAGE
    blocks = []
    for i in range(pages):
        part = build / f"elf{i}.bin"
        part.write_bytes(data[i * PAGE:(i + 1) * PAGE])
        blocks.append(f"Block = #c000, #{APP_PAGE0 + i:02x}, {rel(part)}")
    cfg = ("; created by prep.py, do not edit\n"   # только ASCII: sjasmplus и spgbld
           f'        DEFINE APP_NAME db "{name}",0\n'
           f"APP_LEN         equ {len(data)}\n"
           f"APP_TIME        equ #{fat_time(elf):08X}\n"
           f"APP_PAGE0       equ #{APP_PAGE0:02X}\n")
    free_page = APP_PAGE0 + pages       # первая страница за ELF (и ядром)
    if core is not None:
        bits = core.read_bytes()
        cname = core.name.upper()
        # битстрим Gowin: синхрослово A5C3 в начале; больше слота flash Tang (1 МБ) не бывает
        if len(core.stem) > 8 or core.suffix.upper() != ".BIN" or b"\xa5\xc3" not in bits[:128] \
                or len(bits) > 1024 * 1024:
            print(f"{core}: не ядро ПЛИС ИМЯ.BIN (8.3, битстрим Gowin до 1 МБ)")
            return 1
        core_page0 = APP_PAGE0 + pages
        core_pages = (len(bits) + PAGE - 1) // PAGE
        for i in range(core_pages):
            part = build / f"core{i}.bin"
            part.write_bytes(bits[i * PAGE:(i + 1) * PAGE])
            blocks.append(f"Block = #c000, #{core_page0 + i:02x}, {rel(part)}")
        cfg += ("        DEFINE HAS_CORE\n"
                f'        DEFINE CORE_NAME db "{cname}",0\n'
                f"CORE_LEN        equ {len(bits)}\n"
                f"CORE_TIME       equ #{fat_time(core):08X}\n"
                f"CORE_PAGE0      equ #{core_page0:02X}\n")
        free_page = core_page0 + core_pages
        print(f"{cname}: {len(bits)} байт, {core_pages} стр. с #{core_page0:02X}")
    mod_names = [m.name.upper() for m in mods]
    if len(mods) > MAX_MODS or len(set(mod_names)) != len(mod_names) or name in mod_names:
        print(f"модули: без повторов, не сам ELF и не больше {MAX_MODS}")
        return 1
    table = ["; created by prep.py, do not edit", "ModFiles:"]
    labels = []
    for k, mod in enumerate(mods):
        body = mod.read_bytes() if mod.is_file() else b""
        if len(mod.stem) > 8 or mod.suffix.upper() != ".ELF" or not re.fullmatch(NAME_CHAR + "{1,8}", mod.stem.upper()) \
                or body[:4] != b"\x7fELF" or len(body) > MAX_MODULE:
            print(f"{mod}: не модуль ИМЯ.ELF (8.3: A–Z 0–9 _ -; файл ELF до 2 МБ)")
            return 1
        mod_pages = (len(body) + PAGE - 1) // PAGE
        if free_page + mod_pages > 0x100:
            print("ELF, ядро и модули не помещаются в страницы ОЗУ")
            return 1
        for i in range(mod_pages):
            part = build / f"mod{k}_{i}.bin"
            part.write_bytes(body[i * PAGE:(i + 1) * PAGE])
            blocks.append(f"Block = #c000, #{free_page + i:02x}, {rel(part)}")
        # описание — как AppFile в esp_app.asm: флаги 0, длина, время FAT, первая страница, имя
        table += ["        db 0",
                  f"        dd {len(body)}",
                  f"        dd #{fat_time(mod):08X}",
                  f"        db #{free_page:02X}",
                  f"        dw ModName{k}",
                  f"        dw ModName{k}End-ModName{k}"]
        labels += [f'ModName{k}:       db "{mod.name.upper()}",0', f"ModName{k}End:"]
        print(f"{mod.name.upper()}: {len(body)} байт, {mod_pages} стр. с #{free_page:02X}")
        free_page += mod_pages
    if not mods:
        (build / "app_mods.inc").unlink(missing_ok=True)   # от прошлой сборки с модулями
    else:
        (build / "app_mods.inc").write_text("\n".join(table + labels) + "\n", encoding="ascii")
        cfg += ("        DEFINE HAS_MODS\n"
                f"MOD_COUNT       equ {len(mods)}\n")
    if names:
        # Имя приложения — папка на флешке ESP (/SYSTEM/CACHE/ИМЯ) и ключ path.cfg; файлы данных —
        # 8.3 теми же знаками (app_data_files.cpp: validApp, validName).
        if not re.fullmatch(NAME_CHAR + "{1,8}", stem):
            print(f"{name}: имя приложения для файлов данных — 1–8 знаков A–Z 0–9 _ -")
            return 1
        for item in names:
            if not re.fullmatch(NAME_CHAR + "{1,8}(\\." + NAME_CHAR + "{1,3})?", item) \
                    or item == "WRITING.TMP" or item == name:
                print(f"{item}: не имя файла данных (8.3: A–Z 0–9 _ -; не WRITING.TMP и не сам ELF)")
                return 1
        if len(set(names)) != len(names) or len(names) > MAX_DATA:
            print(f"файлы данных: без повторов и не больше {MAX_DATA}")
            return 1
        if free_page > 0xFF:
            print("ELF, ядро и модули заняли все страницы ОЗУ: рабочей странице места нет")
            return 1
        for part in ("fat32.bin", "fat32-work.bin"):
            if not (FAT32_BUILD / part).is_file():
                print(f"{FAT32_BUILD / part}: нет драйвера FAT32 (shared/fat32/build.py)")
                return 1
            shutil.copyfile(FAT32_BUILD / part, build / part)
        blocks[0:0] = [f"Block = #c000, #{FAT32_PAGE:02x}, {rel(build / 'fat32.bin')}",
                       f"Block = #c000, #{FAT32_WORK_PAGE:02x}, {rel(build / 'fat32-work.bin')}"]
        listing = ",".join(f'"{item}",0' for item in names)
        cfg += ("        DEFINE HAS_DATA\n"
                f"DATA_COUNT      equ {len(names)}\n"
                f"        DEFINE DATA_NAMES db {listing}\n"
                f'        DEFINE DATA_FIRST83 db "{name83(names[0])}"\n'
                f'        DEFINE APP_STEM db "{stem}",0\n'
                f'        DEFINE APP_ELF83 db "{name83(name)}"\n'
                f"FAT32_PAGE      equ #{FAT32_PAGE:02X}\n"
                f"FAT32_WORK_PAGE equ #{FAT32_WORK_PAGE:02X}\n"
                f"SCRATCH_PAGE    equ #{free_page:02X}\n")
        print(f"файлы данных: {' '.join(names)} (папка {stem}); рабочая страница #{free_page:02X}")
    (build / "app_cfg.inc").write_text(cfg, encoding="ascii")
    ini = [
        f"Desc = {stem}",
        "Start = #8000",
        "Stack = #5fff",
        "Resident = #5B00",
        "Page3 = 0",
        "Clock = 2",
        "INT = 0",
        "Pager = 0",
        "",
        f"Block = #c000,  2,{rel(build / 'esp_app.bin')}",
    ] + blocks
    (build / "spgbld.ini").write_text("\n".join(ini) + "\n", encoding="ascii")
    print(f"{name}: {len(data)} байт, {pages} стр. с #{APP_PAGE0:02X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
