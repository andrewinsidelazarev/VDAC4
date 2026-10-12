#include "zifi/spi_link.hpp"

#if ZIFI_HAS_SPI_LINK

#include <Arduino.h>

#include <driver/gpio.h>
#include <driver/pulse_cnt.h>
#include <driver/spi_slave_hd.h>
#include <esp_attr.h>
#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_private/gpio.h>
#include <esp_random.h>
#include <esp_rom_gpio.h>
#include <esp_rom_sys.h>
#include <sdkconfig.h>
#include <soc/spi_periph.h>
#include <soc/spi_struct.h>
#include <string.h>

#include "zifi/bringup_log.hpp"

#if CONFIG_IDF_TARGET_ARCH_XTENSA
#include <xtensa/core-macros.h>
#endif

namespace zifi {
namespace {

constexpr char kTag[] = "zifi-spi";
constexpr spi_host_device_t kHost = SPI2_HOST;
// Задача обмена — самая срочная на ядре 1: пока она не ответит, Z80 ждёт.
constexpr UBaseType_t kTaskPriority = 20;
constexpr BaseType_t kTaskCore = 1;
constexpr uint32_t kTaskStackBytes = 4096;
constexpr uint32_t kStartTimeoutMs = 2000;
constexpr uint32_t kFlushTimeoutMs = 1000;
// Столько Z80 может не вести обмен, пока занята диском. Дольше — её программа
// завершилась: ждать места в очереди уже некого.
constexpr uint32_t kPeerQuietMs = 5000;
// Предел ожидания места, даже если Z80 обмен ведёт, а очередь не убывает.
constexpr uint32_t kReserveLimitMs = 30000;
// Столько отказов ожидания за запуск попадает в журнал наладки.
constexpr uint32_t kReserveReports = 12;
// И столько же записей о блоках DMA, вернувшихся с искажением.
constexpr uint32_t kBulkReports = 12;
// Сколько ждать, пока контроллер SPI примет блок или буфер под обратную посылку.
constexpr uint32_t kBulkLoadMs = 20;
// Номер прерывания внутреннего таймера 0 процессора Xtensa.
constexpr uint32_t kCpuTimer0Interrupt = 6;

// Счётчики линий: CS, SCK, MOSI — в этом порядке.
constexpr int kLineSelect = 0;
constexpr int kLineClock = 1;
constexpr int kLineData = 2;
// Счётчик PCNT 16-разрядный; показания забираются чаще, чем он переполняется.
constexpr int kCounterLimit = 30000;

// Самопроверка: полупериод SCK «ведущего» и время установления уровня, заданного
// резистором подтяжки (десятки кОм), при проверке, свободна ли линия.
constexpr uint32_t kHalfBitUs = 2;
constexpr uint32_t kProbeSettleUs = 300;
constexpr uint8_t kCommandWriteBuffer = 0x01;
constexpr uint8_t kCommandReadBuffer = 0x02;
constexpr uint32_t kSelfTestReplyMs = 100;
// READY ждём с запасом: цикл протокола мог ещё не начать работу.
constexpr uint32_t kSelfTestFrameMs = 3000;
// Кадры протокола ZiFi: PING от «Z80» и READY в ответ.
constexpr uint8_t kPingFrame[] = {0x5A, 0x04, 0x00, 0x00, 0x04};
constexpr uint8_t kReadyFrame[] = {0x5A, 0xF0, 0x00, 0x00, 0xF0};

// Блок, выставленный для чтения через DMA, и буфер под его обратную посылку
// загружены в контроллер: теперь о блоке можно сообщать Z80. Семафоры ждёт
// задача обмена.
SemaphoreHandle_t g_bulkReady = nullptr;
SemaphoreHandle_t g_echoReady = nullptr;

bool IRAM_ATTR giveFromIsr(SemaphoreHandle_t semaphore, int* awoken) {
  BaseType_t woken = pdFALSE;
  if (semaphore != nullptr) {
    xSemaphoreGiveFromISR(semaphore, &woken);
  }
  if (woken == pdTRUE) {
    *awoken = pdTRUE;
  }
  return true;
}

bool IRAM_ATTR onBulkReady(void*, spi_slave_hd_event_t*, int* awoken) {
  return giveFromIsr(g_bulkReady, awoken);
}

bool IRAM_ATTR onEchoReady(void*, spi_slave_hd_event_t*, int* awoken) {
  return giveFromIsr(g_echoReady, awoken);
}

// Поставить транзакцию DMA в очередь драйвера и дождаться её загрузки в
// контроллер. Описатель драйвер держит у себя до завершения транзакции.
bool loadDma(spi_slave_chan_t channel, spi_slave_hd_data_t& transaction,
             uint8_t* data, size_t length, SemaphoreHandle_t ready) {
  transaction = {};
  transaction.data = data;
  transaction.len = length;
  xSemaphoreTake(ready, 0);
  if (spi_slave_hd_queue_trans(kHost, channel, &transaction,
                               pdMS_TO_TICKS(kBulkLoadMs)) != ESP_OK) {
    return false;
  }
  xSemaphoreTake(ready, pdMS_TO_TICKS(kBulkLoadMs));
  return true;
}

// Ведущий закончил запись в общий буфер. Обработчик только будит задачу обмена:
// context — её описатель.
bool IRAM_ATTR onRequestWritten(void* context, spi_slave_hd_event_t*,
                                int* awoken) {
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(static_cast<TaskHandle_t>(context), &woken);
  if (woken == pdTRUE) {
    *awoken = pdTRUE;
  }
  return true;
}

// Счётчик перепадов на выводе. PCNT слушает вывод через матрицу GPIO; создаётся
// до запуска SPI, потому что при создании переводит вывод в режим GPIO, а SPI
// затем возвращает ему свою функцию — вход счётчика при этом остаётся.
pcnt_unit_handle_t makeEdgeCounter(int pin, bool rising, bool falling) {
  pcnt_unit_config_t unitConfig = {};
  unitConfig.low_limit = -1;
  unitConfig.high_limit = kCounterLimit;
  pcnt_unit_handle_t unit = nullptr;
  if (pcnt_new_unit(&unitConfig, &unit) != ESP_OK) {
    return nullptr;
  }
  pcnt_chan_config_t channelConfig = {};
  channelConfig.edge_gpio_num = pin;
  channelConfig.level_gpio_num = -1;
  pcnt_channel_handle_t channel = nullptr;
  const pcnt_channel_edge_action_t count = PCNT_CHANNEL_EDGE_ACTION_INCREASE;
  const pcnt_channel_edge_action_t hold = PCNT_CHANNEL_EDGE_ACTION_HOLD;
  if (pcnt_new_channel(unit, &channelConfig, &channel) != ESP_OK ||
      pcnt_channel_set_edge_action(channel, rising ? count : hold,
                                   falling ? count : hold) != ESP_OK ||
      pcnt_channel_set_level_action(channel, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                    PCNT_CHANNEL_LEVEL_ACTION_KEEP) != ESP_OK ||
      pcnt_unit_enable(unit) != ESP_OK ||
      pcnt_unit_clear_count(unit) != ESP_OK ||
      pcnt_unit_start(unit) != ESP_OK) {
    return nullptr;
  }
  return unit;
}

// --- самопроверка --------------------------------------------------------------

void pull(int pin, bool high) {
  const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
  if (high) {
    gpio_pulldown_dis(gpio);
    gpio_pullup_en(gpio);
  } else {
    gpio_pullup_dis(gpio);
    gpio_pulldown_en(gpio);
  }
}

// Следует ли уровень на выводе за подтяжкой, то есть свободен ли он.
bool floats(int pin) {
  const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
  pull(pin, false);
  esp_rom_delay_us(kProbeSettleUs);
  const int low = gpio_get_level(gpio);
  pull(pin, true);
  esp_rom_delay_us(kProbeSettleUs);
  const int high = gpio_get_level(gpio);
  return low == 0 && high == 1;
}

// Состояние входов без сигнала: ESP не выбрана, на тактовой линии тишина.
void idlePulls() {
  pull(board::kSpiCsPin, true);
  pull(board::kSpiSckPin, false);
  pull(board::kSpiMosiPin, true);
}

void drive(int pin, bool high) {
  gpio_set_level(static_cast<gpio_num_t>(pin), high ? 1 : 0);
}

// Сделать вход SPI выходом GPIO: на время проверки линией управляет сама
// прошивка, как это делала бы Evo. Вход SPI2 получает сигнал с того же вывода
// через матрицу GPIO: прямого пути у вывода в режиме GPIO нет.
void takeLine(int pin, uint32_t signal, bool high) {
  const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
  gpio_set_level(gpio, high ? 1 : 0);
  gpio_set_direction(gpio, GPIO_MODE_INPUT_OUTPUT);
  gpio_set_level(gpio, high ? 1 : 0);
  esp_rom_gpio_connect_in_signal(pin, signal, false);
}

// Вернуть выводу рабочее состояние: вход SPI2 мимо матрицы GPIO, выход выключен.
void releaseLine(int pin, uint32_t signal) {
  const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
  gpio_set_direction(gpio, GPIO_MODE_INPUT);
  const int function = spi_periph_signal[kHost].func;
  gpio_iomux_input(gpio, function, signal);
  gpio_iomux_output(gpio, function);
}

// Один байт SPI в режиме 0, старшим битом вперёд. MISO читается перед фронтом
// SCK: ведомый выставил бит по предыдущему спаду.
uint8_t exchangeByte(uint8_t out) {
  uint8_t in = 0;
  for (int bit = 7; bit >= 0; --bit) {
    drive(board::kSpiMosiPin, ((out >> bit) & 1) != 0);
    esp_rom_delay_us(kHalfBitUs);
    in = static_cast<uint8_t>(
        (in << 1) |
        (gpio_get_level(static_cast<gpio_num_t>(board::kSpiMisoPin)) & 1));
    drive(board::kSpiSckPin, true);
    esp_rom_delay_us(kHalfBitUs);
    drive(board::kSpiSckPin, false);
    esp_rom_delay_us(kHalfBitUs);
  }
  return in;
}

void selectSlave(bool active) {
  drive(board::kSpiCsPin, !active);
  esp_rom_delay_us(kHalfBitUs);
}

void writeShared(uint8_t address, const uint8_t* data, size_t length) {
  selectSlave(true);
  exchangeByte(kCommandWriteBuffer);
  exchangeByte(address);
  exchangeByte(0x00);
  for (size_t index = 0; index < length; ++index) {
    exchangeByte(data[index]);
  }
  selectSlave(false);
}

void readShared(uint8_t address, uint8_t* data, size_t length) {
  selectSlave(true);
  exchangeByte(kCommandReadBuffer);
  exchangeByte(address);
  exchangeByte(0xFF);
  for (size_t index = 0; index < length; ++index) {
    data[index] = exchangeByte(0xFF);
  }
  selectSlave(false);
}

// Отсчёты MISO вокруг фронта SCK при чтении трёх байтов общего буфера: перед
// фронтом и сразу после него. По ним видно, по какому перепаду SCK ведомый
// выставляет очередной бит.
struct MisoTrace {
  uint8_t beforeRise[3];
  uint8_t afterRise[3];
};

void traceShared(uint8_t address, MisoTrace& trace) {
  const gpio_num_t miso = static_cast<gpio_num_t>(board::kSpiMisoPin);
  selectSlave(true);
  exchangeByte(kCommandReadBuffer);
  exchangeByte(address);
  exchangeByte(0xFF);
  for (size_t index = 0; index < sizeof(trace.beforeRise); ++index) {
    uint8_t before = 0;
    uint8_t after = 0;
    for (int bit = 0; bit < 8; ++bit) {
      esp_rom_delay_us(kHalfBitUs);
      before = static_cast<uint8_t>((before << 1) | (gpio_get_level(miso) & 1));
      drive(board::kSpiSckPin, true);
      esp_rom_delay_us(kHalfBitUs);
      after = static_cast<uint8_t>((after << 1) | (gpio_get_level(miso) & 1));
      drive(board::kSpiSckPin, false);
    }
    trace.beforeRise[index] = before;
    trace.afterRise[index] = after;
  }
  esp_rom_delay_us(kHalfBitUs);
  selectSlave(false);
}

// Один обмен «Z80 -> ESP -> Z80» по правилам spi_link_core.hpp.
// Возвращает false, если верного ответа не было.
bool selfTurn(uint8_t tid, uint8_t flags, const uint8_t* data, uint8_t length,
              uint8_t room, uint8_t* reply, uint8_t& replyLength) {
  uint8_t request[SpiLinkCore::kTurnSize] = {};
  request[0] = SpiLinkCore::kRequestMarker;
  request[1] = tid;
  request[2] = flags;
  request[3] = length;
  request[4] = room;
  if (length != 0) {
    memcpy(request + SpiLinkCore::kHeaderSize, data, length);
  }
  const size_t requestSize = SpiLinkCore::kHeaderSize + length;
  request[requestSize] = SpiLinkCore::checksum(request, requestSize);
  writeShared(0, request, requestSize + 1);

  const uint32_t started = millis();
  uint8_t response[SpiLinkCore::kTurnSize] = {};
  do {
    delay(1);
    readShared(0, response, SpiLinkCore::kHeaderSize);
    if (response[0] != SpiLinkCore::kResponseMarker || response[1] != tid ||
        response[3] > room) {
      continue;
    }
    const size_t size = SpiLinkCore::kHeaderSize + response[3] + 1;
    readShared(0, response, size);
    if (response[0] == SpiLinkCore::kResponseMarker && response[1] == tid &&
        SpiLinkCore::checksum(response, size - 1) == response[size - 1] &&
        response[4] == length) {
      replyLength = response[3];
      memcpy(reply, response + SpiLinkCore::kHeaderSize, replyLength);
      return true;
    }
  } while (static_cast<uint32_t>(millis() - started) < kSelfTestReplyMs);
  bringupLog(kTag, "self-test: no reply to request %u, buffer starts %02X %02X %02X %02X %02X",
             static_cast<unsigned>(tid), response[0], response[1], response[2],
             response[3], response[4]);
  return false;
}

}  // namespace

SpiLink::SpiLink()
    : core_(),
      task_(nullptr),
      started_(nullptr),
      storage_(nullptr),
      lineCounters_{},
      lineTotals_{},
      reads_(0),
      badCommands_(0),
      addressErrors_(0),
      lastActivityMs_(0),
      rejected_(0),
      rejectedHead_{},
      reserveReports_(0),
      bulkBuffer_(nullptr),
      echoBuffer_(nullptr),
      bulkBusy_(false),
      dmaBuffer_(nullptr),
      dmaAction_(0),
      dmaSeed_(0),
      dmaLength_(0),
      selfTest_(kSelfTestNotRun),
      ready_(false),
      bootNonce_(0) {}

bool SpiLink::begin() {
  if (task_ != nullptr) {
    return ready_;
  }
  // Кольца лежат во внутренней RAM: к ним обращается задача обмена, а PSRAM
  // недоступна, пока ESP пишет во flash.
  storage_ = static_cast<uint8_t*>(heap_caps_malloc(
      kInputRingBytes + kRingBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  started_ = xSemaphoreCreateBinary();
  if (storage_ == nullptr || started_ == nullptr ||
      !core_.begin(storage_, kInputRingBytes, storage_ + kInputRingBytes, kRingBytes)) {
    ESP_LOGE(kTag, "no memory for the link");
    return false;
  }
  bootNonce_ = static_cast<uint8_t>(esp_random());
  // Драйвер запускает сама задача: прерывание SPI привязывается к тому ядру,
  // на котором вызван spi_slave_hd_init.
  if (xTaskCreatePinnedToCore(taskEntry, "zifi-spi", kTaskStackBytes, this,
                              kTaskPriority, &task_, kTaskCore) != pdPASS) {
    task_ = nullptr;
    ESP_LOGE(kTag, "link task was not created");
    return false;
  }
  xSemaphoreTake(started_, pdMS_TO_TICKS(kStartTimeoutMs));
  return ready_;
}

void SpiLink::taskEntry(void* context) {
  static_cast<SpiLink*>(context)->taskLoop();
}

void SpiLink::startLineCounters() {
  // Отказ счётчика обмену не мешает: без него индикатор просто не покажет
  // активность на этой линии.
  lineCounters_[kLineSelect] = makeEdgeCounter(board::kSpiCsPin, false, true);
  lineCounters_[kLineClock] = makeEdgeCounter(board::kSpiSckPin, true, false);
  lineCounters_[kLineData] = makeEdgeCounter(board::kSpiMosiPin, true, true);
  for (int index = 0; index < kLineCounters; ++index) {
    if (lineCounters_[index] == nullptr) {
      ESP_LOGW(kTag, "line counter %d is not available", index);
    }
  }
}

bool SpiLink::startDriver() {
  startLineCounters();

  spi_bus_config_t bus = {};
  bus.mosi_io_num = board::kSpiMosiPin;
  bus.miso_io_num = board::kSpiMisoPin;
  bus.sclk_io_num = board::kSpiSckPin;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.data4_io_num = -1;
  bus.data5_io_num = -1;
  bus.data6_io_num = -1;
  bus.data7_io_num = -1;
  bus.max_transfer_sz = SpiLinkCore::kMediaMax + 4;
  // Все четыре вывода — «родные» для SPI2, идут мимо матрицы GPIO: у MISO
  // наименьшая задержка, а SPI у TS-Config быстрый.
  bus.flags = SPICOMMON_BUSFLAG_IOMUX_PINS;
  bus.intr_flags = 0;

  spi_slave_hd_slot_config_t slot = {};
  slot.mode = 0;
  slot.spics_io_num = board::kSpiCsPin;
  slot.flags = 0;
  slot.command_bits = 8;
  slot.address_bits = 8;
  slot.dummy_bits = 8;
  slot.queue_size = 1;
  slot.dma_chan = SPI_DMA_CH_AUTO;
  slot.cb_config.cb_buffer_rx = onRequestWritten;
  slot.cb_config.cb_send_dma_ready = onBulkReady;
  slot.cb_config.cb_recv_dma_ready = onEchoReady;
  slot.cb_config.arg = xTaskGetCurrentTaskHandle();
  // Блок для DMA и буфер под его обратную посылку. Четыре байта запаса у
  // блока — под обмен, который DMA TS-Config начинает после последнего нужного
  // байта; длина приёма кратна четырём. Без памяти блоки не предлагаются.
  g_bulkReady = xSemaphoreCreateBinary();
  g_echoReady = xSemaphoreCreateBinary();
  bulkBuffer_ = static_cast<uint8_t*>(heap_caps_calloc(
      1, SpiLinkCore::kBulkSize + 4, MALLOC_CAP_DMA));
  echoBuffer_ = static_cast<uint8_t*>(heap_caps_calloc(
      1, SpiLinkCore::kMediaMax + 4, MALLOC_CAP_DMA));
  if (g_bulkReady == nullptr || g_echoReady == nullptr ||
      echoBuffer_ == nullptr) {
    bulkBuffer_ = nullptr;
  }

#if CONFIG_IDF_TARGET_ARCH_XTENSA && CONFIG_FREERTOS_SYSTICK_USES_SYSTIMER
  // У ESP32-S3 отдельного прерывания DMA для SPI нет (источник -1), и драйвер
  // вешает свой обработчик на внутренний таймер 0 процессора. FreeRTOS этим
  // таймером здесь не пользуется, прерывание от него никому не нужно. Чтобы
  // совпадение счётчика тактов с регистром сравнения не вызывало обработчик,
  // запрос таймера гасится записью в регистр сравнения, а после запуска
  // драйвера прерывание выключается на этом ядре.
  XTHAL_SET_CCOMPARE(0, esp_cpu_get_cycle_count() - 1);
#endif
  const esp_err_t error = spi_slave_hd_init(kHost, &bus, &slot);
#if CONFIG_IDF_TARGET_ARCH_XTENSA && CONFIG_FREERTOS_SYSTICK_USES_SYSTIMER
  esp_cpu_intr_disable(1U << kCpuTimer0Interrupt);
#endif
  if (error != ESP_OK) {
    ESP_LOGE(kTag, "spi_slave_hd_init: %s", esp_err_to_name(error));
    return false;
  }
  if (board::kSpiMisoEarly) {
    // Вывод бита по фронту SCK, а не по спаду (см. board.hpp). Драйвер ESP-IDF
    // этот разряд сбрасывает при запуске и больше не трогает.
    GPSPI2.slave.rsck_data_out = 1;
  }
  // Пока линии никем не управляются (модуль вынут из платы), ESP не выбрана и
  // на тактовой линии тишина. В плате эти слабые подтяжки ничего не меняют.
  idlePulls();

  // Сначала область обмена, потом слово состояния: увидев подпись, Z80 может
  // сразу писать запрос.
  uint8_t zero[SpiLinkCore::kTurnSize] = {};
  spi_slave_hd_write_buffer(kHost, 0, zero, sizeof(zero));
  publishState();
  return true;
}

void SpiLink::publishState() {
  uint8_t word[SpiLinkCore::kStateSize];
  core_.stateWord(word, bootNonce_);
  spi_slave_hd_write_buffer(kHost, SpiLinkCore::kStateOffset, word,
                            sizeof(word));
  // Между проверкой очереди и записью «пусто» приложение могло положить новые
  // байты и поставить «есть данные»; запись «пусто» это стёрла бы. Поэтому
  // после «пусто» очередь проверяется ещё раз.
  if (word[0] == SpiLinkCore::kStateEmpty && core_.pendingToZ80() != 0) {
    word[0] = SpiLinkCore::kStatePending;
    spi_slave_hd_write_buffer(kHost, SpiLinkCore::kStateOffset, word,
                              sizeof(word));
  }
}

void SpiLink::taskLoop() {
  ready_ = startDriver();
  xSemaphoreGive(started_);
  if (!ready_) {
    vTaskDelete(nullptr);
    return;
  }
  ESP_LOGI(kTag, "SPI link ready: CS=%d MOSI=%d SCK=%d MISO=%d",
           board::kSpiCsPin, board::kSpiMosiPin, board::kSpiSckPin,
           board::kSpiMisoPin);

  uint8_t request[SpiLinkCore::kTurnSize];
  uint8_t response[SpiLinkCore::kTurnSize];
  static spi_slave_hd_data_t sendTransaction;
  static spi_slave_hd_data_t echoTransaction;
  bool sendPending = false;   // блок выставлен и ещё не прочитан
  // Приём через DMA открыт. Он один на две цели: обратную посылку блока к Z80
  // и блок от Z80; цель задаёт последний ответ, открывший приём.
  bool echoPending = false;
  bool receiveIsPut = false;
  bool echoGood = false;      // последний блок вернулся без искажений
  // Блок от Z80 пришёл и ждёт запроса «блок записан».
  bool putReceived = false;
  size_t putLength = 0;
  uint32_t bulkReports = 0;
  uint32_t putReports = 0;
  uint32_t gaveUp = 0;
  for (;;) {
    taskStep_.store(kStepWaitRequest, std::memory_order_relaxed);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    taskStep_.store(kStepReadRequest, std::memory_order_relaxed);
    spi_slave_hd_read_buffer(kHost, 0, request, sizeof(request));
    // Чтение блока Z80 завершает командой 08, обратную посылку — командой 07.
    spi_slave_hd_data_t* done = nullptr;
    if (sendPending && spi_slave_hd_get_trans_res(kHost, SPI_SLAVE_CHAN_TX,
                                                  &done, 0) == ESP_OK) {
      sendPending = false;
    }
    if (echoPending && spi_slave_hd_get_trans_res(kHost, SPI_SLAVE_CHAN_RX,
                                                  &done, 0) == ESP_OK) {
      echoPending = false;
      const size_t received = done != nullptr ? done->trans_len : 0;
      if (receiveIsPut) {
        putReceived = true;
        putLength = received;
      } else {
      echoGood = received >= SpiLinkCore::kBulkSize &&
                 memcmp(echoBuffer_, bulkBuffer_, SpiLinkCore::kBulkSize) == 0;
      if (!echoGood && bulkReports < kBulkReports) {
        ++bulkReports;
        size_t first = 0;
        while (first < SpiLinkCore::kBulkSize - 1 &&
               echoBuffer_[first] == bulkBuffer_[first]) {
          ++first;
        }
        bringupLog(kTag,
                   "DMA block came back wrong: %u bytes, first difference at %u (sent %02X %02X, got %02X %02X)",
                   static_cast<unsigned>(received), static_cast<unsigned>(first),
                   bulkBuffer_[first], bulkBuffer_[first + 1], echoBuffer_[first],
                   echoBuffer_[first + 1]);
      }
      }
    }
    // Пока идёт проверка DMA (dmaProbe), канал DMA занят ею.
    const SpiLinkCore::BulkState bulk =
        bulkBuffer_ == nullptr || dmaAction_ != 0
            ? SpiLinkCore::kBulkOff
            : (sendPending ? SpiLinkCore::kBulkLoaded
                           : (echoGood ? SpiLinkCore::kBulkEchoed
                                       : SpiLinkCore::kBulkFree));
    SpiLinkCore::PutInput put;
    put.available = bulk != SpiLinkCore::kBulkOff;
    put.capacity = echoPending ? SpiLinkCore::kBulkSize : SpiLinkCore::kMediaMax;
    if (putReceived) {
      put.received = echoBuffer_;
      put.receivedLength = putLength;
    }
    size_t length = 0;
    taskStep_.store(kStepHandle, std::memory_order_relaxed);
    if (!core_.handleRequest(request, response, length, bulk, bulkBuffer_,
                             put)) {
      // Начало записи остаётся для журнала наладки: по нему видно, что именно
      // дошло до общего буфера.
      memcpy(rejectedHead_, request, sizeof(rejectedHead_));
      rejected_.fetch_add(1, std::memory_order_relaxed);
      bulkBusy_.store(sendPending || echoPending, std::memory_order_relaxed);
      continue;
    }
    if ((response[2] & SpiLinkCore::kStatusBulk) != 0 &&
        (response[2] & SpiLinkCore::kStatusBulkStale) == 0) {
      // Блок в bulkBuffer_ — новый, выданный заново либо тот же при повторе
      // запроса. Выставить его и открыть приём обратной посылки, если это ещё
      // не сделано, и дождаться загрузки в контроллер. Ответ уходит после
      // этого, так что Z80 не начнёт читать раньше времени. Если драйвер
      // откажет, Z80 прочитает или вернёт не то, и блок не пройдёт сравнение.
      echoGood = false;
      receiveIsPut = false;
      if (!sendPending) {
        taskStep_.store(kStepLoadSend, std::memory_order_relaxed);
        sendPending = loadDma(SPI_SLAVE_CHAN_TX, sendTransaction, bulkBuffer_,
                              SpiLinkCore::kBulkSize + 4, g_bulkReady);
      }
      if (!echoPending) {
        taskStep_.store(kStepLoadReceive, std::memory_order_relaxed);
        echoPending = loadDma(SPI_SLAVE_CHAN_RX, echoTransaction, echoBuffer_,
                              SpiLinkCore::kBulkSize + 4, g_echoReady);
      }
      if ((!sendPending || !echoPending) && bulkReports < kBulkReports) {
        ++bulkReports;
        bringupLog(kTag, "DMA block was not queued: send %s, echo %s",
                   sendPending ? "ok" : "FAILED", echoPending ? "ok" : "FAILED");
      }
    }
    if ((response[2] & (SpiLinkCore::kStatusPutOk |
                        SpiLinkCore::kStatusPutBad)) != 0) {
      // Итог по блоку от Z80 выдан: принятое больше не нужно.
      if ((response[2] & SpiLinkCore::kStatusPutBad) != 0 &&
          putReports < kBulkReports) {
        ++putReports;
        bringupLog(kTag, "DMA block from the Z80 was refused: %s, %u bytes",
                   putReceived ? "XOR differs or no room" : "nothing arrived",
                   static_cast<unsigned>(putLength));
      }
      putReceived = false;
      putLength = 0;
    }
    if ((response[2] & SpiLinkCore::kStatusPutReady) != 0) {
      // Z80 сейчас напишет блок: открыть приём, если он ещё не открыт (тогда
      // он был открыт под обратную посылку и теперь примет блок).
      receiveIsPut = true;
      putReceived = false;
      if (!echoPending) {
        taskStep_.store(kStepLoadReceive, std::memory_order_relaxed);
        echoPending = loadDma(SPI_SLAVE_CHAN_RX, echoTransaction, echoBuffer_,
                              core_.putDmaSize() + 4, g_echoReady);
        if (!echoPending && putReports < kBulkReports) {
          ++putReports;
          bringupLog(kTag, "DMA receive for a Z80 block was not queued");
        }
      }
    }
    bulkBusy_.store(sendPending || echoPending, std::memory_order_relaxed);
    const uint32_t gaveUpNow = core_.stats().bulkGaveUp;
    if (gaveUpNow != gaveUp) {
      gaveUp = gaveUpNow;
      bringupLog(kTag,
                 "DMA blocks are off until the next session: a block failed the echo check %u times in a row",
                 static_cast<unsigned>(SpiLinkCore::kBulkAttempts));
    }
    // Ответ пишется с конца: слово 0..3 с признаком ответа — последним. Пока
    // его нет, Z80 видит в байте 0 признак собственного запроса и ждёт.
    taskStep_.store(kStepWriteResponse, std::memory_order_relaxed);
    spi_slave_hd_write_buffer(kHost, 4, response + 4, length - 4);
    spi_slave_hd_write_buffer(kHost, 0, response, 4);
    lastActivityMs_.store(millis(), std::memory_order_relaxed);
    publishState();
  }
}

size_t SpiLink::read(uint8_t* output, size_t capacity) {
  return ready_ ? core_.read(output, capacity) : 0;
}

size_t SpiLink::write(const uint8_t* data, size_t length) {
  if (!ready_) {
    return 0;
  }
  const size_t written = core_.write(data, length);
  if (written != 0) {
    // Признак ставится после того, как байты уже лежат в очереди: Z80, заметив
    // его, обязательно их получит.
    uint8_t word[SpiLinkCore::kStateSize] = {
        SpiLinkCore::kStatePending, SpiLinkCore::kMagic, SpiLinkCore::kVersion,
        bootNonce_};
    spi_slave_hd_write_buffer(kHost, SpiLinkCore::kStateOffset, word,
                              sizeof(word));
  }
  return written;
}

bool SpiLink::reserve(size_t length, uint32_t timeoutMs) {
  if (!ready_ || length > core_.writeCapacity()) {
    return false;
  }
  // Место освобождает только Z80, забирая данные обменами. Ждём, пока она их
  // ведёт. Тишина отсчитывается от начала ожидания: до него Z80 могла молчать
  // сколько угодно — ей нечего было забирать, и это не признак остановки.
  // Раньше отсчёт шёл от последнего обмена, и первая же большая посылка после
  // секундной паузы отвергалась сразу, без единой попытки подождать.
  const uint32_t started = millis();
  const uint32_t quietLimit = timeoutMs > kPeerQuietMs ? timeoutMs : kPeerQuietMs;
  while (core_.writeSpace() < length) {
    const uint32_t now = millis();
    const uint32_t waited = now - started;
    const uint32_t sinceActivity =
        now - lastActivityMs_.load(std::memory_order_relaxed);
    const uint32_t quiet = sinceActivity < waited ? sinceActivity : waited;
    if (quiet >= quietLimit || waited >= kReserveLimitMs) {
      if (reserveReports_ < kReserveReports) {
        ++reserveReports_;
        const SpiLinkCore::Stats stats = core_.stats();
        bringupLog(kTag,
                   "send wait failed: need %u, space %u, waited %u ms, Z80 quiet %u ms, turns %u repeats %u bad %u",
                   static_cast<unsigned>(length),
                   static_cast<unsigned>(core_.writeSpace()),
                   static_cast<unsigned>(waited), static_cast<unsigned>(quiet),
                   static_cast<unsigned>(stats.turns),
                   static_cast<unsigned>(stats.repeats),
                   static_cast<unsigned>(stats.badRequests));
      }
      return false;
    }
    delay(1);
  }
  return true;
}

void SpiLink::flush() {
  // Очередь пуста, когда последние байты ушли в ответ Z80. Сам ответ она
  // прочитает из общего буфера уже без участия прошивки.
  const uint32_t started = millis();
  while (ready_ && core_.pendingToZ80() != 0 &&
         static_cast<uint32_t>(millis() - started) < kFlushTimeoutMs) {
    delay(1);
  }
}

void SpiLink::pollLines() {
  for (int index = 0; index < kLineCounters; ++index) {
    const pcnt_unit_handle_t unit =
        static_cast<pcnt_unit_handle_t>(lineCounters_[index]);
    int count = 0;
    if (unit == nullptr || pcnt_unit_get_count(unit, &count) != ESP_OK ||
        count == 0) {
      continue;
    }
    pcnt_unit_clear_count(unit);
    lineTotals_[index].fetch_add(static_cast<uint32_t>(count),
                                 std::memory_order_relaxed);
  }
  if (!ready_) {
    return;
  }
  // Признаки, которые контроллер SPI выставляет сам. Прерывания по ним не
  // включены: чтение буфера при опросе идёт десятки тысяч раз в секунду, и
  // здесь достаточно знать, что оно было.
  if (GPSPI2.dma_int_raw.rd_buf_done) {
    GPSPI2.dma_int_clr.rd_buf_done = 1;
    reads_.fetch_add(1, std::memory_order_relaxed);
  }
  if (GPSPI2.dma_int_raw.cmd_err) {
    GPSPI2.dma_int_clr.cmd_err = 1;
    badCommands_.fetch_add(1, std::memory_order_relaxed);
  }
  if (GPSPI2.dma_int_raw.buf_addr_err) {
    GPSPI2.dma_int_clr.buf_addr_err = 1;
    addressErrors_.fetch_add(1, std::memory_order_relaxed);
  }
  if (GPSPI2.dma_int_raw.en_qpi) {
    // Команда входа в QPI могла получиться только из искажённого байта: Evo
    // её не посылает. В этом режиме ESP выдавала бы данные и на линию MOSI,
    // поэтому из него сразу выходим.
    GPSPI2.dma_int_clr.en_qpi = 1;
    GPSPI2.user.qpi_mode = 0;
    badCommands_.fetch_add(1, std::memory_order_relaxed);
    ESP_LOGW(kTag, "QPI mode was requested over SPI and cancelled");
  }
}

SpiLink::Lines SpiLink::lines() const {
  Lines result{};
  result.selects = lineTotals_[kLineSelect].load(std::memory_order_relaxed);
  result.clocks = lineTotals_[kLineClock].load(std::memory_order_relaxed);
  result.dataEdges = lineTotals_[kLineData].load(std::memory_order_relaxed);
  result.reads = reads_.load(std::memory_order_relaxed);
  result.badCommands = badCommands_.load(std::memory_order_relaxed);
  result.addressErrors = addressErrors_.load(std::memory_order_relaxed);
  return result;
}

void SpiLink::sharedWords(uint8_t head[4], uint8_t state[4]) const {
  // Только чтение общего буфера ведомого: пишет в него одна задача обмена.
  spi_slave_hd_read_buffer(kHost, 0, head, 4);
  spi_slave_hd_read_buffer(kHost, SpiLinkCore::kStateOffset, state, 4);
}

SpiLink::LastTransfer SpiLink::lastTransfer() const {
  LastTransfer result{};
  if (ready_) {
    const uint32_t value = GPSPI2.slave1.val;
    result.dataBits = value & 0x3FFFF;
    result.command = static_cast<uint8_t>((value >> 18) & 0xFF);
    result.address = static_cast<uint8_t>((value >> 26) & 0x3F);
  }
  return result;
}

SpiLink::DmaProbeResult SpiLink::dmaProbe(uint8_t action, size_t length,
                                         uint8_t seed) {
  // Описатель транзакции драйвер держит у себя до её завершения.
  static spi_slave_hd_data_t transaction;
  DmaProbeResult result = {kDmaProbeFailed, 0, 0, 0};
  if (!ready_) {
    return result;
  }
  if (action == kDmaProbeFinish) {
    if (dmaAction_ == 0) {
      return result;
    }
    const spi_slave_chan_t channel =
        dmaAction_ == kDmaProbeSend ? SPI_SLAVE_CHAN_TX : SPI_SLAVE_CHAN_RX;
    spi_slave_hd_data_t* done = nullptr;
    // Транзакцию завершает ведущий: команда 08 после чтения, 07 после записи.
    if (spi_slave_hd_get_trans_res(kHost, channel, &done, pdMS_TO_TICKS(200)) !=
        ESP_OK) {
      result.status = kDmaProbePending;
      return result;
    }
    result.status = kDmaProbeOk;
    result.length = dmaLength_;
    if (dmaAction_ == kDmaProbeReceive) {
      const size_t received = done != nullptr ? done->trans_len : 0;
      result.length = static_cast<uint16_t>(received);
      const size_t checked = received < dmaLength_ ? received : dmaLength_;
      for (size_t index = 0; index < checked; ++index) {
        if (dmaBuffer_[index] != dmaProbeByte(index, dmaSeed_)) {
          if (result.errors == 0) {
            result.firstBad = static_cast<uint16_t>(index);
          }
          ++result.errors;
        }
      }
      bringupLog(kTag,
                 "DMA probe: received %u of %u, mismatched %u, first at %u, head %02X %02X %02X %02X",
                 static_cast<unsigned>(received),
                 static_cast<unsigned>(dmaLength_),
                 static_cast<unsigned>(result.errors),
                 static_cast<unsigned>(result.firstBad), dmaBuffer_[0],
                 dmaBuffer_[1], dmaBuffer_[2], dmaBuffer_[3]);
    } else {
      bringupLog(kTag, "DMA probe: %u bytes were read by the master",
                 static_cast<unsigned>(dmaLength_));
    }
    dmaAction_ = 0;
    return result;
  }

  if ((action != kDmaProbeSend && action != kDmaProbeReceive) || length == 0 ||
      length > kDmaProbeBytes || (length & 1) != 0 || dmaAction_ != 0 ||
      bulkBusy_.load(std::memory_order_relaxed)) {
    return result;
  }
  // Запас в четыре байта: DMA TS-Config после последнего нужного байта
  // начинает ещё один обмен.
  if (dmaBuffer_ == nullptr) {
    dmaBuffer_ = static_cast<uint8_t*>(
        heap_caps_malloc(kDmaProbeBytes + 4, MALLOC_CAP_DMA));
    if (dmaBuffer_ == nullptr) {
      return result;
    }
  }
  transaction = {};
  transaction.data = dmaBuffer_;
  if (action == kDmaProbeSend) {
    for (size_t index = 0; index < length + 4; ++index) {
      dmaBuffer_[index] = dmaProbeByte(index, seed);
    }
    transaction.len = length + 4;
  } else {
    memset(dmaBuffer_, 0, kDmaProbeBytes + 4);
    transaction.len = kDmaProbeBytes;   // длина приёма кратна четырём
  }
  const esp_err_t error = spi_slave_hd_queue_trans(
      kHost, action == kDmaProbeSend ? SPI_SLAVE_CHAN_TX : SPI_SLAVE_CHAN_RX,
      &transaction, pdMS_TO_TICKS(200));
  if (error != ESP_OK) {
    bringupLog(kTag, "DMA probe: queue_trans %s", esp_err_to_name(error));
    return result;
  }
  dmaAction_ = action;
  dmaSeed_ = seed;
  dmaLength_ = static_cast<uint16_t>(length);
  result.status = kDmaProbeOk;
  result.length = dmaLength_;
  return result;
}

void SpiLink::rejectedHead(uint8_t* output) const {
  // Задача обмена может в этот момент писать сюда же; для журнала это не важно.
  memcpy(output, rejectedHead_, sizeof(rejectedHead_));
}

bool SpiLink::selfTestExchange() {
  // Слово состояния контроллер SPI отдаёт сам, без программы: это проверка
  // выводов и режима ведомого.
  uint8_t state[3] = {};
  readShared(SpiLinkCore::kStateOffset, state, sizeof(state));
  if ((state[0] != SpiLinkCore::kStateEmpty &&
       state[0] != SpiLinkCore::kStatePending) ||
      state[1] != SpiLinkCore::kMagic || state[2] != SpiLinkCore::kVersion) {
    const LastTransfer last = lastTransfer();
    bringupLog(kTag,
               "self-test: state word reads %02X %02X %02X, need 5A D7 01; the slave got command %02X address %02X",
               state[0], state[1], state[2], last.command, last.address);
    return false;
  }

  // Запрос начала сеанса: прерывание, задача обмена и ответ.
  uint8_t tid = static_cast<uint8_t>(0x40 + (bootNonce_ & 0x3F));
  uint8_t reply[SpiLinkCore::kMaxChunk] = {};
  uint8_t replyLength = 0;
  if (!selfTurn(tid, SpiLinkCore::kFlagNewSession | SpiLinkCore::kFlagDropToZ80,
                nullptr, 0, 0, reply, replyLength)) {
    return false;
  }

  // Кадр PING сквозь весь тракт: канал, разбор кадров, цикл протокола и обратно.
  // READY ищется в потоке: перед ним в очереди может оказаться кадр события.
  uint8_t window[sizeof(kReadyFrame)] = {};
  size_t received = 0;
  ++tid;
  if (!selfTurn(tid, 0, kPingFrame, sizeof(kPingFrame),
                static_cast<uint8_t>(sizeof(reply)), reply, replyLength)) {
    return false;
  }
  const uint32_t started = millis();
  for (;;) {
    for (uint8_t index = 0; index < replyLength; ++index) {
      memmove(window, window + 1, sizeof(window) - 1);
      window[sizeof(window) - 1] = reply[index];
      ++received;
      if (received >= sizeof(window) &&
          memcmp(window, kReadyFrame, sizeof(window)) == 0) {
        return true;
      }
    }
    if (static_cast<uint32_t>(millis() - started) >= kSelfTestFrameMs) {
      bringupLog(kTag,
                 "self-test: PING was accepted, READY did not come (%u bytes, last %02X %02X %02X %02X %02X)",
                 static_cast<unsigned>(received), window[0], window[1], window[2],
                 window[3], window[4]);
      return false;
    }
    delay(2);
    ++tid;
    if (!selfTurn(tid, 0, nullptr, 0, static_cast<uint8_t>(sizeof(reply)),
                  reply, replyLength)) {
      return false;
    }
  }
}

void SpiLink::logMisoTiming() {
  // Что стоит на MISO перед фронтом SCK и сразу после него. При выводе по
  // спаду оба отсчёта равны слову состояния; при выводе на полтакта раньше
  // второй — уже следующий бит, то есть слово, сдвинутое на бит влево.
  MisoTrace trace = {};
  traceShared(SpiLinkCore::kStateOffset, trace);
  bringupLog(kTag, "MISO before SCK rise %02X %02X %02X, after rise %02X %02X %02X (%s)",
             trace.beforeRise[0], trace.beforeRise[1], trace.beforeRise[2],
             trace.afterRise[0], trace.afterRise[1], trace.afterRise[2],
             GPSPI2.slave.rsck_data_out ? "bit is set on the rising edge"
                                        : "bit is set on the falling edge");
}

bool SpiLink::selfTestPass() {
  const uint32_t clock = spi_periph_signal[kHost].spiclk_in;
  const uint32_t data = spi_periph_signal[kHost].spid_in;
  const uint32_t select = spi_periph_signal[kHost].spics_in;
  // CS берётся первым и отпускается последним: пока он высокий, перепады на
  // остальных линиях ведомого не касаются.
  takeLine(board::kSpiCsPin, select, true);
  takeLine(board::kSpiSckPin, clock, false);
  takeLine(board::kSpiMosiPin, data, true);
  // Ответ читается с вывода MISO как с обычного входа GPIO.
  gpio_input_enable(static_cast<gpio_num_t>(board::kSpiMisoPin));
  esp_rom_delay_us(50);
  const bool passed = selfTestExchange();
  if (passed) {
    logMisoTiming();
  }
  drive(board::kSpiCsPin, true);
  esp_rom_delay_us(50);
  releaseLine(board::kSpiMosiPin, data);
  releaseLine(board::kSpiSckPin, clock);
  releaseLine(board::kSpiCsPin, select);
  idlePulls();
  esp_rom_delay_us(kProbeSettleUs);
  return passed;
}

SpiLink::SelfTest SpiLink::runSelfTest() {
  if (!ready_) {
    return kSelfTestNotRun;
  }
  // Свободна ли каждая линия. В плате их держит буфер U3; линия, которая и в
  // плате следует за слабой подтяжкой, до буфера не доходит (непропай).
  // CS проверяется последним: пока он подтянут вверх, перепады на остальных
  // линиях ведомого не касаются.
  const bool clockFree = floats(board::kSpiSckPin);
  const bool dataFree = floats(board::kSpiMosiPin);
  const bool selectFree = floats(board::kSpiCsPin);
  idlePulls();
  esp_rom_delay_us(kProbeSettleUs);
  bringupLog(kTag, "lines: CS %s level %d, SCK %s level %d, MOSI %s level %d",
             selectFree ? "floats" : "driven",
             gpio_get_level(static_cast<gpio_num_t>(board::kSpiCsPin)),
             clockFree ? "floats" : "driven",
             gpio_get_level(static_cast<gpio_num_t>(board::kSpiSckPin)),
             dataFree ? "floats" : "driven",
             gpio_get_level(static_cast<gpio_num_t>(board::kSpiMosiPin)));

  SelfTest result = kSelfTestSkipped;
  if (!clockFree || !dataFree || !selectFree) {
    bringupLog(kTag, "self-test skipped: SPI lines are driven by the board");
  } else if (selfTestPass()) {
    result = kSelfTestPassed;
    bringupLog(kTag, "self-test PASSED: SPI slave, driver, link task and PING/READY");
  } else {
    result = kSelfTestFailed;
    bringupLog(kTag, "self-test FAILED");
  }
  selfTest_.store(result, std::memory_order_relaxed);
  return result;
}

}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
