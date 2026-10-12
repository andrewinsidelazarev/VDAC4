#pragma once

#include <stdint.h>

#include "zifi/board.hpp"

#if ZIFI_HAS_SPI_LINK

namespace zifi {

// Где сейчас модуль ESP. USB подключают к модулю и вынув его из платы, и прямо в
// плате при выключенной Evo: диод D1 не пускает 5 В от USB в плату, и вся она
// остаётся без питания. Выходы ESP в таком состоянии вели бы обесточенные
// микросхемы. Буферам платы это не вредит, а входы ЦАП PCM5102 на это не
// рассчитаны, поэтому I2S в таком состоянии не запускается.
enum class BoardState : uint8_t {
  kAbsent,     // модуль вынут из платы
  kPowered,    // в плате, плата под питанием
  kUnpowered,  // в плате, плата без питания (модуль питается от USB)
  kUnknown,
};

// Определяется по линии LINK CS: на плате она подтянута резистором R10 (10 кОм)
// к +3,3 В логики платы, а встроенные подтяжки ESP впятеро слабее. Занимает
// меньше миллисекунды; линия остаётся входом без подтяжек.
BoardState probeBoardState();
const char* boardStateName(BoardState state);

}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
