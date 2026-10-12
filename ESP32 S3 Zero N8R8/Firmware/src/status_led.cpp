#include "zifi/status_led.hpp"

#if ZIFI_HAS_STATUS_LED

#include <Arduino.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "zifi/audio_player.hpp"
#include "zifi/board_state.hpp"
#include "zifi/bringup_log.hpp"
#include "zifi/spi_link.hpp"

namespace zifi {
namespace {

constexpr char kTag[] = "zifi-diag";
constexpr uint32_t kPeriodMs = 20;
// Задача читает и печатает постоянный журнал: LittleFS нужен запас стека.
constexpr uint32_t kTaskStackBytes = 8192;
// Самая несрочная задача прошивки, на ядре сети: обмену с Z80 она не мешает.
constexpr UBaseType_t kTaskPriority = 1;
constexpr BaseType_t kTaskCore = 0;
// Проверки после запуска идут по порядку: выводы I2S, самопроверка SPI, печать
// постоянного журнала. К этому времени прошивка уже запустила звук и цикл
// протокола; I2S, если он ещё не готов, ждём до kPadsGiveUpMs.
constexpr uint32_t kChecksStartMs = 2500;
constexpr uint32_t kPadsGiveUpMs = 10000;
// Счётчики линий пишутся в журнал раз в секунду, пока меняются. Постоянный
// журнал — кольцо на 256 записей, поэтому после первых записей — раз в минуту.
constexpr uint32_t kReportPeriodMs = 1000;
constexpr uint32_t kFastReports = 40;
constexpr uint32_t kSlowReportPeriodMs = 60000;
constexpr uint32_t kRejectedReports = 8;
// Столько раз за запуск в журнал пишется, что сдвиг гнезда замечен или пропал.
constexpr uint32_t kMisplacedReports = 6;

enum Check : uint8_t {
  kCheckBoard,
  kCheckPads,
  kCheckSelfTest,
  kCheckStoredLog,
  kChecksDone,
};

struct Context {
  SpiLink* link;
  const AudioPlayer* audio;
};

void show(const StatusLedLogic::Color& color) {
  // Светодиод WS2812: посылку формирует RMT Arduino-ядра.
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  rgbLedWrite(board::kStatusLedPin, color.red, color.green, color.blue);
#else
  neopixelWrite(board::kStatusLedPin, color.red, color.green, color.blue);
#endif
}

void reportLines(SpiLink* link, const SpiLink::Lines& lines,
                 const StatusLedLogic::Inputs& inputs) {
  // last — что контроллер SPI принял последним: команда/адрес/длина в битах.
  const SpiLink::LastTransfer last = link->lastTransfer();
  bringupLog(kTag,
             "lines: cs=%u sck=%u mosi=%u rdbuf=%u cmderr=%u adrerr=%u ok=%u bad=%u last=%02X/%02X/%u",
             static_cast<unsigned>(lines.selects),
             static_cast<unsigned>(lines.clocks),
             static_cast<unsigned>(lines.dataEdges),
             static_cast<unsigned>(lines.reads),
             static_cast<unsigned>(lines.badCommands),
             static_cast<unsigned>(lines.addressErrors),
             static_cast<unsigned>(inputs.handled),
             static_cast<unsigned>(inputs.rejected), last.command, last.address,
             static_cast<unsigned>(last.dataBits));
}

void reportRejected(SpiLink* link, uint32_t number) {
  uint8_t head[SpiLink::kRejectedHeadSize];
  link->rejectedHead(head);
  bringupLog(kTag, "bad write %u: %02X %02X %02X %02X %02X %02X %02X %02X",
             static_cast<unsigned>(number), head[0], head[1], head[2], head[3],
             head[4], head[5], head[6], head[7]);
}

void ledTask(void* context) {
  const Context* owner = static_cast<const Context*>(context);
  SpiLink* link = owner->link;
  StatusLedLogic logic;
  StatusLedLogic::Color shown = StatusLedLogic::kOff;
  bool first = true;
  uint8_t check = kCheckBoard;
  BoardState board = BoardState::kUnknown;
  const uint32_t startedMs = millis();
  uint32_t reportedMs = startedMs;
  uint32_t reports = 0;
  uint32_t rejectedReports = 0;
  SpiLink::Lines reported = {};
  uint32_t reportedHandled = 0;
  uint32_t reportedRejected = 0;
  uint32_t seenRejected = 0;
  uint32_t misplacedReports = 0;
  bool misplaced = false;

  for (;;) {
    // Время и счётчики линий должны относиться к одному моменту: по ним
    // индикатор считает частоту на линии SCK. Если между ними задачу вытеснили,
    // счётчики забираются заново.
    uint32_t now = millis();
    link->pollLines();
    while (static_cast<uint32_t>(millis() - now) > 1) {
      now = millis();
      link->pollLines();
    }
    const SpiLink::Lines lines = link->lines();
    StatusLedLogic::Inputs inputs = {};
    inputs.linkReady = link->ready();
    inputs.handled = link->handled();
    inputs.rejected = link->rejected();
    inputs.reads = lines.reads;
    inputs.clocks = lines.clocks;
    inputs.selects = lines.selects;
    const StatusLedLogic::Color color = logic.update(now, inputs);
    // Светодиод помнит цвет сам: посылка нужна только при его смене.
    if (first || color != shown) {
      show(color);
      shown = color;
      first = false;
    }

    const uint32_t uptime = now - startedMs;
    if (check != kChecksDone && uptime >= kChecksStartMs) {
      if (check == kCheckBoard) {
        // Запись в журнале отличает запуски в плате от запусков на столе.
        board = probeBoardState();
        bringupLog(kTag, "board: %s", boardStateName(board));
        check = kCheckPads;
      } else if (check == kCheckPads) {
        if (owner->audio == nullptr) {
          check = kCheckSelfTest;
        } else if (board == BoardState::kUnpowered) {
          // I2S в обесточенной плате не запущен; линии проверяются без него.
          AudioPlayer::logPadVoltages();
          check = kCheckSelfTest;
        } else if (owner->audio->logPads()) {
          check = kCheckSelfTest;
        } else if (uptime >= kPadsGiveUpMs) {
          bringupLog(kTag, "pads: I2S is not running");
          check = kCheckSelfTest;
        }
      } else if (check == kCheckSelfTest) {
        link->runSelfTest();
        check = kCheckStoredLog;
      } else {
        bringupPrintStoredLog();
        check = kChecksDone;
      }
    }

    if (logic.misplaced() != misplaced) {
      misplaced = !misplaced;
      if (misplacedReports < kMisplacedReports) {
        ++misplacedReports;
        if (misplaced) {
          bringupLog(kTag,
                     "SCK carries a steady %u Hz and no command: line sync of the Evo, the socket is one column off on X2",
                     static_cast<unsigned>(logic.lineSyncHz()));
        } else {
          bringupLog(kTag, "SCK no longer carries line sync");
        }
      }
    }

    // Первые записи, не оказавшиеся запросом, — в журнал сразу и с содержимым.
    if (inputs.rejected != seenRejected) {
      seenRejected = inputs.rejected;
      if (rejectedReports < kRejectedReports) {
        ++rejectedReports;
        reportRejected(link, inputs.rejected);
      }
    }

    const uint32_t period =
        reports < kFastReports ? kReportPeriodMs : kSlowReportPeriodMs;
    if (static_cast<uint32_t>(now - reportedMs) >= period &&
        (lines.selects != reported.selects || lines.clocks != reported.clocks ||
         lines.dataEdges != reported.dataEdges ||
         lines.reads != reported.reads ||
         lines.badCommands != reported.badCommands ||
         lines.addressErrors != reported.addressErrors ||
         inputs.handled != reportedHandled ||
         inputs.rejected != reportedRejected)) {
      reportLines(link, lines, inputs);
      reportedMs = now;
      ++reports;
      reported = lines;
      reportedHandled = inputs.handled;
      reportedRejected = inputs.rejected;
    }
    vTaskDelay(pdMS_TO_TICKS(kPeriodMs));
  }
}

}  // namespace

bool startStatusLed(SpiLink& link, const AudioPlayer* audio) {
  // Описание живёт всё время работы прошивки: задача не завершается.
  static Context context;
  context.link = &link;
  context.audio = audio;
  return xTaskCreatePinnedToCore(ledTask, "zifi-led", kTaskStackBytes, &context,
                                 kTaskPriority, nullptr, kTaskCore) == pdPASS;
}

}  // namespace zifi

#endif  // ZIFI_HAS_STATUS_LED
