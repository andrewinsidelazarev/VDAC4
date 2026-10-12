# VDAC4

Плата VDAC4 для ZX Evolution (TS-Config): Tang Nano 20K, модуль Waveshare
ESP32-S3-Zero-N8R8 и ЦАП PCM5102. ESP32-S3 подключена к Evo по SPI — к той же
шине, что и SD-карта (порты `#57`/`#77`), и работает ведомой: Z80 обращается к ней
двоичным протоколом ZiFi. Сеть, разбор HTTP, файловые серверы и декодирование
звука делает ESP; программы Z80 — плагины Wild Commander и `zifi.spg` —
показывают интерфейс и работают с картой Evo.

| Папка | Что там |
|---|---|
| [`Docs`](Docs) | блок-схема платы VDAC4 A4 ([PNG](Docs/TS_VDAC_A4-block-diagram.png), [SVG](Docs/TS_VDAC_A4-block-diagram.svg), [PDF](Docs/TS_VDAC_A4-block-diagram.pdf)) |
| [`ESP32 S3 Zero N8R8/Docs`](ESP32%20S3%20Zero%20N8R8/Docs) | ESP32-S3: схемы подключения (SPI к IDE-40, звук GY-PCM5102, распиновка), описание прошивки, канала SPI, команд и наладки ([`VDAC4.md`](ESP32%20S3%20Zero%20N8R8/Docs/VDAC4.md)), протокол кадров ([`PROTOCOL.md`](ESP32%20S3%20Zero%20N8R8/Docs/PROTOCOL.md)) |
| [`ESP32 S3 Zero N8R8/Firmware`](ESP32%20S3%20Zero%20N8R8/Firmware) | прошивка ESP32-S3: канал SPI, Wi-Fi, серверы, вывод звука, загрузчик модулей |
| [`ESP32 S3 Zero N8R8/Network`](ESP32%20S3%20Zero%20N8R8/Network) | сетевые программы Z80: `zifi.spg`, плагины FTP, SMB, NTP, WC Update, заставки погоды, утилиты |
| [`ESP32 S3 Zero N8R8/Sound`](ESP32%20S3%20Zero%20N8R8/Sound) | VDAC4 Media Player: модули ESP (декодеры) и плагин |

Что нужно на стороне Evo — в
[README сетевой части](ESP32%20S3%20Zero%20N8R8/Network/README.md#что-нужно).

Собственный код — [MIT](LICENSE); сторонние компоненты и их лицензии перечислены
в README частей.
