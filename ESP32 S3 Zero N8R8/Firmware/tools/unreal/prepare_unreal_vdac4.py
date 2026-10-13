"""Папка запуска Unreal с ESP платы VDAC4: E:\\zx\\unreal_x64\\vdac4.

Основной Unreal (E:\\zx\\unreal_x64) не трогается: копии его ROM, DLL, CMOS,
NVRAM и ini в подпапке, свой образ SD. В образ (копия wc.img основного) кладутся
WC Improved из репозитория (boot.$C и WC/), плагин VDAC4MP3.WMF в /WC/, модули
ESP, банк MIDI (MIDI.SF2 — TimGM6mb из E:\\zx\\esp\\soundfonts) и ПЗУ OPL4
(YRW801.ROM, если скачано в E:\\zx\\esp\\downloads) в /WC/ESP/ и тестовая
музыка из .test-build/unreal_media (если собрана).

Запуск: python tools/unreal/prepare_unreal_vdac4.py [--fresh-image] [--media IT,MOD]
                                                [--weather | --weather2] [--wcu [--wcu-stale]]
                                                [--ntp] [--online]
(--media — только эти папки unreal_media: в образе 100 МБ всё сразу не помещается;
--weather — заставка погоды VDAC4WTH.WMF первой в [PLUGINS], ScreenSaver=1 и
тестовый /zifi/zifi.ini: сеть и город для модели WIFI_INI и WEATHER_GET хоста ESP;
--weather2 — то же с заставкой для FT812 VDAC4WT2.WMF и эмулятором FT812
(TS_VDAC2=1; кадр FT812 — в отдельном окне Unreal «VDAC2 Emul», снимок —
Unreal.ft812_shot в unreal_vdac4.py). Сеть модели — переменные VDAC4_WIFI,
VDAC4_WIFI_MS, VDAC4_WIFI_OLD (см. vdac4_esp_host.cpp).
--wcu — плагин WC Update VDAC4WCU.WMF в /WC и в [PLUGINS] и тот же тестовый
/zifi/zifi.ini; хост ESP сверяет карту с настоящим GitHub (WC Improved, exe/ ветки
main). --wcu-stale — в образе один файл WC (STALE_FILE) отличается одним байтом при
той же длине: обновлятору есть что найти по SHA и что обновить.
--ntp — плагин NTP Time Sync VDAC4NTP.WMF (тип #06: при каждом запуске WC) в /WC и в
[PLUGINS] и тестовый /zifi/zifi.ini; с VDAC4_NTP=fail у хоста ESP NTP не отвечает, и
плагин выставляет часы ESP по RTC Evo (SYS_CLOCK, в журнале хоста). Флешку модуля
изображает папка из VDAC4_USB (кэш ресурсов музыки, прошивка 0.1.43).
--online — плагин VDAC4 Online Update VDAC4UPD.WMF в /WC и в [PLUGINS] и тестовый
/zifi/zifi.ini; хост ESP читает настоящие манифест и firmware.bin с GitHub (репозиторий
VDAC4), проверяет образ и SHA-256 и ничего не пишет. «Установленную» версию задаёт
VDAC4_FW_VERSION (по умолчанию vdac4-unreal — другая сборка).
"""
import argparse
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]          # Firmware (здесь .test-build/unreal_media)
NETWORK = ROOT.parent / "Network"                  # плагины WC сетевой части
SOUND = ROOT.parent / "Sound"                      # проигрыватель и модули ESP
UNREAL = Path(r"E:\zx\unreal_x64")
RUN = UNREAL / "vdac4"
BUILT = Path(r"E:\zx\unreal-vdac4\bin")
WC = Path(r"E:\zx\esp\wc-improved\exe")
MEDIA = ROOT / ".test-build" / "unreal_media"
BANK = Path(r"E:\zx\esp\soundfonts\TimGM6mb.sf2")
ROM = Path(r"E:\zx\esp\downloads\zxm-moonsound\hardware\firmware\YRW801-M - Yamaha - 1993.rom")
sys.path.insert(0, r"E:\zx\HMM2\Source\Tools")
from inject_hmm2_to_wc_img import Fat32Image  # noqa: E402


def ini(vdac2: bool = False) -> None:
    text = (UNREAL / "unreal.ini").read_bytes().decode("cp1251")

    def setkey(key: str, value: str) -> None:
        nonlocal text
        pattern = rf"(?m)^;?{re.escape(key)}=.*$"
        if re.search(pattern, text):
            text = re.sub(pattern, f"{key}={value}", text, count=1)
        else:
            text = text.replace("[MISC]\r\n", f"[MISC]\r\n{key}={value}\r\n", 1)

    setkey("TS_VDAC2", "1" if vdac2 else "0")   # FT812 — только заставке --weather2
    setkey("TS_ZIFI32", "VDAC4")
    setkey("SDCARD", "vdac4_sd.img")
    setkey("Image0", "vdac4_sd.img")     # загрузчик берёт boot.$C с IDE: тот же образ, как у HMM2
    setkey("diskA", "")
    setkey("snapshot", "")
    setkey("ConfirmExit", "0")
    setkey("ScrShot", "BMP")
    (RUN / "Unreal_vdac4.ini").write_bytes(text.encode("cp1251"))


def write_sfn(img: Fat32Image, folder: int, short: bytes, src: Path) -> None:
    """Файл под точным именем 8.3 без LFN: загрузчик ищет «BOOT    $C», а
    write_file выбрасывает «$» и пишет длинное имя с псевдонимом BOOT~1."""
    for entry in img.parse_dir(folder):
        if entry["short"] == short or entry["name"].upper() == "BOOTTMP.BIN":
            img.mark_deleted(folder, entry)
            img.free_chain(entry["cluster"])
    img.write_file(folder, "BOOTTMP.BIN", src)
    entry = next(e for e in img.parse_dir(folder) if e["name"].upper() == "BOOTTMP.BIN")
    slot = entry["lfn_start"] + entry["entries"] - 1
    for index, value in enumerate(short):
        img.write_dir_byte(folder, slot * 32 + index, value)


WEATHERS = {"ts": NETWORK / "Weather screensaver" / "build" / "vdac4" / "VDAC4WTH.WMF",
            "ft812": NETWORK / "Weather screensaver VDAC2" / "build" / "vdac4" / "VDAC4WT2.WMF"}
# zifi.ini стенда: сеть модели хоста ESP и город (погода настоящая, Open-Meteo).
ZIFI_INI = "ssid: unreal\r\npassword: test1234\r\ntime: 3\r\ncity: Kyiv\r\ncountry: UA\r\n"
WCU = NETWORK / "WC Update" / "build" / "vdac4" / "VDAC4WCU.WMF"
NTP = NETWORK / "NTP Time Sync" / "build" / "vdac4" / "VDAC4NTP.WMF"
UPD = NETWORK / "Online Update" / "build" / "vdac4" / "VDAC4UPD.WMF"
# Файл WC, который --wcu-stale портит: двоичный и в [PLUGINS] закомментирован — WC его при
# запуске не грузит. Текстовые файлы (.LIC, .txt) не годятся: из git они выходят с CRLF
# и отличаются от GitHub уже длиной.
STALE_FILE = "SCRSAVER.WMF"


def image(fresh: bool, media: set[str] | None = None, weather: str | None = None,
          wcu: bool = False, stale: bool = False, ntp: bool = False,
          online: bool = False) -> None:
    target = RUN / "vdac4_sd.img"
    if fresh or not target.exists():
        shutil.copy2(UNREAL / "wc.img", target)
    img = Fat32Image(target)
    root = img.root_cluster
    write_sfn(img, root, b"BOOT    $C ", WC / "boot.$C")
    wc_dir = img.ensure_dir(root, "WC")
    for item in sorted(WC.joinpath("WC").iterdir()):
        if item.is_file() and item.name.lower() != "wc.ini":
            if stale and item.name.upper() == STALE_FILE:
                # Та же длина, другой последний байт: отличие видно только по SHA.
                data = bytearray(item.read_bytes())
                data[-1] ^= 0x20
                temp = RUN / "stale.tmp"
                temp.write_bytes(bytes(data))
                img.write_file(wc_dir, item.name, temp)
                temp.unlink()
                continue
            img.write_file(wc_dir, item.name, item)
    # wc.ini выпуска: 80×25 (как на эскизе окна), плагин проигрывателя в [PLUGINS].
    # Строки wc.ini выпуска разделены одним CR.
    config = WC.joinpath("WC", "wc.ini").read_bytes().decode("cp866")
    newline = "\r\n" if "\r\n" in config else ("\r" if "\r" in config else "\n")
    lines = config.split(newline)
    # Панели — с корня при каждом запуске: иначе WC берёт пути и курсор из
    # NVRAM, и клавиши автопрогона попадают не туда.
    fixed = {"TextMode=": "0", "SavePaths=": "0", "SavePosition=": "0"}
    if weather:
        fixed["ScreenSaver="] = "1"     # заставка — через минуту без клавиш
    for key, value in fixed.items():
        lines = [key + value + line[len(key) + 1:] if line.startswith(key) else line
                 for line in lines]
    if "VDAC4MP3.WMF" not in lines:
        lines.insert(lines.index("FDI2FDD.WMF") + 1, "VDAC4MP3.WMF")
    saver = WEATHERS[weather].name if weather else None
    if saver and saver not in lines:
        # WC запускает первую в [PLUGINS] заставку (тип #02): раньше CLOCK.WMF.
        lines.insert(lines.index("VDAC4MP3.WMF") + 1, saver)
    if wcu and WCU.name not in lines:
        lines.insert(lines.index("VDAC4MP3.WMF") + 1, WCU.name)
    if ntp and NTP.name not in lines:
        lines.insert(lines.index("VDAC4MP3.WMF") + 1, NTP.name)
    if online and UPD.name not in lines:
        lines.insert(lines.index("VDAC4MP3.WMF") + 1, UPD.name)
    config = newline.join(lines)
    temp = RUN / "wc.ini.tmp"
    temp.write_bytes(config.encode("cp866"))
    img.write_file(wc_dir, "wc.ini", temp)
    temp.unlink()
    img.write_file(wc_dir, "VDAC4MP3.WMF", SOUND / "MP3 Player" / "build" / "VDAC4MP3.WMF")
    if wcu:
        img.write_file(wc_dir, WCU.name, WCU)
    if ntp:
        img.write_file(wc_dir, NTP.name, NTP)
    if online:
        img.write_file(wc_dir, UPD.name, UPD)
    if weather:
        img.write_file(wc_dir, saver, WEATHERS[weather])
    if weather or wcu or ntp or online:
        zifi = img.ensure_dir(root, "zifi")
        temp = RUN / "zifi.ini.tmp"
        temp.write_bytes(ZIFI_INI.encode("ascii"))
        img.write_file(zifi, "zifi.ini", temp)
        temp.unlink()
    esp = img.ensure_dir(wc_dir, "ESP")
    for name in ("PLAYER.ELF", "MP3.ELF", "AAC.ELF", "OGG.ELF", "FLAC.ELF", "TRACKER.ELF",
                 "MIDI.ELF", "OPL.ELF", "SAA.ELF", "TSFM.ELF"):
        img.write_file(esp, name, SOUND / "modules" / "out" / name)
    img.write_file(esp, "MIDI.SF2", BANK)
    if ROM.exists():
        img.write_file(esp, "YRW801.ROM", ROM)
    if MEDIA.exists():
        # /MUSIC — папки как в unreal_media, со вложенными (MP3/альбом, MWM/набор).
        def copy_tree(source: Path, cluster: int) -> None:
            for item in sorted(source.iterdir()):
                if item.is_dir():
                    copy_tree(item, img.ensure_dir(cluster, item.name))
                else:
                    img.write_file(cluster, item.name, item)

        music = img.ensure_dir(root, "MUSIC")
        for folder in sorted(p for p in MEDIA.iterdir() if p.is_dir()):
            if media is None or folder.name.upper() in media:
                copy_tree(folder, img.ensure_dir(music, folder.name))
    img.save()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fresh-image", action="store_true")
    parser.add_argument("--media", help="папки unreal_media через запятую (по умолчанию все)")
    parser.add_argument("--weather", action="store_true",
                        help="заставка погоды VDAC4WTH.WMF и тестовый /zifi/zifi.ini")
    parser.add_argument("--weather2", action="store_true",
                        help="заставка погоды для FT812 VDAC4WT2.WMF (TS_VDAC2=1)")
    parser.add_argument("--wcu", action="store_true",
                        help="плагин WC Update VDAC4WCU.WMF и тестовый /zifi/zifi.ini")
    parser.add_argument("--ntp", action="store_true",
                        help="плагин NTP Time Sync VDAC4NTP.WMF и тестовый /zifi/zifi.ini")
    parser.add_argument("--online", action="store_true",
                        help="плагин VDAC4 Online Update VDAC4UPD.WMF и тестовый /zifi/zifi.ini")
    parser.add_argument("--wcu-stale", action="store_true",
                        help=f"{STALE_FILE} в образе отличается одним байтом (есть что обновить)")
    args = parser.parse_args()
    RUN.mkdir(exist_ok=True)
    # WC — всегда свежий из репозитория (boot.$C и WC/ из exe/).
    import subprocess
    subprocess.run(["git", "-C", str(WC.parent), "pull", "--ff-only", "-q"], check=True)
    shutil.copytree(UNREAL / "rom", RUN / "rom", dirs_exist_ok=True)
    # CMOS и NVRAM — каждый раз заново: WC помнит в NVRAM позиции панелей.
    for name in ("bass.dll", "bt8xxemu.dll", "ft8xxemu.dll", "CMOS", "NVRAM"):
        shutil.copy2(UNREAL / name, RUN / name)
    for name in ("Unreal_vdac4.exe", "vdac4_esp_host.exe"):
        shutil.copy2(BUILT / name, RUN / name)
    ini(args.weather2)
    image(args.fresh_image,
          {name.strip().upper() for name in args.media.split(",")} if args.media else None,
          "ft812" if args.weather2 else ("ts" if args.weather else None),
          args.wcu, args.wcu_stale, args.ntp, args.online)
    print(RUN)


if __name__ == "__main__":
    main()
