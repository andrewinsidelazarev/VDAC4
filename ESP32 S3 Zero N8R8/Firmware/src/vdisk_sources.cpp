// Источники тома VDAC4 VFS — см. zifi/vdisk_sources.hpp
#include "zifi/vdisk_sources.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ff.h"   // ff_oem2uni, ff_uni2oem, FF_CODE_PAGE
#include "zifi/bringup_log.hpp"

#if ZIFI_HAS_SPI_LINK
#include "zifi/tang_sd.hpp"
#endif
#if ZIFI_HAS_USB_DRIVE
#include "zifi/usb_drive.hpp"
#endif

namespace zifi {
namespace vdisk_sources {

namespace {

// дата и время изменения в формате FAT
void fatTime(time_t t, uint16_t& date, uint16_t& tm) {
  struct tm local;
  date = tm = 0;
  if (t <= 0 || !gmtime_r(&t, &local) || local.tm_year < 80) return;
  date = (uint16_t)(((local.tm_year - 80) << 9) | ((local.tm_mon + 1) << 5) | local.tm_mday);
  tm = (uint16_t)((local.tm_hour << 11) | (local.tm_min << 5) | (local.tm_sec / 2));
}

}  // namespace

size_t oemToUtf8(const char* in, size_t n, char* out) {
  size_t k = 0;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t c = (uint8_t)in[i];
    uint32_t cp = c < 0x80 ? c : ff_oem2uni(c, FF_CODE_PAGE);
    if (cp == 0) cp = '?';
    if (cp < 0x80) out[k++] = (char)cp;
    else if (cp < 0x800) { out[k++] = (char)(0xC0 | cp >> 6); out[k++] = (char)(0x80 | (cp & 63)); }
    else { out[k++] = (char)(0xE0 | cp >> 12); out[k++] = (char)(0x80 | (cp >> 6 & 63)); out[k++] = (char)(0x80 | (cp & 63)); }
  }
  out[k] = 0;
  return k;
}

size_t utf8ToOem(const char* in, char* out, size_t capacity) {
  size_t k = 0;
  for (size_t i = 0; in[i] && k + 1 < capacity;) {
    const uint8_t c = (uint8_t)in[i];
    uint32_t cp = c;
    size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    if (len > 1) {
      cp = c & (0x3F >> (len - 1));
      for (size_t j = 1; j < len; ++j) {
        const uint8_t d = (uint8_t)in[i + j];
        if ((d & 0xC0) != 0x80) { len = j; cp = '?'; break; }
        cp = cp << 6 | (d & 0x3F);
      }
    }
    i += len;
    const uint32_t o = cp < 0x80 ? cp : cp < 0x10000 ? ff_uni2oem(cp, FF_CODE_PAGE) : 0;
    out[k++] = (char)(o != 0 && o < 0x100 ? o : '?');
  }
  out[k] = 0;
  return k;
}

bool PosixSource::full(const char* path, char* out, size_t size) const {
  char oem[512];
  utf8ToOem(path, oem, sizeof oem);
  const int n = oem[0] ? snprintf(out, size, "%s/%s", mount_, oem) : snprintf(out, size, "%s", mount_);
  return n > 0 && (size_t)n < size;
}

bool PosixSource::list(const char* path, Each each, void* user) {
  char folder[600];
  if (!full(path, folder, sizeof folder)) return false;
  DIR* dir = opendir(folder);
  if (!dir) return false;
  static char item[600 + 256];
  char name[3 * 255 + 1];
  bool ok = true;
  for (dirent* e = readdir(dir); e; e = readdir(dir)) {
    const size_t len = strnlen(e->d_name, 256);
    if (!len || len > 255) continue;
    snprintf(item, sizeof item, "%s/%s", folder, e->d_name);
    struct stat st;
    if (stat(item, &st)) continue;
    oemToUtf8(e->d_name, len, name);
    vdisk::Entry entry{name, S_ISDIR(st.st_mode), S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size, 0, 0};
    fatTime(st.st_mtime, entry.date, entry.time);
    if (!each(entry, user)) break;
  }
  closedir(dir);
  return ok;
}

bool PosixSource::read(const char* path, uint64_t offset, uint8_t* out, size_t n, size_t* got) {
  char file[600];
  *got = 0;
  if (!full(path, file, sizeof file)) return false;
  FILE* f = fopen(file, "rb");
  if (!f) return false;
  bool ok = fseek(f, (long)offset, SEEK_SET) == 0;
  if (ok) {
    *got = fread(out, 1, n, f);
    ok = *got == n || !ferror(f);
  }
  fclose(f);
  return ok;
}

#if ZIFI_HAS_USB_DRIVE
namespace {
// Замок состояния записи UsbSource: служба VDISK пишет с ядра 1, а файл тома читают и приложения
// (видео с SD2 — sd2_files) из своих задач.
class Hold {
 public:
  explicit Hold(void* lock) : lock_(static_cast<SemaphoreHandle_t>(lock)) {
    if (lock_ != nullptr) xSemaphoreTakeRecursive(lock_, portMAX_DELAY);
  }
  ~Hold() {
    if (lock_ != nullptr) xSemaphoreGiveRecursive(lock_);
  }

 private:
  SemaphoreHandle_t lock_;
};
// Сколько ждать место в кольце (и всё кольцо при sync): флешка встала — WRITE получает отказ.
constexpr uint32_t kSlotWaitMs = 20000;
}  // namespace

UsbSource::UsbSource() : PosixSource("USB", "/usb") { lock_ = xSemaphoreCreateRecursiveMutex(); }

bool UsbSource::list(const char* path, Each each, void* user) {
  if (!usbDrive().ready()) return false;      // нет флешки (или не FAT32/exFAT) — папка пуста
  return PosixSource::list(path, each, user);
}

// Как PosixSource::read, но кусками по UsbDrive::kTransferBytes: FatFs отдаёт драйверу USB
// целые сектора прямо в буфер вызывающего, и одно чтение окна тома (десятки КБ) вырастило бы
// буфер драйвера во внутренней памяти до своей длины.
bool UsbSource::read(const char* path, uint64_t offset, uint8_t* out, size_t n, size_t* got) {
  char file[600];
  *got = 0;
  if (!full(path, file, sizeof file)) return false;
  {
    // файл, в который идёт запись: накопленное и кольцо — сначала на флешку, иначе прочитается
    // прежнее
    Hold hold(lock_);
    if (fd_ >= 0 && strcmp(open_, file) == 0) {
      pending_.drain(sink, this);
      sync();
    }
  }
  FILE* f = fopen(file, "rb");
  if (!f) return false;
  bool ok = fseek(f, (long)offset, SEEK_SET) == 0;
  while (ok && *got < n) {
    const size_t part = n - *got < UsbDrive::kTransferBytes ? n - *got : UsbDrive::kTransferBytes;
    const size_t done = fread(out + *got, 1, part, f);
    *got += done;
    if (done != part) {
      ok = !ferror(f);                      // конец файла — не ошибка
      break;
    }
  }
  fclose(f);
  return ok;
}

// ---- запись: том USB Flash (0.1.148) ----
// Имена — через full(): UTF-8 -> OEM FatFs (CP866, как у WC); знака нет в CP866 — '?', и FatFs имя
// отвергает: WC получит ошибку носителя. Отказы — в журнал (удачные операции не пишутся: копирование
// тысячи файлов писало бы во flash ESP тысячу строк).

namespace {
// Файл от стольких байт записи — строка о скорости в журнале при закрытии (0.1.149): по ней на плате
// видно, сколько ушло на флешку (USB) и сколько — на приход секторов от Z80. Мелкие файлы не пишутся.
constexpr uint64_t kSpeedLogBytes = 256 * 1024;
}  // namespace

bool UsbSource::writeNow(uint64_t offset, const uint8_t* data, size_t n) {
  const int64_t start = esp_timer_get_time();
  const bool ok = fd_ >= 0 && lseek(fd_, (off_t)offset, SEEK_SET) == (off_t)offset &&
                  ::write(fd_, data, n) == (ssize_t)n;
  usbUs_ += esp_timer_get_time() - start;
  ++commands_;
  if (!ok) {
    bringupLog("vdisk", "USB write %s at %u, %u bytes: FAILED", open_, static_cast<unsigned>(offset),
               static_cast<unsigned>(n));
  }
  return ok;
}

bool UsbSource::sink(void* user, uint64_t offset, const uint8_t* data, size_t n) {
  return static_cast<UsbSource*>(user)->queue(offset, data, n);
}

// Задача usb-write и её кольцо — при первой записи. Не вышло (нет памяти) — куски пишутся на
// флешку сразу, как в 0.1.149.
bool UsbSource::startWriter() {
  if (ready_ != nullptr) return true;
  if (writerTried_) return false;
  writerTried_ = true;
  ring_ = static_cast<uint8_t*>(heap_caps_malloc(kSlots * (UsbDrive::kTransferBytes + sizeof(Slot)),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  slots_ = ring_ != nullptr ? reinterpret_cast<Slot*>(ring_ + kSlots * UsbDrive::kTransferBytes) : nullptr;
  SemaphoreHandle_t free = xSemaphoreCreateCounting(kSlots, kSlots);
  QueueHandle_t ready = xQueueCreate(kSlots, sizeof(size_t));
  free_ = free;
  ready_ = ready;
  // ядро 1, как задачи хоста USB; приоритет — как у цикла обмена с Z80 (задача почти всё время ждёт
  // флешку); стек в PSRAM: flash ESP задача не трогает (журнал — очередью)
  if (ring_ == nullptr || free == nullptr || ready == nullptr ||
      xTaskCreatePinnedToCoreWithCaps(writerTask, "usb-write", 6144, this, 1, nullptr, 1,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    bringupLog("vdisk", "USB write task not started: writes go to the drive directly");
    if (ready != nullptr) vQueueDelete(ready);
    if (free != nullptr) vSemaphoreDelete(free);
    heap_caps_free(ring_);
    ring_ = nullptr;
    slots_ = nullptr;
    free_ = nullptr;
    ready_ = nullptr;
    return false;
  }
  return true;
}

void UsbSource::writerTask(void* arg) {
  auto* self = static_cast<UsbSource*>(arg);
  for (;;) {
    size_t slot = 0;
    if (xQueueReceive(static_cast<QueueHandle_t>(self->ready_), &slot, portMAX_DELAY) != pdTRUE) continue;
    // файл (fd_) до конца кольца не меняется: закрытие и смена файла сначала ждут sync()
    const Slot& s = self->slots_[slot];
    if (!self->writeNow(s.offset, self->ring_ + slot * UsbDrive::kTransferBytes, s.length)) {
      self->asyncFailed_ = true;
    }
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(self->free_));
  }
}

bool UsbSource::queue(uint64_t offset, const uint8_t* data, size_t n) {
  if (!startWriter()) return writeNow(offset, data, n);
  if (n > UsbDrive::kTransferBytes) {
    sync();                                    // кусок длиннее места в кольце: после прежних, сразу
    return writeNow(offset, data, n);
  }
  SemaphoreHandle_t free = static_cast<SemaphoreHandle_t>(free_);
  if (xSemaphoreTake(free, 0) != pdTRUE) {
    ++ringFull_;                               // флешка отстала на всё кольцо: ждать её
    if (xSemaphoreTake(free, pdMS_TO_TICKS(kSlotWaitMs)) != pdTRUE) {
      bringupLog("vdisk", "USB write %s: the drive took no data for %u ms", open_,
                 static_cast<unsigned>(kSlotWaitMs));
      return false;
    }
  }
  const size_t slot = nextSlot_;               // куски уходят и освобождаются по порядку
  nextSlot_ = (nextSlot_ + 1) % kSlots;
  memcpy(ring_ + slot * UsbDrive::kTransferBytes, data, n);
  slots_[slot] = Slot{offset, static_cast<uint32_t>(n)};
  xQueueSend(static_cast<QueueHandle_t>(ready_), &slot, portMAX_DELAY);   // мест в очереди — kSlots
  return true;
}

void UsbSource::sync() {
  if (free_ == nullptr) return;
  SemaphoreHandle_t free = static_cast<SemaphoreHandle_t>(free_);
  size_t taken = 0;
  while (taken < kSlots && xSemaphoreTake(free, pdMS_TO_TICKS(kSlotWaitMs)) == pdTRUE) ++taken;
  if (taken < kSlots) {
    bringupLog("vdisk", "USB write %s: %u writes still pending", open_,
               static_cast<unsigned>(kSlots - taken));
    asyncFailed_ = true;
  }
  while (taken > 0) {
    xSemaphoreGive(free);
    --taken;
  }
}

bool UsbSource::closeOpen() {
  Hold hold(lock_);
  bool ok = pending_.drain(sink, this);
  sync();
  if (asyncFailed_) {
    asyncFailed_ = false;
    ok = false;
  }
  if (!ok) failed_ = true;                     // скажет следующий write(), если отказ не ему
  if (fd_ >= 0) {
    if (bytes_ >= kSpeedLogBytes) {
      // время — от первого сектора до последнего, пришедшего от Z80; USB — все записи на флешку
      // (с 0.1.150 — в задаче usb-write, параллельно приходу секторов); кольцо полно — WRITE ждал
      bringupLog("vdisk", "USB write %s: %u KB in %u ms, USB %u ms in %u writes, ring full %u", open_,
                 static_cast<unsigned>(bytes_ / 1024), static_cast<unsigned>((lastUs_ - firstUs_) / 1000),
                 static_cast<unsigned>(usbUs_ / 1000), static_cast<unsigned>(commands_),
                 static_cast<unsigned>(ringFull_));
    }
    close(fd_);
  }
  fd_ = -1;
  open_[0] = 0;
  bytes_ = 0;
  usbUs_ = 0;
  commands_ = 0;
  ringFull_ = 0;
  return ok;
}

void UsbSource::flush() { closeOpen(); }

bool UsbSource::create(const char* path, uint64_t size) {
  char file[600];
  if (!full(path, file, sizeof file)) return false;
  Hold hold(lock_);
  closeOpen();
  const int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  // длина сразу: FatFs за концом файла в режиме записи удлиняет его (место занято, данных нет)
  const bool ok = fd >= 0 && (size == 0 || lseek(fd, (off_t)size, SEEK_SET) == (off_t)size);
  if (fd >= 0) close(fd);
  if (!ok) bringupLog("vdisk", "USB create %s, %u bytes: FAILED", file, static_cast<unsigned>(size));
  return ok;
}

bool UsbSource::write(const char* path, uint64_t offset, const uint8_t* data, size_t n) {
  char file[600];
  Hold hold(lock_);
  if (asyncFailed_) {
    asyncFailed_ = false;                      // кусок в задаче usb-write не лёг
    failed_ = true;
  }
  if (failed_) {
    // накопленное ушло на флешку без вызывающего (flush, другая операция) и не легло: WC узнаёт
    // об ошибке на этом секторе — позже, чем она случилась, но не молча
    failed_ = false;
    return false;
  }
  if (!full(path, file, sizeof file)) return false;
  if (fd_ < 0 || strcmp(open_, file) != 0) {
    if (!closeOpen()) {                        // прежний файл не дописан: отказ — этому сектору
      failed_ = false;
      return false;
    }
    fd_ = open(file, O_WRONLY);
    if (fd_ < 0) {
      bringupLog("vdisk", "USB write %s: not opened", file);
      return false;
    }
    snprintf(open_, sizeof open_, "%s", file);
    firstUs_ = esp_timer_get_time();
  }
  if (!pending_.attached()) {
    // один раз: 4 КБ в PSRAM; нет памяти — секторы идут на флешку по одному, как в 0.1.148
    auto* buffer = static_cast<uint8_t*>(
        heap_caps_malloc(UsbDrive::kTransferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer != nullptr) pending_.attach(buffer, UsbDrive::kTransferBytes);
  }
  lastUs_ = esp_timer_get_time();
  bytes_ += n;
  if (!pending_.put(offset, data, n, sink, this)) {
    closeOpen();
    failed_ = false;                           // отказ получает этот вызов
    return false;
  }
  return true;
}

bool UsbSource::resize(const char* path, uint64_t size) {
  char file[600];
  if (!full(path, file, sizeof file)) return false;
  Hold hold(lock_);
  if (strcmp(open_, file) == 0) closeOpen();
  struct stat st;
  if (stat(file, &st) != 0) return false;
  bool ok = true;
  if ((uint64_t)st.st_size > size) {
    ok = truncate(file, (off_t)size) == 0;
  } else if ((uint64_t)st.st_size < size) {
    const int fd = open(file, O_WRONLY);   // truncate ESP-IDF не удлиняет, а lseek в записи — да
    ok = fd >= 0 && lseek(fd, (off_t)size, SEEK_SET) == (off_t)size;
    if (fd >= 0) close(fd);
  }
  if (!ok) bringupLog("vdisk", "USB resize %s to %u: FAILED", file, static_cast<unsigned>(size));
  return ok;
}

bool UsbSource::makeDir(const char* path) {
  char file[600];
  if (!full(path, file, sizeof file)) return false;
  const bool ok = mkdir(file, 0777) == 0;
  if (!ok) bringupLog("vdisk", "USB mkdir %s: FAILED", file);
  return ok;
}

bool UsbSource::remove(const char* path, bool dir) {
  char file[600];
  if (!full(path, file, sizeof file)) return false;
  Hold hold(lock_);
  if (strcmp(open_, file) == 0) {
    pending_.drop();                           // файл удаляется: дописывать нечего
    closeOpen();
  }
  const bool ok = (dir ? rmdir(file) : unlink(file)) == 0;
  if (!ok) bringupLog("vdisk", "USB remove %s: FAILED", file);
  return ok;
}

bool UsbSource::rename(const char* from, const char* to) {
  char a[600], b[600];
  if (!full(from, a, sizeof a) || !full(to, b, sizeof b)) return false;
  closeOpen();
  const bool ok = ::rename(a, b) == 0;
  if (!ok) bringupLog("vdisk", "USB rename %s -> %s: FAILED", a, b);
  return ok;
}

bool UsbSource::setTime(const char* path, uint16_t date, uint16_t time) {
  char file[600];
  if (!full(path, file, sizeof file) || date == 0) return false;
  Hold hold(lock_);
  if (strcmp(open_, file) == 0) closeOpen();   // закрытие после записи само поставило бы «сейчас»
  struct tm t = {};
  t.tm_year = ((date >> 9) & 0x7F) + 80;
  t.tm_mon = ((date >> 5) & 0x0F) - 1;
  t.tm_mday = date & 0x1F;
  t.tm_hour = (time >> 11) & 0x1F;
  t.tm_min = (time >> 5) & 0x3F;
  t.tm_sec = (time & 0x1F) * 2;
  t.tm_isdst = -1;
  struct utimbuf u;
  u.actime = u.modtime = mktime(&t);
  return utime(file, &u) == 0;
}
#endif

#if ZIFI_HAS_SPI_LINK
bool TangSource::list(const char* path, Each each, void* user) {
  tang_sd::Session session(tang_sd::kQuickMs);
  if (!session.ok() || tang_sd::mount()) return false;
  return PosixSource::list(path, each, user);
}

bool TangSource::read(const char* path, uint64_t offset, uint8_t* out, size_t n, size_t* got) {
  tang_sd::Session session(tang_sd::kQuickMs);
  *got = 0;
  if (!session.ok() || tang_sd::mount()) return false;
  return PosixSource::read(path, offset, out, n, got);
}
#endif

}  // namespace vdisk_sources
}  // namespace zifi
