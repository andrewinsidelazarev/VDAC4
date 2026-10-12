#pragma once

#include <stdint.h>

// Привязка прошивки к плате. Сборку выбирает определение ZIFI_BOARD_VDAC4:
//   1         — несущая плата VDAC4 A3.1 (обмен по SPI платы ZX Evolution, ЦАП PCM5102);
//   не задано — стенды ПК (tools/host_*, tests): исходники прошивки без канала SPI,
//               звука и прочего оборудования платы. Сама прошивка так не собирается.
//
// Номера выводов VDAC4 взяты из docs/a3-interface-pinmap.md проекта платы.

#if defined(ZIFI_BOARD_VDAC4) && ZIFI_BOARD_VDAC4

#define ZIFI_HAS_SPI_LINK 1
#define ZIFI_HAS_AUDIO 1
#define ZIFI_HAS_STATUS_LED 1
// Флешка в разъёме USB-C модуля (USB OTG хостом): кэш ресурсов музыки.
#define ZIFI_HAS_USB_DRIVE 1
// Обновление из сети берёт образ только для своей платы. Прошивка переходника ZiFi
// на VDAC4 запустилась бы, но связи с Evo у неё не было бы (там UART, здесь SPI).
#define ZIFI_UPDATE_DIRECTORY \
  "/andrewinsidelazarev/ZiFi-ESP32-S3-Zero/refs/heads/main/firmware/vdac4/"

namespace zifi {
namespace board {

constexpr char kName[] = "VDAC4-A3.1";
// Формат номера версии этой платы: по нему сравниваются установленная и опубликованная.
constexpr char kReleaseVersionFormat[] = "vdac4-%u.%u.%u%n";

// SPI Evo -> ESP. ESP здесь всегда ведомый: обратный драйвер платы однонаправленный,
// и режим ведущего ESP на этой плате запрещён. Выводы совпадают с IO_MUX порта SPI2.
constexpr int kSpiCsPin = 10;
constexpr int kSpiMosiPin = 11;
constexpr int kSpiSckPin = 12;
constexpr int kSpiMisoPin = 13;
// Когда ESP выставляет на MISO очередной бит. В режиме 0 ведомому положено
// делать это по спаду SCK, а Evo читает бит по следующему фронту, через
// полтакта (36 нс при 14 МГц). На этой плате такт идёт к ESP через буфер Evo и
// U3, а данные обратно — через U6 и U8, и вместе с самим ESP это те же
// полтакта: бит приходит к Evo в момент выборки, и она читает то его, то
// предыдущий (esp_info видел 2D EF 00 и 3D FB 01 вместо 5A D7 01). Поэтому бит
// выставляется на полтакта раньше, по фронту SCK: к выборке он стоит уже
// полтакта, а сменится ещё через столько же.
constexpr bool kSpiMisoEarly = true;

// I2S -> GY-PCM5102 (SCK модуля на земле: ЦАП берёт тактовую из BCK).
constexpr int kI2sBckPin = 1;
constexpr int kI2sLrckPin = 2;
constexpr int kI2sDataPin = 8;

// Светодиод WS2812 самого модуля ESP32-S3-Zero; на плату этот вывод не выходит.
constexpr int kStatusLedPin = 21;

// Линии LINK к Tang Nano 20K (QSPI и запрос прерывания). Обмена по ним пока нет:
// они остаются входами и не мешают FPGA.
//   GPIO3 CS, GPIO5 SCK, GPIO4 IO0, GPIO7 IO1, GPIO6 IO2, GPIO44 IO3, GPIO9 IRQ.
// GPIO44 — это ещё и RX UART0, поэтому UART0 на этой плате не используется вовсе.
//
// LINK CS на плате подтянут резистором R10 (10 кОм) к +3,3 В её логики. По нему
// прошивка узнаёт, стоит ли модуль в плате и есть ли на плате питание.
constexpr int kLinkCsPin = 3;

}  // namespace board
}  // namespace zifi

#else

#define ZIFI_HAS_SPI_LINK 0
#define ZIFI_HAS_AUDIO 0
#define ZIFI_HAS_STATUS_LED 0
#define ZIFI_HAS_USB_DRIVE 0

#endif
