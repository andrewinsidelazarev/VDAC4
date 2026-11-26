#include "zifi/apps/module_app.hpp"

#if ZIFI_HAS_AUDIO

#include <driver/temperature_sensor.h>
#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <esp_heap_caps_init.h>
#include <esp_littlefs.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <atomic>
#include <math.h>
#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>

#include <soc/soc.h>
#include <esp_memory_utils.h>
#include <sdkconfig.h>
#if CONFIG_ESP_SYSTEM_MEMPROT
#include <esp_memprot.h>
#include <esp_private/esp_memprot_internal.h>
#endif

#include "esp_elf.h"
#include "zifi/audio_player.hpp"
#include "zifi/apps/core_builtin.hpp"
#include "zifi/bringup_log.hpp"
#include "zifi/memory_retry.hpp"
#include "zifi/core_store.hpp"
#include "zifi/crash_record.hpp"
#include "zifi/elf_image.hpp"
#include "zifi/ft_host.hpp"
#include "zifi/tang_link.hpp"
#include "zifi/tang_sd.hpp"
#include "zifi/sd2_files.hpp"
#include "zifi/tang_files.hpp"
#include "zifi/app_data_files.hpp"
#include "zifi/usb_drive.hpp"

namespace {
// Файлы-диски приложения (sd_open): вне ModuleApp, у Application бюджет статической памяти ядра 1.
constexpr size_t kMaxDiskFiles = 4;
void* g_diskFiles[kMaxDiskFiles] = {};
void* g_sd2Files[kMaxDiskFiles] = {};       // sd2_open (видео с SD2)
// Файлы данных приложений с карты Evo на флешке (0.1.165): доставка — из задачи обмена с Z80,
// чтение — из задач приложения (у объекта свой замок для дескрипторов). Объект — в PSRAM (0.1.166):
// в нём таблицы дескрипторов и учёта (~2,8 КБ), а внутренней памяти мало (USB, Wi-Fi, TLS).
zifi::AppDataFiles& appData() {
  static zifi::AppDataFiles* files = [] {
    void* place = heap_caps_malloc(sizeof(zifi::AppDataFiles), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return place != nullptr ? new (place) zifi::AppDataFiles(zifi::UsbDrive::kAppDataRoot)
                            : new zifi::AppDataFiles(zifi::UsbDrive::kAppDataRoot);
  }();
  return *files;
}

// Файл данных без флешки (0.1.166): куски — у программы Z80, по одному запросу за раз (очередь
// держит AppDataFiles). Запрос — в задаче приложения, куски принимает задача обмена с Z80.
struct RemoteReads {
  SemaphoreHandle_t lock = nullptr;    // мьютекс: request, seq, cancelled
  SemaphoreHandle_t done = nullptr;    // запрос кончился: все байты пришли или Z80 не прочитала
  zifi::AppDataRequest request;
  uint8_t seq = 0;
  bool cancelled = true;               // до запуска приложения и при выгрузке — запросов нет
  // для журнала при выгрузке приложения
  uint32_t bytes = 0;
  uint32_t ms = 0;
  uint32_t requests = 0;
  uint32_t repeats = 0;
  uint32_t longest = 0;
};
RemoteReads g_remote;
constexpr uint32_t kRemoteAttempts = 3;     // запрос и два повтора
constexpr uint32_t kRemoteWaitMs = 4000;    // 32 КБ у Z80 — ~0,3 с; 4 с — значит, не дошло

// cancel: приложение выгружается — ждущий кусок запрос кончается сразу, новых нет; false — снова можно
// (запуск приложения), счёт для журнала — с нуля.
void remoteCancel(bool cancel) {
  if (g_remote.lock == nullptr) {
    return;
  }
  xSemaphoreTake(g_remote.lock, portMAX_DELAY);
  g_remote.cancelled = cancel;
  if (cancel) {
    g_remote.request.end();
  } else {
    g_remote.bytes = g_remote.ms = g_remote.requests = g_remote.repeats = g_remote.longest = 0;
  }
  xSemaphoreGive(g_remote.lock);
  if (cancel) {
    xSemaphoreGive(g_remote.done);
  }
}
}  // namespace
#include "zifi/flash_fs.hpp"
#include "zifi/http_stream.hpp"
#include "zifi/player_overlay.hpp"
#include "zifi/resource_disk_cache.hpp"
#include "zifi/z80_transport.hpp"
#include "zifi/usb_drive.hpp"
#if ZIFI_HAS_USB_DRIVE
#include <esp_vfs_fat.h>
#endif

#include <WiFi.h>

// lwIP — после заголовков Arduino: его INADDR_NONE — макрос, а IPAddress.h объявляет такую переменную
#include <fcntl.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <esp_crt_bundle.h>
#include <esp_tls.h>

namespace zifi {

void registerModuleSymbols();   // zm_symbols.cpp

constexpr uint32_t kSendTimeoutMs = 3000;   // sock_send: TCP не ждёт места дольше

// Каталог кэша в LittleFS (раздел "littlefs" смонтирован в /littlefs).
class LittleFsStorage : public ZmStorage {
 public:
  // Раздел остаётся смонтированным всё время работы (см. flash_fs.hpp).
  bool prepare() {
    mounted_ = flashFsMount(false);
    if (mounted_) {
      mkdir(kDirectory, 0777);
    }
    return mounted_;
  }
  bool mounted() const { return mounted_; }
  bool read(const char* file, uint8_t* data, size_t size) override {
    char path[48];
    pathOf(file, path);
    FILE* input = fopen(path, "rb");
    if (input == nullptr) {
      return false;
    }
    const size_t got = fread(data, 1, size, input);
    const int extra = fgetc(input);
    fclose(input);
    return got == size && extra == EOF;
  }
  bool write(const char* file, const uint8_t* data, size_t size) override {
    char path[48];
    pathOf(file, path);
    FILE* output = fopen(path, "wb");
    if (output == nullptr) {
      return false;
    }
    const size_t put = fwrite(data, 1, size, output);
    const bool closed = fclose(output) == 0;
    return put == size && closed;
  }
  bool remove(const char* file) override {
    char path[48];
    pathOf(file, path);
    return unlink(path) == 0;
  }
  long fileSize(const char* file) override {
    char path[48];
    pathOf(file, path);
    struct stat info;
    return stat(path, &info) == 0 ? static_cast<long>(info.st_size) : kMissing;
  }
  bool list(void (*each)(const char* file, void* context), void* context) override {
    DIR* directory = opendir(kDirectory);
    if (directory == nullptr) {
      return false;
    }
    while (const dirent* item = readdir(directory)) {
      if (item->d_type == DT_REG) {
        each(item->d_name, context);
      }
    }
    closedir(directory);
    return true;
  }
  // Раздел LittleFS минус запас: журнал наладки (48 КБ), настройки и блоки,
  // которые LittleFS нужны свободными для записи.
  size_t capacityBytes() override {
    const esp_partition_t* partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "littlefs");
    if (partition == nullptr || partition->size <= kReservedBytes) {
      return 0;
    }
    return partition->size - kReservedBytes;
  }

 private:
  static constexpr char kDirectory[] = "/littlefs/zm";
  static constexpr size_t kReservedBytes = 320 * 1024;
  bool mounted_ = false;
  static void pathOf(const char* file, char* path) {
    snprintf(path, 48, "%s/%s", kDirectory, file);
  }
};
constexpr char LittleFsStorage::kDirectory[];

namespace {

constexpr char kTag[] = "zifi-zm";
constexpr uint32_t kAllocMagic = 0x5A4D414C;   // "LAMZ"
constexpr uint32_t kMaxModuleBytes = 2 * 1024 * 1024;
constexpr uint32_t kMaxResourceBytes = 7 * 1024 * 1024;
// Ядро ПЛИС с карты Evo (0.1.164): не длиннее слота flash Tang — больше битстрим GW2AR-18 не бывает.
constexpr uint32_t kMaxCoreBytes = 1024 * 1024;
// Временные буферы прошивки (Wi-Fi и LwIP, FTP, журнал) ложатся в PSRAM куда
// придётся: доставке, которой не нашлось места, — подождать, пока уйдут.
constexpr uint32_t kRoomWaitMs = 2000;
// Приложению столько не шлют его команд — его программа Z80 ушла (handleQuery)
constexpr uint32_t kAbandonedMs = 5000;
constexpr uint32_t kRoomStepMs = 50;

// Заголовок каждого блока, выделенного приложением: весь список освобождается
// при выгрузке.
struct AllocHeader {
  AllocHeader* next;
  AllocHeader* prev;
  uint32_t caps;
  uint32_t magic;
};
static_assert(sizeof(AllocHeader) == 16, "allocation header keeps 16-byte alignment");

uint32_t heapCaps(uint32_t caps) {
  return (caps & ZM_MEM_INTERNAL) != 0 ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                                       : (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// Код модуля пишется через шину данных (кэш данных загрузчик сбрасывает в
// PSRAM), а исполняется через шину команд: в кэше команд могли остаться строки
// прежнего кода с тех же адресов — модули грузятся в кучу по разным адресам.
// Сбросить их над всем текстом (строка кэша команд — до 64 байтов).
void forgetStaleCode(const uint8_t* code, size_t size) {
  constexpr uintptr_t kLine = 64;
  const uintptr_t at = reinterpret_cast<uintptr_t>(code) + (SOC_IROM_LOW - SOC_DROM_LOW);
  const uintptr_t start = at & ~(kLine - 1);
  const uintptr_t end = (at + size + kLine - 1) & ~(kLine - 1);
  const esp_err_t result = esp_cache_msync(reinterpret_cast<void*>(start), end - start,
                                           ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                                               ESP_CACHE_MSYNC_FLAG_TYPE_INST);
  if (result != ESP_OK) {
    bringupLog(kTag, "code cache sync at %08X: %s", static_cast<unsigned>(start),
               esp_err_to_name(result));
  }
}

// Датчик температуры кристалла и последнее показание (десятые доли °C).
temperature_sensor_handle_t g_thermometer = nullptr;
volatile int16_t g_temperature = ZM_TEMPERATURE_UNKNOWN;


}  // namespace

zm_core_t ModuleApp::core_ = {};
ModuleApp* ModuleApp::instance_ = nullptr;
ModuleApp::CoreJob ModuleApp::coreJob_ = {};
ModuleApp::CodeRam ModuleApp::codeRam_ = {};
uint8_t* ModuleApp::codeReserve_ = nullptr;
SemaphoreHandle_t ModuleApp::coreJobGo_ = nullptr;
SemaphoreHandle_t ModuleApp::coreJobDone_ = nullptr;
TaskHandle_t ModuleApp::coreLoader_ = nullptr;

ModuleApp::ModuleApp(AppHost& host, AudioPlayer& player) : host_(host), player_(player) {
  instance_ = this;
  core_.size = sizeof(zm_core_t);
  core_.abi_major = ZM_APP_ABI_MAJOR;
  core_.abi_minor = ZM_APP_ABI_MINOR;
  core_.firmware = ZIFI_BUILD_VERSION;
  core_.alloc = coreAlloc;
  core_.realloc = coreRealloc;
  core_.free = coreFree;
  core_.log = coreLog;
  core_.millis = coreMillis;
  core_.delay = coreDelay;
  core_.task_start = coreTaskStart;
  core_.mutex_create = coreMutexCreate;
  core_.mutex_lock = coreMutexLock;
  core_.mutex_unlock = coreMutexUnlock;
  core_.signal_create = coreSignalCreate;
  core_.signal_give = coreSignalGive;
  core_.signal_wait = coreSignalWait;
  core_.reply = coreReply;
  core_.event = coreEvent;
  core_.pcm_open = corePcmOpen;
  core_.pcm_write = corePcmWrite;
  core_.pcm_queued_ms = corePcmQueuedMs;
  core_.pcm_flush = corePcmFlush;
  core_.pcm_close = corePcmClose;
  core_.pcm_pause = corePcmPause;
  core_.set_volume = coreSetVolume;
  core_.volume = coreVolume;
  core_.pcm_faults = corePcmFaults;
  core_.module_open = coreModuleOpen;
  core_.module_close = coreModuleClose;
  core_.resource = coreResource;
  core_.resource_release = coreResourceRelease;
  core_.temperature = coreTemperature;
  core_.net_open = coreNetOpen;
  core_.net_header = coreNetHeader;
  core_.net_read = coreNetRead;
  core_.net_close = coreNetClose;
  core_.ft_open = coreFtOpen;
  core_.ft_write = coreFtWrite;
  core_.ft_read = coreFtRead;
  core_.ft_host_cmd = coreFtHostCmd;
  core_.ft_close = coreFtClose;
  core_.core_load = coreCoreLoad;
  core_.core_back = coreCoreBack;
  core_.link_read = coreLinkRead;
  core_.link_write = coreLinkWrite;
  core_.sock_open = coreSockOpen;
  core_.sock_send = coreSockSend;
  core_.sock_recv = coreSockRecv;
  core_.sock_close = coreSockClose;
  core_.net_resolve = coreNetResolve;
  core_.wifi_info = coreWifiInfo;
  core_.ui_available = coreUiAvailable;
  core_.app_done = coreAppDone;
  core_.sd_list = coreSdList;
  core_.sd_read = coreSdRead;
  core_.sd_write = coreSdWrite;
  core_.sd_open = coreSdOpen;
  core_.sd_pread = coreSdPread;
  core_.sd_pwrite = coreSdPwrite;
  core_.sd_close = coreSdClose;
  core_.sd_create = coreSdCreate;
  core_.wifi_rssi = coreWifiRssi;
  core_.sock_open_tls = coreSockOpenTls;
  core_.link_media = coreLinkMedia;
  core_.handoff_put = coreHandoffPut;
  core_.handoff_take = coreHandoffTake;
  core_.sd2_open = coreSd2Open;
  core_.sd2_read = coreSd2Read;
  core_.sd2_stream = coreSd2Stream;
  core_.sd2_close = coreSd2Close;
  core_.link_burst = coreLinkBurst;
  core_.pcm_low_latency = corePcmLowLatency;
  core_.code_ram = coreCodeRam;
  core_.module_open_ex = coreModuleOpenEx;
  core_.data_open = coreDataOpen;
  core_.data_read = coreDataRead;
  core_.data_close = coreDataClose;
  core_.link_read_checked = coreLinkReadChecked;
  core_.link_burst_dma = coreLinkBurstDma;
  core_.link_burst_wait = coreLinkBurstWait;
}

void ModuleApp::begin() {
  registerModuleSymbols();
  logCodeRamReserve();
  modules_ = static_cast<LoadedModule*>(
      heap_caps_calloc(kMaxModules, sizeof(LoadedModule), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (modules_ == nullptr) {
    bringupLog(kTag, "module slots not allocated: module_open will refuse");
  }
  appLock_ = xSemaphoreCreateMutex();
  // Файлы данных без флешки (0.1.166): замок запроса, сигнал «кончился», блок 8 КБ в PSRAM.
  g_remote.lock = xSemaphoreCreateMutex();
  g_remote.done = xSemaphoreCreateBinary();
  uint8_t* remoteBlock = static_cast<uint8_t*>(
      heap_caps_malloc(AppDataFiles::kRemoteBlock, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (g_remote.lock == nullptr || g_remote.done == nullptr || remoteBlock == nullptr) {
    bringupLog(kTag, "data files: no memory for reads from the Evo card");
  } else {
    appData().setFetch(remoteFetch, this, remoteBlock);
  }
  // Откуда читается каждый файл данных (0.1.175): одна строка на файл за запуск приложения.
  appData().setLog([](void*, const char* line) { bringupLog(kTag, "%s", line); }, nullptr);
#if ZIFI_HAS_SPI_LINK
  coreJobGo_ = xSemaphoreCreateBinary();
  coreJobDone_ = xSemaphoreCreateBinary();
  if (coreJobGo_ == nullptr || coreJobDone_ == nullptr ||
      // стек — в PSRAM: внутренней памяти мало (Wi-Fi, LWIP), а задача не трогает flash ESP
      // (SD Tang и ящик BL616 — по LINK, журнал — очередью). Отметки ядер с карты Evo в NVS она
      // не пишет сама: 0.1.169 упала на этом (assert cache_utils.c:126) — с 0.1.170 их пишет
      // core_store::evoFlush() из задачи обмена с Z80.
      xTaskCreatePinnedToCoreWithCaps(coreLoaderTask, "zm-coreload", 8192, this, 2, &coreLoader_, 1,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    coreLoader_ = nullptr;
    bringupLog(kTag, "core loader task not started");
  }
#endif
  // Датчик температуры кристалла: своя задача, раз в секунду.
  temperature_sensor_config_t thermometer = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
  if (temperature_sensor_install(&thermometer, &g_thermometer) != ESP_OK) {
    g_thermometer = nullptr;
  } else if (temperature_sensor_enable(g_thermometer) != ESP_OK) {
    temperature_sensor_uninstall(g_thermometer);
    g_thermometer = nullptr;
  }
  if (g_thermometer == nullptr) {
    bringupLog(kTag, "temperature sensor: not available");
  }
  // На ядре 1, ниже звука; модули играют на ядре 0.
  if (g_thermometer != nullptr) {
    xTaskCreatePinnedToCore(temperatureTask, "zm-temp", 2048, this, 1, nullptr, 1);
  }
  storage_ = new LittleFsStorage();
  storage_->prepare();
  cache_ = new ZmCache(*storage_);
  cache_->load();
  size_t total = 0;
  size_t used = 0;
  const esp_err_t info = esp_littlefs_info("littlefs", &total, &used);
  bringupLog(kTag,
             "module cache: %s, %u files, %u of %u KB free; LittleFS info %s: %u KB, used %u KB",
             storage_->mounted() ? "mounted" : "NOT MOUNTED", static_cast<unsigned>(cache_->count()),
             static_cast<unsigned>(cache_->freeBytes() / 1024),
             static_cast<unsigned>(storage_->capacityBytes() / 1024), esp_err_to_name(info),
             static_cast<unsigned>(total / 1024), static_cast<unsigned>(used / 1024));
}

// --- команды Z80 -------------------------------------------------------------

bool ModuleApp::handleCommand(const PacketView& packet) {
  switch (packet.command) {
    case kModQuery:
      handleQuery(packet);
      return true;
    case kModPut:
      handlePut(packet);
      return true;
    case kModCommit:
      handleCommit();
      return true;
    case kDataPiece:
      handleDataPiece(packet);
      return true;
    case kAppStart:
      handleStart(packet);
      return true;
    case kCoreList:
      handleCoreList();
      return true;
    case kCoreApp:
      handleCoreApp(packet);
      return true;
    case kAppStop: {
      stopApp();
      const uint8_t ok = 1;
      host_.transport().send(kRespAppStop, &ok, 1);
      return true;
    }
    default:
      break;
  }
  if (app_ == nullptr || packet.command < app_->command_first ||
      packet.command > app_->command_last || app_->command == nullptr) {
    return false;
  }
  appCommandMs_ = coreMillis();
  inCommand_ = true;
  const int result = app_->command(packet.command, packet.data, packet.length);
  inCommand_ = false;
  // Приложение кончило работу (app_done): ответ ушёл — выгрузить, память — назад
  if (appDone_) {
    appDone_ = false;
    bringupLog(kTag, "app %s: done, unloading", appName_);
    stopApp();
  }
  return result == ZM_OK;
}

// Codex: только владелец канала; appLock_ в stopApp исключает одновременный poll ELF.
void ModuleApp::pollCore1(uint32_t now) {
  reapTasks();
#if ZIFI_HAS_SPI_LINK
  if (app_ == nullptr || !ftDetach_.due(now)) return;
  uint32_t state[3] = {};
  if (!tang::ready() || !tang::read(0, state, 3) ||
      !ftDetach_.observe(state[0], state[1], state[2])) return;
  bringupLog(kTag, "app %s: M_SEL=0, slot0, unloading", appName_);
  stopApp();
#else
  (void)now;
#endif
}

void ModuleApp::pollNetworkCore(uint32_t now) {
  if (appLock_ == nullptr || xSemaphoreTake(appLock_, 0) != pdTRUE) {
    return;
  }
  if (app_ != nullptr && app_->poll != nullptr) {
    app_->poll(now);
  }
  xSemaphoreGive(appLock_);
}

// Отметки «модуль с флешки» (0.1.146): его нет в кэше ESP (или там другая версия), а копия в
// /usb/ELF годна — loadElf читает её. Ставит и снимает ядро обмена, читает loadElf (и из задач
// приложений: module_open). Таблица — в PSRAM, заводится при первой отметке: внутренняя
// память ядра 1 на счету (бюджет Application в main.cpp).
namespace {

struct DriveModule {
  char name[13];                          // пусто — место свободно
  uint32_t size;
  uint32_t stamp;
};
constexpr size_t kMaxDriveModules = 16;
std::atomic<DriveModule*> g_driveModules{nullptr};
size_t g_driveNext = 0;                   // какую отметку вытеснять, когда мест нет
portMUX_TYPE g_driveLock = portMUX_INITIALIZER_UNLOCKED;

// Отметка «с флешки» для name (size 0 — снять). true — источник образа сменился
// (работающее приложение тогда надо перезапустить).
bool markDriveModule(const char* name, uint32_t size, uint32_t stamp) {
  DriveModule* table = g_driveModules.load();
  if (table == nullptr) {
    if (size == 0) {
      return false;                       // отметок не было
    }
    table = static_cast<DriveModule*>(
        heap_caps_calloc(kMaxDriveModules, sizeof(DriveModule), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (table == nullptr) {
      return false;
    }
    g_driveModules.store(table);
  }
  bool changed = false;
  portENTER_CRITICAL(&g_driveLock);
  DriveModule* slot = nullptr;
  DriveModule* empty = nullptr;
  for (size_t i = 0; i < kMaxDriveModules; ++i) {
    DriveModule& item = table[i];
    if (item.name[0] != 0 && strcmp(item.name, name) == 0) {
      slot = &item;
    } else if (item.name[0] == 0 && empty == nullptr) {
      empty = &item;
    }
  }
  if (size == 0) {
    if (slot != nullptr) {
      *slot = DriveModule{};
      changed = true;
    }
  } else if (slot != nullptr) {
    changed = slot->size != size || slot->stamp != stamp;
    slot->size = size;
    slot->stamp = stamp;
  } else {
    if (empty == nullptr) {
      empty = &table[g_driveNext];
      g_driveNext = (g_driveNext + 1) % kMaxDriveModules;
    }
    strncpy(empty->name, name, sizeof(empty->name) - 1);
    empty->name[sizeof(empty->name) - 1] = 0;
    empty->size = size;
    empty->stamp = stamp;
    changed = true;
  }
  portEXIT_CRITICAL(&g_driveLock);
  return changed;
}

// Отмечен ли name «с флешки»; size — длина копии.
bool driveModuleSize(const char* name, uint32_t* size) {
  DriveModule* table = g_driveModules.load();
  if (table == nullptr) {
    return false;
  }
  bool found = false;
  portENTER_CRITICAL(&g_driveLock);
  for (size_t i = 0; i < kMaxDriveModules; ++i) {
    if (table[i].name[0] != 0 && strcmp(table[i].name, name) == 0) {
      *size = table[i].size;
      found = true;
      break;
    }
  }
  portEXIT_CRITICAL(&g_driveLock);
  return found;
}

}  // namespace

void ModuleApp::replyQuery(uint8_t state) {
  uint8_t reply[5];
  reply[0] = state;
  writeLe32(reply + 1, static_cast<uint32_t>(cache_ != nullptr ? cache_->freeBytes() : 0));
  host_.transport().send(kRespModQuery, reply, sizeof(reply));
}

// MOD_QUERY с флагом 0x04 (0.1.165): файл данных приложения app с карты Evo. Ответ: 0 — файл есть на
// флешке, 3 — плохое имя, 4 — на флешке нет (или флешки нет). 5 (0.1.166) — файл остаётся на карте Evo,
// и приложение читает его по запросу у программы Z80: она назвала номер файла (index), значит, умеет.
// С 0.1.167 программа Z80 назвала номер — всегда 5 (пользователь 24.11.2026: «кэш флешки OTG не
// используем — ждать полдня на копирование»). Не встал на учёт (плохое имя, таблица полна) — 3.
// С 0.1.175 на флешку ничего не копируется и для запускателей 0.1.165 без номера файла (ответ 1 «шли»
// больше не даётся; пользователь 26.11.2026: «Сама игра эти файлы туда не копирует»). Лежит ли файл на
// флешке — решает data_open: она ищет его во всех папках /SYSTEM/CACHE (AppDataFiles::findOnDrive), и
// найденный читается с флешки, даже если стоит на учёте с карты.
void ModuleApp::handleDataQuery(const char* name, uint32_t size, uint32_t stamp, const char* app,
                                int index) {
#if ZIFI_HAS_USB_DRIVE
  const bool drive = usbDrive().ready();
#else
  const bool drive = false;
#endif
  if (index >= 0) {
    if (appData().addRemote(app, name, size, stamp, static_cast<uint8_t>(index))) {
      bringupLog(kTag, "data file %s of %s, %u bytes: registered from the Evo card (file %d of the Z80 "
                 "program)%s", name, app, static_cast<unsigned>(size), index,
                 drive ? "; a copy on the USB drive goes first" : "");
      replyQuery(AppDataFiles::kRemote);
      return;
    }
    bringupLog(kTag, "data file %s of %s: NOT REGISTERED (bad name or table full)", name, app);
    replyQuery(AppDataFiles::kBadName);
    return;
  }
  if (!drive) {
    bringupLog(kTag, "data file %s of %s: no USB drive", name, app);
    replyQuery(AppDataFiles::kNoDrive);
    return;
  }
#if ZIFI_HAS_USB_DRIVE
  appData().forgetRemote(app, name);           // номера нет — у программы Z80 файл не прочитать
  appData().setVolume(usbDrive().generation());
  char path[300];
  uint32_t length = 0;
  const bool found = appData().findOnDrive(app, name, path, sizeof(path), &length, nullptr);
  if (found) {
    bringupLog(kTag, "data file %s of %s, %u bytes: on the USB drive %s, %u bytes", name, app,
               static_cast<unsigned>(size), path, static_cast<unsigned>(length));
  } else {
    bringupLog(kTag, "data file %s of %s, %u bytes: not on the USB drive (nothing is copied there)",
               name, app, static_cast<unsigned>(size));
  }
  replyQuery(found ? AppDataFiles::kHave : AppDataFiles::kNoDrive);
#endif
}

// ABI 1.20: data_open / data_read / data_close — файлы данных того приложения, что работает
// (папка — имя его ELF без «.ELF»: Q2.ELF → /SYSTEM/CACHE/Q2).
int ModuleApp::coreDataOpen(const char* name, void** file, uint32_t* size) {
  if (file != nullptr) {
    *file = nullptr;
  }
  char app[9] = {};
  const char* elf = instance_ != nullptr ? instance_->appName_ : "";
  for (size_t i = 0; i + 1 < sizeof(app) && elf[i] != 0 && elf[i] != '.'; ++i) {
    app[i] = elf[i];
  }
  // 0.1.175: сначала флешка — папка приложения и все папки /SYSTEM/CACHE (номер монтирования: вынули и
  // вставили — найденное забыто; флешки нет — 0, не ищется); нет на ней — файл на учёте с карты Evo
  // (0.1.166) у программы Z80. Откуда читается файл, AppDataFiles пишет в журнал сама, раз на файл.
#if ZIFI_HAS_USB_DRIVE
  appData().setVolume(usbDrive().isReady() ? usbDrive().generation() : 0);
#else
  appData().setVolume(0);
#endif
  const int result = appData().open(app, name, file, size);
  if (result == ZM_ERROR) {
    bringupLog(kTag, "data_open %s of %s: no free handle", name != nullptr ? name : "?", app);
  }
  return result;
}

// Кусок файла данных у программы Z80 (0.1.166): событие kEventDataRead, ждать куски kDataPiece. Не
// дошло за kRemoteWaitMs или Z80 не прочитала — спросить снова с новым номером (запоздавшие куски
// прежнего номера отсекает AppDataRequest). Выгрузка приложения (remoteCancel) обрывает сразу.
bool ModuleApp::remoteFetch(void* context, uint8_t index, uint32_t offset, void* data, uint32_t count) {
  ModuleApp* self = static_cast<ModuleApp*>(context);
  RemoteReads& r = g_remote;
  if (self == nullptr || r.lock == nullptr || r.done == nullptr) {
    return false;
  }
  const uint32_t began = coreMillis();
  for (uint32_t attempt = 0; attempt < kRemoteAttempts; ++attempt) {
    xSemaphoreTake(r.lock, portMAX_DELAY);
    if (r.cancelled) {
      xSemaphoreGive(r.lock);
      return false;
    }
    const uint8_t seq = ++r.seq;
    r.request.begin(seq, offset, count, static_cast<uint8_t*>(data));
    xSemaphoreTake(r.done, 0);                // сигнал прежнего запроса — не наш
    xSemaphoreGive(r.lock);
    uint8_t event[AppDataRequest::kEventBytes];
    AppDataRequest::event(event, seq, index, offset, count);
    if (self->host_.sendEvent(kEventDataRead, event, sizeof(event), 1000)) {
      xSemaphoreTake(r.done, pdMS_TO_TICKS(kRemoteWaitMs));
    }
    xSemaphoreTake(r.lock, portMAX_DELAY);
    r.request.end();                          // дальше куски этого номера — мимо
    const bool complete = r.request.complete();
    const bool failed = r.request.failed();
    const bool cancelled = r.cancelled;
    xSemaphoreGive(r.lock);
    if (complete) {
      const uint32_t took = coreMillis() - began;
      r.bytes += count;
      r.ms += took;
      ++r.requests;
      if (took > r.longest) {
        r.longest = took;
      }
      return true;
    }
    if (cancelled) {
      return false;
    }
    ++r.repeats;
    bringupLog(kTag, "data file %u of the Z80 program, %u bytes at %u: %s", index,
               static_cast<unsigned>(count), static_cast<unsigned>(offset),
               failed ? "the Z80 could not read it" : "no answer in 4 s");
  }
  bringupLog(kTag, "data file %u of the Z80 program: read at %u FAILED", index,
             static_cast<unsigned>(offset));
  return false;
}

// kDataPiece [номер запроса][смещение LE32][данные] (0.1.166): ответа нет.
void ModuleApp::handleDataPiece(const PacketView& packet) {
  RemoteReads& r = g_remote;
  if (r.lock == nullptr) {
    return;
  }
  xSemaphoreTake(r.lock, portMAX_DELAY);
  const AppDataRequest::Result result = r.request.piece(packet.data, packet.length);
  xSemaphoreGive(r.lock);
  if (result == AppDataRequest::kDone || result == AppDataRequest::kFailed) {
    xSemaphoreGive(r.done);
  }
}

int ModuleApp::coreDataRead(void* file, uint32_t offset, void* data, uint32_t count, uint32_t* got) {
  return appData().read(file, offset, data, count, got);
}

void ModuleApp::coreDataClose(void* file) {
  appData().close(file);
}

// [флаги][длина LE32][время FAT LE32][имя, 0] → [0 есть / 1 шли / 2 нет места / 3 плохое имя][свободно LE32]
void ModuleApp::handleQuery(const PacketView& packet) {
  dropDelivery();
  if (packet.length < 10 || cache_ == nullptr) {
    replyQuery(3);
    return;
  }
  const bool resource = (packet.data[0] & 0x01) != 0;
  const uint32_t size = readLe32(packet.data + 1);
  const uint32_t stamp = readLe32(packet.data + 5);
  char name[13] = {};
  const size_t nameLength = strnlen(reinterpret_cast<const char*>(packet.data + 9),
                                    packet.length - 9);
  if (nameLength == 0 || nameLength >= sizeof(name)) {
    replyQuery(3);
    return;
  }
  memcpy(name, packet.data + 9, nameLength);
  for (char& c : name) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  if (!ZmCache::validName(name) || size == 0) {
    replyQuery(3);
    return;
  }
  // Файл данных приложения с карты Evo (флаг 4, 0.1.165): за именем — имя приложения; файл — на флешку
  if ((packet.data[0] & 0x04) != 0) {
    char app[9] = {};
    const size_t appAt = 9 + nameLength + 1;
    const size_t appLength = appAt < packet.length
                                 ? strnlen(reinterpret_cast<const char*>(packet.data + appAt),
                                           packet.length - appAt)
                                 : 0;
    if (appLength == 0 || appLength >= sizeof(app)) {
      replyQuery(3);
      return;
    }
    memcpy(app, packet.data + appAt, appLength);
    // 0.1.166: за именем приложения — номер файла у программы Z80 (она умеет читать его по запросу)
    const size_t indexAt = appAt + appLength + 1;
    const int index = indexAt < packet.length ? packet.data[indexAt] : -1;
    handleDataQuery(name, size, stamp, app, index);
    return;
  }
  // Ядро ПЛИС с карты Evo (флаг 2, 0.1.164): ИМЯ.BIN — своим путём, в кэш ESP оно не ложится
  if ((packet.data[0] & 0x02) != 0) {
    handleCoreQuery(name, size, stamp);
    return;
  }
  if (resource) {
    for (const Resource& item : resources_) {
      // И время файла: обновлённый SF2 или MWK того же размера (или одноимённый набор
      // из другой папки) прежде считался тем же, и играла старая версия.
      if (item.data != nullptr && strcmp(item.name, name) == 0 && item.size == size &&
          item.stamp == stamp) {
        replyQuery(0);
        return;
      }
    }
  } else if (cache_->has(name, size, stamp)) {
    // Модуль в кэше ESP — как раньше; отметка «с флешки», если была, снимается (0.1.146).
    if (markDriveModule(name, 0, 0) && moduleInUse(name)) {
      appStale_ = true;
    }
    replyQuery(0);
    return;
  } else if (size <= kMaxModuleBytes && moduleFromDrive(name, size, stamp)) {
    replyQuery(0);                             // копия на флешке годна: с карты не передавать
    return;
  }
  // Есть флешка — принятый модуль ляжет на неё (/usb/ELF), а не в кэш ESP: места в кэше
  // не нужно, и приложение, которое его держит, останавливать незачем (0.1.146).
#if ZIFI_HAS_USB_DRIVE
  const bool toDrive = !resource && usbDrive().ready();
#else
  const bool toDrive = false;
#endif
  // Модуль не ляжет лишь потому, что кэш держит ELF приложения, которому давно никто не шлёт его
  // команд (kAbandonedMs): его программа Z80 ушла, не выгрузив его (упала, Evo сброшен) — живое
  // приложение плагин опрашивает постоянно. 29.10.2026 так остался HMM2 после сбоя игры: HMM2.ELF
  // (1,5 МБ из 1664 КБ кэша) не давал места VIDEO.ELF, и FTVIEW4 на «нет места» молча уходил на
  // путь FT812. Брошенное приложение останавливается — его ELF кэш вытеснит.
  if (!resource && !toDrive && size <= kMaxModuleBytes && app_ != nullptr &&
      !cache_->canFit(size, appName_) &&
      cache_->canFit(size, nullptr) && coreMillis() - appCommandMs_ >= kAbandonedMs) {
    bringupLog(kTag, "module %s: the cache is held by app %s, no command for it for %u ms: stopping it",
               name, appName_, static_cast<unsigned>(coreMillis() - appCommandMs_));
    stopApp();
  }
  if (size > (resource ? kMaxResourceBytes : kMaxModuleBytes) ||
      (!resource && !toDrive && !cache_->canFit(size, app_ != nullptr ? appName_ : nullptr))) {
    // Отказ — в журнал: плагин на него уходит молча (FTVIEW4 — на путь FT812)
    bringupLog(kTag, "module %s, %u bytes: no room (cache %u KB free, app %s holds its ELF)", name,
               static_cast<unsigned>(size), static_cast<unsigned>(cache_->freeBytes() / 1024),
               app_ != nullptr ? appName_ : "-");
    replyQuery(2);                             // не передавать того, что не ляжет
    return;
  }
  // Прежняя версия ресурса того же имени уходит. Её может держать работающее
  // приложение: MIDI.ELF читает сэмплы банка прямо из ресурса, и освобождение под
  // ним означало бы шум или падение. Поэтому сначала стоп приложения (как при
  // нехватке места в allocateDelivery), после доставки — запуск снова.
  char restart[sizeof(appName_)] = {};
  if (resource) {
    bool held = false;
    for (const Resource& item : resources_) {
      held = held || (item.data != nullptr && strcmp(item.name, name) == 0);
    }
    if (held && app_ != nullptr) {
      memcpy(restart, appName_, sizeof(restart));
      stopApp();
    }
    coreResourceRelease(name);
  }
  delivery_ = allocateDelivery(name, size, resource);
  if (restart[0] != 0) {
    startApp(restart);
  }
  if (delivery_ == nullptr) {
    replyQuery(2);
    return;
  }
  // Годная копия на флешке — ресурс уже есть, передавать его с карты не нужно.
  if (resource && loadResourceFromDrive(name, size, stamp)) {
    replyQuery(0);
    return;
  }
  memcpy(deliveryName_, name, sizeof(deliveryName_));
  deliverySize_ = size;
  deliveryStamp_ = stamp;
  deliveryReceived_ = 0;
  deliveryResource_ = resource;
  replyQuery(1);
}

// Место в PSRAM под доставку — одним куском. Не нашлось — по порядку освободить
// то, что уже не нужно, пока не найдётся:
// - приложение: декодеры, кольца и всё, что оно держало (его блоки лежат где
//   придётся и дробят PSRAM: после IT в середине оставался код TRACKER.ELF, и
//   банку MIDI не хватало куска при 7,6 МБ свободных). Ресурс плагин шлёт после
//   APP_START, поэтому приложение потом запускается снова; модуль — до APP_START,
//   приложение запустит сам плагин;
// - другие ресурсы, по одному, сначала самый большой: плагин пришлёт их, когда
//   понадобятся (вместе банк MIDI и ПЗУ OPL4 и так не помещаются); набору волн
//   MWM так уступает банк MIDI, а только что присланное ПЗУ остаётся;
// - временные буферы прошивки: до kRoomWaitMs.
// Что вышло — в журнале.
uint8_t* ModuleApp::allocateDelivery(const char* name, uint32_t size, bool resource) {
  constexpr uint32_t kCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  uint8_t* block = static_cast<uint8_t*>(heap_caps_malloc(size, kCaps));
  if (block != nullptr) {
    return block;
  }
#if ZIFI_HAS_SPI_LINK
  // Сначала — холст экрана плеера: окно плагина на экране Evo всё равно есть
  player_overlay::releaseMemory();
  block = static_cast<uint8_t*>(heap_caps_malloc(size, kCaps));
  if (block != nullptr) {
    bringupLog(kTag, "room for %s, %u KB: player screen memory released", name,
               static_cast<unsigned>(size / 1024));
    return block;
  }
#endif
#if ZIFI_HAS_SPI_LINK
  // Потом — образы ядер с карты Evo (0.1.164): с флешки или из кэша BL616 они грузятся и так,
  // иначе плагин пришлёт их снова
  if (const uint32_t releasedCores = core_store::evoReleaseMemory()) {
    block = static_cast<uint8_t*>(heap_caps_malloc(size, kCaps));
    if (block != nullptr) {
      bringupLog(kTag, "room for %s, %u KB: %u KB of Evo card core images released", name,
                 static_cast<unsigned>(size / 1024), static_cast<unsigned>(releasedCores / 1024));
      return block;
    }
  }
#endif
  const size_t largest = heap_caps_get_largest_free_block(kCaps);
  const size_t freeBytes = heap_caps_get_free_size(kCaps);
  char stopped[sizeof(appName_)] = {};
  if (app_ != nullptr) {
    memcpy(stopped, appName_, sizeof(stopped));
    stopApp();
    block = static_cast<uint8_t*>(heap_caps_malloc(size, kCaps));
  }
  unsigned released = 0;
  while (block == nullptr) {
    Resource* largest = nullptr;
    for (Resource& item : resources_) {
      if (item.data != nullptr && (largest == nullptr || item.size > largest->size)) {
        largest = &item;
      }
    }
    if (largest == nullptr) {
      break;
    }
    heap_caps_free(largest->data);
    *largest = Resource{};
    ++released;
    block = static_cast<uint8_t*>(heap_caps_malloc(size, kCaps));
  }
  uint32_t waited = 0;
  while (block == nullptr && waited < kRoomWaitMs) {
    vTaskDelay(pdMS_TO_TICKS(kRoomStepMs));
    waited += kRoomStepMs;
    block = static_cast<uint8_t*>(heap_caps_malloc(size, kCaps));
  }
  bringupLog(kTag,
             "room for %s, %u KB: largest %u KB of %u KB free; app %s stopped, %u resources "
             "released, waited %u ms: %s, largest now %u KB",
             name, static_cast<unsigned>(size / 1024), static_cast<unsigned>(largest / 1024),
             static_cast<unsigned>(freeBytes / 1024), stopped[0] != 0 ? stopped : "-", released,
             static_cast<unsigned>(waited), block != nullptr ? "got it" : "NO ROOM",
             static_cast<unsigned>(heap_caps_get_largest_free_block(kCaps) / 1024));
  if (resource && stopped[0] != 0) {
    startApp(stopped);
  }
  return block;
}

// [смещение LE32][данные] → [1 принято / 0 не то место][принято всего LE32]
void ModuleApp::handlePut(const PacketView& packet) {
  uint8_t reply[5] = {0};
  if (deliveryData_) {
    // Файл данных приложения (0.1.165): кусок сразу на флешку, PSRAM под весь файл не берётся
    if (packet.length >= 4 &&
        appData().put(readLe32(packet.data), packet.data + 4, packet.length - 4)) {
      reply[0] = 1;
    }
    deliveryReceived_ = appData().received();
    writeLe32(reply + 1, deliveryReceived_);
    host_.transport().send(kRespModPut, reply, sizeof(reply));
    return;
  }
  if (delivery_ != nullptr && packet.length >= 4) {
    const uint32_t offset = readLe32(packet.data);
    const uint32_t length = packet.length - 4;
    if (offset == deliveryReceived_ && length <= deliverySize_ - deliveryReceived_) {
      memcpy(delivery_ + offset, packet.data + 4, length);
      deliveryReceived_ += length;
      reply[0] = 1;
    }
  }
  writeLe32(reply + 1, deliveryReceived_);
  host_.transport().send(kRespModPut, reply, sizeof(reply));
}

// → [1 положен / 0 нет]
void ModuleApp::handleCommit() {
  uint8_t ok = 0;
  if (deliveryData_) {
    // Файл данных приложения (0.1.165): всё ли принято — переименовать в настоящее имя, время с карты
    const int64_t began = esp_timer_get_time();
    ok = appData().commit() ? 1 : 0;
    bringupLog(kTag, "data file %s: %u bytes %s in %u ms", deliveryName_,
               static_cast<unsigned>(deliverySize_), ok ? "stored on the USB drive" : "REFUSED",
               static_cast<unsigned>((esp_timer_get_time() - began) / 1000));
    deliveryData_ = false;
    dropDelivery();
    host_.transport().send(kRespModCommit, &ok, 1);
    return;
  }
  // Запись модуля в LittleFS (стирание секторов) идёт секунды, и всё это время
  // плагин ждёт ответа: сколько именно — в строке журнала.
  const int64_t started = esp_timer_get_time();
  const char* where = "stored";
  if (delivery_ != nullptr && deliveryReceived_ == deliverySize_) {
    if (deliveryCore_) {
      // Ядро ПЛИС с карты Evo (0.1.164): проверка битстрима, флешка или PSRAM, отметка для core_load
      where = commitCore();
      ok = where != nullptr ? 1 : 0;
    } else if (deliveryResource_) {
      if (adoptResource(deliveryName_, delivery_, deliverySize_, deliveryStamp_)) {
        delivery_ = nullptr;            // теперь у ресурса
        ok = 1;
      }
    } else if (const char* problem = elfImageProblem(delivery_, deliverySize_)) {
      // Загрузчик ELF размера файла не знает и верит заголовкам: битый или чужой
      // образ в кэш не попадает (прежде проверялись только четыре байта сигнатуры).
      bringupLog(kTag, "module %s: not a loadable ELF (%s)", deliveryName_, problem);
    } else if (storeModuleOnDrive(deliveryName_, delivery_, deliverySize_, deliveryStamp_)) {
      // Флешка есть (0.1.146): модуль — в /usb/ELF, запуск — оттуда; кэш ESP не трогается.
      ok = 1;
      where = "stored on the USB drive";
      if (moduleInUse(deliveryName_)) {
        appStale_ = true;
      }
    } else {
      ok = cache_->store(deliveryName_, deliveryStamp_, delivery_, deliverySize_,
                         app_ != nullptr ? appName_ : nullptr)
               ? 1
               : 0;
      if (ok != 0 && markDriveModule(deliveryName_, 0, 0) && moduleInUse(deliveryName_)) {
        appStale_ = true;
      }
      // Перезапуск нужен, только если сменилось само приложение или модуль, который у него
      // загружен (0.1.114). Прежде — после любого: плагин плеера шлёт новый декодер после
      // запуска PLAYER, и экран Tang гас на секунду, хотя декодер PLAYER берёт из кэша при
      // каждом открытии файла (03.11.2026).
      if (ok != 0 && moduleInUse(deliveryName_)) {
        appStale_ = true;
      }
    }
    bringupLog(kTag, "%s %s: %u bytes %s in %u ms",
               deliveryCore_ ? "core" : deliveryResource_ ? "resource" : "module",
               deliveryName_, static_cast<unsigned>(deliverySize_), ok ? where : "REFUSED",
               static_cast<unsigned>((esp_timer_get_time() - started) / 1000));
    // Принятый ресурс — сразу и на флешку, до ответа: плагин всё это время пишет
    // «Storing on the ESP» и запускает проигрывание после.
    if (deliveryResource_ && ok != 0) {
      for (const Resource& item : resources_) {
        if (item.data != nullptr && strcmp(item.name, deliveryName_) == 0) {
          copyResourceToDrive(item);
          break;
        }
      }
    }
  }
  dropDelivery();
  host_.transport().send(kRespModCommit, &ok, 1);
}

bool ModuleApp::adoptResource(const char* name, uint8_t* data, size_t size, uint32_t stamp) {
  for (Resource& item : resources_) {
    if (item.data == nullptr) {
      snprintf(item.name, sizeof(item.name), "%s", name);
      item.data = data;
      item.size = size;
      item.stamp = stamp;
      return true;
    }
  }
  return false;
}

bool ModuleApp::loadResourceFromDrive(const char* name, uint32_t size, uint32_t stamp) {
#if ZIFI_HAS_USB_DRIVE
  if (!ResourceDiskCache::cacheable(name) || !usbDrive().ready()) {
    return false;
  }
  const ResourceDiskCache disk(UsbDrive::kMusicCache);
  if (!disk.fresh(name, size, stamp)) {
    return false;                         // нет копии, другая длина или старее карты
  }
  const int64_t started = esp_timer_get_time();
  const bool read = disk.read(name, delivery_, size);
  const unsigned ms = static_cast<unsigned>((esp_timer_get_time() - started) / 1000);
  if (!read || !adoptResource(name, delivery_, size, stamp)) {
    bringupLog(kTag, "resource %s: %s from the USB drive after %u ms, sent from the card",
               name, read ? "no slot" : "NOT read", ms);
    return false;
  }
  delivery_ = nullptr;                    // теперь у ресурса
  bringupLog(kTag, "resource %s: %u bytes from the USB drive in %u ms", name,
             static_cast<unsigned>(size), ms);
  return true;
#else
  (void)name;
  (void)size;
  (void)stamp;
  return false;
#endif
}

void ModuleApp::copyResourceToDrive(const Resource& resource) {
#if ZIFI_HAS_USB_DRIVE
  if (!ResourceDiskCache::cacheable(resource.name)) {
    return;
  }
  if (!usbDrive().ready()) {
    // Прежде молча: на 0.1.145 флешка не подключилась, и по журналу было не понять почему.
    bringupLog(kTag, "resource %s: not copied to the USB drive (%s)", resource.name,
               usbDrive().status());
    return;
  }
  const ResourceDiskCache disk(UsbDrive::kMusicCache);
  const int64_t started = esp_timer_get_time();
  const bool written = disk.write(resource.name, resource.data,
                                  static_cast<uint32_t>(resource.size), resource.stamp);
  bringupLog(kTag, "resource %s: %s the USB drive in %u ms", resource.name,
             written ? "copied to" : "NOT copied to",
             static_cast<unsigned>((esp_timer_get_time() - started) / 1000));
#else
  (void)resource;
#endif
}

// --- кэш ELF на флешке (0.1.146) ------------------------------------------------
// Пользователь, 19.11.2026: есть флешка FAT32/exFAT — запускаемый ELF копируется в /ELF
// (папки нет — создать) и запускается оттуда; в следующий раз, если копия не старее файла
// на карте Evo, берётся она, а не карта; старее — заменяется файлом с Evo. Модуль в кэше
// ESP — как раньше. Копия годна, если и длина та же (как у кэша ресурсов): другая длина —
// другой файл, и его тоже заменяет файл с карты.

bool ModuleApp::moduleFromDrive(const char* name, uint32_t size, uint32_t stamp) {
#if ZIFI_HAS_USB_DRIVE
  if (!usbDrive().ready()) {
    return false;
  }
  if (!ResourceDiskCache(UsbDrive::kElfFolder).fresh(name, size, stamp)) {
    return false;                         // нет копии, другая длина или старее карты
  }
  if (markDriveModule(name, size, stamp) && moduleInUse(name)) {
    appStale_ = true;                     // работает другая версия — APP_START перезапустит
  }
  bringupLog(kTag, "module %s: %u bytes on the USB drive, not sent from the card", name,
             static_cast<unsigned>(size));
  return true;
#else
  (void)name;
  (void)size;
  (void)stamp;
  return false;
#endif
}

bool ModuleApp::storeModuleOnDrive(const char* name, const uint8_t* data, uint32_t size,
                                   uint32_t stamp) {
#if ZIFI_HAS_USB_DRIVE
  if (!usbDrive().ready()) {
    return false;
  }
  mkdir(UsbDrive::kElfFolder, 0777);      // папку /ELF могли стереть на ПК: нет — создать
  const int64_t started = esp_timer_get_time();
  const bool written = ResourceDiskCache(UsbDrive::kElfFolder).write(name, data, size, stamp);
  const unsigned ms = static_cast<unsigned>((esp_timer_get_time() - started) / 1000);
  if (!written) {
    bringupLog(kTag, "module %s: NOT written to the USB drive in %u ms, the ESP cache instead",
               name, ms);
    return false;
  }
  bringupLog(kTag, "module %s: %u bytes written to the USB drive in %u ms", name,
             static_cast<unsigned>(size), ms);
  markDriveModule(name, size, stamp);
  return true;
#else
  (void)name;
  (void)data;
  (void)size;
  (void)stamp;
  return false;
#endif
}

// Ядро ПЛИС с карты Evo (0.1.164): [флаги 2][длина][время][ИМЯ.BIN, 0] → 0 есть (образ в PSRAM,
// годная копия на флешке /usb/cores или тот же файл в кэше BL616 — с карты не передавать) / 1 шли /
// 2 нет места / 3 не ядро (имя не *.BIN, длина больше слота flash Tang). Принятое ядро — commitCore;
// грузит его core_load.
void ModuleApp::handleCoreQuery(const char* name, uint32_t size, uint32_t stamp) {
#if ZIFI_HAS_SPI_LINK
  char core[16] = {};
  const char* dot = strrchr(name, '.');
  if (dot == nullptr || dot == name || strcmp(dot, ".BIN") != 0 || size > kMaxCoreBytes ||
      static_cast<size_t>(dot - name) >= sizeof(core)) {
    replyQuery(3);
    return;
  }
  memcpy(core, name, static_cast<size_t>(dot - name));
  if (core_store::evoHeld(core, size, stamp)) {
    bringupLog(kTag, "core %s: %u bytes already in PSRAM, not sent from the card", core,
               static_cast<unsigned>(size));
    replyQuery(0);
    return;
  }
#if ZIFI_HAS_USB_DRIVE
  if (usbDrive().ready() && ResourceDiskCache(UsbDrive::kCoreFolder).fresh(name, size, stamp)) {
    core_store::evoOffer(core, nullptr, size, stamp);
    bringupLog(kTag, "core %s: %u bytes on the USB drive, not sent from the card", core,
               static_cast<unsigned>(size));
    replyQuery(0);
    return;
  }
#endif
  // Флешки нет (или копии на ней нет): тот же файл карты уже в кэше BL616 (отметка с SHA, в NVS)
  if (core_store::evoClaimCached(core, size, stamp)) {
    bringupLog(kTag, "core %s: %u bytes already in the BL616 cache, not sent from the card", core,
               static_cast<unsigned>(size));
    replyQuery(0);
    return;
  }
  delivery_ = allocateDelivery(name, size, false);
  if (delivery_ == nullptr) {
    replyQuery(2);
    return;
  }
  memcpy(deliveryName_, name, sizeof(deliveryName_));
  deliverySize_ = size;
  deliveryStamp_ = stamp;
  deliveryReceived_ = 0;
  deliveryResource_ = false;
  deliveryCore_ = true;
  replyQuery(1);
#else
  (void)name;
  (void)size;
  (void)stamp;
  replyQuery(3);
#endif
}

// Принятое ядро с карты Evo: годный битстрим — на флешку /usb/cores (время — как у файла на карте) и
// отметка «с карты Evo» для core_load; флешки нет (или не записалось) — сразу в кэш BL616, а не
// вышло — образ остаётся в PSRAM у core_store. Возвращает, где оно лежит (для журнала); nullptr — отказ.
const char* ModuleApp::commitCore() {
#if ZIFI_HAS_SPI_LINK
  char core[16] = {};
  memcpy(core, deliveryName_, strcspn(deliveryName_, "."));
  if (const char* bad = core_store::checkEvoImage(delivery_, deliverySize_)) {
    bringupLog(kTag, "core %s: not a VDAC4 core bitstream (%s)", core, bad);
    return nullptr;
  }
#if ZIFI_HAS_USB_DRIVE
  if (usbDrive().ready()) {
    mkdir(UsbDrive::kCoreFolder, 0777);   // папку /cores могли стереть на ПК: нет — создать
    const int64_t started = esp_timer_get_time();
    const bool written = ResourceDiskCache(UsbDrive::kCoreFolder)
                             .write(deliveryName_, delivery_, deliverySize_, deliveryStamp_);
    const unsigned ms = static_cast<unsigned>((esp_timer_get_time() - started) / 1000);
    if (written) {
      core_store::evoOffer(core, nullptr, deliverySize_, deliveryStamp_);
      bringupLog(kTag, "core %s: %u bytes written to the USB drive in %u ms", core,
                 static_cast<unsigned>(deliverySize_), ms);
      return "stored on the USB drive";
    }
    bringupLog(kTag, "core %s: NOT written to the USB drive in %u ms, kept in PSRAM", core, ms);
  }
#endif
  // Флешки нет: ядро сразу в кэш BL616 (его flash — постоянный кэш ядер; пользователь: «должно
  // работать и при отсутствии флешки»), не вышло — образ в PSRAM, core_load положит его сам
  const char* problem = core_store::evoStoreToCache(core, delivery_, deliverySize_, deliveryStamp_);
  delivery_ = nullptr;                    // образ теперь у core_store
  if (problem != nullptr) {
    bringupLog(kTag, "core %s: not put into the BL616 cache now (%s), kept in PSRAM", core, problem);
    return "kept in PSRAM";
  }
  return "put into the BL616 cache";
#else
  return nullptr;
#endif
}

void ModuleApp::dropDelivery() {
  if (deliveryData_) {
    appData().drop();                   // недоставленный файл данных: WRITING.TMP — прочь
    deliveryData_ = false;
  }
  if (delivery_ != nullptr) {
    heap_caps_free(delivery_);
  }
  delivery_ = nullptr;
  deliverySize_ = 0;
  deliveryReceived_ = 0;
  deliveryName_[0] = 0;
  deliveryCore_ = false;
}

// [итог int8][приложение, 0][версия, 0]; не вышло и есть текст — он вместо версии
void ModuleApp::replyApp(uint8_t response, int result, const char* fallbackText) {
  uint8_t reply[96];
  size_t used = 0;
  reply[used++] = static_cast<uint8_t>(static_cast<int8_t>(result));
  const char* appName = app_ != nullptr && app_->name != nullptr ? app_->name : "";
  const char* version = app_ != nullptr && app_->version != nullptr ? app_->version : "";
  if (result != ZM_OK && fallbackText != nullptr) {
    appName = "";
    version = fallbackText;
  }
  const size_t nameLength = strnlen(appName, 24);
  memcpy(reply + used, appName, nameLength);
  used += nameLength;
  reply[used++] = 0;
  const size_t versionLength = strnlen(version, 64);
  memcpy(reply + used, version, versionLength);
  used += versionLength;
  reply[used++] = 0;
  host_.transport().send(response, reply, static_cast<uint16_t>(used));
}

// → [1][число][записи: имя 16, длина LE32, флаги (0 — есть ИМЯ.ELF)] или [0][текст, 0]
void ModuleApp::handleCoreList() {
#if ZIFI_HAS_SPI_LINK
  // Плагин выбирает ядро из списка карты Tang: заявки ядер с карты Evo снимаются (0.1.164)
  core_store::evoReleaseClaims();
  static core_store::Entry entries[core_store::kMaxCores];
  static uint8_t reply[2 + core_store::kMaxCores * 21];
  size_t count = 0;
  const char* problem = core_store::list(entries, core_store::kMaxCores, &count, false, tang_sd::kQuickMs);
  if (problem != nullptr) {
    reply[0] = 0;
    const size_t n = strnlen(problem, 120);
    memcpy(reply + 1, problem, n);
    reply[1 + n] = 0;
    host_.transport().send(kRespCoreList, reply, static_cast<uint16_t>(n + 2));
    return;
  }
  size_t used = 0;
  reply[used++] = 1;
  reply[used++] = static_cast<uint8_t>(count);
  for (size_t i = 0; i < count; ++i) {
    memcpy(reply + used, entries[i].name, 16);
    used += 16;
    writeLe32(reply + used, entries[i].size);
    used += 4;
    reply[used++] = entries[i].elf ? 1 : 0;
  }
  host_.transport().send(kRespCoreList, reply, static_cast<uint16_t>(used));
#else
  static const uint8_t reply[] = "\0no LINK to the Tang";
  host_.transport().send(kRespCoreList, reply, sizeof(reply));
#endif
}

// [имя ядра, 0] → [итог int8][приложение, 0][версия или текст ошибки, 0]. ИМЯ.ELF с SD Tang —
// в кэш модулей (если изменился) и запуск; своего ELF у ядра нет — общий слой из прошивки.
void ModuleApp::handleCoreApp(const PacketView& packet) {
#if ZIFI_HAS_SPI_LINK
  char raw[16] = {};
  char name[16] = {};
  const size_t length = strnlen(reinterpret_cast<const char*>(packet.data), packet.length);
  if (length == 0 || length >= sizeof(raw)) {
    replyApp(kRespCoreApp, ZM_ERROR, "bad core name");
    return;
  }
  memcpy(raw, packet.data, length);
  if (!core_cache::normalizeName(raw, name)) {
    replyApp(kRespCoreApp, ZM_ERROR, "bad core name");
    return;
  }
  // Ядро прислано с карты Evo (0.1.164) — его слой тоже оттуда, SD Tang не читается
  if (core_store::evoMarked(name)) {
    startEvoCoreLayer(name);
    return;
  }
  uint8_t* data = nullptr;
  uint32_t size = 0;
  uint32_t stamp = 0;
  const char* problem = core_store::readElf(name, &data, &size, &stamp, tang_sd::kQuickMs);
  if (problem == core_store::kNoElf) {
    bringupLog(kTag, "core %s: no ELF on the Tang SD, built-in core layer", name);
    replyApp(kRespCoreApp, startBuiltin(coreBuiltinApp(), "CORE"), nullptr);
    return;
  }
  if (problem != nullptr) {
    bringupLog(kTag, "core %s: %s", name, problem);
    replyApp(kRespCoreApp, ZM_ERROR, problem);
    return;
  }
  char module[13];
  if (strlen(name) > 8) {
    heap_caps_free(data);
    replyApp(kRespCoreApp, ZM_ERROR, "core name longer than 8 for its ELF");
    return;
  }
  snprintf(module, sizeof(module), "%s.ELF", name);
  int result = ZM_OK;
  bool cacheFull = false;
  const bool changed = !cache_->has(module, size, stamp);
  if(changed) {
    if(const char* bad=elfImageProblem(data,size)) {
      bringupLog(kTag,"core %s: %s is not a loadable ELF (%s)",name,module,bad);
      heap_caps_free(data);replyApp(kRespCoreApp,ZM_ERROR,"core ELF is not loadable");return;
    }
  }
  // A selected different core owns the machine next. Release the old app,
  // abandoned delivery and cached resources before storage/relocation.
  // Repeating an unchanged running core request remains idempotent.
  if(app_==nullptr || strcmp(appName_,module)!=0 || changed || appStale_) {
    stopApp();dropDelivery();
    for(Resource& item : resources_) {
      if(item.data!=nullptr)heap_caps_free(item.data);
      item=Resource{};
    }
    player_overlay::releaseMemory();
  }
  if(changed) {
    const bool stored=cache_->store(module,stamp,data,size,nullptr);
    bringupLog(kTag,"core %s: %s, %u bytes from the Tang SD %s",name,module,
               static_cast<unsigned>(size),stored ? "stored":"REFUSED");
    if(!stored){result=ZM_NO_MEMORY;cacheFull=true;}
    appStale_=appStale_ || stored;
  }
  heap_caps_free(data);
  markDriveModule(module, 0, 0);            // ELF ядра — из кэша ESP, не с флешки
  if (result == ZM_OK) {
    result = startApp(module);
  }
  replyApp(kRespCoreApp, result, result == ZM_NO_MEMORY ? (cacheFull ? "no space for the core ELF in flash cache" : "not enough working RAM to start the core app") : nullptr);
#else
  (void)packet;
  replyApp(kRespCoreApp, ZM_ERROR, "no LINK to the Tang");
#endif
}

// Слой ESP ядра с карты Evo (0.1.164). SD Tang не читается: своё ИМЯ.ELF программа Z80 присылает
// обычным MOD_QUERY — оно в кэше модулей ESP или на флешке (/usb/ELF); не прислано — общий слой из
// прошивки (ему плагин, как прежде, шлёт имя ядра командой 0x70). Ядро грузит сам слой (core_load):
// отметка «с карты Evo» ведёт его к образу на флешке или в PSRAM.
void ModuleApp::startEvoCoreLayer(const char* name) {
#if ZIFI_HAS_SPI_LINK
  char module[13] = {};
  bool own = false;
  if (strlen(name) <= 8) {
    snprintf(module, sizeof(module), "%s.ELF", name);
    uint32_t driveSize = 0;
    own = driveModuleSize(module, &driveSize) || (cache_ != nullptr && cache_->find(module) != nullptr);
  }
  if (!own) {
    bringupLog(kTag, "core %s (Evo card): no ESP layer of its own sent, built-in core layer", name);
    replyApp(kRespCoreApp, startBuiltin(coreBuiltinApp(), "CORE"), nullptr);
    return;
  }
  // Другой слой — прежний уходит со всем, что держал (как у ядра с SD Tang)
  if (app_ == nullptr || strcmp(appName_, module) != 0 || appStale_) {
    stopApp();
    dropDelivery();
    for (Resource& item : resources_) {
      if (item.data != nullptr) {
        heap_caps_free(item.data);
      }
      item = Resource{};
    }
    player_overlay::releaseMemory();
  }
  bringupLog(kTag, "core %s (Evo card): ESP layer %s", name, module);
  const int result = startApp(module);
  replyApp(kRespCoreApp, result, result == ZM_NO_MEMORY ? "not enough working RAM to start the core app" : nullptr);
#else
  (void)name;
#endif
}

int ModuleApp::startBuiltin(const zm_app_t* app, const char* name) {
  if (app_ == app && strcmp(appName_, name) == 0) {
    return ZM_OK;
  }
  stopApp();
  appStale_ = false;
  if (handoff_ != nullptr && !handoffFor(name)) {
    dropHandoff(name);
  }
  xSemaphoreTake(appLock_, portMAX_DELAY);
  appElf_ = nullptr;
  app_ = app;
  strncpy(appName_, name, sizeof(appName_) - 1);
  appName_[sizeof(appName_) - 1] = 0;
  xSemaphoreGive(appLock_);
  const int result = app->start(&core_);
  if (result != ZM_OK) {
    bringupLog(kTag, "built-in app %s: start failed (%d)", name, result);
    stopApp();
    return result;
  }
  xSemaphoreTake(appLock_, portMAX_DELAY);
  appStarted_ = true;
  appCommandMs_ = coreMillis();
  xSemaphoreGive(appLock_);
  bringupLog(kTag, "built-in app %s %s started, commands %02X..%02X", name, app->version,
             app->command_first, app->command_last);
  return ZM_OK;
}

// [имя, 0] → [итог int8][имя, 0][версия, 0]
void ModuleApp::handleStart(const PacketView& packet) {
  char name[13] = {};
  const size_t length = strnlen(reinterpret_cast<const char*>(packet.data), packet.length);
  int result = ZM_ERROR;
  if (length > 0 && length < sizeof(name)) {
    memcpy(name, packet.data, length);
    for (char& c : name) {
      if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
      }
    }
    result = startApp(name);
  }
  uint8_t reply[64];
  size_t used = 0;
  reply[used++] = static_cast<uint8_t>(static_cast<int8_t>(result));
  const char* appName = app_ != nullptr && app_->name != nullptr ? app_->name : "";
  const char* version = app_ != nullptr && app_->version != nullptr ? app_->version : "";
  const size_t nameLength = strnlen(appName, 24);
  memcpy(reply + used, appName, nameLength);
  used += nameLength;
  reply[used++] = 0;
  const size_t versionLength = strnlen(version, 24);
  memcpy(reply + used, version, versionLength);
  used += versionLength;
  reply[used++] = 0;
  host_.transport().send(kRespAppStart, reply, static_cast<uint16_t>(used));
}

// --- загрузка ELF --------------------------------------------------------------

int ModuleApp::loadElf(const char* name, void* arg1, void* arg2, void** elfOut,
                       int* entryResult, bool internal, bool* inCodeRam) {
  *elfOut = nullptr;
  if (inCodeRam != nullptr) {
    *inCodeRam = false;
  }
  // Отмеченный «с флешки» (0.1.146) — оттуда: его нет в кэше ESP или там другая версия.
  uint32_t driveSize = 0;
  const bool fromDrive = driveModuleSize(name, &driveSize);
  const ZmCacheEntry* entry = !fromDrive && cache_ != nullptr ? cache_->find(name) : nullptr;
  if (!fromDrive && entry == nullptr) {
    return ZM_NOT_FOUND;
  }
  const uint32_t size = fromDrive ? driveSize : entry->size;
  uint8_t* file = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (file == nullptr) {
    return ZM_NO_MEMORY;
  }
  if (fromDrive) {
#if ZIFI_HAS_USB_DRIVE
    const int64_t started = esp_timer_get_time();
    const bool read = ResourceDiskCache(UsbDrive::kElfFolder).read(name, file, size);
    const unsigned ms = static_cast<unsigned>((esp_timer_get_time() - started) / 1000);
#else
    const bool read = false;
    const unsigned ms = 0;
#endif
    if (!read) {
      // Флешку вынули или копию стёрли на ПК: следующий запрос пришлёт модуль с карты.
      bringupLog(kTag, "%s: NOT read from the USB drive in %u ms", name, ms);
      markDriveModule(name, 0, 0);
      heap_caps_free(file);
      return ZM_NOT_FOUND;
    }
    bringupLog(kTag, "%s: %u bytes from the USB drive in %u ms", name,
               static_cast<unsigned>(size), ms);
  } else if (!cache_->read(name, file, size)) {
    heap_caps_free(file);
    return ZM_NOT_FOUND;
  }
  // Ещё раз — после чтения из кэша или с флешки: образ мог испортиться.
  if (const char* problem = elfImageProblem(file, size)) {
    bringupLog(kTag, "%s: not a loadable ELF (%s)", name, problem);
    heap_caps_free(file);
#if ZIFI_HAS_USB_DRIVE
    if (fromDrive) {
      // Негодная копия на флешке не должна жить: следующий запрос пришлёт модуль с карты.
      char path[64];
      snprintf(path, sizeof(path), "%s/%s", UsbDrive::kElfFolder, name);
      remove(path);
      markDriveModule(name, 0, 0);
    }
#endif
    return ZM_ERROR;
  }
  // Запись загрузчика о модуле — в PSRAM (0.1.160): с одним MALLOC_CAP_8BIT куча отдавала её из
  // внутреннего ОЗУ (116 байт на модуль, у Metal Slug ~0,7 КБ). Читают её только загрузка и
  // выгрузка; запись о панике держит свою копию адресов (crashRecordModule).
  esp_elf_t* elf = static_cast<esp_elf_t*>(
      heap_caps_calloc(1, sizeof(esp_elf_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (elf == nullptr) {
    heap_caps_free(file);
    return ZM_NO_MEMORY;
  }
  int result = esp_elf_init(elf);
  if (result == 0 && internal && codeRam_.data != nullptr && codeRam_.used < codeRam_.size) {
    // ABI 1.19: остаток куска code_ram. Шина команд внутреннего ОЗУ — тот же байт со смещением
    // SOC_DIRAM_IRAM_LOW − SOC_DIRAM_DRAM_LOW (у ESP32-S3 зеркало прямое).
    elf->text_room = codeRam_.data + codeRam_.used;
    elf->text_room_size = codeRam_.size - codeRam_.used;
    elf->text_room_bus_off = SOC_DIRAM_IRAM_LOW - SOC_DIRAM_DRAM_LOW;
  }
  if (result == 0) {
    result = esp_elf_relocate(elf, file);
  }
  heap_caps_free(file);              // секции скопированы при связывании
  if (result != 0 || elf->entry == nullptr) {
    bringupLog(kTag, "%s: relocate failed (%d)", name, result);
    esp_elf_deinit(elf);
    heap_caps_free(elf);
    return ZM_ERROR;
  }
  // Где код модуля — для записи о панике; кэш команд над ним — сбросить (во внутреннем ОЗУ кэша нет).
  const esp_elf_sec_t& text = elf->sec[ELF_SEC_TEXT];
  uint8_t* code = reinterpret_cast<uint8_t*>(text.addr);
  if (elf->text_borrowed) {
    codeRam_.used += (static_cast<uint32_t>(text.size) + 3u) & ~3u;
    ++codeRam_.modules;
    if (inCodeRam != nullptr) {
      *inCodeRam = true;
    }
  } else {
    forgetStaleCode(code, text.size);
  }
  crashRecordModule(name, code, text.size, text.v_addr);
  if (elf->text_borrowed) {
    bringupLog(kTag, "%s: code at %08X (bus %08X), %u bytes, ELF %X, internal RAM", name,
               static_cast<unsigned>(text.addr), static_cast<unsigned>(text.addr + elf->text_bus_off),
               static_cast<unsigned>(text.size), static_cast<unsigned>(text.v_addr));
  } else {
    bringupLog(kTag, "%s: code at %08X, %u bytes, ELF %X%s", name, static_cast<unsigned>(text.addr),
               static_cast<unsigned>(text.size), static_cast<unsigned>(text.v_addr),
               internal ? ", in PSRAM: no room in code RAM" : "");
  }
  char program[13];
  strncpy(program, name, sizeof(program) - 1);
  program[sizeof(program) - 1] = 0;
  char* argv[] = {program, static_cast<char*>(arg1), static_cast<char*>(arg2), nullptr};
  *entryResult = elf->entry(3, argv);
  *elfOut = elf;
  return ZM_OK;
}

void ModuleApp::unloadElf(void* elf) {
  if (elf == nullptr) {
    return;
  }
  const uint8_t* code =
      reinterpret_cast<const uint8_t*>(static_cast<esp_elf_t*>(elf)->sec[ELF_SEC_TEXT].addr);
  crashRecordModuleGone(code);
  esp_elf_deinit(static_cast<esp_elf_t*>(elf));
  heap_caps_free(elf);
}

// Модуль name — само приложение или загружен им (module_open).
bool ModuleApp::moduleInUse(const char* name) const {
  if (app_ != nullptr && strcmp(appName_, name) == 0) {
    return true;
  }
  for (const LoadedModule& module : moduleSlots()) {
    if (module.used && strcmp(module.name, name) == 0) {
      return true;
    }
  }
  return false;
}

int ModuleApp::startApp(const char* name) {
  // Плагин шлёт APP_START перед каждым файлом. То же приложение уже работает, а
  // ни его образ, ни загруженные им модули с его запуска не менялись, — перезапуск
  // не нужен.
  if (app_ != nullptr && !appStale_ && strcmp(appName_, name) == 0) {
    return ZM_OK;
  }
  stopApp();
  appStale_ = false;
  if (handoff_ != nullptr && !handoffFor(name)) {
    dropHandoff(name);                    // оставлено не этому приложению — память ему
  }
  // Система однозадачная (пользователь, 03.11.2026): новому приложению — вся память. Файловые
  // серверы (SMB, FTP) прочь: сервер SMB после выхода из своего плагина на Evo работал дальше и
  // держал 582 КБ PSRAM, и с ним MIDI в 23 тыс. событий не открывался (0.1.115).
  if (!host_.stopFileServers()) {
    bringupLog(kTag, "app %s: file servers did not stop", name);
  }
#if ZIFI_HAS_USB_DRIVE
  // Серверы могли вырастить буфер передачи драйвера флешки до 16 КиБ (шара USB, 0.1.174): буфер —
  // во внутреннем ОЗУ и живёт, пока флешка вставлена. Приложению — назад kTransferBytes.
  if (usbDrive().ready()) {
    usbDrive().setTransferBytes(UsbDrive::kTransferBytes);
  }
#endif
  // vTaskDelete returns stack memory on the idle task, after stopApp.
  vTaskDelay(pdMS_TO_TICKS(10));
  const zm_app_t* app = nullptr;
  void* elf = nullptr;
  int entryResult = 0;
  int result = loadElf(name, &core_, &app, &elf, &entryResult);
  if (result != ZM_OK) {
    bringupLog(kTag, "app %s: not loaded (%d)", name, result);
    return result;
  }
  // Таблица приложения minor 4 и старше короче нынешней (без ui_*): годится до поля poll
  if (entryResult != ZM_APP_ABI_MAJOR || app == nullptr || app->abi_major != ZM_APP_ABI_MAJOR ||
      app->size < offsetof(zm_app_t, ui_model) || app->start == nullptr) {
    bringupLog(kTag, "app %s: interface %d/%d, not %d", name, entryResult,
               app != nullptr ? app->abi_major : -1, ZM_APP_ABI_MAJOR);
    freeAllocations();
    unloadElf(elf);
    return ZM_ERROR;
  }
  xSemaphoreTake(appLock_, portMAX_DELAY);
  appElf_ = elf;
  app_ = app;
  strncpy(appName_, name, sizeof(appName_) - 1);
  xSemaphoreGive(appLock_);
  remoteCancel(false);                     // файлы данных без флешки: запросы к Z80 снова можно
  const int64_t started = esp_timer_get_time();
  result = app->start(&core_);
  if (result != ZM_OK) {
    bringupLog(kTag, "app %s: start failed (%d); internal %u free, %u largest, %u min; PSRAM %u free", name,
               result, static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
               static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
               static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
               static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    stopApp();
    return result;
  }
  xSemaphoreTake(appLock_, portMAX_DELAY);
  appStarted_ = true;
  appCommandMs_ = coreMillis();
  xSemaphoreGive(appLock_);
  bringupLog(kTag, "app %s %s %s started in %u us, commands %02X..%02X", name,
             app->name != nullptr ? app->name : "?", app->version != nullptr ? app->version : "",
             static_cast<unsigned>(esp_timer_get_time() - started), app->command_first,
             app->command_last);
  return ZM_OK;
}

void ModuleApp::stopApp() {
  if (app_ == nullptr && appElf_ == nullptr) {
    return;
  }
  xSemaphoreTake(appLock_, portMAX_DELAY);
  const zm_app_t* app = app_;
  app_ = nullptr;
  appStarted_ = false;
  setAppMediaRange(1, 0);                 // порции DMA без суммы — больше ни одной команде
  xSemaphoreGive(appLock_);
  // Задача приложения может ждать кусок файла данных у программы Z80 (0.1.166): до stop() — оборвать,
  // иначе stop() ждал бы её до срока запроса.
  remoteCancel(true);
  if (g_remote.requests != 0) {
    bringupLog(kTag, "app %s: data files read from the Evo card: %u KB in %u ms, %u requests "
               "(%u repeated), longest %u ms", appName_, static_cast<unsigned>(g_remote.bytes / 1024),
               static_cast<unsigned>(g_remote.ms), static_cast<unsigned>(g_remote.requests),
               static_cast<unsigned>(g_remote.repeats), static_cast<unsigned>(g_remote.longest));
  }
#if ZIFI_HAS_SPI_LINK
  player_overlay::appGone();             // экран приложения — сразу прочь
#endif
  if (app != nullptr && app->stop != nullptr) {
    app->stop();
  }
  // Задачи, что не кончились сами за полсекунды, — снять.
  for (int wait = 0; wait < 50; ++wait) {
    bool running = false;
    for (TaskSlot& slot : tasks_) {
      running = running || slot.running;
    }
    if (!running) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  for (TaskSlot& slot : tasks_) {
    if ((slot.running || slot.finished) && slot.handle != nullptr) {
      if (slot.psram) {
        vTaskDeleteWithCaps(slot.handle);   // и кончившуюся (finished): она стоит, ждёт ядра 1
      } else {
        vTaskDelete(slot.handle);
      }
    }
    slot = TaskSlot{};
  }
  // Приложение было хостом FT812: вернуть его Evo (задачи приложения уже не пишут)
  coreFtClose();
  for (size_t i = 0; i < kMaxSockets; ++i) {
    coreSockClose(static_cast<int>(i));
  }
  appData().closeAll();                  // файлы данных приложения (data_open, 0.1.165)
  for (size_t i = 0; i < kMaxDiskFiles; ++i) {
    if (g_diskFiles[i] != nullptr) {
      coreSdClose(g_diskFiles[i]);
    }
    if (g_sd2Files[i] != nullptr) {
      coreSd2Close(g_sd2Files[i]);
    }
  }
  // Приложение загрузило ядро из кэша BL616 (ZX Next): Tang — назад в TS-Config. Ядро ещё грузится —
  // вернёт задача загрузки, когда кончит. Возврат не вышел — ядро остаётся без хозяина, и задача
  // LINK через несколько секунд повторит BOOT сама (tang::kOrphanMs).
  if (coreLoaded_) {
    coreLoaded_ = false;
#if ZIFI_HAS_SPI_LINK
    if (coreJob_.running) {
      coreJob_.abandoned = true;
      bringupLog(kTag, "app stopped while its core loads: the loader returns to slot 0");
    } else {
      if (coreCoreBack(10000) != ZM_OK) {
        bringupLog(kTag, "app stopped: back to slot 0 FAILED, the LINK task retries");
      }
      tang::releaseCore();
    }
#endif
  }
  for (SemaphoreHandle_t& sync : sync_) {
    if (sync != nullptr) {
      vSemaphoreDelete(sync);
      sync = nullptr;
    }
  }
  closeStreams();
#if ZIFI_HAS_SPI_LINK
  tang::releaseBurstDma();                // буферы DMA LINK (link_burst_dma) — назад во внутреннее ОЗУ
#endif
  for (LoadedModule& module : moduleSlots()) {
    if (module.used) {
      unloadElf(module.elf);
      module = LoadedModule{};
    }
  }
  releaseCodeRam();                       // код модулей уже снят: кусок и исполнение — закрыть
  player_.pcmLowLatency(false);           // короткий DMA — только этому приложению
  player_.pcmClose();
  freeAllocations();
  unloadElf(appElf_);
  ftDetach_.reset();
  appElf_ = nullptr;
  bringupLog(kTag, "app %s stopped", appName_);
  if (handoff_ != nullptr && handoffFor(appName_)) {
    dropHandoff("its app stopped without taking it");   // получатель (старый слой) не взял
  }
  appName_[0] = 0;
}

// --- ABI 1.14: файл следующему приложению -------------------------------------------------------
//
// Экран ядер (COREMENU.ELF) принимает образ с карты Evo, рисуя ход, и оставляет его слою ядра (MD.ELF):
// блок из alloc экрана уходит из списка его памяти (выгрузка экрана его не освободит) и ждёт
// получателя. Получатель берёт его в свой список. Запустилось другое приложение или получатель
// выгружен, не взяв, — блок освобождается: несколько мегабайт PSRAM не висят.

// Блок оставлен приложению app («MD.ELF» или «MD»)?
bool ModuleApp::handoffFor(const char* app) const {
  if (handoff_ == nullptr || app == nullptr) {
    return false;
  }
  size_t n = strlen(handoffTo_);
  return strncasecmp(app, handoffTo_, n) == 0 && (app[n] == 0 || app[n] == '.');
}

void ModuleApp::dropHandoff(const char* why) {
  portENTER_CRITICAL(&allocLock_);
  AllocHeader* header = static_cast<AllocHeader*>(handoff_);
  handoff_ = nullptr;
  portEXIT_CRITICAL(&allocLock_);
  if (header == nullptr) {
    return;
  }
  bringupLog(kTag, "handoff: %u bytes from %s for %s freed (%s)", static_cast<unsigned>(handoffSize_),
             handoffFrom_, handoffTo_, why);
  header->magic = 0;
  heap_caps_free(header);
}

int ModuleApp::coreHandoffPut(const char* to, void* block, uint32_t size) {
  ModuleApp* self = instance_;
  if (self == nullptr || to == nullptr || *to == 0 || block == nullptr) {
    return ZM_ERROR;
  }
  AllocHeader* header = static_cast<AllocHeader*>(block) - 1;
  if (header->magic != kAllocMagic) {
    return ZM_ERROR;
  }
  if (self->handoff_ != nullptr) {
    self->dropHandoff("replaced");
  }
  portENTER_CRITICAL(&self->allocLock_);
  if (header->prev != nullptr) {
    header->prev->next = header->next;
  } else {
    self->allocations_ = header->next;
  }
  if (header->next != nullptr) {
    header->next->prev = header->prev;
  }
  header->next = header->prev = nullptr;
  self->handoff_ = header;
  self->handoffSize_ = size;
  portEXIT_CRITICAL(&self->allocLock_);
  strncpy(self->handoffTo_, to, sizeof(self->handoffTo_) - 1);
  self->handoffTo_[sizeof(self->handoffTo_) - 1] = 0;
  strncpy(self->handoffFrom_, self->appName_, sizeof(self->handoffFrom_) - 1);
  self->handoffFrom_[sizeof(self->handoffFrom_) - 1] = 0;
  bringupLog(kTag, "handoff: %s leaves %u bytes for %s", self->appName_, static_cast<unsigned>(size),
             self->handoffTo_);
  return ZM_OK;
}

void* ModuleApp::coreHandoffTake(uint32_t* size) {
  ModuleApp* self = instance_;
  if (self == nullptr || self->handoff_ == nullptr || !self->handoffFor(self->appName_)) {
    return nullptr;
  }
  portENTER_CRITICAL(&self->allocLock_);
  AllocHeader* header = static_cast<AllocHeader*>(self->handoff_);
  self->handoff_ = nullptr;
  header->prev = nullptr;
  header->next = static_cast<AllocHeader*>(self->allocations_);
  if (header->next != nullptr) {
    header->next->prev = header;
  }
  self->allocations_ = header;
  portEXIT_CRITICAL(&self->allocLock_);
  if (size != nullptr) {
    *size = self->handoffSize_;
  }
  bringupLog(kTag, "handoff: %s took %u bytes from %s", self->appName_,
             static_cast<unsigned>(self->handoffSize_), self->handoffFrom_);
  return header + 1;
}

void ModuleApp::freeAllocations() {
  portENTER_CRITICAL(&allocLock_);
  AllocHeader* header = static_cast<AllocHeader*>(allocations_);
  allocations_ = nullptr;
  portEXIT_CRITICAL(&allocLock_);
  while (header != nullptr) {
    AllocHeader* next = header->next;
    header->magic = 0;
    heap_caps_free(header);
    header = next;
  }
}

// --- температура кристалла ---------------------------------------------------
//
// Раз в секунду: для AUDIO_STATUS (core->temperature) и записи о панике.

void ModuleApp::temperatureTask(void* self) {
  (void)self;
  for (;;) {
    float celsius = 0;
    if (temperature_sensor_get_celsius(g_thermometer, &celsius) == ESP_OK) {
      g_temperature = static_cast<int16_t>(lroundf(celsius * 10));
      crashRecordTemperature(g_temperature);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// --- таблица ядра ----------------------------------------------------------------

void* ModuleApp::coreAlloc(size_t size, uint32_t caps) {
  AllocHeader* header = static_cast<AllocHeader*>(
      heap_caps_malloc(size + sizeof(AllocHeader), heapCaps(caps)));
  if (header == nullptr) {
    // Другой вид памяти лучше, чем никакой.
    header = static_cast<AllocHeader*>(
        heap_caps_malloc(size + sizeof(AllocHeader), heapCaps(caps ^ ZM_MEM_INTERNAL)));
  }
#if ZIFI_HAS_SPI_LINK
  if (header == nullptr && (caps & ZM_MEM_INTERNAL) == 0) {
    // PSRAM нужна приложению: холст экрана плеера — прочь (окно плагина на Evo остаётся)
    player_overlay::releaseMemory();
    header = static_cast<AllocHeader*>(
        heap_caps_malloc(size + sizeof(AllocHeader), heapCaps(caps)));
  }
#endif
  if (header == nullptr) {
    return nullptr;
  }
  header->caps = caps;
  header->magic = kAllocMagic;
  header->prev = nullptr;
  portENTER_CRITICAL(&instance_->allocLock_);
  header->next = static_cast<AllocHeader*>(instance_->allocations_);
  if (header->next != nullptr) {
    header->next->prev = header;
  }
  instance_->allocations_ = header;
  portEXIT_CRITICAL(&instance_->allocLock_);
  return header + 1;
}

void ModuleApp::coreFree(void* pointer) {
  if (pointer == nullptr) {
    return;
  }
  AllocHeader* header = static_cast<AllocHeader*>(pointer) - 1;
  if (header->magic != kAllocMagic) {
    return;                            // не наш блок: не трогать
  }
  portENTER_CRITICAL(&instance_->allocLock_);
  if (header->prev != nullptr) {
    header->prev->next = header->next;
  } else {
    instance_->allocations_ = header->next;
  }
  if (header->next != nullptr) {
    header->next->prev = header->prev;
  }
  portEXIT_CRITICAL(&instance_->allocLock_);
  header->magic = 0;
  heap_caps_free(header);
}

void* ModuleApp::coreRealloc(void* pointer, size_t size) {
  if (pointer == nullptr) {
    return coreAlloc(size, ZM_MEM_PSRAM);
  }
  if (size == 0) {
    coreFree(pointer);
    return nullptr;
  }
  AllocHeader* header = static_cast<AllocHeader*>(pointer) - 1;
  if (header->magic != kAllocMagic) {
    return nullptr;
  }
  const size_t old = heap_caps_get_allocated_size(header) - sizeof(AllocHeader);
  // 0.1.166: на меньший размер — на месте. Куча ESP-IDF (TLSF) отрезает хвост блока и оставляет адрес: ни
  // второго блока на время копии (у блока в 2 МБ это ещё до 2 МБ PSRAM), ни сдвига — приложение вправе держать
  // указатели внутрь блока (Quake 2: Hunk_End урезает кусок, в который уже разложена модель). Прежде realloc
  // всегда брал новый блок и копировал — найдено в Unreal NG 24.11.2026 (его модель ESP повторяет прошивку).
  // Вид памяти — тот, где блок лежит (coreAlloc мог взять другой, если нужного не было): иначе
  // heap_caps_realloc перенёс бы блок туда, куда просили.
  if (size <= old) {
    portENTER_CRITICAL(&instance_->allocLock_);
    if (header->prev != nullptr) {
      header->prev->next = header->next;
    } else {
      instance_->allocations_ = header->next;
    }
    if (header->next != nullptr) {
      header->next->prev = header->prev;
    }
    portEXIT_CRITICAL(&instance_->allocLock_);
    const uint32_t kind = esp_ptr_external_ram(header) ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                                       : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    AllocHeader* kept =
        static_cast<AllocHeader*>(heap_caps_realloc(header, size + sizeof(AllocHeader), kind));
    if (kept == nullptr) {
      kept = header;                      // не вышло — блок прежний (realloc его не тронул)
    }
    kept->prev = nullptr;
    portENTER_CRITICAL(&instance_->allocLock_);
    kept->next = static_cast<AllocHeader*>(instance_->allocations_);
    if (kept->next != nullptr) {
      kept->next->prev = kept;
    }
    instance_->allocations_ = kept;
    portEXIT_CRITICAL(&instance_->allocLock_);
    return kept + 1;
  }
  void* fresh = coreAlloc(size, header->caps);
  if (fresh == nullptr) {
    return nullptr;
  }
  memcpy(fresh, pointer, old < size ? old : size);
  coreFree(pointer);
  return fresh;
}

void ModuleApp::coreLog(const char* format, ...) {
  char line[136];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(line, sizeof(line), format, arguments);
  va_end(arguments);
  bringupLog("zifi-app", "%s", line);
}

uint32_t ModuleApp::coreMillis() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void ModuleApp::coreDelay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms == 0 ? 1 : ms)); }

namespace {
// Место задачи: занять (coreTaskStart) и освободить снятую (reapTasks) — под замком: снимает ядро 1, а
// задачи приложение запускает и из своих задач (ядро 0).
portMUX_TYPE g_taskSlotLock = portMUX_INITIALIZER_UNLOCKED;
}  // namespace

void ModuleApp::taskTrampoline(void* context) {
  TaskSlot* slot = static_cast<TaskSlot*>(context);
  slot->fn(slot->arg);
  if (slot->psram) {
    // Стек в PSRAM (0.1.152): себя не удалять — vTaskDeleteWithCaps(nullptr) запускает задачу очистки
    // во внутренней памяти, а её при плеере нет (abort; вероятно, сбой PLAYER 1.34 при радио). Встать
    // и ждать: снимет ядро 1 (reapTasks), место освободится тогда же.
    slot->finished = true;
    slot->running = false;
    vTaskSuspend(nullptr);
    for (;;) vTaskDelay(portMAX_DELAY);   // сюда не возвращаются
  }
  slot->running = false;                  // место может тут же занять другая задача
  slot->handle = nullptr;
  vTaskDelete(nullptr);
}

// Ядро 1 (цикл обмена): кончившиеся задачи со стеком в PSRAM — снять. Задача уже стоит (vTaskSuspend):
// удалить её отсюда — стек и TCB освобождаются сразу, без задачи очистки.
void ModuleApp::reapTasks() {
  for (TaskSlot& slot : tasks_) {
    if (!slot.finished || slot.handle == nullptr) continue;
    if (eTaskGetState(slot.handle) != eSuspended) continue;   // ещё не встала
    TaskHandle_t handle = nullptr;
    portENTER_CRITICAL(&g_taskSlotLock);
    if (slot.finished) {                  // её не снимает ли уже другая задача (task_start с ядра 0)
      handle = slot.handle;
      slot.finished = false;
      slot.running = true;                // место занято, пока снимаем
    }
    portEXIT_CRITICAL(&g_taskSlotLock);
    if (handle == nullptr) continue;
    vTaskDeleteWithCaps(handle);
    portENTER_CRITICAL(&g_taskSlotLock);
    slot = TaskSlot{};
    portEXIT_CRITICAL(&g_taskSlotLock);
  }
}

int ModuleApp::coreTaskStart(zm_task_fn fn, void* arg, uint32_t stack, uint8_t priority,
                             int8_t cpu) {
  if (fn == nullptr) {
    return ZM_ERROR;
  }
  // ZM_TASK_STACK_PSRAM (0.1.112): внутренней памяти на стек нет — задаче, которой это можно, стек в
  // PSRAM. 03.11.2026 при радио задаче поиска обложки PLAYER (6 КБ) внутренней не хватало (свободно
  // ~2 КБ, самый большой кусок 704 байта). Без флага — как прежде, только внутренняя.
  // ZM_TASK_STACK_PSRAM_FIRST (0.1.153, ABI 18): во внутренней не пробовать — она нужна задаче на другое
  const bool psramFirst = (stack & ZM_TASK_STACK_PSRAM_FIRST) != 0;
  const bool psramAllowed = psramFirst || (stack & ZM_TASK_STACK_PSRAM) != 0;
  stack &= ~(ZM_TASK_STACK_PSRAM | ZM_TASK_STACK_PSRAM_FIRST);
  const uint32_t depth = stack < 2048 ? 2048 : stack;
  instance_->reapTasks();                 // место кончившейся задачи со стеком в PSRAM — свободно
  for (TaskSlot& slot : instance_->tasks_) {
    bool claimed = false;
    portENTER_CRITICAL(&g_taskSlotLock);
    if (!slot.running && !slot.finished) {
      slot.running = true;
      claimed = true;
    }
    portEXIT_CRITICAL(&g_taskSlotLock);
    if (claimed) {
      slot.fn = fn;
      slot.arg = arg;
      slot.psram = false;
      const BaseType_t core = cpu < 0 ? tskNO_AFFINITY : cpu;
      const UBaseType_t level = priority > 20 ? 20 : priority;
      const size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      const size_t internalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      BaseType_t made=pdFAIL;
      uint32_t waited=0;
      if (!psramFirst) allocateAfterCleanup(
          [&]() {
            made=xTaskCreatePinnedToCore(taskTrampoline, "zm-app", depth, &slot, level,
                                         &slot.handle, core);
            return made==pdPASS;
          },
          [&]() {
#if ZIFI_HAS_SPI_LINK
            if(!instance_->appStarted_)player_overlay::releaseMemory();
#endif
          },
          [&](uint32_t ms) {vTaskDelay(pdMS_TO_TICKS(ms));waited+=ms;},
          psramAllowed ? 2U:25U,10U);
      if(made==pdPASS && waited!=0) {
        bringupLog(kTag,"app task: %u-byte internal stack after %u ms of cleanup",
                   static_cast<unsigned>(depth),static_cast<unsigned>(waited));
      }
      if (made != pdPASS && psramAllowed) {
        slot.psram = true;                // до создания: задача может кончиться сразу
        made = xTaskCreatePinnedToCoreWithCaps(taskTrampoline, "zm-app", depth, &slot, level,
                                               &slot.handle, core, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (made == pdPASS) {
          bringupLog(kTag, "app task: %u-byte stack in PSRAM (internal %u free, %u largest)",
                     static_cast<unsigned>(depth), static_cast<unsigned>(internalFree),
                     static_cast<unsigned>(internalLargest));
        }
      }
      if (made != pdPASS) {
        bringupLog(kTag, "app task: %u-byte stack not allocated (internal %u free, %u largest%s)",
                   static_cast<unsigned>(stack), static_cast<unsigned>(internalFree),
                   static_cast<unsigned>(internalLargest), psramAllowed ? ", PSRAM too" : "");
        slot = TaskSlot{};
        return ZM_NO_MEMORY;
      }
      return ZM_OK;
    }
  }
  return ZM_BUSY;
}

void* ModuleApp::coreMutexCreate() {
  for (SemaphoreHandle_t& sync : instance_->sync_) {
    if (sync == nullptr) {
      sync = xSemaphoreCreateMutex();
      return sync;
    }
  }
  return nullptr;
}

void ModuleApp::coreMutexLock(void* mutex) {
  if (mutex != nullptr) {
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex), portMAX_DELAY);
  }
}

void ModuleApp::coreMutexUnlock(void* mutex) {
  if (mutex != nullptr) {
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex));
  }
}

void* ModuleApp::coreSignalCreate() {
  for (SemaphoreHandle_t& sync : instance_->sync_) {
    if (sync == nullptr) {
      sync = xSemaphoreCreateBinary();
      return sync;
    }
  }
  return nullptr;
}

void ModuleApp::coreSignalGive(void* signal) {
  if (signal != nullptr) {
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(signal));
  }
}

int ModuleApp::coreSignalWait(void* signal, uint32_t timeoutMs) {
  if (signal == nullptr) {
    return ZM_ERROR;
  }
  return xSemaphoreTake(static_cast<SemaphoreHandle_t>(signal), pdMS_TO_TICKS(timeoutMs)) ==
                 pdTRUE
             ? ZM_OK
             : ZM_TIMEOUT;
}

int ModuleApp::coreReply(uint8_t response, const void* data, uint16_t length) {
  if (!instance_->inCommand_) {
    return ZM_ERROR;                   // ответ возможен только на текущую команду
  }
  return instance_->host_.transport().send(response, static_cast<const uint8_t*>(data), length)
             ? ZM_OK
             : ZM_ERROR;
}

int ModuleApp::coreEvent(uint8_t code, const void* data, uint16_t length, uint32_t waitMs) {
  return instance_->host_.sendEvent(code, static_cast<const uint8_t*>(data), length, waitMs)
             ? ZM_OK
             : ZM_BUSY;
}

int ModuleApp::corePcmOpen(const zm_pcm_format_t* format) {
  if (format == nullptr) {
    return ZM_ERROR;
  }
  return instance_->player_.pcmOpen(format->rate, format->channels) ? ZM_OK : ZM_ERROR;
}

int ModuleApp::corePcmWrite(const float* frames, uint32_t count) {
  return static_cast<int>(instance_->player_.pcmWrite(frames, count));
}

uint32_t ModuleApp::corePcmQueuedMs() { return instance_->player_.pcmQueuedMs(); }

void ModuleApp::corePcmFlush() { instance_->player_.pcmFlush(); }

void ModuleApp::corePcmClose() { instance_->player_.pcmClose(); }

void ModuleApp::corePcmPause(int paused) { instance_->player_.pause(paused != 0); }

void ModuleApp::coreSetVolume(uint8_t volume) { instance_->player_.setVolume(volume); }

uint8_t ModuleApp::coreVolume() { return instance_->player_.volume(); }

uint16_t ModuleApp::corePcmFaults() { return instance_->player_.i2sFaults(); }

int16_t ModuleApp::coreTemperature() { return g_temperature; }

int16_t ModuleApp::temperatureTenths() const { return g_temperature; }

// --- потоки HTTP(S) приложений (интернет-радио) --------------------------------

void* ModuleApp::coreNetOpen(const char* url, const char* extra, uint32_t timeoutMs,
                             int* status) {
  ModuleApp* self = instance_;
  if (status != nullptr) {
    *status = 0;
  }
  if (url == nullptr) {
    return nullptr;
  }
  int slot = -1;
  portENTER_CRITICAL(&self->streamLock_);
  for (size_t i = 0; i < kMaxStreams; ++i) {
    if (!self->streamUsed_[i]) {
      self->streamUsed_[i] = true;
      slot = static_cast<int>(i);
      break;
    }
  }
  portEXIT_CRITICAL(&self->streamLock_);
  if (slot < 0) {
    bringupLog(kTag, "net: no free stream (%u open)", static_cast<unsigned>(kMaxStreams));
    return nullptr;
  }
  if (self->streams_[slot] == nullptr) {
    // Объект большой (буферы заголовков, клиент TLS): в PSRAM.
    void* memory = heap_caps_malloc(sizeof(HttpStream), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr) {
      memory = heap_caps_malloc(sizeof(HttpStream), MALLOC_CAP_8BIT);
    }
    if (memory == nullptr) {
      self->streamUsed_[slot] = false;
      return nullptr;
    }
    self->streams_[slot] = new (memory) HttpStream();
  }
  HttpStream* stream = self->streams_[slot];
  int code = 0;
  if (!stream->open(url, extra, timeoutMs, code)) {
    bringupLog(kTag, "net: %.60s: %s", url, stream->error());
    self->streamUsed_[slot] = false;
    if (status != nullptr) {
      *status = code;
    }
    return nullptr;
  }
  if (status != nullptr) {
    *status = code;
  }
  return stream;
}

const char* ModuleApp::coreNetHeader(void* stream, const char* name) {
  return stream != nullptr && name != nullptr ? static_cast<HttpStream*>(stream)->header(name)
                                              : nullptr;
}

int ModuleApp::coreNetRead(void* stream, void* data, uint32_t count, uint32_t timeoutMs) {
  return stream != nullptr
             ? static_cast<HttpStream*>(stream)->read(static_cast<uint8_t*>(data), count, timeoutMs)
             : -1;
}

void ModuleApp::coreNetClose(void* stream) {
  ModuleApp* self = instance_;
  for (size_t i = 0; i < kMaxStreams; ++i) {
    if (stream != nullptr && self->streams_[i] == stream) {
      self->streams_[i]->close();
      self->streamUsed_[i] = false;
    }
  }
}

// --- FT812 эмулятора Tang: приложение — хост (ft_host.hpp) --------------------------------------
int ModuleApp::coreFtOpen(uint32_t timeoutMs) {
#if ZIFI_HAS_SPI_LINK
  if (!tang::ready()) {
    return ZM_NOT_FOUND;
  }
  if (!ft_host::switchToFtCore(timeoutMs)) {
    return ZM_NOT_FOUND;
  }
  if (!ft_host::setHost(true)) {
    return ZM_ERROR;
  }
  instance_->ftOpened_ = true;
  uint32_t ftState[3] = {};
  const bool selected = tang::read(0, ftState, 3) && ftState[0] == 0x56444334u &&
                        (ftState[1] >> 24) == 2 && (ftState[2] & (1u << 12));
  instance_->ftDetach_.opened(selected);
  return ft_host::boot1024x768() ? ZM_OK : ZM_ERROR;
#else
  (void)timeoutMs;
  return ZM_NOT_FOUND;
#endif
}

int ModuleApp::coreFtWrite(uint32_t address, const uint32_t* words, uint32_t count) {
#if ZIFI_HAS_SPI_LINK
  return words != nullptr && (address & 3) == 0 && ft_host::writeBlock(address, words, count)
             ? ZM_OK
             : ZM_ERROR;
#else
  (void)address; (void)words; (void)count;
  return ZM_ERROR;
#endif
}

int ModuleApp::coreFtRead(uint32_t address, uint32_t* value) {
#if ZIFI_HAS_SPI_LINK
  return value != nullptr && (address & 3) == 0 && ft_host::read32(address, value) ? ZM_OK : ZM_ERROR;
#else
  (void)address; (void)value;
  return ZM_ERROR;
#endif
}

int ModuleApp::coreFtHostCmd(uint8_t command, uint8_t parameter) {
#if ZIFI_HAS_SPI_LINK
  return ft_host::hostCommand(command, parameter) ? ZM_OK : ZM_ERROR;
#else
  (void)command; (void)parameter;
  return ZM_ERROR;
#endif
}

void ModuleApp::coreFtClose() {
#if ZIFI_HAS_SPI_LINK
  if (instance_->ftOpened_) {
    instance_->ftOpened_ = false;
    ft_host::setHost(false);
  }
#endif
}

// --- ABI 1.4: ядра ПЛИС из кэша BL616 (core_store.hpp), LINK, сокеты TCP/UDP ------------------
#if ZIFI_HAS_SPI_LINK
void ModuleApp::coreLoaderTask(void* context) {
  ModuleApp* self = static_cast<ModuleApp*>(context);
  for (;;) {
    xSemaphoreTake(self->coreJobGo_, portMAX_DELAY);
    CoreJob& job = self->coreJob_;
    uint32_t v = 0;
    const char* problem = core_store::load(job.name, &v, nullptr, nullptr);
    if (problem != nullptr) {
      bringupLog(kTag, "core_load %s: %s", job.name, problem);
      job.result = strstr(problem, "no such core") != nullptr ? ZM_NOT_FOUND : ZM_ERROR;
    } else {
      job.version = v;
      job.result = ZM_OK;
    }
    if (job.abandoned) {
      // приложения уже нет: ядро — назад, хозяина у него больше нет
      if (job.result == ZM_OK && !tang::backToSlot0(10000)) {
        bringupLog(kTag, "core %s: back to slot 0 FAILED, the LINK task retries", job.name);
      }
      tang::releaseCore();
    }
    job.running = false;
    xSemaphoreGive(self->coreJobDone_);
  }
}
#endif

int ModuleApp::coreCoreLoad(const char* name, uint32_t* version) {
#if ZIFI_HAS_SPI_LINK
  ModuleApp* self = instance_;
  if (name == nullptr || !tang::ready() || self->coreLoader_ == nullptr) {
    return ZM_ERROR;
  }
  if (self->coreJob_.running) {
    return ZM_BUSY;
  }
  CoreJob& job = self->coreJob_;
  strncpy(job.name, name, sizeof job.name - 1);
  job.name[sizeof job.name - 1] = 0;
  job.version = 0;
  job.result = ZM_ERROR;
  job.abandoned = false;
  job.running = true;
  // Переключение начато: при выгрузке приложения — назад в слот 0, а у нового ядра — хозяин
  self->coreLoaded_ = true;
  tang::claimCore();
  xSemaphoreTake(self->coreJobDone_, 0);
  xSemaphoreGive(self->coreJobGo_);
  xSemaphoreTake(self->coreJobDone_, portMAX_DELAY);   // задачу приложения могут снять здесь
  if (job.result != ZM_OK) {
    self->coreLoaded_ = false;
    tang::releaseCore();
    return job.result;
  }
  if (version != nullptr) {
    *version = job.version;
  }
  return ZM_OK;
#else
  (void)name; (void)version;
  return ZM_NOT_FOUND;
#endif
}

int ModuleApp::coreCoreBack(uint32_t timeoutMs) {
#if ZIFI_HAS_SPI_LINK
  // Приложение отпускает ядро в любом случае: возврат не вышел — задача LINK повторит BOOT сама
  // (tang::kOrphanMs), а выгрузка приложения — ещё раз здесь. Вышел — ядра у приложения больше нет:
  // выгрузка не тронет то, что будет на Tang потом (FT812, ядро консоли).
  tang::releaseCore();
  if (!tang::ready() || !tang::backToSlot0(timeoutMs)) {
    return tang::ready() ? ZM_TIMEOUT : ZM_ERROR;
  }
  instance_->coreLoaded_ = false;
  if (tang_sd::heldByWc()) {
    tang_sd::holdForWc();             // карта у WC (видео с SD2): снова к мосту TS-Config
  }
  return ZM_OK;
#else
  (void)timeoutMs;
  return ZM_ERROR;
#endif
}

int ModuleApp::coreLinkRead(uint32_t address, uint32_t* words, uint32_t count) {
#if ZIFI_HAS_SPI_LINK
  return words != nullptr && (address & 3) == 0 && count > 0 && count <= 16 && tang::read(address, words, count)
             ? ZM_OK
             : ZM_ERROR;
#else
  (void)address; (void)words; (void)count;
  return ZM_ERROR;
#endif
}

// ABI 1.21 (0.1.168): то же, что coreLinkRead, но слова читаются 2–3 раза и сверяются
// (tang::readChecked, почему так — в link_vote.hpp).
int ModuleApp::coreLinkReadChecked(uint32_t address, uint32_t* words, uint32_t count) {
#if ZIFI_HAS_SPI_LINK
  return words != nullptr && (address & 3) == 0 && count > 0 && count <= 16 &&
                 tang::readChecked(address, words, count)
             ? ZM_OK
             : ZM_ERROR;
#else
  (void)address; (void)words; (void)count;
  return ZM_ERROR;
#endif
}

int ModuleApp::coreLinkWrite(uint32_t address, const uint32_t* words, uint32_t count) {
#if ZIFI_HAS_SPI_LINK
  return words != nullptr && (address & 3) == 0 && count > 0 && count <= 16 && tang::write(address, words, count)
             ? ZM_OK
             : ZM_ERROR;
#else
  (void)address; (void)words; (void)count;
  return ZM_ERROR;
#endif
}

int ModuleApp::coreSockOpen(int udp, const char* host, uint16_t port, uint32_t timeoutMs) {
  ModuleApp* self = instance_;
  if (host == nullptr) {
    return ZM_ERROR;
  }
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
  addrinfo* found = nullptr;
  char service[8];
  snprintf(service, sizeof service, "%u", static_cast<unsigned>(port));
  if (getaddrinfo(host, service, &hints, &found) != 0 || found == nullptr) {
    bringupLog(kTag, "sock: %.40s: no address", host);
    return ZM_ERROR;
  }
  const int fd = socket(found->ai_family, found->ai_socktype, 0);
  if (fd < 0) {
    freeaddrinfo(found);
    return ZM_ERROR;
  }
  // TCP: соединение без блокировки, ждать до timeoutMs
  const int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int rc = connect(fd, found->ai_addr, found->ai_addrlen);
  freeaddrinfo(found);
  if (rc != 0 && errno == EINPROGRESS) {
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
    timeval tv = {static_cast<long>(timeoutMs / 1000), static_cast<long>((timeoutMs % 1000) * 1000)};
    rc = select(fd + 1, nullptr, &w, nullptr, &tv) == 1 ? 0 : -1;
    int error = 0;
    socklen_t len = sizeof error;
    if (rc == 0 && (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0 || error != 0)) {
      rc = -1;
    }
  }
  if (rc != 0) {
    close(fd);
    return ZM_ERROR;
  }
  fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
  int slot = -1;
  portENTER_CRITICAL(&self->sockLock_);
  for (size_t i = 0; i < kMaxSockets; ++i) {
    if (self->sockets_[i] < 0) {
      self->sockets_[i] = fd;
      slot = static_cast<int>(i);
      break;
    }
  }
  portEXIT_CRITICAL(&self->sockLock_);
  if (slot < 0) {
    close(fd);
    return ZM_ERROR;
  }
  if (!udp) {
    const int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    // Отправка не ждёт дольше kSendTimeoutMs: сервер перестал читать — ошибка, а не задача
    // приложения, стоящая навсегда (у NEXT.ELF за ней — F12 и возврат; разбор Codex, № 3)
    const timeval send = {static_cast<long>(kSendTimeoutMs / 1000),
                          static_cast<long>((kSendTimeoutMs % 1000) * 1000)};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send, sizeof send);
  }
  return slot;
}

int ModuleApp::coreSockSend(int sock, const void* data, uint32_t count) {
  ModuleApp* self = instance_;
  if (sock < 0 || sock >= static_cast<int>(kMaxSockets) || self->sockets_[sock] < 0 || data == nullptr) {
    return ZM_ERROR;
  }
  const uint8_t* p = static_cast<const uint8_t*>(data);
  esp_tls_t* tls = self->tls_[sock];
  const uint32_t start = millis();
  while (count > 0) {
    if (tls != nullptr) {
      // TLS: сокет без блокировки — «подожди» (WANT_*) значит, что буфер полон: ждать места
      // не дольше kSendTimeoutMs, как обычный сокет
      const ssize_t sent = esp_tls_conn_write(tls, p, count);
      if (sent > 0) {
        p += sent;
        count -= static_cast<uint32_t>(sent);
        continue;
      }
      if ((sent != ESP_TLS_ERR_SSL_WANT_WRITE && sent != ESP_TLS_ERR_SSL_WANT_READ) ||
          millis() - start > kSendTimeoutMs) {
        return ZM_ERROR;
      }
      fd_set w;
      FD_ZERO(&w);
      FD_SET(self->sockets_[sock], &w);
      timeval tv = {0, 50000};
      select(self->sockets_[sock] + 1, nullptr, &w, nullptr, &tv);
      continue;
    }
    const int sent = send(self->sockets_[sock], p, count, 0);
    if (sent <= 0) {
      return ZM_ERROR;
    }
    p += sent;
    count -= static_cast<uint32_t>(sent);
  }
  return ZM_OK;
}

int ModuleApp::coreSockRecv(int sock, void* data, uint32_t count, uint32_t timeoutMs) {
  ModuleApp* self = instance_;
  if (sock < 0 || sock >= static_cast<int>(kMaxSockets) || self->sockets_[sock] < 0 || data == nullptr) {
    return -1;
  }
  const int fd = self->sockets_[sock];
  esp_tls_t* tls = self->tls_[sock];
  // У TLS расшифрованные байты могут уже лежать в буфере mbedTLS — тогда ждать сокет не нужно
  if (tls == nullptr || esp_tls_get_bytes_avail(tls) <= 0) {
    fd_set r;
    FD_ZERO(&r);
    FD_SET(fd, &r);
    timeval tv = {static_cast<long>(timeoutMs / 1000), static_cast<long>((timeoutMs % 1000) * 1000)};
    const int ready = select(fd + 1, &r, nullptr, nullptr, &tv);
    if (ready == 0) {
      return 0;
    }
    if (ready < 0) {
      return -1;
    }
  }
  if (tls != nullptr) {
    // пришла только часть записи TLS — «подожди», это ещё не данные; 0 — сервер закрыл
    const ssize_t got = esp_tls_conn_read(tls, data, count);
    if (got > 0) {
      return static_cast<int>(got);
    }
    return got == ESP_TLS_ERR_SSL_WANT_READ || got == ESP_TLS_ERR_SSL_WANT_WRITE ? 0 : -1;
  }
  const int got = recv(fd, data, count, 0);
  if (got > 0) {
    return got;
  }
  if (got == 0) {
    // пустая датаграмма UDP — не закрытие (у TCP 0 — удалённая сторона закрыла)
    int type = 0;
    socklen_t length = sizeof type;
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) == 0 && type == SOCK_DGRAM) {
      return 0;
    }
  }
  return -1;
}

void ModuleApp::coreSockClose(int sock) {
  ModuleApp* self = instance_;
  if (sock < 0 || sock >= static_cast<int>(kMaxSockets)) {
    return;
  }
  int fd = -1;
  esp_tls_t* tls = nullptr;
  portENTER_CRITICAL(&self->sockLock_);
  fd = self->sockets_[sock];
  tls = self->tls_[sock];
  self->sockets_[sock] = -1;
  self->tls_[sock] = nullptr;
  portEXIT_CRITICAL(&self->sockLock_);
  if (tls != nullptr) {
    esp_tls_conn_destroy(tls);               // закрывает и сам сокет
  } else if (fd >= 0) {
    close(fd);
  }
}

// ABI 1.13 (браузер TSafari, 07.11.2026: кэш картинок на карте Evo читался кадрами по 1 КБ с
// ответом на каждый — ~30 КБ/с; порциями DMA по 8 КБ, как звук плееру). Разрешить можно только
// свои команды приложения; при выгрузке (stopApp) разрешение снимается.
int ModuleApp::coreLinkMedia(uint8_t first, uint8_t last) {
  ModuleApp* self = instance_;
  if (first > last) {
    setAppMediaRange(1, 0);
    return ZM_OK;
  }
  const zm_app_t* app = self != nullptr ? self->app_ : nullptr;
  if (app == nullptr || first < app->command_first || last > app->command_last) {
    return ZM_ERROR;
  }
  setAppMediaRange(first, last);
  bringupLog(kTag, "app %s: raw media DMA for commands %02X..%02X", self->appName_, first, last);
  return ZM_OK;
}

// ABI 1.12 (браузер TSafari, 04.11.2026: формы POST по HTTPS — net_open умеет только GET).
// Соединение и договор TLS — esp_tls; сертификат сервера — по набору корневых сертификатов
// ESP-IDF, тому же, что у net_open (WiFiClientSecure::useBuiltinCACertBundle). Договор идёт в
// задаче приложения, стек — её. После договора сокет без блокировки: sock_recv ждёт его select,
// а неполная запись TLS — «пока ничего» (0), а не ожидание внутри mbedTLS.
int ModuleApp::coreSockOpenTls(const char* host, uint16_t port, uint32_t timeoutMs) {
  ModuleApp* self = instance_;
  if (host == nullptr || host[0] == 0) {
    return ZM_ERROR;
  }
  esp_tls_t* tls = esp_tls_init();
  if (tls == nullptr) {
    return ZM_NO_MEMORY;
  }
  esp_tls_cfg_t cfg = {};
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = static_cast<int>(timeoutMs);
  if (esp_tls_conn_new_sync(host, static_cast<int>(strlen(host)), port, &cfg, tls) != 1) {
    bringupLog(kTag, "sock tls: %.40s:%u: no connection or bad certificate", host, static_cast<unsigned>(port));
    esp_tls_conn_destroy(tls);
    return ZM_ERROR;
  }
  int fd = -1;
  if (esp_tls_get_conn_sockfd(tls, &fd) != ESP_OK || fd < 0) {
    esp_tls_conn_destroy(tls);
    return ZM_ERROR;
  }
  int slot = -1;
  portENTER_CRITICAL(&self->sockLock_);
  for (size_t i = 0; i < kMaxSockets; ++i) {
    if (self->sockets_[i] < 0) {
      self->sockets_[i] = fd;
      self->tls_[i] = tls;
      slot = static_cast<int>(i);
      break;
    }
  }
  portEXIT_CRITICAL(&self->sockLock_);
  if (slot < 0) {
    esp_tls_conn_destroy(tls);
    return ZM_ERROR;
  }
  const int on = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  return slot;
}

int ModuleApp::coreNetResolve(const char* host, char ip[16]) {
  if (host == nullptr || ip == nullptr) {
    return ZM_ERROR;
  }
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  addrinfo* found = nullptr;
  if (getaddrinfo(host, nullptr, &hints, &found) != 0 || found == nullptr) {
    return ZM_NOT_FOUND;
  }
  const sockaddr_in* in = reinterpret_cast<const sockaddr_in*>(found->ai_addr);
  inet_ntoa_r(in->sin_addr, ip, 16);
  freeaddrinfo(found);
  return ZM_OK;
}

int ModuleApp::coreSdList(const char* path, const char* suffix, uint32_t skip,
                           zm_sd_entry_t* entries, uint32_t capacity, uint32_t* count,
                           int* more, const volatile int* active) {
#if ZIFI_HAS_SPI_LINK
  if (count) *count = 0;
  if (more) *more = 0;
  tang_sd::Session session(tang_sd::kQuickMs);
  if (!session.ok()) return ZM_BUSY;
  if (tang_sd::mount()) return ZM_ERROR;
  return tang_files::list(path, suffix, skip, entries, capacity, count, more, active,
                          coreMillis, coreDelay);
#else
  return ZM_ERROR;
#endif
}

int ModuleApp::coreSdRead(const char* path, uint8_t** data, size_t* size, size_t maxSize,
                           const volatile int* active) {
#if ZIFI_HAS_SPI_LINK
  if (data) *data = nullptr;
  if (size) *size = 0;
  tang_sd::Session session(tang_sd::kQuickMs);
  if (!session.ok()) return ZM_BUSY;
  if (tang_sd::mount()) return ZM_ERROR;
  return tang_files::read(path, data, size, maxSize, active, coreAlloc, coreFree,
                          coreMillis, coreDelay);
#else
  return ZM_ERROR;
#endif
}

int ModuleApp::coreSdWrite(const char* path, const uint8_t* data, size_t size,
                            const volatile int* active) {
#if ZIFI_HAS_SPI_LINK
  tang_sd::Session session(tang_sd::kQuickMs);
  if (!session.ok()) return ZM_BUSY;
  if (tang_sd::mount()) return ZM_ERROR;
  return tang_files::write(path, data, size, active, coreMillis, coreDelay);
#else
  return ZM_ERROR;
#endif
}

int ModuleApp::coreSdOpen(const char* path, int writable, uint32_t* sectors, void** file) {
#if ZIFI_HAS_SPI_LINK
  if (file == nullptr || sectors == nullptr) return ZM_ERROR;
  *file = nullptr;
  *sectors = 0;
  char checked[528];
  if (!tang_files::pathOf(path, checked)) return ZM_ERROR;
  size_t slot = 0;
  while (slot < kMaxDiskFiles && g_diskFiles[slot] != nullptr) ++slot;
  if (slot == kMaxDiskFiles) return ZM_ERROR;
  tang_sd::Session session(tang_sd::kQuickMs);
  if (!session.ok()) return ZM_BUSY;
  if (tang_sd::mount()) return ZM_ERROR;
  tang_sd::DiskFile* opened = nullptr;
  const int result = tang_sd::diskOpen(path, writable != 0, &opened, sectors);
  if (result != 0) return result == 1 ? ZM_NOT_FOUND : ZM_ERROR;
  g_diskFiles[slot] = opened;
  *file = opened;
  return ZM_OK;
#else
  return ZM_ERROR;
#endif
}

int ModuleApp::coreSdPread(void* file, uint32_t sector, uint8_t* data, uint32_t count) {
#if ZIFI_HAS_SPI_LINK
  tang_sd::Session session(tang_sd::kQuickMs);
  if (!session.ok()) return ZM_BUSY;
  return tang_sd::diskRead(static_cast<tang_sd::DiskFile*>(file), sector, data, count) ? ZM_OK : ZM_ERROR;
#else
  return ZM_ERROR;
#endif
}

int ModuleApp::coreSdPwrite(void* file, uint32_t sector, const uint8_t* data, uint32_t count) {
#if ZIFI_HAS_SPI_LINK
  tang_sd::Session session(tang_sd::kQuickMs);
  if (!session.ok()) return ZM_BUSY;
  return tang_sd::diskWrite(static_cast<tang_sd::DiskFile*>(file), sector, data, count) ? ZM_OK : ZM_ERROR;
#else
  return ZM_ERROR;
#endif
}

void ModuleApp::coreSdClose(void* file) {
#if ZIFI_HAS_SPI_LINK
  if (file == nullptr) return;
  for (size_t i = 0; i < kMaxDiskFiles; ++i) {
    if (g_diskFiles[i] == file) {
      g_diskFiles[i] = nullptr;
      tang_sd::Session session;
      tang_sd::diskClose(static_cast<tang_sd::DiskFile*>(file));
      return;
    }
  }
#endif
}

int ModuleApp::coreSdCreate(const char* path, uint32_t sectors) {
#if ZIFI_HAS_SPI_LINK
  char checked[528];
  if (!tang_files::pathOf(path, checked)) return ZM_ERROR;
  tang_sd::Session session(tang_sd::kQuickMs);
  if (!session.ok()) return ZM_BUSY;
  if (tang_sd::mount()) return ZM_ERROR;
  const int result = tang_sd::diskCreate(path, sectors);
  return result == 0 ? ZM_OK : result == 3 ? ZM_NO_MEMORY : ZM_ERROR;
#else
  return ZM_ERROR;
#endif
}

int ModuleApp::coreSd2Open(uint32_t cluster, uint32_t size, void** file, int* raw) {
  if (file == nullptr) return ZM_ERROR;
  *file = nullptr;
  if (raw != nullptr) *raw = 0;
  size_t slot = 0;
  while (slot < kMaxDiskFiles && g_sd2Files[slot] != nullptr) ++slot;
  if (slot == kMaxDiskFiles) return ZM_ERROR;
  sd2_files::File* opened = nullptr;
  const int result = sd2_files::open(cluster, size, &opened);
  if (result != 0) return result == 1 ? ZM_NOT_FOUND : ZM_ERROR;
  g_sd2Files[slot] = opened;
  *file = opened;
  if (raw != nullptr) *raw = sd2_files::raw(opened) ? 1 : 0;
  return ZM_OK;
}

int ModuleApp::coreSd2Read(void* file, uint32_t offset, uint8_t* data, uint32_t count, uint32_t* got) {
  uint32_t done = 0;
  const bool ok = file != nullptr && data != nullptr &&
                  sd2_files::read(static_cast<sd2_files::File*>(file), offset, data, count, &done);
  if (got != nullptr) *got = done;
  return ok ? ZM_OK : ZM_ERROR;
}

int ModuleApp::coreSd2Stream(void* file, uint32_t offset, uint32_t count, uint32_t* crcErrors) {
  uint32_t crc = 0;
  const int r = file == nullptr ? 1 : sd2_files::stream(static_cast<sd2_files::File*>(file), offset, count, &crc);
  if (crcErrors != nullptr) *crcErrors = crc;
  return r == 0 ? ZM_OK : r == 1 ? ZM_NOT_FOUND : ZM_ERROR;
}

void ModuleApp::coreSd2Close(void* file) {
  if (file == nullptr) return;
  for (size_t i = 0; i < kMaxDiskFiles; ++i) {
    if (g_sd2Files[i] == file) {
      g_sd2Files[i] = nullptr;
      sd2_files::close(static_cast<sd2_files::File*>(file));
      return;
    }
  }
}

int ModuleApp::coreLinkBurst(uint32_t address, const uint32_t* words, uint32_t count) {
#if ZIFI_HAS_SPI_LINK
  return words != nullptr && (address & 3) == 0 && count > 0 && tang::writeBurst(address, words, count)
             ? ZM_OK
             : ZM_ERROR;
#else
  (void)address; (void)words; (void)count;
  return ZM_ERROR;
#endif
}

// ABI 1.22 (0.1.176): link_burst длинными транзакциями DMA (link_spi3.hpp — как и зачем). Буферы DMA
// (2 × 1 КБ внутреннего ОЗУ) — при первом вызове приложения, назад — в stopApp. Запас внутреннего ОЗУ
// после них — тот же, что у code_ram (kCodeRamKeep): Wi-Fi, обмен с Evo и DMA ядра.
int ModuleApp::coreLinkBurstDma(uint32_t address, const uint32_t* words, uint32_t count, uint32_t flags) {
#if ZIFI_HAS_SPI_LINK
  constexpr size_t kLinkDmaKeep = 12 * 1024;
  if (words == nullptr || (address & 3) != 0 || count == 0 || (flags & ~ZM_LINK_NOWAIT) != 0) {
    return ZM_ERROR;
  }
  if (!tang::ready()) {
    return ZM_ERROR;
  }
  if (!tang::prepareBurstDma(kLinkDmaKeep)) {
    return ZM_NO_MEMORY;
  }
  return tang::writeBurstDma(address, words, count, (flags & ZM_LINK_NOWAIT) == 0) ? ZM_OK : ZM_ERROR;
#else
  (void)address; (void)words; (void)count; (void)flags;
  return ZM_ERROR;
#endif
}

int ModuleApp::coreLinkBurstWait() {
#if ZIFI_HAS_SPI_LINK
  return tang::waitBurstDma() ? ZM_OK : ZM_ERROR;
#else
  return ZM_ERROR;
#endif
}

uint32_t ModuleApp::corePcmLowLatency(int on) {
  return instance_->player_.pcmLowLatency(on != 0);
}

void ModuleApp::coreAppDone() {
  if (instance_->inCommand_) instance_->appDone_ = true;
}

int ModuleApp::coreUiAvailable() {
#if ZIFI_HAS_SPI_LINK
  return player_overlay::available() ? 1 : 0;
#else
  return 0;
#endif
}

// Снимок экрана — под appLock_, без ожидания (как poll): выгрузка приложения его ждёт, а
// приложение, которое ещё запускается (start не вернулся), снимок не даёт.
int ModuleApp::uiModel(zm_ui_model_t* model) {
  ModuleApp* self = instance_;
  if (self == nullptr || self->appLock_ == nullptr || xSemaphoreTake(self->appLock_, 0) != pdTRUE) {
    return ZM_BUSY;
  }
  int result = ZM_NOT_FOUND;
  const zm_app_t* app = self->app_;
  if (app != nullptr && self->appStarted_ &&
      app->size >= offsetof(zm_app_t, ui_model) + sizeof(app->ui_model) && app->ui_model != nullptr) {
    result = app->ui_model(model);
  }
  xSemaphoreGive(self->appLock_);
  return result;
}

int ModuleApp::uiCover(uint16_t* pixels, uint16_t side) {
  ModuleApp* self = instance_;
  if (self == nullptr || self->appLock_ == nullptr || xSemaphoreTake(self->appLock_, 0) != pdTRUE) {
    return ZM_BUSY;
  }
  int result = ZM_NOT_FOUND;
  const zm_app_t* app = self->app_;
  if (app != nullptr && self->appStarted_ &&
      app->size >= offsetof(zm_app_t, ui_cover) + sizeof(app->ui_cover) && app->ui_cover != nullptr) {
    result = app->ui_cover(pixels, side);
  }
  xSemaphoreGive(self->appLock_);
  return result;
}

int ModuleApp::coreWifiInfo(char ssid[33], char ip[16], char mac[18]) {
  if (ssid == nullptr || ip == nullptr || mac == nullptr || WiFi.status() != WL_CONNECTED) {
    return ZM_ERROR;
  }
  snprintf(ssid, 33, "%s", WiFi.SSID().c_str());
  snprintf(ip, 16, "%s", WiFi.localIP().toString().c_str());
  snprintf(mac, 18, "%s", WiFi.macAddress().c_str());
  return ZM_OK;
}

// ABI 1.11: уровень сигнала (экран интернет-радио, 04.11.2026).
int ModuleApp::coreWifiRssi(int* dbm) {
  if (dbm == nullptr || WiFi.status() != WL_CONNECTED) {
    return ZM_ERROR;
  }
  *dbm = WiFi.RSSI();
  return *dbm < 0 ? ZM_OK : ZM_ERROR;
}

// Пока поток открыт, Wi-Fi не спит (с 0.1.34). В энергосбережении ESP забирает кадры
// у точки доступа раз в маяк (около 100 мс), и при окне TCP 5760 байтов поток не
// быстрее ~57 КБ/с — впритык станции 320 кбит/с (40 КБ/с); на плате 5 октября поток
// 128 кбит/с временами шёл вдвое медленнее звука.
bool ModuleApp::keepsWifiAwake() const {
  for (size_t i = 0; i < kMaxStreams; ++i) {
    if (streamUsed_[i]) {
      return true;
    }
  }
  return false;
}

// Выгрузка приложения: его потоки — закрыть (задачи его уже сняты).
void ModuleApp::closeStreams() {
  for (size_t i = 0; i < kMaxStreams; ++i) {
    if (streamUsed_[i] && streams_[i] != nullptr) {
      streams_[i]->close();
    }
    streamUsed_[i] = false;
  }
}

int ModuleApp::coreModuleOpen(const char* name, void* arg1, void* arg2, void** handle,
                              int* entryResult) {
  return openModule(name, false, arg1, arg2, handle, entryResult, nullptr);
}

int ModuleApp::coreModuleOpenEx(const char* name, uint32_t flags, void* arg1, void* arg2,
                                void** handle, int* entryResult, uint32_t* placed) {
  return openModule(name, (flags & ZM_MODULE_TEXT_INTERNAL) != 0, arg1, arg2, handle, entryResult,
                    placed);
}

int ModuleApp::openModule(const char* name, bool internal, void* arg1, void* arg2, void** handle,
                          int* entryResult, uint32_t* placed) {
  if (placed != nullptr) {
    *placed = 0;
  }
  if (name == nullptr || handle == nullptr) {
    return ZM_ERROR;
  }
  *handle = nullptr;
  for (LoadedModule& module : instance_->moduleSlots()) {
    if (module.used) {
      continue;
    }
    int result = 0;
    void* elf = nullptr;
    bool inCodeRam = false;
    const int loaded = instance_->loadElf(name, arg1, arg2, &elf, &result, internal, &inCodeRam);
    if (loaded != ZM_OK) {
      return loaded;
    }
    module.used = true;
    strncpy(module.name, name, sizeof(module.name) - 1);
    module.inCodeRam = inCodeRam;
    module.elf = elf;
    *handle = &module;
    if (entryResult != nullptr) {
      *entryResult = result;
    }
    if (placed != nullptr && inCodeRam) {
      *placed = ZM_MODULE_TEXT_INTERNAL;
    }
    return ZM_OK;
  }
  bringupLog(kTag, "module %s: all %u module slots are open", name, static_cast<unsigned>(kMaxModules));
  return ZM_BUSY;
}

void ModuleApp::coreModuleClose(void* handle) {
  LoadedModule* module = static_cast<LoadedModule*>(handle);
  if (module == nullptr || !module->used) {
    return;
  }
  const bool inCodeRam = module->inCodeRam;
  instance_->unloadElf(module->elf);
  *module = LoadedModule{};
  // Последний модуль из куска code_ram закрыт: место — снова с начала.
  if (inCodeRam && codeRam_.modules != 0 && --codeRam_.modules == 0) {
    codeRam_.used = 0;
  }
}

// --- ABI 1.19: исполняемое внутреннее ОЗУ для кода модулей ----------------------------------------
//
// Защита памяти ESP32-S3 (PMS) делит внутреннее ОЗУ (SRAM1) чертой: ниже — код прошивки (шина
// команд: чтение и исполнение; шина данных: только чтение), выше — данные (шина данных: чтение и
// запись; шина команд — ничего, область IRAM0_3). Замок защиты снят (sdkconfig, 0.1.157), и на время
// работы приложения, попросившего code_ram, прошивка даёт области IRAM0_3 чтение и исполнение: код
// модуля, записанный загрузчиком через шину данных, исполняется через шину команд по тому же
// адресу + (SOC_DIRAM_IRAM_LOW − SOC_DIRAM_DRAM_LOW). Запись в код прошивки (DRAM0_0) запрещена
// всегда. При выгрузке приложения — снова «ничего».

namespace {

// После куска во внутреннем ОЗУ должно остаться столько: Wi-Fi, обмен с Evo, DMA (19.11 плеер жил при
// ~9 КБ свободных, но стек 6 КБ туда уже не помещался).
constexpr uint32_t kCodeRamKeep = 12 * 1024;

// Резерв под код (0.1.158). Куча ESP-IDF отдаёт внутреннюю память сначала из SRAM1 (у неё высший
// приоритет), потом из пула для DMA и SRAM2. К старту приложения SRAM1 раздроблена: 21.11 Metal Slug
// не получил 13568 байт при 37527 свободных — самый большой кусок (21504) лежал в SRAM2 (0x3FCF0000+,
// там только данные, зеркала на шине команд нет). Поэтому при старте, пока SRAM1 цела, прошивка
// берёт из неё kCodeRamReserve и делает из него отдельную кучу с последним приоритетом: обычные
// выделения идут туда, только когда остальное внутреннее ОЗУ занято, — память остаётся общей и
// в свободной считается. code_ram берёт кусок оттуда первым — по метке kCodeRamReserveCap.
// Цена для всех — служебная таблица этой кучи (TLSF ~0,7 КБ), она лежит в самом резерве.
// 17 КБ: свободный блок кучи — 17408 − 20 (multi_heap) − 716 (TLSF) − 8 = 16664 ≥ ZM_CODE_RAM_RESERVED.
constexpr uint32_t kCodeRamReserve = 17 * 1024;
// Метка резерва: PID-метки ESP-IDF не использует, у других куч их нет.
constexpr uint32_t kCodeRamReserveCap = MALLOC_CAP_PID7;
// Свойства обычной SRAM1 при включённой защите памяти (MALLOC_DIRAM_BASE_CAPS | SIMD) плюс метка.
constexpr uint32_t kCodeRamReserveCaps = MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT |
                                         MALLOC_CAP_32BIT | MALLOC_CAP_DMA | MALLOC_CAP_RETENTION |
                                         MALLOC_CAP_SIMD | kCodeRamReserveCap;
// Внутреннее ОЗУ с зеркалом на шине команд: MALLOC_CAP_RETENTION есть только у обычной SRAM1 и у
// резерва; у пула для DMA (CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL), SRAM2 и RTC её нет.
constexpr uint32_t kCodeRamSram1Caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_RETENTION;

bool inSram1(const uint8_t* data, uint32_t size) {
  return esp_ptr_in_diram_dram(data) && esp_ptr_in_diram_dram(data + size - 1);
}

// Исполнение в области данных внутреннего ОЗУ: true — права стали такими, как просили.
bool setDataExec(bool on) {
#if CONFIG_ESP_SYSTEM_MEMPROT
  const uint32_t flags = on ? (MEMPROT_OP_READ | MEMPROT_OP_EXEC) : MEMPROT_OP_NONE;
  if (esp_mprot_set_pms_area(MEMPROT_PMS_AREA_IRAM0_3, flags, DEFAULT_CPU_NUM) != ESP_OK) {
    return false;
  }
  uint32_t now = 0;
  if (esp_mprot_get_pms_area(MEMPROT_PMS_AREA_IRAM0_3, &now, DEFAULT_CPU_NUM) != ESP_OK) {
    return false;
  }
  return ((now & MEMPROT_OP_EXEC) != 0) == on;   // запертая защита запись молча не примет
#else
  (void)on;
  return true;                                   // защиты нет: исполнять можно и так
#endif
}

}  // namespace

int ModuleApp::coreCodeRam(uint32_t bytes, uint32_t* granted) {
  if (granted != nullptr) {
    *granted = 0;
  }
  if (bytes == 0 || bytes > ZM_CODE_RAM_MAX) {
    return ZM_ERROR;
  }
  if (codeRam_.data != nullptr) {
    return ZM_BUSY;
  }
  const uint32_t size = (bytes + 3u) & ~3u;
  const char* app = instance_ != nullptr ? instance_->appName_ : "?";
  const uint32_t freeBefore =
      static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  uint8_t* data = nullptr;
  bool fromReserve = false;
  if (freeBefore >= size + kCodeRamKeep) {
    // Сначала — резерв (его метка только у него), потом — любая SRAM1 (без пула DMA и SRAM2).
    data = static_cast<uint8_t*>(heap_caps_malloc(size, kCodeRamReserveCap));
    fromReserve = data != nullptr;
    if (data == nullptr) {
      data = static_cast<uint8_t*>(heap_caps_malloc(size, kCodeRamSram1Caps));
    }
  }
  // Кусок должен лежать в SRAM1 с зеркалом на шине команд: последние 32 КБ DRAM (0x3FCF0000) у
  // ESP32-S3 — только данные.
  if (data != nullptr && (!esp_ptr_in_diram_dram(data) || !esp_ptr_in_diram_dram(data + size - 1))) {
    heap_caps_free(data);
    data = nullptr;
  }
  if (data == nullptr) {
    bringupLog(kTag, "app %s: code RAM %u bytes refused; reserve %u, SRAM1 %u largest; internal %u free, %u largest",
               app, static_cast<unsigned>(size),
               static_cast<unsigned>(heap_caps_get_largest_free_block(kCodeRamReserveCap)),
               static_cast<unsigned>(heap_caps_get_largest_free_block(kCodeRamSram1Caps)),
               static_cast<unsigned>(freeBefore),
               static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    return ZM_NO_MEMORY;
  }
  if (!setDataExec(true)) {
    setDataExec(false);
    heap_caps_free(data);
    bringupLog(kTag, "app %s: code RAM refused: memory protection is locked", app);
    return ZM_ERROR;
  }
  codeRam_ = CodeRam{data, size, 0, 0, true};
  bringupLog(kTag, "app %s: code RAM %u bytes at %08X (bus %08X), %s; internal %u free, %u largest", app,
             static_cast<unsigned>(size), static_cast<unsigned>(reinterpret_cast<uintptr_t>(data)),
             static_cast<unsigned>(reinterpret_cast<uintptr_t>(data) + (SOC_DIRAM_IRAM_LOW - SOC_DIRAM_DRAM_LOW)),
             fromReserve ? "reserve" : "SRAM1",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
  if (granted != nullptr) {
    *granted = size;
  }
  return ZM_OK;
}

void ModuleApp::reserveCodeRam() {
  if (codeReserve_ != nullptr) {
    return;
  }
  uint8_t* block = static_cast<uint8_t*>(heap_caps_malloc(kCodeRamReserve, kCodeRamSram1Caps));
  if (block != nullptr && !inSram1(block, kCodeRamReserve)) {
    heap_caps_free(block);
    block = nullptr;
  }
  if (block == nullptr) {
    return;                                      // logCodeRamReserve напишет об этом в журнал
  }
  // Свойства по столбцам приоритета (SOC_MEMORY_TYPE_NO_PRIOS = 3): только в последнем — туда
  // обычные выделения идут, когда остальная внутренняя память занята. Куча внутри занятого блока
  // SRAM1 — так же ESP-IDF делает пул для DMA (esp_psram_extram_reserve_dma_pool).
  const uint32_t caps[3] = {0, 0, kCodeRamReserveCaps};
  const intptr_t start = reinterpret_cast<intptr_t>(block);
  if (heap_caps_add_region_with_caps(caps, start, start + kCodeRamReserve) != ESP_OK) {
    heap_caps_free(block);
    return;
  }
  codeReserve_ = block;
}

void ModuleApp::logCodeRamReserve() {
  if (codeReserve_ == nullptr) {
    bringupLog(kTag, "code RAM reserve NOT made: no %u bytes in SRAM1 at start",
               static_cast<unsigned>(kCodeRamReserve));
    return;
  }
  bringupLog(kTag, "code RAM reserve %u bytes at %08X: %u for code_ram", static_cast<unsigned>(kCodeRamReserve),
             static_cast<unsigned>(reinterpret_cast<uintptr_t>(codeReserve_)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(kCodeRamReserveCap)));
}

void ModuleApp::releaseCodeRam() {
  if (codeRam_.data == nullptr) {
    return;
  }
  // Код модулей снят раньше (stopApp выгружает их до этого): исполнять там уже некому.
  if (codeRam_.execOpen && !setDataExec(false)) {
    bringupLog(kTag, "code RAM: memory protection was NOT closed again");
  }
  heap_caps_free(codeRam_.data);
  codeRam_ = CodeRam{};
}

int ModuleApp::coreResource(const char* name, const uint8_t** data, size_t* size) {
  for (const Resource& item : instance_->resources_) {
    if (item.data != nullptr && name != nullptr && strcmp(item.name, name) == 0) {
      if (data != nullptr) {
        *data = item.data;
      }
      if (size != nullptr) {
        *size = item.size;
      }
      return ZM_OK;
    }
  }
  return ZM_NOT_FOUND;
}

// name == nullptr — все ресурсы (03.11.2026): экран ядер COREMENU освобождает PSRAM на старте, а имена наборов
// MWM плеера ему неизвестны. Прошивки старше 0.1.112 на nullptr ничего не делают.
void ModuleApp::coreResourceRelease(const char* name) {
  for (Resource& item : instance_->resources_) {
    if (item.data != nullptr && (name == nullptr || strcmp(item.name, name) == 0)) {
      heap_caps_free(item.data);
      item = Resource{};
    }
  }
}

}  // namespace zifi

#endif  // ZIFI_HAS_AUDIO
