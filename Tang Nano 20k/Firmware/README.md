# Прошивка Tang Nano 20K платы VDAC4

Видеотракт платы: штатное видео ZX Evolution в режиме TS-Config (`M_SEL=0`) приходит на плату
двумя фазами мультиплексоров 2×8 и выводится по HDMI. Устройство — GW2AR-LV18QN88C8/I7.

Во flash Tang два ядра (multiboot):

| Слот flash | Ядро | Файл | Версия |
|---|---|---|---|
| 0 (0x000000) — при включении | **кадровый буфер TS-Config** `ts_fb_hdmi_top` | [`firmware/ts_fb_hdmi_top.fs`](firmware/ts_fb_hdmi_top.fs) | 1.32 (`0x01010120`) |
| 1 (0x100000) | **FT812** (то же ядро с видеоконтроллером FT812) | [`firmware/ts_fb_slot1.fs`](firmware/ts_fb_slot1.fs) | 1.29 (`0x0201011D`) |

Ядро слота 0 само уходит в слот 1, когда программа Evo обращается к FT812 (переход разрешает ESP),
и возвращается обратно по сбросу. Ядра для запуска из кэша BL616 (ZX Next, NES) — в [`../Cores`](../Cores).

**Ядро слота 0** — 1280×720p50 от кварца Tang, картинка на всю высоту экрана, 4:3 или 16:9, размер
100/95/90 %, сглаживание, экранное меню, AVI InfoFrame, связь LINK с ESP32-S3, настройки во flash
Tang, мост к microSD Tang для ESP. **FRAME BLEND** (с 1.32, пункт меню Tang на ESP, по умолчанию
выключен): вывод среднего двух последних кадров Evo — демо с мерцанием кадров A/B дают ровный цвет.
Описание — [Docs/a3-fpga-framebuffer.md](../Docs/a3-fpga-framebuffer.md).

**Ядро слота 1** — то же с видеоконтроллером FT812 (регистры, дисплей-лист, RAM_G, отрисовка);
командный сопроцессор FT812 исполняет BL616 модуля Tang. Описание —
[Docs/ft812-emulation.md](../Docs/ft812-emulation.md).

## Как обновить ядро Tang

**По сети через ESP** (прошивка ESP `vdac4-0.1.99` и новее, плата в Evo), из
`ESP32 S3 Zero N8R8/Firmware`:

```bash
python tools/tang_tool.py update <IP ESP> "../../Tang Nano 20k/Firmware/firmware/ts_fb_hdmi_top.fs"
python tools/tang_tool.py update --slot 1 <IP ESP> "../../Tang Nano 20k/Firmware/firmware/ts_fb_slot1.fs"
```

ESP проверяет образ, пишет его во flash Tang и сверяет чтением; новое ядро загружается при
следующем включении Evo. Настройки Tang (последний сектор flash) при этом сохраняются. Пока
переписывается слот 1, переход в него запрещён.

**По USB** — Gowin Programmer, External Flash Mode, `firmware/ts_fb_hdmi_top.fs` по адресу 0. Плата
при этом вынута из Evo: питание Evo и USB одновременно не включать. Слот 1 — только через ESP.

## Две прошивки видеотракта

| Верхний модуль | Развёртка выхода | Задержка | Описание |
|---|---|---|---|
| `ts_fb_hdmi_top` | **1280×720p50 CEA-861** от кварца Tang (сборка HD = 0 — 720×576p50 точка в точку) | до ~1 кадра | [a3-fpga-framebuffer.md](../Docs/a3-fpga-framebuffer.md) |
| `ts_hdmi_top` | генлок: 896×640, 31,25 кГц, кадр 48,83 Гц (как у входа) | < 1 строки | [a3-fpga-video.md](../Docs/a3-fpga-video.md) |

Файлы кадрового буфера:

| Файл | Назначение |
|---|---|
| `rtl/ts_fb_hdmi_top.sv` | верхний модуль: выводы, PLL, SDRAM, flash, HDMI; параметры сборки слотов 0 и 1 |
| `rtl/fb_clocking.sv` | rPLL от кварца 27 МГц: 371,25 МГц TMDS, 61,875 МГц SDRAM, 74,25 МГц пиксели |
| `rtl/ts_fb_core.sv` | тракт без примитивов и регистры LINK (карта — в комментарии) |
| `rtl/ts_mux_capture.sv`, `rtl/raster_detect.sv`, `rtl/active_bbox.sv` | приём двух фаз мультиплексора, определение растра Evo, границы картинки |
| `rtl/fb_capture.sv`, `rtl/async_fifo.sv`, `rtl/fb_cdc.sv` | окно захвата, FIFO и переход между доменами |
| `rtl/sdram_burst.sv`, `rtl/fb_core.sv` | контроллер встроенной SDRAM (пакеты по 8 слов, пачки до 64 слов), буферы кадров (3, с FRAME BLEND — 4) |
| `rtl/fb_avg_rom.sv`, `rtl/fb_avg_tab.svh` | FRAME BLEND: таблица среднего двух цветов по яркости (`scripts/gen_avg_tab.py`) |
| `rtl/fb_scanout.sv`, `rtl/fb_line_ram.sv` | развёртка 720p: масштаб, ASPECT/SIZE, сглаживание; буфер строк |
| `rtl/hdmi_tx.sv`, `rtl/tmds_encoder.sv`, `rtl/tmds_serializer_gowin.sv` | HDMI: TMDS, острова данных, AVI InfoFrame |
| `rtl/fb_ui.sv`, `rtl/fb_osd.sv`, `rtl/fb_osd_rom.sv`, `rtl/fb_ovl.sv` | кнопки и UI_KEY, экранное меню (ПЗУ — `scripts/gen_osd_rom.py`), оверлеи ESP |
| `rtl/fb_link.sv` | LINK: ведомый QSPI для ESP32-S3 |
| `rtl/fb_flash.sv` | flash Tang: загрузка настроек при включении, мост для ESP |
| `rtl/fb_sdspi.sv`, `rtl/sd_bridge.sv` | мост к microSD Tang для ESP (ядра Next/NES, файлы карты) |
| `rtl/ft_core.sv`, `rtl/ft_*.sv` | FT812 слота 1: регистры, RAM_G в SDRAM, обход дисплей-листа и отрисовка |
| `rtl/cp_link.sv`, `rtl/cp_spi.sv` | связь с BL616 (сопроцессор FT812, кэш ядер) |
| `rtl/fb_pattern.sv`, `rtl/legacy_palette.sv` | испытательная таблица, палитра RGB555 → RGB888 |
| `constraints/ts_fb_hdmi_top.cst`, `.sdc`, `scripts/gowin_fb.tcl`, `scripts/gowin_fb_slot1.tcl` | выводы, ограничения, сборка слотов 0 и 1 в gw_sh |
| `tb/tb_ts_fb.sv`, `tb/sdram_model.sv`, `tb/spi_flash_model.sv`, `tb/sd_card_model.sv`, `tb/tb_ft_*.sv` | тестбенчи: сквозной через декодер TMDS, модели SDRAM, flash и SD, FT812 |

Файлы `ft812_core`, `ft812_spi`, `video_timing_480p`, `video_pll_x5` — от ранней ветки FT812 и в
сборку не входят.

## Симуляция (Icarus Verilog 13)

Установка в отдельную папку, без прав администратора и изменения PATH:

```bash
py -3 tools/bootstrap_a3_rtl_tools.py --directory <папка> --package iverilog --env ucrt64
```

Из этой папки (`Tang Nano 20k/Firmware`):

```bash
sh scripts/run_fb_sim.sh              # кадровый буфер: сценарии в уменьшенном растре, flash, LINK, FRAME BLEND
sh scripts/run_fb_sim.sh hd 1 1       # настоящий растр Evo → 720p50, 16:9, 95 % (~1 ч)
sh scripts/run_fb_sim.sh hd 1 1 avg   # то же с FRAME BLEND
sh scripts/run_fb_sim.sh flash        # только flash Tang: настройки и мост
sh scripts/run_sim.sh                 # генлок-тракт
```

Сценарий `full` подаёт на вход картинку: её делает `scripts/scr2hex.py` из экрана `.scr`
(в репозитории картинок нет). Поведенческие заглушки PLL, CLKDIV и сериализатора включаются
только макросом `SIMULATION`; для синтеза собираются примитивы Gowin.

## Синтез (Gowin 1.9.11.03 Education)

```bash
gw_sh scripts/gowin_fb.tcl        # слот 0: impl/pnr/ts_fb_hdmi_top.fs
gw_sh scripts/gowin_fb_slot1.tcl  # слот 1 (FT812)
gw_sh scripts/gowin_genlock.tcl   # генлок: impl/pnr/ts_hdmi_top.fs
```

Ядро 1.32 (слот 0): логика 7 298 (36 %), BSRAM 30/46; Fmax `clk_mem` 119,8 МГц при нужных 61,875,
пикселей 102,8 при 74,25; нарушенных путей нет. Ядро 1.29 (слот 1): логика 94 %, BSRAM 44/46.
