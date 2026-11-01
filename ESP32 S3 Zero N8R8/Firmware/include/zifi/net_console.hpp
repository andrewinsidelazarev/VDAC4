#pragma once

#include "zifi/board.hpp"

namespace zifi {

#if ZIFI_HAS_SPI_LINK

// Сетевая консоль платы VDAC4: TCP-порт 23. Подключившийся получает последние
// 32 КБ вывода прошивки — всё, что идёт через ESP_LOG* (сюда же попадают
// записи наладки bringupLog и сообщения модулей ESP), — и дальше всё новое,
// пока подключён. Команды строкой: help, mem, ver, log (постоянный журнал из
// flash), tang (регистры Tang по LINK). Всё, кроме «tang wr», только читает;
// «tang wr» пишет регистр Tang — как меню настроек Tang.
//
// netConsoleBegin — как можно раньше после запуска (перехват вывода начинается
// с него); сервер ждёт Wi-Fi сам.
void netConsoleBegin(const char* firmware);

// Службы платы (консоль, порт 8268) с клиентом: пока клиент есть, Wi-Fi не спит (main.cpp).
void netServiceClient(bool active);
bool netServiceClientActive();
// Сокет клиента службы: мёртвого клиента (его FIN потерялся) TCP заметит сам за ~30 с. Без этого
// задача ждала бы его вечно, а при listen(…, 1) следующий клиент соединяется и не получает ничего.
void netServiceSocketOptions(int client);
// Wi-Fi оборвался: счёт и причина для команды ver и журнала.
void netWifiDropped(unsigned reason);
// Из сетевой задачи, часто: раз в 5 с счёт загрузки по задачам (команда top), раз в минуту —
// температура, RSSI, сон Wi-Fi, обрывы и загрузка во flash-журнал; он переживает пропажу связи.
void netDiagnosticPoll(uint32_t now);

#else

inline void netConsoleBegin(const char*) {}
inline void netServiceClient(bool) {}
inline bool netServiceClientActive() { return false; }
inline void netServiceSocketOptions(int) {}
inline void netWifiDropped(unsigned) {}
inline void netDiagnosticPoll(uint32_t) {}

#endif

}  // namespace zifi
