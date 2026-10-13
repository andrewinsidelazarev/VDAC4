# Сеть VDAC4

Сетевые программы Z80 для ZX Evolution (TS-Config) с платой VDAC4: `zifi.spg`,
плагины Wild Commander и служебные утилиты. Сеть, DNS, HTTP(S), файловые
серверы и NTP делает ESP32-S3 платы (прошивка — в
[`../Firmware`](../Firmware)); программы Z80 показывают интерфейс и работают с
картой Evo. С ESP они говорят двоичным протоколом ZiFi по SPI — общий драйвер
канала лежит в [`shared/z80`](shared/z80).

Описание платы, канала SPI и наладки — [`Docs/VDAC4.md`](../Docs/VDAC4.md);
протокол кадров — [`Docs/PROTOCOL.md`](../Docs/PROTOCOL.md).

## Что умеет

- Wi-Fi по `/zifi/zifi.ini` (SSID, пароль, часовой пояс); настройки ESP
  запоминает и после перезапуска подключается сама. Часовой пояс ESP — из
  `time:` в `zifi.ini`, как только он известен.
- `zifi.spg` — каталоги сайтов и загрузка файлов: ESP сама делает DNS, HTTP и
  HTTPS (проверка сервера по встроенному набору сертификатов Mozilla), Z80
  забирает тело ответа. Сохранение на SD — самостоятельным драйвером FAT32 через
  временный файл: под своим именем файл появляется только целиком.
- HTTP-прокси для ретро-архивов (`vtrd.in`, `zxart.ee` и др.) с проверкой
  доступности.
- FTP-сервер: три сеанса, пассивный и активный режимы, даты файлов как в панели
  WC.
- SMB2/SMB3-сервер для Проводника Windows: том активной панели WC как
  `\\ZX-Evo\0`, NBNS и WS-Discovery для сетевого окружения.
- Часы RTC по NTP. Часы самой ESP — по тому же ответу NTP, а если NTP не
  ответил — по RTC Evo (только если дата в нём позже 1 октября 2026 года).
- Погода для заставок WC: город из `zifi.ini` → координаты → прогноз Open-Meteo.
- WC Update: сверка файлов Wild Commander Improved на SD с GitHub по git-SHA и
  обновление отличающихся.
- Online Update: прошивка ESP с GitHub (этот репозиторий) — проверка версии,
  загрузка и проверка SHA-256 на самой ESP, без ПК.
- Утилиты для `/zifi/tools`: проверка связи с ESP, порт обновления прошивки по
  сети, проверка DMA и ЦАП.

## Что нужно

- ZX Evolution с конфигурацией TS-Config, собранной с ключом `ESP32_SPI`:
  штатная прошивка Evo для VDAC2 из репозитория TS-Labs
  ([`zxevo_fw_vdac2.bin`](https://github.com/tslabs/zx-evo/tree/master/pentevo/avr/current/default)),
  сентябрь 2024 года или новее.
- [Wild Commander Improved](https://github.com/andrewinsidelazarev/Wild-Commander-Improved)
  v1.11i от 28 сентября 2026 года или новее (даты файлов в FTP и SMB, запись
  через `FILEX.WMF`).
- Плата VDAC4 с модулем ESP32-S3-Zero-N8R8 (8 МБ Flash, 8 МБ PSRAM) и
  прошивкой из [`../Firmware`](../Firmware).

## Файлы для платы

| Файл | Куда на карте | Что это |
|---|---|---|
| [`ZiFi SPG/build/zifi.spg`](ZiFi%20SPG/build/zifi.spg) | `/zifi` | каталоги сайтов и загрузка |
| [`FTP Server/build/vdac4/VDAC4FTP.WMF`](FTP%20Server/build/vdac4/VDAC4FTP.WMF) | `/WC` | FTP-сервер |
| [`SMB Server/build/vdac4/VDAC4SMB.WMF`](SMB%20Server/build/vdac4/VDAC4SMB.WMF) | `/WC` | SMB-сервер |
| [`NTP Time Sync/build/vdac4/VDAC4NTP.WMF`](NTP%20Time%20Sync/build/vdac4/VDAC4NTP.WMF) | `/WC` | часы RTC по NTP |
| [`WC Update/build/vdac4/VDAC4WCU.WMF`](WC%20Update/build/vdac4/VDAC4WCU.WMF) | `/WC` | проверка и обновление Wild Commander с GitHub |
| [`Online Update/build/vdac4/VDAC4UPD.WMF`](Online%20Update/build/vdac4/VDAC4UPD.WMF) | `/WC` | обновление прошивки ESP с GitHub ([README](Online%20Update/README.md)) |
| [`Weather screensaver/build/vdac4/VDAC4WTH.WMF`](Weather%20screensaver/build/vdac4/VDAC4WTH.WMF) | `/WC` | заставка: часы, календарь, погода |
| [`Weather screensaver VDAC2/build/vdac4/VDAC4WT2.WMF`](Weather%20screensaver%20VDAC2/build/vdac4/VDAC4WT2.WMF) | `/WC` | та же заставка в 1024×768 на FT812 |
| [`tools/esp_info_vdac4.sna`](tools/esp_info_vdac4.sna) | `/zifi/tools` | проверка связи с ESP и сведения о прошивке, без Wi-Fi |
| [`tools/update_vdac4.sna`](tools/update_vdac4.sna) | `/zifi/tools` | открывает порт обновления прошивки по сети |
| [`tools/dma_probe_vdac4.sna`](tools/dma_probe_vdac4.sna) | `/zifi/tools` | обмен с ESP через DMA TS-Config в обе стороны |
| [`tools/audio_test_vdac4.sna`](tools/audio_test_vdac4.sna) | `/zifi/tools` | проверка ЦАП: тоны по каналам, громкость, линии I2S |

Каждый плагин нужно назвать в секции `[PLUGINS]` файла `/WC/wc.ini`; для записи
на карту по FTP и SMB первой активной строкой там должен стоять `FILEX.WMF`.
Пример `/zifi/zifi.ini`:

```ini
SSID:MyNetwork
password:secret
time:+3
city:Kyiv
```

## Сборка

Нужен [sjasmplus](https://github.com/z00m128/sjasmplus) (путь — в переменной
`SJASMPLUS`, если его нет в `PATH`) и Python 3 (заставка для VDAC2 рисует
картинки шрифтами Windows).

```powershell
& ".\FTP Server\build.bat" vdac4
& ".\SMB Server\build.bat" vdac4
& ".\NTP Time Sync\build.bat" vdac4
& ".\WC Update\build.bat" vdac4
& ".\Weather screensaver\build.bat" vdac4
& ".\Weather screensaver VDAC2\build.bat" vdac4
& ".\tools\build_esp_info.bat"   # esp_info, update, dma_probe, audio_test
& ".\ZiFi SPG\build.bat"         # нужен FAT32-Driver-ZX-Evolution (FAT32_DRIVER_DIR)
```

Всё вместе с прошивкой собирает `..\Firmware\build_vdac4.bat`.

## Тесты

Программы Z80 исполняются в эмуляторе (пакет Python `z80`) против модели канала
SPI прошивки ([`tests/spi_link_model.py`](tests/spi_link_model.py)):

```powershell
python -m unittest discover -s tests -p "test_*.py"
python -m unittest discover -s "Weather screensaver\tests"
python -m unittest discover -s "Weather screensaver VDAC2\tests"
```

`tests/test_zifi_spg_fat32.py` берёт драйвер FAT32 из `FAT32_DRIVER_DIR`.

## Сторонние компоненты

| Компонент | Где | Лицензия |
|---|---|---|
| [FAT32-Driver-ZX-Evolution](https://github.com/andrewinsidelazarev/FAT32-Driver-ZX-Evolution) | подключается при сборке `zifi.spg` | см. репозиторий |

Собственный код — [MIT](../../LICENSE).
