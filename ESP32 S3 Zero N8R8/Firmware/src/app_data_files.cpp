#include "zifi/app_data_files.hpp"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "zifi/resource_disk_cache.hpp"

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <sys/utime.h>
#else
#include <dirent.h>
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
// Длина обычного файла (-1 — нет или это папка: папка с именем PAK0.PAK файлом не считается).
long long fileSize(const char* path) {
  struct _stat64 info;
  return _stat64(path, &info) == 0 && (info.st_mode & _S_IFREG) != 0 ? info.st_size : -1;
}
// Вложенные папки каталога dir: each(context, имя) на каждую.
void forEachSubdir(const char* dir, void (*each)(void*, const char*), void* context) {
  char pattern[260];
  if (snprintf(pattern, sizeof(pattern), "%s/*", dir) >= static_cast<int>(sizeof(pattern))) {
    return;
  }
  struct _finddata_t info;
  const intptr_t find = _findfirst(pattern, &info);
  if (find == -1) {
    return;
  }
  do {
    if ((info.attrib & _A_SUBDIR) != 0 && strcmp(info.name, ".") != 0 && strcmp(info.name, "..") != 0) {
      each(context, info.name);
    }
  } while (_findnext(find, &info) == 0);
  _findclose(find);
}
#else
int makeDir(const char* path) { return mkdir(path, 0777); }
int setFileTime(const char* path, time_t when) {
  struct utimbuf times = {when, when};
  return utime(path, &times);
}
long long fileSize(const char* path) {
  struct stat info;
  return stat(path, &info) == 0 && S_ISREG(info.st_mode) ? static_cast<long long>(info.st_size) : -1;
}
// Вложенные папки каталога dir (на плате — VFS FatFs: d_type у записи есть).
void forEachSubdir(const char* dir, void (*each)(void*, const char*), void* context) {
  DIR* folder = opendir(dir);
  if (folder == nullptr) {
    return;
  }
  while (struct dirent* entry = readdir(folder)) {
    if (entry->d_type == DT_DIR && strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
      each(context, entry->d_name);
    }
  }
  closedir(folder);
}
#endif

bool nameChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '-';
}

// Порядок папок поиска: по путям без учёта регистра, «/» — меньше любого знака. Так папка идёт сразу за
// своей родительской, раньше соседних (A, A/B, A B — а не A, A B, A/B).
int folderOrder(const void* a, const void* b) {
  const unsigned char* x = static_cast<const unsigned char*>(a);
  const unsigned char* y = static_cast<const unsigned char*>(b);
  for (;; ++x, ++y) {
    unsigned cx = *x == '/' ? 1u : (*x >= 'a' && *x <= 'z' ? *x - 'a' + 'A' : *x);
    unsigned cy = *y == '/' ? 1u : (*y >= 'a' && *y <= 'z' ? *y - 'a' + 'A' : *y);
    if (cx != cy || cx == 0) {
      return static_cast<int>(cx) - static_cast<int>(cy);
    }
  }
}

bool samePath(const char* a, const char* b) { return folderOrder(a, b) == 0; }

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

// --- поиск на флешке (0.1.175) ----------------------------------------------------------------------

void AppDataFiles::setVolume(uint32_t stamp) { volume_.store(stamp); }

void AppDataFiles::setLog(Log log, void* context) {
  log_ = log;
  logContext_ = context;
}

void AppDataFiles::log(const char* format, ...) {
  if (log_ == nullptr) {
    return;
  }
  char line[400];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  log_(logContext_, line);
}

void AppDataFiles::resetSearch() {
  searching_ = false;
  searchApp_[0] = 0;
  dirCount_ = 0;
  dirsListed_ = false;
  home_ = -1;
  knownCount_ = 0;
}

// Запомненное годно для приложения upperApp и нынешнего тома? Нет — начать заново: папка приложения —
// первая. false — путь папки приложения длиннее kDirPath.
bool AppDataFiles::prepareSearch(const char* upperApp) {
  const uint32_t volume = volume_.load();
  if (resetPending_.exchange(false)) {
    resetSearch();
  }
  if (searching_ && searchVolume_ == volume && strcmp(searchApp_, upperApp) == 0) {
    return true;
  }
  resetSearch();
  if (snprintf(dirs_, kDirPath, "%s/%s", root_, upperApp) >= static_cast<int>(kDirPath)) {
    return false;
  }
  dirCount_ = 1;
  snprintf(searchApp_, sizeof(searchApp_), "%s", upperApp);
  searchVolume_ = volume;
  searching_ = true;
  return true;
}

// Все папки <root> на глубину kMaxScanDepth (сам <root> — тоже), кроме папки приложения (она уже
// первая): сначала собрать по уровням, потом расставить по folderOrder.
void AppDataFiles::listDirs() {
  if (dirsListed_) {
    return;
  }
  dirsListed_ = true;
  if (snprintf(dirs_ + dirCount_ * kDirPath, kDirPath, "%s", root_) >= static_cast<int>(kDirPath)) {
    return;
  }
  const size_t first = dirCount_++;
  const size_t rootLength = strlen(root_);
  struct Walk {
    AppDataFiles* self;
    size_t parent;
  };
  for (size_t i = first; i < dirCount_; ++i) {
    int depth = 0;
    for (const char* c = dirAt(i) + rootLength; *c != 0; ++c) {
      depth += *c == '/' ? 1 : 0;
    }
    if (depth >= kMaxScanDepth) {
      continue;
    }
    Walk walk{this, i};
    forEachSubdir(dirAt(i), [](void* context, const char* child) {
      Walk* w = static_cast<Walk*>(context);
      AppDataFiles* s = w->self;
      if (s->dirCount_ >= kMaxScanDirs) {
        return;
      }
      char* out = s->dirs_ + s->dirCount_ * kDirPath;
      if (snprintf(out, kDirPath, "%s/%s", s->dirAt(w->parent), child) >= static_cast<int>(kDirPath) ||
          samePath(out, s->dirAt(0))) {
        return;                                // длинный путь не берём; папка приложения — уже первая
      }
      ++s->dirCount_;
    }, &walk);
  }
  qsort(dirs_ + first * kDirPath, dirCount_ - first, kDirPath, folderOrder);
}

bool AppDataFiles::findOnDrive(const char* app, const char* name, char* path, size_t size,
                               uint32_t* length, bool* fresh) {
  if (fresh != nullptr) {
    *fresh = false;
  }
  if (!validApp(app) || !validName(name) || path == nullptr || size == 0) {
    return false;
  }
  char upperApp[9];
  char upperName[13];
  upper(app, upperApp, sizeof(upperApp));
  upper(name, upperName, sizeof(upperName));
  std::lock_guard<std::mutex> guard(searchLock_);
  if (!prepareSearch(upperApp)) {
    return false;
  }
  const Known* known = nullptr;
  for (size_t i = 0; i < knownCount_; ++i) {
    if (strcmp(known_[i].name, upperName) == 0) {
      known = &known_[i];
      break;
    }
  }
  Known found;
  if (known == nullptr) {
    snprintf(found.name, sizeof(found.name), "%s", upperName);
    if (searchVolume_ != 0) {
      char candidate[kDirPath + 16];
      auto tryDir = [&](size_t dir) {
        snprintf(candidate, sizeof(candidate), "%s/%s", dirAt(dir), upperName);
        const long long bytes = fileSize(candidate);
        if (bytes < 0 || bytes > 0xFFFFFFFFLL) {
          return false;
        }
        found.dir = static_cast<int>(dir);
        found.length = static_cast<uint32_t>(bytes);
        return true;
      };
      if (home_ >= 0) {
        tryDir(static_cast<size_t>(home_));    // домашняя папка есть — только в ней
      } else if (!tryDir(0)) {
        listDirs();
        for (size_t dir = 1; dir < dirCount_ && !tryDir(dir); ++dir) {
        }
      }
      if (found.dir >= 0 && home_ < 0) {
        home_ = found.dir;
        log("data files of %s: USB drive folder %s", upperApp, dirAt(static_cast<size_t>(home_)));
      }
    }
    if (knownCount_ < kMaxKnown) {
      known_[knownCount_++] = found;
    }
    known = &found;
    if (fresh != nullptr) {
      *fresh = true;
    }
  }
  if (known->dir < 0) {
    return false;
  }
  if (snprintf(path, size, "%s/%s", dirAt(static_cast<size_t>(known->dir)), upperName) >=
      static_cast<int>(size)) {
    return false;
  }
  if (length != nullptr) {
    *length = known->length;
  }
  return true;
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
  {
    std::lock_guard<std::mutex> guard(searchLock_);
    resetSearch();                             // на флешке новый файл: запомненное «нет» уже неверно
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
  char upperApp[9];
  upper(app, upperApp, sizeof(upperApp));
  // 0.1.175: файл на флешке (в папке приложения или любой вложенной в <root>) — читается с неё, даже
  // если такой же стоит на учёте с карты Evo.
  char drivePath[300];
  uint32_t driveLength = 0;
  bool fresh = false;
  const bool onDrive = findOnDrive(upperApp, upperName, drivePath, sizeof(drivePath), &driveLength, &fresh);
  {
    std::lock_guard<std::mutex> guard(lock_);
    for (const Remote& remote : remotes_) {
      if (!remote.used || strcmp(remote.app, upperApp) != 0 || strcmp(remote.name, upperName) != 0) {
        continue;
      }
      if (onDrive) {
        if (fresh && remote.size != driveLength) {
          log("data file %s/%s: the Evo card file is %u bytes, the USB drive copy %u bytes; reading the "
              "USB drive", upperApp, upperName, static_cast<unsigned>(remote.size),
              static_cast<unsigned>(driveLength));
        }
        break;                                 // учёт есть, но читается флешка
      }
      if (fresh) {
        log("data file %s/%s: Evo card (file %u of the Z80 program), %u bytes", upperApp, upperName,
            static_cast<unsigned>(remote.index), static_cast<unsigned>(remote.size));
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
  if (!onDrive) {
    if (fresh) {
      log("data file %s/%s: neither on the USB drive nor registered from the Evo card", upperApp,
          upperName);
    }
    return kNotFound;
  }
  if (fresh) {
    log("data file %s/%s: USB drive %s, %u bytes", upperApp, upperName, drivePath,
        static_cast<unsigned>(driveLength));
  }
  std::lock_guard<std::mutex> guard(lock_);
  for (size_t i = 0; i < kMaxOpen; ++i) {
    if (!handles_[i].used) {
      handles_[i] = Handle{};
      handles_[i].used = true;
      handles_[i].size = driveLength;
      snprintf(handles_[i].path, sizeof(handles_[i].path), "%s", drivePath);
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
  {
    std::lock_guard<std::mutex> guard(lock_);
    closeCached();
    for (Handle& handle : handles_) {
      handle = Handle{};
    }
  }
  // Следующий запуск ищет заново: флешку могли пополнить. Замок поиска не ждать — его могла держать
  // задача приложения, снятая посреди поиска (stopApp): тогда сброс сделает следующий поиск.
  std::unique_lock<std::mutex> guard(searchLock_, std::try_to_lock);
  if (guard.owns_lock()) {
    resetSearch();
  } else {
    resetPending_.store(true);
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
