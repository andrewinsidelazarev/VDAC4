# Прошивка ESP32-S3 платы VDAC4

Прошивка модуля Waveshare ESP32-S3-Zero-N8R8 платы VDAC4 (ESP-IDF 6.0,
Arduino-ядро как компонент). ESP32-S3 — ведомый SPI Slave HD на шине SD-карты Evo
(порты `#57`/`#77`): канал SPI и разбор кадров ZiFi, мост VFS к карте Evo, Wi-Fi,
TCP/HTTP(S), FTP, SMB, NTP, OTA, вывод звука в I2S (PCM5102) и загрузчик модулей
ESP (ELF с карты: декодеры и проигрыватель лежат в [`../Sound`](../Sound),
программы Z80 сетевой части — в [`../Network`](../Network)).

Подробное описание платы, канала SPI, команд звука и наладки —
[`Docs/VDAC4.md`](../../Docs/VDAC4.md); протокол кадров —
[`Docs/PROTOCOL.md`](../../Docs/PROTOCOL.md).

## Файлы

| Файл | Что это |
|---|---|
| [`firmware/vdac4/firmware.factory.bin`](firmware/vdac4/firmware.factory.bin) | полный образ для первой записи через USB с адреса `0x0` |
| [`firmware/vdac4/firmware.bin`](firmware/vdac4/firmware.bin) | образ приложения для обновления по сети ([SHA-256](firmware/vdac4/firmware.sha256)) |

## Первая запись прошивки

Через USB Type-C модуля и [esptool](https://github.com/espressif/esptool) (`COM5`
замените своим портом; при первой записи добавьте `--erase-all`):

```bash
python -m esptool --chip esp32s3 --port COM5 write-flash 0x0 firmware/vdac4/firmware.factory.bin
```

> [!WARNING]
> Модуль прошивается вынутым из платы либо прямо в ней при **выключенной** Evo.
> Одновременно с питанием от Evo USB не подключать: такая работа проектом платы
> не проверялась.

Подробности — в [`Docs/VDAC4.md`](../../Docs/VDAC4.md#запись-в-модуль-через-usb).

## Обновление прошивки по сети

На Evo запустить `update_vdac4.sna` (из [`../Network/tools`](../Network/tools);
покажет адрес и порт `8267`), на ПК:

```bash
python tools/esp_tool.py --net <IP платы>
```

Образ `firmware/vdac4/firmware.bin` уходит по Wi-Fi; ESP проверяет заголовок и
SHA-256, переключает раздел OTA и перезапускается, при неудаче остаётся прежняя
прошивка.

## Сборка

Нужны ESP-IDF 6.0 (Arduino-ядро 3.3.12 подключается менеджером компонентов),
Python 3 и [sjasmplus](https://github.com/z00m128/sjasmplus).

```powershell
.\build_vdac4.bat              # прошивка, плагины WC (../Network, ../Sound) и .sna
.\build_vdac4.bat -SkipZ80     # только прошивка
```

Окружение ESP-IDF берётся из «ESP-IDF PowerShell» или из скрипта, указанного в
переменной `ZIFI_IDF_ENV`. Подробности, ключи и тонкости сборки —
[`Docs/VDAC4.md`](../../Docs/VDAC4.md#сборка).

## Тесты

Стенды на ПК (MSVC и Python): канал SPI, транспорт кадров, кэш модулей, загрузчик
ELF, FTP, SMB, WC Update, погода, обложки и др.

```powershell
python -m unittest discover -s tests -p "test_*.py"
```

Стенд ESP для эмулятора Unreal (режим `TS_ZIFI32=VDAC4`) — [`tools/unreal`](tools/unreal).

## Устройство

```text
Evo (Z80, TS-Config) ──SPI #57/#77──▶ ESP32-S3 (ведомый SPI Slave HD)
                                      ядро 1: канал SPI, разбор кадров, мост VFS к карте Evo
                                      ядро 0: Wi-Fi, TCP/HTTP(S), FTP, SMB, NTP, OTA, модули
                                      I2S ──▶ PCM5102
```

- Z80 — ведущий: пишет запрос в общий буфер ESP и перечитывает его до ответа;
  большие блоки идут через DMA TS-Config.
- Файлы карты Evo ESP читает и пишет через плагин Z80 (мост VFS), окнами до
  16 КиБ; между сетью и картой — кольца в PSRAM.
- Пользовательские программы ESP — модули ELF с карты: ядро прошивки загружает
  их в PSRAM и кэширует во Flash. Заголовки для модулей — [`modules/sdk`](modules/sdk).

Исходники: `src/`, `include/`, `main/`, `components/`, `lib/`.

## Сторонние компоненты

| Компонент | Где | Лицензия |
|---|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf), [arduino-esp32](https://github.com/espressif/arduino-esp32) | менеджер компонентов | Apache-2.0, LGPL-2.1 |
| [libsmb2](https://github.com/sahlberg/libsmb2) (Ronnie Sahlberg) | `components/libsmb2`, `lib/libsmb2` | LGPL-2.1 |
| [ELF loader](https://components.espressif.com/components/espressif/elf_loader), cmake_utilities (Espressif) | `components/elf_loader`, `components/cmake_utilities` | Apache-2.0 |
| USB-хост и драйвер флешек: [espressif/usb](https://components.espressif.com/components/espressif/usb), [espressif/usb_host_msc](https://components.espressif.com/components/espressif/usb_host_msc) | менеджер компонентов | Apache-2.0 |
| FatFs (ChaN) в компоненте `fatfs` ESP-IDF 6.0.2, с exFAT | `components/fatfs` (см. `README.VDAC4.md`) | лицензия FatFs, Apache-2.0 |
| Helix MP3 ([esp-libhelix-mp3](https://components.espressif.com/components/chmorgan/esp-libhelix-mp3)) | менеджер компонентов | RPSL / RCSL |
| libxm (Romain Dalmaso); загрузчики MOD/S3M из прошивки TS-Labs | `components/tracker` | WTFPL; см. README компонента |
| [jpgd](https://github.com/richgel999/jpeg-compressor) (Richard Geldreich) | `lib/jpgd` | Apache-2.0 |

Изменения в сторонних компонентах описаны в их README. Собственный код —
[MIT](../../LICENSE).
