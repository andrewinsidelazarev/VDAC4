#include "zifi/ota_image.hpp"

#include <string.h>

namespace zifi {
namespace {

constexpr uint8_t kEspImageMagic = 0xE9;
constexpr uint16_t kEsp32S3ChipId = 9;
constexpr uint32_t kApplicationDescriptorMagic = 0xABCD5432;

uint16_t readLe16Local(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) |
         (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t readLe32Local(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

#if defined(ZIFI_BOARD_VDAC4) && ZIFI_BOARD_VDAC4
constexpr size_t kProjectNameOffset = 80;
// До версии 0.1.11 проект сборки для этой платы назывался zifi-tsvdac.
constexpr char kProjectName[] = "zifi-vdac4";
constexpr char kFormerProjectName[] = "zifi-tsvdac";

bool isForThisBoard(const uint8_t* data) {
  const char* name = reinterpret_cast<const char*>(data + kProjectNameOffset);
  return strncmp(name, kProjectName, sizeof(kProjectName)) == 0 ||
         strncmp(name, kFormerProjectName, sizeof(kFormerProjectName)) == 0;
}
#else
bool isForThisBoard(const uint8_t*) { return true; }
#endif

}  // namespace

bool isEsp32S3ApplicationImage(const uint8_t* data, size_t length) {
  return data != nullptr && length >= kOtaImageHeaderSize &&
         data[0] == kEspImageMagic && readLe16Local(data + 12) == kEsp32S3ChipId &&
         readLe32Local(data + 32) == kApplicationDescriptorMagic &&
         isForThisBoard(data);
}

}  // namespace zifi
