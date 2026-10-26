# NES / Денди на VDAC4

Ядро — [NESTang](https://github.com/nand2mario/nestang) (commit `5b24a710`) с обвязкой платы VDAC4. Готовые
файлы — [`../SD/CORES/NES.BIN`](../SD/CORES/NES.BIN) (0.8, VERSION `04010008`) и
[`NES.ELF`](../SD/CORES/NES.ELF) (0.6).

## Что здесь

| Папка | Что |
|---|---|
| [`upstream/src`](upstream/src) | процессор 6502, PPU, APU, мапперы и контроллер SDRAM из NESTang |
| [`fpga`](fpga) | обвязка VDAC4: верхний модуль, LINK с ESP, меню на экране, мост к картам для снимков и сохранений, масштаб 4:3/16:9 |
| [`esp`](esp) | слой ESP `NES.ELF`: выбор картриджа с карт Tang и Evo, клавиатура и Kempston, звук на ЦАП ESP, `.sav` и снимки `.sst` |
| [`build.tcl`](build.tcl) | сборка в Gowin EDA |

## Возможности

- Картриджи `.nes` с карты Tang и с карты Evo (F1 — меню выбора, игра на паузе).
- Сохранения battery в `.sav` рядом с картриджем на той же карте, снимки всей машины F5/F6 в `.sst`.
- Мапперы NESTang. Снимки поддерживают MMC1, MMC2, MMC3, MMC4, 28, 34, 66, 71.
- Kempston всеми восемью битами: огонь / B Sega → A, A Sega → B, C → Select, START → Start.
- Звук на ЦАП ESP, картинка на HDMI 720p, пропорции 4:3 или 16:9 (F7).

## Сборка

Ядро: Gowin EDA 1.9.11. Рядом с `build.tcl` нужны папки `fpga` и `upstream/src` и файл `background.txt`.
Команда `gw_sh build.tcl`, результат — `impl/pnr/nesVdac4.bin` (это `NES.BIN`). Слой ESP: ESP-IDF 6.0,
`esp/tools/build.ps1`.
