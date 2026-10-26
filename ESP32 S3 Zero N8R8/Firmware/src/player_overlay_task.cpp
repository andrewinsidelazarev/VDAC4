// Задача экрана проигрывателя на ESP (player_overlay.hpp): Port к LINK Tang, приложению и
// памяти, шаг PlayerOverlay каждые 20 мс на ядре 1 — ниже звука, обмена и помощника декодеров.
#include "zifi/player_overlay.hpp"

#include "zifi/board.hpp"

#if ZIFI_HAS_SPI_LINK && ZIFI_HAS_AUDIO

#include <atomic>
#include <new>
#include <stdio.h>

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "zifi/apps/module_app.hpp"
#include "zifi/bringup_log.hpp"
#include "zifi/tang_link.hpp"
#include "zifi/tang_menu.hpp"
#include "zifi/tang_overlay.hpp"

namespace zifi {
namespace player_overlay {
namespace {

constexpr char kTag[] = "zifi-screen";
constexpr uint32_t kStackBytes = 6144;
constexpr UBaseType_t kPriority = 2;
constexpr BaseType_t kCore = 1;
// Холст берётся, только если PSRAM после него останется столько: модулю, банку MIDI, очереди.
constexpr size_t kReserveBytes = 1024U * 1024U;
constexpr uint32_t kReleaseWaitMs = 100;

class EspPort : public PlayerOverlay::Port {
 public:
  bool overlaySupported() override { return tang_overlay::supported(); }
  uint32_t tangEpoch() override {
    return (tang::reloads() << 1) | (tang::ready() ? 1U : 0U);
  }
  bool configure(bool on, uint8_t alpha) override {
    if (!on) return tang_overlay::hide(PlayerOverlay::kOverlay);
    tang_overlay::Config c;
    c.on = true;
    c.x2 = true;
    c.alpha = alpha;
    c.x = 0;
    c.y = 0;
    c.w = PlayerOverlay::kWidth;
    c.h = PlayerOverlay::kHeight;
    c.base = PlayerOverlay::kBase;
    return tang_overlay::configure(PlayerOverlay::kOverlay, c);
  }
  bool write(uint32_t word, const uint32_t* words, size_t count) override {
    return tang::write(tang_overlay::kMemBase + 4U * word, words, count);
  }
  int model(zm_ui_model_t* model) override { return ModuleApp::uiModel(model); }
  int cover(uint16_t* pixels, uint16_t side) override { return ModuleApp::uiCover(pixels, side); }
  void menuYield() override { tang_menu::yield(); }
  void* allocate(size_t bytes) override {
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < bytes + kReserveBytes) {
      return nullptr;
    }
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  void release(void* pointer) override { heap_caps_free(pointer); }
  uint64_t micros() override { return static_cast<uint64_t>(esp_timer_get_time()); }
};

EspPort g_port;
alignas(PlayerOverlay) unsigned char g_space[sizeof(PlayerOverlay)];
PlayerOverlay* g_overlay = nullptr;
TaskHandle_t g_task = nullptr;
std::atomic<bool> g_release{false};
std::atomic<bool> g_gone{false};

const char* stateName(PlayerOverlay::State state) {
  switch (state) {
    case PlayerOverlay::State::kFadeIn: return "fading in";
    case PlayerOverlay::State::kShown: return "shown";
    case PlayerOverlay::State::kFadeOut: return "fading out";
    default: return "off";
  }
}

void report(PlayerOverlay::State from, PlayerOverlay::State to) {
  const PlayerOverlay::Stats& s = g_overlay->stats();
  if (to == PlayerOverlay::State::kFadeIn && from == PlayerOverlay::State::kOff) {
    bringupLog(kTag, "screen %u: canvas sent in %u ms, fading in", static_cast<unsigned>(s.shows),
               static_cast<unsigned>(s.fullUploadUs / 1000));
  } else if (to == PlayerOverlay::State::kOff) {
    bringupLog(kTag,
               "screen off: %u frames, last %u words, worst render %u us, upload %u us; "
               "write fails %u, no memory %u",
               static_cast<unsigned>(s.frames), static_cast<unsigned>(s.words),
               static_cast<unsigned>(s.worstRenderUs), static_cast<unsigned>(s.worstUploadUs),
               static_cast<unsigned>(s.writeFails), static_cast<unsigned>(s.noMemory));
  }
}

void task(void*) {
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(PlayerOverlay::kTickMs));
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    const PlayerOverlay::State before = g_overlay->state();
    if (g_gone.exchange(false)) g_overlay->appGone();
    if (g_release.load()) {
      g_overlay->releaseMemory(now);
      g_release.store(false);
    }
    g_overlay->tick(now);
    if (g_overlay->state() != before) report(before, g_overlay->state());
  }
}

}  // namespace

void start() {
  if (g_overlay != nullptr) return;
  g_overlay = new (g_space) PlayerOverlay(g_port);
  // стек — в PSRAM (внутренней памяти мало): задача не трогает flash ESP, LINK копирует данные
  // в свои буферы, журнал — очередью
  if (xTaskCreatePinnedToCoreWithCaps(task, "zifi-screen", kStackBytes, nullptr, kPriority, &g_task,
                                      kCore, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    g_task = nullptr;
    bringupLog(kTag, "screen task not started");
  }
}

bool available() { return g_task != nullptr && tang_overlay::supported(); }

void releaseMemory() {
  if (g_task == nullptr || g_overlay->canvas() == nullptr) return;
  g_release.store(true);
  if (xTaskGetCurrentTaskHandle() == g_task) return;   // из самой задачи экрана — не ждать
  for (uint32_t waited = 0; g_release.load() && waited < kReleaseWaitMs; ++waited) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void appGone() {
  if (g_task != nullptr) g_gone.store(true);
}

void describe(char* out, size_t size) {
  if (g_overlay == nullptr) {
    snprintf(out, size, "no screen task");
    return;
  }
  const PlayerOverlay::Stats& s = g_overlay->stats();
  snprintf(out, size,
           "%s, alpha %u, overlays %s; shows %u (canvas %u ms), frames %u, last %u words, "
           "render %u us (worst %u), upload %u us (worst %u), write fails %u, no memory %u",
           stateName(g_overlay->state()), static_cast<unsigned>(g_overlay->alpha()),
           tang_overlay::supported() ? "yes" : "no", static_cast<unsigned>(s.shows),
           static_cast<unsigned>(s.fullUploadUs / 1000), static_cast<unsigned>(s.frames),
           static_cast<unsigned>(s.words), static_cast<unsigned>(s.renderUs),
           static_cast<unsigned>(s.worstRenderUs), static_cast<unsigned>(s.uploadUs),
           static_cast<unsigned>(s.worstUploadUs), static_cast<unsigned>(s.writeFails),
           static_cast<unsigned>(s.noMemory));
}

}  // namespace player_overlay
}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK && ZIFI_HAS_AUDIO
