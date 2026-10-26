// Экран проигрывателя на HDMI Tang (screen.h): PlayerOverlay прошивки (Firmware/src/
// player_overlay.cpp: появление, кадры, уход) и отрисовка player_ui (Sound/player_ui) — без
// копии; к Tang их ведёт Port на core->link_write.
//
// Оверлей Tang — общий механизм ядра TS-Config (fpga/vdac2/rtl/fb_ovl.sv): любое место и размер
// холста (ширина до 1024), ×1 или ×2, прозрачность 0…15. Меню настроек Tang — оверлей 0 со своими
// размером и прозрачностью, экран плеера — оверлей 1 с kPlace ниже. Регион оверлеев у них общий
// (холст плеера — с PlayerOverlay::kBase до конца), но разом они не работают: меню, показываясь,
// пишет свой холст заново (прошивка 0.1.87).
//
// В модуле ESP нет конструкторов статических объектов: Port и экран создаются в screen_begin.
#include "screen.h"

#include <new>
#include <stddef.h>

#include "player_cover.h"
#include "zifi/player_overlay.hpp"

using zifi::PlayerOverlay;

namespace {

// Оверлей 1 на выводе 720p: место, масштаб. Холст — размер отрисовки (player_ui), прозрачность
// меняет PlayerOverlay (появление и уход — по ступени за кадр вывода).
struct Place {
  uint16_t x, y;
  bool x2;
};
constexpr uint16_t kOutWidth = 1280, kOutHeight = 720;
constexpr Place kPlace = {(kOutWidth - PlayerOverlay::kWidth) / 2,
                          (kOutHeight - PlayerOverlay::kHeight) / 2, false};

// Регистры оверлея k на LINK (ts_fb_core.sv): CTRL, POS, SIZE, BASE с 0xC0 + 16·k; регион
// оверлеев — окно 0x80000…0xFFFFF, слово региона n — адрес 0x80000 + 4·n.
constexpr uint32_t kCtrl = 0xC0 + 16U * PlayerOverlay::kOverlay;
constexpr uint32_t kMem = 0x80000;

const zm_core_t* core = nullptr;

bool tangReady() {
  return core->size >= offsetof(zm_core_t, ui_available) + sizeof(core->ui_available) &&
         core->ui_available != nullptr && core->link_write != nullptr &&
         core->ui_available() != 0;
}

class LinkPort : public PlayerOverlay::Port {
 public:
  bool overlaySupported() override { return tangReady(); }
  // Перезагрузка ПЛИС (другое ядро, multiboot): Tang на время пропадает — номер меняется
  uint32_t tangEpoch() override {
    const bool now = tangReady();
    if (now && !seen_) ++reloads_;
    seen_ = now;
    return reloads_;
  }
  bool configure(bool on, uint8_t alpha) override {
    if (!on) {
      const uint32_t off = 0;
      return core->link_write(kCtrl, &off, 1) == ZM_OK;
    }
    // Сначала место, размер и начало, затем включение: Tang берёт всё в начале кадра
    const uint32_t regs[3] = {
        static_cast<uint32_t>(kPlace.x & 0x7FFU) | (static_cast<uint32_t>(kPlace.y & 0x3FFU) << 16),
        static_cast<uint32_t>(PlayerOverlay::kWidth & 0x7FFU) |
            (static_cast<uint32_t>(PlayerOverlay::kHeight & 0x3FFU) << 16),
        PlayerOverlay::kBase & 0x1FFF8U,
    };
    const uint32_t ctrl = 1U | (kPlace.x2 ? 2U : 0U) | (static_cast<uint32_t>(alpha & 15) << 4);
    return core->link_write(kCtrl + 4, regs, 3) == ZM_OK && core->link_write(kCtrl, &ctrl, 1) == ZM_OK;
  }
  bool write(uint32_t word, const uint32_t* words, size_t count) override {
    return core->link_write(kMem + 4U * word, words, static_cast<uint32_t>(count)) == ZM_OK;
  }
  int model(zm_ui_model_t* model) override { return player_ui_model(model); }
  int cover(uint16_t* pixels, uint16_t side) override {
    return cover_ui_copy(pixels, side) ? ZM_OK : ZM_NOT_FOUND;
  }
  void menuYield() override {}
  void* allocate(size_t bytes) override { return core->alloc(bytes, ZM_MEM_PSRAM); }
  void release(void* pointer) override { core->free(pointer); }
  uint64_t micros() override { return static_cast<uint64_t>(core->millis()) * 1000U; }

 private:
  bool seen_ = false;
  uint32_t reloads_ = 0;
};

alignas(LinkPort) unsigned char portSpace[sizeof(LinkPort)];
alignas(PlayerOverlay) unsigned char screenSpace[sizeof(PlayerOverlay)];
PlayerOverlay* screen = nullptr;
uint32_t lastTick = 0;
volatile uint8_t flags = 0;
bool tangSeen = false;                   // для журнала: Tang был на прошлом шаге

// В журнал ядра — как прежний экран прошивки (player_overlay_task.cpp): появление, уход, сбои
void report(PlayerOverlay::State from, PlayerOverlay::State to) {
  const PlayerOverlay::Stats& s = screen->stats();
  if (to == PlayerOverlay::State::kFadeIn && from == PlayerOverlay::State::kOff) {
    core->log("player: screen %u on Tang: canvas sent in %u ms, fading in", (unsigned)s.shows,
              (unsigned)(s.fullUploadUs / 1000));
  } else if (to == PlayerOverlay::State::kOff) {
    core->log("player: screen off: %u frames, last %u words, worst render %u us, upload %u us; "
              "write fails %u, no memory %u",
              (unsigned)s.frames, (unsigned)s.words, (unsigned)s.worstRenderUs,
              (unsigned)s.worstUploadUs, (unsigned)s.writeFails, (unsigned)s.noMemory);
  }
}

}  // namespace

#ifdef __XTENSA__
// Port прошивки абстрактный: его таблица виртуальных функций ссылается на __cxa_pure_virtual, а
// деструктор — на operator delete. Ядро ESP модулю их не даёт; объекты здесь не удаляются.
extern "C" void __cxa_pure_virtual() {
  for (;;) {
  }
}
void operator delete(void*) noexcept {}
void operator delete(void*, size_t) noexcept {}
#endif

extern "C" int screen_begin(const zm_core_t* c) {
  core = c;
  if (core->size < offsetof(zm_core_t, ui_available) + sizeof(core->ui_available)) {
    return ZM_ERROR;                     // ядро старше ABI 1.5: Tang экрану не дать
  }
  LinkPort* port = new (portSpace) LinkPort();
  screen = new (screenSpace) PlayerOverlay(*port);
  lastTick = core->millis() - PlayerOverlay::kTickMs;
  flags = 0;
  tangSeen = false;
  return ZM_OK;
}

extern "C" uint32_t screen_tick(uint32_t now) {
  if (screen == nullptr) {
    return 200;
  }
  const uint32_t since = now - lastTick;
  if (since < PlayerOverlay::kTickMs) {
    return PlayerOverlay::kTickMs - since;
  }
  lastTick = since < 2 * PlayerOverlay::kTickMs ? lastTick + PlayerOverlay::kTickMs : now;
  const PlayerOverlay::State before = screen->state();
  const uint32_t fails = screen->stats().writeFails, misses = screen->stats().noMemory;
  screen->tick(now);
  if (screen->state() != before) report(before, screen->state());
  else if (screen->stats().writeFails != fails || screen->stats().noMemory != misses) {
    core->log("player: screen not shown: write fails %u, no memory %u",
              (unsigned)screen->stats().writeFails, (unsigned)screen->stats().noMemory);
  }
  const bool tang = tangReady();
  if (tang != tangSeen) {
    core->log("player: screen: Tang with overlays %s", tang ? "here" : "gone");
    tangSeen = tang;
  }
  flags = static_cast<uint8_t>((tang ? SCREEN_F_TANG : 0) |
                               (screen->state() != PlayerOverlay::State::kOff ? SCREEN_F_SHOWN : 0));
  const uint32_t spent = core->millis() - now;
  return spent >= PlayerOverlay::kTickMs ? 1 : PlayerOverlay::kTickMs - spent;
}

extern "C" void screen_end(void) {
  if (screen == nullptr) {
    return;
  }
  // Задача обложек уже вышла: экран — отсюда. Плагин ушёл (снимок не виден) — уйти плавно, по
  // ступени прозрачности за кадр; виден (приложение перезапускают) — сразу.
  for (int step = 0; step < 20 && screen->state() != PlayerOverlay::State::kOff; ++step) {
    screen->tick(core->millis());
    if (step >= 2 && screen->state() != PlayerOverlay::State::kFadeOut) break;
    core->delay(PlayerOverlay::kTickMs);
  }
  const PlayerOverlay::State before = screen->state();
  screen->appGone();
  if (before != PlayerOverlay::State::kOff) report(before, PlayerOverlay::State::kOff);
  screen->~PlayerOverlay();
  screen = nullptr;
  flags = 0;
}

extern "C" uint8_t screen_flags(void) {
  // Tang — и до первого шага (ответ AUDIO_OPEN приходит раньше, чем экран что-то сделал)
  return static_cast<uint8_t>(flags | (screen != nullptr && tangReady() ? SCREEN_F_TANG : 0));
}
