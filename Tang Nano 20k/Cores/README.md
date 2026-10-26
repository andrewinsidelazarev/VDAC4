# Ядра Tang Nano 20K

Tang Nano 20K на VDAC4 работает с несколькими ядрами ПЛИС. Два живут во flash самой Tang, остальные —
на её microSD и грузятся по выбору из Wild Commander.

| Ядро | Где хранится | Версия | Файлы |
|---|---|---|---|
| **TS-Config** — картинка Evo на HDMI 720p, меню Tang, FRAME BLEND | flash Tang, слот 0 | 1.32 | [`Firmware/firmware/ts_fb_hdmi_top.fs`](../Firmware/firmware/ts_fb_hdmi_top.fs) |
| **FT812** — видеоускоритель для программ Evo | flash Tang, слот 1 | 1.29 | [`Firmware/firmware/ts_fb_slot1.fs`](../Firmware/firmware/ts_fb_slot1.fs) |
| **ZX Spectrum Next** (на основе NextNano) | карта Tang, `CORES` | 0.18, слой ESP 0.7 | [`SD/CORES/NEXT.BIN`](SD/CORES/NEXT.BIN), [`NEXT.ELF`](SD/CORES/NEXT.ELF) |
| **NES / Денди** (на основе NESTang) | карта Tang, `CORES` | 0.8, слой ESP 0.6 | [`SD/CORES/NES.BIN`](SD/CORES/NES.BIN), [`NES.ELF`](SD/CORES/NES.ELF) |

`.BIN` — само ядро. `.ELF` — его слой на ESP: клавиатура и мышь Evo, звук на ЦАП ESP, сохранения на
карты, выход в WC. Исходники TS-Config и FT812 — в [`../Firmware`](../Firmware), Next и NES — здесь,
в [`Next`](Next) и [`NES`](NES).

## Что нужно

- Прошивка ESP **vdac4-0.1.100**: мост к карте Tang, кэш ядер, запуск слоёв `.ELF`.
- Прошивка BL616 модуля Tang **24** — [`../BL616`](../BL616): её кэш ядер грузит ядро с карты в ПЛИС,
  а по F12 возвращает TS-Config.
- Плагин Wild Commander **VDAC4 Cores** 0.10 —
  [`ESP32 S3 Zero N8R8/Network/VDAC4 Cores`](../../ESP32%20S3%20Zero%20N8R8/Network/VDAC4%20Cores): меню ядер.
- Для Next — NextZXOS на той же карте Tang (дистрибутив `sn-complete-24.11`) и правленые ROM
  (ниже, шаг 3).

## Установка

1. **Ядра во flash Tang** — по сети через ESP, Evo включён, USB не нужен (`tang_tool.py` —
   в [`ESP32 S3 Zero N8R8/Firmware/tools`](../../ESP32%20S3%20Zero%20N8R8/Firmware/tools)):

   ```
   python tang_tool.py update <IP ESP> ts_fb_hdmi_top.fs
   python tang_tool.py update --slot 1 <IP ESP> ts_fb_slot1.fs
   ```

   Новое ядро слота 0 начинает работать после выключения и включения питания.
2. **Карта Tang** — скопировать папку [`SD/CORES`](SD/CORES) в корень microSD Tang. Можно кардридером,
   можно из WC: F10 → «VDAC4 VFS» → Enter (SD2 = карта Tang с записью). Ещё один способ — по сети:
   `python tang_tool.py sd-put <IP ESP> NEXT.BIN CORES/NEXT.BIN`.
3. **ROM NextZXOS для Next** — меню, F8 и NMI берут общий выбор частоты ядра, и он сохраняется после
   F4 и повторного запуска. Исходные ROM берутся из `machines/next` дистрибутива:

   ```
   python Next/rom-patch/patch_rom.py Next/rom-patch/enNextZX.patch.json enNextZX.rom enNextZX.new.rom
   python Next/rom-patch/patch_rom.py Next/rom-patch/enNextMf.patch.json enNextMf.rom enNextMf.new.rom
   ```

   Готовые файлы кладутся на карту Tang вместо исходных: `machines/next/enNextZX.rom` и
   `machines/next/enNextMf.rom`. Без правки Next тоже работает, но частота после F4 сбрасывается.
4. **Запуск** — в WC плагин VDAC4 Cores, выбрать ядро. Первый запуск копирует ядро с карты в кэш
   BL616, это занимает до минуты. Дальше ядро грузится секунд за десять.

## Клавиши

| Next | |
|---|---|
| F12 | выход в WC, выбор частоты записывается в `CORES/next.cfg` |
| F4 | сброс (частота сохраняется) |
| F1 | полный сброс через TBBlue |
| F8 | частота процессора 3,5 / 7 / 14 / 28 МГц |
| F9 | NMI |

| NES | |
|---|---|
| Стрелки, X / Z, Пробел / Enter | крестовина, A / B, Select / Start |
| F1 | меню выбора картриджа с карт Tang и Evo |
| F4 | сброс |
| F5 / F6 | снимок игры / загрузка снимка (`.sst` рядом с картриджем) |
| F7 | пропорции 4:3 / 16:9 |
| F12 | выход в WC; сохранения battery — в `.sav` рядом с картриджем |

Джойстик Kempston работает в обоих ядрах. Мышь Kempston работает в Next.

## Проверено на плате

2026-10-26, ESP 0.1.100, TS-Config 1.32:
- **Next 0.18** грузится с первого раза, файлы читаются. Частота меняется F8 и стрелками меню и
  сохраняется после F4 и после выхода с повторным запуском.
- **NES 0.8**: сохранения F5/F6, F1, Kempston.

Мышь и громкость звука в Next ещё не проверены.

## Лицензии

Next и NES основаны на проектах под GPL-3.0: [NextNano](https://github.com/RetroSilicon/NextNano) и
[NESTang](https://github.com/nand2mario/nestang). Их исходники с изменениями для VDAC4 лежат здесь же,
текст лицензии — [`Next/LICENSE`](Next/LICENSE), [`NES/COPYING`](NES/COPYING). ROM NextZXOS не
выкладываются: правка накладывается на исходные файлы дистрибутива.
