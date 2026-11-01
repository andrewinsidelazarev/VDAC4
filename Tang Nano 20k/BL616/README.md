# BL616 модуля Tang Nano 20K

BL616 — второй микроконтроллер на Tang Nano 20K. На VDAC4 у него своя прошивка:

- **Сопроцессор FT812** — то, что в настоящем FT812 делает микропрограмма J1: кольцо RAM_CMD,
  виджеты, текст шрифтами ПЗУ, картинки CMD_LOADIMAGE (JPEG, PNG), CMD_INFLATE. Регистры,
  дисплей-лист и RAM_G — в ядре FT812 (слот 1 flash Tang), BL616 читает и пишет их по SPI.
- **Свои 3D-команды VDAC4** `0xFFFFFFA0…AF`: сетки из RAM_G, отсечение, свет, z-буфер, текстуры,
  туман; кадр уходит в RAM_G битмапом. Версия 3D API — 3 ([`docs/3d-api-v3.md`](docs/3d-api-v3.md),
  заголовок для программ [`copro/vdac3d.h`](copro/vdac3d.h)).
- **Видео FT812** (с прошивки 31): CMD_MEDIAFIFO, CMD_VIDEOSTART, CMD_VIDEOFRAME — MJPEG AVI из
  медиа-FIFO; CMD_LOADIMAGE с OPT_FULLSCREEN вписывает картинку в 1024×768 (с 30) — для FTVIEW.
- **Кэш ядер** для ядер с карты Tang (Next, NES): ESP кладёт ядро во flash BL616, BL616 грузит
  его в ПЛИС по JTAG, а по просьбе ядра (F12) возвращает TS-Config.
- **Перезагрузка ПЛИС** при переходе между слотами flash Tang (TS-Config ↔ FT812), с прошивки 35 —
  в любой слот (команда BOOT ящика: заголовок битстрима с адресом перехода по JTAG и Reconfig;
  ядро видео — слот 3).
- **Обновление по сети** через ESP — USB нужен только для первой установки.

Готовый образ — [`firmware/vdac4_copro_bl616.bin`](firmware/vdac4_copro_bl616.bin), прошивка **38**
(141568 байтов, SHA-256 `308dd94aa917b99c6987bd2cf78efce30e5684cac7bb48ed1138995fe825d3f9`).

В прошивке 38: сдвиг битстрима по JTAG в ITCM и прямой записью в регистры GPIO, такт около 5 МГц по расчёту. Ядро Amiga (907 КБ) выходит на LINK за 2,1 с, на прошивке 36 было 9,8 с. **Прошивку 37 ставить нельзя:** ядро из кэша грузится у неё около 25 с, а ESP ждёт 15 с и сообщает «no core answered on LINK after the load».

В прошивке 37: два ребра на строку вместо трёх в быстром пути (те же границы, сверено с полным перебором на 388 370 треугольниках), подготовка кадра и TEXMESH в ITCM, константы горячего кода без чтения из flash. Кадры в модели побитно те же, что у 36. На плате: ROOMTEST 33 кадра/с без SSAA и 11 с SSAA (на 36 было 30 и 10,7).

В прошивке 36: быстрый путь текстур работает и с SSAA, и с цветом без квантования; старые пути MESH и SSAA без программного
`double` (`(float)int64`); горячий код в ITCM; `pump` не ждёт места в очереди ПЛИС; чтение на 53,3 МГц, если самопроверка
проходит на обеих фазах (контрольной суммы у чтения нет). На плате: ROOMTEST с SSAA 7 → 10,7 кадра/с, без SSAA 27 → 30.
Она стоит на плате с 27.10.2026. ESP показывает версию в слове CP_INFO: `C023xxxx`. Что менялось
в 25–35 — в начале [`app/main.c`](app/main.c).

## Что здесь

| Папка | Что |
|---|---|
| [`copro`](copro) | ядро сопроцессора на C: команды FT812 (`ft_copro.c`), картинки (`ft_image.c`), inflate (`ft_inflate.c`, tinfl из miniz 3.1.2), 3D (`ft_3d.c`) |
| [`copro/test`](copro/test) | проверка ядра на ПК против bt8xxemu: поток Weather 2, CMD_GRADIENT, CMD_TEXT, CMD_NUMBER, CMD_PROGRESS, JPEG/PNG |
| [`app`](app) | прошивка (bouffalo_sdk): связь с ПЛИС (`fpga_link.c`), главный цикл (`main.c`), кэш ядер (`core_cache.c`, `core_jtag.c`), обновление по сети (`fw_update.c`) |
| [`app/test`](app/test) | проверка на ПК: связь поверх модели ПЛИС, JTAG, кэш ядер, обновление по сети вместе с кодом ESP |
| [`firmware`](firmware) | готовый образ и файл для первой установки по USB |

## Установка

**Обновление по сети** — Evo включена, USB не нужен (`tang_tool.py` — в
[`ESP32 S3 Zero N8R8/Firmware/tools`](../../ESP32%20S3%20Zero%20N8R8/Firmware/tools)):

```
python tang_tool.py bl616 <IP ESP> vdac4_copro_bl616.bin
```

ESP проверяет SHA-256 и передаёт образ BL616 через ядро Tang. BL616 собирает его в памяти,
проверяет ещё раз, пишет во flash и перезапускается; `tang_tool` печатает новую версию.

**Первая установка по USB** — только на столе: Evo выключена, USB и Evo вместе не включать.
Образ ставится с адреса 0x40000 на место FPGA Companion; FPGA Partner Sipeed с 0x0 (USB-JTAG для
прошивки ПЛИС) остаётся. Tang подключить по USB с нажатой кнопкой UPDATE — BL616 появится как
«Bouffalo CDC DEMO» (COM-порт), затем в папке `firmware`:

```
BLFlashCommand.exe --interface=uart --baudrate=2000000 --port=COMn --chipname=bl616 --cpu_id= --config=flash_vdac4_copro.ini
```

`BLFlashCommand.exe` — из bouffalo_sdk (`tools/bflb_tools/bouffalo_flash_cube`). После записи
переподключить USB без кнопки. USB-клавиатура NextNano через FPGA Companion после этого не
работает: на VDAC4 клавиатура идёт от Evo через ESP.

## Сборка

Нужны bouffalo_sdk, компилятор T-Head (Xuantie GCC), make, ninja, cmake, python3:

```sh
export BL_SDK_BASE=<путь к bouffalo_sdk>
export PATH=<xuantie-gcc>/bin:$BL_SDK_BASE/tools/make:$BL_SDK_BASE/tools/ninja:$BL_SDK_BASE/tools/cmake/bin:$PATH
sh app/build.sh                 # → app/out/vdac4_copro_bl616.bin
```

bouffalo_sdk не собирает проект из пути с пробелами, поэтому `build.sh` копирует исходники в
каталог `WORK` (по умолчанию — во временный) и собирает там.

Проверки на ПК: `sh copro/test/run.sh` (нужны bt8xxemu и образ ПЗУ FT812 в `FT812_ROM_DIR`),
`sh app/test/run.sh`, `sh app/test/run_cache.sh`, `sh app/test/run_update.sh` (вместе с кодом
ESP из [`ESP32 S3 Zero N8R8/Firmware`](../../ESP32%20S3%20Zero%20N8R8/Firmware)).

## Лицензии

Файлы прошивки помечены SPDX `Apache-2.0`. tinfl — из [miniz](https://github.com/richgel999/miniz)
(MIT, [`copro/third_party/miniz/LICENSE`](copro/third_party/miniz/LICENSE)). Загрузка ПЛИС по JTAG
повторяет порядок FPGA-Companion MiSTle (Apache-2.0).
