#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zifi/vfs_backend.hpp"

namespace zifi {

// Том FatFs, с которым работает TangVfs. На ESP — microSD Tang (tang_sd: замок Session, монтирование,
// буква диска FatFs), на стенде ПК — образ диска в памяти.
class FatVolumePort {
 public:
  virtual ~FatVolumePort() = default;
  // Занять том (замок) и убедиться, что он смонтирован: nullptr — можно работать, иначе причина
  // (замок при отказе не держится). Каждой удачной acquire — своя release.
  virtual const char* acquire() = 0;
  virtual void release() = 0;
  // Буква диска FatFs вместе с двоеточием ("0:"); действует между acquire и release.
  virtual const char* drive() const = 0;
};

// Источник моста VFS — microSD Tang через FatFs самой ESP (шара SMB «TANG», без Z80).
// Смысл операций — как у VfsClient (vfs_backend.hpp). Пути — от корня карты ("/CORES/NES.BIN"),
// в UTF-8; FatFs прошивки работает в кодовой странице 437 (CONFIG_FATFS_API_ENCODING_ANSI_OEM),
// поэтому имя со знаками вне неё отвергается (0x13), а в листинге такие знаки — «?» от FatFs.
// Вызывать только с одной задачи (ядро 1, мост VFS).
class TangVfs : public VfsBackend {
 public:
  static constexpr uint32_t kSerial = 0x54414E47;   // «TANG»: у FatFs нет серийного номера тома
  // 16 КиБ (0.1.137, было 4): FatFs отдаёт карте серии CMD25 до 32 блоков; буфер — в PSRAM.
  static constexpr size_t kBufferSize = 16384;

  explicit TangVfs(FatVolumePort& port);
  ~TangVfs() override;

  TangVfs(const TangVfs&) = delete;
  TangVfs& operator=(const TangVfs&) = delete;

  bool stat(const char* path, VfsEntry& entry) override;
  bool openDirectory(const char* path) override;
  bool readDirectory(VfsEntry& entry, bool& atEnd) override;

  bool openFile(const char* path, bool forWrite) override;
  bool openFileAppend(const char* path) override;
  bool openFileRandom(const char* path) override;
  bool seekFile(uint32_t offset) override;
  bool readFileWindow(size_t wanted, WindowSink sink, void* sinkContext,
                      size_t& received) override;
  bool writeFileWindow(size_t length, WindowSource source, void* sourceContext) override;
  bool extendFile(uint32_t length) override;
  bool setFileSize(uint32_t size, uint32_t& actualSize) override;
  bool closeFile(bool commit = true) override;

  bool getFsInfo(VfsFsInfo& info, bool refreshFree = false, bool cacheOnly = false) override;
  void noteFileResized(uint32_t, uint32_t) override {}
  void invalidateFsCache() override {}
  bool moveRename(const char* oldPath, const char* newPath, bool directory,
                  bool replace) override;
  bool setMetadata(const VfsMetadata& metadata, uint8_t& appliedAttributes) override;

  bool remove(const char* path) override;
  bool makeDirectory(const char* path) override;
  bool rename(const char* oldPath, const char* newName, bool directory) override;

  const char* lastError() const override { return lastError_; }
  uint8_t lastStatus() const override { return lastStatus_; }
  uint8_t openMode() const override { return openMode_; }

  // UTF-8 <-> имя FatFs (кодовая страница тома или UTF-8 — как собрана FatFs). Для тестов.
  static bool toFatName(const char* utf8, char* output, size_t capacity);
  static bool fromFatName(const char* fat, char* output, size_t capacity);

 private:
  struct State;
  class Lock;

  bool fatPath(const char* path, char* output, size_t capacity);
  bool openWith(const char* path, uint8_t mode);
  bool fail(int result, const char* what);
  bool failStatus(uint8_t status, const char* format, ...);
  void closeDirectory();
  void applyPendingTimes();

  FatVolumePort& port_;
  State* state_;
  uint8_t* buffer_;
  uint8_t openMode_;
  uint8_t lastStatus_;
  char lastError_[64];
};

}  // namespace zifi
