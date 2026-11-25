#include "zifi/usb_drive.hpp"

#if ZIFI_HAS_USB_DRIVE

#include <atomic>
#include <string.h>
#include <sys/stat.h>

#include <driver/usb_serial_jtag.h>
#include <esp_heap_caps.h>
#include <esp_intr_alloc.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "esp_private/msc_scsi_bot.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#include "usb/usb_host.h"
#include "zifi/bringup_log.hpp"
#include "zifi/resource_disk_cache.hpp"

namespace zifi {

namespace {

constexpr char kTag[] = "zifi-usb";

// Кэш ресурсов читает и пишет кусками не больше передачи: иначе драйвер станет
// растить свой буфер во внутренней памяти посреди работы плеера.
static_assert(ResourceDiskCache::kChunkBytes <= UsbDrive::kTransferBytes,
              "кусок кэша больше передачи флешке");

// Через сколько после запуска ESP решать, есть ли на разъёме ПК: компьютер
// перечисляет консоль USB Serial/JTAG за доли секунды после сброса ESP.
constexpr uint32_t kPcCheckMs = 3000;
// Сколько после включения хоста ждать флешку, прежде чем записать «нет флешки».
constexpr uint32_t kFirstWaitMs = 3000;
// Не чаще раза в столько ready() просит сбросить порт, когда флешки нет.
constexpr uint32_t kRetryMs = 30000;
// Стеки задач хоста и флешки — в PSRAM: они не трогают flash ESP (журнал —
// очередью), а внутренней памяти при работе плеера остаётся несколько КБ.
constexpr uint32_t kStackBytes = 4096;
constexpr int kCore = 1;
constexpr UBaseType_t kHostPriority = 2;
constexpr UBaseType_t kMscPriority = 3;
constexpr UBaseType_t kDrivePriority = 1;   // как у цикла обмена с Z80: делят ядро поровну

// Что с флешкой (g_state). Меняет задача usb-drive; «вынули → нет» — ядро 1.
enum : uint8_t {
  kWaiting,     // запуск: ждём kPcCheckMs
  kOff,         // хоста нет до перезапуска: к разъёму подключён ПК или хост не запустился
  kEmpty,       // хост работает, флешки нет
  kUnusable,    // флешка открыта, но тома с папками кэша на ней нет
  kReady,       // том в /usb, папки кэша есть
  kRemoved,     // готовую флешку вынули: ядро 1 отмонтирует её
};
std::atomic<uint8_t> g_state{kWaiting};
// Сколько раз флешка становилась готовой с запуска ESP (0.1.171): новое монтирование — новый номер.
// По нему шара SMB «USB» узнаёт, что прежняя флешка вынута и её буква FatFs могла смениться.
std::atomic<uint32_t> g_generation{0};
std::atomic<bool> g_pcOnPort{false};
std::atomic<bool> g_retryPort{false};
std::atomic<uint32_t> g_hostStartedMs{0};

// События драйвера флешек (его задача usb-msc) для задачи usb-drive: адрес
// подключённой флешки, которую ещё не открывали, и признак «вынули».
portMUX_TYPE g_eventLock = portMUX_INITIALIZER_UNLOCKED;
uint8_t g_connectedAddress = 0;
bool g_disconnected = false;
SemaphoreHandle_t g_signal = nullptr;   // будит задачу usb-drive

// Открытая флешка и её том. Пишет задача usb-drive (открытие; закрытие негодной)
// и ядро 1 (закрытие вынутой готовой) — никогда одновременно: по g_state.
msc_host_device_handle_t g_device = nullptr;
msc_host_vfs_handle_t g_vfs = nullptr;

uint32_t uptimeMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void mscEvent(const msc_host_event_t* event, void*) {
  portENTER_CRITICAL(&g_eventLock);
  if (event->event == msc_host_event_t::MSC_DEVICE_CONNECTED) {
    g_connectedAddress = event->device.address;
  } else if (event->event == msc_host_event_t::MSC_DEVICE_DISCONNECTED) {
    g_disconnected = true;
  }
  portEXIT_CRITICAL(&g_eventLock);
  if (g_signal != nullptr) {
    xSemaphoreGive(g_signal);
  }
}

// Библиотека USB-хоста: подключение, перечисление, отключение.
void hostTask(void*) {
  for (;;) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if ((flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) != 0) {
      usb_host_device_free_all();
    }
  }
}

// Драйвер флешек: передачи и события «подключили / вынули».
void mscTask(void*) {
  for (;;) {
    if (msc_host_handle_events(portMAX_DELAY) != ESP_OK) {
      vTaskDelay(pdMS_TO_TICKS(10));
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
// MBR, у которого он есть. Сектор читается в PSRAM: драйвер копирует его из
// своего буфера.
Volume probe(msc_host_device_handle_t device, uint32_t sectorSize, uint32_t sectorCount) {
  if (sectorSize < 512 || sectorSize > 4096) {
    return Volume::kNone;
  }
  uint8_t* sector =
      static_cast<uint8_t*>(heap_caps_malloc(sectorSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
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

// Буфер обмена драйвера — сразу до kTransferBytes, одним чтением такой длины:
// дальше передачи не больше его, и внутренней памяти драйвер уже не просит.
bool growTransferBuffer(msc_host_device_handle_t device, uint32_t sectorSize) {
  if (sectorSize == 0 || UsbDrive::kTransferBytes % sectorSize != 0) {
    return false;
  }
  uint8_t* data = static_cast<uint8_t*>(
      heap_caps_malloc(UsbDrive::kTransferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (data == nullptr) {
    return false;
  }
  const bool read = scsi_cmd_read10(device, data, 0, UsbDrive::kTransferBytes / sectorSize,
                                    sectorSize) == ESP_OK;
  heap_caps_free(data);
  return read;
}

void closeDrive() {
  if (g_vfs != nullptr) {
    msc_host_vfs_unregister(g_vfs);
    g_vfs = nullptr;
  }
  if (g_device != nullptr) {
    msc_host_uninstall_device(g_device);
    g_device = nullptr;
  }
}

// Открыть флешку по адресу, проверить том, смонтировать, завести папки кэша.
// Итог — новое состояние: kEmpty (не открылась), kUnusable или kReady.
uint8_t openDrive(uint8_t address) {
  msc_host_device_handle_t device = nullptr;
  esp_err_t result = msc_host_install_device(address, &device);
  if (result != ESP_OK) {
    bringupLog(kTag, "drive at %u: not opened (%s)", static_cast<unsigned>(address),
               esp_err_to_name(result));
    return kEmpty;
  }
  g_device = device;
  msc_host_device_info_t info = {};
  msc_host_get_device_info(device, &info);
  const uint32_t megabytes = static_cast<uint32_t>(
      static_cast<uint64_t>(info.sector_count) * info.sector_size / (1024 * 1024));
  const Volume volume = probe(device, info.sector_size, info.sector_count);
  if (volume != Volume::kFat32 && volume != Volume::kExfat) {
    bringupLog(kTag, "drive %04X:%04X, %u MB, %s: not used", info.idVendor, info.idProduct,
               static_cast<unsigned>(megabytes), volumeName(volume));
    return kUnusable;
  }
  // Буфер — до монтирования (0.1.147): FatFs пишет и читает том с первых же команд, и
  // ни одна передача после этого не должна растить буфер.
  const bool grown = growTransferBuffer(device, info.sector_size);
  if (!grown) {
    bringupLog(kTag, "drive %04X:%04X: transfer buffer NOT reserved, not used", info.idVendor,
               info.idProduct);
    return kUnusable;
  }
  esp_vfs_fat_mount_config_t mount = {};
  mount.format_if_mount_failed = false;   // чужую флешку не трогаем
  mount.max_files = 4;
  mount.allocation_unit_size = 0;
  msc_host_vfs_handle_t vfs = nullptr;
  result = msc_host_vfs_register(device, UsbDrive::kMountPoint, &mount, &vfs);
  if (result != ESP_OK) {
    bringupLog(kTag, "drive %04X:%04X, %s: not mounted (%s)", info.idVendor, info.idProduct,
               volumeName(volume), esp_err_to_name(result));
    return kUnusable;
  }
  g_vfs = vfs;
  mkdir("/usb/SYSTEM", 0777);
  mkdir("/usb/SYSTEM/CACHE", 0777);
  mkdir(UsbDrive::kMusicCache, 0777);
  mkdir(UsbDrive::kElfFolder, 0777);
  mkdir(UsbDrive::kCoreFolder, 0777);
  const bool folders = isDirectory(UsbDrive::kMusicCache) && isDirectory(UsbDrive::kElfFolder) &&
                       isDirectory(UsbDrive::kCoreFolder);
  bringupLog(kTag, "drive %04X:%04X, %u MB, %s: %s", info.idVendor, info.idProduct,
             static_cast<unsigned>(megabytes), volumeName(volume),
             folders ? "music cache /SYSTEM/CACHE/MUSIC, ELF cache /ELF, core cache /cores" : "cache folders NOT made");
  return folders ? kReady : kUnusable;
}

bool startHost() {
  usb_host_config_t host = {};
  host.intr_flags = ESP_INTR_FLAG_LEVEL1;
  esp_err_t result = usb_host_install(&host);
  if (result != ESP_OK) {
    bringupLog(kTag, "USB host: not started (%s)", esp_err_to_name(result));
    return false;
  }
  if (xTaskCreatePinnedToCoreWithCaps(hostTask, "usb-host", kStackBytes, nullptr, kHostPriority,
                                      nullptr, kCore, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) !=
      pdPASS) {
    bringupLog(kTag, "USB host: task not started");
    return false;
  }
  msc_host_driver_config_t msc = {};
  msc.create_backround_task = false;      // своя задача ниже — со стеком в PSRAM
  msc.callback = mscEvent;
  result = msc_host_install(&msc);
  if (result != ESP_OK) {
    bringupLog(kTag, "USB drive driver: not started (%s)", esp_err_to_name(result));
    return false;
  }
  if (xTaskCreatePinnedToCoreWithCaps(mscTask, "usb-msc", kStackBytes, nullptr, kMscPriority,
                                      nullptr, kCore, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) !=
      pdPASS) {
    bringupLog(kTag, "USB drive driver: task not started");
    return false;
  }
  bringupLog(kTag, "USB host started: the USB console is off until restart");
  return true;
}

// Задача usb-drive: решает, включать ли хост, открывает вставленную флешку,
// закрывает негодную вынутую, сбрасывает порт по просьбе ready().
void driveTask(void*) {
  const uint32_t now = uptimeMs();
  if (now < kPcCheckMs) {
    vTaskDelay(pdMS_TO_TICKS(kPcCheckMs - now));
  }
  if (usb_serial_jtag_is_connected()) {
    g_pcOnPort.store(true);
    g_state.store(kOff);
    bringupLog(kTag, "a PC is on the USB port: USB console stays, no flash drive until restart");
    vTaskSuspend(nullptr);
  }
  if (!startHost()) {
    g_state.store(kOff);
    vTaskSuspend(nullptr);
  }
  g_hostStartedMs.store(uptimeMs() | 1);
  g_state.store(kEmpty);
  bool waitReported = false;
  uint8_t pending = 0;
  for (;;) {
    xSemaphoreTake(g_signal, pdMS_TO_TICKS(500));
    portENTER_CRITICAL(&g_eventLock);
    const uint8_t address = g_connectedAddress;
    const bool gone = g_disconnected;
    g_connectedAddress = 0;
    g_disconnected = false;
    portEXIT_CRITICAL(&g_eventLock);
    if (address != 0) {
      pending = address;
    }
    if (gone) {
      const uint8_t state = g_state.load();
      if (state == kReady) {
        g_state.store(kRemoved);          // её файлы мог открыть кто угодно: закроет ядро 1
        bringupLog(kTag, "drive removed");
      } else if (state == kUnusable) {
        closeDrive();
        g_state.store(kEmpty);
        bringupLog(kTag, "drive removed");
      }
    }
    if (pending != 0 && g_state.load() == kEmpty) {
      const uint8_t opened = openDrive(pending);
      pending = 0;
      waitReported = true;
      if (opened == kEmpty) {
        closeDrive();
      }
      if (opened == kReady) {
        ++g_generation;                   // до «готова»: увидевший kReady видит и новый номер
      }
      g_state.store(opened);
    }
    if (!waitReported && g_state.load() == kEmpty &&
        uptimeMs() - g_hostStartedMs.load() >= kFirstWaitMs) {
      waitReported = true;
      bringupLog(kTag, "no drive in %u ms after the USB host start",
                 static_cast<unsigned>(kFirstWaitMs));
    }
    // Подключение могло сорваться (на 0.1.145 — нехватка памяти): флешка в разъёме,
    // а события нет. Выключенный и снова включённый порт хост перечисляет заново.
    if (g_retryPort.exchange(false) && g_state.load() == kEmpty) {
      usb_host_lib_set_root_port_power(false);
      vTaskDelay(pdMS_TO_TICKS(100));
      usb_host_lib_set_root_port_power(true);
    }
  }
}

}  // namespace

UsbDrive& usbDrive() {
  static UsbDrive drive;
  return drive;
}

void UsbDrive::begin() {
  if (g_signal != nullptr) {
    return;
  }
  g_signal = xSemaphoreCreateBinary();
  if (g_signal == nullptr ||
      xTaskCreatePinnedToCoreWithCaps(driveTask, "usb-drive", kStackBytes, nullptr,
                                      kDrivePriority, nullptr, kCore,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    g_state.store(kOff);
    bringupLog(kTag, "USB drive task not started");
  }
}

void UsbDrive::poll() {
  if (g_state.load() == kRemoved) {
    detach();
  }
}

void UsbDrive::detach() {
  closeDrive();
  g_state.store(kEmpty);
  xSemaphoreGive(g_signal);               // флешку могли уже вставить снова
}

bool UsbDrive::ready() {
  poll();
  const uint8_t state = g_state.load();
  if (state == kEmpty) {
    const uint32_t now = uptimeMs();
    const uint32_t since = lastRetryMs_ != 0 ? lastRetryMs_ : g_hostStartedMs.load();
    if (now - since >= kRetryMs) {
      lastRetryMs_ = now | 1;
      g_retryPort.store(true);
      xSemaphoreGive(g_signal);
    }
  }
  return state == kReady;
}

bool UsbDrive::isReady() const { return g_state.load() == kReady; }

uint32_t UsbDrive::generation() const { return g_generation.load(); }

const char* UsbDrive::status() const {
  switch (g_state.load()) {
    case kWaiting:
      return "USB host not started yet";
    case kOff:
      return g_pcOnPort.load() ? "a PC is on the USB port" : "USB host not started";
    case kEmpty:
      return "no drive";
    case kUnusable:
      return "drive without FAT32/exFAT cache folders";
    case kRemoved:
      return "drive removed";
    default:
      return "drive ready";
  }
}

}  // namespace zifi

#endif  // ZIFI_HAS_USB_DRIVE
