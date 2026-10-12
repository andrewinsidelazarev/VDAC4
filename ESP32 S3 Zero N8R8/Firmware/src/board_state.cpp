#include "zifi/board_state.hpp"

#if ZIFI_HAS_SPI_LINK

#include <driver/gpio.h>
#include <esp_rom_sys.h>

namespace zifi {
namespace {

// Время установления уровня, заданного встроенной подтяжкой (десятки кОм).
constexpr uint32_t kSettleUs = 300;

}  // namespace

BoardState probeBoardState() {
  const gpio_num_t gpio = static_cast<gpio_num_t>(board::kLinkCsPin);
  gpio_set_direction(gpio, GPIO_MODE_INPUT);
  gpio_pullup_dis(gpio);
  gpio_pulldown_en(gpio);
  esp_rom_delay_us(kSettleUs);
  const int pulledDown = gpio_get_level(gpio);
  gpio_pulldown_dis(gpio);
  gpio_pullup_en(gpio);
  esp_rom_delay_us(kSettleUs);
  const int pulledUp = gpio_get_level(gpio);
  gpio_pullup_dis(gpio);

  if (pulledDown == 0 && pulledUp == 1) {
    return BoardState::kAbsent;       // линия свободна
  }
  if (pulledDown == 1 && pulledUp == 1) {
    return BoardState::kPowered;      // R10 держит её у +3,3 В
  }
  if (pulledDown == 0 && pulledUp == 0) {
    return BoardState::kUnpowered;    // R10 тянет её к обесточенной шине
  }
  return BoardState::kUnknown;
}

const char* boardStateName(BoardState state) {
  switch (state) {
    case BoardState::kAbsent:
      return "the module is out of the board";
    case BoardState::kPowered:
      return "in the board, board is powered";
    case BoardState::kUnpowered:
      return "in the board, board is NOT powered";
    default:
      return "unknown";
  }
}

}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
