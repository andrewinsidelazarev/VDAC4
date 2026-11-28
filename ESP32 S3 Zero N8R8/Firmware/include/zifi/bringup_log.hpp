#pragma once

#include "zifi/board.hpp"

#if ZIFI_HAS_SPI_LINK

namespace zifi {

// Журнал наладки платы. Строка уходит сразу в журнал ESP-IDF (USB модуля) и в
// журнал прошивки (diagnostic_log.hpp): он живёт в ОЗУ, его читают сетевая консоль
// («log») и OTA. В плате USB к модулю не подключён, поэтому видно строки только
// оттуда. Во flash журнал попадает только при запуске после падения (0.1.183).
//
// Вызывать из несрочных задач: если журнал занят другой задачей, вызов ждёт до
// 200 мс.
void bringupLog(const char* tag, const char* format, ...)
    __attribute__((format(printf, 2, 3)));

// Напечатать журнал в USB модуля, если к нему подключён компьютер.
void bringupPrintStoredLog();

}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
