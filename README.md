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
| [`Hardware`](Hardware) | плата VDAC4, ревизии A5 (текущая) и A4: проекты KiCad, комплекты для заказа на JLCPCB, PDF схем и BOM; перемычки ЦАП и меры предосторожности — в [`Hardware/README.md`](Hardware/README.md) |
| [`ESP32 S3 Zero N8R8/Docs`](ESP32%20S3%20Zero%20N8R8/Docs) | ESP32-S3: схемы подключения (SPI к IDE-40, звук GY-PCM5102, распиновка), описание прошивки, канала SPI, команд и наладки ([`VDAC4.md`](ESP32%20S3%20Zero%20N8R8/Docs/VDAC4.md)), протокол кадров ([`PROTOCOL.md`](ESP32%20S3%20Zero%20N8R8/Docs/PROTOCOL.md)) |
| [`ESP32 S3 Zero N8R8/Firmware`](ESP32%20S3%20Zero%20N8R8/Firmware) | прошивка ESP32-S3: канал SPI, Wi-Fi, серверы, вывод звука, загрузчик модулей |
| [`ESP32 S3 Zero N8R8/Network`](ESP32%20S3%20Zero%20N8R8/Network) | сетевые программы Z80: `zifi.spg`, плагины FTP, SMB, NTP, WC Update, заставки погоды, утилиты |
| [`ESP32 S3 Zero N8R8/Sound`](ESP32%20S3%20Zero%20N8R8/Sound) | VDAC4 Media Player: модули ESP (декодеры) и плагин |
| [`Tang Nano 20k/Docs`](Tang%20Nano%2020k/Docs) | Tang Nano 20K: кадровый буфер TS-Config → HDMI 720p50, LINK с ESP, настройки во flash Tang, обновление через ESP ([`a3-fpga-framebuffer.md`](Tang%20Nano%2020k/Docs/a3-fpga-framebuffer.md)); генлок-тракт ([`a3-fpga-video.md`](Tang%20Nano%2020k/Docs/a3-fpga-video.md)); распиновка ESP/Tang ([`a3-interface-pinmap.md`](Tang%20Nano%2020k/Docs/a3-interface-pinmap.md)) |
| [`Tang Nano 20k/Firmware`](Tang%20Nano%2020k/Firmware) | прошивка Tang (GW2AR-18): RTL, тестбенчи, сборка в Gowin; готовые ядра flash Tang — TS-Config [`firmware/ts_fb_hdmi_top.fs`](Tang%20Nano%2020k/Firmware/firmware/ts_fb_hdmi_top.fs) (слот 0) и FT812 [`firmware/ts_fb_slot1.fs`](Tang%20Nano%2020k/Firmware/firmware/ts_fb_slot1.fs) (слот 1) |
| [`Tang Nano 20k/BL616`](Tang%20Nano%2020k/BL616) | прошивка BL616 модуля Tang: сопроцессор FT812, 3D-команды VDAC4, кэш ядер карты Tang, обновление по сети; готовый образ — прошивка 24 [`firmware/vdac4_copro_bl616.bin`](Tang%20Nano%2020k/BL616/firmware/vdac4_copro_bl616.bin) |
| [`Tang Nano 20k/FTVIEW4`](Tang%20Nano%2020k/FTVIEW4) | плагин Wild Commander FTVIEW4 для VDAC4: картинки и ролики через FT812 и BL616, MJPEG AVI — через ядро видео Tang (слот 3) и VIDEO.ELF на ESP; сборка — [`build/FTVIEW4.WMF`](Tang%20Nano%2020k/FTVIEW4/build/FTVIEW4.WMF) |
| [`Tang Nano 20k/Cores`](Tang%20Nano%2020k/Cores) | все ядра Tang и их установка; ядра для карты Tang — ZX Spectrum Next и NES / Денди со слоями ESP, их исходники (GPL-3.0) и правка ROM NextZXOS |

Что нужно на стороне Evo — в
[README сетевой части](ESP32%20S3%20Zero%20N8R8/Network/README.md#что-нужно).

Собственный код — [MIT](LICENSE); сторонние компоненты и их лицензии перечислены
в README частей.
