// mfm_check — журнал записей в OPL4 проигрывателя MFM (modules/opl/main/mfm.cpp)
// в формате эталона tests/mfm_reference.py: строка init, строка на тик
// («Frrr=vv» — FM, «Wrr=vv» — волны), в конце «END n» (песня без повтора
// остановилась), «LOOP n» (ушла на повтор) или «LIMIT n».
//
// mfm_check SONG.MFM [тиков]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "mfm.h"

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
  if (argc < 2) {
    fprintf(stderr, "usage: mfm_check SONG.MFM [ticks]\n");
    return 2;
  }
  std::vector<unsigned char> file = read_file(argv[1]);
  mfm::Song song;
  if (!mfm::parse(file.data(), (uint32_t)file.size(), &song)) {
    fprintf(stderr, "not an MFM: %s\n", argv[1]);
    return 1;
  }
  const long limit = argc > 2 ? atol(argv[2]) : 60L * 60 * 15;
  mfm::Output out = {nullptr, wave, fm, nullptr};
  static mfm::Player player;
  line.clear();
  player.start(&song, &out, true);
  printf("%s\n", line.c_str());
  int last = -1;
  for (long tick = 0; tick < limit; ++tick) {
    line.clear();
    const bool more = player.tick();
    printf("%s\n", line.c_str());
    if (!more) {
      printf("END %ld\n", tick);
      return 0;
    }
    const int now = player.position();
    if (last >= 0 && last != 0xFF && now < last) {
      printf("LOOP %ld\n", tick);
      return 0;
    }
    last = now;
  }
  printf("LIMIT %ld\n", limit);
  return 0;
}
