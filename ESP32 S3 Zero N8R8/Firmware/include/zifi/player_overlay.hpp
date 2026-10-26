#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zm_app.h"
#include "zifi/player_view.hpp"

namespace zifi {

// Экран проигрывателя оверлеем Tang (ABI приложений 1.5, zm_ui_model_t; Docs/VDAC4.md, «Экран
// плеера на HDMI Tang»). Есть Tang с ядром TS-Config 1.10+ — интерфейс проигрывателя рисует ESP
// (player_view) и выводит оверлеем 1 поверх картинки Evo: холст 640×360 ×2 — весь кадр 1280×720,
// непрозрачный; появляется и уходит плавно — общей прозрачностью оверлея 0 → 15 и обратно, по
// ступени за кадр вывода (0,32 с). Tang нет — ничего: окно плагина на экране Evo как было.
//
// Холст лежит в регионе оверлеев с слова kBase до конца (115 200 слов) и задевает конец холста
// меню настроек Tang (слова 0…22 847): показываясь, экран просит меню уйти и забыть свой холст
// (tang_menu::yield). Двойного буфера нет — места в регионе на второй холст не хватает.
//
// PlayerOverlay — сама логика, без FreeRTOS и LINK (стенд ПК: tests/player_overlay_host.cpp):
// всё внешнее — через Port. Задача экрана (player_overlay::start) зовёт tick() каждые 20 мс.
class PlayerOverlay {
 public:
  static constexpr int kOverlay = 1;                      // поверх меню настроек (оверлей 0)
  static constexpr uint16_t kWidth = player_view::kWidth;
  static constexpr uint16_t kHeight = player_view::kHeight;
  static constexpr uint32_t kStride = ((kWidth + 15U) / 16U) * 8U;   // слов на строку холста
  static constexpr uint32_t kRegionWords = 131072;
  static constexpr uint32_t kBase = kRegionWords - kStride * kHeight;  // 15 872
  static constexpr uint32_t kTickMs = 20;                 // кадр вывода 720p50
  static constexpr uint32_t kFrameTicks = 2;              // обычный кадр экрана — 25 раз в секунду
  static constexpr uint8_t kOpaque = 15;
  static constexpr uint32_t kModelLostMs = 1000;          // снимков нет — уйти
  static constexpr uint32_t kRetryMs = 1000;              // не вышло показать — снова не раньше
  static constexpr uint32_t kReleaseHoldMs = 5000;        // память отдана приложению — не просить
  static constexpr int kMaxDirty = 32;

  enum class State : uint8_t { kOff, kFadeIn, kShown, kFadeOut };

  struct Config {                                         // настройки оверлея (tang_overlay)
    bool on;
    uint8_t alpha;
  };

  class Port {
   public:
    virtual ~Port() = default;
    // Tang на LINK с ядром, у которого есть оверлеи.
    virtual bool overlaySupported() = 0;
    // Меняется при каждой перезагрузке ПЛИС (холсты в SDRAM Tang пропали, оверлеи выключены).
    virtual uint32_t tangEpoch() = 0;
    // Оверлей kOverlay: вкл., ×2, прозрачность, на весь кадр, холст с kBase.
    virtual bool configure(bool on, uint8_t alpha) = 0;
    // Слова региона оверлеев подряд с word (до 16 за раз).
    virtual bool write(uint32_t word, const uint32_t* words, size_t count) = 0;
    // Снимок у приложения (ZM_OK) и его обложка side×side.
    virtual int model(zm_ui_model_t* model) = 0;
    virtual int cover(uint16_t* pixels, uint16_t side) = 0;
    // Меню настроек Tang: скрыться и забыть холст (экран его задевает).
    virtual void menuYield() = 0;
    // Память под холст и отрисовку (PSRAM) — может не дать.
    virtual void* allocate(size_t bytes) = 0;
    virtual void release(void* pointer) = 0;
    // Время в мкс — для замеров.
    virtual uint64_t micros() = 0;
  };

  struct Stats {
    uint32_t shows = 0;           // сколько раз экран появлялся
    uint32_t frames = 0;          // обычных кадров
    uint32_t words = 0;           // слов за последний кадр
    uint32_t renderUs = 0;        // последний кадр: отрисовка и передача
    uint32_t uploadUs = 0;
    uint32_t worstRenderUs = 0;
    uint32_t worstUploadUs = 0;
    uint32_t fullUploadUs = 0;    // весь холст при появлении
    uint32_t writeFails = 0;
    uint32_t noMemory = 0;
  };

  explicit PlayerOverlay(Port& port) : port_(port) {}
  ~PlayerOverlay() { freeMemory(); }
  PlayerOverlay(const PlayerOverlay&) = delete;
  PlayerOverlay& operator=(const PlayerOverlay&) = delete;

  // Шаг раз в kTickMs.
  void tick(uint32_t nowMs);
  // Память нужна другим (приложению не хватило PSRAM): сразу скрыть экран, отдать холст и
  // kReleaseHoldMs не показываться.
  void releaseMemory(uint32_t nowMs);
  // Приложение выгружено: скрыть сразу (его данных больше нет).
  void appGone();

  State state() const { return state_; }
  uint8_t alpha() const { return alpha_; }
  const Stats& stats() const { return stats_; }
  const uint16_t* canvas() const { return canvas_; }

 private:
  bool ensureMemory();
  void freeMemory();
  bool fetchModel(uint32_t nowMs);
  void fetchCover();
  bool show(uint32_t nowMs);
  void frame(uint32_t nowMs);
  bool upload(const player_view::Rect* rects, int count);
  bool setAlpha(uint8_t alpha);
  void off();

  Port& port_;
  State state_ = State::kOff;
  uint8_t alpha_ = 0;
  uint32_t tick_ = 0;
  uint32_t epoch_ = 0;
  bool epochKnown_ = false;
  uint32_t lastModelMs_ = 0;
  uint32_t retryAtMs_ = 0;
  bool haveModel_ = false;
  uint16_t coverSerial_ = 0;
  bool coverOk_ = false;
  bool coverChanged_ = false;
  bool viewReady_ = false;
  uint16_t* canvas_ = nullptr;
  uint16_t* cover_ = nullptr;
  zm_ui_model_t model_ = {};
  Stats stats_;
};

namespace player_overlay {

// Задача экрана (ядро 1, ниже звука и обмена): после tang::start.
void start();
// Для core->ui_available: экран сейчас можно показать.
bool available();
// См. PlayerOverlay::releaseMemory / appGone — из любой задачи.
void releaseMemory();
void appGone();
// Строка состояния для сетевой консоли («tang player»).
void describe(char* out, size_t size);

}  // namespace player_overlay
}  // namespace zifi
