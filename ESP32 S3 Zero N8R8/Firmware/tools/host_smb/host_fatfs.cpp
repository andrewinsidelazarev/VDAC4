// microSD Tang на стенде ПК: диск FatFs в памяти и служебные функции FatFs (на плате их дают
// ESP-IDF и tang_sd). Сама FatFs — components/fatfs/src/ff.c прошивки, собранная для ПК.

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
std::vector<uint8_t> g_disk;
FATFS g_fs;
bool g_mounted = false;
bool g_present = true;
std::recursive_mutex g_volumeLocks[FF_VOLUMES + 1];

class HostVolume : public zifi::FatVolumePort {
 public:
  const char* acquire() override {
    if (!g_present) {
      return "no Tang SD card";
    }
    return g_mounted ? nullptr : "Tang SD is not mounted";
  }
  void release() override {}
  const char* drive() const override { return "0:"; }
};

HostVolume g_volume;

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

}  // namespace

// --- устройство FatFs: образ в памяти --------------------------------------------------------

extern "C" DSTATUS disk_initialize(BYTE) { return g_disk.empty() ? STA_NOINIT : 0; }
extern "C" DSTATUS disk_status(BYTE) { return g_disk.empty() ? STA_NOINIT : 0; }

extern "C" DRESULT disk_read(BYTE, BYTE* buffer, LBA_t sector, UINT count) {
  const uint64_t at = static_cast<uint64_t>(sector) * kSectorBytes;
  const uint64_t length = static_cast<uint64_t>(count) * kSectorBytes;
  if (at + length > g_disk.size()) {
    return RES_PARERR;
  }
  std::memcpy(buffer, g_disk.data() + at, static_cast<size_t>(length));
  return RES_OK;
}

extern "C" DRESULT disk_write(BYTE, const BYTE* buffer, LBA_t sector, UINT count) {
  const uint64_t at = static_cast<uint64_t>(sector) * kSectorBytes;
  const uint64_t length = static_cast<uint64_t>(count) * kSectorBytes;
  if (at + length > g_disk.size()) {
    return RES_PARERR;
  }
  std::memcpy(g_disk.data() + at, buffer, static_cast<size_t>(length));
  return RES_OK;
}

extern "C" DRESULT disk_ioctl(BYTE, BYTE command, void* buffer) {
  switch (command) {
    case CTRL_SYNC:
    case CTRL_TRIM:
      return RES_OK;
    case GET_SECTOR_COUNT:
      *static_cast<LBA_t*>(buffer) = static_cast<LBA_t>(g_disk.size() / kSectorBytes);
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
  if (g_mounted) {
    f_mount(nullptr, "0:", 0);
    g_mounted = false;
  }
  g_disk.assign(static_cast<size_t>(megabytes) * 1024U * 1024U, 0);
  MKFS_PARM options = {};
  options.fmt = FM_FAT32;
  options.au_size = kSectorBytes;
  std::vector<uint8_t> work(4096);
  FRESULT result = f_mkfs("0:", &options, work.data(), static_cast<UINT>(work.size()));
  if (result != FR_OK) {
    error = "mkfs: " + std::to_string(result);
    return false;
  }
  result = f_mount(&g_fs, "0:", 1);
  if (result != FR_OK) {
    error = "mount: " + std::to_string(result);
    return false;
  }
  g_mounted = true;
  if (!folder.empty() && !copyIn(fs::path(folder), "0:", error)) {
    return false;
  }
  return true;
}

FatVolumePort& tangVolume() { return g_volume; }
void setTangCardPresent(bool present) { g_present = present; }
bool tangCardPresent() { return g_present && g_mounted; }

}  // namespace host
}  // namespace zifi
