#pragma once

// Модель контроллера SPI3 и канала GDMA ESP32-S3 для стенда src/link_spi3.cpp на ПК
// (tests/link_spi3_host.cpp, 0.1.176). Заголовки ESP-IDF, которые подключает link_spi3.cpp
// (hal/spi_hal.h, hal/spi_ll.h, esp_private/gdma.h …), в этой папке — короткие заглушки: все они
// ведут сюда. Модель не повторяет регистры по битам; она следит за тем, что важно для LINK:
//   - транзакция стартует только при низком CS, и CS поднимался после прошлой (каждая — в рамке CS);
//   - CS не поднимается, пока транзакция идёт (иначе ведомый Tang оборвёт её посреди слова);
//   - пока транзакция идёт, контроллер не перенастраивают и канал DMA не сбрасывают;
//   - слова «на линии» берутся в конце транзакции: из буфера W0…W15 или из буфера дескриптора DMA
//     (байты в порядке памяти, слово — старшим байтом вперёд). Кто перепишет буфер, который ещё
//     уходит, получит на линии чужие слова — стенд это увидит.
// Транзакция идёт flightPolls опросов флага «готово»; hang — не кончается никогда.

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <malloc.h>
#include <string>
#include <vector>

#define __builtin_bswap32(x) static_cast<uint32_t>(_byteswap_ulong(static_cast<unsigned long>(x)))

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NOT_FOUND 0x105

// ---------------------------------------------------------------- регистры (только нужное)
struct spi_dev_t {
  uint32_t data_buf[16];
  struct {
    uint32_t outfifo_empty_err;
    uint32_t trans_done;
  } dma_int_raw, dma_int_clr;
  // ниже — состояние модели, в железе это разные регистры
  uint32_t cmd;
  int cmd_bits;
  uint32_t addr;
  int addr_bits;
  int dummy;
  size_t mosi_bits;
  size_t miso_bits;
  bool mosi_en;
  bool miso_en;
  bool dma_tx_ena;
};

struct dma_descriptor_t {
  struct {
    uint32_t size : 12;
    uint32_t length : 12;
    uint32_t reversed24_27 : 4;
    uint32_t err_eof : 1;
    uint32_t reserved29 : 1;
    uint32_t suc_eof : 1;
    uint32_t owner : 1;
  } dw0;
  void* buffer;
  dma_descriptor_t* next;
};
#define DMA_DESCRIPTOR_BUFFER_OWNER_DMA 1

struct gdma_channel_t {
  int id;
};
typedef gdma_channel_t* gdma_channel_handle_t;

struct Transaction {
  uint8_t cmd = 0;
  uint32_t addr = 0;
  bool reading = false;
  bool dma = false;
  std::vector<uint32_t> words;
};

struct Model {
  spi_dev_t hw = {};
  int csPin = -1;
  bool cs = true;             // уровень CS
  bool framed = true;         // CS поднимался после прошлого старта
  bool running = false;       // транзакция идёт
  bool runningDma = false;
  int pollsLeft = 0;
  int flightPolls = 3;
  bool hang = false;          // следующая транзакция не кончится
  bool hangThis = false;      // идущая транзакция не кончится
  bool starve = false;        // DMA «не успевает» (флаг outfifo_empty_err в конце)
  Transaction current;
  std::vector<Transaction> log;
  std::vector<std::string> errors;
  uint64_t cycles = 0;
  // GDMA
  int channelsFree = 1;
  int channelsLive = 0;
  bool connected = false;
  bool resetSinceStart = false;
  const dma_descriptor_t* started = nullptr;   // дескриптор, с которого запущен канал
  // куча
  size_t freeInternal = 300000;
  bool failAlloc = false;
  int liveAllocs = 0;
  uint32_t lastCaps = 0;
  int logErrors = 0;

  void error(const char* format, ...) {
    char text[256];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    errors.push_back(text);
  }

  // Ответ ведомого на чтение: слово по адресу
  static uint32_t pattern(uint32_t address) { return 0xA5000000u ^ (address * 2654435761u); }

  void start() {
    if (running) error("start while a transaction runs");
    if (cs) error("start with CS high");
    if (!framed) error("start without CS rising since the previous start");
    framed = false;
    current = Transaction{};
    current.cmd = static_cast<uint8_t>(hw.cmd);
    current.addr = hw.addr;
    current.reading = hw.miso_en;
    current.dma = !current.reading && hw.dma_tx_ena;
    if (current.dma && (started == nullptr || !resetSinceStart)) error("DMA start without channel reset/start");
    if (!current.reading && !current.dma) {
      for (size_t i = 0; i < hw.mosi_bits / 32; ++i) current.words.push_back(__builtin_bswap32(hw.data_buf[i]));
    }
    running = true;
    runningDma = current.dma;
    hangThis = hang;
    pollsLeft = flightPolls;
    hw.dma_int_raw.trans_done = 0;
  }

  void complete() {
    if (current.dma) {
      const dma_descriptor_t* d = started;
      if (d->dw0.owner != DMA_DESCRIPTOR_BUFFER_OWNER_DMA || !d->dw0.suc_eof || d->next != nullptr)
        error("bad descriptor");
      if (d->dw0.length != hw.mosi_bits / 8 || d->dw0.size < d->dw0.length)
        error("descriptor length %u, MOSI %u bits", static_cast<unsigned>(d->dw0.length),
              static_cast<unsigned>(hw.mosi_bits));
      const uint8_t* b = static_cast<const uint8_t*>(d->buffer);
      for (size_t i = 0; i + 4 <= d->dw0.length; i += 4) {
        current.words.push_back((uint32_t(b[i]) << 24) | (uint32_t(b[i + 1]) << 16) |
                                (uint32_t(b[i + 2]) << 8) | b[i + 3]);
      }
      started = nullptr;
      resetSinceStart = false;
      if (starve) hw.dma_int_raw.outfifo_empty_err = 1;
    }
    if (current.reading) {
      for (size_t i = 0; i < hw.miso_bits / 32; ++i) {
        const uint32_t w = pattern(current.addr + 4 * static_cast<uint32_t>(i));
        current.words.push_back(w);
        hw.data_buf[i] = __builtin_bswap32(w);
      }
    }
    log.push_back(current);
    running = false;
    runningDma = false;
    hw.dma_int_raw.trans_done = 1;
  }

  bool poll() {
    if (running && !hangThis && --pollsLeft <= 0) complete();
    return hw.dma_int_raw.trans_done != 0;
  }

  void setCs(int pin, uint32_t level) {
    if (pin != csPin) return;
    if (level) {
      if (running && !hangThis) error("CS raised while a transaction runs");
      if (!cs) framed = true;
      cs = true;
    } else {
      cs = false;
    }
  }
};

inline Model g_model;

// ---------------------------------------------------------------- GPIO
typedef int gpio_num_t;
struct gpio_dev_t {
  int unused;
};
inline gpio_dev_t GPIO;
#define GPIO_MODE_OUTPUT 2
struct gpio_config_t {
  uint64_t pin_bit_mask;
  int mode;
  int pull_up_en;
  int pull_down_en;
  int intr_type;
};
inline esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level) {
  g_model.setCs(pin, level);
  return ESP_OK;
}
inline esp_err_t gpio_config(const gpio_config_t*) { return ESP_OK; }
inline void gpio_ll_set_level(gpio_dev_t*, gpio_num_t pin, uint32_t level) { g_model.setCs(pin, level); }
#define BIT64(n) (1ULL << (n))
#define PIN_FUNC_GPIO 1
inline uint64_t esp_gpio_reserve(uint64_t mask) { return mask; }
inline void gpio_matrix_input(gpio_num_t, int, bool) {}
inline void gpio_matrix_output(gpio_num_t, int, bool, bool) {}
inline void gpio_func_sel(gpio_num_t, int) {}

// ---------------------------------------------------------------- процессор, журнал, куча
inline uint32_t esp_cpu_get_cycle_count() {
  g_model.cycles += 100000;   // срок 12 млн тактов — ~120 опросов
  return static_cast<uint32_t>(g_model.cycles);
}
#define ESP_LOGE(tag, format, ...) (++g_model.logErrors)

#define MALLOC_CAP_8BIT (1u << 2)
#define MALLOC_CAP_DMA (1u << 3)
#define MALLOC_CAP_INTERNAL (1u << 11)
inline size_t heap_caps_get_free_size(uint32_t) { return g_model.freeInternal; }
inline void* heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps) {
  g_model.lastCaps = caps;
  if (g_model.failAlloc) return nullptr;
  ++g_model.liveAllocs;
  return _aligned_malloc(size, alignment);
}
inline void heap_caps_free(void* pointer) {
  if (pointer != nullptr) --g_model.liveAllocs;
  _aligned_free(pointer);
}

// ---------------------------------------------------------------- такты периферии
#define PERIPH_RCC_ATOMIC() for (int periph_rcc_once = 0; periph_rcc_once < 1; ++periph_rcc_once)

// ---------------------------------------------------------------- SPI (HAL и LL)
typedef enum { SPI1_HOST = 0, SPI2_HOST = 1, SPI3_HOST = 2 } spi_host_device_t;
#define SPI_CLK_SRC_DEFAULT 1
#define SPI_SAMPLING_POINT_PHASE_0 0
#define SPI_LL_INTR_TRANS_DONE 1

struct spi_hal_timing_param_t {
  uint32_t clk_src_hz;
  int half_duplex;
  int no_compensate;
  uint32_t expected_freq;
  int duty_cycle;
  int input_delay_ns;
  bool use_gpio;
};
struct spi_hal_timing_conf_t {
  int clock_source;
  int source_pre_div;
  uint32_t source_real_freq;
  int rx_sample_point;
  int real_freq;
  int timing_dummy;
  int timing_miso_delay;
};
struct spi_hal_dev_config_t {
  int mode;
  int cs_setup;
  int cs_hold;
  int cs_pin_id;
  int half_duplex;
  spi_hal_timing_conf_t timing_conf;
};
struct spi_line_mode_t {
  uint8_t cmd_lines;
  uint8_t addr_lines;
  uint8_t data_lines;
};
struct spi_hal_trans_config_t {
  uint16_t cmd;
  int cmd_bits;
  int addr_bits;
  int dummy_bits;
  int tx_bitlen;
  int rx_bitlen;
  uint64_t addr;
  uint8_t* send_buffer;
  uint8_t* rcv_buffer;
  spi_line_mode_t line_mode;
  int cs_keep_active;
};
struct spi_hal_context_t {
  spi_dev_t* hw;
  spi_hal_trans_config_t trans_config;
};
struct spi_signal_conn_t {
  int spiclk_out;
  int spid_out;
  int spiq_out;
  int spiwp_out;
  int spihd_out;
  int spid_in;
  int spiq_in;
  int spiwp_in;
  int spihd_in;
};
inline spi_signal_conn_t spi_periph_signal[3] = {};

inline esp_err_t spi_hal_cal_clock_conf(const spi_hal_timing_param_t* timing, spi_hal_timing_conf_t* conf) {
  conf->real_freq = static_cast<int>(timing->expected_freq);
  return ESP_OK;
}
inline void spi_hal_setup_device(spi_hal_context_t*, const spi_hal_dev_config_t*) {}
inline void spi_ll_set_clk_source(spi_dev_t*, int) {}
inline void spi_ll_enable_bus_clock(spi_host_device_t, bool) {}
inline void spi_ll_reset_register(spi_host_device_t) {}
inline void spi_ll_enable_clock(spi_host_device_t, bool) {}
inline void spi_hal_init(spi_hal_context_t* hal, uint32_t) {
  memset(hal, 0, sizeof(*hal));
  hal->hw = &g_model.hw;
}
inline void spi_hal_set_data_pin_idle_level(spi_hal_context_t*, bool) {}

inline void spi_hal_setup_trans(spi_hal_context_t* hal, const spi_hal_dev_config_t*, const spi_hal_trans_config_t* t) {
  if (g_model.running && !g_model.hangThis) g_model.error("setup_trans while a transaction runs");
  spi_dev_t* hw = hal->hw;
  hw->dma_int_raw.trans_done = 0;
  hw->cmd = t->cmd;
  hw->cmd_bits = t->cmd_bits;
  hw->addr = static_cast<uint32_t>(t->addr);
  hw->addr_bits = t->addr_bits;
  hw->dummy = t->dummy_bits;
  hw->mosi_bits = static_cast<size_t>(t->tx_bitlen);
  hw->miso_bits = static_cast<size_t>(t->rx_bitlen);
  hal->trans_config = *t;
}
inline void spi_hal_push_tx_buffer(const spi_hal_context_t* hal, const spi_hal_trans_config_t* t) {
  memcpy(hal->hw->data_buf, t->send_buffer, (static_cast<size_t>(t->tx_bitlen) + 7) / 8);
}
inline void spi_hal_enable_data_line(spi_dev_t* hw, bool mosi, bool miso) {
  hw->mosi_en = mosi;
  hw->miso_en = miso;
}
inline void spi_ll_apply_config(spi_dev_t*) {}
inline void spi_ll_user_start(spi_dev_t*) { g_model.start(); }
inline bool spi_ll_usr_is_done(spi_dev_t*) { return g_model.poll(); }
inline void spi_ll_clear_int_stat(spi_dev_t* hw) { hw->dma_int_raw.trans_done = 0; }
inline void spi_ll_set_mosi_bitlen(spi_dev_t* hw, size_t bits) {
  if (g_model.running && !g_model.hangThis) g_model.error("MOSI length changed while a transaction runs");
  hw->mosi_bits = bits;
}
inline void spi_ll_dma_tx_enable(spi_dev_t* hw, bool enable) {
  if (g_model.running && !g_model.hangThis) g_model.error("dma_tx_ena changed while a transaction runs");
  hw->dma_tx_ena = enable;
}
inline void spi_hal_hw_prepare_tx(spi_dev_t* hw) {
  if (g_model.running) g_model.error("prepare_tx while a transaction runs");
  hw->dma_tx_ena = true;
  hw->dma_int_raw.outfifo_empty_err = 0;
}
inline void spi_hal_user_start(const spi_hal_context_t* hal) {
  spi_ll_apply_config(hal->hw);
  spi_ll_user_start(hal->hw);
}
inline bool spi_hal_usr_is_done(const spi_hal_context_t* hal) { return spi_ll_usr_is_done(hal->hw); }
inline void spi_hal_fetch_result(const spi_hal_context_t* hal) {
  memcpy(hal->trans_config.rcv_buffer, hal->hw->data_buf,
         (static_cast<size_t>(hal->trans_config.rx_bitlen) + 7) / 8);
}
inline void spi_hal_clear_intr_mask(spi_hal_context_t* hal, uint32_t) { hal->hw->dma_int_raw.trans_done = 0; }

// ---------------------------------------------------------------- GDMA
struct gdma_channel_alloc_config_t {
  struct {
    int isr_cache_safe : 1;
  } flags;
};
struct gdma_transfer_config_t {
  uint32_t max_data_burst_size;
  bool access_ext_mem;
};
struct gdma_trigger_t {
  int instance_id;
  int bus_id;
};
#define GDMA_MAKE_TRIGGER(peri, id) (gdma_trigger_t{id, 0})

inline esp_err_t gdma_new_ahb_channel(const gdma_channel_alloc_config_t*, gdma_channel_handle_t* tx,
                                      gdma_channel_handle_t* rx) {
  if (rx != nullptr || tx == nullptr) return ESP_FAIL;
  if (g_model.channelsFree <= 0) return ESP_ERR_NOT_FOUND;
  --g_model.channelsFree;
  ++g_model.channelsLive;
  *tx = new gdma_channel_t{g_model.channelsLive};
  return ESP_OK;
}
inline esp_err_t gdma_connect(gdma_channel_handle_t, gdma_trigger_t trigger) {
  if (trigger.instance_id != 3) g_model.error("GDMA connected to SPI%d", trigger.instance_id);
  g_model.connected = true;
  return ESP_OK;
}
inline esp_err_t gdma_disconnect(gdma_channel_handle_t) {
  const bool was = g_model.connected;
  g_model.connected = false;
  return was ? ESP_OK : ESP_FAIL;
}
inline esp_err_t gdma_config_transfer(gdma_channel_handle_t, const gdma_transfer_config_t* config) {
  if (config->access_ext_mem) g_model.error("GDMA set up for external memory");
  return ESP_OK;
}
inline esp_err_t gdma_del_channel(gdma_channel_handle_t channel) {
  delete channel;
  ++g_model.channelsFree;
  --g_model.channelsLive;
  return ESP_OK;
}
inline esp_err_t gdma_reset(gdma_channel_handle_t) {
  if (g_model.running) g_model.error("GDMA reset while a transaction runs");
  g_model.resetSinceStart = true;
  g_model.started = nullptr;
  return ESP_OK;
}
inline esp_err_t gdma_start(gdma_channel_handle_t, intptr_t desc) {
  if (g_model.running) g_model.error("GDMA start while a transaction runs");
  if (!g_model.connected) g_model.error("GDMA start without connect");
  g_model.started = reinterpret_cast<const dma_descriptor_t*>(desc);
  return ESP_OK;
}
inline esp_err_t gdma_stop(gdma_channel_handle_t) {
  // Стоп после срока: модель считает транзакцию брошенной
  g_model.running = false;
  g_model.runningDma = false;
  g_model.started = nullptr;
  return ESP_OK;
}
