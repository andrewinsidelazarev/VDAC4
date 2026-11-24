#include "zifi/app_data_files.hpp"

#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "zifi/resource_disk_cache.hpp"

#ifdef _WIN32
#include <direct.h>
#include <sys/utime.h>
#else
#include <unistd.h>
#include <utime.h>
#endif

namespace zifi {

namespace {

#ifdef _WIN32
// Тест на ПК собирается MSVC: те же вызовы под своими именами (как в resource_disk_cache.cpp).
int makeDir(const char* path) { return _mkdir(path); }
int setFileTime(const char* path, time_t when) {
  struct __utimbuf64 times = {when, when};
  return _utime64(path, &times);
}
long long fileSize(const char* path) {
  struct _stat64 info;
  return _stat64(path, &info) == 0 ? info.st_size : -1;
}
#else
int makeDir(const char* path) { return mkdir(path, 0777); }
int setFileTime(const char* path, time_t when) {
  struct utimbuf times = {when, when};
  return utime(path, &times);
}
long long fileSize(const char* path) {
  struct stat info;
  return stat(path, &info) == 0 ? static_cast<long long>(info.st_size) : -1;
}
#endif

bool nameChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '-';
}

// Имя в верхний регистр (на карте Evo и в MOD_QUERY имена 8.3 — заглавными; FatFs регистр не различает,
// но папка и файл на флешке пусть называются одинаково при любом запросе).
void upper(const char* in, char* out, size_t size) {
  size_t i = 0;
  for (; in[i] != 0 && i + 1 < size; ++i) {
    char c = in[i];
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
    out[i] = c;
  }
  out[i] = 0;
}

}  // namespace

AppDataFiles::AppDataFiles(const char* root) {
  snprintf(root_, sizeof(root_), "%s", root);
}

bool AppDataFiles::validApp(const char* app) {
  size_t n = 0;
  for (; app != nullptr && app[n] != 0; ++n) {
    if (n >= 8 || !nameChar(app[n])) {
      return false;
    }
  }
  return n > 0;
}

bool AppDataFiles::validName(const char* name) {
  if (name == nullptr) {
    return false;
  }
  size_t base = 0, ext = 0;
  bool dot = false;
  for (const char* p = name; *p != 0; ++p) {
    if (*p == '.') {
      if (dot || base == 0) {
        return false;
      }
      dot = true;
    } else if (!nameChar(*p)) {
      return false;
    } else if (dot) {
      if (++ext > 3) {
        return false;
      }
    } else if (++base > 8) {
      return false;
    }
  }
  return base > 0 && strcmp(name, kTempName) != 0;
}

bool AppDataFiles::folderOf(const char* app, char* out, size_t size) const {
  char name[9];
  if (!validApp(app)) {
    return false;
  }
  upper(app, name, sizeof(name));
  return snprintf(out, size, "%s/%s", root_, name) < static_cast<int>(size);
}

uint8_t AppDataFiles::begin(const char* app, const char* name, uint32_t size, uint32_t stamp,
                            uint64_t freeBytes) {
  drop();
  char dir[260];
  char file[13];
  if (!validName(name) || size == 0 || !folderOf(app, dir, sizeof(dir))) {
    return kBadName;
  }
  upper(name, file, sizeof(file));
  // Годная копия (та же длина, не старее файла на карте) — передавать не нужно; правило то же, что у
  // кэша музыки и ядер (ResourceDiskCache::fresh).
  if (ResourceDiskCache(dir).fresh(file, size, stamp)) {
    return kHave;
  }
  char path[300];
  snprintf(path, sizeof(path), "%s/%s", dir, file);
  const long long old = fileSize(path);       // прежняя копия уйдёт при COMMIT — её место в счёт
  if (static_cast<uint64_t>(size) > freeBytes + static_cast<uint64_t>(old > 0 ? old : 0)) {
    return kNoRoom;
  }
  makeDir(root_);                              // на плате root уже есть (usb_drive), на ПК — нет
  makeDir(dir);
  char temp[300];
  snprintf(temp, sizeof(temp), "%s/%s", dir, kTempName);
  out_ = fopen(temp, "wb");
  if (out_ == nullptr) {
    return kNoRoom;
  }
  setvbuf(out_, nullptr, _IONBF, 0);           // куски и так по 4 КБ: своего буфера stdio не нужно
  snprintf(outDir_, sizeof(outDir_), "%s", dir);
  memcpy(outName_, file, sizeof(outName_));
  outSize_ = size;
  outStamp_ = stamp;
  received_ = 0;
  return kSend;
}

bool AppDataFiles::put(uint32_t offset, const uint8_t* data, uint32_t length) {
  if (out_ == nullptr || offset != received_ || length > outSize_ - received_) {
    return false;
  }
  for (uint32_t done = 0; done < length;) {
    const uint32_t part = length - done < kChunkBytes ? length - done : kChunkBytes;
    if (fwrite(data + done, 1, part, out_) != part) {
      return false;
    }
    done += part;
  }
  received_ += length;
  return true;
}

bool AppDataFiles::commit() {
  if (out_ == nullptr) {
    return false;
  }
  const bool all = received_ == outSize_;
  const bool closed = fclose(out_) == 0;
  out_ = nullptr;
  char temp[300];
  char path[300];
  snprintf(temp, sizeof(temp), "%s/%s", outDir_, kTempName);
  snprintf(path, sizeof(path), "%s/%s", outDir_, outName_);
  if (!all || !closed) {
    remove(temp);
    return false;
  }
  {
    // Файл, который приложение держит открытым, не переименовать поверх: закрыть его дескриптор.
    std::lock_guard<std::mutex> guard(lock_);
    closeCached();
  }
  remove(path);                                // FatFs не переименовывает поверх существующего файла
  if (rename(temp, path) != 0) {
    remove(temp);
    return false;
  }
  // Дата FAT без месяца или числа негодна — у копии остаётся время записи (как у ResourceDiskCache).
  const uint32_t date = outStamp_ >> 16;
  if (((date >> 5) & 0x0F) == 0 || (date & 0x1F) == 0) {
    return true;
  }
  return setFileTime(path, ResourceDiskCache::timeOf(outStamp_)) == 0;
}

void AppDataFiles::drop() {
  if (out_ == nullptr) {
    return;
  }
  fclose(out_);
  out_ = nullptr;
  char temp[300];
  snprintf(temp, sizeof(temp), "%s/%s", outDir_, kTempName);
  remove(temp);
  received_ = 0;
}

int AppDataFiles::open(const char* app, const char* name, void** file, uint32_t* size) {
  if (file != nullptr) {
    *file = nullptr;
  }
  if (size != nullptr) {
    *size = 0;
  }
  char dir[260];
  char upperName[13];
  if (file == nullptr || !validName(name) || !folderOf(app, dir, sizeof(dir))) {
    return kNotFound;
  }
  upper(name, upperName, sizeof(upperName));
  // Файл на учёте (флешки не было, 0.1.166) — читается у программы Z80, на флешку не смотрим.
  char upperApp[9];
  upper(app, upperApp, sizeof(upperApp));
  {
    std::lock_guard<std::mutex> guard(lock_);
    for (const Remote& remote : remotes_) {
      if (!remote.used || strcmp(remote.app, upperApp) != 0 || strcmp(remote.name, upperName) != 0) {
        continue;
      }
      if (fetch_ == nullptr || block_ == nullptr) {
        return kNotFound;
      }
      for (Handle& handle : handles_) {
        if (!handle.used) {
          handle = Handle{};
          handle.used = true;
          handle.remote = true;
          handle.remoteIndex = remote.index;
          handle.size = remote.size;
          *file = &handle;
          if (size != nullptr) {
            *size = handle.size;
          }
          return kOk;
        }
      }
      return kError;                           // все kMaxOpen дескрипторов заняты
    }
  }
  char path[300];
  snprintf(path, sizeof(path), "%s/%s", dir, upperName);
  const long long length = fileSize(path);
  if (length < 0 || length > 0xFFFFFFFFLL) {
    return kNotFound;
  }
  std::lock_guard<std::mutex> guard(lock_);
  for (size_t i = 0; i < kMaxOpen; ++i) {
    if (!handles_[i].used) {
      handles_[i] = Handle{};
      handles_[i].used = true;
      handles_[i].size = static_cast<uint32_t>(length);
      snprintf(handles_[i].path, sizeof(handles_[i].path), "%s", path);
      *file = &handles_[i];
      if (size != nullptr) {
        *size = handles_[i].size;
      }
      return kOk;
    }
  }
  return kError;                               // все kMaxOpen дескрипторов заняты
}

int AppDataFiles::read(void* file, uint32_t offset, void* data, uint32_t count, uint32_t* got) {
  if (got != nullptr) {
    *got = 0;
  }
  Handle* handle = static_cast<Handle*>(file);
  if (handle == nullptr) {
    return kError;
  }
  const int index = static_cast<int>(handle - handles_);
  if (index < 0 || index >= static_cast<int>(kMaxOpen) || !handle->used ||
      (data == nullptr && count != 0)) {
    return kError;
  }
  if (offset >= handle->size || count == 0) {
    return kOk;                                // за концом файла — 0 байтов, не ошибка
  }
  if (count > handle->size - offset) {
    count = handle->size - offset;
  }
  if (handle->remote) {
    uint8_t remoteIndex = 0;
    {
      std::lock_guard<std::mutex> guard(lock_);
      if (!handle->used || !handle->remote) {
        return kError;                         // дескриптор закрыли, пока мы шли сюда
      }
      remoteIndex = handle->remoteIndex;
    }
    return readRemote(remoteIndex, handle->size, offset, static_cast<uint8_t*>(data), count, got);
  }
  std::lock_guard<std::mutex> guard(lock_);
  if (cachedIndex_ != index) {
    closeCached();
    cached_ = fopen(handle->path, "rb");
    if (cached_ == nullptr) {
      return kError;                           // флешку вынули или файл стёрли
    }
    setvbuf(cached_, nullptr, _IONBF, 0);
    cachedIndex_ = index;
  }
  if (fseek(cached_, static_cast<long>(offset), SEEK_SET) != 0) {
    closeCached();
    return kError;
  }
  uint8_t* out = static_cast<uint8_t*>(data);
  uint32_t done = 0;
  while (done < count) {
    const uint32_t part = count - done < kChunkBytes ? count - done : kChunkBytes;
    const size_t n = fread(out + done, 1, part, cached_);
    done += static_cast<uint32_t>(n);
    if (n != part) {
      closeCached();
      if (got != nullptr) {
        *got = done;
      }
      return kError;
    }
  }
  if (got != nullptr) {
    *got = done;
  }
  return kOk;
}

void AppDataFiles::close(void* file) {
  Handle* handle = static_cast<Handle*>(file);
  if (handle == nullptr) {
    return;
  }
  const int index = static_cast<int>(handle - handles_);
  if (index < 0 || index >= static_cast<int>(kMaxOpen)) {
    return;
  }
  std::lock_guard<std::mutex> guard(lock_);
  if (cachedIndex_ == index) {
    closeCached();
  }
  handles_[index] = Handle{};
}

void AppDataFiles::closeAll() {
  std::lock_guard<std::mutex> guard(lock_);
  closeCached();
  for (Handle& handle : handles_) {
    handle = Handle{};
  }
}

void AppDataFiles::closeCached() {
  if (cached_ != nullptr) {
    fclose(cached_);
  }
  cached_ = nullptr;
  cachedIndex_ = -1;
}

// --- файлы на карте Evo: чтение по запросу у программы Z80 (0.1.166) -------------------------------

bool AppDataFiles::addRemote(const char* app, const char* name, uint32_t size, uint32_t stamp,
                             uint8_t index) {
  if (!validApp(app) || !validName(name) || size == 0) {
    return false;
  }
  char upperApp[9];
  char upperName[13];
  upper(app, upperApp, sizeof(upperApp));
  upper(name, upperName, sizeof(upperName));
  std::lock_guard<std::mutex> guard(lock_);
  Remote* slot = nullptr;
  for (Remote& remote : remotes_) {            // тот же файл приложения — на его место
    if (remote.used && strcmp(remote.app, upperApp) == 0 && strcmp(remote.name, upperName) == 0) {
      slot = &remote;
      break;
    }
  }
  for (int pass = 0; pass < 2 && slot == nullptr; ++pass) {
    if (pass == 1) {
      // Учёт полон: записи других приложений — от прежних запусков, их программы Z80 уже не отвечают.
      for (Remote& remote : remotes_) {
        if (strcmp(remote.app, upperApp) != 0) {
          remote = Remote{};
        }
      }
    }
    for (Remote& remote : remotes_) {
      if (slot == nullptr && !remote.used) {
        slot = &remote;
      }
    }
  }
  if (slot == nullptr) {
    return false;                              // у одного приложения больше kMaxRemote файлов
  }
  *slot = Remote{};
  slot->used = true;
  slot->index = index;
  slot->size = size;
  slot->stamp = stamp;
  memcpy(slot->app, upperApp, sizeof(slot->app));
  memcpy(slot->name, upperName, sizeof(slot->name));
  generation_.fetch_add(1);
  return true;
}

void AppDataFiles::forgetRemote(const char* app, const char* name) {
  if (!validApp(app) || !validName(name)) {
    return;
  }
  char upperApp[9];
  char upperName[13];
  upper(app, upperApp, sizeof(upperApp));
  upper(name, upperName, sizeof(upperName));
  std::lock_guard<std::mutex> guard(lock_);
  for (Remote& remote : remotes_) {
    if (remote.used && strcmp(remote.app, upperApp) == 0 && strcmp(remote.name, upperName) == 0) {
      remote = Remote{};
      generation_.fetch_add(1);
    }
  }
}

void AppDataFiles::clearRemote() {
  std::lock_guard<std::mutex> guard(lock_);
  for (Remote& remote : remotes_) {
    remote = Remote{};
  }
  generation_.fetch_add(1);
}

void AppDataFiles::setFetch(Fetch fetch, void* context, uint8_t* block) {
  std::lock_guard<std::mutex> remoteGuard(remoteLock_);
  std::lock_guard<std::mutex> guard(lock_);
  fetch_ = fetch;
  fetchContext_ = context;
  block_ = block;
  blockIndex_ = -1;
}

// count байтов с offset (уже не за концом файла длиной size). Программа Z80 читает карту целыми
// секторами, поэтому у неё просим только с начала сектора:
//   - что уже есть в блоке (прошлое чтение) — оттуда;
//   - с начала сектора и не меньше сектора — прямо в буфер приложения, до kRemoteRequestMax байтов
//     за запрос, целыми секторами (кроме хвоста файла) — без лишнего копирования;
//   - иначе (начало посреди сектора, короткое чтение) — блок kRemoteBlock с начала этого сектора, из
//     него — нужное; следующие короткие чтения рядом (каталог pak, заголовки) возьмут остальное.
int AppDataFiles::readRemote(uint8_t index, uint32_t size, uint32_t offset, uint8_t* out,
                             uint32_t count, uint32_t* got) {
  std::lock_guard<std::mutex> guard(remoteLock_);
  if (fetch_ == nullptr || block_ == nullptr) {
    return kError;
  }
  const uint32_t generation = generation_.load();
  uint32_t done = 0;
  while (done < count) {
    const uint32_t at = offset + done;
    const uint32_t left = count - done;
    if (blockIndex_ == static_cast<int>(index) && blockGeneration_ == generation && at >= blockAt_ &&
        at - blockAt_ < blockLength_) {
      uint32_t n = blockLength_ - (at - blockAt_);
      if (n > left) {
        n = left;
      }
      memcpy(out + done, block_ + (at - blockAt_), n);
      done += n;
      continue;
    }
    if (at % kSectorBytes == 0 && left >= kSectorBytes) {
      uint32_t n = left < kRemoteRequestMax ? left : kRemoteRequestMax;
      if (at + n != size) {
        n -= n % kSectorBytes;                 // целые секторы; хвост файла — как есть
      }
      if (!fetch_(fetchContext_, index, at, out + done, n)) {
        break;
      }
      done += n;
      continue;
    }
    const uint32_t blockAt = at - at % kSectorBytes;
    const uint32_t length = size - blockAt < kRemoteBlock ? size - blockAt : kRemoteBlock;
    blockIndex_ = -1;                          // пока куски идут, блок негоден
    if (!fetch_(fetchContext_, index, blockAt, block_, length)) {
      break;
    }
    blockIndex_ = index;
    blockGeneration_ = generation;
    blockAt_ = blockAt;
    blockLength_ = length;
  }
  if (got != nullptr) {
    *got = done;
  }
  return done == count ? kOk : kError;
}

void AppDataRequest::event(uint8_t* out, uint8_t seq, uint8_t index, uint32_t offset, uint32_t count) {
  out[0] = seq;
  out[1] = index;
  for (int i = 0; i < 4; ++i) {
    out[2 + i] = static_cast<uint8_t>(offset >> (8 * i));
  }
  out[6] = static_cast<uint8_t>(count);
  out[7] = static_cast<uint8_t>(count >> 8);
}

void AppDataRequest::begin(uint8_t seq, uint32_t offset, uint32_t count, uint8_t* dest) {
  active_ = count != 0 && dest != nullptr;
  failed_ = false;
  seq_ = seq;
  offset_ = offset;
  count_ = count;
  received_ = 0;
  dest_ = dest;
}

// Кусок принимается, только если он свой (номер запроса), идёт ровно за принятым и не выходит за
// длину запроса: повтор куска, чужой или запоздавший — мимо (kIgnored).
AppDataRequest::Result AppDataRequest::piece(const uint8_t* body, size_t length) {
  if (!active_ || body == nullptr || length < kPiecePrefix || body[0] != seq_) {
    return kIgnored;
  }
  const uint32_t at = static_cast<uint32_t>(body[1]) | static_cast<uint32_t>(body[2]) << 8 |
                     static_cast<uint32_t>(body[3]) << 16 | static_cast<uint32_t>(body[4]) << 24;
  const size_t n = length - kPiecePrefix;
  if (at != offset_ + received_) {
    return kIgnored;
  }
  if (n == 0) {                                // программа Z80 сектор не прочитала
    active_ = false;
    failed_ = true;
    return kFailed;
  }
  if (n > count_ - received_) {
    return kIgnored;
  }
  memcpy(dest_ + received_, body + kPiecePrefix, n);
  received_ += static_cast<uint32_t>(n);
  if (received_ < count_) {
    return kTaken;
  }
  active_ = false;
  return kDone;
}

}  // namespace zifi
