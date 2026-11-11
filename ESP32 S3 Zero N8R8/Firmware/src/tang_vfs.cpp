#include "zifi/tang_vfs.hpp"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>

// FreeRTOS — до ff.h: её ffconf.h включает эти заголовки внутри extern "C", а на стенде ПК
// они C++ (tests/stubs_host); здесь они уже включены, и повтор пропускается.
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ff.h"

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

namespace zifi {
namespace {

constexpr size_t kPathCapacity = 520;      // буква диска + путь до 255 знаков UTF-8 в OEM
constexpr uint8_t kAttributeMask = AM_RDO | AM_HID | AM_SYS | AM_ARC;

// Коды ошибок FILEX (vfs_backend.hpp) — по ним SMB-сервер выбирает NTSTATUS.
constexpr uint8_t kStatusEnd = 0x01;
constexpr uint8_t kStatusInvalid = 0x13;
constexpr uint8_t kStatusUnsupported = 0x15;
constexpr uint8_t kStatusProtected = 0x18;
constexpr uint8_t kStatusNotFound = 0x19;
constexpr uint8_t kStatusExists = 0x1A;
constexpr uint8_t kStatusNotEmpty = 0x1B;
constexpr uint8_t kStatusFull = 0x22;
constexpr uint8_t kStatusFailure = 0xFF;

uint8_t statusOf(FRESULT result) {
  switch (result) {
    case FR_OK: return 0;
    case FR_NO_FILE:
    case FR_NO_PATH: return kStatusNotFound;
    case FR_INVALID_NAME:
    case FR_INVALID_PARAMETER: return kStatusInvalid;
    case FR_DENIED:
    case FR_WRITE_PROTECTED: return kStatusProtected;
    case FR_EXIST: return kStatusExists;
    default: return kStatusFailure;
  }
}

void* allocate(size_t bytes) {
#if defined(ESP_PLATFORM)
  void* memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (memory != nullptr) {
    return memory;
  }
#endif
  return malloc(bytes);
}

void release(void* memory) {
#if defined(ESP_PLATFORM)
  heap_caps_free(memory);
#else
  free(memory);
#endif
}

void fillEntry(const FILINFO& info, VfsEntry& entry) {
  entry.isDirectory = (info.fattrib & AM_DIR) != 0;
  entry.size = info.fsize > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : static_cast<uint32_t>(info.fsize);
  entry.writeDate = info.fdate;
  entry.writeTime = info.ftime;
  // У FatFs без FF_FS_CRTIME есть только время изменения: создание и доступ — оно же
  entry.hasMetadata = true;
  entry.attributes = info.fattrib;
  entry.createTenth = 0;
  entry.createDate = info.fdate;
  entry.createTime = info.ftime;
  entry.accessDate = info.fdate;
}

}  // namespace

struct TangVfs::State {
  FIL file;
  FF_DIR directory;
  bool fileOpen = false;
  bool directoryOpen = false;
  bool readOnly = false;              // режим 3 у файла с атрибутом «только чтение»
  bool pendingTimes = false;          // время изменения от SET_METADATA: ещё раз после закрытия
  WORD pendingDate = 0;
  WORD pendingTime = 0;
  char openPath[kPathCapacity] = {};  // путь FatFs открытого файла (для SET_METADATA)
};

// Замок тома на время одной операции: acquire/release порта.
class TangVfs::Lock {
 public:
  explicit Lock(TangVfs& owner) : owner_(owner), problem_(owner.port_.acquire()) {}
  ~Lock() {
    if (problem_ == nullptr) {
      owner_.port_.release();
    }
  }
  bool ok() {
    if (owner_.state_ == nullptr || owner_.buffer_ == nullptr) {
      owner_.failStatus(kStatusFailure, "tang-no-memory");
      return false;
    }
    if (problem_ != nullptr) {
      owner_.failStatus(kStatusFailure, "tang-%s", problem_);
      return false;
    }
    return true;
  }

 private:
  TangVfs& owner_;
  const char* problem_;
};

TangVfs::TangVfs(FatVolumePort& port)
    : port_(port),
      state_(nullptr),
      buffer_(static_cast<uint8_t*>(allocate(kBufferSize))),
      openMode_(0),
      lastStatus_(0),
      lastError_{} {
  void* memory = allocate(sizeof(State));
  if (memory != nullptr) {
    state_ = new (memory) State();
  }
  snprintf(lastError_, sizeof(lastError_), "none");
}

TangVfs::~TangVfs() {
  if (state_ != nullptr) {
    closeFile(false);
    state_->~State();
    release(state_);
  }
  release(buffer_);
}

bool TangVfs::failStatus(uint8_t status, const char* format, ...) {
  lastStatus_ = status;
  va_list args;
  va_start(args, format);
  vsnprintf(lastError_, sizeof(lastError_), format, args);
  va_end(args);
  return false;
}

bool TangVfs::fail(int result, const char* what) {
  return failStatus(statusOf(static_cast<FRESULT>(result)), "%s-fr%d", what, result);
}

// --- имена ----------------------------------------------------------------------------------

bool TangVfs::toFatName(const char* utf8, char* output, size_t capacity) {
#if FF_LFN_UNICODE == 2
  const size_t length = strlen(utf8);
  if (length + 1 > capacity) {
    return false;
  }
  memcpy(output, utf8, length + 1);
  return true;
#else
  size_t at = 0;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8);
  while (*p != 0) {
    uint32_t code = *p;
    int extra = 0;
    if (code >= 0xF0) { code &= 0x07; extra = 3; }
    else if (code >= 0xE0) { code &= 0x0F; extra = 2; }
    else if (code >= 0xC0) { code &= 0x1F; extra = 1; }
    else if (code >= 0x80) { return false; }
    ++p;
    for (int i = 0; i < extra; ++i, ++p) {
      if ((*p & 0xC0) != 0x80) {
        return false;
      }
      code = (code << 6) | (*p & 0x3F);
    }
    uint32_t fat = code;
    if (code >= 0x80) {
      fat = ff_uni2oem(code, FF_CODE_PAGE);
      if (fat == 0) {
        return false;                   // знака нет в кодовой странице тома
      }
    }
    if (fat > 0xFF) {
      if (at + 2 >= capacity) return false;
      output[at++] = static_cast<char>(fat >> 8);
    } else if (at + 1 >= capacity) {
      return false;
    }
    output[at++] = static_cast<char>(fat & 0xFF);
  }
  output[at] = 0;
  return true;
#endif
}

bool TangVfs::fromFatName(const char* fat, char* output, size_t capacity) {
#if FF_LFN_UNICODE == 2
  snprintf(output, capacity, "%s", fat);
  return strlen(fat) < capacity;
#else
  size_t at = 0;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(fat); *p != 0; ++p) {
    uint32_t code = *p;
    if (code >= 0x80) {
      code = ff_oem2uni(static_cast<WCHAR>(code), FF_CODE_PAGE);
      if (code == 0) {
        code = '?';
      }
    }
    char bytes[3];
    size_t n = 0;
    if (code < 0x80) {
      bytes[n++] = static_cast<char>(code);
    } else if (code < 0x800) {
      bytes[n++] = static_cast<char>(0xC0 | (code >> 6));
      bytes[n++] = static_cast<char>(0x80 | (code & 0x3F));
    } else {
      bytes[n++] = static_cast<char>(0xE0 | (code >> 12));
      bytes[n++] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      bytes[n++] = static_cast<char>(0x80 | (code & 0x3F));
    }
    if (at + n >= capacity) {
      return false;
    }
    memcpy(output + at, bytes, n);
    at += n;
  }
  output[at] = 0;
  return true;
#endif
}

bool TangVfs::fatPath(const char* path, char* output, size_t capacity) {
  const char* drive = port_.drive();
  const size_t driveLength = strlen(drive);
  if (path == nullptr || path[0] != '/' || driveLength + 2 > capacity) {
    return failStatus(kStatusInvalid, "tang-path");
  }
  memcpy(output, drive, driveLength);
  if (!toFatName(path, output + driveLength, capacity - driveLength)) {
    return failStatus(kStatusInvalid, "tang-name");
  }
  return true;
}

// --- каталоги и сведения --------------------------------------------------------------------

bool TangVfs::stat(const char* path, VfsEntry& entry) {
  entry = VfsEntry();
  Lock lock(*this);
  if (!lock.ok()) {
    return false;
  }
  if (path != nullptr && strcmp(path, "/") == 0) {
    entry.isDirectory = true;           // у корня нет записи каталога: FatFs его не находит
    entry.attributes = AM_DIR;
    lastStatus_ = 0;
    return true;
  }
  char full[kPathCapacity];
  if (!fatPath(path, full, sizeof(full))) {
    return false;
  }
  FILINFO info = {};
  const FRESULT result = f_stat(full, &info);
  if (result != FR_OK) {
    return fail(result, "stat");
  }
  fillEntry(info, entry);
  fromFatName(info.fname, entry.name, sizeof(entry.name));
  lastStatus_ = 0;
  return true;
}

void TangVfs::closeDirectory() {
  if (state_->directoryOpen) {
    f_closedir(&state_->directory);
    state_->directoryOpen = false;
  }
}

bool TangVfs::openDirectory(const char* path) {
  Lock lock(*this);
  if (!lock.ok()) {
    return false;
  }
  closeDirectory();
  char full[kPathCapacity];
  if (!fatPath(path, full, sizeof(full))) {
    return false;
  }
  const FRESULT result = f_opendir(&state_->directory, full);
  if (result != FR_OK) {
    return fail(result, "opendir");
  }
  state_->directoryOpen = true;
  lastStatus_ = 0;
  return true;
}

bool TangVfs::readDirectory(VfsEntry& entry, bool& atEnd) {
  entry = VfsEntry();
  atEnd = false;
  Lock lock(*this);
  if (!lock.ok()) {
    return false;
  }
  if (!state_->directoryOpen) {
    return failStatus(kStatusUnsupported, "readdir-not-open");
  }
  FILINFO info = {};
  const FRESULT result = f_readdir(&state_->directory, &info);
  if (result != FR_OK) {
    closeDirectory();
    return fail(result, "readdir");
  }
  if (info.fname[0] == 0) {
    atEnd = true;
    closeDirectory();
    lastStatus_ = 0;
    return true;
  }
  fillEntry(info, entry);
  fromFatName(info.fname, entry.name, sizeof(entry.name));
  lastStatus_ = 0;
  return true;
}

bool TangVfs::getFsInfo(VfsFsInfo& info, bool, bool) {
  info = VfsFsInfo();
  Lock lock(*this);
  if (!lock.ok()) {
    return false;
  }
  char root[8];
  snprintf(root, sizeof(root), "%s/", port_.drive());
  DWORD freeClusters = 0;
  FATFS* fs = nullptr;
  const FRESULT result = f_getfree(root, &freeClusters, &fs);
  if (result != FR_OK || fs == nullptr) {
    return fail(result, "getfree");
  }
#if FF_MAX_SS != FF_MIN_SS
  uint32_t bytesPerSector = fs->ssize;
#else
  uint32_t bytesPerSector = FF_MAX_SS;
#endif
  uint32_t sectorsPerCluster = fs->csize;
  // В VfsFsInfo секторов на кластер — байт: кластер exFAT до 32 МБ укладываем укрупнением сектора
  while (sectorsPerCluster > 128 && bytesPerSector < 32768) {
    sectorsPerCluster /= 2;
    bytesPerSector *= 2;
  }
  info.flags = 0x04;                  // число свободных кластеров точное
  info.bytesPerSector = static_cast<uint16_t>(bytesPerSector);
  info.sectorsPerCluster = static_cast<uint8_t>(sectorsPerCluster);
  info.totalClusters = fs->n_fatent - 2;
  info.freeClusters = freeClusters;
  info.serial = kSerial;
  snprintf(info.label, sizeof(info.label), "TANG");
  info.totalSectors = info.totalClusters * sectorsPerCluster;
  lastStatus_ = 0;
  return true;
}

// --- файлы ----------------------------------------------------------------------------------

bool TangVfs::openWith(const char* path, uint8_t mode) {
  Lock lock(*this);
  if (!lock.ok()) {
    return false;
  }
  if (state_->fileOpen) {
    closeFile(true);
  }
  closeDirectory();
  char* full = state_->openPath;
  if (!fatPath(path, full, sizeof(state_->openPath))) {
    return false;
  }
  BYTE flags = FA_READ | FA_OPEN_EXISTING;
  if (mode == 1) {
    flags = FA_WRITE | FA_CREATE_ALWAYS;
  } else if (mode == 2) {
    flags = FA_WRITE | FA_OPEN_APPEND;
  } else if (mode == 3) {
    flags = FA_READ | FA_WRITE | FA_OPEN_EXISTING;
  }
  state_->readOnly = false;
  FRESULT result = f_open(&state_->file, full, flags);
  if (result == FR_DENIED && mode == 3) {
    // «только чтение»: позиционное чтение такого файла законно, запись в него — отказ 0x18
    result = f_open(&state_->file, full, FA_READ | FA_OPEN_EXISTING);
    state_->readOnly = result == FR_OK;
  }
  if (result != FR_OK) {
    return fail(result, "open");
  }
  state_->fileOpen = true;
  state_->pendingTimes = false;
  openMode_ = mode;
  lastStatus_ = 0;
  return true;
}

bool TangVfs::openFile(const char* path, bool forWrite) { return openWith(path, forWrite ? 1 : 0); }
bool TangVfs::openFileAppend(const char* path) { return openWith(path, 2); }
bool TangVfs::openFileRandom(const char* path) { return openWith(path, 3); }

bool TangVfs::seekFile(uint32_t offset) {
  if (openMode_ != 3 || !state_->fileOpen) {
    return failStatus(kStatusUnsupported, "seek-not-random");
  }
  Lock lock(*this);
  if (!lock.ok()) {
    return false;
  }
  // За конец файла — как FILEX: у файла на запись FatFs удлиняет его до позиции (так Проводник
  // пишет хвост копии первым, начало — следом); у «только чтения» позиция встаёт на конец, и
  // чтение отдаёт конец файла.
  const FRESULT result = f_lseek(&state_->file, offset);
  if (result != FR_OK) {
    return fail(result, "seek");
  }
  if (!state_->readOnly && f_tell(&state_->file) != offset) {
    return failStatus(kStatusFull, "seek-disk-full");   // удлинить не хватило места
  }
  lastStatus_ = 0;
  return true;
}

bool TangVfs::readFileWindow(size_t wanted, WindowSink sink, void* sinkContext,
                             size_t& received) {
  received = 0;
  if (!state_->fileOpen || buffer_ == nullptr) {
    return failStatus(kStatusUnsupported, "read-not-open");
  }
  Lock lock(*this);
  if (!lock.ok()) {
    return false;
  }
  while (received < wanted) {
    const size_t part = (wanted - received) < kBufferSize ? (wanted - received) : kBufferSize;
    UINT got = 0;
    const FRESULT result = f_read(&state_->file, buffer_, static_cast<UINT>(part), &got);
    if (result != FR_OK) {
      return fail(result, "read");
    }
    if (got != 0 && !sink(sinkContext, buffer_, got)) {
      return failStatus(kStatusFailure, "read-sink");
    }
    received += got;
    if (got < part) {
      break;                            // конец файла
    }
  }
  lastStatus_ = 0;
  return true;
}

bool TangVfs::writeFileWindow(size_t length, WindowSource source, void* sourceContext) {
  if (!state_->fileOpen || openMode_ == 0 || buffer_ == nullptr) {
    return failStatus(kStatusUnsupported, "write-not-open");
  }
  if (state_->readOnly) {
    return failStatus(kStatusProtected, "write-read-only");
  }
  Lock lock(*this);
  if (!lock.ok()) {
    return false;
  }
  for (size_t done = 0; done < length;) {
    const size_t want = (length - done) < kBufferSize ? (length - done) : kBufferSize;
    const size_t got = source(sourceContext, done, buffer_, want);
    if (got == 0) {
      return failStatus(kStatusFailure, "write-source");
    }
    UINT written = 0;
    const FRESULT result = f_write(&state_->file, buffer_, static_cast<UINT>(got), &written);
    if (result != FR_OK) {
      return fail(result, "write");
    }
    if (written != got) {
      return failStatus(kStatusFull, "write-disk-full");
    }
    done += got;
  }
  lastStatus_ = 0;
  return true;
}

bool TangVfs::extendFile(uint32_t) {
  return failStatus(kStatusUnsupported, "extend-unsupported");
}

bool TangVfs::setFileSize(uint32_t size, uint32_t& actualSize) {
  actualSize = 0;
  if (openMode_ != 3 || !state_->fileOpen) {
    return failStatus(kStatusUnsupported, "set-eof-not-random");
  }
  if (state_->readOnly) {
    return failStatus(kStatusProtected, "set-eof-read-only");
  }
  Lock lock(*this);
  if (!lock.ok()) {
    return false;
  }
  FIL* file = &state_->file;
  const FSIZE_t old = f_size(file);
  FRESULT result = FR_OK;
  if (size < old) {
    result = f_lseek(file, size);
    if (result == FR_OK) {
      result = f_truncate(file);
    }
  } else if (size > old) {
    // Рост — нулями, как SetEndOfFile в Windows: FatFs при f_lseek за конец оставил бы в новых
    // кластерах прежнее содержимое карты.
    result = f_lseek(file, old);
    memset(buffer_, 0, kBufferSize);
    for (FSIZE_t at = old; result == FR_OK && at < size;) {
      const UINT part = static_cast<UINT>((size - at) < kBufferSize ? (size - at) : kBufferSize);
      UINT written = 0;
      result = f_write(file, buffer_, part, &written);
      if (result == FR_OK && written != part) {
        return failStatus(kStatusFull, "set-eof-disk-full");
      }
      at += part;
    }
  }
  if (result != FR_OK) {
    return fail(result, "set-eof");
  }
  actualSize = static_cast<uint32_t>(f_size(file));
  lastStatus_ = 0;
  return true;
}

void TangVfs::applyPendingTimes() {
  if (!state_->pendingTimes) {
    return;
  }
  state_->pendingTimes = false;
  FILINFO times = {};
  times.fdate = state_->pendingDate;
  times.ftime = state_->pendingTime;
  f_utime(state_->openPath, &times);
}

bool TangVfs::closeFile(bool) {
  // FatFs фиксирует записанное при любом закрытии; отказ от файла (abort) — забота сервера
  // (он сам удаляет недописанный новый файл).
  Lock lock(*this);
  if (!lock.ok()) {
    // тома нет (ядро сменилось, карта у WC): открытого больше нет и трогать FatFs нельзя
    if (state_ != nullptr) {
      state_->fileOpen = false;
      state_->directoryOpen = false;
    }
    openMode_ = 0;
    return false;
  }
  closeDirectory();
  if (!state_->fileOpen) {
    openMode_ = 0;
    lastStatus_ = 0;
    return true;
  }
  const FRESULT result = f_close(&state_->file);
  state_->fileOpen = false;
  openMode_ = 0;
  // закрытие записанного файла ставит время «сейчас»: время клиента — поверх
  applyPendingTimes();
  if (result != FR_OK) {
    return fail(result, "close");
  }
  lastStatus_ = 0;
  return true;
}

bool TangVfs::setMetadata(const VfsMetadata& metadata, uint8_t& appliedAttributes) {
  appliedAttributes = 0;
  if (!state_->fileOpen) {
    return failStatus(kStatusUnsupported, "metadata-not-open");
  }
  Lock lock(*this);
  if (!lock.ok()) {
    return false;
  }
  const BYTE mask = static_cast<BYTE>(metadata.attrMask & kAttributeMask);
  if (mask != 0) {
    const FRESULT result = f_chmod(state_->openPath, metadata.attrValue & mask, mask);
    if (result != FR_OK) {
      return fail(result, "chmod");
    }
    appliedAttributes = static_cast<uint8_t>(metadata.attrValue & mask);
  }
  if ((metadata.timeMask & 0x04) != 0) {
    FILINFO times = {};
    times.fdate = metadata.writeDate;
    times.ftime = metadata.writeTime;
    const FRESULT result = f_utime(state_->openPath, &times);
    if (result != FR_OK) {
      return fail(result, "utime");
    }
    state_->pendingTimes = openMode_ != 0;
    state_->pendingDate = metadata.writeDate;
    state_->pendingTime = metadata.writeTime;
  }
  lastStatus_ = 0;
  return true;
}

// --- правка тома ----------------------------------------------------------------------------

bool TangVfs::remove(const char* path) {
  Lock lock(*this);
  if (!lock.ok()) {
    return false;
  }
  char full[kPathCapacity];
  if (!fatPath(path, full, sizeof(full))) {
    return false;
  }
  const FRESULT result = f_unlink(full);
  if (result == FR_DENIED) {
    FILINFO info = {};
    if (f_stat(full, &info) == FR_OK && (info.fattrib & AM_DIR) != 0) {
      return failStatus(kStatusNotEmpty, "delete-not-empty");
    }
  }
  if (result != FR_OK) {
    return fail(result, "delete");
  }
  lastStatus_ = 0;
  return true;
}

bool TangVfs::makeDirectory(const char* path) {
  Lock lock(*this);
  if (!lock.ok()) {
    return false;
  }
  char full[kPathCapacity];
  if (!fatPath(path, full, sizeof(full))) {
    return false;
  }
  const FRESULT result = f_mkdir(full);
  if (result != FR_OK) {
    return fail(result, "mkdir");
  }
  lastStatus_ = 0;
  return true;
}

bool TangVfs::moveRename(const char* oldPath, const char* newPath, bool directory,
                         bool replace) {
  Lock lock(*this);
  if (!lock.ok()) {
    return false;
  }
  char from[kPathCapacity];
  char to[kPathCapacity];
  if (!fatPath(oldPath, from, sizeof(from)) || !fatPath(newPath, to, sizeof(to))) {
    return false;
  }
  if (replace) {
    FILINFO info = {};
    if (f_stat(to, &info) == FR_OK) {
      if ((info.fattrib & AM_DIR) != 0 || directory) {
        return failStatus(kStatusExists, "move-target-directory");
      }
      const FRESULT removed = f_unlink(to);
      if (removed != FR_OK) {
        return fail(removed, "move-replace");
      }
    }
  }
  const FRESULT result = f_rename(from, to);
  if (result != FR_OK) {
    return fail(result, "move");
  }
  lastStatus_ = 0;
  return true;
}

bool TangVfs::rename(const char* oldPath, const char* newName, bool directory) {
  if (oldPath == nullptr || newName == nullptr || strchr(newName, '/') != nullptr) {
    return failStatus(kStatusInvalid, "rename-name");
  }
  const char* slash = strrchr(oldPath, '/');
  if (slash == nullptr) {
    return failStatus(kStatusInvalid, "rename-path");
  }
  char target[kPathCapacity];
  const int written = snprintf(target, sizeof(target), "%.*s/%s",
                               static_cast<int>(slash - oldPath), oldPath, newName);
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(target)) {
    return failStatus(kStatusInvalid, "rename-long");
  }
  return moveRename(oldPath, target, directory, false);
}

}  // namespace zifi
