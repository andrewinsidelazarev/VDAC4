#pragma once

#include "zifi/board.hpp"

#if ZIFI_HAS_SPI_LINK

namespace zifi {

// Журнал наладки платы. Строка уходит сразу в журнал ESP-IDF (USB модуля) и в
// постоянный журнал во flash (diagnostic_log.hpp). В плате USB к модулю не
// подключён, поэтому узнать, что прошивка видела там на линиях, можно только
// из постоянного журнала — переставив модуль на USB.
//
// Во flash строку переносит сетевая задача, не позже чем через секунду. Вызывать
// из несрочных задач: если журнал занят, вызов ждёт до 200 мс.
void bringupLog(const char* tag, const char* format, ...)
    __attribute__((format(printf, 2, 3)));

// Напечатать постоянный журнал в USB модуля, если к нему подключён компьютер.
// Читает flash: вызывать не из задачи обмена с Z80.
void bringupPrintStoredLog();

}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
