#include "zifi/board_state.hpp"

#if ZIFI_HAS_SPI_LINK

#include <atomic>

#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_rom_sys.h>

#include "zifi/tang_link.hpp"

namespace zifi {
namespace {

// Время установления уровня, заданного встроенной подтяжкой (десятки кОм).
constexpr uint32_t kSettleUs = 300;

// Пороги по напряжению на линии, мВ (АЦП без калибровки, шкала до ~3,1 В).
// С подтяжкой ESP вниз (~45 кОм): R10 от запитанной платы держит линию на 2,3 В (плата в Evo,
// 0.1.67); светодиод Tang на FPGA18 (510 Ом к +3,3 В Tang) при обесточенной плате даёт 1,5 В
// (стол, Tang от USB, 0.1.63) — R10 тогда тянет линию к мёртвой шине. Порог — посередине.
constexpr int kPoweredMv = 1900;
// С подтяжкой вверх: свободная линия (модуль вынут) поднимается почти до шкалы; R10 к
// обесточенной шине держит её ниже 1 В, светодиод Tang — ниже порога.
constexpr int kFreeHighMv = 2300;
constexpr int kLowMv = 400;

std::atomic<int> g_downMv{-1};
std::atomic<int> g_upMv{-1};

int readMillivolts(adc_oneshot_unit_handle_t unit, adc_channel_t channel) {
  esp_rom_delay_us(kSettleUs);
  int raw = 0;
  if (adc_oneshot_read(unit, channel, &raw) != ESP_OK) {
    return -1;
  }
  return raw * 3100 / 4095;
}

// Напряжение линии с подтяжкой вниз и вверх. false — АЦП занят (им пользуется проверка
// выводов звука) или вывод без канала АЦП.
bool measure(gpio_num_t gpio, int& down, int& up) {
  adc_unit_t unitId = ADC_UNIT_1;
  adc_channel_t channel = ADC_CHANNEL_0;
  if (adc_oneshot_io_to_channel(gpio, &unitId, &channel) != ESP_OK) {
    return false;
  }
  adc_oneshot_unit_handle_t unit = nullptr;
  adc_oneshot_unit_init_cfg_t unitConfig = {};
  unitConfig.unit_id = unitId;
  if (adc_oneshot_new_unit(&unitConfig, &unit) != ESP_OK) {
    return false;
  }
  adc_oneshot_chan_cfg_t channelConfig = {};
  channelConfig.atten = ADC_ATTEN_DB_12;
  channelConfig.bitwidth = ADC_BITWIDTH_DEFAULT;
  bool ok = false;
  // Настройка канала отключает у вывода вход, выход и подтяжки
  if (adc_oneshot_config_channel(unit, channel, &channelConfig) == ESP_OK) {
    gpio_pulldown_en(gpio);
    down = readMillivolts(unit, channel);
    gpio_pulldown_dis(gpio);
    gpio_pullup_en(gpio);
    up = readMillivolts(unit, channel);
    gpio_pullup_dis(gpio);
    ok = down >= 0 && up >= 0;
  }
  adc_oneshot_del_unit(unit);
  gpio_set_direction(gpio, GPIO_MODE_INPUT);
  return ok;
}

// Без АЦП — по логическим уровням (светодиод Tang так не отличить от питания платы).
BoardState probeDigital(gpio_num_t gpio) {
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

}  // namespace

BoardState probeBoardState() {
  // Линию занял LINK к Tang (tang_link.cpp): ответ — то, что было видно при его запуске.
  // На столе (Tang от USB) LINK работает и при обесточенной плате.
  if (tangLinkOwnsCs()) {
    return tangLinkBoardPowered() ? BoardState::kPowered : BoardState::kUnpowered;
  }
  const gpio_num_t gpio = static_cast<gpio_num_t>(board::kLinkCsPin);
  int down = -1;
  int up = -1;
  if (!measure(gpio, down, up)) {
    return probeDigital(gpio);
  }
  g_downMv = down;
  g_upMv = up;
  if (down >= kPoweredMv) {
    return BoardState::kPowered;
  }
  if (down < kLowMv && up >= kFreeHighMv) {
    return BoardState::kAbsent;
  }
  // Остальное — плата без питания: R10 тянет линию к мёртвой шине, а светодиод
  // запитанной от USB Tang может её приподнимать. Сомнение решается в пользу «нет
  // питания»: так I2S не поведёт входы обесточенного ЦАП.
  return BoardState::kUnpowered;
}

void lastBoardProbeMillivolts(int& down, int& up) {
  down = g_downMv;
  up = g_upMv;
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
