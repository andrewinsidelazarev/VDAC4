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

  // Запись — только у источника тома с записью (USB Flash, 0.1.148): WC меняет том секторами
  // FAT32, а том переводит их в эти операции над файлами источника. false — отказ (WC получит
  // ошибку носителя). Пути — как у list/read, UTF-8.
  virtual bool create(const char* path, uint64_t size) { (void)path; (void)size; return false; }   // новый файл длины size
  virtual bool write(const char* path, uint64_t offset, const uint8_t* data, size_t n) {
    (void)path; (void)offset; (void)data; (void)n; return false;
  }
  virtual bool resize(const char* path, uint64_t size) { (void)path; (void)size; return false; }
  virtual bool makeDir(const char* path) { (void)path; return false; }
  virtual bool remove(const char* path, bool dir) { (void)path; (void)dir; return false; }
  virtual bool rename(const char* from, const char* to) { (void)from; (void)to; return false; }
  // Время изменения (FAT: дата, время) — ставится, когда файл дописан (flush или другой файл)
  virtual bool setTime(const char* path, uint16_t date, uint16_t time) { (void)path; (void)date; (void)time; return false; }
  // Дописать открытое (источник может держать файл открытым между записями секторов)
  virtual void flush() {}
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
  // Том одного источника с записью (USB Flash, 0.1.148): корень тома — корень источника, WC пишет
  // секторы, том переводит их в операции источника (см. vdisk_rw.cpp).
  void configureWritable(Source* source);
  bool writable() const { return writable_; }
  // Сектор lba (512 байт) от WC. false — отказ (ошибка источника или том только для чтения).
  bool write(uint32_t lba, const uint8_t* data);
  // Дописать отложенное у источника (перед простоем, сменой режима)
  void flush();
  void reset();                     // забыть дерево: следующий запрос строит заново
  uint32_t sectorCount() const { return kTotalSectors; }
  uint8_t flags() const;
  size_t sourceCount() const { return sources_.size(); }
  uint32_t generation() const { return generation_; }   // растёт на каждом reset
  // Сектор lba (512 байт) в out. false — ошибка источника (сектор не годен).
  bool read(uint32_t lba, uint8_t* out);

  // Файл тома, который начинается кластером cluster (первый кластер из записи каталога WC: видео с
  // SD2 — sd2_files): источник, путь внутри него, длина. false — такого файла нет (каталог, середина
  // файла, том собран заново).
  bool fileAt(uint32_t cluster, Source** source, std::vector<char>& where, uint64_t* size) const;

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
    uint8_t flags;         // kDir, kLoaded, kDeleted, kWc, kMoved
    // Том с записью: где SFN-запись узла, если её поставил WC (создание, перенос)
    uint32_t slotDir;      // каталог (узел)
    uint32_t slot;         // номер 32-байтной записи в каталоге
  };
  // kDeleted — WC удалил запись (файл источника удалён); kWc — узел создан WC: кластеры — по цепочке
  // FAT (таблица владельцев), каталог — в накладке; kMoved — запись перенесена WC (slotDir/slot).
  static constexpr uint8_t kDir = 1, kLoaded = 2, kDeleted = 4, kWc = 8, kMoved = 16;
  struct Extent { uint32_t cluster, node; };

  // ---- том с записью (vdisk_rw.cpp) ----
  // Секторы, записанные WC: FAT, каталоги, данные кластеров без владельца. Открытая адресация в
  // больших векторах (PSRAM): мелкие узлы map легли бы во внутреннюю память ESP.
  class SectorStore {
   public:
    const uint8_t* find(uint32_t lba) const;
    bool put(uint32_t lba, const uint8_t* data);   // false — места нет (kMaxSectors)
    void erase(uint32_t lba);
    void clear();
    size_t size() const { return used_; }
    static constexpr size_t kMaxSectors = 8192;    // 4 МиБ
   private:
    int slotOf(uint32_t lba) const;
    std::vector<uint32_t> keys_;                    // lba + 1; 0 — пусто, 1 — удалено
    std::vector<uint32_t> index_;                   // номер сектора в pages_
    std::vector<std::vector<uint8_t>> pages_;       // по 64 сектора
    std::vector<uint32_t> freeList_;
    size_t used_ = 0, live_ = 0;
  };
  // Кластер -> (узел, номер кластера в цепочке узла) для цепочек, известных по FAT, а не по раскладке
  class OwnerMap {
   public:
    bool find(uint32_t cluster, uint32_t* node, uint32_t* index) const;
    void put(uint32_t cluster, uint32_t node, uint32_t index);
    void erase(uint32_t cluster);
    void clear();
   private:
    int slotOf(uint32_t cluster) const;
    struct Slot { uint32_t key, node, index; };     // key: cluster + 1; 0 — пусто, 1 — удалено
    std::vector<Slot> slots_;
    size_t used_ = 0, live_ = 0;
  };
  bool owner(uint32_t cluster, uint32_t* node, uint32_t* index) const;   // по таблице, потом по раскладке
  uint32_t synthFat(uint32_t cluster) const;    // FAT по раскладке
  uint32_t fatEntry(uint32_t cluster) const;    // FAT с накладкой
  bool clusterOf(uint32_t node, uint32_t index, uint32_t* cluster) const;   // index-й кластер цепочки
  uint32_t firstUsed(uint32_t first, uint32_t count) const;
  void patchFat(uint32_t first, uint32_t count);
  void claim(uint32_t cluster, uint32_t node, uint32_t index);
  void registerChain(uint32_t node, uint64_t size);
  bool writeFat(uint32_t lba, const uint8_t* data);
  bool writeDir(uint32_t node, uint64_t offset, uint32_t lba, const uint8_t* data);
  bool writeFile(uint32_t node, uint64_t offset, const uint8_t* data);
  bool record(uint32_t dir, uint32_t index, uint8_t* out);
  void entryName(uint32_t dir, uint32_t index, const uint8_t* sfn, std::vector<char>& out);
  uint32_t nodeOfRecord(uint32_t dir, const uint8_t* rec) const;
  uint32_t appendNode(uint32_t parent, const std::vector<char>& name, bool dir, uint64_t size,
                      const uint8_t* rec, uint32_t slot);
  void childPath(uint32_t dir, const std::vector<char>& name, std::vector<char>& out) const;
  void rename(uint32_t node, uint32_t dir, const std::vector<char>& name);
  bool created(uint32_t dir, uint32_t index, const uint8_t* rec);
  bool changed(uint32_t dir, uint32_t index, const uint8_t* was, const uint8_t* rec);
  bool removed(uint32_t dir, uint32_t index, const uint8_t* was);
  void finishDirty();

  bool writable_ = false;
  SectorStore overlay_;
  OwnerMap owners_;
  OwnerMap firsts_;          // первый кластер -> узел у узлов WC (index не нужен); FAT его не снимает
  uint32_t dirty_ = UINT32_MAX;   // файл, в который шла запись: дописать и поставить время
  uint64_t dirtyEnd_ = 0;         // докуда в него писали (за размером из записи — хвост сектора)

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
