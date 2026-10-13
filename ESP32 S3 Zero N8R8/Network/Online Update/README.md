# VDAC4 Online Update

`VDAC4UPD.WMF` — плагин Wild Commander: обновляет прошивку ESP32-S3 платы VDAC4
с GitHub, без ПК и без USB. Образ качает и проверяет сама ESP; через Z80 идут
только команды и индикатор.

Источник — этот репозиторий, ветка `main`, папка
[`Firmware/firmware/vdac4`](../../Firmware/firmware/vdac4): манифест
`firmware.sha256` (строка `VERSION vdac4-X.Y.Z` и SHA-256 образов) и
`firmware.bin`. Адрес зашит в прошивку (`ZIFI_UPDATE_DIRECTORY` в
`Firmware/include/zifi/board.hpp`); на новый репозиторий он указывает с
`vdac4-0.1.50`. Прошивку старше один раз ставят по локальной сети
(`tools/update_vdac4.sna` и `esp_tool.py --net`, см.
[`Docs/VDAC4.md`](../../Docs/VDAC4.md#обновление-по-сети)).

## Установка

1. `build/vdac4/VDAC4UPD.WMF` — в `/WC/`, имя — строкой в секцию `[PLUGINS]`
   файла `/WC/wc.ini`.
2. В `/zifi/zifi.ini` — действующие `ssid:` и `password:` (как для остальных
   сетевых плагинов). Плагин передаёт файл целиком, до 1024 байтов; больший
   отвергается с надписью, а не обрезается.

## Работа

Запуск — из меню плагинов (F10): `VDAC4 Online Update`.

1. Плагин читает `zifi.ini`, перезапускает ESP (`SYS_RESET`, как FTP и SMB:
   прежние серверы, приложение звука и раздробленная куча уходят, HTTPS нужна
   память), ждёт `READY` и передаёт настройки Wi-Fi.
2. ESP читает с GitHub только манифест (`ONLINE_UPDATE_CHECK`); flash на запись
   не открывается. В окне — `Current` (версия на ESP, из `SYS_INFO`) и
   `Available` (опубликованная):
   - `New version available. ENTER = install` — опубликована новее;
   - `Same version. ENTER = reinstall` — та же: можно поставить заново
     (восстановление);
   - `Different build. ENTER = install` — номер не сравнить (не формат
     `vdac4-X.Y.Z`);
   - `Older version. DOWNGRADE BLOCKED` — опубликована старше: Enter не
     действует, команда установки не отправляется.
3. Enter — установка (`ONLINE_UPDATE`): ESP читает манифест заново, качает
   `firmware.bin`, проверяет заголовок (образ приложения ESP32-S3 именно для
   VDAC4: прошивку переходника ZiFi она не поставит), пишет образ в свободный
   раздел OTA и считает SHA-256 на ходу. Совпал с манифестом — переключает раздел
   и перезапускается; не совпал, оборвалось или не записалось — раздел не
   переключается, работает прежняя прошивка. Если новая не запустится, загрузчик
   вернёт прежнюю.
4. Полоса и проценты показывают этап: `Reading version and SHA-256`,
   `Downloading firmware.bin`, `Checking SHA-256 and installing`. После
   перезапуска плагин ждёт `READY` новой загрузки и показывает новую версию:
   `Update completed. ESC = return`.

Esc до подтверждения — отмена. После Enter питание не выключать.

Ошибки: `ERROR: no ESP on the SPI`, `ERROR: ESP does not answer` (нет `READY`
после перезапуска), `ERROR: Wi-Fi connection failed` или текст от ESP
(например, `update-check:manifest http 404`), отказы чтения `zifi.ini`.

## Сборка

```powershell
& ".\Online Update\build.bat"
```

Сборка — только для платы VDAC4 (`-DZIFI_SPI`, канал SPI без DMA; общий драйвер —
[`shared/z80`](../shared/z80)). Плагин перенесён с `ZIFIUPD.WMF` 0.6 переходника
ZiFi (UART): та же логика, другой канал, имена и тексты VDAC4.

Проверка в Unreal: `prepare_unreal_vdac4.py --online` кладёт плагин и тестовый
`zifi.ini`; программа ESP для Unreal читает настоящие манифест и `firmware.bin` с
GitHub, проверяет образ и SHA-256 и ничего не пишет. «Установленную» версию
задаёт переменная `VDAC4_FW_VERSION`.
