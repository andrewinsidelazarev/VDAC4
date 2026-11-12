#pragma once
// Файл тома FAT32 на сырых секторах — для видео с SD2, когда SD2 — сама карта Tang (WC держит её
// посекторно, FatFs ESP её не монтирует): FTVIEW4 знает первый кластер и длину файла из записи
// каталога WC, ESP по FAT карты находит его секторы — отрезки подряд, их мост ядра видео шлёт
// потоком CMD18 прямо в медиа-FIFO.
//
// Том — суперфлоппи (загрузочный сектор в LBA 0) или первый раздел FAT32 таблицы MBR (типы 0x0B,
// 0x0C; как ищет том WC и FatFs). Сектор — 512 байт. Код переносимый: секторы — через ReadFn (на ESP
// — tang_sd::rawRead, на ПК — образ в памяти, tests/fat32_map_host.cpp).
#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace zifi {

class Fat32Map {
 public:
  using ReadFn = bool (*)(void* ctx, uint32_t lba, uint8_t* out);   // сектор 512 байт
  struct Run {
    uint32_t lba;        // первый сектор отрезка
    uint32_t sectors;    // секторов подряд
  };

  // Загрузочный сектор тома. nullptr — FAT32 разобран; иначе — что не так.
  const char* mount(ReadFn read, void* ctx);
  bool mounted() const { return mounted_; }
  uint32_t clusterBytes() const { return sectorsPerCluster_ * 512U; }
  // Первый сектор кластера (2…); 0 — кластера в томе нет
  uint32_t clusterLba(uint32_t cluster) const {
    return cluster >= 2 && cluster <= clusters_ + 1 ? dataStart_ + (cluster - 2) * sectorsPerCluster_ : 0;
  }

  // Цепочка файла с первым кластером cluster и длиной size: отрезки секторов подряд (смежные
  // кластеры — одним отрезком), ровно на ceil(size / 512) секторов. nullptr — готово.
  const char* chain(uint32_t cluster, uint64_t size, std::vector<Run>& runs);

  // Сектор и сколько секторов подряд с него — для байта offset файла (по отрезкам chain)
  static bool locate(const std::vector<Run>& runs, uint64_t offset, uint32_t* lba, uint32_t* left);

 private:
  bool fatEntry(uint32_t cluster, uint32_t* next);

  ReadFn read_ = nullptr;
  void* ctx_ = nullptr;
  bool mounted_ = false;
  uint32_t fatStart_ = 0;           // LBA первой FAT
  uint32_t dataStart_ = 0;          // LBA кластера 2
  uint32_t sectorsPerCluster_ = 0;
  uint32_t clusters_ = 0;           // кластеров данных (последний — clusters_ + 1)
  uint32_t cachedLba_ = UINT32_MAX; // сектор FAT в cache_
  uint8_t cache_[512];
};

}  // namespace zifi
