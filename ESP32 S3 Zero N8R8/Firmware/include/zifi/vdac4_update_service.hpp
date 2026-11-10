#pragma once

#include "zifi/board.hpp"

#if ZIFI_HAS_SPI_LINK

#include <stddef.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "zifi/vdac4_board_esp.hpp"
#include "zifi/vdac4_updater.hpp"
#include "zifi/wc_update_service.hpp"

namespace zifi {

// Сеанс обновления платы VDAC4 (vdac4_updater) в собственной задаче ядра 0 — как WcUpdateService:
// HTTPS, LINK к Tang и VFS карты Evo блокирующие и долгие, сетевая задача только передаёт сюда
// команды плагина. Поставив прошивку ESP, задача даёт событиям дойти до Z80 и перезапускает ESP.
class Vdac4UpdateService {
 public:
  Vdac4UpdateService(VfsBridge& bridge, WcUpdater::EventSink sink, void* context);
  ~Vdac4UpdateService();

  Vdac4UpdateService(const Vdac4UpdateService&) = delete;
  Vdac4UpdateService& operator=(const Vdac4UpdateService&) = delete;

  bool start(const uint8_t* payload, uint16_t length, char* error, size_t errorSize);
  // Номера строк по байту. Принимается, пока сеанс ждёт команды и прежний APPLY закончен.
  bool apply(const uint8_t* payload, uint16_t length);
  bool sync();
  bool stop(uint32_t timeoutMs);
  bool running() const { return task_ != nullptr; }

 private:
  struct Command {
    uint8_t kind;
    uint8_t count;
    uint8_t indices[Vdac4Updater::kMaxRows];
  };
  static constexpr uint8_t kCommandApply = 1;
  static constexpr uint8_t kCommandStop = 2;
  static constexpr uint8_t kCommandSync = 3;

  static void taskEntry(void* context);
  static uint32_t clock();
  void run();
  void release();

  VfsBridge& bridge_;
  WcUpdater::EventSink sink_;
  void* context_;
  Vdac4BoardEsp board_;
  NetWcFetcher* fetcher_;
  Vdac4Updater* updater_;
  QueueHandle_t commands_;
  TaskHandle_t task_;
  volatile bool finished_;
  volatile bool applying_;
};

}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
