#include "zifi/vfs_file_store.hpp"

#include <Arduino.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace zifi {
namespace {

// Ожидание моста дольше собственных пределов VfsClient (5 с, 60 с, 180 с):
// обмен ядра 1 всегда заканчивается раньше, и мост не остаётся занятым.
constexpr uint32_t kVfsNormalWaitMs = 15000;
constexpr uint32_t kVfsMutateWaitMs = 70000;
constexpr uint32_t kVfsCloseWaitMs = 190000;

}  // namespace

VfsFileStore::VfsFileStore(VfsBridge& bridge, const volatile bool* stop,
                           char* error, size_t errorSize)
    : bridge_(bridge),
      stop_(stop),
      error_(error),
      errorSize_(errorSize < kMaxError ? errorSize : kMaxError),
      progress_(nullptr),
      progressContext_(nullptr),
      diskSuspect_(false),
      leftoverSeen_(false),
      directories_{},
      leftover_{},
      present_{},
      directoryCount_(0),
      chunk_{} {}

bool VfsFileStore::equalNoCase(const char* left, const char* right) {
  while (*left != 0 && *right != 0) {
    const unsigned char a = static_cast<unsigned char>(*left++);
    const unsigned char b = static_cast<unsigned char>(*right++);
    // FAT сравнивает имена без учёта регистра ASCII; байты UTF-8 — как есть.
    if ((a < 0x80 ? toupper(a) : a) != (b < 0x80 ? toupper(b) : b)) {
      return false;
    }
  }
  return *left == 0 && *right == 0;
}

void VfsFileStore::localPath(const char* relative, char* output,
                             size_t capacity) {
  snprintf(output, capacity, "/%s", relative);
}

void VfsFileStore::setError(const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(error_, errorSize_, format, arguments);
  va_end(arguments);
}

void VfsFileStore::clearDirectories() {
  directoryCount_ = 0;
  memset(leftover_, 0, sizeof(leftover_));
  memset(present_, 0, sizeof(present_));
}

bool VfsFileStore::addDirectory(const char* path) {
  if (directoryCount_ == kMaxDirectories) {
    return false;
  }
  snprintf(directories_[directoryCount_], sizeof(directories_[0]), "%s",
           path);
  leftover_[directoryCount_] = false;
  present_[directoryCount_] = false;
  ++directoryCount_;
  return true;
}

bool VfsFileStore::awaitVfs(VfsResult& result, uint32_t timeoutMs) {
  const uint32_t started = millis();
  while (static_cast<uint32_t>(millis() - started) < timeoutMs) {
    if (bridge_.takeResult(result)) {
      if (!result.success) {
        setError("%s", result.error);
      }
      return result.success;
    }
    bridge_.waitForResult(1);
  }
  // Обмен ещё принадлежит ядру 1: забрать его результат, иначе следующий
  // запрос не пройдёт. Пределы VfsClient короче наших, так что он придёт.
  setError("vfs timeout");
  bridge_.reclaim(kVfsCloseWaitMs);
  return false;
}

bool VfsFileStore::vfs(VfsOperation operation, const char* path,
                       uint32_t value, VfsResult& result,
                       uint32_t timeoutMs) {
  memset(&result, 0, sizeof(result));
  if (!bridge_.submit(operation, path, value)) {
    setError("vfs busy");
    return false;
  }
  return awaitVfs(result, timeoutMs);
}

// Проверенная копия встаёт на место старого файла.
//
// Основной путь — FILEX MOVE_RENAME с REPLACE (Wild Commander Improved): WC
// перенаправляет запись файла на цепочку копии одной записью сектора, а при
// сбое записи возвращает прежнюю. Имя файла не пропадает ни на миг: и при
// пропадании питания на карте либо старый файл, либо новый.
//
// Защищённый файл (wc.ini) ставится переносом без REPLACE: если он всё-таки
// есть на карте, FILEX откажет (#1A EXISTS), и файл пользователя цел, даже
// если опись по ошибке сочла его отсутствующим.
VfsFileStore::Replace VfsFileStore::vfsReplace(const char* tempPath,
                                               const char* finalPath,
                                               const char* finalName,
                                               const char* asidePath,
                                               const char* relativeFile,
                                               bool keepExisting) {
  VfsResult result;
  memset(&result, 0, sizeof(result));
  if (!bridge_.submitMoveRename(tempPath, finalPath, false, !keepExisting)) {
    setError("vfs busy");
    return Replace::kUntouched;
  }
  if (awaitVfs(result, kVfsMutateWaitMs) ||
      result.status == kFilexCommittedCleanup) {
    return Replace::kDone;  // #25: перенос состоялся, хвост — потерянные кластеры
  }
  if (result.status == kMoveUnsupported) {
    return renameReplace(tempPath, finalPath, finalName, asidePath,
                         relativeFile, keepExisting);
  }
  // Отказ плагина (путь, каталог) и проверки FILEX #10..#1D — до изменений.
  if (result.status == 1 || (result.status >= 0x10 && result.status <= 0x1D)) {
    setError("replace refused: %s", result.error);
    return Replace::kUntouched;
  }
  // Сбой носителя (#20..#24, #26) или обрыв связи: откат мог и не пройти.
  setError("check the disk: replace %s", result.error);
  return Replace::kUnknown;
}

// Запасной путь для WC без FILEX MOVE. RENAME (API 74) работает внутри
// каталога и занятое имя не заменяет, поэтому прежний файл сначала
// откладывается под именем WCUPD.OLD, копия получает имя файла, и лишь потом
// отложенный удаляется.
//
// Любой отказ RENAME здесь — исход неизвестен. Даже в WC Improved код отказа
// не доказывает, что записи целы: A=0 бывает и после того, как удаление
// прежней записи легло, а откат убрал новую. Без FILEX MOVE ядро, скорее
// всего, старее, и гарантий у него меньше. Поэтому после отказа ни одна
// копия не удаляется, и запись в этом сеансе прекращается. Обе копии —
// WCUPD.OLD и WCUPD.TMP — найдёт опись следующего запуска, и каталог
// останется только для чтения до проверки карты.
VfsFileStore::Replace VfsFileStore::renameReplace(const char* tempPath,
                                                  const char* finalPath,
                                                  const char* finalName,
                                                  const char* asidePath,
                                                  const char* relativeFile,
                                                  bool keepExisting) {
  VfsResult result;
  // Есть ли прежний файл: только ответ плагина "stat-1" — «нет». Если это
  // была ошибка чтения, а файл есть, RENAME копии на его имя откажет:
  // WC не создаёт запись с занятым именем.
  const bool exists =
      vfs(VfsOperation::kStat, finalPath, 0, result, kVfsNormalWaitMs);
  if (!exists && strcmp(result.error, "stat-1") != 0) {
    setError("SD: cannot stat file");
    return Replace::kUntouched;
  }
  if (exists && keepExisting) {
    setError("protected file is on SD");
    return Replace::kUntouched;
  }
  if (exists) {
    // Имя WCUPD.OLD должно быть свободно; остаток прошлой замены не трогаем.
    if (vfs(VfsOperation::kStat, asidePath, 0, result, kVfsNormalWaitMs)) {
      markLeftover(relativeFile);
      setError("%s", kLeftoverReason);
      return Replace::kUntouched;
    }
    if (strcmp(result.error, "stat-1") != 0) {
      setError("SD: cannot stat %s", kAsideName);
      return Replace::kUntouched;
    }
    if (!renameEntry(finalPath, kAsideName)) {
      setError("rename failed: CHKDSK");
      return Replace::kUnknown;
    }
  }
  if (!renameEntry(tempPath, finalName)) {
    if (exists) {
      // Попытка вернуть прежний файл на имя; исход всё равно неизвестен.
      renameEntry(asidePath, finalName);
    }
    setError("rename failed: CHKDSK");
    return Replace::kUnknown;
  }
  if (exists &&
      !quietVfs(VfsOperation::kDelete, asidePath, kVfsMutateWaitMs)) {
    // Прежний файл остался под именем WCUPD.OLD: следующая замена в этом
    // каталоге упёрлась бы в занятое имя — каталог только для чтения.
    markLeftover(relativeFile);
  }
  return Replace::kDone;
}

bool VfsFileStore::renameEntry(const char* oldPath, const char* newName) {
  VfsResult result;
  memset(&result, 0, sizeof(result));
  if (!bridge_.submitRename(oldPath, newName, false)) {
    setError("vfs busy");
    return false;
  }
  return awaitVfs(result, kVfsMutateWaitMs);
}

int VfsFileStore::directoryIndex(const char* dir, size_t length) const {
  for (size_t index = 0; index < directoryCount_; ++index) {
    if (strlen(directories_[index]) != length) {
      continue;
    }
    bool same = true;
    for (size_t position = 0; position < length && same; ++position) {
      const unsigned char a =
          static_cast<unsigned char>(directories_[index][position]);
      const unsigned char b = static_cast<unsigned char>(dir[position]);
      same = (a < 0x80 ? toupper(a) : a) == (b < 0x80 ? toupper(b) : b);
    }
    if (same) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

int VfsFileStore::directoryOf(const char* relativeFile) const {
  const char* slash = strrchr(relativeFile, '/');
  return directoryIndex(
      relativeFile,
      slash == nullptr ? 0 : static_cast<size_t>(slash - relativeFile));
}

bool VfsFileStore::leftoverIn(const char* relativeFile) const {
  const int folder = directoryOf(relativeFile);
  return folder >= 0 && leftover_[folder];
}

void VfsFileStore::markLeftover(const char* relativeFile) {
  const int folder = directoryOf(relativeFile);
  if (folder >= 0) {
    leftover_[folder] = true;
  }
  leftoverSeen_ = true;
}

// Остатки прерванной замены ищутся ещё и прямым STAT: листинг мог их
// пропустить — ошибка чтения посреди каталога для WC выглядит его концом.
// Отказ STAT иной, чем «нет такого», — опись недостоверна.
bool VfsFileStore::probeLeftovers(size_t index, const char* directoryPath) {
  const char* names[] = {kTempName, kAsideName};
  const size_t length = strlen(directoryPath);
  const bool slash = length != 0 && directoryPath[length - 1] == '/';
  for (const char* name : names) {
    char path[GitTreeEntry::kPathSize + 16];
    snprintf(path, sizeof(path), "%s%s%s", directoryPath, slash ? "" : "/",
             name);
    VfsResult result;
    if (vfs(VfsOperation::kStat, path, 0, result, kVfsNormalWaitMs)) {
      leftover_[index] = true;
      leftoverSeen_ = true;
    } else if (strcmp(result.error, "stat-1") != 0) {
      return false;
    }
  }
  return true;
}

bool VfsFileStore::quietVfs(VfsOperation operation, const char* path,
                            uint32_t timeoutMs) {
  char saved[kMaxError];
  memcpy(saved, error_, errorSize_);
  VfsResult result;
  const bool ok = vfs(operation, path, 0, result, timeoutMs);
  memcpy(error_, saved, errorSize_);
  return ok;
}

VfsFileStore::Scan VfsFileStore::scanDirectory(size_t index, Visitor visitor,
                                               void* context) {
  const char* directory = directories_[index];
  char path[GitTreeEntry::kPathSize + 2];
  localPath(directory, path, sizeof(path));
  VfsResult result;
  if (!vfs(VfsOperation::kOpenDirectory, path, 0, result, kVfsNormalWaitMs)) {
    // Z80 ответил отказом ("opendir-N") — каталога на SD нет, все его файлы
    // с GitHub новые. Сбой связи отсутствием каталога не считается: иначе
    // имеющийся защищённый wc.ini стал бы «новым» и получил право на замену.
    // Отказ Z80 ещё не доказывает отсутствие: STAT, что каталог есть, —
    // значит, это ошибка чтения, и опись недостоверна.
    // Отказ плагина — статус 1; иное (короткий ответ) — не доказательство.
    if (strcmp(result.error, "opendir-1") == 0) {
      VfsResult stat;
      if (!vfs(VfsOperation::kStat, path, 0, stat, kVfsNormalWaitMs) &&
          strcmp(stat.error, "stat-1") == 0) {
        return Scan::kDone;
      }
    }
    setError("SD: cannot read %s", path);
    return Scan::kFailed;
  }
  present_[index] = true;
  for (;;) {
    if (stopped()) {
      return Scan::kStopped;
    }
    if (!vfs(VfsOperation::kReadDirectory, nullptr, 0, result,
             kVfsNormalWaitMs)) {
      setError("SD: cannot read %s", path);
      return Scan::kFailed;
    }
    if (result.atEnd) {
      if (!probeLeftovers(index, path)) {
        setError("SD: cannot read %s", path);
        return Scan::kFailed;
      }
      return Scan::kDone;
    }
    if (result.isDirectory || strcmp(result.name, ".") == 0 ||
        strcmp(result.name, "..") == 0) {
      continue;
    }
    // Остаток прерванной замены в список не входит и не удаляется: запись в
    // этот каталог запрещена до проверки карты.
    if (equalNoCase(result.name, kTempName) ||
        equalNoCase(result.name, kAsideName)) {
      leftover_[index] = true;
      leftoverSeen_ = true;
      continue;
    }
    char relative[GitTreeEntry::kPathSize];
    const char* name = relative;
    if (directory[0] == 0) {
      snprintf(relative, sizeof(relative), "%s", result.name);
    } else if (snprintf(relative, sizeof(relative), "%s/%s", directory,
                        result.name) >= static_cast<int>(sizeof(relative))) {
      name = nullptr;
    }
    visitor(context, index, name, result);
  }
}

bool VfsFileStore::hashLocal(const char* path, uint32_t size,
                             uint8_t digest[Sha1::kDigestSize]) {
  VfsResult result;
  // SHA считается по длине с GitHub, поэтому сначала — длина самого файла:
  // верное начало с лишним хвостом не должно сойти за целый файл.
  if (!vfs(VfsOperation::kStat, path, 0, result, kVfsNormalWaitMs)) {
    return false;
  }
  if (result.isDirectory || result.size != size) {
    setError("SD size %lu, expected %lu",
             static_cast<unsigned long>(result.size),
             static_cast<unsigned long>(size));
    return false;
  }
  if (!vfs(VfsOperation::kResetBuffers, nullptr, 0, result,
           kVfsNormalWaitMs) ||
      !vfs(VfsOperation::kOpenRead, path, 0, result, kVfsNormalWaitMs)) {
    return false;
  }
  Sha1 sha;
  gitBlobBegin(sha, size);
  uint32_t done = 0;
  bool ok = true;
  while (done < size) {
    if (stopped()) {
      ok = false;
      break;
    }
    if (bridge_.vfsToNetworkAvailable() == 0) {
      const uint32_t wanted = static_cast<uint32_t>(
          size - done < VfsBridge::kMaxPumpPerRequest
              ? size - done
              : VfsBridge::kMaxPumpPerRequest);
      if (!vfs(VfsOperation::kRead, nullptr, wanted, result,
               kVfsNormalWaitMs) ||
          result.transferred == 0) {
        ok = false;
        break;
      }
    }
    const size_t available = bridge_.vfsToNetworkAvailable();
    const size_t wanted = available < sizeof(chunk_) ? available
                                                     : sizeof(chunk_);
    const size_t received = bridge_.readForNetwork(chunk_, wanted);
    if (received == 0 || done + received > size) {
      ok = false;
      break;
    }
    sha.update(chunk_, received);
    done += static_cast<uint32_t>(received);
    addProgress(static_cast<uint32_t>(received));
  }
  // После сбоя чтения закрытие — уборка: его отказ не подменяет причину.
  VfsResult closed;
  const bool closeOk =
      ok ? vfs(VfsOperation::kCloseCommit, nullptr, 0, closed, kVfsCloseWaitMs)
         : quietVfs(VfsOperation::kCloseCommit, nullptr, kVfsCloseWaitMs);
  if (!ok || !closeOk) {
    return false;
  }
  sha.finish(digest);
  return true;
}

bool VfsFileStore::writeLocal(const char* path, const uint8_t* data,
                              uint32_t size, bool& left) {
  left = false;
  VfsResult result;
  if (!vfs(VfsOperation::kResetBuffers, nullptr, 0, result,
           kVfsNormalWaitMs) ||
      !vfs(VfsOperation::kOpenWrite, path, 0, result, kVfsMutateWaitMs)) {
    // Файл не открыт: созданного здесь нет, а чужой с этим именем плагин
    // не тронул — удалять нечего.
    return false;
  }
  left = true;
  uint32_t offset = 0;
  bool ok = true;
  const size_t window = VfsBridge::kMaxPumpPerRequest < bridge_.ringCapacity()
                            ? VfsBridge::kMaxPumpPerRequest
                            : bridge_.ringCapacity();
  while (ok && (offset < size || bridge_.networkToVfsAvailable() != 0)) {
    if (stopped()) {
      ok = false;
      break;
    }
    while (offset < size && bridge_.networkToVfsFree() != 0) {
      const size_t free = bridge_.networkToVfsFree();
      const size_t part = size - offset < free ? size - offset : free;
      const size_t written = bridge_.writeFromNetwork(data + offset, part);
      if (written == 0) {
        break;
      }
      offset += static_cast<uint32_t>(written);
    }
    const size_t queued = bridge_.networkToVfsAvailable();
    if (queued < window && offset < size) {
      continue;  // кольцо ещё не набрало окно: докладываем данные
    }
    const uint32_t wanted = static_cast<uint32_t>(
        queued < VfsBridge::kMaxPumpPerRequest
            ? queued
            : VfsBridge::kMaxPumpPerRequest);
    if (!vfs(VfsOperation::kWrite, nullptr, wanted, result,
             kVfsMutateWaitMs) ||
        result.transferred == 0) {
      ok = false;
    } else {
      addProgress(result.transferred);
    }
  }
  if (ok && vfs(VfsOperation::kCloseCommit, nullptr, 0, result,
                kVfsCloseWaitMs)) {
    return true;
  }
  // Недописанная копия — своя (её создал OPEN выше): убрать. Отказы уборки
  // не подменяют причину сбоя в error.
  quietVfs(VfsOperation::kCloseAbort, nullptr, kVfsCloseWaitMs);
  left = !quietVfs(VfsOperation::kDelete, path, kVfsMutateWaitMs);
  return false;
}

bool VfsFileStore::ensureDirectory(const char* relativeFile) {
  char partial[GitTreeEntry::kPathSize + 2];
  const char* slash = strchr(relativeFile, '/');
  while (slash != nullptr) {
    const size_t length = static_cast<size_t>(slash - relativeFile);
    snprintf(partial, sizeof(partial), "/%.*s", static_cast<int>(length),
             relativeFile);
    const int folder = directoryIndex(relativeFile, length);
    VfsResult result;
    if (vfs(VfsOperation::kStat, partial, 0, result, kVfsNormalWaitMs)) {
      if (!result.isDirectory) {
        setError("%s is a file", partial);
        return false;
      }
      // Опись сочла каталог отсутствующим, а он есть: чтение карты сбоит, и
      // остаток прошлой замены в нём мог остаться незамеченным. Писать нельзя.
      if (folder >= 0 && !present_[folder]) {
        diskSuspect_ = true;
        setError("SD read errors: CHKDSK");
        return false;
      }
    } else if (!vfs(VfsOperation::kMkdir, partial, 0, result,
                    kVfsMutateWaitMs)) {
      setError("cannot create %s", partial);
      return false;
    } else if (folder >= 0) {
      present_[folder] = true;  // создан сейчас: остатков в нём нет
    }
    slash = strchr(slash + 1, '/');
  }
  return true;
}

void VfsFileStore::installFile(const char* relativeFile,
                               const uint8_t sha[Sha1::kDigestSize],
                               uint32_t size, bool keepExisting,
                               uint8_t* buffer, Download download,
                               void* context, Install& install) {
  install = Install();
  char finalPath[GitTreeEntry::kPathSize + 2];
  char tempPath[GitTreeEntry::kPathSize + 16];
  char asidePath[GitTreeEntry::kPathSize + 16];
  localPath(relativeFile, finalPath, sizeof(finalPath));
  const char* lastSlash = strrchr(relativeFile, '/');
  if (lastSlash == nullptr) {
    snprintf(tempPath, sizeof(tempPath), "/%s", kTempName);
    snprintf(asidePath, sizeof(asidePath), "/%s", kAsideName);
  } else {
    const int folder = static_cast<int>(lastSlash - relativeFile);
    snprintf(tempPath, sizeof(tempPath), "/%.*s/%s", folder, relativeFile,
             kTempName);
    snprintf(asidePath, sizeof(asidePath), "/%.*s/%s", folder, relativeFile,
             kAsideName);
  }
  const char* finalName = lastSlash == nullptr ? relativeFile : lastSlash + 1;

  // Причина неудачи запоминается сразу, до уборки: отказ удаления копии её
  // не подменит.
  char* reason = install.reason;
  auto note = [this, reason](const char* text) {
    snprintf(reason, kReasonSize, "%s", stopped() ? "stopped" : text);
  };
  auto errorOr = [this](const char* fallback) -> const char* {
    return error_[0] != 0 ? error_ : fallback;
  };

  bool& done = install.done;
  // Два полных круга: если после замены файл на SD не сошёлся, он
  // скачивается и пишется заново.
  for (unsigned round = 0; round < 2 && !done && !stopped(); ++round) {
    if (keepExisting) {
      // Защищённый файл ставится, только если его нет. Есть — значит, он
      // уже стоит (прошлый круг поставил его, а проверка чтением отказала,
      // или опись его не увидела): только сверить, не перезаписывая.
      VfsResult stat;
      if (vfs(VfsOperation::kStat, finalPath, 0, stat, kVfsNormalWaitMs)) {
        install.keptThere = true;
        install.keptSize = stat.size;
        uint8_t digest[Sha1::kDigestSize];
        done = !stat.isDirectory && stat.size == size &&
               hashLocal(finalPath, size, digest) &&
               memcmp(digest, sha, sizeof(digest)) == 0;
        break;
      }
      if (strcmp(stat.error, "stat-1") != 0) {
        note("SD: cannot stat file");
        break;
      }
    }
    // Остаток мог появиться и на прошлом круге (не удалилась копия).
    if (leftoverIn(relativeFile)) {
      note(kLeftoverReason);
      break;
    }
    error_[0] = 0;
    if (!download(context, buffer) || !ensureDirectory(relativeFile)) {
      note(errorOr("download failed"));
      break;
    }
    // Имя копии должно быть свободно. Остаток прошлой замены не удаляется
    // (он может делить цепочку с файлом): каталог — только для чтения.
    VfsResult probe;
    if (vfs(VfsOperation::kStat, tempPath, 0, probe, kVfsNormalWaitMs)) {
      markLeftover(relativeFile);
      note(kLeftoverReason);
      break;
    }
    if (strcmp(probe.error, "stat-1") != 0) {
      note("SD: cannot stat copy");
      break;
    }
    bool staged = false;
    bool ours = false;  // на SD лежит копия, созданная здесь
    for (unsigned attempt = 1; attempt <= kWriteAttempts && !stopped();
         ++attempt) {
      error_[0] = 0;
      if (!writeLocal(tempPath, buffer, size, ours)) {
        // OPEN отказал, хотя STAT имени не нашёл: оно занято (ошибка чтения
        // скрыла остаток) либо каталог не пишется — копию не создать.
        note(strncmp(error_, "open-", 5) == 0
                 ? "cannot create WCUPD.TMP: CHKDSK"
                 : errorOr("SD copy: write failed"));
        if (ours) {
          break;  // своя недописанная копия не удалилась
        }
        continue;
      }
      uint8_t digest[Sha1::kDigestSize];
      error_[0] = 0;
      if (!hashLocal(tempPath, size, digest)) {
        note(errorOr("SD copy: read failed"));
      } else if (memcmp(digest, sha, sizeof(digest)) != 0) {
        note("SD copy: SHA mismatch");
      } else {
        staged = true;
        break;
      }
      // Копия своя (занятое имя OPEN на запись не берёт): её можно удалить.
      if (!quietVfs(VfsOperation::kDelete, tempPath, kVfsMutateWaitMs)) {
        break;
      }
      ours = false;
    }
    if (!staged) {
      if (ours) {
        markLeftover(relativeFile);  // своя копия осталась: только чтение
      }
      break;
    }
    error_[0] = 0;
    const Replace replaced = vfsReplace(tempPath, finalPath, finalName,
                                        asidePath, relativeFile, keepExisting);
    if (replaced == Replace::kUntouched) {
      note(errorOr("replace refused"));
      if (!quietVfs(VfsOperation::kDelete, tempPath, kVfsMutateWaitMs)) {
        markLeftover(relativeFile);
        break;
      }
      if (keepExisting) {
        continue;  // защищённый файл мог найтись на SD: круг его сверит
      }
      break;
    }
    install.installed = install.installed || replaced == Replace::kDone;
    const bool unknown = replaced == Replace::kUnknown;
    if (unknown) {
      note(errorOr("replace failed: CHKDSK"));
    }
    // Итог — по самой карте, и при неизвестном исходе тоже: файл мог встать.
    uint8_t digest[Sha1::kDigestSize];
    error_[0] = 0;
    const bool read = hashLocal(finalPath, size, digest);
    const bool good = read && memcmp(digest, sha, sizeof(digest)) == 0;
    if (unknown) {
      // Верный файл на месте засчитывается, но копии не трогаем и в этом
      // сеансе больше не пишем — нужна проверка карты.
      done = good;
      diskSuspect_ = true;
      break;
    }
    if (good) {
      done = true;
    } else {
      note(read ? "SD file: SHA mismatch" : errorOr("SD file: read failed"));
    }
  }
  if (!done && reason[0] == 0) {
    snprintf(reason, kReasonSize, "%s", stopped() ? "stopped" : "not updated");
  }
}

}  // namespace zifi
