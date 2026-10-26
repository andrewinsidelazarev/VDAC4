#include "zifi/tang_overlay.hpp"

#if ZIFI_HAS_SPI_LINK

#include "zifi/tang_link.hpp"

namespace zifi {
namespace tang_overlay {

namespace {
constexpr uint32_t kMinCore = 0x010A;
constexpr size_t kChunkWords = 16;  // больше tang::write за раз не берёт
}  // namespace

// Оверлеи есть только у ядра TS-Config 1.10+: у ядер из кэша (ZX Next, личность 3+) по этим
// адресам другие регистры, сравнивать их номер версии с 1.10 нельзя.
bool supported() {
  const uint32_t version = tang::coreVersion();
  return tang::ready() && (version >> 24) == tang::kPersonalityTs && (version & 0xFFFF) >= kMinCore;
}

bool configure(int k, const Config& c) {
  if (k < 0 || k > 1 || !supported()) {
    return false;
  }
  // Сначала положение, размер и начало, затем включение: Tang берёт всё в начале кадра
  const uint32_t words[4] = {
      (c.on ? 1U : 0U) | (c.x2 ? 2U : 0U) | (static_cast<uint32_t>(c.alpha & 15) << 4),
      (c.x & 0x7FFU) | (static_cast<uint32_t>(c.y & 0x3FFU) << 16),
      (c.w & 0x7FFU) | (static_cast<uint32_t>(c.h & 0x3FFU) << 16),
      c.base & 0x1FFF8U,
  };
  const uint32_t reg = kRegCtrl + 16U * static_cast<uint32_t>(k);
  return tang::write(reg + 4, words + 1, 3) && tang::write(reg, words, 1);
}

bool hide(int k) {
  if (k < 0 || k > 1 || !supported()) {
    return false;
  }
  const uint32_t off = 0;
  return tang::write(kRegCtrl + 16U * static_cast<uint32_t>(k), &off, 1);
}

bool writeRows(uint32_t base, uint32_t stride, uint32_t row0, uint32_t rows,
               const uint16_t* pixels) {
  if (!supported() || stride == 0 || base + (row0 + rows) * stride > kRegionWords) {
    return false;
  }
  uint32_t words[kChunkWords];
  for (uint32_t r = 0; r < rows; ++r) {
    const uint16_t* row = pixels + static_cast<size_t>(r) * stride * 2;
    const uint32_t first = base + (row0 + r) * stride;
    for (uint32_t w = 0; w < stride; w += kChunkWords) {
      const size_t n = stride - w < kChunkWords ? stride - w : kChunkWords;
      for (size_t i = 0; i < n; ++i) {
        words[i] = row[2 * (w + i)] | (static_cast<uint32_t>(row[2 * (w + i) + 1]) << 16);
      }
      if (!tang::write(kMemBase + 4U * (first + w), words, n)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace tang_overlay
}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
