#pragma once

#include "zifi/board.hpp"

#if ZIFI_HAS_SPI_LINK

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "zifi/byte_link.hpp"
#include "zifi/spi_link_core.hpp"

namespace zifi {

// Канал до Z80 по SPI платы VDAC4: ESP32-S3 — ведомый в режиме SPI Slave HD.
// Правила обмена описаны в spi_link_core.hpp. Здесь только привязка к драйверу
// ESP-IDF и задача, которая отвечает на запросы Z80.
//
// Прерывание SPI лишь будит задачу обмена на ядре 1. Она читает запрос из общего
// буфера, складывает принятые байты в кольцо для цикла протокола и пишет ответ.
// Цикл протокола (то же ядро, приоритет ниже) берёт и кладёт байты через read и
// write, поэтому обмен не останавливается, пока он занят.
class SpiLink : public ByteLink {
 public:
  // Что видно на линиях SPI независимо от разбора запросов (для наладки платы).
  // Счётчики только растут.
  struct Lines {
    uint32_t selects;      // спадов CS
    uint32_t clocks;       // фронтов SCK
    uint32_t dataEdges;    // перепадов MOSI
    uint32_t reads;        // опросов, заметивших чтение общего буфера ведущим
    uint32_t badCommands;    // опросов, заметивших неизвестную команду
    uint32_t addressErrors;  // опросов, заметивших обращение за конец буфера
  };

  // Что контроллер SPI принял последним: байт команды, адрес и длина данных в
  // битах. Регистр читается как есть и обновляется самим контроллером; по нему
  // видно, что на самом деле доходит до ESP по линии MOSI.
  struct LastTransfer {
    uint8_t command;
    uint8_t address;
    uint32_t dataBits;
  };

  static constexpr size_t kRejectedHeadSize = 8;

  enum SelfTest : uint8_t {
    kSelfTestNotRun,     // ещё не запускалась
    kSelfTestSkipped,    // линии кем-то управляются: модуль стоит в плате
    kSelfTestPassed,
    kSelfTestFailed,
  };

  SpiLink();

  bool begin() override;
  size_t read(uint8_t* output, size_t capacity) override;
  size_t write(const uint8_t* data, size_t length) override;
  bool reserve(size_t length, uint32_t timeoutMs) override;
  void flush() override;
  const char* name() const override { return "SPI"; }
  bool padsFrames() const override { return true; }

  bool ready() const { return ready_; }
  SpiLinkCore::Stats stats() const { return core_.stats(); }
  // Счётчики для индикатора. handled — обработано запросов Z80, новых и
  // повторных; rejected — сколько раз ведущий записал в общий буфер то, что
  // запросом не оказалось.
  uint32_t handled() const { return core_.activity(); }
  uint32_t rejected() const {
    return rejected_.load(std::memory_order_relaxed);
  }

  // Забрать показания счётчиков линий. Вызывает задача индикатора, не чаще
  // нескольких десятков раз в секунду.
  void pollLines();
  Lines lines() const;
  LastTransfer lastTransfer() const;

  // Для сторожа обмена (link_watchdog.hpp). Начало общего буфера (признак
  // запроса или ответа) и слово состояния — как их видит Z80; шаг задачи обмена
  // (TaskStep); сколько данных ждёт в каждой очереди.
  enum TaskStep : uint8_t {
    kStepWaitRequest = 1,   // ждёт записи запроса
    kStepReadRequest = 2,   // читает запрос, итоги DMA
    kStepHandle = 3,        // разбирает запрос
    kStepLoadSend = 4,      // выставляет блок DMA для чтения
    kStepLoadReceive = 5,   // открывает приём блока DMA
    kStepWriteResponse = 6, // пишет ответ и слово состояния
  };
  void sharedWords(uint8_t head[4], uint8_t state[4]) const;
  uint8_t taskStep() const { return taskStep_.load(std::memory_order_relaxed); }
  size_t pendingToZ80() const { return core_.pendingToZ80(); }
  size_t pendingFromZ80() const { return core_.pendingFromZ80(); }
  // Начало последней записи в общий буфер, не оказавшейся запросом:
  // kRejectedHeadSize байтов в output.
  void rejectedHead(uint8_t* output) const;

  // Самопроверка приёмника без Evo. Пока линии SPI никем не управляются (модуль
  // вынут из платы или плата без питания), прошивка на время делает их выходами
  // и сама подаёт себе запрос, как это сделала бы Evo, а потом проверяет ответ —
  // через те же выводы, контроллер SPI, драйвер и задачу обмена. Последним
  // шагом по каналу проходит кадр PING, поэтому вызывать её нужно, когда цикл
  // протокола уже работает. На время проверки входы SPI2 переключаются на
  // матрицу GPIO: прямой путь от вывода (IO_MUX), которым они идут в работе,
  // действует, только пока вывод отдан SPI, и этой проверкой не охвачен.
  // В работающей плате линии держит буфер U3: проверка замечает это по тому,
  // что уровень не следует за слабой подтяжкой, и выводы не трогает.
  // Вызывается один раз, не из задачи обмена.
  SelfTest runSelfTest();
  SelfTest selfTest() const {
    return static_cast<SelfTest>(selfTest_.load(std::memory_order_relaxed));
  }

  // Проверка обмена через DMA (наладка платы). Кроме общего буфера, у ведомого
  // SPI есть передача блоков через DMA: команда 04 — ведущий читает, 03 —
  // пишет, 08 и 07 завершают их. По ней Evo может гонять данные своим DMA, не
  // тратя такты Z80 на каждый байт. Проверка кладёт в DMA известный узор либо
  // принимает его от Evo и сверяет; общий буфер при этом работает как обычно.
  static constexpr size_t kDmaProbeBytes = 2048;
  enum DmaProbe : uint8_t {
    kDmaProbeSend = 1,     // выставить length байтов узора для чтения ведущим
    kDmaProbeReceive = 2,  // приготовиться принять узор от ведущего
    kDmaProbeFinish = 3,   // закончена ли транзакция и что принято
  };
  enum DmaProbeStatus : uint8_t {
    kDmaProbeFailed = 0,   // драйвер отказал (нет памяти, длина, занято)
    kDmaProbeOk = 1,
    kDmaProbePending = 2,  // ведущий транзакцию не завершил
  };
  struct DmaProbeResult {
    uint8_t status;
    uint16_t length;    // для приёма — сколько байтов пришло
    uint16_t errors;    // сколько из них не совпало с узором
    uint16_t firstBad;  // смещение первого несовпавшего
  };
  static uint8_t dmaProbeByte(size_t index, uint8_t seed) {
    return static_cast<uint8_t>(index + 3 * (index >> 8) + seed);
  }
  DmaProbeResult dmaProbe(uint8_t action, size_t length, uint8_t seed);

 private:
  // Кольца в каждую сторону. Кадр протокола — до 1029 байт, так что в очереди
  // помещается ответ и несколько событий.
  static constexpr size_t kRingBytes = 4096;
  static constexpr size_t kInputRingBytes = 16384; // full raw media DMA + header
  static constexpr int kLineCounters = 3;

  static void taskEntry(void* context);
  void taskLoop();
  bool startDriver();
  void startLineCounters();
  void publishState();
  bool selfTestPass();
  void logMisoTiming();
  bool selfTestExchange();

  SpiLinkCore core_;
  TaskHandle_t task_;
  SemaphoreHandle_t started_;
  uint8_t* storage_;
  void* lineCounters_[kLineCounters];      // pcnt_unit_handle_t: CS, SCK, MOSI
  std::atomic<uint32_t> lineTotals_[kLineCounters];
  std::atomic<uint32_t> reads_;
  std::atomic<uint32_t> badCommands_;
  std::atomic<uint32_t> addressErrors_;
  std::atomic<uint32_t> lastActivityMs_;
  std::atomic<uint8_t> taskStep_{0};
  std::atomic<uint32_t> rejected_;
  uint8_t rejectedHead_[kRejectedHeadSize];
  uint32_t reserveReports_;
  uint8_t* bulkBuffer_;      // блок для чтения Z80 через DMA (см. spi_link_core.hpp)
  uint8_t* echoBuffer_;      // сюда Z80 отсылает блок обратно
  // DMA занят блоком: он выставлен либо открыт приём его обратной посылки.
  std::atomic<bool> bulkBusy_;
  uint8_t* dmaBuffer_;       // буфер проверки DMA, выделяется при первой проверке
  uint8_t dmaAction_;        // незавершённая транзакция проверки: 0 — нет
  uint8_t dmaSeed_;
  uint16_t dmaLength_;
  std::atomic<uint8_t> selfTest_;
  volatile bool ready_;
  uint8_t bootNonce_;
};

}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
