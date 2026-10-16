#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zifi/board.hpp"

#if ZIFI_HAS_SPI_LINK

namespace zifi {
namespace link_spi3 {

// Аппаратный QSPI-ведущий на контроллере SPI3 без драйвера ESP-IDF (spi_master).
//
// Почему без драйвера: в этой прошивке spi_bus_initialize() для SPI3 вешает ESP — внутри
// spicommon_bus_initialize_io(), на любых выводах и без платы (проверено на столе пробами
// 0.1.54–0.1.61: interrupt watchdog, PC в обработчике прерываний). Те же операции —
// тактирование, сброс, маршрутизация выводов через матрицу GPIO — выполненные своим кодом,
// проходят. Поэтому здесь контроллер готовится вручную, а транзакции ведёт HAL (spi_hal_*)
// опросом: ни шины-замка драйвера, ни очередей, ни выделения прерываний.
//
// Транзакция полудуплексная: команда 8 бит по IO0, адрес 32 бита по IO0…IO3, для чтения —
// такты разворота, данные по IO0…IO3. CS — обычный вывод GPIO.

struct Pins {
  int sck;
  int cs;
  int io0;
  int io1;
  int io2;
  int io3;
};

// Запустить контроллер и подключить выводы. Вызывать один раз.
bool begin(const Pins& pins, uint32_t hz);
// Сменить частоту LINK_SCK (без компенсации задержки матрицы GPIO лишними тактами:
// число тактов разворота должно совпадать с ведомым).
bool setClock(uint32_t hz);
uint32_t clockHz();

// Одна транзакция; слова — 32 бита, на линии старшим байтом вперёд; не больше 16 слов.
bool transfer(uint8_t command, uint32_t address, uint32_t* words, size_t count, bool reading,
              int dummyCycles);

}  // namespace link_spi3
}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
