# A3.1 — распиновка ESP/Tang и отдельная синхронизация

Генерируется `hardware/kicad/Tools/generate_a3_interfaces.py`; входит в полную схему A3.1.
Это карта соединений, не файл ограничений для прошивки.

## RGB555 + PAL: две фазы по восемь линий

| Линия | Mux / канал | SEL=0 | IDE | SEL=1 | IDE | Пины A/B/Y |
|---|---|---|---|---|---|---|
| D0 | U1 / 1 | R0 | 15 | B0 | 8 | 2/3/4 |
| D1 | U1 / 2 | R1 | 13 | B1 | 10 | 5/6/7 |
| D2 | U1 / 3 | R2 | 11 | B2 | 12 | 11/10/9 |
| D3 | U1 / 4 | R3 | 9 | B3 | 14 | 14/13/12 |
| D4 | U2 / 1 | R4 | 7 | B4 | 16 | 2/3/4 |
| D5 | U2 / 2 | G0 | 5 | G4 | 6 | 5/6/7 |
| D6 | U2 / 3 | G1 | 3 | PAL_SEL | 18 | 11/10/9 |
| D7 | U2 / 4 | G2 | 17 | G3 | 4 | 14/13/12 |

У U1/U2: 1=SEL, 8=GND, 15=/OE=GND, 16=+3V3_LOGIC. C6/C7 — по 100 нФ.
HS: IDE37 → U3.8 → U3.12 → Tang FPGA15. VS: IDE38 → U3.9 → U3.11 → FPGA16.
PCLK: IDE36 → U3.7 → U3.13 → FPGA80. U3: DIR=HIGH, /OE=LOW, питание 3,3 В.
HS/VS доступны независимо от SEL и M_SEL. Будущий RTL должен согласовать их с задержкой RGB/PAL.

## Tang: направления относительно FPGA

| Цепь | FPGA | Гребёнка | Направление | ESP | Нагрузка / условие |
|---|---|---|---|---|---|
| MUX_D0 | 27 | J6.8 | in |  |  |
| MUX_D1 | 28 | J6.9 | in |  |  |
| MUX_D2 | 29 | J6.12 | in |  |  |
| MUX_D3 | 30 | J6.13 | in |  |  |
| MUX_D4 | 41 | J5.6 | in |  |  |
| MUX_D5 | 42 | J5.5 | in |  |  |
| MUX_D6 | 48 | J5.10 | in |  |  |
| MUX_D7 | 54 | J5.8 | in |  | Onboard I2S DIN; amplifier disabled by FPGA top |
| TANG_HSYNC | 15 | J6.6 | in |  | U3.12; onboard LED0 / 510R to 3V3 |
| TANG_VSYNC | 16 | J6.7 | in |  | U3.11; onboard LED1 / 510R to 3V3 |
| M_SEL_3V3 | 73 | J6.1 | in |  | LCD_INIT3 / HSPI_DIN2; no LCD attached |
| TANG_PCLK | 80 | J5.4 | in |  | GCLKT_0; onboard SD DAT2 10k pull-up; no SD card |
| MUX_SEL | 56 | J5.7 | out |  | Onboard I2S BCLK; amplifier disabled by FPGA top |
| TANG_FT_INT_N | 74 | J6.2 | out |  | Dedicated return via U11, independent of mux |
| FT_CS_N_3V3 | 17 | J6.15 | in |  | Onboard LED2 load |
| SPI_SCK_3V3 | 20 | J6.16 | in |  | Onboard LED5 load; external shared SPI clock |
| SPI_MOSI_3V3 | 19 | J6.17 | in |  | Onboard LED4 load |
| TANG_SPI_MISO | 79 | J5.14 | out |  | Onboard 5.1k pull-down and WS2812 DIN via 100R |
| LINK_SCK | 77 | J6.5 | in | GPIO5 | GCLKT_1; ESP master; no LCD attached |
| LINK_IO0 | 71 | J5.18 | inout | GPIO4 | QSPI data 0 / MOSI in single-bit mode |
| LINK_IO1 | 72 | J5.17 | inout | GPIO7 | QSPI data 1 / MISO in single-bit mode |
| LINK_IO2 | 55 | J5.11 | inout | GPIO6 | QSPI data 2; onboard I2S LRCK input |
| LINK_IO3 | 31 | J6.14 | inout | GPIO44 | QSPI data 3; LCD_B3, no LCD attached |
| LINK_CS_N | 18 | J6.18 | in | GPIO3 | R10 10k to 3V3; onboard LED3 / 510R load |
| LINK_IRQ_N | 85 | J6.4 | out | GPIO9 | Active LOW; onboard SD DAT1 10k pull-up |

Заняты все 25 доступных сигнальных выводов. HDMI/BL616 сохранены; 49/51 не подключать к carrier.
Все четыре LINK_IO двунаправленные; передача и приём идут по очереди.

## ESP32-S3-Zero-N8R8-M

Номера A2 ниже относятся к локальному символу и посадке. На готовом модуле ориентироваться по GPIO, не по P15 из PDF другой ревизии.

| Контакт A2 | Маркировка / GPIO | Цепь |
|---|---|---|
| 1 | GPIO1 | I2S_BCK |
| 2 | GPIO2 | I2S_LRCK |
| 3 | GPIO3 | LINK_CS_N |
| 4 | GPIO4 | LINK_IO0 |
| 5 | GPIO5 | LINK_SCK |
| 6 | GPIO6 | LINK_IO2 |
| 7 | GPIO7 | LINK_IO1 |
| 8 | GPIO8 | I2S_DIN |
| 9 | GPIO9 | LINK_IRQ_N |
| 10 | GPIO10 | ESP_CS_N_3V3 |
| 11 | GPIO11 | SPI_MOSI_3V3 |
| 12 | GPIO12 | SPI_SCK_3V3 |
| 13 | GPIO13 | ESP_SPI_MISO |
| 14 | GND | GND |
| 15 | 5V | ESP_5V |
| 16 | 3V3_OUT | NC |
| 17 | GPIO43/TX | NC |
| 18 | GPIO44/RX | LINK_IO3 |

SPI2_HOST — внешний SPI slave Evo (GPIO10–13); SPI3_HOST — внутренний QSPI master.
В Arduino использовать драйвер ESP-IDF SPI master с half-duplex/Quad и отдельными транзакциями чтения/записи.
UART отсутствует; GPIO44/RX используется как QSPI IO3, GPIO43/TX свободен. I2S остаётся на 1/2/8.
R10=10 кОм удерживает GPIO3/CS HIGH при сбросе. GPIO3 — strap выбора JTAG, не режима загрузки;
при включённом STRAP_JTAG_SEL высокий уровень выбирает USB Serial/JTAG. eFuse не менять.
USB использует штатные GPIO19/20. UART0 на GPIO43/44 в приложении не включать.

## Источники и границы

Проверены локальные pinout и схемы. PDF ESP32-S3-Zero-Sch.pdf относится к FH4R2, а не к точной плате N8R8-M.
Рабочая частота QSPI, переключение направления и временное согласование HS/VS с RGB ещё требуют RTL/STA и аппаратной проверки.
Старые результаты приёмника A3.0 (2×9) не подтверждают новый интерфейс (2×8 + HS/VS).

- `docs/datasheets/sn74lvc257a.pdf`; SHA-256 `79a39fc53062a5a3a298f692d83798440d14610a91c313ae85aba53f35b545af`
- `docs/references/a3-mux-ingress-20260905/video_out.v`; SHA-256 `e486a2075af1468696312b3542c371e2da486af5fd2c2210ded3c07c87ad2137`
- `docs/datasheets/Tang_Nano_20K_3921.pdf`; SHA-256 `c3c85c579c446a29cc2bcd77f54c285d8c60f01ecf725a185e2fb1e6a56fdcc5`
- `docs/datasheets/sn74lvc245a.pdf`; SHA-256 `ad3e705aef834214404d6e1e38cb00f95fdb6f0d9807a784a6f58c4d060f13a3`
- `docs/references/a3.1-claude-review-20260908/GW2AR_UG115E.pdf`; SHA-256 `a67ba07252a64efcdffec5005f3b58cf02903c26ee2b179998b8388bc00ce217`
- `docs/datasheets/ESP32-S3-PICO-1_datasheet_en.pdf`; SHA-256 `44daca9121111a9c7df437e18c3c153258a20e68fe57fcd3bd9be593ad162979`
- `docs/datasheets/ESP32-S3_technical_reference_manual_en.pdf`; SHA-256 `4484bf8a69035ec42a731c58c64ada6fbd1f1618c5559409f134d9ea083f444f`
- `docs/reference-images/esp32-s3-zero-n8r8-m-official.jpg`; SHA-256 `15a3b271a0ecb7bc4af39688933412f9b541b2d8b0a5630cab90d4612b70f2f9`
