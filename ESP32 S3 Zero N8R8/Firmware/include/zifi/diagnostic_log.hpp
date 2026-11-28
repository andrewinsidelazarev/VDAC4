#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef ZIFI_DIAGNOSTIC_LOG
#define ZIFI_DIAGNOSTIC_LOG 0
#endif

namespace zifi {

// Текстовая копия журнала: запись о последнем падении (если она есть во flash) и строки этого
// запуска. Память выделяется преимущественно в PSRAM и должна быть освобождена
// diagnosticLogFreeSnapshot().
struct DiagnosticLogSnapshot {
  uint8_t* data;
  size_t length;
};

#if ZIFI_DIAGNOSTIC_LOG

// Журнал наладки (0.1.183). Строки — в ОЗУ: кольцо в PSRAM и хвост последних строк в RTC-памяти,
// которая переживает перезапуск ESP. Во flash журнал попадает только при запуске после падения (см.
// diagnosticLogCrashFile). Подготовить кольцо и хвост; хвост прошлого запуска откладывается до
// diagnosticLogCrashFile. Ошибка журнала не мешает основной работе ZiFi — вызывающий получает только
// false.
bool diagnosticLogBegin();

// Поставить одно событие в журнал. К каждой строке автоматически добавляются время от запуска,
// свободная и минимальная RAM, свободная и минимальная PSRAM. flash этот вызов не трогает и безопасен во
// время активного VFS-обмена с Z80.
void diagnosticLogEvent(const char* format, ...);

// То же, но если журнал занят другой задачей, ждёт его до timeoutMs, а не теряет запись. Для записей из
// несрочных задач.
void diagnosticLogEventWait(uint32_t timeoutMs, const char* format, ...);

// Один раз при запуске, после crashRecordReport (он пишет в журнал причину паники). Запуск после
// падения — паника, сторож задач или прерываний, аппаратный сторож или diagnosticLogHangRestart —
// пишет во flash один файл: последние строки прошлого запуска и первые строки этого. При обычном
// запуске прошлый такой файл (если он есть) только читается в PSRAM — его покажет
// diagnosticLogSnapshot. Единственное место, где журнал пишет во flash.
void diagnosticLogCrashFile();

// Прошивка сейчас сама перезапустит ESP, потому что та зависла (сторож обмена с Z80): следующий
// запуск сохранит журнал, как после падения.
void diagnosticLogHangRestart();

// Собрать текст: запись о последнем падении, затем строки этого запуска от старой к новой. flash не
// читает: запись о падении уже лежит в PSRAM.
bool diagnosticLogSnapshot(DiagnosticLogSnapshot& snapshot);
void diagnosticLogFreeSnapshot(DiagnosticLogSnapshot& snapshot);

#else

// В обычной прошивке журнал выключен на этапе компиляции. Макрос удаляет не
// только вызов, но и его форматную строку с вычислением аргументов. OTA оставляет
// совместимый интерфейс чтения, который явно сообщает, что снимка нет.
inline bool diagnosticLogBegin() { return false; }
#define diagnosticLogEvent(...) ((void)0)
#define diagnosticLogEventWait(...) ((void)0)
inline void diagnosticLogCrashFile() {}
inline void diagnosticLogHangRestart() {}
inline bool diagnosticLogSnapshot(DiagnosticLogSnapshot& snapshot) {
  snapshot.data = nullptr;
  snapshot.length = 0;
  return false;
}
inline void diagnosticLogFreeSnapshot(DiagnosticLogSnapshot& snapshot) {
  snapshot.data = nullptr;
  snapshot.length = 0;
}

#endif

}  // namespace zifi
