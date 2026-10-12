#pragma once

#include "zifi/board.hpp"

namespace zifi {

#if ZIFI_HAS_SPI_LINK

// Сетевая консоль платы VDAC4: TCP-порт 23. Подключившийся получает последние
// 32 КБ вывода прошивки — всё, что идёт через ESP_LOG* (сюда же попадают
// записи наладки bringupLog и сообщения модулей ESP), — и дальше всё новое,
// пока подключён. Команды строкой: help, mem, ver, log (постоянный журнал из
// flash). Консоль только читает: ничего не меняет и не записывает.
//
// netConsoleBegin — как можно раньше после запуска (перехват вывода начинается
// с него); сервер ждёт Wi-Fi сам.
void netConsoleBegin(const char* firmware);

#else

inline void netConsoleBegin(const char*) {}

#endif

}  // namespace zifi
