#pragma once

#include <stdint.h>

#include "zifi/board.hpp"

namespace zifi {

// Что показывает светодиод модуля ESP32-S3-Zero на плате VDAC4. Здесь только
// выбор цвета, без обращений к железу: этот код исполняется и в тестах на ПК.
//
// Индикатор нужен при наладке платы. USB к модулю, пока он стоит в плате,
// подключать нельзя, а Z80 узнаёт о состоянии ESP только по линии MISO. Если
// молчит именно она, светодиод сразу показывает, что доходит до ESP (точные
// числа остаются в постоянном журнале, см. bringup_log.hpp). Показывается самое
// «глубокое» из того, что замечено за последнюю секунду:
//
//   белый, слабо           — прошивка работает, на линиях SPI тихо;
//   жёлтый, редко мигает   — такты SCK идут, но ESP не выбрана (CS не было).
//                            Линии SCK и MOSI у ESP общие с SD-картой Evo, так
//                            что это и обычное чтение карты;
//   белый, редко мигает    — ведущий выбирает ESP (CS), но тактов SCK нет;
//   жёлтый, часто мигает   — есть и CS, и SCK, но ни одной команды ESP не
//                            разобрала;
//   жёлтый                 — ведущий читает общий буфер: CS, SCK и MOSI в порядке
//                            (так выглядит и обычный опрос «есть ли данные»);
//   синий мигает           — в буфер записано то, что запросом не является;
//   синий                  — идут верные запросы Z80;
//   белый, часто мигает    — драйвер SPI не запустился;
//   жёлтый и синий по очереди — на линии SCK строчная синхронизация Evo: гнездо
//                            платы надето на разъём X2 со сдвигом на ряд.
//
// В каждом цвете красного и зелёного поровну: у светодиодов разных партий
// модуля порядок этих двух цветов разный, и «красный» мог бы оказаться зелёным.
class StatusLedLogic {
 public:
  struct Color {
    uint8_t red;
    uint8_t green;
    uint8_t blue;

    bool operator==(const Color& other) const {
      return red == other.red && green == other.green && blue == other.blue;
    }
    bool operator!=(const Color& other) const { return !(*this == other); }
  };

  // Счётчики канала. Все только растут; важен сам факт изменения.
  struct Inputs {
    bool linkReady;      // запущен ли драйвер SPI
    uint32_t handled;    // обработано запросов Z80, новых и повторных
    uint32_t rejected;   // записей в общий буфер, не оказавшихся запросом
    uint32_t reads;      // замечено чтений общего буфера ведущим
    uint32_t clocks;     // фронтов SCK
    uint32_t selects;    // спадов CS
  };

  static constexpr Color kOff = {0, 0, 0};
  static constexpr Color kIdle = {4, 4, 4};
  static constexpr Color kWhite = {16, 16, 16};
  static constexpr Color kYellow = {24, 24, 0};
  static constexpr Color kBlue = {0, 0, 32};

  // Столько признак держится после последнего события: тест длится секунды,
  // и одиночный запрос иначе не разглядеть.
  static constexpr uint32_t kHoldMs = 1000;
  static constexpr uint32_t kFastBlinkMs = 100;   // полупериод частого мигания
  static constexpr uint32_t kSlowBlinkMs = 400;   // полупериод редкого мигания

  // Гнездо платы без ключа, и на разъём X2 Evo его можно надеть со сдвигом на
  // ряд. Тогда на линию SCK платы попадает контакт 37 Evo — строчная
  // синхронизация: 15,6 кГц (с удвоением строк 31,25 кГц), ровно и без пауз.
  // Обмен SPI так не выглядит: он идёт пачками, и ESP разбирает в нём команды.
  // Признак сдвига — kLineSyncSlices замеров подряд по kSliceMs, в каждом из
  // которых частота фронтов SCK строчная и ни одной команды не разобрано.
  static constexpr uint32_t kSliceMs = 100;
  static constexpr uint32_t kLineSyncSlices = 20;
  static constexpr uint32_t kLineRateHz = 15625;  // и вдвое большая
  static constexpr uint32_t kSliceTolerancePercent = 10;
  // За всю серию замеров частота известна точнее, чем за один.
  static constexpr uint32_t kRunTolerancePercent = 2;

  Color update(uint32_t nowMs, const Inputs& inputs) {
    const uint32_t values[kStages] = {inputs.rejected, inputs.handled,
                                      inputs.reads, inputs.clocks,
                                      inputs.selects};
    bool recent[kStages];
    for (int stage = 0; stage < kStages; ++stage) {
      Stage& entry = stages_[stage];
      if (!started_) {
        // Счётчики на момент запуска индикатора событием не считаются.
        entry.last = values[stage];
      } else if (values[stage] != entry.last) {
        entry.last = values[stage];
        entry.atMs = nowMs;
        entry.seen = true;
      }
      // Вычитание беззнаковых миллисекунд верно и после переполнения счётчика.
      // Давнее событие забывается: через 49 суток разность снова стала бы малой.
      if (entry.seen && static_cast<uint32_t>(nowMs - entry.atMs) >= kHoldMs) {
        entry.seen = false;
      }
      recent[stage] = entry.seen;
    }
    started_ = true;
    watchLineSync(nowMs, inputs);

    if (!inputs.linkReady) {
      return blink(nowMs, kFastBlinkMs, kWhite);
    }
    if (misplaced()) {
      // Остальные признаки при этом ничего не значат: на линиях не SPI.
      return (nowMs / kSlowBlinkMs) % 2 == 0 ? kYellow : kBlue;
    }
    // Первым стоит отказ: редкий сбой среди верных запросов иначе не заметить.
    // Дальше — от полностью работающего обмена к отдельным линиям.
    if (recent[kStageRejected]) {
      return blink(nowMs, kFastBlinkMs, kBlue);
    }
    if (recent[kStageHandled]) {
      return kBlue;
    }
    if (recent[kStageReads]) {
      return kYellow;
    }
    if (recent[kStageClocks] && recent[kStageSelects]) {
      return blink(nowMs, kFastBlinkMs, kYellow);
    }
    if (recent[kStageSelects]) {
      return blink(nowMs, kSlowBlinkMs, kWhite);
    }
    if (recent[kStageClocks]) {
      return blink(nowMs, kSlowBlinkMs, kYellow);
    }
    return kIdle;
  }

  // Видна ли сейчас на линии SCK строчная синхронизация Evo (см. kSliceMs).
  bool misplaced() const { return slices_ >= kLineSyncSlices; }
  // Частота на линии SCK за серию замеров, по которой это решено, Гц.
  uint32_t lineSyncHz() const { return runHz_; }

 private:
  enum : int {
    kStageRejected,
    kStageHandled,
    kStageReads,
    kStageClocks,
    kStageSelects,
    kStages,
  };

  struct Stage {
    uint32_t last = 0;
    uint32_t atMs = 0;
    bool seen = false;
  };

  static Color blink(uint32_t nowMs, uint32_t halfPeriodMs, const Color& color) {
    return (nowMs / halfPeriodMs) % 2 == 0 ? color : kOff;
  }

  static uint32_t rateHz(uint32_t edges, uint32_t elapsedMs) {
    return static_cast<uint32_t>(static_cast<uint64_t>(edges) * 1000 /
                                 elapsedMs);
  }

  // Строчная частота, от которой hertz отличается не больше чем на percent
  // процентов; 0 — ни одна.
  static uint32_t lineRate(uint32_t hertz, uint32_t percent) {
    for (uint32_t rate = kLineRateHz; rate <= 2 * kLineRateHz; rate *= 2) {
      const uint32_t tolerance = rate * percent / 100;
      if (hertz + tolerance >= rate && hertz <= rate + tolerance) {
        return rate;
      }
    }
    return 0;
  }

  void watchLineSync(uint32_t nowMs, const Inputs& inputs) {
    const uint32_t decoded = inputs.reads + inputs.handled + inputs.rejected;
    if (sliceOpen_) {
      const uint32_t elapsedMs = nowMs - sliceMs_;
      if (elapsedMs < kSliceMs) {
        return;
      }
      const uint32_t rate =
          decoded != sliceDecoded_
              ? 0
              : lineRate(rateHz(inputs.clocks - sliceClocks_, elapsedMs),
                         kSliceTolerancePercent);
      if (rate == 0 || rate != runRate_) {
        // Серия прервалась либо начинается заново, с этого замера.
        slices_ = 0;
        runRate_ = rate;
        runMs_ = sliceMs_;
        runClocks_ = sliceClocks_;
      }
      if (rate != 0 && slices_ < kLineSyncSlices) {
        ++slices_;
        if (slices_ == kLineSyncSlices) {
          runHz_ = rateHz(inputs.clocks - runClocks_, nowMs - runMs_);
          if (lineRate(runHz_, kRunTolerancePercent) != rate) {
            slices_ = 0;
            runRate_ = 0;
          }
        }
      }
    }
    sliceOpen_ = true;
    sliceMs_ = nowMs;
    sliceClocks_ = inputs.clocks;
    sliceDecoded_ = decoded;
  }

  bool started_ = false;
  Stage stages_[kStages];
  // Текущий замер частоты на линии SCK: когда начат и какими были счётчики.
  bool sliceOpen_ = false;
  uint32_t sliceMs_ = 0;
  uint32_t sliceClocks_ = 0;
  uint32_t sliceDecoded_ = 0;
  // Серия замеров подряд с одной и той же строчной частотой.
  uint32_t slices_ = 0;
  uint32_t runRate_ = 0;
  uint32_t runMs_ = 0;
  uint32_t runClocks_ = 0;
  uint32_t runHz_ = 0;
};

#if ZIFI_HAS_STATUS_LED
class SpiLink;
class AudioPlayer;

// Запустить задачу индикатора и наладочных проверок. Она сама следит за
// счётчиками канала и пишет их в журнал наладки, после запуска проверяет выводы
// I2S (audio может быть nullptr), запускает самопроверку SPI и печатает в USB
// постоянный журнал. link и audio должны жить всё время работы прошивки.
bool startStatusLed(SpiLink& link, const AudioPlayer* audio);
#endif

}  // namespace zifi
