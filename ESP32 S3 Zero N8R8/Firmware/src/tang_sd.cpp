#include "zifi/tang_sd.hpp"

#if ZIFI_HAS_SPI_LINK

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

#include "diskio_impl.h"
#include "ff.h"
#include "zifi/bringup_log.hpp"
#include "zifi/sd_bridge_card.hpp"
#include "zifi/tang_link.hpp"

namespace zifi {
namespace tang_sd {
namespace {

constexpr const char* kTag = "tangsd";
constexpr uint32_t kMinCore = 0x011E;          // TS-Config 1.30: мост к microSD
constexpr uint32_t kPersonalityTs = 0x01;      // VERSION[31:24]: TS-Config (кадровый буфер)
// Ядро NES 0.7+ (VERSION 0401xxxx): тот же мост fb_sdspi по тем же адресам LINK — меню выбора
// образа и снимки во время игры (nes-vdac4/fpga/nes_link.sv). Мост проверяется и по метке SD_STAT.
constexpr uint32_t kPersonalityNes = 0x04;
constexpr uint32_t kMinNes = 0x0007;
// Ядра с личностью 05 и выше (Amiga 05, Mega Drive 06 и следующие, 03.11.2026): тот же мост fb_sdspi
// по тем же адресам LINK, есть ли он — проверяется по метке SD_STAT (present ниже). Новому ядру с
// мостом прошивка не нужна. 02 (FT812) и 03 (Next: своя карта у NextZXOS) моста не имеют.
constexpr uint32_t kPersonalityAmiga = 0x05;
constexpr uint32_t kPersonalityLast = 0x7F;

bool linkRead(void*, uint32_t address, uint32_t* words, size_t count) {
  return tang::read(address, words, count);
}
bool linkWrite(void*, uint32_t address, const uint32_t* words, size_t count) {
  return tang::write(address, words, count);
}
uint32_t nowMs(void*) { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
void yieldMs(void*) { vTaskDelay(1); }  // 1 тик (1 мс): ядру ESP — его фоновая задача IDLE

const SdBridgeIo g_io = {nullptr, linkRead, linkWrite, nowMs, yieldMs};

// Длинные проходы FatFs (выделение места под образ в сотни МБ) не отдают процессор сами: задача ядра 0
// не должна держать его дольше сторожа IDLE.
void diskPause() {
  static uint32_t operations = 0;
  if (++operations >= 32) {
    operations = 0;
    vTaskDelay(1);
  }
}
SdBridgeCard g_card(g_io);
FATFS* g_fs = nullptr;
BYTE g_pdrv = 0xFF;
char g_drive[4] = "";
uint32_t g_reloads = 0;      // перезагрузок ПЛИС при монтировании: сменилось — моста нет
bool g_mounted = false;

bool sameCore() { return tang::ready() && tang::reloads() == g_reloads; }

DSTATUS diskInit(BYTE) { return g_card.ready() && sameCore() ? 0 : STA_NOINIT; }
DSTATUS diskStatus(BYTE) { return g_card.ready() && sameCore() ? 0 : STA_NOINIT; }

DRESULT diskRead(BYTE, BYTE* buff, uint32_t sector, unsigned count) {
  if (!sameCore()) {
    return RES_NOTRDY;
  }
  diskPause();
  tang::LinkGuard guard;
  return g_card.read(sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT diskWrite(BYTE, const BYTE* buff, uint32_t sector, unsigned count) {
  if (!sameCore()) {
    return RES_NOTRDY;
  }
  diskPause();
  tang::LinkGuard guard;
  return g_card.write(sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT diskIoctl(BYTE, BYTE cmd, void* buff) {
  switch (cmd) {
    case CTRL_SYNC: return RES_OK;      // запись блока кончается, когда карта её закончила
    case GET_SECTOR_COUNT: *static_cast<LBA_t*>(buff) = g_card.sectors(); return RES_OK;
    case GET_SECTOR_SIZE: *static_cast<WORD*>(buff) = SdBridgeCard::kBlock; return RES_OK;
    case GET_BLOCK_SIZE: *static_cast<DWORD*>(buff) = 1; return RES_OK;
    default: return RES_PARERR;
  }
}

const ff_diskio_impl_t g_impl = {diskInit, diskStatus, diskRead, diskWrite, diskIoctl};

void release() {
  if (sameCore()) {
    tang::LinkGuard guard;
    g_card.release();
  }
}

SemaphoreHandle_t sessionLock() {
  static SemaphoreHandle_t lock = nullptr;
  static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
  if (lock == nullptr) {
    SemaphoreHandle_t made = xSemaphoreCreateRecursiveMutex();
    portENTER_CRITICAL(&guard);
    if (lock == nullptr) {
      lock = made;
      made = nullptr;
    }
    portEXIT_CRITICAL(&guard);
    if (made != nullptr) {
      vSemaphoreDelete(made);
    }
  }
  return lock;
}

}  // namespace

Session::Session(uint32_t waitMs) {
  SemaphoreHandle_t lock = sessionLock();
  ok_ = lock != nullptr &&
        xSemaphoreTakeRecursive(lock, waitMs == kForever ? portMAX_DELAY : pdMS_TO_TICKS(waitMs)) == pdTRUE;
}

Session::~Session() {
  if (ok_) {
    xSemaphoreGiveRecursive(sessionLock());
  }
}

bool mounted() { return g_mounted && sameCore(); }

const char* fatDrive() { return g_mounted ? g_drive : ""; }

const char* mount() {
  Session session;
  if (heldByWc()) {
    return kHeldByWc;
  }
  if (g_mounted) {
    if (sameCore()) {
      return nullptr;
    }
    unmount();                  // ядро сменилось: мост новый, карту — заново
  }
  if (!tang::ready()) {
    return "LINK is not ready";
  }
  const uint32_t version = tang::coreVersion();
  const bool ts = (version >> 24) == kPersonalityTs && (version & 0xFFFF) >= kMinCore;
  const bool nes = (version >> 24) == kPersonalityNes && (version & 0xFFFF) >= kMinNes;
  const bool other = (version >> 24) >= kPersonalityAmiga && (version >> 24) <= kPersonalityLast;
  if (!ts && !nes && !other) {
    return "the Tang core has no SD bridge (TS-Config 1.30+, NES 0.7+, Amiga, Mega Drive or newer needed)";
  }
  if (nes || other) {
    bool present = false;
    {
      tang::LinkGuard guard;
      present = g_card.present();
    }
    if (!present) {
      return "the Tang core does not answer as an SD bridge";
    }
  }
  g_reloads = tang::reloads();
  const char* problem = nullptr;
  {
    tang::LinkGuard guard;
    problem = g_card.init();
  }
  if (problem != nullptr) {
    bringupLog(kTag, "card: %s", problem);
    return problem;
  }
  if (ff_diskio_get_drive(&g_pdrv) != ESP_OK) {
    release();
    return "no free FatFs drive";
  }
  ff_diskio_register(g_pdrv, &g_impl);
  snprintf(g_drive, sizeof(g_drive), "%u:", static_cast<unsigned>(g_pdrv));
  esp_vfs_fat_conf_t conf = {};
  conf.base_path = kMountPoint;
  conf.fat_drive = g_drive;
  conf.max_files = 4;
  if (esp_vfs_fat_register(&conf, &g_fs) != ESP_OK) {
    ff_diskio_unregister(g_pdrv);
    release();
    return "VFS register failed";
  }
  const FRESULT fr = f_mount(g_fs, g_drive, 1);
  if (fr != FR_OK) {
    esp_vfs_fat_unregister_path(kMountPoint);
    ff_diskio_unregister(g_pdrv);
    g_fs = nullptr;
    release();
    bringupLog(kTag, "card %u MB: no FAT32/exFAT volume (FatFs %d)",
               static_cast<unsigned>(g_card.sectors() / 2048), static_cast<int>(fr));
    return "no FAT32/exFAT volume on the Tang SD card";
  }
  g_mounted = true;
  bringupLog(kTag, "card %u MB, %s, SCK %u kHz: mounted at %s",
             static_cast<unsigned>(g_card.sectors() / 2048), g_card.highCapacity() ? "SDHC/SDXC" : "SDSC",
             static_cast<unsigned>(g_card.sckKhz()), kMountPoint);
  return nullptr;
}

void unmount() {
  Session session;
  if (!g_mounted) {
    return;
  }
  f_mount(nullptr, g_drive, 0);
  esp_vfs_fat_unregister_path(kMountPoint);
  ff_diskio_unregister(g_pdrv);
  g_fs = nullptr;
  g_mounted = false;
  release();
}

bool readBlocks(uint32_t lba, uint8_t* data, uint32_t count) {
  Session session;
  if (!mounted()) {
    return false;
  }
  tang::LinkGuard guard;
  return g_card.read(lba, data, count);
}

const char* holdForWc() {
  Session session(kQuickMs);
  if (!session.ok()) {
    return kBusy;
  }
  if (heldByWc()) {
    // Ядро сменилось под картой: кэш WC уже не про неё — только снять SD2 и подключить заново
    return sameCore() ? nullptr : "the Tang core changed while WC held the card: switch SD2 off in F10";
  }
  unmount();                    // FatFs ESP — вон: после записей WC её FAT и каталоги были бы чужими
  if (!tang::ready()) {
    return "LINK is not ready";
  }
  const uint32_t version = tang::coreVersion();
  if ((version >> 24) != kPersonalityTs || (version & 0xFFFF) < kMinCore) {
    return "the Tang core has no SD bridge (TS-Config 1.30+ in slot 0 needed)";
  }
  g_reloads = tang::reloads();
  const char* problem = nullptr;
  {
    tang::LinkGuard guard;
    problem = g_card.init();
  }
  if (problem != nullptr) {
    bringupLog(kTag, "card: %s", problem);
    return problem;
  }
  setHeldByWc(true);
  tang::disarmBoot();           // заставка WC на FT812 не уведёт TS-Config в ядро FT посреди записи
  bringupLog(kTag, "card %u MB, %s: held by WC (SD2 = Tang card), FatFs of ESP off",
             static_cast<unsigned>(g_card.sectors() / 2048), g_card.highCapacity() ? "SDHC/SDXC" : "SDSC");
  return nullptr;
}

void releaseFromWc() {
  Session session;
  if (!heldByWc()) {
    return;
  }
  setHeldByWc(false);
  release();
  tang::rearmBoot();
  bringupLog(kTag, "card released by WC");
}

bool rawRead(uint32_t lba, uint8_t* data) {
  Session session(kQuickMs);
  if (!session.ok() || !heldByWc() || !sameCore()) {
    return false;
  }
  tang::LinkGuard guard;
  return g_card.read(lba, data, 1);
}

bool rawWrite(uint32_t lba, const uint8_t* data) {
  Session session(kQuickMs);
  if (!session.ok() || !heldByWc() || !sameCore()) {
    return false;
  }
  tang::LinkGuard guard;
  return g_card.write(lba, data, 1);
}

// Быстрый поиск FatFs: без таблицы переход назад по большому файлу проходит цепочку кластеров
// с начала. Таблица на 512 кусков файла; образ сильнее разбит — работает и без неё, медленнее.
constexpr uint32_t kDiskTableWords = 1024;

struct DiskFile {
  FIL fil;
  uint32_t sectors;
  bool writable;
  DWORD table[kDiskTableWords];
};

int diskOpen(const char* path, bool writable, DiskFile** file, uint32_t* sectors) {
  *file = nullptr;
  *sectors = 0;
  auto* made = static_cast<DiskFile*>(heap_caps_calloc(1, sizeof(DiskFile), MALLOC_CAP_SPIRAM));
  if (made == nullptr) {
    return 2;
  }
  char name[528];
  snprintf(name, sizeof(name), "%s%s", g_drive, path);
  const FRESULT fr = f_open(&made->fil, name, FA_READ | FA_OPEN_EXISTING | (writable ? FA_WRITE : 0));
  if (fr != FR_OK) {
    heap_caps_free(made);
    return fr == FR_NO_FILE || fr == FR_NO_PATH ? 1 : 2;
  }
  const FSIZE_t size = f_size(&made->fil);
  if (size == 0 || (size & 511) != 0 || (size >> 9) > 0xFFFFFFFFull) {
    f_close(&made->fil);
    heap_caps_free(made);
    return 2;
  }
  made->sectors = static_cast<uint32_t>(size >> 9);
  made->writable = writable;
#if FF_USE_FASTSEEK
  made->table[0] = kDiskTableWords;
  made->fil.cltbl = made->table;
  if (f_lseek(&made->fil, CREATE_LINKMAP) != FR_OK) {
    made->fil.cltbl = nullptr;
  }
#endif
  *file = made;
  *sectors = made->sectors;
  return 0;
}

bool diskRead(DiskFile* file, uint32_t sector, uint8_t* data, uint32_t count) {
  if (file == nullptr || !mounted() || count > file->sectors || sector > file->sectors - count) {
    return false;
  }
  UINT done = 0;
  return f_lseek(&file->fil, static_cast<FSIZE_t>(sector) << 9) == FR_OK &&
         f_read(&file->fil, data, count << 9, &done) == FR_OK && done == count << 9;
}

bool diskWrite(DiskFile* file, uint32_t sector, const uint8_t* data, uint32_t count) {
  if (file == nullptr || !file->writable || !mounted() || count > file->sectors ||
      sector > file->sectors - count) {
    return false;
  }
  UINT done = 0;
  return f_lseek(&file->fil, static_cast<FSIZE_t>(sector) << 9) == FR_OK &&
         f_write(&file->fil, data, count << 9, &done) == FR_OK && done == count << 9;
}

void diskClose(DiskFile* file) {
  if (file == nullptr) {
    return;
  }
  if (mounted()) {
    f_close(&file->fil);        // ядро сменилось — карта уже размонтирована, закрывать нечего
  }
  heap_caps_free(file);
}

int diskCreate(const char* path, uint32_t sectors) {
  if (sectors == 0 || sectors > 0x7FFFFFu) {
    return 2;
  }
  auto* fil = static_cast<FIL*>(heap_caps_calloc(1, sizeof(FIL), MALLOC_CAP_SPIRAM));
  if (fil == nullptr) {
    return 2;
  }
  char name[528];
  snprintf(name, sizeof(name), "%s%s", g_drive, path);
  FRESULT fr = f_open(fil, name, FA_CREATE_NEW | FA_WRITE);
  if (fr != FR_OK) {
    heap_caps_free(fil);
    return fr == FR_EXIST ? 1 : 2;
  }
  const FSIZE_t size = static_cast<FSIZE_t>(sectors) << 9;
  fr = f_expand(fil, size, 1);          // одним куском: быстрее и выделение, и работа с образом
  if (fr == FR_DENIED) {
    fr = f_lseek(fil, size);            // сплошного места нет: по кластерам
    if (fr == FR_OK && f_tell(fil) != size) {
      fr = FR_DENIED;
    }
  }
  const FRESULT closed = f_close(fil);
  heap_caps_free(fil);
  if (fr == FR_OK) {
    fr = closed;
  }
  if (fr != FR_OK) {
    f_unlink(name);
    return fr == FR_DENIED ? 3 : 2;
  }
  return 0;
}

uint32_t rawSectors() { return heldByWc() ? g_card.sectors() : 0; }

void describe(char* out, size_t size) {
  if (heldByWc()) {
    snprintf(out, size, "%u MB held by WC (SD2 = Tang card)%s, CRC retries %u, errors %u",
             static_cast<unsigned>(g_card.sectors() / 2048), sameCore() ? "" : ", core changed",
             static_cast<unsigned>(g_card.crcRetries()), static_cast<unsigned>(g_card.errors()));
  } else if (!g_mounted) {
    snprintf(out, size, "not mounted");
  } else if (!sameCore()) {
    snprintf(out, size, "gone (Tang core changed), mount again");
  } else {
    snprintf(out, size, "%u MB %s at %s, SCK %u kHz, CRC retries %u, errors %u",
             static_cast<unsigned>(g_card.sectors() / 2048), g_card.highCapacity() ? "SDHC/SDXC" : "SDSC",
             kMountPoint, static_cast<unsigned>(g_card.sckKhz()), static_cast<unsigned>(g_card.crcRetries()),
             static_cast<unsigned>(g_card.errors()));
  }
}

}  // namespace tang_sd
}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
