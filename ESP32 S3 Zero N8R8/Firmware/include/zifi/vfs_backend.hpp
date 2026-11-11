#pragma once

#include <stddef.h>
#include <stdint.h>

namespace zifi {

struct VfsEntry {
  bool isDirectory = false;
  uint32_t size = 0;
  // Штамп изменения записи FAT — местное время, как его пишут WC и Windows.
  // Дата 0 — неизвестен: старый плагин или старое ядро WC.
  uint16_t writeDate = 0;
  uint16_t writeTime = 0;
  // Остальные времена и атрибут есть только в ответе STAT нового плагина
  // (FILEX GET_METADATA). В листинге каталога их нет.
  bool hasMetadata = false;
  uint8_t attributes = 0;
  uint8_t createTenth = 0;
  uint16_t createTime = 0;
  uint16_t createDate = 0;
  uint16_t accessDate = 0;
  char name[256] = {};
};

struct VfsFsInfo {
  uint8_t flags = 0;
  uint8_t sectorsPerCluster = 0;
  uint16_t bytesPerSector = 0;
  uint32_t totalClusters = 0;
  uint32_t freeClusters = 0xFFFFFFFFUL;
  uint32_t serial = 0;
  char label[12] = {};
  uint32_t totalSectors = 0;
  uint32_t nextFree = 0xFFFFFFFFUL;
  uint8_t fatCount = 0;
  uint8_t activeFat = 0;
  uint8_t fatFlags = 0;
  uint8_t mediaState = 0;
  uint32_t fatSectors = 0;
};

struct VfsMetadata {
  uint8_t attrMask = 0;
  uint8_t attrValue = 0;
  uint8_t timeMask = 0;
  uint8_t createTenth = 0;
  uint16_t createTime = 0;
  uint16_t createDate = 0;
  uint16_t accessDate = 0;
  uint16_t writeTime = 0;
  uint16_t writeDate = 0;
  // Эти значения нужны только SMB-серверу для согласованного QUERY_INFO.
  // В пакет FILEX по-прежнему уходят первые 16 байт в формате FAT.
  uint64_t createFileTime = 0;
  uint64_t accessFileTime = 0;
  uint64_t writeFileTime = 0;
};

// Источник файлов моста VFS (vfs_bridge): то, что мост исполняет на ядре 1. Их два:
//   VfsClient — том Wild Commander через Z80 (файловый API WC, FILEX);
//   TangVfs   — microSD Tang через FatFs самой ESP (шара SMB «TANG»).
// Смысл операций задаёт VfsClient: один открытый файл или один обход каталога за раз,
// режимы открытия 0 — чтение, 1 — запись с нуля, 2 — дописывание, 3 — произвольный доступ
// (SEEK, SET_EOF, SET_METADATA). Коды ошибок lastStatus() — коды FILEX: 0x01 конец файла,
// 0x15 не поддерживается, 0x18 защита записи, 0x19 нет имени, 0x1A имя занято, 0x1B каталог
// не пуст, 0x22 нет места, 0xFF сбой обмена.
class VfsBackend {
 public:
  using WindowSource = size_t (*)(void* context, size_t offset,
                                  uint8_t* output, size_t capacity);
  using WindowSink = bool (*)(void* context, const uint8_t* data,
                              size_t length);

  virtual ~VfsBackend() = default;

  virtual bool stat(const char* path, VfsEntry& entry) = 0;
  virtual bool openDirectory(const char* path) = 0;
  virtual bool readDirectory(VfsEntry& entry, bool& atEnd) = 0;

  virtual bool openFile(const char* path, bool forWrite) = 0;
  virtual bool openFileAppend(const char* path) = 0;
  virtual bool openFileRandom(const char* path) = 0;
  virtual bool seekFile(uint32_t offset) = 0;
  virtual bool readFileWindow(size_t wanted, WindowSink sink, void* sinkContext,
                              size_t& received) = 0;
  virtual bool writeFileWindow(size_t length, WindowSource source,
                               void* sourceContext) = 0;
  virtual bool extendFile(uint32_t length) = 0;
  virtual bool setFileSize(uint32_t size, uint32_t& actualSize) = 0;
  virtual bool closeFile(bool commit = true) = 0;

  virtual bool getFsInfo(VfsFsInfo& info, bool refreshFree = false,
                         bool cacheOnly = false) = 0;
  virtual void noteFileResized(uint32_t oldSize, uint32_t newSize) = 0;
  virtual void invalidateFsCache() = 0;
  virtual bool moveRename(const char* oldPath, const char* newPath, bool directory,
                          bool replace) = 0;
  virtual bool setMetadata(const VfsMetadata& metadata, uint8_t& appliedAttributes) = 0;

  virtual bool remove(const char* path) = 0;
  virtual bool makeDirectory(const char* path) = 0;
  virtual bool rename(const char* oldPath, const char* newName, bool directory) = 0;

  virtual const char* lastError() const = 0;
  virtual uint8_t lastStatus() const = 0;
  virtual uint8_t openMode() const = 0;
};

}  // namespace zifi
