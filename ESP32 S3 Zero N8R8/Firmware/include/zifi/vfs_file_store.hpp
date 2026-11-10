#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zifi/git_sha1.hpp"
#include "zifi/github_tree.hpp"
#include "zifi/vfs_bridge.hpp"

namespace zifi {

// Файлы карты Evo через мост VFS (FAT ведёт сам Wild Commander на Z80) —
// механизм обновлятора WC, вынесенный из WcUpdater: им же ставит файлы карты
// Evo обновлятор VDAC4 (vdac4_updater). Здесь всё, что касается карты:
// опись каталога, SHA файла чтением, запись копии WCUPD.TMP с чтением
// обратно и замена ею прежнего файла с откатом, поиск остатков прерванной
// замены. GitHub, таблица файлов и события — у владельца.
//
// Пути — от корня тома, где плагин нашёл /zifi/zifi.ini ("/WC/ESP/X.ELF");
// «относительный» путь — тот же без первой '/' ("WC/ESP/X.ELF"): по нему
// находится каталог в таблице каталогов.
//
// Причина любой неудачи — в буфере error владельца (его lastError): владелец
// показывает её в строке итога. Все методы зовёт одна задача ядра 0; stop —
// флаг остановки сеанса владельца.
class VfsFileStore {
 public:
  static constexpr const char* kTempName = "WCUPD.TMP";
  // Прежний файл на время замены запасным путём (WC без FILEX MOVE).
  static constexpr const char* kAsideName = "WCUPD.OLD";
  static constexpr const char* kLeftoverReason =
      "CHKDSK: old WCUPD.* in folder";
  static constexpr unsigned kWriteAttempts = 3;
  // Ответы Z80 на VFS MOVE_RENAME: у WC нет FILEX MOVE (ничего не менялось)
  // и статус FILEX «перенос состоялся, старая цепочка не освобождена».
  static constexpr uint8_t kMoveUnsupported = 0xFE;
  static constexpr uint8_t kFilexCommittedCleanup = 0x25;
  static constexpr size_t kMaxDirectories = 16;
  static constexpr size_t kReasonSize = 40;
  static constexpr size_t kMaxError = 128;

  // Прочитано или записано столько байтов — для полосы хода владельца.
  using Progress = void (*)(void* context, uint32_t bytes);
  // Запись каталога при описи. relative — путь от корня (nullptr — путь не
  // поместился в GitTreeEntry::kPathSize); каталоги, «.», «..» и остатки
  // WCUPD.* сюда не приходят.
  using Visitor = void (*)(void* context, size_t directory,
                           const char* relative, const VfsResult& entry);
  // Скачать файл целиком в buffer и сверить его SHA; false — причина в error.
  using Download = bool (*)(void* context, uint8_t* buffer);

  enum class Scan : uint8_t { kDone, kStopped, kFailed };
  // Итог замены файла копией: kDone — копия стоит на имени файла;
  // kUntouched — ничего не менялось, копию можно удалить; kUnknown — исход
  // неизвестен (сбой носителя, обрыв связи, любой отказ RENAME): копию
  // удалять нельзя.
  enum class Replace : uint8_t { kDone, kUntouched, kUnknown };

  // Итог установки одного файла (installFile).
  struct Install {
    bool done = false;       // на карте файл с SHA эталона
    bool installed = false;  // копия встала на имя файла в этом вызове
    bool keptThere = false;  // защищённый файл уже был на карте: только сверка
    uint32_t keptSize = 0;   // его длина
    char reason[kReasonSize] = {};
  };

  VfsFileStore(VfsBridge& bridge, const volatile bool* stop, char* error,
               size_t errorSize);

  VfsFileStore(const VfsFileStore&) = delete;
  VfsFileStore& operator=(const VfsFileStore&) = delete;

  void setProgress(Progress progress, void* context) {
    progress_ = progress;
    progressContext_ = context;
  }

  // Каталоги эталона (относительные пути, "" — корень): только в них идёт
  // опись и только в них пишутся файлы.
  void clearDirectories();
  bool addDirectory(const char* path);  // false — таблица полна
  size_t directoryCount() const { return directoryCount_; }
  const char* directory(size_t index) const { return directories_[index]; }
  // Номер каталога эталона: первые length байтов dir без учёта регистра;
  // -1 — такого нет.
  int directoryIndex(const char* dir, size_t length) const;
  // Номер каталога эталона, где лежит файл relativeFile.
  int directoryOf(const char* relativeFile) const;
  bool leftoverIn(const char* relativeFile) const;
  void markLeftover(const char* relativeFile);
  // Опись нашла WCUPD.TMP или WCUPD.OLD: прерванная замена. Такой остаток
  // может делить цепочку кластеров с файлом (сбой с неподтверждённым
  // откатом), и удалять его нельзя; запись в его каталог запрещена до
  // проверки карты.
  bool leftoverSeen() const { return leftoverSeen_; }
  // Замена кончилась неизвестным исходом: карту надо проверить, дальше в
  // этом сеансе не писать.
  bool diskSuspect() const { return diskSuspect_; }

  bool vfs(VfsOperation operation, const char* path, uint32_t value,
           VfsResult& result, uint32_t timeoutMs);

  // Опись каталога index: каждая запись — visitor. Каталога нет на карте —
  // kDone без записей. kFailed — причина в error.
  Scan scanDirectory(size_t index, Visitor visitor, void* context);
  // git-SHA-1 файла длиной size (иная длина — отказ).
  bool hashLocal(const char* path, uint32_t size,
                 uint8_t digest[Sha1::kDigestSize]);
  // Скачанный и сверенный файл встаёт на место relativeFile: копия, чтение
  // обратно, замена, сверка самого файла; два полных круга. keepExisting —
  // защищённый файл (wc.ini): ставится, только если его на карте нет, а
  // имеющийся только сверяется. buffer — не меньше size + 1 байт.
  void installFile(const char* relativeFile, const uint8_t sha[Sha1::kDigestSize],
                   uint32_t size, bool keepExisting, uint8_t* buffer,
                   Download download, void* context, Install& result);

  static void localPath(const char* relative, char* output, size_t capacity);
  // FAT сравнивает имена без учёта регистра ASCII; байты UTF-8 — как есть.
  static bool equalNoCase(const char* left, const char* right);

 private:
  // left — на SD осталась копия, созданная этим вызовом: при успехе либо
  // при сбое, когда удалить её не удалось. Занятое имя плагин обновлятора
  // на запись не открывает (MKFILE без удаления прежнего файла).
  bool writeLocal(const char* path, const uint8_t* data, uint32_t size,
                  bool& left);
  bool ensureDirectory(const char* relativeFile);
  // keepExisting — защищённый файл: ставится, только если его на SD нет.
  // relativeFile — путь файла от корня WC: по нему отмечается остаток.
  Replace vfsReplace(const char* tempPath, const char* finalPath,
                     const char* finalName, const char* asidePath,
                     const char* relativeFile, bool keepExisting);
  Replace renameReplace(const char* tempPath, const char* finalPath,
                        const char* finalName, const char* asidePath,
                        const char* relativeFile, bool keepExisting);
  // RENAME в пределах каталога; false — отказ или обрыв связи.
  bool renameEntry(const char* oldPath, const char* newName);
  bool probeLeftovers(size_t index, const char* directoryPath);
  // Запрос уборки: его отказ не подменяет причину неудачи в error.
  bool quietVfs(VfsOperation operation, const char* path, uint32_t timeoutMs);
  bool awaitVfs(VfsResult& result, uint32_t timeoutMs);
  void addProgress(uint32_t bytes) {
    if (progress_ != nullptr) {
      progress_(progressContext_, bytes);
    }
  }
  bool stopped() const { return stop_ != nullptr && *stop_; }
  void setError(const char* format, ...);

  VfsBridge& bridge_;
  const volatile bool* stop_;
  char* error_;
  size_t errorSize_;
  Progress progress_;
  void* progressContext_;
  bool diskSuspect_;
  bool leftoverSeen_;
  char directories_[kMaxDirectories][GitTreeEntry::kPathSize];
  bool leftover_[kMaxDirectories];
  // Опись прочитала каталог (он есть на SD) либо обновлятор создал его
  // сам. Каталог, который опись сочла отсутствующим, а запись нашла, —
  // признак ошибок чтения: остаток в нём мог остаться незамеченным.
  bool present_[kMaxDirectories];
  size_t directoryCount_;
  uint8_t chunk_[1024];
};

}  // namespace zifi
