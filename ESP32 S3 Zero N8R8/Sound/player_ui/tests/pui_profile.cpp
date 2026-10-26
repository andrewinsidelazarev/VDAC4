#include "player_ui.h"
#include "tests/pui_profile.h"
#include <cstdlib>
#include <cstring>

namespace pv = zifi::player_view;
void* allocate(void*, size_t n) { return std::malloc(n); }
void release(void*, void* p) { std::free(p); }
uint16_t canvas[pv::kWidth * pv::kHeight], art[pv::kCoverSide * pv::kCoverSide];
void print(const char* mode, unsigned frame) {
  const auto& c = pui_profile::counts;
  std::printf("%s,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", mode, frame,
    (unsigned long long)c.put, (unsigned long long)c.stores, (unsigned long long)c.dither,
    (unsigned long long)c.background, (unsigned long long)c.placeholder, (unsigned long long)c.round,
    (unsigned long long)c.restore, (unsigned long long)c.glyph, (unsigned long long)c.alpha,
    (unsigned long long)c.hash, (unsigned long long)c.copied, (unsigned long long)c.formats);
}
int main() {
  zm_ui_model_t m{}; m.size = sizeof(m); m.state = ZM_UI_PLAYING;
  m.view = ZM_UI_VIEW_SPECTRUM; m.count = 64; m.volume = 75; m.temperature = 47;
  m.track = 3; m.tracks = 10; m.duration_ms = 225000;
  std::strcpy(m.title, "Deterministic test track"); std::strcpy(m.artist, "PUI host test");
  std::strcpy(m.album, "Stage two"); std::strcpy(m.year, "2026");
  std::strcpy(m.format, "FLAC  24 bit  96 kHz  stereo");
  uint32_t random = 0x10203040;
  pv::Rect dirty[32];
  std::puts("mode,frame,put,stores,dither,background,placeholder,round,restore,glyph,alpha,hash,copied,formats");
  for (unsigned mode=0; mode<4; ++mode) {
    if (!pv::begin(allocate, release, nullptr)) return 1;
    pv::render(m, art, true, true, 0, canvas, dirty, 32);
    const unsigned frames = mode < 2 ? 1000 : 1;
    for (unsigned f=0; f<frames; ++f) {
      for (auto& level : m.level) {
        random ^= random<<13; random ^= random>>17; random ^= random<<5;
        // Стресс чередует тишину и максимум каждые 400 мс.
        level = mode == 1 ? ((f / 10) % 2 ? 255 : 0) : static_cast<uint8_t>(random);
      }
      m.played_ms = 83000 + f * 40;
      pui_profile::counts = {};
      pv::render(m, mode == 3 ? nullptr : art, false, mode >= 2, (f+1)*40, canvas, dirty, 32);
      print(mode==0?"normal":mode==1?"stress":mode==2?"full_cover":"full_placeholder", f);
    }
    pv::end();
  }
}
