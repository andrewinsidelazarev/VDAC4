#include "zifi/usb_drive.hpp"

#if ZIFI_HAS_USB_DRIVE

#include <string.h>
#include <sys/stat.h>

#include <esp_heap_caps.h>
#include <esp_intr_alloc.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "esp_private/msc_scsi_bot.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#include "usb/usb_host.h"
#include "zifi/bringup_log.hpp"

namespace zifi {

namespace {

constexpr char kTag[] = "zifi-usb";

// События драйвера флешек (его задача) для ядра 1: адрес подключённой флешки,
// которую ещё не проверяли, и признак «вынули».
portMUX_TYPE g_eventLock = portMUX_INITIALIZER_UNLOCKED;
volatile uint8_t g_connectedAddress = 0;
volatile bool g_disconnected = false;
SemaphoreHandle_t g_connectSignal = nullptr;

void mscEvent(const msc_host_event_t* event, void*) {
  const bool connected = event->event == msc_host_event_t::MSC_DEVICE_CONNECTED;
  portENTER_CRITICAL(&g_eventLock);
  if (connected) {
    g_connectedAddress = event->device.address;
  } else if (event->event == msc_host_event_t::MSC_DEVICE_DISCONNECTED) {
    g_disconnected = true;
  }
  portEXIT_CRITICAL(&g_eventLock);
  if (connected && g_connectSignal != nullptr) {
    xSemaphoreGive(g_connectSignal);
  }
}

// Обработчик библиотеки USB-хоста: подключение, перечисление, отключение.
void hostTask(void*) {
  for (;;) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if ((flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) != 0) {
      usb_host_device_free_all();
    }
  }
}

// Что на флешке, по загрузочному сектору тома или таблице разделов.
enum class Volume : uint8_t { kNone, kOldFat, kFat32, kExfat, kTable, kGpt, kNtfs };

const char* volumeName(Volume volume) {
  switch (volume) {
    case Volume::kFat32:
      return "FAT32";
    case Volume::kExfat:
      return "exFAT";
    case Volume::kOldFat:
      return "FAT12/16";
    case Volume::kGpt:
      return "GPT partitions";
    case Volume::kNtfs:
      return "NTFS";
    default:
      return "no file system";
  }
}

uint32_t le32(const uint8_t* p) {
  return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24;
}

// Как check_fs FatFs: сначала загрузочный сектор тома (exFAT — по имени, FAT —
// по полям BPB: у FAT32 длина FAT в 16-битном поле нулевая), иначе — MBR.
Volume bootKind(const uint8_t* s) {
  if (s[510] != 0x55 || s[511] != 0xAA) {
    return Volume::kNone;
  }
  if (memcmp(s + 3, "EXFAT   ", 8) == 0) {
    return Volume::kExfat;
  }
  if (memcmp(s + 3, "NTFS    ", 8) == 0) {
    return Volume::kNtfs;
  }
  const bool jump = s[0] == 0xEB || s[0] == 0xE9 || s[0] == 0xE8;
  const uint32_t bytes = s[11] | s[12] << 8;
  const uint8_t cluster = s[13];
  const bool bpb = jump && bytes >= 512 && bytes <= 4096 && (bytes & (bytes - 1)) == 0 &&
                   cluster != 0 && (cluster & (cluster - 1)) == 0 && s[16] != 0;
  if (bpb) {
    const uint32_t fat16 = s[22] | s[23] << 8;
    if (fat16 == 0 && le32(s + 36) != 0) {
      return Volume::kFat32;
    }
    if (fat16 != 0) {
      return Volume::kOldFat;
    }
  }
  for (int i = 0; i < 4; ++i) {
    if (s[446 + 16 * i + 4] == 0xEE) {
      return Volume::kGpt;               // FatFs без 64-битных номеров секторов GPT не читает
    }
  }
  return Volume::kTable;
}

// Том, который смонтирует FatFs: загрузочный сектор с LBA 0 или первый раздел
// MBR, у которого он есть.
Volume probe(msc_host_device_handle_t device, uint32_t sectorSize, uint32_t sectorCount) {
  if (sectorSize < 512 || sectorSize > 4096) {
    return Volume::kNone;
  }
  uint8_t* sector = static_cast<uint8_t*>(
      heap_caps_malloc(sectorSize, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  if (sector == nullptr) {
    return Volume::kNone;
  }
  Volume volume = Volume::kNone;
  if (scsi_cmd_read10(device, sector, 0, 1, sectorSize) == ESP_OK) {
    volume = bootKind(sector);
    if (volume == Volume::kTable) {
      uint32_t starts[4];
      for (int i = 0; i < 4; ++i) {
        const uint8_t* entry = sector + 446 + 16 * i;
        starts[i] = entry[4] != 0 ? le32(entry + 8) : 0;
      }
      volume = Volume::kNone;
      for (int i = 0; i < 4 && volume == Volume::kNone; ++i) {
        if (starts[i] == 0 || starts[i] >= sectorCount ||
            scsi_cmd_read10(device, sector, starts[i], 1, sectorSize) != ESP_OK) {
          continue;
        }
        const Volume found = bootKind(sector);
        if (found == Volume::kFat32 || found == Volume::kExfat || found == Volume::kOldFat ||
            found == Volume::kNtfs) {
          volume = found;
        }
      }
    }
  }
  heap_caps_free(sector);
  return volume;
}

bool isDirectory(const char* path) {
  struct stat info;
  return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

}  // namespace

UsbDrive& usbDrive() {
  static UsbDrive drive;
  return drive;
}

bool UsbDrive::startHost() {
  g_connectSignal = xSemaphoreCreateBinary();
  usb_host_config_t host = {};
  host.intr_flags = ESP_INTR_FLAG_LEVEL1;
  esp_err_t result = g_connectSignal != nullptr ? usb_host_install(&host) : ESP_ERR_NO_MEM;
  if (result != ESP_OK) {
    bringupLog(kTag, "USB host: not started (%s)", esp_err_to_name(result));
    return false;
  }
  // Ядро 1, ниже звука и обмена с Z80: флешка нужна только при доставке ресурса.
  xTaskCreatePinnedToCore(hostTask, "usb-host", 4096, nullptr, 2, nullptr, 1);
  msc_host_driver_config_t msc = {};
  msc.create_backround_task = true;
  msc.task_priority = 3;
  msc.stack_size = 4096;
  msc.core_id = 1;
  msc.callback = mscEvent;
  result = msc_host_install(&msc);
  if (result != ESP_OK) {
    bringupLog(kTag, "USB drive driver: not started (%s)", esp_err_to_name(result));
    return false;
  }
  bringupLog(kTag, "USB host started: the USB console is off until restart");
  return true;
}

void UsbDrive::detach() {
  if (vfs_ != nullptr) {
    msc_host_vfs_unregister(static_cast<msc_host_vfs_handle_t>(vfs_));
    vfs_ = nullptr;
  }
  if (device_ != nullptr) {
    msc_host_uninstall_device(static_cast<msc_host_device_handle_t>(device_));
    device_ = nullptr;
  }
  usable_ = false;
}

void UsbDrive::attach(uint8_t address) {
  msc_host_device_handle_t device = nullptr;
  esp_err_t result = msc_host_install_device(address, &device);
  if (result != ESP_OK) {
    bringupLog(kTag, "drive at %u: not opened (%s)", static_cast<unsigned>(address),
               esp_err_to_name(result));
    return;
  }
  device_ = device;
  msc_host_device_info_t info = {};
  msc_host_get_device_info(device, &info);
  const uint32_t megabytes = static_cast<uint32_t>(
      static_cast<uint64_t>(info.sector_count) * info.sector_size / (1024 * 1024));
  const Volume volume = probe(device, info.sector_size, info.sector_count);
  if (volume != Volume::kFat32 && volume != Volume::kExfat) {
    bringupLog(kTag, "drive %04X:%04X, %u MB, %s: not used", info.idVendor, info.idProduct,
               static_cast<unsigned>(megabytes), volumeName(volume));
    return;
  }
  esp_vfs_fat_mount_config_t mount = {};
  mount.format_if_mount_failed = false;   // чужую флешку не трогаем
  mount.max_files = 4;
  mount.allocation_unit_size = 0;
  msc_host_vfs_handle_t vfs = nullptr;
  result = msc_host_vfs_register(device, kMountPoint, &mount, &vfs);
  if (result != ESP_OK) {
    bringupLog(kTag, "drive %04X:%04X, %s: not mounted (%s)", info.idVendor, info.idProduct,
               volumeName(volume), esp_err_to_name(result));
    return;
  }
  vfs_ = vfs;
  mkdir("/usb/SYSTEM", 0777);
  mkdir("/usb/SYSTEM/CACHE", 0777);
  mkdir(kMusicCache, 0777);
  usable_ = isDirectory(kMusicCache);
  bringupLog(kTag, "drive %04X:%04X, %u MB, %s: %s", info.idVendor, info.idProduct,
             static_cast<unsigned>(megabytes), volumeName(volume),
             usable_ ? "music cache /SYSTEM/CACHE/MUSIC" : "cache folder NOT made");
}

void UsbDrive::handleEvents() {
  portENTER_CRITICAL(&g_eventLock);
  const uint8_t address = g_connectedAddress;
  const bool gone = g_disconnected;
  g_connectedAddress = 0;
  g_disconnected = false;
  portEXIT_CRITICAL(&g_eventLock);
  if (gone && device_ != nullptr) {
    bringupLog(kTag, "drive removed");
    detach();
  }
  if (address != 0) {
    detach();
    attach(address);
  }
}

bool UsbDrive::ready() {
  if (hostFailed_) {
    return false;
  }
  if (!hostStarted_) {
    if (!startHost()) {
      hostFailed_ = true;
      return false;
    }
    hostStarted_ = true;
    // Вставленная заранее флешка подключается за доли секунды.
    if (xSemaphoreTake(g_connectSignal, pdMS_TO_TICKS(kFirstWaitMs)) != pdTRUE) {
      bringupLog(kTag, "no drive in %u ms", static_cast<unsigned>(kFirstWaitMs));
    }
  }
  handleEvents();
  return usable_;
}

}  // namespace zifi

#endif  // ZIFI_HAS_USB_DRIVE
