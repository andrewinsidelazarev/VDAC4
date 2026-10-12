# VDAC4 Media Player — звук платы VDAC4

Медиапроигрыватель для ZX Evolution (TS-Config) с платой VDAC4. Z80 только
читает файл с SD-карты и отдаёт его по SPI модулю ESP32-S3 Zero N8R8 на плате;
декодирует ESP и выводит звук через ЦАП PCM5102 (I2S). Поэтому Evo играет
FLAC 24 бит / 96 кГц, Ogg Vorbis, AAC, MIDI с банком SoundFont, трекерные
модули, музыку микросхем AY, YM2203, OPL и SAA1099 и интернет-радио по Wi-Fi.

Части:

- **плагин Wild Commander** [`VDAC4MP3.WMF`](MP3%20Player/build/VDAC4MP3.WMF)
  (Z80, [`MP3 Player`](MP3%20Player)): окно проигрывателя, чтение файла,
  передача на ESP, клавиши, переход к следующему файлу панели;
- **модули ESP** [`modules/out/*.ELF`](modules/out): сам проигрыватель
  `PLAYER.ELF` и декодеры форматов; прошивка ESP грузит их с карты Evo по
  надобности.

## Что нужно

- плата VDAC4 с ESP32-S3 Zero N8R8 и прошивкой VDAC4 из
  [`../Firmware`](../Firmware) (лучше последней; радио — с `vdac4-0.1.32`, кэш на
  флешке — с `vdac4-0.1.43`);
- [Wild Commander Improved](https://github.com/andrewinsidelazarev/Wild-Commander-Improved)
  v1.11i от 11 октября 2026 или новее: у плагина заголовок WMF формата `#11`,
  старое ядро WC его не загрузит;
- для радио — Wi-Fi на ESP (`/zifi/zifi.ini`, см. [`../Network`](../Network)).

## Форматы

44 расширения в одном плагине. Расширения длиннее трёх букв (`.flac`, `.midi`)
WC сравнивает по полному имени файла.

| Что | Расширения | Модуль ESP | Чем и как играется |
|---|---|---|---|
| MP3 | `MP3` | `MP3.ELF` | MPEG-1/2/2.5 Layer III (Helix MP3), потоком; метки ID3v2, обложка |
| WAV | `WAV` | `MP3.ELF` | PCM, float, A-law, µ-law; потоком |
| AAC | `AAC` | `AAC.ELF` | поток ADTS: AAC-LC, HE-AAC v1 (Helix AAC); HE-AAC v2 — моно; MP4/M4A — нет |
| Ogg Vorbis | `OGG`, `OGA` | `OGG.ELF` | stb_vorbis, потоком; комментарии Vorbis, обложка; Opus — нет |
| FLAC | `FLAC`, `FLA` | `FLAC.ELF` | foxen-flac, 4–24 бит, в том числе 96 кГц; 3–8 каналов сводятся в стерео; Vorbis Comment, обложка |
| Трекерные модули | `MOD`, `MO`, `XM`, `S3M`, `IT` | `TRACKER.ELF` | libxmp; модули других трекеров под этими расширениями (MED, MultiTracker, упакованные Amiga) узнаются по содержимому; файл до 6 МиБ, IT — любой длины |
| MIDI | `MID`, `MIDI`, `RMI`, `KAR` | `MIDI.ELF` | TinySoundFont с банком SoundFont 2 `MIDI.SF2` |
| Музыка OPL | `VGM`, `VGZ`, `DRO`, `IMF`, `WLF` | `OPL.ELF` | ymfm: Y8950, YM3526, YM3812, YMF262, YM2413, YMF278B (OPL4 — с ПЗУ `YRW801.ROM`) |
| MoonBlaster (MSX MoonSound) | `MWM`, `MFM` | `OPL.ELF` | MoonBlaster 1.4: MWM — таблицы волн OPL4 (`YRW801.ROM` и набор волн `*.MWK` песни), MFM — FM |
| Набор волн MoonBlaster | `MWK` | — | не играется: его берёт песня MWM, плагин такие файлы пропускает |
| AY и TurboSound | `AS0`, `ASC`, `FTC`, `GTR`, `PSC`, `PSM`, `PT1`, `PT2`, `PT3`, `SQT`, `ST3`, `STC`, `STP`, `TS` | `TSFM.ELF` | проигрыватели AYFly/ZXTune, звук AY-3-8910/YM2149 — ayumi; TurboSound — два AY |
| TurboSound FM | `TFC`, `TFD` | `TSFM.ELF` | два YM2203 (ymfm) |
| ZXTune MTC | `MTC` | `TSFM.ELF` | многодорожечные: AY, TFC и TFD вместе |
| SAA1099 (SAM Coupé) | `SAA`, `COP` | `SAA.ELF` | E-Tracker, SAASound: шесть стереоканалов, шум, огибающие |
| Интернет-радио | `M3U` | `PLAYER.ELF` + `MP3.ELF`, `AAC.ELF`, `OGG.ELF` | плейлист станций; поток MP3, AAC или Ogg Vorbis ESP качает сама по Wi-Fi; до 64 станций |

MP3, WAV, AAC, Ogg и FLAC идут на ESP потоком, по мере проигрывания. Остальное
уходит на ESP целиком, до начала звука (до 6 МиБ; IT — любой длины). Пример
плейлиста радио — [`MP3 Player/radio/RADIO.M3U`](MP3%20Player/radio/RADIO.M3U).

## Модули ESP (`*.ELF`)

Прошивка ESP — это ядро: связь с Evo, Wi-Fi, сеть, загрузчик модулей. Сам
проигрыватель и декодеры в неё не входят: это отдельные программы для
ESP32-S3 в формате ELF, собранные ESP-IDF против ядра прошивки, — загружаемые
модули. Ядро связывает модуль при загрузке, как динамическую библиотеку.
Поэтому проигрыватель обновляется без перепрошивки ESP: на карту кладут новый
`.ELF`.

**Куда копировать: в `/WC/ESP/` на том томе, откуда загружен Wild Commander.**

| Файл | Нужен для |
|---|---|
| `PLAYER.ELF` | всегда: сам проигрыватель (очередь, перемотка, громкость, спектр, обложки, радио) |
| `MP3.ELF` | MP3, WAV, радио |
| `AAC.ELF` | AAC, станции AAC |
| `OGG.ELF` | Ogg Vorbis, станции Ogg |
| `FLAC.ELF` | FLAC |
| `TRACKER.ELF` | MOD, XM, S3M, IT |
| `MIDI.ELF` | MIDI |
| `OPL.ELF` | VGM, VGZ, DRO, IMF, WLF, MWM, MFM |
| `TSFM.ELF` | AY, TurboSound, TurboSound FM, MTC |
| `SAA.ELF` | SAA, COP |

Проще скопировать все. Нет нужного модуля — файл этого формата не играет, в окне
`ERROR: missing /wc/esp/ИМЯ`.

Ресурсы — тоже в `/WC/ESP/`. Это чужие материалы, в репозиторий они не входят:

| Файл | Что это | Нужен для |
|---|---|---|
| `MIDI.SF2` | банк звуков General MIDI в SoundFont 2 (например, TimGM6mb), до 7 МБ | MIDI |
| `YRW801.ROM` | ПЗУ таблиц волн OPL4 (YRW801-M, 2 МБ) | MWM, MFM, VGM для OPL4 |
| `*.MWK` | набор волн песни MoonBlaster Wave — не в `/WC/ESP/`, а в папке песни | MWM |

Как модули попадают на ESP. При запуске плагин сверяет модуль на карте с кэшем
ESP по имени, длине и времени файла. Модуля в кэше нет или он изменился —
плагин передаёт его по SPI (`Sending to the ESP: ИМЯ`), ESP записывает его в
свою flash (`Storing on the ESP: ИМЯ`, несколько секунд) и дальше берёт оттуда.
После замены файла на карте это происходит один раз. Кэш модулей — каталог в
LittleFS ESP (раздел 1,9 МБ); не хватает места — вытесняется модуль, который
давно не нужен.

Ресурсы (`MIDI.SF2`, `YRW801.ROM`, `*.MWK`) ESP держит в PSRAM до перезапуска.
Плагин присылает их, когда они впервые понадобятся: банк — перед MIDI, ПЗУ и
набор — перед песней MoonBlaster, ПЗУ для VGM — когда его попросит декодер.

## Установка

1. `MP3 Player/build/VDAC4MP3.WMF` — в `/WC/`, имя `VDAC4MP3.WMF` — строкой в
   секцию `[PLUGINS]` файла `/WC/wc.ini`. Строку прежнего `VDAC4TSF.WMF`, если
   она есть, убрать: его форматы теперь в этом плагине.
2. `modules/out/*.ELF` — в `/WC/ESP/`.
3. По желанию `MIDI.SF2` и `YRW801.ROM` — в `/WC/ESP/`.
4. Для радио — плейлист `.M3U` в любую папку.

Файл играет по Enter или F3 на нём; из меню Shift+F3 — `Play on VDAC4`.
Если расширение уже занято другим плагином, вызывайте проигрыватель через
Shift+F3.

| Клавиша | Действие |
|---|---|
| пробел | пауза и продолжение |
| влево, вправо | перемотка на 10 с (модуль — на позицию; радио — станция) |
| PgUp, PgDn | предыдущий и следующий файл панели (радио — станция) |
| вверх, вниз | громкость |
| I | подробности: формат, отдано на ESP, очередь, сбои |
| H | справка |
| Esc | закрыть окно поверх; иначе остановить и выйти |

Доиграв файл, плагин открывает следующий файл панели. Подробно об окне,
радио, перемотке и сообщениях — [`MP3 Player/README.md`](MP3%20Player/README.md).

## Кэш на флешке USB-C (необязательно)

В разъём USB-C модуля ESP32-S3 можно вставить флешку с FAT32 или exFAT
(прошивка `vdac4-0.1.43` и новее). Она хранит копии ресурсов: банка
`MIDI.SF2` и ПЗУ `YRW801.ROM`.

Без флешки ресурсы живут в PSRAM ESP только до перезапуска. После включения
плагин снова передаёт их с карты Evo через Z80, а банк — это мегабайты. С
флешкой ESP при первой передаче кладёт копию в `/SYSTEM/CACHE/MUSIC`, а после
перезапуска берёт её оттуда, если копия не старее файла в `/WC/ESP`. Плагин её
тогда не передаёт; пока ESP проверяет кэш (до 60 с), в окне —
`Checking the ESP for: ИМЯ`.

- Готовить флешку не нужно: каталог и копии ESP создаёт сама.
- Музыки на флешке нет. Наборы волн `*.MWK` не кэшируются: они у каждой песни
  свои и небольшие. Модули `*.ELF` тоже не на флешке: они в кэше во flash самой
  ESP.
- Флешки нет или файловая система другая — всё работает как без неё.
- Пока ESP не включила USB-хост, разъём остаётся консолью USB. USB-хост
  включается при первом ресурсе. После этого до перезапуска консоль и прошивка
  по USB через этот разъём недоступны; прошивка по сети работает.

## Сборка

```powershell
.\build_modules.bat              # модули ESP в modules\out (нужен ESP-IDF 6.0)
.\build_modules.bat tsfm player  # выбранные
& ".\MP3 Player\build.bat"       # плагин (sjasmplus)
```

Модули — отдельные проекты ESP-IDF; собираются против прошивки из
[`../Firmware`](../Firmware) (загрузчик ELF и заголовки `modules/sdk`). После
сборки каждого модуля `tools/check_module_symbols.py` сверяет его внешние
символы с тем, что даёт ядро прошивки. Канал Z80 — общий драйвер
[`../Network/shared/z80`](../Network/shared/z80).

## Тесты

```powershell
python -m unittest discover -s tests -p "test_*.py"
```

Стенд проигрывателя (`tests/test_player_host.py`) собирает PLAYER и все
декодеры MSVC на ПК; тесты плагина исполняют `VDAC4MP3.WMF` в эмуляторе Z80
(пакет Python `z80`, ассемблер — переменная `SJASMPLUS`). Общие помощники
берутся из тестов `../Firmware/tests` и `../Network/tests`
(`tests/sound_paths.py`). Музыки для проверок в репозитории нет: тесты берут её
снаружи.

Команды звука прошивки — [`Docs/VDAC4.md`](../../Docs/VDAC4.md).

## Сторонние компоненты

| Компонент | Где | Лицензия |
|---|---|---|
| Helix MP3 (RealNetworks), [esp-libhelix-mp3](https://github.com/chmorgan/esp-libhelix-mp3) | компонент прошивки | RPSL / RCSL |
| Helix AAC (RealNetworks) | `components/helix_aac` | RPSL / RCSL |
| [stb_vorbis](https://github.com/nothings/stb) (Sean Barrett) | `components/stb_vorbis` | public domain / MIT |
| [libxmp](https://github.com/libxmp/libxmp) | `components/libxmp`, `components/libxmp_lite` | MIT |
| [TinySoundFont](https://github.com/schellingb/TinySoundFont) (Bernhard Schelling) | `components/tsf` | MIT |
| [ymfm](https://github.com/aaronsgiles/ymfm) (Aaron Giles) | `components/ymfm` | BSD-3-Clause |
| [foxen-flac](https://github.com/astoeckel/libfoxenflac) (Andreas Stöckel) | `components/foxen_flac` | LGPL-2.1 |
| [SAASound](https://github.com/stripwax/SAASound) (Dave Hooper, Simon Owen) | `components/saasound` | BSD-3-Clause |
| Модули `TSFM.ELF` (AYFly, ZXTune, ayumi и др.) и `SAA.ELF` (E-Tracker из ZXTune) | `modules/tsfm`, `modules/saa` | GPL-3.0 для модуля целиком; см. их README |

Изменения в сторонних компонентах описаны в их README. Собственный код —
[MIT](../../LICENSE).
