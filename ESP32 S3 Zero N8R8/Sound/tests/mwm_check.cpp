// mwm_check — журнал записей в OPL4 проигрывателя MWM (modules/opl/main/mwm.cpp)
// в формате эталона tests/mwm_reference.py: строка init, строка на тик
// («Wrr=vv» — волны, «Frrr=vv» — FM), в конце «END n» или «LIMIT n».
// Без повтора: в конце позиций — стоп, как у порта ZX.
//
// mwm_check SONG.MWM KIT.MWK|- [тиков]
// mwm_check --image KIT.MWK|- OUT.BIN — образ ОЗУ OPL4 из набора (mwm::load_kit).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "mwm.h"

namespace {

std::vector<unsigned char> read_file(const char* path) {
  std::vector<unsigned char> data;
  FILE* f = fopen(path, "rb");
  if (f == nullptr) {
    fprintf(stderr, "cannot open %s\n", path);
    exit(2);
  }
  unsigned char buffer[65536];
  size_t n;
  while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) {
    data.insert(data.end(), buffer, buffer + n);
  }
  fclose(f);
  return data;
}

std::string line;

void wave(void*, uint8_t reg, uint8_t value) {
  char text[16];
  snprintf(text, sizeof(text), "%sW%02X=%02X", line.empty() ? "" : " ", reg, value);
  line += text;
}

void fm(void*, uint16_t reg, uint8_t value) {
  char text[16];
  snprintf(text, sizeof(text), "%sF%03X=%02X", line.empty() ? "" : " ", reg, value);
  line += text;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: mwm_check SONG.MWM KIT.MWK|- [ticks]\n");
    return 2;
  }
  if (strcmp(argv[1], "--image") == 0 && argc > 3) {
    std::vector<unsigned char> k;
    if (strcmp(argv[2], "-") != 0) {
      k = read_file(argv[2]);
    }
    const unsigned char* data = k.empty() ? nullptr : k.data();
    const uint32_t size = mwm::load_kit(data, (uint32_t)k.size(), nullptr, nullptr, 0);
    if (size == 0) {
      fprintf(stderr, "bad kit\n");
      return 1;
    }
    static mwm::Kit kit;
    std::vector<unsigned char> image(size);
    mwm::load_kit(data, (uint32_t)k.size(), &kit, image.data(), size);
    FILE* f = fopen(argv[3], "wb");
    fwrite(image.data(), 1, image.size(), f);
    fclose(f);
    return 0;
  }
  std::vector<unsigned char> song_file = read_file(argv[1]);
  mwm::Song song;
  if (!mwm::parse(song_file.data(), (uint32_t)song_file.size(), &song)) {
    fprintf(stderr, "not an MWM: %s\n", argv[1]);
    return 1;
  }
  static mwm::Kit kit;
  memset(&kit, 0, sizeof(kit));
  if (strcmp(argv[2], "-") != 0) {
    std::vector<unsigned char> k = read_file(argv[2]);
    if (k.size() < 74 || memcmp(k.data(), "MBMS\x10\x0d", 6) != 0) {
      fprintf(stderr, "not an MWK: %s\n", argv[2]);
      return 1;
    }
    const size_t count = k[9];
    memcpy(kit.tones, k.data() + 10, 64);
    const size_t waves = 25 * count < sizeof(kit.waves) ? 25 * count : sizeof(kit.waves);
    memcpy(kit.waves, k.data() + 74, waves);
  }
  const long limit = argc > 3 ? atol(argv[3]) : 60L * 60 * 15;
  mwm::Output out = {nullptr, wave, fm, nullptr};
  static mwm::Player player;
  line.clear();
  player.start(&song, &kit, &out, false);
  printf("%s\n", line.c_str());
  long tick = 0;
  bool ended = false;
  for (; tick < limit; ++tick) {
    line.clear();
    const bool more = player.tick();
    printf("%s\n", line.c_str());
    if (!more) {
      ended = true;
      break;
    }
  }
  printf(ended ? "END %ld\n" : "LIMIT %ld\n", ended ? tick : limit);
  return 0;
}
