#include "zifi/link_watchdog.hpp"

#if ZIFI_HAS_SPI_LINK

#include <atomic>

#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "zifi/bringup_log.hpp"
#include "zifi/diagnostic_log.hpp"
#include "zifi/spi_link.hpp"
#include "zifi/spi_link_core.hpp"
#include "zifi/vfs_bridge.hpp"

namespace zifi {

namespace {

constexpr char kTag[] = "zifi-wdog";
constexpr uint32_t kPeriodMs = 500;
// Ответ на запрос задача обмена даёт за доли миллисекунды; долгие дела ядра 1
// (запись на карту, сеть) её не задерживают. 15 с без ответа — она встала.
constexpr uint32_t kStuckMs = 15000;

std::atomic<uint8_t> g_coreStep{0};
std::atomic<uint8_t> g_coreDetail{0};
std::atomic<uint32_t> g_coreSince{0};

struct Watched {
  SpiLink* link;
  VfsBridge* bridge;
};

uint32_t nowMs() { return static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS); }

void watchTask(void* context) {
  const Watched watched = *static_cast<Watched*>(context);
  SpiLink& link = *watched.link;
  uint32_t stuckSince = 0;
  uint32_t selectsAtStart = 0;
  uint32_t handledAtStart = 0;
  uint32_t rejectedAtStart = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(kPeriodMs));
    if (!link.ready()) {
      continue;
    }
    uint8_t head[SpiLinkCore::kStateSize] = {};
    uint8_t state[SpiLinkCore::kStateSize] = {};
    link.sharedWords(head, state);
    const uint32_t selects = link.lines().selects;
    const uint32_t handled = link.handled();
    const uint32_t rejected = link.rejected();
    const bool unanswered = head[0] == SpiLinkCore::kRequestMarker;
    const bool stateBad = (state[0] != SpiLinkCore::kStateEmpty &&
                           state[0] != SpiLinkCore::kStatePending) ||
                          state[1] != SpiLinkCore::kMagic;
    if (stuckSince == 0) {
      if (unanswered || stateBad) {
        stuckSince = nowMs() | 1;
        selectsAtStart = selects;
        handledAtStart = handled;
        rejectedAtStart = rejected;
      }
      continue;
    }
    // Задача обмена жива, если с начала подозрения обработала или отвергла
    // хоть один запрос; без обращений Z80 зависание не видно и не важно.
    const bool taskAlive = handled != handledAtStart || rejected != rejectedAtStart;
    if ((!unanswered || taskAlive) && !stateBad) {
      stuckSince = 0;
      continue;
    }
    if (stateBad && taskAlive) {
      stuckSince = 0;                    // слово состояния обновится с ответом
      continue;
    }
    const uint32_t stuckMs = nowMs() - stuckSince;
    if (stuckMs < kStuckMs || selects == selectsAtStart) {
      continue;
    }
    const uint32_t coreMs = nowMs() - g_coreSince.load(std::memory_order_relaxed);
    VfsBridge& bridge = *watched.bridge;
    bringupLog(kTag,
               "link stuck %u s (%s): Z80 selects +%u, handled %u, rejected %u; spi step %u; "
               "core1 step %u/%02X for %u ms; vfs %s op %u for %u ms; to Z80 %u, from Z80 %u; "
               "head %02X %02X %02X %02X state %02X %02X %02X %02X; restarting",
               static_cast<unsigned>(stuckMs / 1000),
               unanswered ? "request unanswered" : "state word bad",
               static_cast<unsigned>(selects - selectsAtStart), static_cast<unsigned>(handled),
               static_cast<unsigned>(rejected), static_cast<unsigned>(link.taskStep()),
               static_cast<unsigned>(g_coreStep.load(std::memory_order_relaxed)),
               static_cast<unsigned>(g_coreDetail.load(std::memory_order_relaxed)),
               static_cast<unsigned>(coreMs), bridge.requestPending() ? "busy" : "idle",
               static_cast<unsigned>(bridge.pendingOperation()),
               static_cast<unsigned>(bridge.requestPending()
                                         ? nowMs() - bridge.pendingSinceMs()
                                         : 0),
               static_cast<unsigned>(link.pendingToZ80()),
               static_cast<unsigned>(link.pendingFromZ80()), head[0], head[1], head[2], head[3],
               state[0], state[1], state[2], state[3]);
    // Строку во flash обычно переносит сетевая задача; перед перезапуском — сами.
    diagnosticLogFlush();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
  }
}

}  // namespace

void coreStep(CoreStep step, uint8_t detail) {
  const uint8_t value = static_cast<uint8_t>(step);
  if (g_coreStep.load(std::memory_order_relaxed) != value ||
      g_coreDetail.load(std::memory_order_relaxed) != detail) {
    g_coreStep.store(value, std::memory_order_relaxed);
    g_coreDetail.store(detail, std::memory_order_relaxed);
    g_coreSince.store(nowMs(), std::memory_order_relaxed);
  }
}

void startLinkWatchdog(SpiLink& link, VfsBridge& bridge) {
  static Watched watched;
  watched.link = &link;
  watched.bridge = &bridge;
  // Ядро 0, выше простоя и ниже Wi-Fi: от задачи обмена и ядра 1 не зависит.
  if (xTaskCreatePinnedToCore(watchTask, "zifi-wdog", 4096, &watched, 3, nullptr, 0) != pdPASS) {
    bringupLog(kTag, "link watchdog: not started");
  }
}

}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
