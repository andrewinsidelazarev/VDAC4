#pragma once

#include "zifi/app.hpp"
#include "zifi/board.hpp"

#if ZIFI_HAS_AUDIO

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "zifi/zm_cache.hpp"
#include "zm_app.h"

namespace zifi {

class AudioPlayer;
class HttpStream;

// Узел модулей ESP: приложения и декодеры — файлы ELF из /WC/ESP/ на карте Evo
// (modules/sdk/include/zm_app.h). Плагин доставляет файл (MOD_QUERY / PUT /
// COMMIT), ядро держит копию в кэше flash (ZmCache) и по APP_START грузит
// приложение в PSRAM. Активно одно приложение; команды Z80 из его диапазона
// ядро отдаёт ему, а без приложения они идут по-старому (AudioApp).
//
// Ресурсы (банк MIDI, ПЗУ OPL4) живут в PSRAM до перезапуска ESP. Если в разъёме
// USB модуля флешка FAT32 или exFAT, ядро держит их копии и там
// (/SYSTEM/CACHE/MUSIC, ResourceDiskCache): ресурс, которого нет в PSRAM, при
// годной копии (та же длина, время не старее файла на карте Evo) грузится с
// флешки, и плагин его не передаёт; принятый с карты ресурс ядро сразу пишет
// на флешку. Флешки нет или она не годится — всё как без неё.
//
// Стоит первым в списке приложений ядра.
class ModuleApp : public App {
 public:
  ModuleApp(AppHost& host, AudioPlayer& player);

  const char* name() const override { return "modules"; }
  void begin() override;
  bool handleCommand(const PacketView& packet) override;
  void pollNetworkCore(uint32_t now) override;
  // Приложение качает поток (net_open, интернет-радио): Wi-Fi без энергосбережения.
  bool keepsWifiAwake() const override;
  // Кристалл ESP, десятые доли °C (раз в секунду); INT16_MIN — датчика нет.
  int16_t temperatureTenths() const;

 private:
  static constexpr size_t kMaxTasks = 8;
  static constexpr size_t kMaxSync = 16;
  static constexpr size_t kMaxModules = 4;
  static constexpr size_t kMaxResources = 4;
  static constexpr size_t kMaxStreams = 2;

  struct TaskSlot {
    TaskHandle_t handle;
    zm_task_fn fn;
    void* arg;
    volatile bool running;
  };
  struct LoadedModule {
    bool used;
    char name[13];
    void* elf;               // esp_elf_t* в PSRAM
  };
  struct Resource {
    char name[13];
    uint8_t* data;
    size_t size;
    uint32_t stamp;                    // время FAT файла: обновлённый банк того же размера — другой
  };

  // Таблица ядра для приложения: статические функции идут к единственному узлу.
  static ModuleApp* instance_;
  static void* coreAlloc(size_t size, uint32_t caps);
  static void* coreRealloc(void* pointer, size_t size);
  static void coreFree(void* pointer);
  static void coreLog(const char* format, ...);
  static uint32_t coreMillis();
  static void coreDelay(uint32_t ms);
  static int coreTaskStart(zm_task_fn fn, void* arg, uint32_t stack, uint8_t priority,
                           int8_t cpu);
  static void taskTrampoline(void* slot);
  static void* coreMutexCreate();
  static void coreMutexLock(void* mutex);
  static void coreMutexUnlock(void* mutex);
  static void* coreSignalCreate();
  static void coreSignalGive(void* signal);
  static int coreSignalWait(void* signal, uint32_t timeoutMs);
  static int coreReply(uint8_t response, const void* data, uint16_t length);
  static int coreEvent(uint8_t code, const void* data, uint16_t length, uint32_t waitMs);
  static int corePcmOpen(const zm_pcm_format_t* format);
  static int corePcmWrite(const float* frames, uint32_t count);
  static uint32_t corePcmQueuedMs();
  static void corePcmFlush();
  static void corePcmClose();
  static void corePcmPause(int paused);
  static void coreSetVolume(uint8_t volume);
  static uint8_t coreVolume();
  static uint16_t corePcmFaults();
  static int coreModuleOpen(const char* name, void* arg1, void* arg2, void** handle,
                            int* entryResult);
  static void coreModuleClose(void* handle);
  static int coreResource(const char* name, const uint8_t** data, size_t* size);
  static void coreResourceRelease(const char* name);
  static int16_t coreTemperature();
  static void* coreNetOpen(const char* url, const char* extra, uint32_t timeoutMs, int* status);
  static const char* coreNetHeader(void* stream, const char* name);
  static int coreNetRead(void* stream, void* data, uint32_t count, uint32_t timeoutMs);
  static void coreNetClose(void* stream);

  void replyQuery(uint8_t state);
  void handleQuery(const PacketView& packet);
  uint8_t* allocateDelivery(const char* name, uint32_t size, bool resource);
  void handlePut(const PacketView& packet);
  void handleCommit();
  // Ресурс — в свободное место таблицы; false — мест нет.
  bool adoptResource(const char* name, uint8_t* data, size_t size, uint32_t stamp);
  // Ресурс delivery_ с флешки, если там годная копия: true — он уже в таблице.
  bool loadResourceFromDrive(const char* name, uint32_t size, uint32_t stamp);
  // Только что принятый ресурс — на флешку (если она есть и ресурс кэшируется).
  void copyResourceToDrive(const Resource& resource);
  void handleStart(const PacketView& packet);
  int loadElf(const char* name, void* arg1, void* arg2, void** elfOut, int* entryResult);
  void unloadElf(void* elf);
  int startApp(const char* name);
  void stopApp();
  void freeAllocations();
  void closeStreams();
  void dropDelivery();
  static void temperatureTask(void* self);

  AppHost& host_;
  AudioPlayer& player_;
  class LittleFsStorage* storage_ = nullptr;
  ZmCache* cache_ = nullptr;
  zm_core_t core_ = {};

  // Доставка файла от плагина.
  char deliveryName_[13] = {};
  uint8_t* delivery_ = nullptr;
  uint32_t deliverySize_ = 0;
  uint32_t deliveryStamp_ = 0;
  uint32_t deliveryReceived_ = 0;
  bool deliveryResource_ = false;

  // Активное приложение.
  void* appElf_ = nullptr;
  const zm_app_t* app_ = nullptr;
  char appName_[13] = {};
  bool appStale_ = false;                 // с запуска в кэш лёг новый модуль: перезапустить
  SemaphoreHandle_t appLock_ = nullptr;   // выгрузка против poll на сетевом ядре
  bool inCommand_ = false;

  // Что приложение завело: освобождается при выгрузке.
  portMUX_TYPE allocLock_ = portMUX_INITIALIZER_UNLOCKED;
  void* allocations_ = nullptr;           // голова списка заголовков
  TaskSlot tasks_[kMaxTasks] = {};
  SemaphoreHandle_t sync_[kMaxSync] = {};
  LoadedModule modules_[kMaxModules] = {};
  Resource resources_[kMaxResources] = {};
  // Потоки HTTP(S) приложения (net_*): объекты живут до перезапуска ESP, занятость —
  // до net_close или выгрузки приложения.
  HttpStream* streams_[kMaxStreams] = {};
  volatile bool streamUsed_[kMaxStreams] = {};
  portMUX_TYPE streamLock_ = portMUX_INITIALIZER_UNLOCKED;
};

}  // namespace zifi

#endif  // ZIFI_HAS_AUDIO
