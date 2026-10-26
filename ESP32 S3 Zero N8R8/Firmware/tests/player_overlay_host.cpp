// Стенд экрана проигрывателя (src/player_overlay.cpp, src/player_view.cpp) на ПК для
// test_player_overlay_host.py: Port — модель Tang (регион оверлеев 131 072 слова, регистры
// оверлея), приложения (снимок, обложка), меню и памяти. Сценарии идут подряд, на выходе —
// строки ключ=значение.
//
// Главная проверка — то, что видит Tang: после каждого кадра регион с kBase совпадает с
// холстом ESP точка в точку (передаются только изменившиеся прямоугольники), а оверлей
// включается не раньше, чем весь холст уже в Tang.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "zifi/player_overlay.hpp"

using zifi::PlayerOverlay;

namespace {

void put(const char* key, long long value) { printf("%s=%lld\n", key, value); }

class FakePort : public PlayerOverlay::Port {
 public:
  std::vector<uint32_t> region = std::vector<uint32_t>(PlayerOverlay::kRegionWords, 0xDEADBEEF);
  bool supported = true;
  uint32_t epoch = 1;
  bool on = false;
  uint8_t alpha = 0;
  std::vector<int> alphas;            // каждая настройка: прозрачность или −1 (выкл.)
  int configures = 0;
  int yields = 0;
  int modelResult = ZM_OK;
  zm_ui_model_t app = {};
  int coverCalls = 0;
  bool failAlloc = false;
  bool failWrite = false;
  long long live = 0;                 // байтов выдано и не возвращено
  long long words = 0;                // слов записано всего
  long long writesOutside = 0;        // запись мимо холста экрана
  bool canvasBeforeOn = true;         // включение — после записи всего холста
  bool wroteSinceOff = false;
  uint64_t clock = 0;

  bool overlaySupported() override { return supported; }
  uint32_t tangEpoch() override { return epoch; }
  bool configure(bool enable, uint8_t a) override {
    ++configures;
    if (enable && !on && !wroteSinceOff) canvasBeforeOn = false;
    on = enable;
    alpha = a;
    alphas.push_back(enable ? a : -1);
    if (!enable) wroteSinceOff = false;
    return true;
  }
  bool write(uint32_t word, const uint32_t* w, size_t count) override {
    if (failWrite) return false;
    if (count == 0 || count > 16) {
      ++writesOutside;
      return false;
    }
    if (word < PlayerOverlay::kBase || word + count > PlayerOverlay::kRegionWords) {
      ++writesOutside;
      return false;
    }
    memcpy(&region[word], w, count * sizeof(uint32_t));
    words += static_cast<long long>(count);
    wroteSinceOff = true;
    return true;
  }
  int model(zm_ui_model_t* m) override {
    if (modelResult != ZM_OK) return modelResult;
    const uint16_t size = m->size;
    memcpy(m, &app, sizeof(app));
    m->size = size;
    return ZM_OK;
  }
  int cover(uint16_t* pixels, uint16_t side) override {
    ++coverCalls;
    if (app.cover_serial == 0) return ZM_NOT_FOUND;
    for (int y = 0; y < side; ++y)
      for (int x = 0; x < side; ++x)
        pixels[y * side + x] = static_cast<uint16_t>((x * 31 / side) << 11 | (y * 63 / side) << 5 |
                                                     (app.cover_serial & 31));
    return ZM_OK;
  }
  void menuYield() override { ++yields; }
  void* allocate(size_t bytes) override {
    if (failAlloc) return nullptr;
    size_t* block = static_cast<size_t*>(malloc(bytes + sizeof(size_t) * 2));
    block[0] = bytes;
    live += static_cast<long long>(bytes);
    return block + 2;
  }
  void release(void* pointer) override {
    size_t* block = static_cast<size_t*>(pointer) - 2;
    live -= static_cast<long long>(block[0]);
    free(block);
  }
  uint64_t micros() override { return clock += 7; }
};

// Холст в Tang совпадает с холстом ESP?
long long mismatches(const FakePort& port, const PlayerOverlay& overlay) {
  const uint16_t* c = overlay.canvas();
  if (c == nullptr) return -1;
  long long bad = 0;
  for (uint32_t y = 0; y < PlayerOverlay::kHeight; ++y) {
    for (uint32_t x = 0; x < PlayerOverlay::kWidth; x += 2) {
      const uint32_t word = port.region[PlayerOverlay::kBase + y * PlayerOverlay::kStride + x / 2];
      const uint32_t want = c[y * PlayerOverlay::kWidth + x] |
                            (static_cast<uint32_t>(c[y * PlayerOverlay::kWidth + x + 1]) << 16);
      if (word != want) ++bad;
    }
  }
  return bad;
}

void mp3(zm_ui_model_t& m) {
  memset(&m, 0, sizeof(m));
  m.size = sizeof(m);
  m.visible = 1;
  m.state = ZM_UI_PLAYING;
  strcpy(m.title, "Children");
  strcpy(m.artist, "Robert Miles");
  strcpy(m.file, "CHILDREN.MP3");
  strcpy(m.format, "MP3  320 kbps  44.1 kHz  stereo");
  m.track = 3;
  m.tracks = 10;
  m.duration_ms = 225000;
  m.volume = 75;
  m.temperature = 47;
  m.view = ZM_UI_VIEW_SPECTRUM;
  m.count = ZM_UI_LEVELS;
  m.cover_serial = 1;
}

}  // namespace

int main() {
  FakePort port;
  PlayerOverlay overlay(port);
  uint32_t now = 1000;
  auto tick = [&](int n) {
    for (int i = 0; i < n; ++i) {
      overlay.tick(now);
      now += PlayerOverlay::kTickMs;
    }
  };

  // 1. Окна плагина нет — экрана нет: ни записи, ни настройки
  mp3(port.app);
  port.app.visible = 0;
  tick(50);
  put("hidden_configures", port.configures);
  put("hidden_words", port.words);
  put("hidden_state", static_cast<int>(overlay.state()));

  // 2. Окно открыто — холст целиком в Tang, затем прозрачность 0…15 по ступени за кадр
  port.app.visible = 1;
  tick(5);
  put("show_state", static_cast<int>(overlay.state()));
  put("show_canvas_before_on", port.canvasBeforeOn ? 1 : 0);
  put("show_words", port.words);
  put("show_yields", port.yields);
  put("show_cover_calls", port.coverCalls);
  put("show_mismatch", mismatches(port, overlay));
  tick(16);
  put("faded_state", static_cast<int>(overlay.state()));
  put("faded_alpha", port.alpha);
  {
    bool rising = true;
    for (size_t i = 1; i < port.alphas.size(); ++i) {
      if (port.alphas[i] != port.alphas[i - 1] + 1) rising = false;
    }
    put("fade_steps", static_cast<long long>(port.alphas.size()));
    put("fade_rising", rising ? 1 : 0);
    put("fade_first", port.alphas.empty() ? -2 : port.alphas.front());
  }

  // 3. Кадры: спектр и время меняются — в Tang ровно холст ESP, передаётся немного
  long long worstMismatch = 0;
  long long maxWords = 0;
  long long frameWords = 0;
  int frames = 0;
  for (int f = 0; f < 200; ++f) {
    // Как у звука: полосы меняются плавно (огибающая бежит по частотам), а не случайно
    for (int i = 0; i < ZM_UI_LEVELS; ++i) {
      const double v = 140.0 + 90.0 * sin(i * 0.25 + f * 0.15) * cos(f * 0.05);
      port.app.level[i] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
    port.app.played_ms = 83000 + static_cast<uint32_t>(f) * 40;
    const long long before = port.words;
    tick(2);
    const long long sent = port.words - before;
    frameWords += sent;
    if (sent > maxWords) maxWords = sent;
    ++frames;
    const long long bad = mismatches(port, overlay);
    if (bad > worstMismatch) worstMismatch = bad;
  }
  put("frames_mismatch", worstMismatch);
  put("frames_max_words", maxWords);
  put("frames_mean_words", frameWords / frames);
  put("frames_counted", overlay.stats().frames);
  // Худший случай для справки: все полосы скачут случайно каждый кадр
  long long worstWords = 0;
  for (int f = 0; f < 50; ++f) {
    for (int i = 0; i < ZM_UI_LEVELS; ++i) {
      port.app.level[i] = static_cast<uint8_t>((i * 37 + f * 101) % 256);
    }
    const long long before = port.words;
    tick(2);
    if (port.words - before > worstWords) worstWords = port.words - before;
  }
  put("random_max_words", worstWords);
  put("random_mismatch", mismatches(port, overlay));

  // Новое название, обложка, окно поверх и обратно — тоже точка в точку
  strcpy(port.app.title, "Another Song");
  port.app.cover_serial = 2;
  tick(2);
  put("title_mismatch", mismatches(port, overlay));
  put("title_cover_calls", port.coverCalls);
  port.app.page = ZM_UI_PAGE_HELP;
  tick(2);
  put("help_mismatch", mismatches(port, overlay));
  port.app.page = ZM_UI_PAGE_MAIN;
  tick(2);
  put("main_mismatch", mismatches(port, overlay));
  // Радио: список станций вместо столбиков
  port.app.radio = 1;
  port.app.view = ZM_UI_VIEW_STATIONS;
  port.app.count = 5;
  port.app.current = 2;
  for (int i = 0; i < 5; ++i) snprintf(port.app.stations[i], ZM_UI_STATION_TEXT, "Station %d", i + 1);
  tick(2);
  put("stations_mismatch", mismatches(port, overlay));
  port.app.current = 3;
  tick(2);
  put("stations_next_mismatch", mismatches(port, overlay));
  // Строки без нуля от приложения (порча): обрезаются, стенд не падает
  memset(port.app.title, 'A', sizeof(port.app.title));
  port.app.count = 250;
  tick(2);
  put("garbage_state", static_cast<int>(overlay.state()));
  put("garbage_mismatch", mismatches(port, overlay));
  mp3(port.app);

  // 4. Окно закрыто — прозрачность вниз до 0, оверлей выкл., память возвращена
  port.alphas.clear();
  port.app.visible = 0;
  tick(20);
  put("closed_state", static_cast<int>(overlay.state()));
  put("closed_on", port.on ? 1 : 0);
  put("closed_live", port.live);
  {
    bool falling = true;
    for (size_t i = 1; i + 1 < port.alphas.size(); ++i) {
      if (port.alphas[i] != port.alphas[i - 1] - 1) falling = false;
    }
    put("close_steps", static_cast<long long>(port.alphas.size()));
    put("close_falling", falling ? 1 : 0);
    put("close_last", port.alphas.empty() ? -2 : port.alphas.back());
  }

  // 5. Приложение пропало (снимков нет) дольше секунды — экран уходит сам
  port.app.visible = 1;
  tick(25);
  put("again_state", static_cast<int>(overlay.state()));
  port.modelResult = ZM_BUSY;
  tick(40);
  put("lost_mid_state", static_cast<int>(overlay.state()));
  tick(40);
  put("lost_state", static_cast<int>(overlay.state()));
  put("lost_on", port.on ? 1 : 0);
  port.modelResult = ZM_OK;

  // 6. Перезагрузка ПЛИС посреди показа: холст в Tang пропал — заново целиком
  tick(25);
  put("before_reload_state", static_cast<int>(overlay.state()));
  std::fill(port.region.begin(), port.region.end(), 0);
  port.on = false;
  port.wroteSinceOff = false;
  port.epoch = 2;
  const long long wordsBefore = port.words;
  tick(25);
  put("reload_state", static_cast<int>(overlay.state()));
  put("reload_full_words", port.words - wordsBefore);
  put("reload_mismatch", mismatches(port, overlay));
  put("reload_canvas_before_on", port.canvasBeforeOn ? 1 : 0);

  // 7. Ядро без оверлеев (FT812 или старое) — экрана нет, память возвращена
  port.supported = false;
  tick(5);
  put("unsupported_state", static_cast<int>(overlay.state()));
  put("unsupported_live", port.live);
  port.supported = true;
  tick(25);

  // 8. Память нужна приложению: экран сразу прочь, 5 с не просится
  overlay.releaseMemory(now);
  put("released_state", static_cast<int>(overlay.state()));
  put("released_live", port.live);
  put("released_on", port.on ? 1 : 0);
  tick(200);                                   // 4 с
  put("released_hold_state", static_cast<int>(overlay.state()));
  tick(100);                                   // ещё 2 с
  put("released_back_state", static_cast<int>(overlay.state()));

  // 9. Памяти нет — экрана нет, попытка не чаще раза в секунду
  overlay.appGone();
  port.failAlloc = true;
  const long long configuresBefore = port.configures;
  tick(100);
  put("nomem_state", static_cast<int>(overlay.state()));
  put("nomem_configures", port.configures - configuresBefore);
  put("nomem_attempts", overlay.stats().noMemory);
  port.failAlloc = false;
  tick(60);

  // 10. Запись в Tang отказала посреди кадра — экран выключается
  put("before_fail_state", static_cast<int>(overlay.state()));
  port.failWrite = true;
  port.app.played_ms += 5000;
  tick(4);
  put("write_fail_state", static_cast<int>(overlay.state()));
  put("write_fail_live", port.live);
  port.failWrite = false;

  put("writes_outside", port.writesOutside);
  overlay.appGone();
  put("end_live", port.live);
  return 0;
}
