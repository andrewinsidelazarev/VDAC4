#pragma once

#include <stddef.h>
#include <stdint.h>

namespace zifi {

// Кэш модулей ESP во flash (LittleFS): копии файлов /WC/ESP/ с карты Evo —
// приложений (PLAYER.ELF) и декодеров. Ключ — имя 8.3, длина и время записи
// FAT: изменился файл на карте — изменились длина или время, и плагин пришлёт
// его снова. Места не хватает — вытесняется давно не нужный (кроме занятого).
//
// Хранилище — за ZmStorage: на ESP это каталог в LittleFS, в тестах на ПК —
// каталог на диске. Однопоточный: вызывает только ядро обмена.
//
// Место кэш считает сам: сколько ему отведено (capacityBytes) минус то, что
// занимают его файлы. Свободное место всей LittleFS (esp_littlefs_info) для
// этого не годится: ошибку lfs_fs_size драйвер выдаёт за «занято всё», и в
// vdac4-0.1.23 кэш на плате видел 0 КБ при пустом разделе.
//
// Питание может пропасть посреди любой записи. Поэтому указатель никогда не
// ссылается на удалённый файл: запись из него убирается и указатель сохраняется
// раньше, чем удаляется файл. При старте (load) записи, чей файл пропал или
// другой длины, выбрасываются, а файлы, которых в указателе нет, удаляются;
// модуль, который не прочитался (read), выбрасывается — плагин пришлёт его снова.

struct ZmCacheEntry {
  char name[13];      // "PLAYER.ELF", пусто — запись свободна
  uint32_t size;
  uint32_t stamp;     // дата FAT << 16 | время FAT
  uint32_t used;      // часы кэша: когда модуль брали последним
};

class ZmStorage {
 public:
  virtual ~ZmStorage() = default;
  // Файл целиком; false — нет или длина другая.
  virtual bool read(const char* file, uint8_t* data, size_t size) = 0;
  virtual bool write(const char* file, const uint8_t* data, size_t size) = 0;
  virtual bool remove(const char* file) = 0;
  // Сколько байтов могут занять файлы кэша вместе с указателем.
  virtual size_t capacityBytes() = 0;
  // Длина файла; kMissing — его нет; kUnknown — хранилище этого не умеет (сверки нет).
  static constexpr long kMissing = -1;
  static constexpr long kUnknown = -2;
  virtual long fileSize(const char* file) {
    (void)file;
    return kUnknown;
  }
  // Каждый файл каталога кэша (и указатель) — в each; false — хранилище не умеет.
  virtual bool list(void (*each)(const char* file, void* context), void* context) {
    (void)each;
    (void)context;
    return false;
  }
};

class ZmCache {
 public:
  static constexpr size_t kMaxEntries = 24;
  // Блок LittleFS: файл занимает целые блоки и ещё блок метаданных.
  static constexpr size_t kBlockBytes = 4096;

  explicit ZmCache(ZmStorage& storage) : storage_(storage) {}

  // Прочитать указатель кэша; испорченный или чужой — кэш пуст.
  void load();
  // Годится ли имя: 8.3, буквы, цифры, «_~-», одна точка.
  static bool validName(const char* name);
  // Модуль с этим именем, длиной и временем уже лежит.
  bool has(const char* name, uint32_t size, uint32_t stamp) const;
  const ZmCacheEntry* find(const char* name) const;
  // Положить модуль; busy — имя, которое вытеснять нельзя (активное
  // приложение), или nullptr. false — не поместился или запись не удалась.
  bool store(const char* name, uint32_t stamp, const uint8_t* data, uint32_t size,
             const char* busy);
  // Прочитать модуль целиком в data (size — из find()) и отметить, что взят.
  bool read(const char* name, uint8_t* data, uint32_t size);
  size_t count() const;
  // Место во flash под файл такой длины.
  static size_t footprint(size_t size);
  // Свободно в кэше, байтов (без вытеснения).
  size_t freeBytes() const;
  // Поместится ли файл, если вытеснить всё, кроме busy.
  bool canFit(uint32_t size, const char* busy) const;

 private:
  static constexpr uint32_t kMagic = 0x315A4D43;   // "CMZ1"
  static constexpr char kIndex[] = "index.bin";

  ZmCacheEntry* slot(const char* name);
  bool evictOne(const char* busy);
  bool saveIndex();
  // Убрать запись и её файл: сначала указатель без неё, потом файл.
  void drop(ZmCacheEntry* entry);

  ZmStorage& storage_;
  ZmCacheEntry entries_[kMaxEntries] = {};
  uint32_t clock_ = 0;
};

}  // namespace zifi
