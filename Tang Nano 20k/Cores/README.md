# Ядра Tang Nano 20K

Tang Nano 20K на VDAC4 работает с несколькими ядрами ПЛИС. Три живут во flash самой Tang, остальные —
на её microSD и грузятся по выбору из Wild Commander.

| Ядро | Где хранится | Версия | Файлы |
|---|---|---|---|
| **TS-Config** — картинка Evo на HDMI 720p, меню Tang, FRAME BLEND | flash Tang, слот 0 | 1.32 | [`Firmware/firmware/ts_fb_hdmi_top.fs`](../Firmware/firmware/ts_fb_hdmi_top.fs) |
| **FT812** — видеоускоритель для программ Evo, вывод 1366×768 | flash Tang, слот 1 | 1.41 | [`Firmware/firmware/ts_fb_slot1.fs`](../Firmware/firmware/ts_fb_slot1.fs) |
| **Видео** — ролики MJPEG плагина FTVIEW4 декодирует ПЛИС | flash Tang, слот 3; слой ESP — карта Evo, `/WC/ESP` | 1.43, слой ESP 0.9 | [`Firmware/firmware/ts_fb_video.bin`](../Firmware/firmware/ts_fb_video.bin), [`VIDEO/esp/dist/VIDEO.ELF`](VIDEO/esp/dist/VIDEO.ELF) |
| **ZX Spectrum Next** (на основе NextNano) — с часами Evo и картой Evo как второй SD | карта Tang, `CORES` | 0.40, слой ESP 0.10 | [`SD/CORES/NEXT.BIN`](SD/CORES/NEXT.BIN), [`NEXT.ELF`](SD/CORES/NEXT.ELF) |
| **NES / Денди** (на основе NESTang) | карта Tang, `CORES` | 0.10, слой ESP 0.9 | [`SD/CORES/NES.BIN`](SD/CORES/NES.BIN), [`NES.ELF`](SD/CORES/NES.ELF) |
| **Amiga / AGA** (на основе NanoMig) — 68020, 2 МиБ chip + 4 МиБ fast, дискета DF0 | карта Tang, `CORES` | 0.4-debug5 | [`SD/CORES/AMIGA.BIN`](SD/CORES/AMIGA.BIN), [`AMIGA.ELF`](SD/CORES/AMIGA.ELF) |

`.BIN` — само ядро. `.ELF` — его слой на ESP: клавиатура и мышь Evo, звук на ЦАП ESP, сохранения на
карты, выход в WC. Исходники TS-Config, FT812 и ядра видео — в [`../Firmware`](../Firmware), Next,
NES и Amiga — здесь, в [`Next`](Next), [`NES`](NES) и [`AMIGA`](AMIGA), слой ESP ядра видео — в [`VIDEO/esp`](VIDEO/esp)
(описание ядра — [`../Docs/video-core.md`](../Docs/video-core.md)). Слой ядра видео — не на карте
Tang: битстрим ядра — во flash Tang, а `VIDEO.ELF` лежит в `/WC/ESP` карты Evo рядом с модулями плеера,
в ESP его отдаёт плагин [FTVIEW4](../FTVIEW4).

## Что нужно

- Прошивка ESP **vdac4-0.1.105**: мост к карте Tang, кэш ядер, запуск слоёв `.ELF`, ядро видео из
  слота 3, пункт FT SIZE меню Tang (размер картинки ядра FT812 отдельно от ядер 720p). С 0.1.105
  приложение ESP, брошенное своей программой Z80 (сбой, сброс Evo), не держит кэш модулей, а
  FT-приложение выгружается само, когда Tang вернулся в слот 0 и M_SEL = 0: прежде такое
  приложение не давало места `VIDEO.ELF`, и FTVIEW4 молча играл ролик путём FT812.
- Прошивка BL616 модуля Tang **35** — [`../BL616`](../BL616): её кэш ядер грузит ядро с карты в ПЛИС,
  по F12 возвращает TS-Config, ядро видео поднимает из слота 3 flash Tang.
- Плагин Wild Commander **VDAC4 Cores** 0.13 —
  [`ESP32 S3 Zero N8R8/Network/VDAC4 Cores`](../../ESP32%20S3%20Zero%20N8R8/Network/VDAC4%20Cores): меню ядер,
  часы Evo для Next, сектора карты Evo для Next, NES из меню — сразу в меню F1 ядра.
- Для ядра видео — плагин Wild Commander **FTVIEW4** (сборка bb34) — [`../FTVIEW4`](../FTVIEW4):
  `build/FTVIEW4.WMF` — в `/WC` карты Evo, `VIDEO.ELF` — в `/WC/ESP`.
- Для Amiga — свой Kickstart на карте Tang: `AMIGA/KICK31.ROM`, 512 КиБ. ROM не выкладывается. Подробности —
  в [`AMIGA/README.md`](AMIGA/README.md).
- Для Next — NextZXOS на той же карте Tang (дистрибутив `sn-complete-24.11`) и правленые ROM
  (ниже, шаг 3).

## Установка

1. **Ядра во flash Tang** — по сети через ESP, Evo включён, USB не нужен (`tang_tool.py` —
   в [`ESP32 S3 Zero N8R8/Firmware/tools`](../../ESP32%20S3%20Zero%20N8R8/Firmware/tools)):

   ```
   python tang_tool.py update <IP ESP> ts_fb_hdmi_top.fs
   python tang_tool.py update --slot 1 <IP ESP> ts_fb_slot1.fs
   python tang_tool.py update --slot 3 <IP ESP> ts_fb_video.bin
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
| Ctrl | Symbol Shift; двоеточие — Ctrl + Z (или клавиша `\`), кавычка — `'`, косая черта — `/` |

В NextZXOS карта Tang — диск C:, карта Evo — следующий диск (обычно D:), например
`SAVE "D:/downloads/t.bas"`.

| NES | |
|---|---|
| Стрелки, X / Z, Пробел / Enter | крестовина, A / B, Select / Start |
| F1 | меню выбора картриджа с карт Tang и Evo; при запуске NES из меню ядер оно открыто сразу и не закрывается, пока не выбран `.nes` |
| F4 | сброс |
| F5 / F6 | снимок игры / загрузка снимка (`.sst` рядом с картриджем) |
| F7 | пропорции 4:3 / 16:9 |
| F8 (в меню F1) | размер картинки: AUTO (как SIZE в TS-Config) → 100 → 95 → 90 % |
| F12 | выход в WC; сохранения battery — в `.sav` рядом с картриджем |

| Amiga | |
|---|---|
| Ctrl+Alt+F4 | тёплый сброс |
| F12 | выход в WC |

Джойстик Kempston работает в Next и NES. Мышь Kempston работает в Next.

## Проверено на плате

2026-10-31, ESP 0.1.105-heat2, BL616 35:
- **Amiga 0.4-debug5**: Test Kit 1.21 загружается с DF0 (108 секторов), F1 Memory — 3 круга без ошибок,
  картинка под нагрузкой чистая. Мышь, джойстик, звук, игры и Workbench ещё не проверены.

2026-10-29, ESP 0.1.104, BL616 35, TS-Config 1.32, FTVIEW4 bb34, VIDEO.ELF 0.9:
- **Видео 1.43** — деблокер и зерно: квадраты сжатия на ролике с малым потоком сглажены (фильтр
  окна края блока, решение по яркости — уходят и цветовые квадраты 16×16), тёмные заливки — без
  ступеней RGB565 (синий шум, каждый кадр на новом месте). Описание — [`../Docs/video-core.md`](../Docs/video-core.md).

2026-10-28, ESP 0.1.104, BL616 35, TS-Config 1.32, FTVIEW4 bb34:
- **Видео 1.42**, VIDEO.ELF 0.8–0.9: меню файлов и панель плеера — в точках 720p, шрифты чёткие;
  PgUp/PgDn — соседний ролик тем же ядром, без возврата в слот 0; русские имена в меню — целиком.
- **FT812 1.41** — вывод 1366×768 приняли телевизор LG, монитор ASUS по HDMI и Dell 16:10 через
  переходник VGA; шрифты просмотрщика текстов WC больше не режутся; пункт FT SIZE работает.

2026-10-27, ESP 0.1.103, BL616 35, TS-Config 1.32:
- **Видео 1.39**, VIDEO.ELF 0.5, FTVIEW4: ролики 4:3 и 16:9 растянуты по ASPECT слота 0, ASPECT 4:3
  сохраняется после выхода.

2026-10-27, ESP 0.1.100, TS-Config 1.32, плагин VDAC4 Cores 0.12:
- **Next 0.40**, NEXT.ELF 0.10:
  - время Evo в NextZXOS верное;
  - карта Evo — диск в NextZXOS и в Browser;
  - за сеанс прочитано 1745 секторов, все вовремя, ошибок нет;
  - запись файла из NextBASIC (7 секторов) — без отказов.

2026-10-27, ESP 0.1.100, TS-Config 1.32:
- **Next 0.37** — без сиреневой полосы. NextZXOS, браузер SD, запуск `.nex`, загрузка программ — без
  глюков. Та же правка записи в банк 5 со счётчиками на плате: 3,4 млн проверенных чтений процессора и
  174 млн чтений ULA, ошибок 0.

2026-10-26, ESP 0.1.100, TS-Config 1.32:
- **Next 0.18** грузится с первого раза, файлы читаются. Частота меняется F8 и стрелками меню и
  сохраняется после F4 и после выхода с повторным запуском.
- **NES 0.8**: сохранения F5/F6, F1, Kempston.

Мышь и громкость звука в Next ещё не проверены.

## Лицензии

Next, NES и Amiga основаны на проектах под GPL: [NextNano](https://github.com/RetroSilicon/NextNano),
[NESTang](https://github.com/nand2mario/nestang) и [NanoMig](https://github.com/MiSTle-Dev/NanoMig). Их исходники с изменениями для VDAC4 лежат здесь же,
текст лицензии — [`Next/LICENSE`](Next/LICENSE), [`NES/COPYING`](NES/COPYING), [`AMIGA/COPYING`](AMIGA/COPYING). ROM NextZXOS не
выкладываются: правка накладывается на исходные файлы дистрибутива. Kickstart тоже не выкладывается.
