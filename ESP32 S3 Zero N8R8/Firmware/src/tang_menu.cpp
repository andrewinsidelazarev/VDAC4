#include "zifi/tang_menu.hpp"

#if ZIFI_HAS_SPI_LINK

#include <atomic>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "zifi/tang_link.hpp"
#include "zifi/tang_menu_font.hpp"
#include "zifi/tang_overlay.hpp"

namespace zifi {
namespace tang_menu {

namespace {

constexpr char kTag[] = "zifi-menu";

// Панель: 40 знаков в строке, до 16 строк по 8×8 точек холста и поле 8 точек вокруг; на
// выводе — ×2 (знак 16×16, как у прежнего меню ядра), по центру кадра 1280×720. Только
// настройки: заголовок, пункты этого ядра, подсказка; состояние входа — в сетевой консоли
// («tang»). Высота — по числу пунктов (у ядра FT812 их меньше).
constexpr int kCols = 40;
constexpr int kRows = 16;
constexpr int kFirstItem = 2;                       // строка первого пункта
constexpr int kBorder = 8;
constexpr uint16_t kW = kCols * 8 + 2 * kBorder;   // 336
constexpr uint16_t kH = kRows * 8 + 2 * kBorder;   // 144
constexpr uint32_t kStride = tang_overlay::strideWords(kW);
constexpr uint16_t kOutW = 1280, kOutH = 720;
constexpr uint32_t kBase = 0;                       // начало холста в регионе оверлеев:
                                                    // слова 0…24 191; экран плеера — с 15 872
                                                    // (player_overlay): холст общий, см. yield()
constexpr int kOverlay = 0;
constexpr uint8_t kAlpha = 12;                      // картинка под панелью видна на 3/16

constexpr uint16_t rgb565(unsigned r, unsigned g, unsigned b) {
  return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
constexpr uint16_t kBg = rgb565(0, 0, 48);
constexpr uint16_t kText = rgb565(255, 255, 255);
constexpr uint16_t kMark = rgb565(255, 224, 0);

constexpr uint32_t kRefreshMs = 250;    // строки состояния
// Без нажатий меню скрывается: плагин WC, пока открыт, повторяет «показать» раз в 5 с, а сброса
// Evo ESP не видит — если Evo сбросили с открытым плагином, меню уходит после двух пропущенных
// повторов, а не висит после старта Evo (ядро до 1.9 держало своё меню 20 с).
constexpr uint32_t kHideMs = 12000;

// Пункты (курсор)
enum Item : int {
  kPhase, kDelay, kClock, kHAdjust, kVAdjust, kAlign, kSdRead, kCalibrate, kAspect, kSize,
  kBlend, kSource, kItems
};
// FRAME BLEND (CTRL 17, CHECK 18 — смешивает сейчас): ядро TS-Config 1.32+
constexpr uint32_t kBlendCore = 0x0120;
// События нажатий
enum Event : uint8_t { kNext = 1, kPrev = 2, kInc = 3, kDec = 4, kShow = 5, kHide = 6 };

// Регистры Tang
constexpr uint32_t kCtrlMenuBits = (1U << 8) | (1U << 9);   // 8 — прежнее меню, 9 — калибровка

struct Regs {
  uint32_t status = 0, raster = 0, sync = 0, window = 0, picx = 0, picy = 0, frames = 0,
           check = 0, ctrl = 0, adjust = 0;
};

QueueHandle_t g_keys = nullptr;
uint16_t* g_canvas = nullptr;
bool g_ready = false;        // ядро с оверлеями, холст есть
bool g_shown = false;        // после запуска меню скрыто: показывает его плагин WC
bool g_ovlOn = false;
bool g_uploaded = false;     // поле холста записано в Tang (до скрытия меню)
bool g_dirty = true;
std::atomic<bool> g_yield{false};   // экран плеера затёр холст в Tang: скрыться, потом — заново
int g_cursor = 0;            // номер видимого пункта (g_items)
// Пункты этого ядра: TS-Config — все; ядро FT812 (VERSION, личность 02, ядро 1.12+) — без
// мультиплексора и окна захвата: Evo оно не принимает
bool g_ftCore = false;
int g_items[kItems];
int g_count = kItems;
int panelRows() { return kFirstItem + g_count + 2; }
uint16_t panelH() { return static_cast<uint16_t>(panelRows() * 8 + 2 * kBorder); }
uint32_t g_lastRefreshMs = 0;
uint32_t g_lastKeyMs = 0;
Regs g_regs;
char g_lines[kRows][kCols + 1];
bool g_lineOk[kRows];

bool readRegs(Regs* r) {
  uint32_t a[8];
  uint32_t b[2];
  if (!tang::read(tang::kRegStatus, a, 8) || !tang::read(tang::kRegCtrl, b, 2)) {
    return false;
  }
  r->status = a[0]; r->raster = a[1]; r->sync = a[2]; r->window = a[3];
  r->picx = a[4]; r->picy = a[5]; r->frames = a[6]; r->check = a[7];
  r->ctrl = b[0]; r->adjust = b[1];
  return true;
}

bool bit(uint32_t v, int n) { return ((v >> n) & 1U) != 0; }

// Строка из 40 знаков: printf, обрезка, дополнение пробелами, верхний регистр
void setLine(char* out, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void setLine(char* out, const char* fmt, ...) {
  char tmp[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  size_t n = strlen(tmp);
  for (int i = 0; i < kCols; ++i) {
    char c = i < static_cast<int>(n) ? tmp[i] : ' ';
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (c < 0x20 || c > 0x5F) c = ' ';
    out[i] = c;
  }
  out[kCols] = 0;
}

void buildLines(const Regs& r, char lines[kRows][kCols + 1]) {
  const bool calBusy = bit(r.status, 13), calDone = bit(r.status, 14), calFail = bit(r.status, 15);
  const unsigned src = r.ctrl & 3;
  const unsigned size = (r.ctrl >> 15) & 3;
  const unsigned clock = (r.ctrl >> 10) & 15;
  const int h = static_cast<int8_t>(r.adjust & 0xFF);
  const int v = static_cast<int8_t>((r.adjust >> 8) & 0xFF);
  const unsigned calMap = (r.sync >> 16) & 15;
  char map[5];
  for (int p = 0; p < 4; ++p) map[p] = calBusy ? '.' : (((calMap >> p) & 1) ? '+' : '-');
  map[4] = 0;
  setLine(lines[0], g_ftCore ? "VDAC4  FT812 -> HDMI 1280X720P50" : "VDAC4  ZX EVOLUTION -> HDMI 1280X720P50");
  setLine(lines[1], " ");
  for (int i = 0; i < g_count; ++i) {
    char* out = lines[kFirstItem + i];
    const char c = g_cursor == i ? '>' : ' ';
    switch (g_items[i]) {
      case kPhase: setLine(out, "%c MUX PHASE   %u", c, static_cast<unsigned>(bit(r.ctrl, 2))); break;
      case kDelay: setLine(out, "%c MUX DELAY   %u", c, static_cast<unsigned>(((r.ctrl >> 3) & 3) + 1)); break;
      case kClock: setLine(out, "%c MUX CLOCK   %X", c, clock); break;
      case kHAdjust: setLine(out, "%c H ADJUST  %c%02d", c, h < 0 ? '-' : '+', h < 0 ? -h : h); break;
      case kVAdjust: setLine(out, "%c V ADJUST  %c%02d", c, v < 0 ? '-' : '+', v < 0 ? -v : v); break;
      case kAlign: setLine(out, "%c AUTO ALIGN  %s", c, bit(r.ctrl, 5) ? "ON" : "OFF"); break;
      case kSdRead:
        setLine(out, "%c SD READ     %u   MAP %s", c, static_cast<unsigned>((r.ctrl >> 6) & 3), map);
        break;
      case kCalibrate:
        setLine(out, "%c SD CALIBRATE    %s", c, calBusy ? "RUN" : calFail ? "FAIL" : calDone ? "OK" : "");
        break;
      case kAspect: setLine(out, "%c ASPECT      %s", c, bit(r.ctrl, 14) ? "16:9" : "4:3"); break;
      case kSize: setLine(out, "%c SIZE        %s", c, size == 1 ? "95%" : size == 2 ? "90%" : "100%"); break;
      case kBlend:
        setLine(out, "%c FRAME BLEND %-4s   NOW %s", c, bit(r.ctrl, 17) ? "ON" : "OFF",
                bit(r.check, 18) ? "ON" : "OFF");
        break;
      default:
        setLine(out, "%c SOURCE      %-4s   NOW %s", c, src == 1 ? "EVO" : src == 2 ? "TEST" : "AUTO",
                bit(r.status, 9) ? "TEST" : g_ftCore ? "FT812" : "EVO");
        break;
    }
  }
  setLine(lines[kFirstItem + g_count], " ");
  setLine(lines[kFirstItem + g_count + 1], "UP/DOWN ITEM  LEFT/RIGHT VALUE  ESC EXIT");   // клавиши плагина WC
}
static_assert(kFirstItem + kItems + 2 == kRows, "строки панели: заголовок, пункты, подсказка");

uint16_t* canvasRow(int y) { return g_canvas + static_cast<size_t>(y) * kStride * 2; }

// Строка текста line → 8 строк холста
void drawLine(int line, const char* text) {
  const bool mark = line >= kFirstItem && line < kFirstItem + g_count && text[0] == '>';
  for (int fy = 0; fy < 8; ++fy) {
    uint16_t* row = canvasRow(kBorder + line * 8 + fy);
    for (int c = 0; c < kCols; ++c) {
      const uint8_t bits = kMenuFont[static_cast<uint8_t>(text[c]) - 0x20][fy];
      const uint16_t ink = (mark && c == 0) || line == 0 ? kMark : kText;
      uint16_t* px = row + kBorder + c * 8;
      for (int fx = 0; fx < 8; ++fx) px[fx] = (bits & (0x80 >> fx)) ? ink : kBg;
    }
  }
}

bool uploadRows(int y0, int rows) {
  return tang_overlay::writeRows(kBase, kStride, static_cast<uint32_t>(y0),
                                 static_cast<uint32_t>(rows), canvasRow(y0));
}

bool setOverlay(bool on) {
  if (!on) {
    g_ovlOn = !tang_overlay::hide(kOverlay) && g_ovlOn;
    return !g_ovlOn;
  }
  tang_overlay::Config c;
  c.on = true;
  c.x2 = true;
  c.alpha = kAlpha;
  c.w = kW;
  c.h = panelH();
  c.x = static_cast<uint16_t>((kOutW - 2 * kW) / 2);
  c.y = static_cast<uint16_t>((kOutH - 2 * panelH()) / 2);
  c.base = kBase;
  g_ovlOn = tang_overlay::configure(kOverlay, c);
  return g_ovlOn;
}

// Изменить пункт: настройки — заново из Tang (их мог поменять и плагин WC)
void change(int item, bool up) {
  uint32_t r[2];
  uint32_t st = 0;
  if (!tang::read(tang::kRegCtrl, r, 2) || !tang::read(tang::kRegStatus, &st, 1)) {
    return;
  }
  uint32_t ctrl = r[0] & ~kCtrlMenuBits;
  int h = static_cast<int8_t>(r[1] & 0xFF);
  int v = static_cast<int8_t>((r[1] >> 8) & 0xFF);
  const bool calBusy = bit(st, 13);
  auto field = [&](int lo, int bits, unsigned value) {
    const uint32_t m = ((1U << bits) - 1U) << lo;
    ctrl = (ctrl & ~m) | ((value << lo) & m);
  };
  auto step = [&](unsigned value, unsigned n) { return up ? (value + 1) % n : (value + n - 1) % n; };
  bool writeCtrl = true, writeAdj = false;
  switch (item) {
    case kPhase: field(2, 1, bit(ctrl, 2) ? 0 : 1); break;
    case kDelay: field(3, 2, step((ctrl >> 3) & 3, 4)); break;
    case kClock: field(10, 4, step((ctrl >> 10) & 15, 16)); break;
    case kHAdjust: writeCtrl = false; writeAdj = up ? h < 63 : h > -64; h += up ? 1 : -1; break;
    case kVAdjust: writeCtrl = false; writeAdj = up ? v < 63 : v > -64; v += up ? 1 : -1; break;
    case kAlign: field(5, 1, bit(ctrl, 5) ? 0 : 1); break;
    case kSdRead:
      if (calBusy) return;
      field(6, 2, step((ctrl >> 6) & 3, 4));
      break;
    case kCalibrate: ctrl |= 1U << 9; break;
    case kAspect: field(14, 1, bit(ctrl, 14) ? 0 : 1); break;
    case kSize: field(15, 2, step((ctrl >> 15) & 3, 3)); break;
    case kBlend: field(17, 1, bit(ctrl, 17) ? 0 : 1); break;
    default: field(0, 2, step(ctrl & 3, 3)); break;   // источник
  }
  if (writeCtrl) {
    tang::write(tang::kRegCtrl, &ctrl, 1);
  }
  if (writeAdj) {
    const uint32_t adj = (static_cast<uint32_t>(h) & 0xFF) | ((static_cast<uint32_t>(v) & 0xFF) << 8);
    tang::write(tang::kRegAdjust, &adj, 1);
  }
  g_dirty = true;
}

void onEvent(uint8_t e) {
  if (e == kShow) {
    g_shown = true;
  } else if (e == kHide) {
    g_shown = false;
  } else if (!g_shown) {
    g_shown = true;                       // скрытое меню первое нажатие только показывает
  } else if (e == kNext) {
    g_cursor = g_cursor == g_count - 1 ? 0 : g_cursor + 1;
  } else if (e == kPrev) {
    g_cursor = g_cursor == 0 ? g_count - 1 : g_cursor - 1;
  } else if (e == kInc || e == kDec) {
    change(g_items[g_cursor], e == kInc);
  }
  g_dirty = true;
}

}  // namespace

void begin() {
  // Заново и после перезагрузки Tang (multiboot): у нового ядра может не быть оверлеев, а у
  // свежезагруженной ПЛИС оверлей выключен — прежние g_ready и g_ovlOn к ней не относятся
  g_ready = false;
  g_ovlOn = false;
  if (g_keys == nullptr) {
    g_keys = xQueueCreate(16, sizeof(uint8_t));
  }
  if (!tang_overlay::supported()) {
    ESP_LOGI(kTag, "Tang core %08X: no overlays, the menu stays in the core",
             static_cast<unsigned>(tang::coreVersion()));
    return;
  }
  if (g_canvas == nullptr) {
    g_canvas = static_cast<uint16_t*>(
        heap_caps_malloc(static_cast<size_t>(kH) * kStride * 2 * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
  }
  if (g_canvas == nullptr || g_keys == nullptr) {
    ESP_LOGE(kTag, "no memory for the menu canvas");
    return;
  }
  // Пункты — по ядру: после смены ядра (multiboot) begin() вызывается заново. FRAME BLEND —
  // только TS-Config 1.32+: у других ядер бит CTRL ничего не меняет
  g_ftCore = (tang::coreVersion() >> 24) == 0x02;
  const bool blend = !g_ftCore && (tang::coreVersion() & 0xFFFF) >= kBlendCore;
  g_count = 0;
  for (int item = 0; item < kItems; ++item) {
    const bool capture = item == kPhase || item == kDelay || item == kClock || item == kHAdjust ||
                         item == kVAdjust || item == kAlign;
    if (item == kBlend && !blend) continue;
    if (!g_ftCore || !capture) g_items[g_count++] = item;
  }
  g_cursor = 0;
  for (size_t i = 0; i < static_cast<size_t>(kH) * kStride * 2; ++i) g_canvas[i] = kBg;
  memset(g_lineOk, 0, sizeof(g_lineOk));
  g_ready = true;
  g_uploaded = false;
  g_shown = false;
  g_dirty = true;
}

bool active() { return g_ready; }

void key(uint8_t code) {
  if (g_keys != nullptr && code >= kNext && code <= kHide) {
    xQueueSend(g_keys, &code, 0);
  }
}

void yield() { g_yield.store(true); }

void poll(uint32_t now) {
  if (!g_ready) {
    return;
  }
  if (g_yield.exchange(false)) {
    g_shown = false;
    if (g_ovlOn) setOverlay(false);
    g_uploaded = false;
    memset(g_lineOk, 0, sizeof(g_lineOk));
  }
  uint8_t code;
  while (xQueueReceive(g_keys, &code, 0) == pdTRUE) {
    onEvent(code);
    g_lastKeyMs = now;
  }
  if (g_shown && now - g_lastKeyMs >= kHideMs) {
    g_shown = false;
  }
  if (!g_shown) {
    if (g_ovlOn) setOverlay(false);
    // Регион оверлеев — общий: пока меню скрыто, его пишут другие (экран плеера в PLAYER.ELF).
    // Показываясь, меню пишет холст целиком.
    g_uploaded = false;
    memset(g_lineOk, 0, sizeof(g_lineOk));
    g_dirty = false;
    return;
  }
  if (g_dirty || now - g_lastRefreshMs >= kRefreshMs) {
    g_lastRefreshMs = now;
    if (!readRegs(&g_regs)) return;
  }
  if (!g_uploaded) {
    // поле панели: всё один раз, дальше — только изменившиеся строки текста
    if (!uploadRows(0, panelH())) return;
    g_uploaded = true;
  }
  char lines[kRows][kCols + 1];
  buildLines(g_regs, lines);
  for (int i = 0; i < panelRows(); ++i) {
    if (!g_lineOk[i] || memcmp(lines[i], g_lines[i], kCols) != 0) {
      memcpy(g_lines[i], lines[i], sizeof(g_lines[i]));
      drawLine(i, g_lines[i]);
      g_lineOk[i] = uploadRows(kBorder + i * 8, 8);
    }
  }
  if (!g_ovlOn) setOverlay(true);
  g_dirty = false;
}

}  // namespace tang_menu
}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
