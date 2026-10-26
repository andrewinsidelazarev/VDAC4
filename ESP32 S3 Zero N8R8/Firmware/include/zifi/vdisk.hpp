#pragma once
// Виртуальный диск VDAC4 VFS: том FAT32, который ESP собирает на лету из своих источников.
//
// Wild Commander видит его вместо SD2 (устройство 6, драйвер плагина VDAC4 VFS) и сам разбирает
// FAT32: ESP отвечает только на чтение секторов по LBA. В корне тома — по папке на источник:
// TANG — microSD Tang, USB — флешка на USB host ESP, дальше — сетевые (например vtrd.in).
// Каталоги читаются лениво, когда WC впервые читает их кластер; кластеры выдаются по порядку
// и больше не меняются, пока том не сброшен (reset). Только чтение.
//
// Раскладка: суперфлоппи (загрузочный сектор в LBA 0, без MBR), кластер 32 КиБ, каталог — 4
// кластера (4096 записей с LFN), две копии FAT. Имена — UTF-8 от источника, в томе — LFN UTF-16
// и короткое имя 8.3 с хвостом ~N.
#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace zifi {
namespace vdisk {

constexpr uint32_t kSectorSize = 512;
constexpr uint32_t kSectorsPerCluster = 64;
constexpr uint32_t kClusterBytes = kSectorSize * kSectorsPerCluster;
constexpr uint32_t kReservedSectors = 32;
constexpr uint32_t kFatCount = 2;
constexpr uint32_t kTotalSectors = 0x08000000;   // 64 ГиБ адресного места
constexpr uint32_t kDirClusters = 4;             // 128 КиБ на каталог: 4096 записей
constexpr uint32_t kRootCluster = 2;
constexpr uint32_t kMaxNodes = 65536;

// Запись каталога источника
struct Entry {
  const char* name;      // UTF-8, без '/'
  bool dir;
  uint64_t size;         // байт (файл)
  uint16_t date, time;   // FAT: дата и время изменения (0 — нет)
};

// Источник — папка в корне тома
class Source {
 public:
  virtual ~Source() = default;
  virtual const char* folder() const = 0;   // имя папки в корне ("TANG")
  // Каталог path ("" — корень источника, иначе "a/b"): each(entry) на каждую запись; false — каталога нет
  using Each = bool (*)(const Entry& entry, void* user);   // false — хватит
  virtual bool list(const char* path, Each each, void* user) = 0;
  // Байты файла path с offset: true — прочитано *got (меньше n только у конца файла)
  virtual bool read(const char* path, uint64_t offset, uint8_t* out, size_t n, size_t* got) = 0;
};

enum Flags : uint8_t {
  kReady = 0x01,       // том собран
  kReadOnly = 0x02,
  kTruncated = 0x04,   // что-то не поместилось (каталог длиннее 2048 записей, узлов или места мало)
};

class Volume {
 public:
  // Источники живут дольше тома; порядок — порядок папок в корне
  void configure(Source* const* sources, size_t count);
  void reset();                     // забыть дерево: следующий запрос строит заново
  uint32_t sectorCount() const { return kTotalSectors; }
  uint8_t flags() const;
  size_t sourceCount() const { return sources_.size(); }
  uint32_t generation() const { return generation_; }   // растёт на каждом reset
  // Сектор lba (512 байт) в out. false — ошибка источника (сектор не годен).
  bool read(uint32_t lba, uint8_t* out);

  // Для проверок: сколько узлов и кластеров выдано
  size_t nodeCount() const { return nodes_.size(); }
  uint32_t clustersUsed() const { return nextCluster_ - kRootCluster; }

 private:
  struct Node {
    uint32_t parent;
    uint32_t firstChild;   // дети загруженного каталога — подряд
    uint32_t childCount;
    uint32_t cluster;      // первый кластер (0 — пустой файл)
    uint32_t clusters;
    uint64_t size;
    uint32_t nameOffset;   // в names_
    uint16_t nameLength;
    uint16_t date, time;
    uint8_t sfn[11];
    uint8_t lfnCount;      // записей LFN перед короткой
    uint8_t source;        // 0xFF — корень тома
    uint8_t flags;         // kDir, kLoaded
  };
  static constexpr uint8_t kDir = 1, kLoaded = 2;
  struct Extent { uint32_t cluster, node; };

  void build();
  bool load(uint32_t node);
  uint32_t addNode(uint32_t parent, const char* name, size_t nameLength, bool dir, uint64_t size,
                   uint16_t date, uint16_t time, uint8_t source);
  bool allocate(Node& node, uint32_t clusters);
  void makeShortName(uint32_t parent, uint32_t first, uint32_t node);
  bool path(uint32_t node, std::vector<char>& out) const;   // путь внутри источника
  int findExtent(uint32_t cluster) const;
  void renderBoot(uint8_t* out) const;
  void renderInfo(uint8_t* out) const;
  void renderFat(uint32_t sector, uint8_t* out) const;
  bool renderDir(uint32_t node, uint64_t offset, uint8_t* out);
  bool renderFile(uint32_t node, uint64_t offset, uint8_t* out);
  uint32_t fatSectors() const { return fatSectors_; }
  uint32_t dataStart() const { return kReservedSectors + kFatCount * fatSectors_; }
  uint32_t clusterCount() const { return clusterCount_; }

  std::vector<Source*> sources_;
  std::vector<Node> nodes_;
  std::vector<Extent> extents_;
  std::vector<char> names_;
  uint32_t nextCluster_ = kRootCluster;
  uint32_t fatSectors_ = 0, clusterCount_ = 0;
  uint32_t generation_ = 0;
  bool built_ = false, truncated_ = false;

  // Окно файла: последние прочитанные байты (чтение WC идёт по порядку)
  struct Window { uint32_t node = UINT32_MAX; uint64_t offset = 0; size_t length = 0; std::vector<uint8_t> data; };
  Window windows_[2];
  unsigned nextWindow_ = 0;
};

}  // namespace vdisk
}  // namespace zifi
