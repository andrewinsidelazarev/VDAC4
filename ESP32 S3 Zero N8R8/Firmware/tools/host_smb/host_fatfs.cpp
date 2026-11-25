// Тома FatFs на стенде ПК: диски в памяти и служебные функции FatFs (на плате их дают ESP-IDF,
// tang_sd и usb_drive). Сама FatFs — components/fatfs/src/ff.c прошивки, собранная для ПК.
// Диск 0 ("0:") — microSD Tang (шара «TANG», FAT32), диск 1 ("1:") — флешка в USB-C ESP32
// (шара «USB», 0.1.171; exFAT — как у пользователя на плате).

#include "host_fatfs.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

// Заглушки FreeRTOS (C++) — до ff.h: её ffconf.h включает их внутри extern "C".
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "diskio.h"
#include "ff.h"

namespace fs = std::filesystem;

namespace {

constexpr UINT kSectorBytes = 512;

// Один диск стенда: образ в памяти, объект тома FatFs и «вставлен ли».
struct Disk {
  std::vector<uint8_t> data;
  FATFS fs = {};
  bool mounted = false;
  bool present = true;
  // Самая большая передача устройству (disk_read/disk_write) после создания образа, байтов: на
  // плате передача флешке USB не должна быть длиннее буфера её драйвера (0.1.174).
  uint64_t largest = 0;
};
Disk g_disks[2];
std::recursive_mutex g_volumeLocks[FF_VOLUMES + 1];

Disk* diskOf(BYTE pdrv) { return pdrv < 2 ? &g_disks[pdrv] : nullptr; }

// Том для TangVfs: диск pdrv под буквой "pdrv:"; transfer — предел передачи, как у порта на плате.
class HostVolume : public zifi::FatVolumePort {
 public:
  HostVolume(BYTE pdrv, const char* drive, const char* absent, size_t transfer)
      : pdrv_(pdrv), drive_(drive), absent_(absent), transfer_(transfer) {}
  const char* acquire() override {
    const Disk& disk = g_disks[pdrv_];
    if (!disk.present) {
      return absent_;
    }
    return disk.mounted ? nullptr : "volume is not mounted";
  }
  void release() override {}
  const char* drive() const override { return drive_; }
  size_t maxTransfer() const override { return transfer_; }
  void setTransfer(size_t transfer) { transfer_ = transfer; }

 private:
  BYTE pdrv_;
  const char* drive_;
  const char* absent_;
  size_t transfer_;
};

// Флешке — как UsbVolume на плате, когда серверы вырастили буфер драйвера
// (UsbDrive::kServerTransferBytes = 16384; совпадение проверяет tests/test_usb_share.py), карте
// Tang — предел по умолчанию.
constexpr size_t kUsbTransferBytes = 16384;
HostVolume g_tangVolume(0, "0:", "no Tang SD card", zifi::FatVolumePort::kDefaultTransfer);
HostVolume g_usbVolume(1, "1:", "no USB drive", kUsbTransferBytes);

// Передача length байтов диску pdrv: новая самая большая — в журнал стенда (его читает
// build_and_test.py, run_usb_share).
void noteTransfer(BYTE pdrv, uint64_t length) {
  Disk& disk = g_disks[pdrv];
  if (length > disk.largest) {
    disk.largest = length;
    std::printf("host disk %u: largest transfer %llu bytes\n", static_cast<unsigned>(pdrv),
                static_cast<unsigned long long>(length));
    std::fflush(stdout);
  }
}

bool copyIn(const fs::path& source, const std::string& target, std::string& error) {
  for (const fs::directory_entry& entry : fs::directory_iterator(source)) {
    const std::string name = entry.path().filename().string();
    const std::string path = target + "/" + name;
    if (entry.is_directory()) {
      const FRESULT made = f_mkdir(path.c_str());
      if (made != FR_OK) {
        error = "mkdir " + path + ": " + std::to_string(made);
        return false;
      }
      if (!copyIn(entry.path(), path, error)) {
        return false;
      }
      continue;
    }
    std::ifstream input(entry.path(), std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    FIL file = {};
    FRESULT result = f_open(&file, path.c_str(), FA_WRITE | FA_CREATE_ALWAYS);
    if (result != FR_OK) {
      error = "open " + path + ": " + std::to_string(result);
      return false;
    }
    UINT written = 0;
    if (!data.empty()) {
      result = f_write(&file, data.data(), static_cast<UINT>(data.size()), &written);
    }
    const FRESULT closed = f_close(&file);
    if (result == FR_OK) {
      result = closed;
    }
    if (result != FR_OK || written != data.size()) {
      error = "write " + path + ": " + std::to_string(result);
      return false;
    }
  }
  return true;
}

// Новый диск pdrv: образ megabytes МБ, файловая система format (FM_FAT32, FM_EXFAT) с кластером
// cluster байт, смонтирован под буквой drive, содержимое — копия папки folder (пусто — пустой).
bool createDisk(BYTE pdrv, const char* drive, const std::string& folder, uint32_t megabytes,
                BYTE format, DWORD cluster, std::string& error) {
  Disk& disk = g_disks[pdrv];
  if (disk.mounted) {
    f_mount(nullptr, drive, 0);
    disk.mounted = false;
  }
  disk.data.assign(static_cast<size_t>(megabytes) * 1024U * 1024U, 0);
  MKFS_PARM options = {};
  options.fmt = format;
  options.au_size = cluster;
  std::vector<uint8_t> work(4096);
  FRESULT result = f_mkfs(drive, &options, work.data(), static_cast<UINT>(work.size()));
  if (result != FR_OK) {
    error = "mkfs: " + std::to_string(result);
    return false;
  }
  result = f_mount(&disk.fs, drive, 1);
  if (result != FR_OK) {
    error = "mount: " + std::to_string(result);
    return false;
  }
  disk.mounted = true;
  if (!folder.empty() && !copyIn(fs::path(folder), drive, error)) {
    return false;
  }
  disk.largest = 0;                     // заполнение образа на ПК — не работа шары
  std::printf("host disk %u: transfers counted from here\n", static_cast<unsigned>(pdrv));
  std::fflush(stdout);
  return true;
}

}  // namespace

// --- устройство FatFs: образы в памяти --------------------------------------------------------

extern "C" DSTATUS disk_initialize(BYTE pdrv) {
  const Disk* disk = diskOf(pdrv);
  return disk == nullptr || disk->data.empty() ? STA_NOINIT : 0;
}
extern "C" DSTATUS disk_status(BYTE pdrv) { return disk_initialize(pdrv); }

extern "C" DRESULT disk_read(BYTE pdrv, BYTE* buffer, LBA_t sector, UINT count) {
  const Disk* disk = diskOf(pdrv);
  const uint64_t at = static_cast<uint64_t>(sector) * kSectorBytes;
  const uint64_t length = static_cast<uint64_t>(count) * kSectorBytes;
  if (disk == nullptr || at + length > disk->data.size()) {
    return RES_PARERR;
  }
  std::memcpy(buffer, disk->data.data() + at, static_cast<size_t>(length));
  noteTransfer(pdrv, length);
  return RES_OK;
}

extern "C" DRESULT disk_write(BYTE pdrv, const BYTE* buffer, LBA_t sector, UINT count) {
  Disk* disk = diskOf(pdrv);
  const uint64_t at = static_cast<uint64_t>(sector) * kSectorBytes;
  const uint64_t length = static_cast<uint64_t>(count) * kSectorBytes;
  if (disk == nullptr || at + length > disk->data.size()) {
    return RES_PARERR;
  }
  std::memcpy(disk->data.data() + at, buffer, static_cast<size_t>(length));
  noteTransfer(pdrv, length);
  return RES_OK;
}

extern "C" DRESULT disk_ioctl(BYTE pdrv, BYTE command, void* buffer) {
  const Disk* disk = diskOf(pdrv);
  if (disk == nullptr) {
    return RES_PARERR;
  }
  switch (command) {
    case CTRL_SYNC:
    case CTRL_TRIM:
      return RES_OK;
    case GET_SECTOR_COUNT:
      *static_cast<LBA_t*>(buffer) = static_cast<LBA_t>(disk->data.size() / kSectorBytes);
      return RES_OK;
    case GET_SECTOR_SIZE:
      *static_cast<WORD*>(buffer) = static_cast<WORD>(kSectorBytes);
      return RES_OK;
    case GET_BLOCK_SIZE:
      *static_cast<DWORD*>(buffer) = 1;
      return RES_OK;
    default:
      return RES_PARERR;
  }
}

// --- служебные функции FatFs (на ESP — ffsystem.c и vfs_fat) ---------------------------------

// Тома FatFs по разделам (FF_MULTI_PARTITION): том N — первый подходящий раздел диска N.
extern "C" PARTITION VolToPart[FF_VOLUMES] = {{0, 0}, {1, 0}};

extern "C" DWORD get_fattime(void) {
  const std::time_t now = std::time(nullptr);
  std::tm local = {};
  localtime_s(&local, &now);
  return static_cast<DWORD>((local.tm_year - 80) << 25) | static_cast<DWORD>((local.tm_mon + 1) << 21) |
         static_cast<DWORD>(local.tm_mday << 16) | static_cast<DWORD>(local.tm_hour << 11) |
         static_cast<DWORD>(local.tm_min << 5) | static_cast<DWORD>(local.tm_sec / 2);
}

extern "C" void* ff_memalloc(UINT size) { return std::malloc(size); }
extern "C" void ff_memfree(void* block) { std::free(block); }
extern "C" int ff_mutex_create(int) { return 1; }
extern "C" void ff_mutex_delete(int) {}
extern "C" int ff_mutex_take(int volume) {
  g_volumeLocks[volume].lock();
  return 1;
}
extern "C" void ff_mutex_give(int volume) { g_volumeLocks[volume].unlock(); }

namespace zifi {
namespace host {

bool createTangCard(const std::string& folder, uint32_t megabytes, std::string& error) {
  return createDisk(0, "0:", folder, megabytes, FM_FAT32, kSectorBytes, error);
}

FatVolumePort& tangVolume() { return g_tangVolume; }
void setTangCardPresent(bool present) { g_disks[0].present = present; }
bool tangCardPresent() { return g_disks[0].present && g_disks[0].mounted; }

bool createUsbFlash(const std::string& folder, uint32_t megabytes, std::string& error) {
  // exFAT, кластер 128 КБ — как у флешки пользователя на плате (60 ГБ). Кластер важен (0.1.173):
  // FatFs передаёт устройству за раз не больше остатка кластера, и при кластере 4 КБ стенд не увидел
  // бы передач по 16 КиБ, которые на 0.1.171–0.1.172 заняли резерв code_ram.
  // ZIFI_HOST_USB_TRANSFER — другой предел передачи тома (контрольный прогон run_usb_share: 4096 —
  // на плате не нашлось памяти вырастить буфер драйвера, и шара обязана уложиться в него).
  if (const char* transfer = std::getenv("ZIFI_HOST_USB_TRANSFER")) {
    g_usbVolume.setTransfer(static_cast<size_t>(std::strtoul(transfer, nullptr, 10)));
  }
  return createDisk(1, "1:", folder, megabytes, FM_EXFAT, 128 * 1024, error);
}

FatVolumePort& usbVolume() { return g_usbVolume; }
void setUsbFlashPresent(bool present) { g_disks[1].present = present; }
bool usbFlashPresent() { return g_disks[1].present && g_disks[1].mounted; }

}  // namespace host
}  // namespace zifi
