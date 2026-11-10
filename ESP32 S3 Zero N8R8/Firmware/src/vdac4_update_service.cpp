#include "zifi/vdac4_update_service.hpp"

#if ZIFI_HAS_SPI_LINK

#include <Arduino.h>

#include <new>
#include <stdio.h>
#include <string.h>

#include "zifi/bringup_log.hpp"

namespace zifi {
namespace {

// Рукопожатие mbedTLS идёт прямо на стеке задачи, как у WC Update.
constexpr uint32_t kTaskStackBytes = 16384;
constexpr UBaseType_t kTaskPriority = 2;
constexpr BaseType_t kTaskCore = 0;
// Строка ESP (UPDATED) и этап 7 должны дойти до Z80 до перезапуска: очередь событий разбирает
// ядро 1, Z80 забирает их раз в кадр (20 мс).
constexpr uint32_t kRestartDelayMs = 1000;

}  // namespace

Vdac4UpdateService::Vdac4UpdateService(VfsBridge& bridge, WcUpdater::EventSink sink,
                                       void* context)
    : bridge_(bridge),
      sink_(sink),
      context_(context),
      board_(),
      fetcher_(nullptr),
      updater_(nullptr),
      commands_(nullptr),
      task_(nullptr),
      finished_(true),
      applying_(false) {}

Vdac4UpdateService::~Vdac4UpdateService() { stop(1000); }

uint32_t Vdac4UpdateService::clock() {
  return static_cast<uint32_t>(millis());
}

void Vdac4UpdateService::release() {
  delete updater_;
  updater_ = nullptr;
  delete fetcher_;
  fetcher_ = nullptr;
  if (commands_ != nullptr) {
    vQueueDelete(commands_);
    commands_ = nullptr;
  }
  task_ = nullptr;
}

bool Vdac4UpdateService::start(const uint8_t* payload, uint16_t length, char* error,
                               size_t errorSize) {
  if (task_ != nullptr) {
    snprintf(error, errorSize, "already running");
    return false;
  }
  fetcher_ = new (std::nothrow) NetWcFetcher();
  updater_ = fetcher_ == nullptr ? nullptr
                                 : new (std::nothrow) Vdac4Updater(&bridge_, *fetcher_, board_,
                                                                   sink_, context_, clock);
  commands_ = xQueueCreate(2, sizeof(Command));
  if (fetcher_ == nullptr || updater_ == nullptr || commands_ == nullptr) {
    release();
    snprintf(error, errorSize, "no memory");
    return false;
  }
  if (!updater_->configure(payload, length, error, errorSize)) {
    release();
    return false;
  }
  fetcher_->setCancel(updater_->stopFlag());
  finished_ = false;
  applying_ = false;
  if (xTaskCreatePinnedToCore(taskEntry, "vdac4-update", kTaskStackBytes, this, kTaskPriority,
                              &task_, kTaskCore) != pdPASS) {
    finished_ = true;
    release();
    snprintf(error, errorSize, "no task");
    return false;
  }
  return true;
}

void Vdac4UpdateService::taskEntry(void* context) {
  static_cast<Vdac4UpdateService*>(context)->run();
}

void Vdac4UpdateService::run() {
  // Итог проверки плагину уже сообщён событиями; сеанс ждёт команд до STOP.
  updater_->check();
  Command command{};
  while (!*updater_->stopFlag() &&
         xQueueReceive(commands_, &command, portMAX_DELAY) == pdTRUE) {
    if (command.kind == kCommandStop || *updater_->stopFlag()) {
      break;
    }
    if (command.kind == kCommandApply) {
      updater_->apply(command.indices, command.count);
      if (updater_->restartPending()) {
        // Новая прошивка ESP сверена и стала загрузочной: дать событиям дойти и перезапуститься.
        // Плагин ждёт READY и начинает сеанс заново.
        bringupLog("zifi-vup", "ESP firmware installed: restarting");
        vTaskDelay(pdMS_TO_TICKS(kRestartDelayMs));
        board_.espRestart();
      }
      applying_ = false;
    } else if (command.kind == kCommandSync) {
      updater_->resend();
    }
  }
  finished_ = true;
  vTaskDelete(nullptr);
}

bool Vdac4UpdateService::apply(const uint8_t* payload, uint16_t length) {
  if (task_ == nullptr || finished_ || applying_ || *updater_->stopFlag() ||
      length > Vdac4Updater::kMaxRows) {
    return false;
  }
  Command command{};
  command.kind = kCommandApply;
  command.count = static_cast<uint8_t>(length);
  if (length != 0) {
    memcpy(command.indices, payload, length);
  }
  applying_ = true;
  if (xQueueSend(commands_, &command, 0) != pdTRUE) {
    applying_ = false;
    return false;
  }
  return true;
}

bool Vdac4UpdateService::sync() {
  if (task_ == nullptr || finished_ || *updater_->stopFlag()) {
    return false;
  }
  Command command{};
  command.kind = kCommandSync;
  return xQueueSend(commands_, &command, 0) == pdTRUE;
}

bool Vdac4UpdateService::stop(uint32_t timeoutMs) {
  if (task_ == nullptr) {
    return true;
  }
  updater_->requestStop();
  Command command{};
  command.kind = kCommandStop;
  xQueueSend(commands_, &command, pdMS_TO_TICKS(100));
  const uint32_t started = millis();
  while (!finished_) {
    if (static_cast<uint32_t>(millis() - started) >= timeoutMs) {
      return false;  // задача ещё в обмене; память освободит следующий stop
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  vTaskDelay(pdMS_TO_TICKS(20));
  release();
  return true;
}

}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
