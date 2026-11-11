#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zifi/fat_allocation_cache.hpp"
#include "zifi/vfs_backend.hpp"
#include "zifi/z80_transport.hpp"

namespace zifi {

// Синхронный клиент файлового API Wild Commander. Вызывать его разрешено
// только с ядра 1: он отправляет VFS-команды в канал и ждёт ответы Z80.
// Источник моста VFS — том WC (vfs_backend.hpp).
class VfsClient : public VfsBackend {
 public:
  static constexpr size_t kTransferWindowSize = 16 * 1024;
  // FILEX needs a 32-byte parameter block in the same 16 KiB page as data.
  static constexpr size_t kFilexTransferWindowSize =
      kTransferWindowSize - 32;
  // Окно FILEX, кратное сектору: 31 сектор, 15872 байта. Им SMB-сервер режет
  // позиционную запись и чтение. Окно 16352 не кратно сектору: на каждом краю
  // FILEX дочитывал неполный сектор перед записью (а при чтении читал сектор
  // стыка дважды), а кусок 16384 уходил двумя вызовами — 16352 и 32 байта, —
  // и каждый заново проходил FAT-цепочку от начала файла. С этим окном запрос
  // 64 КиБ — пять вызовов FILEX вместо восьми, без неполных секторов.
  static constexpr size_t kFilexSectorWindowSize =
      (kFilexTransferWindowSize / 512) * 512;

  explicit VfsClient(Z80Transport& transport);

  void beginFatCache(bool psramAvailable);

  bool stat(const char* path, VfsEntry& entry) override;
  bool openDirectory(const char* path) override;
  bool readDirectory(VfsEntry& entry, bool& atEnd) override;

  bool openFile(const char* path, bool forWrite) override;
  bool openFileAppend(const char* path) override;
  bool openFileRandom(const char* path) override;
  bool seekFile(uint32_t offset) override;
  bool readFile(uint8_t* output, size_t capacity, size_t wanted,
                size_t& received);
  bool readFileWindow(size_t wanted, WindowSink sink, void* sinkContext,
                      size_t& received) override;
  bool writeFile(const uint8_t* data, size_t length);
  bool writeFileWindow(size_t length, WindowSource source,
                       void* sourceContext) override;
  bool extendFile(uint32_t length) override;
  bool setFileSize(uint32_t size, uint32_t& actualSize) override;
  bool closeFile(bool commit = true) override;

  // cacheOnly — вернуть ответ, только если он уже есть: вызывающий ещё не
  // закрывал активный файл и по отказу поймёт, что закрыть придётся.
  bool getFsInfo(VfsFsInfo& info, bool refreshFree = false,
                 bool cacheOnly = false) override;
  // Изменение длины файла: единственный способ сообщить владельцу счётчика,
  // сколько кластеров занято или освобождено, без обхода FAT.
  void noteFileResized(uint32_t oldSize, uint32_t newSize) override;
  // Полный сброс сведений о томе — для операций, объём которых неизвестен.
  void invalidateFsCache() override;
  bool moveRename(const char* oldPath, const char* newPath, bool directory,
                  bool replace) override;
  bool setMetadata(const VfsMetadata& metadata, uint8_t& appliedAttributes) override;

  bool remove(const char* path) override;
  bool makeDirectory(const char* path) override;
  bool rename(const char* oldPath, const char* newName, bool directory) override;

  const char* lastError() const override { return lastError_; }
  uint8_t lastStatus() const override { return lastStatus_; }
  uint8_t filexCapabilities() const { return filexCapabilities_; }
  uint8_t openMode() const override { return openMode_; }

 private:
  static constexpr size_t kBlockSize = 512;
  // Структура FILEX SET_METADATA/GET_METADATA в ответе STAT.
  static constexpr uint8_t kMetadataSize = 16;
  static constexpr size_t kFirstData = 248;
  static constexpr size_t kContinueData = 252;
  static constexpr size_t kWriteWindowHeader = 8;
  static constexpr size_t kReadWindowHeader = 9;
  static constexpr size_t kFatTransferWindowSize = 31 * 512;
  static constexpr size_t kWriteWindowData = kMaxPayload - kWriteWindowHeader;
  static constexpr uint8_t kCapabilityWriteWindow = 0x01;
  static constexpr uint8_t kCapabilityReadWindow = 0x02;
  static constexpr uint8_t kCapabilityExtend = 0x04;
  static constexpr uint8_t kWindowStart = 0x01;
  static constexpr uint8_t kWindowEnd = 0x02;

  bool request(uint8_t command, const uint8_t* data, uint16_t length,
               uint32_t timeoutMs, PacketView& response);
  bool pathRequest(uint8_t command, const char* path, uint32_t timeoutMs,
                   PacketView& response);
  bool flushWriteBlock(size_t rawLength);
  bool openFileMode(const char* path, uint8_t mode);
  bool checkBlockAck(const PacketView& response, uint8_t sequence,
                     uint16_t accepted);
  bool readFatWindow(uint32_t rawOffset, size_t wanted, WindowSink sink,
                     void* sinkContext, size_t& received);
  bool buildFatCache(VfsFsInfo& info);
  void rememberFsInfo(const VfsFsInfo& info);
  static bool ingestFatWindow(void* context, const uint8_t* data,
                              size_t length);
  void resetWriteState(bool active);
  void setError(const char* format, ...);
  static void handleUnexpected(void* context, const PacketView& packet);
  bool fetchDirectoryBatch();
  static void parseDirectoryEntry(const PacketView& response, VfsEntry& entry);

  // Пакетный READDIR: плагин объявляет его байтом возможностей в ответе
  // OPENDIR. Один запрос приносит до kDirectoryBatchEntries записей отдельными
  // кадрами — Z80 ищет следующую запись, пока предыдущая идёт по линии, — и
  // вместо обмена «запрос — ответ» на каждую запись остаётся один на пачку.
  static constexpr uint8_t kDirectoryCapabilityBatch = 0x01;
  static constexpr uint8_t kDirectoryBatchMore = 2;
  static constexpr size_t kDirectoryBatchEntries = 16;

  Z80Transport& transport_;
  char lastError_[64];
  uint8_t writeBuffer_[kBlockSize];
  uint8_t fragment_[kMaxPayload];
  size_t writeCount_;
  uint8_t writeSequence_;
  uint8_t readSequence_;
  uint8_t fatSequence_;
  uint8_t capabilities_;
  uint8_t filexCapabilities_;
  uint8_t lastStatus_;
  uint8_t openMode_;
  uint32_t randomOffset_;
  bool randomOffsetValid_;
  bool writeActive_;
  bool writeFailed_;
  FatAllocationCache fatCache_;
  // Последние прочитанные сведения о томе: геометрия читается один раз, а
  // свободное место поддерживается точным через noteFileResized.
  VfsFsInfo fsInfoCache_;
  bool fsInfoValid_;
  // Пачка записей каталога, ещё не отданных мосту. Новый OPENDIR её сбрасывает.
  // Сама пачка лежит в PSRAM: у Application жёсткий бюджет статической памяти.
  // Без PSRAM каталог читается, как прежде, по одной записи.
  bool directoryBatch_;
  bool directoryEnded_;
  uint8_t directoryCount_;
  uint8_t directoryNext_;
  VfsEntry* directoryEntries_;
};

}  // namespace zifi
