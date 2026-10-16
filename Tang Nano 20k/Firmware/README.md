# Прошивка Tang Nano 20K платы VDAC4

Видеотракт платы: штатное видео ZX Evolution в режиме TS-Config (`M_SEL=0`) приходит на плату
двумя фазами мультиплексоров 2×8 и выводится по HDMI. Устройство — GW2AR-LV18QN88C8/I7.

Рабочая прошивка — **кадровый буфер `ts_fb_hdmi_top`**: 1280×720p50 от кварца Tang, картинка
на всю высоту экрана, 4:3 или 16:9, размер 100/95/90 %, сглаживание, экранное меню, AVI
InfoFrame, связь LINK с ESP32-S3, настройки во flash Tang. Описание —
[Docs/a3-fpga-framebuffer.md](../Docs/a3-fpga-framebuffer.md).

Готовое ядро — [`firmware/ts_fb_hdmi_top.fs`](firmware/ts_fb_hdmi_top.fs) (версия 1.3).

## Как обновить ядро Tang

**По сети через ESP** (прошивка ESP `vdac4-0.1.65` и новее, плата в Evo), из
`ESP32 S3 Zero N8R8/Firmware`:

```bash
python tools/tang_tool.py update <IP ESP> "../../Tang Nano 20k/Firmware/firmware/ts_fb_hdmi_top.fs"
```

ESP проверяет образ, пишет его во flash Tang и сверяет чтением; новое ядро загружается при
следующем включении Evo. Настройки Tang (последний сектор flash) при этом сохраняются.

**По USB** — Gowin Programmer, External Flash Mode, тот же `.fs`. Плата при этом вынута
из Evo: питание Evo и USB одновременно не включать.

## Две прошивки видеотракта

| Верхний модуль | Развёртка выхода | Задержка | Описание |
|---|---|---|---|
| `ts_fb_hdmi_top` | **1280×720p50 CEA-861** от кварца Tang (сборка HD = 0 — 720×576p50 точка в точку) | до ~1 кадра | [a3-fpga-framebuffer.md](../Docs/a3-fpga-framebuffer.md) |
| `ts_hdmi_top` | генлок: 896×640, 31,25 кГц, кадр 48,83 Гц (как у входа) | < 1 строки | [a3-fpga-video.md](../Docs/a3-fpga-video.md) |

Файлы кадрового буфера:

| Файл | Назначение |
|---|---|
| `rtl/ts_fb_hdmi_top.sv` | верхний модуль: выводы, PLL, SDRAM, flash, HDMI |
| `rtl/fb_clocking.sv` | rPLL от кварца 27 МГц: 371,25 МГц TMDS, 61,875 МГц SDRAM, 74,25 МГц пиксели |
| `rtl/ts_fb_core.sv` | тракт без примитивов и регистры LINK (карта — в комментарии) |
| `rtl/ts_mux_capture.sv`, `rtl/raster_detect.sv`, `rtl/active_bbox.sv` | приём двух фаз мультиплексора, определение растра Evo, границы картинки |
| `rtl/fb_capture.sv`, `rtl/async_fifo.sv`, `rtl/fb_cdc.sv` | окно захвата, FIFO и переход между доменами |
| `rtl/sdram_burst.sv`, `rtl/fb_core.sv` | контроллер встроенной SDRAM (пакеты по 8 слов), тройная буферизация кадров |
| `rtl/fb_scanout.sv`, `rtl/fb_line_ram.sv` | развёртка 720p: масштаб, ASPECT/SIZE, сглаживание; буфер строк |
| `rtl/hdmi_tx.sv`, `rtl/tmds_encoder.sv`, `rtl/tmds_serializer_gowin.sv` | HDMI: TMDS, острова данных, AVI InfoFrame |
| `rtl/fb_ui.sv`, `rtl/fb_osd.sv`, `rtl/fb_osd_rom.sv` | кнопки и UI_KEY, экранное меню (ПЗУ — `scripts/gen_osd_rom.py`) |
| `rtl/fb_link.sv` | LINK: ведомый QSPI для ESP32-S3 |
| `rtl/fb_flash.sv` | flash Tang: загрузка настроек при включении, мост для ESP |
| `rtl/fb_pattern.sv`, `rtl/legacy_palette.sv` | испытательная таблица, палитра RGB555 → RGB888 |
| `constraints/ts_fb_hdmi_top.cst`, `.sdc`, `scripts/gowin_fb.tcl` | выводы, ограничения, сборка в gw_sh |
| `tb/tb_ts_fb.sv`, `tb/sdram_model.sv`, `tb/spi_flash_model.sv`, `tb/tb_fb_flash.sv`, `tb/tb_hdmi_tx.sv` | тестбенчи: сквозной через декодер TMDS, модели SDRAM и flash |

Файлы `ft812_*`, `video_timing_480p`, `video_pll_x5` относятся к другой, ещё не законченной
ветке (FT812 на Tang) и в сборку не входят.

## Симуляция (Icarus Verilog 13)

Установка в отдельную папку, без прав администратора и изменения PATH:

```bash
py -3 tools/bootstrap_a3_rtl_tools.py --directory <папка> --package iverilog --env ucrt64
```

Из этой папки (`Tang Nano 20k/Firmware`):

```bash
sh scripts/run_fb_sim.sh          # кадровый буфер: сценарии в уменьшенном растре и flash
sh scripts/run_fb_sim.sh hd 0 1   # настоящий растр Evo → 720p50, 4:3, 95 % (~25 мин)
sh scripts/run_fb_sim.sh flash    # только flash Tang: настройки и мост
sh scripts/run_sim.sh             # генлок-тракт
```

Сценарий `full` подаёт на вход картинку: её делает `scripts/scr2hex.py` из экрана `.scr`
(в репозитории картинок нет). Поведенческие заглушки PLL, CLKDIV и сериализатора включаются
только макросом `SIMULATION`; для синтеза собираются примитивы Gowin.

## Синтез (Gowin 1.9.11.03 Education)

```bash
gw_sh scripts/gowin_fb.tcl       # кадровый буфер: impl/pnr/ts_fb_hdmi_top.fs
gw_sh scripts/gowin_genlock.tcl  # генлок: impl/pnr/ts_hdmi_top.fs
```

Ядро 1.3: 5974 LUT, 965 ALU, 4149 регистров, 7 BSRAM, 5,5 DSP, обе rPLL; Fmax пикселей
85,5 МГц при нужных 74,25, SDRAM 85,6 при 61,875.
