#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zifi/board.hpp"

namespace zifi {

#if ZIFI_HAS_SPI_LINK

// Причина последней паники. Обработчик паники (крючок Arduino-ядра над
// esp_panic_handler, ключ --wrap в CMakeLists.txt) кладёт её в RTC-память, которая
// переживает перезапуск; при загрузке crashRecordReport() пишет её в журнал
// наладки и в сетевую консоль: печать самой паники уходит только в USB, а в
// плате USB не подключён. Загруженные модули ESP отмечают, где лежат, — адрес
// из модуля пишется как «ИМЯ+смещение» для addr2line по ELF модуля. Паника в
// коде модуля: в запись идут 16 байтов кода у PC — как они в памяти и как были
// при загрузке (pristine — копия кода модуля, nullptr — копии нет).
void crashRecordReport();
void crashRecordModule(const char* name, const void* base, size_t size, uint32_t vaddr,
                       const void* pristine = nullptr);
void crashRecordModuleGone(const void* base);
// Последняя температура кристалла (десятые доли °C) — в запись о панике.
void crashRecordTemperature(int16_t tenths);
#else
inline void crashRecordReport() {}
inline void crashRecordModule(const char*, const void*, size_t, uint32_t, const void* = nullptr) {}
inline void crashRecordModuleGone(const void*) {}
inline void crashRecordTemperature(int16_t) {}
#endif

}  // namespace zifi
