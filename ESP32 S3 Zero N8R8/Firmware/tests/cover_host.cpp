// Стенд обложки MP3 на ПК (tests/test_cover_host.py): настоящие CoverParser и
// CoverThumb (src/cover_art.cpp), Z80 и декодер JPEG — модели.
//
// cover_host <файл> <кусок> <режим>
//   Файл — начало MP3 с меткой ID3v2. Z80 шлёт куски по <кусок> байтов с того
//   места, которое просит ESP (режим seq); в режиме overlap — начиная на 7
//   байтов раньше; в режиме late сначала присылает кусок дальше нужного места.
//   Картинка «JPEG» стенда: FF D8 FF 'FAKE' [ширина LE16][высота LE16][RGB888],
//   модель декодера отдаёт её блоками 16×16, как TJpgDec отдаёт MCU.
// Вывод — строки «ключ значение».
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "zifi/cover_art.hpp"

namespace zifi {

int decodeCoverJpeg(const uint8_t* jpeg, size_t size, CoverThumb& thumb,
                    CoverDecodeInfo& info, const volatile bool* cancel, void (*)()) {
  (void)cancel;
  if (size < 11 || memcmp(jpeg + 3, "FAKE", 4) != 0) {
    return 108;                        // как JDR_FMT3 у jd_prepare
  }
  const uint32_t width = jpeg[7] | (jpeg[8] << 8);
  const uint32_t height = jpeg[9] | (jpeg[10] << 8);
  if (size < 11 + static_cast<size_t>(width) * height * 3) {
    return 102;                        // JDR_INP
  }
  info.width = width;
  info.height = height;
  info.scale = 0;
  if (!thumb.begin(width, height)) {
    return 2;
  }
  std::vector<uint8_t> block(16 * 16 * 3);
  for (uint32_t top = 0; top < height; top += 16) {
    for (uint32_t left = 0; left < width; left += 16) {
      const uint32_t right = left + 15 < width ? left + 15 : width - 1;
      const uint32_t bottom = top + 15 < height ? top + 15 : height - 1;
      const uint32_t blockWidth = right - left + 1;
      for (uint32_t y = top; y <= bottom; ++y) {
        memcpy(&block[(y - top) * blockWidth * 3], jpeg + 11 + (y * width + left) * 3,
               blockWidth * 3);
      }
      thumb.addBlock(left, top, right, bottom, block.data());
    }
  }
  return 0;
}

}  // namespace zifi

namespace {

void* hostAlloc(size_t size) { return malloc(size); }
void hostFree(void* pointer) { free(pointer); }

uint32_t fnv1a(const uint8_t* data, size_t size) {
  uint32_t hash = 2166136261u;
  for (size_t index = 0; index < size; ++index) {
    hash = (hash ^ data[index]) * 16777619u;
  }
  return hash;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: cover_host <file> <chunk> <seq|overlap|late>\n");
    return 2;
  }
  FILE* input = fopen(argv[1], "rb");
  if (input == nullptr) {
    return 2;
  }
  std::vector<uint8_t> file;
  uint8_t buffer[65536];
  size_t got = 0;
  while ((got = fread(buffer, 1, sizeof(buffer), input)) != 0) {
    file.insert(file.end(), buffer, buffer + got);
  }
  fclose(input);
  const size_t chunk = static_cast<size_t>(atoi(argv[2]));
  const char* mode = argv[3];

  zifi::CoverParser parser(hostAlloc, hostFree);
  zifi::CoverParser::Result result = zifi::CoverParser::kWant;
  int feeds = 0;
  size_t fed = 0;
  bool lateDone = false;
  std::vector<uint32_t> wants;
  while (result == zifi::CoverParser::kWant && feeds < 100000) {
    const uint32_t want = parser.want();
    if (wants.size() < 64 && (wants.empty() || wants.back() != want)) {
      wants.push_back(want);
    }
    uint32_t offset = want;
    if (strcmp(mode, "overlap") == 0) {
      offset = want >= 7 ? want - 7 : 0;
    } else if (strcmp(mode, "late") == 0 && !lateDone) {
      offset = want + 5;
      lateDone = true;
    } else {
      lateDone = false;
    }
    size_t length = 0;
    if (offset < file.size()) {
      length = file.size() - offset < chunk ? file.size() - offset : chunk;
    }
    result = parser.feed(offset, length != 0 ? &file[offset] : nullptr, length);
    ++feeds;
    fed += length;
    if (strcmp(mode, "late") == 0 && offset == want + 5 && parser.want() != want) {
      printf("late_moved %u\n", static_cast<unsigned>(parser.want()));
    }
  }
  printf("result %u\n", static_cast<unsigned>(result));
  printf("reason %s\n", parser.reason()[0] != 0 ? parser.reason() : "-");
  printf("png %u\n", parser.isPng() ? 1u : 0u);
  printf("feeds %d\n", feeds);
  printf("fed %u\n", static_cast<unsigned>(fed));
  printf("wants");
  for (size_t index = 0; index < wants.size(); ++index) {
    printf("%c%u", index == 0 ? ' ' : ',', static_cast<unsigned>(wants[index]));
  }
  printf("\n");
  // Метки — шестнадцатерично: в них CP866.
  const zifi::CoverTags& tags = parser.tags();
  const char* names[] = {"title", "artist", "album", "year", "track"};
  const char* fields[] = {tags.title, tags.artist, tags.album, tags.year, tags.track};
  for (int index = 0; index < 5; ++index) {
    printf("%s ", names[index]);
    for (const char* c = fields[index]; *c != 0; ++c) {
      printf("%02x", static_cast<unsigned char>(*c));
    }
    printf("\n");
  }
  if (result != zifi::CoverParser::kReceived) {
    return 0;
  }
  size_t size = 0;
  uint8_t* image = parser.takeImage(size);
  printf("image %u %08x\n", static_cast<unsigned>(size), fnv1a(image, size));
  zifi::CoverThumb thumb(hostAlloc, hostFree);
  zifi::CoverDecodeInfo info;
  const int code = zifi::decodeCoverJpeg(image, size, thumb, info, nullptr);
  printf("decode %d\n", code);
  free(image);
  if (code != 0) {
    return 0;
  }
  uint8_t output[zifi::CoverThumb::kBytes];
  thumb.finish(output);
  printf("thumb ");
  for (size_t index = 0; index < sizeof(output); ++index) {
    printf("%02x", output[index]);
  }
  printf("\n");
  return 0;
}
