#include "zifi/link_spi3.hpp"

#if ZIFI_HAS_SPI_LINK

#include <string.h>

#include <atomic>

#include <driver/gpio.h>
#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_private/esp_gpio_reserve.h>
#include <esp_private/gdma.h>
#include <esp_private/gpio.h>
#include <esp_private/periph_ctrl.h>
#include <hal/dma_types.h>
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

// --- Запись через DMA (0.1.176, link_spi3.hpp) ---
// Один кусок внутреннего ОЗУ, видного DMA: два буфера по kDmaWords слов и два дескриптора. Дескриптор
// — запись для GDMA «откуда читать и сколько байт»: адрес буфера, длина, «последний» и «чей» (owner:
// DMA может его взять). Выделяется в dmaPrepare, отдаётся в dmaRelease.
constexpr size_t kDmaBufferBytes = kDmaWords * sizeof(uint32_t);
void* g_dmaBlock = nullptr;
uint32_t* g_dmaBuffer[2] = {};
dma_descriptor_t* g_dmaDesc = nullptr;           // два подряд: свой у каждого буфера
gdma_channel_handle_t g_dmaChannel = nullptr;    // канал GDMA «память → SPI3»
int g_dmaNext = 0;                               // в какой буфер копировать следующий кусок
// Транзакция DMA запущена и, возможно, ещё идёт: CS низкий, контроллер берёт данные из DMA.
// Меняется только под замком LINK (все обмены модуля — под ним, tang_link.cpp).
bool g_pending = false;
// Транзакция DMA не кончилась за срок, а сообщить было некому (её дождался чужой обмен или её не
// ждали): ошибку вернёт следующий writeBurstDma или dmaWait.
bool g_dmaError = false;
// Счётчики для консоли (dmaStats): пишет задача под замком LINK, читает консоль без него.
std::atomic<uint32_t> g_dmaCalls{0};
std::atomic<uint32_t> g_dmaTransactions{0};
std::atomic<uint64_t> g_dmaWords{0};
std::atomic<uint32_t> g_dmaStarved{0};
std::atomic<uint32_t> g_dmaFailed{0};

// Дождаться транзакции DMA, если она идёт: конец — флаг trans_done контроллера; затем CS вверх и
// контроллер снова на буфер W0…W15 (dma_tx_ena = 0), чтобы обычные обмены шли как прежде.
// true — транзакции не было или она кончилась. Срок отсчитывается от начала ожидания: транзакция
// в kDmaWords слов длится не больше ~4 мс даже на 1 МГц, а срок — 50 мс, как у остальных обменов.
bool finishPending() {
  if (!g_pending) {
    return true;
  }
  spi_dev_t* hw = g_hal.hw;
  const uint32_t started = esp_cpu_get_cycle_count();
  bool done = false;
  while (!(done = spi_ll_usr_is_done(hw))) {
    if (esp_cpu_get_cycle_count() - started > kTimeoutCycles) {
      done = spi_ll_usr_is_done(hw);   // как в transfer(): решает последняя проверка
      break;
    }
  }
  // outfifo_empty_err: DMA не успевал подавать слова, и контроллер ждал данных (в режиме ведущего
  // он при этом останавливает такты, а не шлёт мусор). Только для счётчика: данные не испорчены.
  if (hw->dma_int_raw.outfifo_empty_err) {
    ++g_dmaStarved;
    hw->dma_int_clr.outfifo_empty_err = 1;
  }
  spi_ll_clear_int_stat(hw);
  gpio_ll_set_level(&GPIO, static_cast<gpio_num_t>(g_pins.cs), 1);
  spi_ll_dma_tx_enable(hw, false);
  g_pending = false;
  if (!done) {
    gdma_stop(g_dmaChannel);
    ++g_dmaFailed;
    g_dmaError = true;
    ESP_LOGE(kTag, "DMA transaction did not finish");
    return false;
  }
  return true;
}

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
  finishPending();   // транзакция DMA без ожидания — до смены частоты
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
  if (!g_ready || count == 0 || count > kMaxWords || !finishPending()) {
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
  if (!g_ready || count == 0 || !finishPending()) {
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
  if (!g_ready || count == 0 || !finishPending()) {
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

// --- Запись через DMA (0.1.176) ---

bool dmaPrepared() { return g_dmaChannel != nullptr; }

bool dmaPrepare(size_t keepInternal) {
  if (g_dmaChannel != nullptr) {
    return true;
  }
  if (!g_ready) {
    return false;
  }
  // Буферы и дескрипторы — одним куском: меньше дробится куча. MALLOC_CAP_DMA — внутреннее ОЗУ,
  // которое видно GDMA; выравнивание 32 — под пачки GDMA по 32 байта (max_data_burst_size).
  const size_t bytes = 2 * kDmaBufferBytes + 2 * sizeof(dma_descriptor_t);
  const uint32_t caps = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < bytes + keepInternal) {
    return false;   // после выделения осталось бы меньше запаса ядра
  }
  void* block = heap_caps_aligned_alloc(32, bytes, caps);
  if (block == nullptr) {
    return false;
  }
  // Канал GDMA только на передачу (память → SPI3), без прерываний: конец транзакции ловит опрос
  // флага контроллера, как у остальных обменов. Каналов у ESP32-S3 пять; их делят I2S, SPI2 (обмен
  // с Evo) и другие — канал может и не найтись.
  gdma_channel_alloc_config_t alloc = {};
  gdma_channel_handle_t channel = nullptr;
  if (gdma_new_ahb_channel(&alloc, &channel, nullptr) != ESP_OK) {
    heap_caps_free(block);
    return false;
  }
  gdma_transfer_config_t transfer = {};
  transfer.max_data_burst_size = 32;   // данные — пачками по 32 байта: быстрее, чем по слову
  transfer.access_ext_mem = false;     // DMA читает только наши буферы во внутреннем ОЗУ
  if (gdma_connect(channel, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_SPI, 3)) != ESP_OK ||
      gdma_config_transfer(channel, &transfer) != ESP_OK) {
    gdma_disconnect(channel);   // не подключён — вернёт ошибку, это не страшно
    gdma_del_channel(channel);
    heap_caps_free(block);
    return false;
  }
  uint8_t* bytesAt = static_cast<uint8_t*>(block);
  g_dmaBuffer[0] = reinterpret_cast<uint32_t*>(bytesAt);
  g_dmaBuffer[1] = reinterpret_cast<uint32_t*>(bytesAt + kDmaBufferBytes);
  g_dmaDesc = reinterpret_cast<dma_descriptor_t*>(bytesAt + 2 * kDmaBufferBytes);
  memset(g_dmaDesc, 0, 2 * sizeof(dma_descriptor_t));
  g_dmaBlock = block;
  g_dmaNext = 0;
  g_dmaError = false;
  g_dmaChannel = channel;
  return true;
}

void dmaRelease() {
  if (g_dmaChannel == nullptr) {
    return;
  }
  finishPending();
  gdma_disconnect(g_dmaChannel);
  gdma_del_channel(g_dmaChannel);
  heap_caps_free(g_dmaBlock);
  g_dmaChannel = nullptr;
  g_dmaBlock = nullptr;
  g_dmaBuffer[0] = g_dmaBuffer[1] = nullptr;
  g_dmaDesc = nullptr;
  g_dmaError = false;
}

bool writeBurstDma(uint8_t command, uint32_t address, const uint32_t* words, size_t count, bool wait) {
  if (!g_ready || g_dmaChannel == nullptr || words == nullptr || count == 0) {
    return false;
  }
  if (g_dmaError) {
    // Прошлый «хвост» не кончился за срок, и о нём ещё не сказали: сказать сейчас, ничего не слав
    g_dmaError = false;
    return false;
  }
  ++g_dmaCalls;
  spi_dev_t* hw = g_hal.hw;
  const gpio_num_t cs = static_cast<gpio_num_t>(g_pins.cs);
  bool configured = false;
  size_t bits = 0;
  for (size_t at = 0; at < count;) {
    const size_t n = count - at < kDmaWords ? count - at : kDmaWords;
    // 1. Кусок — в свободный буфер, с разворотом байтов. В это время DMA ещё может слать прошлый
    //    кусок из другого буфера: копирование и передача идут одновременно.
    uint32_t* buffer = g_dmaBuffer[g_dmaNext];
    const uint32_t* from = words + at;
    for (size_t i = 0; i < n; ++i) {
      buffer[i] = __builtin_bswap32(from[i]);
    }
    // 2. Прошлая транзакция (этого вызова или прошлого, «без ожидания») — до конца, CS вверх
    if (!finishPending()) {
      g_dmaError = false;   // ошибка возвращается прямо сейчас
      return false;
    }
    // 3. Контроллер: команда, адрес, линии и длина — один раз за вызов; дальше меняется только
    //    длина у последней, неполной транзакции (как в writeBurst)
    if (!configured) {
      spi_hal_trans_config_t trans = {};
      trans.cmd = command;
      trans.cmd_bits = 8;
      trans.addr = address;
      trans.addr_bits = 32;
      trans.line_mode.cmd_lines = 1;
      trans.line_mode.addr_lines = 4;
      trans.line_mode.data_lines = 4;
      trans.tx_bitlen = static_cast<int>(n * 32);
      spi_hal_setup_trans(&g_hal, &g_dev, &trans);
      spi_hal_enable_data_line(hw, true, false);
      bits = n * 32;
      configured = true;
    } else if (n * 32 != bits) {
      bits = n * 32;
      spi_ll_set_mosi_bitlen(hw, bits);
    }
    // 4. Дескриптор: этот буфер, n слов, последний в цепочке, владелец — DMA
    dma_descriptor_t* desc = &g_dmaDesc[g_dmaNext];
    desc->dw0.size = static_cast<uint32_t>(n * 4);
    desc->dw0.length = static_cast<uint32_t>(n * 4);
    desc->dw0.suc_eof = 1;
    desc->dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
    desc->buffer = buffer;
    desc->next = nullptr;
    // 5. Пуск — в том же порядке, что у драйвера ESP-IDF (spi_master.c, s_spi_dma_prepare_data):
    //    сброс канала, сброс очереди DMA контроллера и dma_tx_ena = 1, старт канала (он сразу
    //    начинает наполнять очередь), потом CS и старт транзакции
    gdma_reset(g_dmaChannel);
    spi_hal_hw_prepare_tx(hw);
    gdma_start(g_dmaChannel, reinterpret_cast<intptr_t>(desc));
    gpio_ll_set_level(&GPIO, cs, 0);
    spi_ll_apply_config(hw);
    spi_ll_user_start(hw);
    g_pending = true;
    ++g_dmaTransactions;
    g_dmaWords += n;
    g_dmaNext ^= 1;
    at += n;
  }
  if (!wait) {
    return true;   // последняя транзакция ещё идёт: её дождётся следующий обмен или dmaWait
  }
  const bool ok = finishPending();
  g_dmaError = false;   // ошибка (если была) возвращается прямо сейчас
  return ok;
}

bool dmaWait() {
  finishPending();
  const bool ok = !g_dmaError;
  g_dmaError = false;
  return ok;
}

DmaStats dmaStats() {
  DmaStats stats;
  stats.calls = g_dmaCalls.load();
  stats.transactions = g_dmaTransactions.load();
  stats.words = g_dmaWords.load();
  stats.starved = g_dmaStarved.load();
  stats.failed = g_dmaFailed.load();
  return stats;
}

}  // namespace link_spi3
}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
