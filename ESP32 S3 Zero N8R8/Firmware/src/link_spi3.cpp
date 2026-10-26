#include "zifi/link_spi3.hpp"

#if ZIFI_HAS_SPI_LINK

#include <string.h>

#include <driver/gpio.h>
#include <esp_cpu.h>
#include <esp_log.h>
#include <esp_private/esp_gpio_reserve.h>
#include <esp_private/gpio.h>
#include <esp_private/periph_ctrl.h>
#include <hal/gpio_ll.h>
#include <hal/spi_hal.h>
#include <hal/spi_ll.h>
#include <soc/gpio_struct.h>
#include <soc/spi_periph.h>

namespace zifi {
namespace link_spi3 {
namespace {

constexpr char kTag[] = "zifi-spi3";
constexpr spi_host_device_t kHost = SPI3_HOST;
constexpr uint32_t kSourceHz = 80000000;      // APB — источник клока SPI по умолчанию
constexpr size_t kMaxWords = 16;               // FIFO контроллера — 64 байта
// Транзакция в 16 слов на 1 МГц длится ~0,2 мс; всё, что дольше 50 мс, — сбой
constexpr uint32_t kTimeoutCycles = 240000000 / 20;

spi_hal_context_t g_hal = {};
spi_hal_dev_config_t g_dev = {};
Pins g_pins = {};
uint32_t g_hz = 0;
bool g_ready = false;
uint32_t g_tx[kMaxWords];
uint32_t g_rx[kMaxWords];

// Вывод к сигналам SPI3 через матрицу GPIO — так же, как это делает драйвер ESP-IDF
// (spicommon_bus_initialize_io), но своим кодом.
void route(int pin, int inSignal, int outSignal) {
  const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
  if (inSignal != -1) {
    gpio_matrix_input(gpio, inSignal, false);
  }
  if (outSignal != -1) {
    esp_gpio_reserve(BIT64(pin));
    gpio_matrix_output(gpio, outSignal, false, false);
  }
  gpio_func_sel(gpio, PIN_FUNC_GPIO);
}

}  // namespace

bool setClock(uint32_t hz) {
  spi_hal_timing_param_t timing = {};
  timing.clk_src_hz = kSourceHz;
  timing.half_duplex = 1;
  // Без лишних тактов разворота: их число на линии должно совпадать с ведомым (Tang)
  timing.no_compensate = 1;
  timing.expected_freq = hz;
  timing.duty_cycle = 128;
  timing.input_delay_ns = 0;
  timing.use_gpio = true;
  spi_hal_timing_conf_t conf = {};
  if (spi_hal_cal_clock_conf(&timing, &conf) != ESP_OK) {
    ESP_LOGE(kTag, "clock %u Hz is not supported", static_cast<unsigned>(hz));
    return false;
  }
  conf.clock_source = SPI_CLK_SRC_DEFAULT;
  conf.source_pre_div = 1;
  conf.source_real_freq = kSourceHz;
  conf.rx_sample_point = SPI_SAMPLING_POINT_PHASE_0;
  g_dev.timing_conf = conf;
  spi_hal_setup_device(&g_hal, &g_dev);
  PERIPH_RCC_ATOMIC() {
    spi_ll_set_clk_source(g_hal.hw, SPI_CLK_SRC_DEFAULT);
  }
  g_hz = static_cast<uint32_t>(conf.real_freq);
  return true;
}

uint32_t clockHz() { return g_hz; }

bool begin(const Pins& pins, uint32_t hz) {
  if (g_ready) {
    return setClock(hz);
  }
  g_pins = pins;
  // CS — обычный выход, неактивен
  gpio_set_level(static_cast<gpio_num_t>(pins.cs), 1);
  gpio_config_t cs = {};
  cs.pin_bit_mask = 1ULL << pins.cs;
  cs.mode = GPIO_MODE_OUTPUT;
  gpio_config(&cs);
  gpio_set_level(static_cast<gpio_num_t>(pins.cs), 1);

  // Контроллер: тактирование шины, сброс, тактирование ядра SPI
  PERIPH_RCC_ATOMIC() {
    spi_ll_enable_bus_clock(kHost, true);
    spi_ll_reset_register(kHost);
  }
  PERIPH_RCC_ATOMIC() {
    spi_ll_enable_clock(kHost, true);
  }
  spi_hal_init(&g_hal, kHost);
  spi_hal_set_data_pin_idle_level(&g_hal, false);

  g_dev = {};
  g_dev.mode = 0;
  g_dev.cs_setup = 0;
  g_dev.cs_hold = 0;
  g_dev.cs_pin_id = -1;            // аппаратные CS не используются
  g_dev.half_duplex = 1;
  if (!setClock(hz)) {
    return false;
  }

  // Линии: IO0…IO3 двунаправленные (направлением в фазах управляет контроллер), SCK — выход
  const spi_signal_conn_t& s = spi_periph_signal[kHost];
  route(pins.io0, s.spid_in, s.spid_out);
  route(pins.io1, s.spiq_in, s.spiq_out);
  route(pins.io2, s.spiwp_in, s.spiwp_out);
  route(pins.io3, s.spihd_in, s.spihd_out);
  route(pins.sck, -1, s.spiclk_out);
  g_ready = true;
  return true;
}

bool transfer(uint8_t command, uint32_t address, uint32_t* words, size_t count, bool reading,
              int dummyCycles) {
  if (!g_ready || count == 0 || count > kMaxWords) {
    return false;
  }
  spi_hal_trans_config_t trans = {};
  trans.cmd = command;
  trans.cmd_bits = 8;
  trans.addr = address;
  trans.addr_bits = 32;
  trans.dummy_bits = reading ? dummyCycles : 0;
  trans.line_mode.cmd_lines = 1;
  trans.line_mode.addr_lines = 4;
  trans.line_mode.data_lines = 4;
  if (reading) {
    memset(g_rx, 0, count * 4);
    trans.rx_bitlen = static_cast<int>(count * 32);
    trans.rcv_buffer = reinterpret_cast<uint8_t*>(g_rx);
  } else {
    for (size_t i = 0; i < count; ++i) {
      g_tx[i] = __builtin_bswap32(words[i]);
    }
    trans.tx_bitlen = static_cast<int>(count * 32);
    trans.send_buffer = reinterpret_cast<uint8_t*>(g_tx);
  }

  gpio_set_level(static_cast<gpio_num_t>(g_pins.cs), 0);
  spi_hal_setup_trans(&g_hal, &g_dev, &trans);
  if (!reading) {
    spi_hal_push_tx_buffer(&g_hal, &trans);
  }
  spi_hal_enable_data_line(g_hal.hw, !reading, reading);
  spi_hal_user_start(&g_hal);
  const uint32_t started = esp_cpu_get_cycle_count();
  bool done = false;
  while (!(done = spi_hal_usr_is_done(&g_hal))) {
    if (esp_cpu_get_cycle_count() - started > kTimeoutCycles) {
      // Задачу могли прервать между проверками дольше срока (другая задача, запись во flash
      // ESP с остановкой кэша) — обмен тогда давно закончен: решает последняя проверка
      done = spi_hal_usr_is_done(&g_hal);
      break;
    }
  }
  if (done && reading) {
    spi_hal_fetch_result(&g_hal);
  }
  spi_hal_clear_intr_mask(&g_hal, SPI_LL_INTR_TRANS_DONE);
  gpio_set_level(static_cast<gpio_num_t>(g_pins.cs), 1);
  if (!done) {
    ESP_LOGE(kTag, "transaction did not finish");
    return false;
  }
  if (reading) {
    for (size_t i = 0; i < count; ++i) {
      words[i] = __builtin_bswap32(g_rx[i]);
    }
  }
  return true;
}

bool writeRepeat(uint8_t command, uint32_t address, const uint32_t* words, size_t count) {
  if (!g_ready || count == 0) {
    return false;
  }
  spi_hal_trans_config_t trans = {};
  trans.cmd = command;
  trans.cmd_bits = 8;
  trans.addr = address;
  trans.addr_bits = 32;
  trans.line_mode.cmd_lines = 1;
  trans.line_mode.addr_lines = 4;
  trans.line_mode.data_lines = 4;
  trans.tx_bitlen = 32;
  trans.send_buffer = reinterpret_cast<uint8_t*>(g_tx);
  spi_hal_setup_trans(&g_hal, &g_dev, &trans);
  spi_hal_enable_data_line(g_hal.hw, true, false);
  spi_dev_t* hw = g_hal.hw;
  const gpio_num_t cs = static_cast<gpio_num_t>(g_pins.cs);
  for (size_t i = 0; i < count; ++i) {
    // Команда, адрес и длины — в регистрах с прошлой транзакции; слово — в буфер W0
    gpio_ll_set_level(&GPIO, cs, 0);
    hw->data_buf[0] = __builtin_bswap32(words[i]);
    spi_ll_apply_config(hw);
    spi_ll_user_start(hw);
    const uint32_t started = esp_cpu_get_cycle_count();
    bool done = false;
    while (!(done = spi_ll_usr_is_done(hw))) {
      if (esp_cpu_get_cycle_count() - started > kTimeoutCycles) {
        done = spi_ll_usr_is_done(hw);   // как в transfer(): решает последняя проверка
        break;
      }
    }
    spi_ll_clear_int_stat(hw);
    gpio_ll_set_level(&GPIO, cs, 1);
    if (!done) {
      ESP_LOGE(kTag, "transaction did not finish");
      return false;
    }
  }
  return true;
}

bool writeBurst(uint8_t command, uint32_t address, const uint32_t* words, size_t count) {
  if (!g_ready || count == 0) {
    return false;
  }
  size_t bits = (count < kMaxWords ? count : kMaxWords) * 32;
  spi_hal_trans_config_t trans = {};
  trans.cmd = command;
  trans.cmd_bits = 8;
  trans.addr = address;
  trans.addr_bits = 32;
  trans.line_mode.cmd_lines = 1;
  trans.line_mode.addr_lines = 4;
  trans.line_mode.data_lines = 4;
  trans.tx_bitlen = static_cast<int>(bits);
  trans.send_buffer = reinterpret_cast<uint8_t*>(g_tx);
  spi_hal_setup_trans(&g_hal, &g_dev, &trans);
  spi_hal_enable_data_line(g_hal.hw, true, false);
  spi_dev_t* hw = g_hal.hw;
  const gpio_num_t cs = static_cast<gpio_num_t>(g_pins.cs);
  for (size_t at = 0; at < count;) {
    // Команда и адрес — в регистрах с прошлой транзакции; слова — в буфер W0…W15, длина
    // меняется только у последней, неполной транзакции
    const size_t n = count - at < kMaxWords ? count - at : kMaxWords;
    if (n * 32 != bits) {
      bits = n * 32;
      spi_ll_set_mosi_bitlen(hw, bits);
    }
    gpio_ll_set_level(&GPIO, cs, 0);
    for (size_t i = 0; i < n; ++i) {
      hw->data_buf[i] = __builtin_bswap32(words[at + i]);
    }
    spi_ll_apply_config(hw);
    spi_ll_user_start(hw);
    const uint32_t started = esp_cpu_get_cycle_count();
    bool done = false;
    while (!(done = spi_ll_usr_is_done(hw))) {
      if (esp_cpu_get_cycle_count() - started > kTimeoutCycles) {
        done = spi_ll_usr_is_done(hw);   // как в transfer(): решает последняя проверка
        break;
      }
    }
    spi_ll_clear_int_stat(hw);
    gpio_ll_set_level(&GPIO, cs, 1);
    if (!done) {
      ESP_LOGE(kTag, "transaction did not finish");
      return false;
    }
    at += n;
  }
  return true;
}

}  // namespace link_spi3
}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
