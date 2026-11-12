// Видео с SD2 WC — см. zifi/sd2_files.hpp
#include "zifi/sd2_files.hpp"

#include <string.h>

#include <new>
#include <vector>

#include "zifi/bringup_log.hpp"
#include "zifi/fat32_map.hpp"
#include "zifi/vdisk.hpp"
#include "zifi/vdisk_service.hpp"
#if ZIFI_HAS_SPI_LINK
#include "zifi/tang_sd.hpp"
#endif

namespace zifi {
namespace sd2_files {
namespace {

constexpr const char* kTag = "sd2";
// Блоков за операцию STREAM моста: 128 (64 КБ, ~30 мс на 18,6 МГц) — карту операция держит
// (tang_sd::Session), а чтения WC с SD2 ждут её не дольше kQuickMs (300 мс)
constexpr uint32_t kMaxBlocks = 128;

#if ZIFI_HAS_SPI_LINK
bool rawSector(void*, uint32_t lba, uint8_t* out) { return tang_sd::rawRead(lba, out); }
#endif

}  // namespace

struct File {
  bool raw = false;
  uint32_t cluster = 0;
  uint32_t size = 0;
  // сама карта Tang: том (загрузочный сектор — при открытии) и отрезки секторов файла (цепочка —
  // при первом обращении за первый кластер: обход FAT большого файла — до секунды)
  Fat32Map map;
  bool mapped = false;
  const char* mapProblem = nullptr;
  std::vector<Fat32Map::Run> runs;
  // том VFS
  vdisk::Source* source = nullptr;
  std::vector<char> path;
};

namespace {

bool ensureMap(File* f) {
#if ZIFI_HAS_SPI_LINK
  if (f->mapped) {
    return f->mapProblem == nullptr;
  }
  f->mapped = true;
  if (f->mapProblem == nullptr) {
    f->mapProblem = f->map.chain(f->cluster, f->size, f->runs);
  }
  bringupLog(kTag, "Tang card file at cluster %u, %u bytes: %s (%u runs)", static_cast<unsigned>(f->cluster),
             static_cast<unsigned>(f->size), f->mapProblem ? f->mapProblem : "chain read",
             static_cast<unsigned>(f->runs.size()));
  return f->mapProblem == nullptr;
#else
  f->mapProblem = "no Tang card on this board";
  return false;
#endif
}

}  // namespace

int open(uint32_t cluster, uint32_t size, File** file) {
  *file = nullptr;
  File* f = new (std::nothrow) File();
  if (f == nullptr) {
    return 2;
  }
  f->cluster = cluster;
  f->size = size;
  if (vdisk_service::rawMode()) {
    f->raw = true;
#if ZIFI_HAS_SPI_LINK
    // загрузочный сектор тома — сразу (1–2 сектора): начало файла читается без обхода FAT
    f->mapProblem = f->map.mount(rawSector, nullptr);
    if (f->mapProblem == nullptr && f->map.clusterLba(cluster) == 0) {
      f->mapProblem = "bad first cluster";
    }
    if (f->mapProblem != nullptr) {
      bringupLog(kTag, "Tang card file at cluster %u: %s", static_cast<unsigned>(cluster), f->mapProblem);
      delete f;
      return 2;
    }
#endif
  } else {
    uint64_t length = 0;
    if (!vdisk_service::volumeFile(cluster, &f->source, f->path, &length)) {
      delete f;
      return 1;
    }
    if (length != size) {
      bringupLog(kTag, "VFS file at cluster %u: %u bytes, WC says %u", static_cast<unsigned>(cluster),
                 static_cast<unsigned>(length), static_cast<unsigned>(size));
      delete f;
      return 1;
    }
  }
  *file = f;
  return 0;
}

bool raw(const File* file) { return file != nullptr && file->raw; }
uint32_t size(const File* file) { return file != nullptr ? file->size : 0; }
const char* problem(const File* file) { return file != nullptr ? file->mapProblem : nullptr; }

bool read(File* f, uint32_t offset, uint8_t* out, uint32_t n, uint32_t* got) {
  *got = 0;
  if (f == nullptr || offset > f->size) {
    return false;
  }
  if (n > f->size - offset) {
    n = f->size - offset;
  }
  if (n == 0) {
    return true;
  }
  if (!f->raw) {
    size_t done = 0;
    const bool ok = f->source->read(f->path.data(), offset, out, n, &done);
    *got = static_cast<uint32_t>(done);
    return ok;
  }
#if ZIFI_HAS_SPI_LINK
  // в первом кластере — без цепочки (сверка начала файла при открытии: ответ Z80 не ждёт обход FAT)
  const bool first = !f->mapped && offset + n <= f->map.clusterBytes();
  if (!first && !ensureMap(f)) {
    return false;
  }
  uint8_t sector[512];
  while (*got < n) {
    const uint32_t at = offset + *got;
    uint32_t lba = 0, left = 0;
    if (first) {
      lba = f->map.clusterLba(f->cluster) + at / 512;
    } else if (!Fat32Map::locate(f->runs, at, &lba, &left)) {
      return false;
    }
    if (!tang_sd::rawRead(lba, sector)) {
      return false;
    }
    const uint32_t from = at % 512;
    const uint32_t part = 512 - from < n - *got ? 512 - from : n - *got;
    memcpy(out + *got, sector + from, part);
    *got += part;
  }
  return true;
#else
  return false;
#endif
}

int stream(File* f, uint32_t offset, uint32_t n, uint32_t* crcErrors) {
  *crcErrors = 0;
  if (f == nullptr || !f->raw || (offset & 511) != 0 || (n & 511) != 0 ||
      static_cast<uint64_t>(offset) + n > (static_cast<uint64_t>(f->size) + 511) / 512 * 512) {
    return 1;
  }
#if ZIFI_HAS_SPI_LINK
  if (!ensureMap(f)) {
    return 2;
  }
  while (n != 0) {
    uint32_t lba = 0, left = 0;
    if (!Fat32Map::locate(f->runs, offset, &lba, &left)) {
      return 2;
    }
    // блоков: до конца отрезка, до конца порции, не больше операции моста
    uint32_t blocks = n / 512;
    if (blocks > left) blocks = left;
    if (blocks > kMaxBlocks) blocks = kMaxBlocks;
    bool crc = false;
    const bool ok = tang_sd::rawStream(lba, blocks, &crc);
    *crcErrors += crc ? 1 : 0;
    if (!ok) {
      return tang_sd::videoBridge() ? 2 : 1;
    }
    offset += blocks * 512;
    n -= blocks * 512;
  }
  return 0;
#else
  return 1;
#endif
}

void close(File* file) { delete file; }

}  // namespace sd2_files
}  // namespace zifi
