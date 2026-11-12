// Файл тома FAT32 на сырых секторах — см. zifi/fat32_map.hpp
#include "zifi/fat32_map.hpp"

namespace zifi {
namespace {

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) {
  return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24;
}

// Загрузочный сектор FAT32: 512 байт в секторе, кластер 1…128 секторов, корень — кластер,
// FAT16-полей нет (корень 0 записей, FATSz16 0)
bool isFat32Boot(const uint8_t* s) {
  if (s[510] != 0x55 || s[511] != 0xAA || le16(s + 11) != 512) {
    return false;
  }
  const uint8_t spc = s[13];
  return spc != 0 && (spc & (spc - 1)) == 0 && le16(s + 14) != 0 && s[16] != 0 && le16(s + 17) == 0 &&
         le16(s + 22) == 0 && le32(s + 36) != 0;
}

}  // namespace

const char* Fat32Map::mount(ReadFn read, void* ctx) {
  read_ = read;
  ctx_ = ctx;
  mounted_ = false;
  cachedLba_ = UINT32_MAX;
  uint8_t s[512];
  if (!read_(ctx_, 0, s)) {
    return "the card does not read (sector 0)";
  }
  uint32_t base = 0;
  if (!isFat32Boot(s)) {
    // таблица разделов MBR: первый раздел FAT32 (0x0B CHS, 0x0C LBA)
    if (s[510] != 0x55 || s[511] != 0xAA) {
      return "no FAT32 volume on the card (no boot sector)";
    }
    for (int i = 0; i < 4 && base == 0; ++i) {
      const uint8_t* e = s + 446 + 16 * i;
      if (e[4] == 0x0B || e[4] == 0x0C) {
        base = le32(e + 8);
      }
    }
    if (base == 0) {
      return "no FAT32 partition on the card";
    }
    if (!read_(ctx_, base, s)) {
      return "the card does not read (volume boot sector)";
    }
    if (!isFat32Boot(s)) {
      return "the FAT32 partition has no FAT32 boot sector";
    }
  }
  const uint32_t reserved = le16(s + 14);
  const uint32_t fats = s[16];
  const uint32_t fatSize = le32(s + 36);
  const uint32_t total = le16(s + 19) != 0 ? le16(s + 19) : le32(s + 32);
  sectorsPerCluster_ = s[13];
  fatStart_ = base + reserved;
  dataStart_ = fatStart_ + fats * fatSize;
  const uint32_t meta = reserved + fats * fatSize;
  if (total <= meta) {
    return "the FAT32 boot sector is broken (sizes)";
  }
  clusters_ = (total - meta) / sectorsPerCluster_;
  if (clusters_ < 65525) {
    return "the volume is FAT12/16, not FAT32";
  }
  if (clusters_ + 2 > fatSize * 128) {
    clusters_ = fatSize * 128 - 2;    // FAT короче тома: кластеры за ней не адресуются
  }
  mounted_ = true;
  return nullptr;
}

bool Fat32Map::fatEntry(uint32_t cluster, uint32_t* next) {
  const uint32_t lba = fatStart_ + cluster / 128;
  if (lba != cachedLba_) {
    if (!read_(ctx_, lba, cache_)) {
      cachedLba_ = UINT32_MAX;
      return false;
    }
    cachedLba_ = lba;
  }
  *next = le32(cache_ + 4 * (cluster % 128)) & 0x0FFFFFFF;
  return true;
}

const char* Fat32Map::chain(uint32_t cluster, uint64_t size, std::vector<Run>& runs) {
  runs.clear();
  if (!mounted_) {
    return "the card volume is not read";
  }
  if (size == 0) {
    return nullptr;
  }
  uint64_t sectors = (size + 511) / 512;
  const uint64_t limit = (sectors + sectorsPerCluster_ - 1) / sectorsPerCluster_;
  uint64_t taken = 0;
  while (sectors != 0) {
    if (cluster < 2 || cluster > clusters_ + 1) {
      return cluster >= 0x0FFFFFF8 ? "the file chain is shorter than its size" : "bad cluster in the file chain";
    }
    if (++taken > limit) {
      return "the file chain is longer than its size";   // петля в FAT
    }
    const uint32_t lba = dataStart_ + (cluster - 2) * sectorsPerCluster_;
    const uint32_t n = sectors < sectorsPerCluster_ ? static_cast<uint32_t>(sectors) : sectorsPerCluster_;
    if (!runs.empty() && runs.back().lba + runs.back().sectors == lba) {
      runs.back().sectors += n;
    } else {
      runs.push_back(Run{lba, n});
    }
    sectors -= n;
    if (sectors == 0) {
      break;
    }
    uint32_t next = 0;
    if (!fatEntry(cluster, &next)) {
      return "the card does not read (FAT)";
    }
    cluster = next;
  }
  return nullptr;
}

bool Fat32Map::locate(const std::vector<Run>& runs, uint64_t offset, uint32_t* lba, uint32_t* left) {
  uint64_t sector = offset / 512;
  for (const Run& r : runs) {
    if (sector < r.sectors) {
      *lba = r.lba + static_cast<uint32_t>(sector);
      *left = r.sectors - static_cast<uint32_t>(sector);
      return true;
    }
    sector -= r.sectors;
  }
  return false;
}

}  // namespace zifi
