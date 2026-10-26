#include "player_ui.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Три отдельно скомпилированных экземпляра имеют независимые кэши и огибающие.
#define DECLARE_COPY(name) namespace zifi { namespace name { \
using player_view::Allocate; using player_view::Release; \
struct Rect { uint16_t x0, y0, x1, y1; }; \
bool begin(Allocate, Release, void*); void end(); \
int render(const zm_ui_model_t&, const uint16_t*, bool, bool, uint32_t, uint16_t*, Rect*, int); \
} }
DECLARE_COPY(pui_reference)
DECLARE_COPY(pui_repeat)
namespace pv = zifi::player_view;
constexpr size_t pixels = pv::kWidth * pv::kHeight;
constexpr size_t coverPixels = pv::kCoverSide * pv::kCoverSide;
constexpr size_t guard = 64;
constexpr uint16_t sentinel = 0xa57c;
static unsigned frameNumber;
static const char* scenario = "init";

[[noreturn]] void fail(const char* message, size_t pixel = 0) {
  std::fprintf(stderr, "FAIL %s frame=%u: %s pixel=(%zu,%zu)\n",
               scenario, frameNumber, message, pixel % pv::kWidth, pixel / pv::kWidth);
  std::exit(1);
}
void check(bool value, const char* message) { if (!value) fail(message); }

struct Guarded {
  std::vector<uint16_t> data;
  size_t count;
  explicit Guarded(size_t n, uint16_t value = 0) : data(n + 2 * guard, sentinel), count(n) {
    std::fill(data.begin() + guard, data.end() - guard, value);
  }
  uint16_t* ptr() { return data.data() + guard; }
  void verify() const {
    for (size_t i = 0; i < guard; ++i)
      check(data[i] == sentinel && data[guard + count + i] == sentinel, "buffer guard");
  }
};
struct Arena {
  Guarded memory{pixels};
  unsigned allocations = 0, releases = 0;
  bool live = false, reject = false;
  static void* allocate(void* ctx, size_t bytes) {
    auto& a = *static_cast<Arena*>(ctx);
    check(!a.live && bytes == pixels * 2, "allocation size/lifetime");
    ++a.allocations;
    if (a.reject) return nullptr;
    a.live = true;
    return a.memory.ptr();
  }
  static void release(void* ctx, void* ptr) {
    auto& a = *static_cast<Arena*>(ctx);
    check(a.live && ptr == a.memory.ptr(), "release ownership");
    a.memory.verify(); a.live = false; ++a.releases;
  }
};

uint32_t randomWord(uint32_t& seed) {
  seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed;
}
zm_ui_model_t model() {
  zm_ui_model_t m{};
  m.size = sizeof(m); m.visible = 1; m.state = ZM_UI_PLAYING;
  m.view = ZM_UI_VIEW_SPECTRUM; m.count = 64; m.duration_ms = 5400000;
  m.volume = 75; m.temperature = 47; m.track = 3; m.tracks = 10;
  std::strcpy(m.title, "Deterministic test track");
  std::strcpy(m.artist, "PUI host test"); std::strcpy(m.album, "Stage two");
  std::strcpy(m.year, "2026"); std::strcpy(m.file, "TEST.FLAC");
  std::strcpy(m.format, "FLAC  24 bit  96 kHz  stereo");
  for (unsigned i = 0; i < ZM_UI_STATIONS; ++i)
    std::snprintf(m.stations[i], sizeof(m.stations[i]), "Station %u", i + 1);
  return m;
}
void fillStrings(zm_ui_model_t& m, unsigned char c) {
  char* fields[] = {m.title, m.artist, m.album, m.file, m.station, m.format, m.detail, m.status};
  for (char* field : fields) std::memset(field, c, ZM_UI_TEXT);
  std::memset(m.year, c, sizeof(m.year));
  std::memset(m.stations, c, sizeof(m.stations));
}

struct Suite {
  Arena a, b, c;
  Guarded incremental{pixels, 0x1234}, reference{pixels, 0x4321}, repeat{pixels, 0x1234};
  Guarded cover{coverPixels};
  std::vector<uint16_t> previous = std::vector<uint16_t>(pixels, 0x1234);
  std::vector<uint16_t> tang = previous;
  std::vector<uint8_t> covered = std::vector<uint8_t>(pixels);
  unsigned fullFrames = 0, partialFrames = 0, emptyFrames = 0;
  uint64_t checkedChanges = 0;
  Suite() {
    check(pv::begin(Arena::allocate, Arena::release, &a), "begin");
    check(zifi::pui_reference::begin(Arena::allocate, Arena::release, &b), "reference begin");
    check(zifi::pui_repeat::begin(Arena::allocate, Arena::release, &c), "repeat begin");
    for (size_t i = 0; i < coverPixels; ++i) cover.ptr()[i] = static_cast<uint16_t>(i * 73);
  }
  ~Suite() {
    pv::end(); zifi::pui_reference::end(); zifi::pui_repeat::end();
    check(a.releases == 1 && b.releases == 1 && c.releases == 1, "release count");
  }
  void step(const zm_ui_model_t& m, uint32_t now, int capacity = 32,
            bool withCover = true, bool coverChanged = false, bool full = false) {
    ++frameNumber;
    const auto beforeModel = m;
    const auto beforeCover = cover.data;
    std::array<pv::Rect, 34> dirty;
    std::array<zifi::pui_repeat::Rect, 34> again;
    std::memset(dirty.data(), 0xa5, sizeof(dirty));
    std::memset(again.data(), 0xa5, sizeof(again));
    const auto untouched = dirty;
    zifi::pui_reference::Rect all[32]{};
    const uint16_t* art = withCover ? cover.ptr() : nullptr;
    const int n = pv::render(m, art, coverChanged, full, now, incremental.ptr(), dirty.data()+1, capacity);
    const int nr = zifi::pui_repeat::render(m, art, coverChanged, full, now, repeat.ptr(), again.data()+1, capacity);
    const int nf = zifi::pui_reference::render(m, art, coverChanged, true, now, reference.ptr(), all, 32);
    check(n >= 0 && n <= capacity && nr == n && nf == 1, "dirty count");
    check(all[0].x0 == 0 && all[0].y0 == 0 && all[0].x1 == pv::kWidth && all[0].y1 == pv::kHeight, "full rect");
    check(std::memcmp(dirty.data(), again.data(), sizeof(dirty)) == 0, "deterministic rectangles");
    check(std::memcmp(&dirty[0], &untouched[0], sizeof(pv::Rect)) == 0, "dirty left guard");
    for (size_t i = static_cast<size_t>(capacity)+1; i < dirty.size(); ++i)
      check(std::memcmp(&dirty[i], &untouched[i], sizeof(pv::Rect)) == 0, "dirty right guard");
    std::fill(covered.begin(), covered.end(), 0);
    for (int r = 1; r <= n; ++r) {
      const auto q = dirty[r];
      check(q.x0 < q.x1 && q.y0 < q.y1 && q.x1 <= pv::kWidth && q.y1 <= pv::kHeight &&
            !(q.x0 & 1) && !(q.x1 & 1), "rectangle bounds/alignment");
      for (unsigned y = q.y0; y < q.y1; ++y) for (unsigned x = q.x0; x < q.x1; ++x) {
        const size_t i = y * pv::kWidth + x;
        covered[i] = 1; tang[i] = incremental.ptr()[i];
      }
    }
    for (size_t i = 0; i < pixels; ++i) {
      if (incremental.ptr()[i] != reference.ptr()[i]) fail("incremental != full", i);
      if (incremental.ptr()[i] != repeat.ptr()[i]) fail("determinism", i);
      if (previous[i] != incremental.ptr()[i]) {
        if (!covered[i]) fail("changed pixel outside dirty union", i);
        ++checkedChanges;
      }
      if (tang[i] != reference.ptr()[i]) fail("Tang copy != full", i);
    }
    previous.assign(incremental.ptr(), incremental.ptr() + pixels);
    incremental.verify(); reference.verify(); repeat.verify(); cover.verify();
    a.memory.verify(); b.memory.verify(); c.memory.verify();
    check(a.allocations == 1 && b.allocations == 1 && c.allocations == 1, "allocation during render");
    check(std::memcmp(&beforeModel, &m, sizeof(m)) == 0 && beforeCover == cover.data, "input changed");
    if (n == 0) ++emptyFrames;
    else if (n == 1 && dirty[1].x0 == 0 && dirty[1].y0 == 0 && dirty[1].x1 == pv::kWidth && dirty[1].y1 == pv::kHeight) ++fullFrames;
    else ++partialFrames;
  }
};

void lifecycle() {
  scenario = "lifecycle";
  Arena a; Guarded canvas{pixels}; pv::Rect rects[32]{}; auto m = model();
  check(pv::render(m, nullptr, false, false, 0, canvas.ptr(), rects, 32) == 0, "render before begin");
  check(!pv::begin(nullptr, Arena::release, &a), "null allocator");
  check(!pv::begin(Arena::allocate, nullptr, &a), "null release");
  a.reject = true; check(!pv::begin(Arena::allocate, Arena::release, &a), "allocation failure");
  a.reject = false; check(pv::begin(Arena::allocate, Arena::release, &a), "begin after failure");
  check(pv::begin(Arena::allocate, Arena::release, &a) && a.releases == 1, "repeated begin");
  const auto saved = canvas.data;
  for (int n : {0, -1}) check(pv::render(m, nullptr, false, false, 100, canvas.ptr(), rects, n) == 0, "no dirty slots");
  check(pv::render(m, nullptr, false, false, 100, nullptr, rects, 32) == 0, "null canvas");
  check(pv::render(m, nullptr, false, false, 100, canvas.ptr(), nullptr, 32) == 0, "null dirty");
  check(canvas.data == saved, "invalid call changed canvas");
  check(pv::render(m, nullptr, false, false, 100, canvas.ptr(), rects, 32) == 1 && rects[0].x0 == 0 && rects[0].x1 == pv::kWidth, "invalid call consumed first frame");
  pv::end(); pv::end(); check(!a.live && a.releases == 2, "end idempotence");
  canvas.verify();
}

void correctness() {
  lifecycle();
  Suite s; auto m = model(); uint32_t seed = 0x6d2b79f5, now = 0xfffff000U;
  const int capacities[] = {32, 12, 8, 3, 2, 1};
  scenario = "continuous spectrum and time";
  for (unsigned f = 0; f < 2048; ++f) {
    for (auto& value : m.level) value = static_cast<uint8_t>(randomWord(seed));
    m.played_ms += 40;
    s.step(m, now += 40, capacities[(f / 37) % 6]);
  }
  scenario = "isolated metadata and cover changes";
  char* fields[] = {m.title, m.artist, m.album, m.year, m.file, m.station, m.format, m.detail, m.status};
  for (char* field : fields) {
    field[0] ^= 1; s.step(m, now += 40, 1);
    field[0] ^= 1; s.step(m, now += 40, 32);
  }
  ++m.track; s.step(m, now += 40);
  ++m.tracks; s.step(m, now += 40);
  for (size_t i=0; i<coverPixels; ++i) s.cover.ptr()[i] ^= 0xffff;
  s.step(m, now += 40, 1, true, true); // только coverChanged, без нового serial
  for (size_t i=0; i<coverPixels; ++i) s.cover.ptr()[i] ^= 0x7e0;
  ++m.cover_serial; s.step(m, now += 40); // только serial, без coverChanged
  s.step(m, now += 40, 32, false); s.step(m, now += 40, 32, true);
  scenario = "mixed sequence";
  for (unsigned f = 0; f < 6000; ++f) {
    for (auto& value : m.level) value = static_cast<uint8_t>(randomWord(seed));
    m.played_ms += 40;
    if (f % 149 == 0) { m.page = static_cast<uint8_t>((f / 149) % 3); }
    if (f % 211 == 0) { m.view = static_cast<uint8_t>((f / 211) % 4); m.count = (f / 211) % 2 ? 64 : 32; }
    if (f % 313 == 0) { m.state = static_cast<uint8_t>((f / 313) % 7); }
    if (f % 73 == 0) { m.volume = static_cast<uint8_t>(randomWord(seed)); }
    if (f % 53 == 0) { m.temperature = static_cast<int8_t>(randomWord(seed)); }
    if (f % 127 == 0) { m.current = static_cast<uint8_t>(randomWord(seed)); m.load_percent = static_cast<uint8_t>(randomWord(seed)); }
    if (f % 277 == 0) {
      std::snprintf(m.title, sizeof(m.title), "New title %u", f);
      std::snprintf(m.status, sizeof(m.status), "Status %u", f);
      m.radio ^= 1; m.duration_ms = m.duration_ms ? 0 : 5400000;
    }
    if (f % 109 == 0) { m.sent_kb = randomWord(seed); m.queue_bytes = randomWord(seed); ++m.faults_decode; ++m.faults_starve; ++m.faults_i2s; }
    const bool coverChanged = f % 197 == 0;
    if (coverChanged) { ++m.cover_serial; for (size_t i=0;i<coverPixels;++i) s.cover.ptr()[i] ^= 0x1f; }
    now += f % 101 == 0 ? 0 : (f % 181 == 0 ? 5000 : 40);
    s.step(m, now, capacities[(f / 37) % 6], (f / 227) % 2 == 0, coverChanged, f % 503 == 0);
  }
  scenario = "all CP866 / unterminated strings";
  for (unsigned code = 0; code < 256; ++code) {
    m = model(); fillStrings(m, static_cast<unsigned char>(code));
    for (unsigned page = 0; page < 3; ++page) {
      m.page = static_cast<uint8_t>(page); s.step(m, now += 40, 1, false);
    }
    m.page = ZM_UI_PAGE_MAIN; m.radio = 1; m.view = ZM_UI_VIEW_STATIONS;
    m.count = 255; m.current = 255; s.step(m, now += 40, 2, false);
  }
  scenario = "count 0..255 / spectrum channels stations";
  for (unsigned count = 0; count < 256; ++count) for (uint8_t view : {ZM_UI_VIEW_SPECTRUM, ZM_UI_VIEW_CHANNELS, ZM_UI_VIEW_STATIONS}) {
    m = model(); m.count = static_cast<uint8_t>(count); m.view = view; m.current = 255;
    std::memset(m.level, 255, sizeof(m.level)); s.step(m, now += 40, 1);
  }
  scenario = "time and temperature boundaries";
  for (uint32_t ms : {0U, 999U, 1000U, 59999U, 60000U, 3599999U, 3600000U, 3765000U, 36000000U, 360000000U, 0xffffffffU}) {
    m = model(); m.played_ms = ms;
    for (uint32_t duration : {0U, 1U, 225000U, 3600000U, 0xffffffffU}) {
      m.duration_ms = duration;
      for (int temp : {-128, -127, -1, 0, 69, 70, 74, 75, 127}) {
        m.temperature = static_cast<int8_t>(temp); s.step(m, now += 40, 32);
      }
    }
  }
  scenario = "raw garbage model";
  for (unsigned f=0; f<512; ++f) {
    auto* bytes = reinterpret_cast<unsigned char*>(&m);
    for (size_t i=0; i<sizeof(m); ++i) bytes[i] = static_cast<unsigned char>(randomWord(seed));
    m.page = static_cast<uint8_t>(f % 3); m.view = static_cast<uint8_t>(f % 4);
    m.state = static_cast<uint8_t>(f % 7); m.count = 255;
    s.step(m, now += 40, capacities[f % 6], f % 2 != 0);
  }
  scenario = "empty / repeated timestamp";
  m = {}; s.step(m, now, 1, false);
  const auto saved = s.incremental.data;
  s.step(m, now, 1, false);
  check(saved == s.incremental.data, "repeat timestamp changed static pixels");
  check(s.partialFrames > 2000 && s.emptyFrames > 0, "insufficient incremental coverage");
  std::printf("PASS frames=%u full=%u partial=%u empty=%u changed_pixels=%llu\n",
              frameNumber, s.fullFrames, s.partialFrames, s.emptyFrames,
              static_cast<unsigned long long>(s.checkedChanges));
  std::printf("PASS determinism/full-reference/dirty-union/Tang/guards/CP866/count/time/garbage/lifecycle\n");
}

void benchmark() {
  scenario = "benchmark";
  Arena a; Guarded canvas{pixels}, art{coverPixels}; pv::Rect dirty[32];
  auto m = model();
  std::array<std::array<uint8_t,64>,256> levels{};
  uint32_t seed = 0x10203040;
  for (auto& frame : levels) for (auto& v : frame) v = static_cast<uint8_t>(randomWord(seed));
  for (int mode=0; mode<3; ++mode) {
    std::vector<double> times;
    const int frames = mode == 0 ? 1000 : 40;
    for (unsigned batch=0; batch<9; ++batch) {
      check(pv::begin(Arena::allocate, Arena::release, &a), "benchmark begin");
      pv::render(m, art.ptr(), true, true, 0, canvas.ptr(), dirty, 32);
      const auto start = std::chrono::steady_clock::now();
      for (int f=0; f<frames; ++f) {
        std::memcpy(m.level, levels[f%256].data(), 64); m.played_ms = static_cast<uint32_t>(f)*40;
        pv::render(m, mode == 2 ? nullptr : art.ptr(), false, mode != 0, static_cast<uint32_t>(f+1)*40, canvas.ptr(), dirty, 32);
      }
      const auto stop = std::chrono::steady_clock::now();
      times.push_back(std::chrono::duration<double,std::micro>(stop-start).count()/frames);
      pv::end();
    }
    std::sort(times.begin(),times.end());
    std::printf("BENCH %s min_us=%.3f median_us=%.3f max_us=%.3f batches=9 frames_per_batch=%d\n",
                mode==0?"normal":mode==1?"full_cover":"full_placeholder",times.front(),times[4],times.back(),frames);
  }
}
int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--benchmark") == 0) benchmark();
  else correctness();
  return 0;
}
