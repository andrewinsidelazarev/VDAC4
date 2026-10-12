#include <Arduino.h>
#include <WiFi.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
#include <esp_idf_version.h>
#include <esp_psram.h>
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "zifi/app.hpp"
#include "zifi/apps/audio_app.hpp"
#include "zifi/apps/info_apps.hpp"
#include "zifi/apps/module_app.hpp"
#include "zifi/apps/wc_update_app.hpp"
#include "zifi/board.hpp"
#include "zifi/config.hpp"
#include "zifi/config_store.hpp"
#include "zifi/bringup_log.hpp"
#include "zifi/diagnostic_log.hpp"
#include "zifi/elf_probe.hpp"
#include "zifi/ftp_server.hpp"
#include "zifi/link_watchdog.hpp"
#include "zifi/net_client.hpp"
#include "zifi/crash_record.hpp"
#include "zifi/net_console.hpp"
#include "zifi/online_updater.hpp"
#include "zifi/ota_server.hpp"
#include "zifi/protocol.hpp"
#include "zifi/smb_server.hpp"
#include "zifi/z80_transport.hpp"
#include "zifi/vfs_bridge.hpp"
#if ZIFI_HAS_SPI_LINK
#include "zifi/spi_link.hpp"
#endif
#if ZIFI_HAS_STATUS_LED
#include "zifi/status_led.hpp"
#endif

// Прошивка — только для платы VDAC4: канал до Z80 — SPI платы Evo (SpiLink).
#if !ZIFI_HAS_SPI_LINK
#error "Прошивка собирается только для платы VDAC4 (ZIFI_BOARD_VDAC4=1)"
#endif

// Arduino по умолчанию подтверждает новую OTA-прошивку ещё до setup(). Для
// настоящего rollback откладываем подтверждение до запуска канала SPI, VFS и второго
// ядра. Сигнатура переопределяет weak-функцию Arduino core.
extern "C" bool verifyRollbackLater() {
  return true;
}

namespace zifi {
namespace {

constexpr uint32_t kWifiTimeoutMs = 10000;
// Столько подключение к той же сети может идти само (Arduino повторяет его,
// setAutoReconnect); дольше — начать заново: значит, повторять его некому
// (отключение по причине, которую Arduino не повторяет).
constexpr uint32_t kWifiAttemptMs = 30000;
// Обычному TCP хватало 6 КиБ, но цепочка processHttpGet -> WiFiClientSecure ->
// mbedTLS использует существенно больше стека во время проверки сертификата и
// рукопожатия. Переполнение этой задачи перезапускает ESP, а Z80 продолжает
// ждать уже потерянный ответ NET_HTTP_GET. Оставляем измеримый запас.
constexpr uint32_t kNetworkStackBytes = 16384;
constexpr UBaseType_t kNetworkPriority = 2;
constexpr BaseType_t kNetworkCore = 0;
constexpr uint32_t kWifiSignalIntervalMs = 2000;
// Сколько файловый сервер может молчать, прежде чем радио снова начнёт спать.
constexpr uint32_t kWifiAwakeIdleMs = 10UL * 60UL * 1000UL;
// Кристалл ESP горячее kWifiHotTenths — Wi-Fi в энергосбережении при любой работе
// (радио без него — заметная часть нагрева); снова как обычно — ниже
// kWifiCoolTenths: запас, чтобы не дёргать режим на границе. Десятые доли °C.
constexpr int16_t kWifiHotTenths = 750;
constexpr int16_t kWifiCoolTenths = 700;
constexpr size_t kWifiSignalBarWidth = 16;

uint8_t wifiSignalPercent(int32_t rssi) {
  if (rssi <= -90) {
    return 0;
  }
  if (rssi >= -50) {
    return 100;
  }
  return static_cast<uint8_t>((rssi + 90) * 100 / 40);
}

size_t formatWifiSignal(char* output, size_t capacity, bool connected,
                        int32_t rssi) {
  if (output == nullptr || capacity < 30) {
    return 0;
  }
  const uint8_t percent = connected ? wifiSignalPercent(rssi) : 0;
  const size_t filled =
      (static_cast<size_t>(percent) * kWifiSignalBarWidth + 50) / 100;
  size_t length = static_cast<size_t>(snprintf(output, capacity, "Wi-Fi ["));
  for (size_t index = 0; index < kWifiSignalBarWidth; ++index) {
    output[length++] = index < filled ? '#' : '.';
  }
  const int suffix = snprintf(output + length, capacity - length,
                              "] %3u%%", static_cast<unsigned>(percent));
  if (suffix < 0 || static_cast<size_t>(suffix) >= capacity - length) {
    output[0] = 0;
    return 0;
  }
  return length + static_cast<size_t>(suffix);
}

// Есть ли PSRAM на самом деле. В Arduino-ядре 3.x psramFound() отвечает «да»
// всегда, если память включена в настройках: её запускает ESP-IDF до ядра, и
// отказ (при CONFIG_SPIRAM_IGNORE_NOTFOUND) до ядра не доходит.
bool psramAvailable() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  return esp_psram_is_initialized();
#else
  return psramFound();
#endif
}

bool runningFirmwareIsPending() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  return running != nullptr &&
         esp_ota_get_state_partition(running, &state) == ESP_OK &&
         state == ESP_OTA_IMG_PENDING_VERIFY;
}

bool confirmRunningFirmware() {
  return !runningFirmwareIsPending() ||
         esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
}

void rejectRunningFirmware() {
  if (runningFirmwareIsPending()) {
    // При наличии предыдущего исправного слота функция сама перезагрузит ESP.
    // Для первой factory-установки откат может быть невозможен — тогда она
    // просто вернёт ошибку, а канал SPI останется доступен для диагностики.
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

void copyIp(uint8_t* output, const IPAddress& address) {
  for (uint8_t index = 0; index < 4; ++index) {
    output[index] = address[index];
  }
}

bool parseReleaseVersion(const char* text, unsigned& major, unsigned& minor,
                         unsigned& patch) {
  if (text == nullptr) {
    return false;
  }
  int consumed = 0;
  return sscanf(text, board::kReleaseVersionFormat, &major, &minor, &patch,
                &consumed) == 3 && text[consumed] == 0;
}

uint8_t comparePublishedVersion(const char* installed,
                                const char* published) {
  if (installed != nullptr && published != nullptr &&
      strcmp(installed, published) == 0) {
    return kOnlineUpdateSame;
  }
  unsigned installedMajor = 0;
  unsigned installedMinor = 0;
  unsigned installedPatch = 0;
  unsigned publishedMajor = 0;
  unsigned publishedMinor = 0;
  unsigned publishedPatch = 0;
  if (!parseReleaseVersion(installed, installedMajor, installedMinor,
                           installedPatch) ||
      !parseReleaseVersion(published, publishedMajor, publishedMinor,
                           publishedPatch)) {
    return kOnlineUpdateDifferent;
  }
  if (publishedMajor != installedMajor) {
    return publishedMajor > installedMajor ? kOnlineUpdateNewer
                                            : kOnlineUpdateOlder;
  }
  if (publishedMinor != installedMinor) {
    return publishedMinor > installedMinor ? kOnlineUpdateNewer
                                            : kOnlineUpdateOlder;
  }
  if (publishedPatch != installedPatch) {
    return publishedPatch > installedPatch ? kOnlineUpdateNewer
                                            : kOnlineUpdateOlder;
  }
  return kOnlineUpdateDifferent;
}

// Тела команд и ответов сетевого ядра — в IntercoreExchange (app.hpp).
// Потоковые данные VFS передаются отдельно через два SPSC-кольца VfsBridge,
// поэтому управляющий слот не блокирует поток.

// Сетевое ядро не пишет в канал Z80 напрямую. Короткие индикаторные события серверов
// копируются в эту очередь и отправляются владельцем канала на ядре 1.
struct IntercoreEvent {
  uint8_t command;
  uint16_t length;
  uint8_t data[kMaxEventPayload];
};

static_assert(sizeof(IntercoreExchange) < 3 * 1024,
              "inter-core exchange unexpectedly grew");

// Ядро прошивки. Прикладные команды раздаются приложениям (apps_).
class Application : private AppHost {
 public:
  Application();

  void begin();
  void pollLink();

 private:
  // AppHost: что ядро даёт приложениям.
  Z80Transport& transport() override { return transport_; }
  const IniConfig& config() const override { return config_; }
  NetClient& netClient() override { return netClient_; }
  bool otaRunning() const override { return ota_.running(); }
  bool stopFileServers() override;
  bool sendEvent(uint8_t command, const uint8_t* data, uint16_t length,
                 uint32_t waitMs) override;
  void noteFileActivity() override { fileActivityMs_ = millis(); }
  bool psram() const override { return psramAvailable(); }

  bool dispatchToApps(const PacketView& packet);
  App* networkApp(uint8_t command) const;
  const char* stopAppsForFileServer();
  bool appsKeepWifiAwake() const;
  bool appsBlockFlashWrites() const;

  static void networkTaskEntry(void* context);
  static bool networkEventEntry(void* context, uint8_t command,
                                 const uint8_t* data, uint16_t length);
  static bool onlineUpdateProgressEntry(void* context, uint8_t stage,
                                        uint8_t percent);
  void networkTaskLoop();
  void updateWifiPowerSave(bool fileServerRunning, bool appAwake, uint32_t now);
  void processNetworkRequest();
  void processWifiConnect();
  void processWifiIni();
  void processFtpStart();
  void processFtpStop();
  void processFtpRamStats();
  void processSmbStart();
  void processSmbStop();
  void processUpdateStart();
  void processUpdateStop();
  void processOnlineUpdateCheck();
  void processOnlineUpdate();
  void processNetOpen();
  void processNetSend();
  void processNetReceive();
  void processNetClose();
  void processHttpGet();
  void processNetProbe();
  void processIpConfig();
  void processNetProxyStatus();
  void updateProxyState();

  void handle(const PacketView& packet);
  void handleSysInfo();
  void handleReset();
  // Пояс ESP — из time: файла zifi.ini (localtime, время файлов на флешке).
  void applyTimezone();
  void handleSysClock(const PacketView& packet);
  void pollNetworkResponse();
  void pollNetworkEvent();
  bool submitNetwork(uint8_t command, const uint8_t* data, uint16_t length);
  bool enqueueNetworkEvent(uint8_t command, const uint8_t* data,
                           uint16_t length);
  void sendWifiFailure(uint8_t responseCommand);
  void sendNetworkFailure(uint8_t command);
  void clearError();
  void reportError(const char* format, ...);

  bool connectWifi(const char* ssid, const char* password);
  void prepareWifiResponse(uint8_t responseCommand, bool connected);
  void setNetworkError(const char* format, ...);
  static bool copyString(const uint8_t* data, size_t length, size_t offset,
                         char* output, size_t capacity, size_t& nextOffset,
                         bool requireTerminator);

#if ZIFI_HAS_SPI_LINK
  SpiLink link_;
#endif
  Z80Transport transport_;
  IniConfig config_;
  ConfigStore configStore_;
  VfsBridge vfsBridge_;
  NetClient netClient_;
  OnlineUpdater onlineUpdater_;
  FtpServer ftp_;
  SmbServer smb_;
  OtaServer ota_;
  // Приложения. Порядок в apps_ — порядок, в котором им предлагается команда.
#if ZIFI_HAS_AUDIO
  AudioApp audioApp_;
  // Модули ESP (ELF из /WC/ESP/): первым — команды активного приложения
  // перехватываются раньше прежнего AudioApp.
  ModuleApp moduleApp_;
#endif
  TimeApp timeApp_;
  WeatherApp weatherApp_;
  WcUpdateApp wcuApp_;
  static constexpr size_t kAppCount = 3 + (ZIFI_HAS_AUDIO ? 2 : 0);
  App* apps_[kAppCount];
  IntercoreExchange* exchange_;
  QueueHandle_t requestQueue_;
  QueueHandle_t responseQueue_;
  QueueHandle_t eventQueue_;
  TaskHandle_t networkTask_;
  volatile bool networkReady_;
  bool requestPending_;
  bool exchangeInPsram_;
  bool configStoreReady_;
  bool restartAfterOnlineUpdate_;
  uint8_t lastStep_;
  char lastError_[49];
  char activeSsid_[33];                   // сеть, к которой подключались последней (WiFi.begin)
  char activePassword_[64];
  uint32_t wifiBeganMs_;                  // когда был этот WiFi.begin (0 — не было)
  bool proxyUpdated_;                     // connectWifi сам проверил прокси (новое подключение)
  uint8_t proxyStatus_;
  char activeProxy_[64];
  // Последний обмен с файловым сервером: событие клиента FTP/SMB. Пишут SMB-
  // задача и сетевая задача, читает сетевая — обе на ядре 0.
  volatile uint32_t fileActivityMs_;
  bool fileServerWasRunning_;
  bool wifiAwake_;
  bool wifiHot_;                          // кристалл перегрет: Wi-Fi спит при любой работе
  uint8_t configCache_[ConfigStore::kMaxIniSize];
  uint8_t response_[kMaxPayload];
};

// Keep the previous application budget plus the explicitly enlarged receive
// payload (8196 instead of 1024 bytes); the parser lives outside task stacks.
static_assert(sizeof(Application) <= 20 * 1024 + kMaxMediaPayload - kMaxPayload,
              "core-1 static RAM budget exceeded");

Application::Application()
    :
      link_(),
      transport_(link_),
      config_(),
      configStore_(),
      vfsBridge_(transport_),
      netClient_(),
      onlineUpdater_(netClient_),
      ftp_(vfsBridge_, networkEventEntry, this),
      smb_(vfsBridge_, networkEventEntry, this),
      ota_(),
#if ZIFI_HAS_AUDIO
      audioApp_(*this),
      moduleApp_(*this, audioApp_.player()),
#endif
      timeApp_(*this),
      weatherApp_(*this, netClient_),
      wcuApp_(*this, vfsBridge_),
      apps_{
#if ZIFI_HAS_AUDIO
          &moduleApp_,
          &audioApp_,
#endif
          &timeApp_, &weatherApp_, &wcuApp_},
      exchange_(nullptr),
      requestQueue_(nullptr),
      responseQueue_(nullptr),
      eventQueue_(nullptr),
      networkTask_(nullptr),
      networkReady_(false),
      requestPending_(false),
      exchangeInPsram_(false),
      configStoreReady_(false),
      restartAfterOnlineUpdate_(false),
      lastStep_(0),
      lastError_{},
      activeSsid_{},
      activePassword_{},
      wifiBeganMs_(0),
      proxyUpdated_(false),
      proxyStatus_(0),
      activeProxy_{},
      fileActivityMs_(0),
      fileServerWasRunning_(false),
      wifiAwake_(false),
      wifiHot_(false),
      configCache_{},
      response_{} {}

void Application::clearError() {
  lastError_[0] = 0;
}

void Application::reportError(const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(lastError_, sizeof(lastError_), format, arguments);
  va_end(arguments);
  transport_.sendError(lastError_);
}

void Application::setNetworkError(const char* format, ...) {
  if (exchange_ == nullptr) {
    return;
  }
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(exchange_->error, sizeof(exchange_->error), format, arguments);
  va_end(arguments);
}

bool Application::copyString(const uint8_t* data, size_t length, size_t offset,
                             char* output, size_t capacity,
                             size_t& nextOffset, bool requireTerminator) {
  if (data == nullptr || output == nullptr || capacity == 0 || offset > length) {
    return false;
  }
  size_t end = offset;
  while (end < length && data[end] != 0) {
    ++end;
  }
  if ((requireTerminator && end == length) || end - offset >= capacity) {
    return false;
  }
  memcpy(output, data + offset, end - offset);
  output[end - offset] = 0;
  nextOffset = end < length ? end + 1 : end;
  return true;
}

void Application::begin() {
  // Канал до Z80 — SPI; журнал ESP-IDF идёт в USB модуля и обмену не мешает.
  const bool linkReady = transport_.begin();
  ESP_LOGI("zifi", "firmware %s, reset reason %d", ZIFI_BUILD_VERSION,
           static_cast<int>(esp_reset_reason()));
#if ZIFI_HAS_STATUS_LED
  // Индикатор запускается при любом исходе: отказ канала он и показывает.
#if ZIFI_HAS_AUDIO
  startStatusLed(link_, &audioApp_.player());
#else
  startStatusLed(link_, nullptr);
#endif
#endif
  clearError();
  bool startupHealthy = true;
  if (!linkReady) {
    snprintf(lastError_, sizeof(lastError_), "link init");
    startupHealthy = false;
  }

  // WIFI_INI сохраняется целиком. После SYS_RESET сеть поднимается без участия
  // zifi.spg — это обязательное условие для update.sna.
  char configError[64] = {};
  configStoreReady_ = configStore_.begin(configError, sizeof(configError));
  if (!configStoreReady_) {
    snprintf(lastError_, sizeof(lastError_), "cfg:%s", configError);
    startupHealthy = false;
  } else {
    size_t configLength = 0;
    if (configStore_.load(configCache_, sizeof(configCache_), configLength,
                          configError, sizeof(configError))) {
      char parseError[64] = {};
      if (!config_.parse(configCache_, configLength,
                         parseError, sizeof(parseError))) {
        snprintf(lastError_, sizeof(lastError_), "cfg parse:%s", parseError);
        startupHealthy = false;
      } else {
        applyTimezone();
      }
    } else if (strcmp(configError, "no saved config") != 0) {
      snprintf(lastError_, sizeof(lastError_), "cfg:%s", configError);
      startupHealthy = false;
    }
  }

#if ZIFI_DIAGNOSTIC_LOG
  // Журнал монтирует тот же именованный LittleFS-раздел, поэтому запускаем
  // его только после окончания загрузки конфигурации. Запись BOOT переживает
  // последующий panic/watchdog reset и показывает его причину при чтении.
  if (diagnosticLogBegin()) {
    diagnosticLogEvent("BOOT firmware=%s reset_reason=%d",
                       ZIFI_BUILD_VERSION,
                       static_cast<int>(esp_reset_reason()));
  }
#endif
  // Сетевая консоль (порт 23) и причина прошлой паники — сразу после журнала.
  netConsoleBegin(ZIFI_BUILD_VERSION);
  crashRecordReport();

  for (App* app : apps_) {
    app->begin();
  }
  runElfProbe();

  if (psramAvailable()) {
    exchange_ = static_cast<IntercoreExchange*>(heap_caps_calloc(
        1, sizeof(IntercoreExchange), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    exchangeInPsram_ = exchange_ != nullptr;
  }
  if (exchange_ == nullptr) {
    exchange_ = static_cast<IntercoreExchange*>(heap_caps_calloc(
        1, sizeof(IntercoreExchange), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  requestQueue_ = xQueueCreate(1, sizeof(uint8_t));
  responseQueue_ = xQueueCreate(1, sizeof(uint8_t));
  eventQueue_ = xQueueCreate(8, sizeof(IntercoreEvent));
  if (exchange_ == nullptr || requestQueue_ == nullptr ||
      responseQueue_ == nullptr || eventQueue_ == nullptr) {
    snprintf(lastError_, sizeof(lastError_), "intercore init");
    rejectRunningFirmware();
    return;
  }

  if (!vfsBridge_.begin(psramAvailable())) {
    snprintf(lastError_, sizeof(lastError_), "vfs bridge init");
    startupHealthy = false;
  }
#if ZIFI_HAS_SPI_LINK
  // Сторож обмена: встала задача обмена — запись в журнал и перезапуск ESP.
  startLinkWatchdog(link_, vfsBridge_);
#endif

  if (xTaskCreatePinnedToCore(networkTaskEntry, "zifi-net",
                              kNetworkStackBytes, this, kNetworkPriority,
                              &networkTask_, kNetworkCore) != pdPASS) {
    snprintf(lastError_, sizeof(lastError_), "network task");
    rejectRunningFirmware();
    return;
  }
  const uint32_t started = millis();
  while (!networkReady_ && static_cast<uint32_t>(millis() - started) < 1000) {
    delay(1);
  }
  if (!networkReady_) {
    snprintf(lastError_, sizeof(lastError_), "network core timeout");
    startupHealthy = false;
  }

  if (startupHealthy) {
    if (!confirmRunningFirmware()) {
      snprintf(lastError_, sizeof(lastError_), "ota confirm failed");
    }
  } else {
    rejectRunningFirmware();
  }
}

void Application::networkTaskEntry(void* context) {
  static_cast<Application*>(context)->networkTaskLoop();
}

bool Application::networkEventEntry(void* context, uint8_t command,
                                     const uint8_t* data, uint16_t length) {
  auto* self = static_cast<Application*>(context);
  // Клиент подключился, дал команду или идёт передача — это обмен. Шкала Wi-Fi
  // (kEventWifiSignal) обменом не считается: её шлёт сама прошивка.
  if (command >= kEventFtpClient && command <= kEventSmbProgress) {
    self->fileActivityMs_ = millis();
  }
  return self->enqueueNetworkEvent(command, data, length);
}

bool Application::sendEvent(uint8_t command, const uint8_t* data,
                            uint16_t length, uint32_t waitMs) {
  if (eventQueue_ == nullptr || length > sizeof(IntercoreEvent::data) ||
      (length != 0 && data == nullptr)) {
    return false;
  }
  IntercoreEvent event{};
  event.command = command;
  event.length = length;
  if (length != 0) {
    memcpy(event.data, data, length);
  }
  return xQueueSend(eventQueue_, &event,
                    waitMs == 0 ? 0 : pdMS_TO_TICKS(waitMs)) == pdTRUE;
}

bool Application::stopFileServers() {
  ftp_.stop();
  return smb_.stop();
}

// Команда Z80, которой нет у ядра: её исполняет приложение — сразу либо на
// сетевом ядре. false — команды нет ни у кого.
bool Application::dispatchToApps(const PacketView& packet) {
  for (App* app : apps_) {
    if (app->handleCommand(packet)) {
      return true;
    }
    NetworkCommand how;
    if (!app->networkCommand(packet.command, how)) {
      continue;
    }
    if (how.ack) {
      transport_.sendAck();
    }
    const bool submitted =
        how.payload ? submitNetwork(packet.command, packet.data, packet.length)
                    : submitNetwork(packet.command, nullptr, 0);
    if (!submitted) {
      reportError("network busy");
      app->sendNetworkFailure(packet.command);
    }
    return true;
  }
  return false;
}

App* Application::networkApp(uint8_t command) const {
  for (App* app : apps_) {
    NetworkCommand how;
    if (app->networkCommand(command, how)) {
      return app;
    }
  }
  return nullptr;
}

// Перед FTP или SMB: остановить приложения, монопольно занявшие мост VFS.
// Возврат — имя того, кто не остановился, либо nullptr.
const char* Application::stopAppsForFileServer() {
  for (App* app : apps_) {
    if (!app->stopForFileServer()) {
      return app->name();
    }
  }
  return nullptr;
}

bool Application::appsKeepWifiAwake() const {
  for (App* app : apps_) {
    if (app->keepsWifiAwake()) {
      return true;
    }
  }
  return false;
}

bool Application::appsBlockFlashWrites() const {
  for (App* app : apps_) {
    if (app->blocksFlashWrites()) {
      return true;
    }
  }
  return false;
}

// Энергосбережение Wi-Fi: выключено, пока с файловым сервером работают (не дольше
// kWifiAwakeIdleMs без обмена) и всё время, пока приложение работает с сетью
// (appAwake: обновление WC, поток радио модуля). До 0.1.34 приложения шли тем же
// путём, что сервер, и через kWifiAwakeIdleMs Wi-Fi засыпал посреди радио. Кристалл
// горячее kWifiHotTenths — энергосбережение в любом случае (до kWifiCoolTenths).
void Application::updateWifiPowerSave(bool fileServerRunning, bool appAwake, uint32_t now) {
  if (fileServerRunning && !fileServerWasRunning_) {
    fileActivityMs_ = now;  // запуск сервера — тоже обмен
  }
  fileServerWasRunning_ = fileServerRunning;
  bool awake = appAwake;
  if (!awake && fileServerRunning) {
    // Обмен — это и событие клиента, и любой файловый запрос к Z80: мост
    // запоминает время каждой заявки. Длинная передача FTP идёт без команд,
    // но окна чтения и записи уходят через мост постоянно.
    uint32_t last = fileActivityMs_;
    const uint32_t vfs = vfsBridge_.pendingSinceMs();
    if (static_cast<int32_t>(vfs - last) > 0) {
      last = vfs;
    }
    awake = static_cast<uint32_t>(now - last) < kWifiAwakeIdleMs;
  }
#if ZIFI_HAS_AUDIO
  const int16_t tenths = moduleApp_.temperatureTenths();
  if (tenths != INT16_MIN) {
    if (!wifiHot_ && tenths > kWifiHotTenths) {
      wifiHot_ = true;
      diagnosticLogEvent("NET chip %d.%d C: wifi power save", tenths / 10, tenths % 10);
    } else if (wifiHot_ && tenths < kWifiCoolTenths) {
      wifiHot_ = false;
      diagnosticLogEvent("NET chip %d.%d C: wifi power save as usual", tenths / 10,
                         tenths % 10);
    }
  }
#endif
  if (wifiHot_) {
    awake = false;
  }
  if (awake == wifiAwake_) {
    return;
  }
  // Без энергосбережения радио слушает эфир постоянно: запрос не ждёт маяка
  // точки доступа, а на занятом эфире ESP не теряет кадры. Цена — больший ток,
  // поэтому только пока с сервером работают.
  WiFi.setSleep(!awake);
  wifiAwake_ = awake;
  diagnosticLogEvent("NET wifi-power-save=%u", awake ? 0U : 1U);
}

bool Application::onlineUpdateProgressEntry(void* context, uint8_t stage,
                                            uint8_t percent) {
  const uint8_t data[2] = {stage, percent};
  return static_cast<Application*>(context)->enqueueNetworkEvent(
      kEventOnlineUpdateProgress, data, sizeof(data));
}

bool Application::enqueueNetworkEvent(uint8_t command, const uint8_t* data,
                                      uint16_t length) {
  // События служат только для индикации. Переполненная очередь не должна
  // останавливать FTP-сокет или файловую операцию.
  return sendEvent(command, data, length, 0);
}

void Application::networkTaskLoop() {
  // Всё прикладное состояние Wi-Fi принадлежит ядру 0. Задача событий Arduino
  // тоже закреплена за ядром 0 (CONFIG_ARDUINO_EVENT_RUN_CORE0 в sdkconfig.defaults).
  // Ядро 1 общается с сетью только через очереди, поэтому канал с Z80 и VFS не
  // блокируются на DNS, AP или UDP.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (config_.ssid()[0] != 0) {
    const char* password = config_.password();
    WiFi.begin(config_.ssid(), password);
    snprintf(activeSsid_, sizeof(activeSsid_), "%s", config_.ssid());
    snprintf(activePassword_, sizeof(activePassword_), "%s", password);
    wifiBeganMs_ = millis() | 1;
  }
  diagnosticLogEvent("NET task-start wifi_status=%d",
                     static_cast<int>(WiFi.status()));
  networkReady_ = true;

  uint8_t token = 0;
  uint32_t lastDiagnosticFlushMs = 0;
  uint32_t lastWifiSignalMs = 0;
  bool wifiSignalActive = false;
  for (;;) {
    if (xQueueReceive(requestQueue_, &token, pdMS_TO_TICKS(2)) == pdTRUE) {
      processNetworkRequest();
      xQueueOverwrite(responseQueue_, &token);
    }
    // OTA, FTP и SMB запускаются взаимоисключающе. SMB-протокол исполняется в
    // своей задаче ядра 0, а здесь обслуживаются лёгкие NBNS/WS-Discovery.
    smb_.pollDiscovery();
    if (ota_.running()) {
      ota_.poll();
    } else if (!smb_.running()) {
      ftp_.poll();
    }
    // RSSI читается локально на сетевом ядре и уходит отдельным индикаторным
    // событием, пока работает FTP или SMB. Никаких SYS_INFO/FILEX-команд для
    // шкалы не создаётся.
    const bool fileServerRunning = smb_.running() || ftp_.running();
    const uint32_t signalNow = millis();
    if (fileServerRunning &&
        (!wifiSignalActive ||
         static_cast<uint32_t>(signalNow - lastWifiSignalMs) >=
             kWifiSignalIntervalMs)) {
      char signal[31] = {};
      const bool connected = WiFi.status() == WL_CONNECTED;
      const size_t length = formatWifiSignal(
          signal, sizeof(signal), connected, connected ? WiFi.RSSI() : -127);
      if (length != 0) {
        enqueueNetworkEvent(kEventWifiSignal,
                            reinterpret_cast<const uint8_t*>(signal),
                            static_cast<uint16_t>(length));
      }
      lastWifiSignalMs = signalNow;
      wifiSignalActive = true;
    } else if (!fileServerRunning) {
      wifiSignalActive = false;
    }
    updateWifiPowerSave(fileServerRunning, appsKeepWifiAwake(), signalNow);
    for (App* app : apps_) {
      app->pollNetworkCore(signalNow);
    }
#if ZIFI_DIAGNOSTIC_LOG
    // LittleFS блокирует flash cache. Сохранять журнал можно только когда ни
    // один сетевой файловый сервер и канал/VFS не способны принять работу и
    // не идёт звук: запись во flash слышна в нём как запинка.
    const uint32_t now = millis();
    if (!appsBlockFlashWrites() && !smb_.running() && !ftp_.running() &&
        !ota_.running() &&
        !vfsBridge_.requestPending() &&
        vfsBridge_.networkToVfsAvailable() == 0 &&
        vfsBridge_.vfsToNetworkAvailable() == 0 &&
        static_cast<uint32_t>(now - lastDiagnosticFlushMs) >= 1000) {
      diagnosticLogFlush();
      lastDiagnosticFlushMs = now;
    }
#endif
  }
}

// Подключиться к сети ssid (не дольше kWifiTimeoutMs). Начатое подключение к той
// же сети (при загрузке или прошлой командой) не обрывается, а дожидается: его
// повторяет само Arduino, и медленная точка доступа (дольше kWifiTimeoutMs)
// подключится к следующей попытке плагина. До 0.1.33 каждая команда начинала
// подключение заново, и такая точка не подключалась ни с какой попытки (заставка
// погоды повторяет WIFI_INI через 10, 20, 30… с). Заново — другая сеть, отказ
// (неверный пароль: WL_CONNECT_FAILED Arduino не повторяет), Wi-Fi остановлен
// или подключение идёт дольше kWifiAttemptMs.
bool Application::connectWifi(const char* ssid, const char* password) {
  if (ssid == nullptr || *ssid == 0) {
    return false;
  }
  const char* safePassword = password == nullptr ? "" : password;
  const bool sameNetwork = strcmp(activeSsid_, ssid) == 0 &&
                           strcmp(activePassword_, safePassword) == 0;
  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED && sameNetwork) {
    return true;
  }
  bool stopped = status == WL_NO_SHIELD;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  stopped = stopped || status == WL_STOPPED;      // в Arduino 2.x (переходник ZiFi) его нет
#endif
  const bool restart = !sameNetwork || status == WL_CONNECT_FAILED || stopped ||
                       wifiBeganMs_ == 0 ||
                       static_cast<uint32_t>(millis() - wifiBeganMs_) >= kWifiAttemptMs;
  if (restart) {
    WiFi.disconnect(false, false);
    delay(10);
    WiFi.begin(ssid, safePassword);
    snprintf(activeSsid_, sizeof(activeSsid_), "%s", ssid);
    snprintf(activePassword_, sizeof(activePassword_), "%s", safePassword);
    wifiBeganMs_ = millis() | 1;
  }
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED &&
         static_cast<uint32_t>(millis() - started) < kWifiTimeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(25));
  }
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  configTime(config_.timezoneHours() * 3600, 0, "pool.ntp.org", "time.google.com");
  updateProxyState();
  proxyUpdated_ = true;
  return true;
}

void Application::updateProxyState() {
  const char* host = config_.proxyHost();
  const uint16_t port = config_.proxyPort();
  if (host == nullptr || *host == 0) {
    proxyStatus_ = 0;
    activeProxy_[0] = 0;
    netClient_.clearProxy();
    return;
  }
  snprintf(activeProxy_, sizeof(activeProxy_), "%s:%u", host, port);
  if (WiFi.status() == WL_CONNECTED) {
    uint16_t elapsedMs = 0;
    if (netClient_.probe(host, port, elapsedMs)) {
      proxyStatus_ = 1;
      netClient_.setProxy(host, port);
    } else {
      proxyStatus_ = 2;
      netClient_.clearProxy();
    }
  } else {
    proxyStatus_ = 0;
    netClient_.clearProxy();
  }
}

void Application::prepareWifiResponse(uint8_t responseCommand,
                                      bool connected) {
  exchange_->responseCommand = responseCommand;
  exchange_->responseLength = 5;
  memset(exchange_->response, 0, 5);
  if (!connected) {
    return;
  }
  exchange_->response[0] = 1;
  const IPAddress address = WiFi.localIP();
  for (uint8_t index = 0; index < 4; ++index) {
    exchange_->response[index + 1] = address[index];
  }
}

void Application::processWifiConnect() {
  char ssid[33];
  char password[64];
  size_t next = 0;
  size_t ignored = 0;
  if (ota_.running()) {
    setNetworkError("ota active");
    prepareWifiResponse(kRespWifiConnect, false);
    return;
  }
  if (!copyString(exchange_->request, exchange_->requestLength, 0,
                  ssid, sizeof(ssid), next, true) ||
      !copyString(exchange_->request, exchange_->requestLength, next,
                  password, sizeof(password), ignored, false)) {
    setNetworkError("wifi payload");
    prepareWifiResponse(kRespWifiConnect, false);
    return;
  }
  const bool connected = connectWifi(ssid, password);
  if (!connected) {
    setNetworkError("wifi timeout");
  }
  prepareWifiResponse(kRespWifiConnect, connected);
}

void Application::processWifiIni() {
  char error[64];
  if (ota_.running()) {
    setNetworkError("ota active");
    prepareWifiResponse(kRespWifiIni, false);
    return;
  }
  if (exchange_->requestLength == 0 ||
      exchange_->requestLength > ConfigStore::kMaxIniSize ||
      !config_.parse(exchange_->request, exchange_->requestLength,
                     error, sizeof(error))) {
    setNetworkError("ini:%s", error[0] == 0 ? "invalid" : error);
    prepareWifiResponse(kRespWifiIni, false);
    return;
  }
  applyTimezone();                       // ещё до сети: часы Evo могут прийти и без неё
  const ConfigSaveResult saved = configStoreReady_
      ? configStore_.saveIfChanged(exchange_->request,
                                   exchange_->requestLength,
                                   error, sizeof(error))
      : ConfigSaveResult::kError;
  if (saved == ConfigSaveResult::kError) {
    setNetworkError("ini cache:%s",
                    configStoreReady_ ? error : "unavailable");
  }
  // Новое подключение connectWifi уже проверил прокси; уже в сети — проверить здесь
  // (в ini мог смениться прокси). Дважды подряд — до 6 с вместо 3 при недоступном
  // прокси, а заставка погоды до 0.1.33 ждала ответ 15 с.
  proxyUpdated_ = false;
  const bool connected = connectWifi(config_.ssid(), config_.password());
  if (!connected) {
    setNetworkError("wifi timeout");
  }
  if (!proxyUpdated_) {
    updateProxyState();
  }
  prepareWifiResponse(kRespWifiIni, connected);
}

void Application::processNetProxyStatus() {
  exchange_->responseCommand = kRespNetProxyStatus;
  exchange_->response[0] = proxyStatus_;
  size_t len = 1;
  if (activeProxy_[0] != 0) {
    const size_t slen = strlen(activeProxy_);
    if (slen < sizeof(exchange_->response) - 2) {
      memcpy(exchange_->response + 1, activeProxy_, slen);
      len += slen;
    }
  }
  exchange_->response[len] = 0;
  exchange_->responseLength = static_cast<uint16_t>(len);
}

void Application::processFtpStart() {
  exchange_->responseCommand = kRespFtpStart;
  exchange_->responseLength = 3;
  memset(exchange_->response, 0, 3);
  if (ota_.running()) {
    setNetworkError("ftp:ota active");
    return;
  }
  netClient_.close();
  // Приложение с монопольным мостом VFS (обновление WC): пока его задача
  // жива, FTP запускать нельзя.
  const char* busy = stopAppsForFileServer();
  if (busy != nullptr) {
    setNetworkError("ftp:%s stopping", busy);
    return;
  }
  if (!smb_.stop()) {
    setNetworkError("ftp:smb stopping");
    return;
  }
  ftp_.stop();
  char error[64] = {};
  uint16_t port = 0;
  ftp_.setTimezoneHours(config_.timezoneHours());
  const bool started = ftp_.start(exchange_->request, exchange_->requestLength,
                                  port, error, sizeof(error));
  if (started) {
    exchange_->response[0] = 1;
    writeLe16(exchange_->response + 1, port);
#if ZIFI_HAS_SPI_LINK
    // Время в LIST, MLSD и MDTM — UTC: местное время карты минус пояс time:.
    bringupLog("zifi-ftp", "FTP on port %u: file times in UTC = card time %+d h",
               static_cast<unsigned>(port), -config_.timezoneHours());
#endif
  } else {
    setNetworkError("ftp:%s", error);
  }
}

void Application::processFtpStop() {
  ftp_.stop();
  exchange_->responseCommand = kRespFtpStop;
  exchange_->responseLength = 1;
  exchange_->response[0] = 1;
}

void Application::processFtpRamStats() {
  exchange_->responseCommand = kRespFtpRamStats;
  exchange_->responseLength = static_cast<uint16_t>(
      ftp_.makeRamStats(exchange_->response, sizeof(exchange_->response)));
}

void Application::processSmbStart() {
  // Ответ: [успех][порт LE16][NBNS 0/1]. Последний байт позволяет плагину
  // честно показать, удалось ли занять UDP/137 для заданного имени SMB.
  exchange_->responseCommand = kRespSmbStart;
  exchange_->responseLength = 4;
  memset(exchange_->response, 0, 4);
  if (ota_.running()) {
    setNetworkError("smb:ota active");
    return;
  }
  netClient_.close();
  ftp_.stop();
  const char* busy = stopAppsForFileServer();
  if (busy != nullptr) {
    setNetworkError("smb:%s stopping", busy);
    return;
  }
  if (!smb_.stop()) {
    setNetworkError("smb:previous stopping");
    return;
  }
  char error[64] = {};
  uint16_t port = 0;
  bool netbios = false;
  smb_.setTimezoneHours(config_.timezoneHours());
  if (!smb_.start(exchange_->request, exchange_->requestLength, port, netbios,
                  error, sizeof(error))) {
    setNetworkError("smb:%s", error);
    return;
  }
  exchange_->response[0] = 1;
  writeLe16(exchange_->response + 1, port);
  exchange_->response[3] = netbios ? 1 : 0;
}

void Application::processSmbStop() {
  exchange_->responseCommand = kRespSmbStop;
  exchange_->responseLength = 1;
  exchange_->response[0] = smb_.stop() ? 1 : 0;
  if (exchange_->response[0] == 0) {
    setNetworkError("smb:stop timeout");
  }
}

void Application::processUpdateStart() {
  exchange_->responseCommand = kRespUpdateStart;
  exchange_->responseLength = 7;
  memset(exchange_->response, 0, 7);

  bool connected = WiFi.status() == WL_CONNECTED;
  if (!connected && config_.ssid()[0] != 0) {
    connected = connectWifi(config_.ssid(), config_.password());
  }
  if (!connected) {
    setNetworkError("ota:no wifi config");
    return;
  }

  netClient_.close();
  ftp_.stop();
  if (!smb_.stop()) {
    setNetworkError("ota:smb stopping");
    return;
  }
#if ZIFI_DIAGNOSTIC_LOG
  // Здесь SMB/FTP уже остановлены, поэтому можно сохранить весь журнал
  // завершившегося прогона до того, как OTA начнёт писать другой flash-раздел.
  diagnosticLogFlush();
#endif
  char error[64] = {};
  const bool started = ota_.running() ||
      ota_.start(OtaServer::kDefaultPort, error, sizeof(error));
  if (!started) {
    setNetworkError("ota:%s", error);
    return;
  }
  exchange_->response[0] = 1;
  copyIp(exchange_->response + 1, WiFi.localIP());
  writeLe16(exchange_->response + 5, ota_.port());
}

void Application::processUpdateStop() {
  ota_.stop();
  exchange_->responseCommand = kRespUpdateStop;
  exchange_->responseLength = 1;
  exchange_->response[0] = 1;
}

void Application::processOnlineUpdateCheck() {
  exchange_->responseCommand = kRespOnlineUpdateCheck;
  exchange_->responseLength = 1;
  exchange_->response[0] = 0;

  bool connected = WiFi.status() == WL_CONNECTED;
  if (!connected && config_.ssid()[0] != 0) {
    connected = connectWifi(config_.ssid(), config_.password());
  }
  if (!connected) {
    setNetworkError("update-check:no wifi");
    return;
  }
  if (ota_.running()) {
    setNetworkError("update-check:ota listener active");
    return;
  }

  netClient_.close();
  OnlineUpdateManifest manifest{};
  char error[64] = {};
  if (!onlineUpdater_.check(exchange_->request, sizeof(exchange_->request),
                            manifest, onlineUpdateProgressEntry, this, error,
                            sizeof(error))) {
    setNetworkError("update-check:%s", error);
    return;
  }
  const size_t versionLength = strlen(manifest.version);
  exchange_->response[0] = 1;
  exchange_->response[1] =
      comparePublishedVersion(ZIFI_BUILD_VERSION, manifest.version);
  memcpy(exchange_->response + 2, manifest.version, versionLength);
  exchange_->responseLength = static_cast<uint16_t>(2 + versionLength);
}

void Application::processOnlineUpdate() {
  exchange_->responseCommand = kRespOnlineUpdate;
  exchange_->responseLength = 1;
  exchange_->response[0] = 0;
  restartAfterOnlineUpdate_ = false;

  bool connected = WiFi.status() == WL_CONNECTED;
  if (!connected && config_.ssid()[0] != 0) {
    connected = connectWifi(config_.ssid(), config_.password());
  }
  if (!connected) {
    setNetworkError("update:no wifi");
    return;
  }
  if (ota_.running()) {
    setNetworkError("update:ota listener active");
    return;
  }

  netClient_.close();
  ftp_.stop();
  if (!smb_.stop()) {
    setNetworkError("update:smb stopping");
    return;
  }
#if ZIFI_DIAGNOSTIC_LOG
  diagnosticLogFlush();
#endif

  char error[64] = {};
  uint8_t digest[32] = {};
  if (!onlineUpdater_.install(
          exchange_->request, sizeof(exchange_->request), digest,
          onlineUpdateProgressEntry, this, error, sizeof(error))) {
    setNetworkError("update:%s", error);
    return;
  }
  exchange_->response[0] = 1;
  memcpy(exchange_->response + 1, digest, sizeof(digest));
  exchange_->responseLength = 1 + sizeof(digest);
  restartAfterOnlineUpdate_ = true;
}

void Application::processNetOpen() {
  char host[254];
  size_t next = 0;
  exchange_->responseCommand = kRespNetOpen;
  exchange_->responseLength = 1;
  exchange_->response[0] = 0;
  if (ota_.running()) {
    setNetworkError("open:ota active");
    return;
  }
  if (!copyString(exchange_->request, exchange_->requestLength, 0,
                  host, sizeof(host), next, false) || host[0] == 0) {
    setNetworkError("open:no host");
    return;
  }
  uint16_t port = 80;
  if (next + 2 <= exchange_->requestLength) {
    port = readLe16(exchange_->request + next);
  }
  ftp_.stop();
  char error[64] = {};
  bool opened = false;
  if (WiFi.status() != WL_CONNECTED) {
    snprintf(error, sizeof(error), "no wifi");
  } else {
    opened = netClient_.open(host, port, error, sizeof(error));
  }
  exchange_->response[0] = opened ? 1 : 0;
  if (!opened) {
    setNetworkError("open:%s", error);
  }
}

void Application::processNetSend() {
  char error[64] = {};
  const bool sent = netClient_.sendAll(exchange_->request,
                                       exchange_->requestLength,
                                       error, sizeof(error));
  exchange_->responseCommand = kRespNetSend;
  exchange_->responseLength = 1;
  exchange_->response[0] = sent ? 1 : 0;
  if (!sent) {
    setNetworkError("send:%s", error);
  }
}

void Application::processNetReceive() {
  size_t limit = kMaxPayload - 1;
  if (exchange_->requestLength >= 2) {
    limit = readLe16(exchange_->request);
    if (limit < 1) {
      limit = 1;
    }
    if (limit > kMaxPayload - 1) {
      limit = kMaxPayload - 1;
    }
  }
  size_t received = 0;
  bool eof = false;
  char error[64] = {};
  exchange_->responseCommand = kRespNetRecv;
  if (!netClient_.receive(exchange_->response + 1, limit, received, eof,
                          error, sizeof(error))) {
    setNetworkError("recv:%s", error);
    exchange_->response[0] = 1;
    exchange_->responseLength = 1;
    return;
  }
  exchange_->response[0] = eof ? 1 : 0;
  exchange_->responseLength = static_cast<uint16_t>(received + 1);
}

void Application::processNetClose() {
  netClient_.close();
  exchange_->responseCommand = kRespNetClose;
  exchange_->responseLength = 1;
  exchange_->response[0] = 1;
}

void Application::processHttpGet() {
  char host[254];
  char path[384];
  size_t next = 0;
  size_t afterPath = 0;
  bool valid = copyString(exchange_->request, exchange_->requestLength, 0,
                          host, sizeof(host), next, true) &&
               host[0] != 0 && next + 2 <= exchange_->requestLength;
  uint16_t port = 80;
  if (valid) {
    port = readLe16(exchange_->request + next);
    next += 2;
    valid = copyString(exchange_->request, exchange_->requestLength, next,
                       path, sizeof(path), afterPath, false);
  }
  exchange_->responseCommand = kRespNetHttpGet;
  exchange_->responseLength = 7;
  memset(exchange_->response, 0, 7);
  if (ota_.running()) {
    setNetworkError("get:ota active");
    return;
  }
  if (!valid) {
    setNetworkError("get:bad payload");
    return;
  }
  if (path[0] == 0) {
    snprintf(path, sizeof(path), "/");
  }
  ftp_.stop();
  uint16_t statusCode = 0;
  uint32_t contentLength = 0;
  char error[64] = {};
  bool ok = false;
  if (WiFi.status() != WL_CONNECTED) {
    snprintf(error, sizeof(error), "no wifi");
  } else {
    ok = netClient_.httpGet(host, port, path, statusCode, contentLength,
                            error, sizeof(error));
  }
  if (ok) {
    exchange_->response[0] = 1;
    writeLe16(exchange_->response + 1, statusCode);
    writeLe32(exchange_->response + 3, contentLength);
  } else {
    setNetworkError("get:%s", error);
  }
}

void Application::processNetProbe() {
  char host[254];
  size_t next = 0;
  exchange_->responseCommand = kRespNetPing;
  exchange_->responseLength = 3;
  memset(exchange_->response, 0, 3);
  if (ota_.running()) {
    setNetworkError("ping:ota active");
    return;
  }
  if (!copyString(exchange_->request, exchange_->requestLength, 0,
                  host, sizeof(host), next, false) || host[0] == 0) {
    setNetworkError("ping:no host");
    return;
  }
  ftp_.stop();
  uint16_t elapsed = 0;
  const bool connected = WiFi.status() == WL_CONNECTED &&
                         netClient_.probe(host, 80, elapsed);
  exchange_->response[0] = connected ? 1 : 0;
  writeLe16(exchange_->response + 1, connected ? elapsed : 0);
  // Таймаут сетевой пробы — нормальный отрицательный результат, а не ошибка.
}

void Application::processIpConfig() {
  exchange_->responseCommand = kRespNetIpConfig;
  exchange_->responseLength = 16;
  memset(exchange_->response, 0, 16);
  if (WiFi.status() == WL_CONNECTED) {
    copyIp(exchange_->response, WiFi.localIP());
    copyIp(exchange_->response + 4, WiFi.subnetMask());
    copyIp(exchange_->response + 8, WiFi.gatewayIP());
    copyIp(exchange_->response + 12, WiFi.dnsIP(0));
  }
}

void Application::processNetworkRequest() {
  exchange_->responseCommand = kError;
  exchange_->responseLength = 0;
  exchange_->error[0] = 0;
  switch (exchange_->requestCommand) {
    case kWifiConnect:
      processWifiConnect();
      break;
    case kWifiIni:
      processWifiIni();
      break;
    case kFtpStart:
      processFtpStart();
      break;
    case kFtpStop:
      processFtpStop();
      break;
    case kFtpRamStats:
      processFtpRamStats();
      break;
    case kSmbStart:
      processSmbStart();
      break;
    case kSmbStop:
      processSmbStop();
      break;
    case kUpdateStart:
      processUpdateStart();
      break;
    case kUpdateStop:
      processUpdateStop();
      break;
    case kOnlineUpdateCheck:
      processOnlineUpdateCheck();
      break;
    case kOnlineUpdate:
      processOnlineUpdate();
      break;
    case kNetOpen:
      processNetOpen();
      break;
    case kNetSend:
      processNetSend();
      break;
    case kNetRecv:
      processNetReceive();
      break;
    case kNetClose:
      processNetClose();
      break;
    case kNetHttpGet:
      processHttpGet();
      break;
    case kNetPing:
      processNetProbe();
      break;
    case kNetIpConfig:
      processIpConfig();
      break;
    case kNetProxyStatus:
      processNetProxyStatus();
      break;
    default: {
      App* app = networkApp(exchange_->requestCommand);
      if (app != nullptr) {
        app->processNetwork(*exchange_);
      } else {
        setNetworkError("network command:%02X", exchange_->requestCommand);
      }
      break;
    }
  }
}

bool Application::submitNetwork(uint8_t command, const uint8_t* data,
                                uint16_t length) {
  if (!networkReady_ || exchange_ == nullptr || requestPending_ ||
      length > sizeof(exchange_->request) ||
      (length != 0 && data == nullptr)) {
    return false;
  }
  exchange_->requestCommand = command;
  exchange_->requestLength = length;
  if (length != 0) {
    memcpy(exchange_->request, data, length);
  }
  const uint8_t token = 1;
  if (xQueueSend(requestQueue_, &token, 0) != pdTRUE) {
    return false;
  }
  requestPending_ = true;
  return true;
}

void Application::sendWifiFailure(uint8_t responseCommand) {
  const uint8_t failed[5] = {};
  transport_.send(responseCommand, failed, sizeof(failed));
}

void Application::sendNetworkFailure(uint8_t command) {
  uint8_t failed[16] = {};
  switch (command) {
    case kFtpStart:
      transport_.send(kRespFtpStart, failed, 3);
      break;
    case kFtpStop:
      transport_.send(kRespFtpStop, failed, 1);
      break;
    case kFtpRamStats:
      transport_.send(kRespFtpRamStats);
      break;
    case kSmbStart:
      transport_.send(kRespSmbStart, failed, 4);
      break;
    case kSmbStop:
      transport_.send(kRespSmbStop, failed, 1);
      break;
    case kUpdateStart:
      transport_.send(kRespUpdateStart, failed, 7);
      break;
    case kUpdateStop:
      transport_.send(kRespUpdateStop, failed, 1);
      break;
    case kOnlineUpdateCheck:
      transport_.send(kRespOnlineUpdateCheck, failed, 1);
      break;
    case kOnlineUpdate:
      transport_.send(kRespOnlineUpdate, failed, 1);
      break;
    case kNetOpen:
      transport_.send(kRespNetOpen, failed, 1);
      break;
    case kNetSend:
      transport_.send(kRespNetSend, failed, 1);
      break;
    case kNetRecv:
      failed[0] = 1;
      transport_.send(kRespNetRecv, failed, 1);
      break;
    case kNetClose:
      transport_.send(kRespNetClose, failed, 1);
      break;
    case kNetHttpGet:
      transport_.send(kRespNetHttpGet, failed, 7);
      break;
    case kNetPing:
      transport_.send(kRespNetPing, failed, 3);
      break;
    case kNetIpConfig:
      transport_.send(kRespNetIpConfig, failed, 16);
      break;
    case kNetProxyStatus:
      transport_.send(kRespNetProxyStatus, failed, 1);
      break;
  }
}

void Application::pollNetworkResponse() {
  if (!requestPending_) {
    return;
  }
  uint8_t token = 0;
  if (xQueueReceive(responseQueue_, &token, 0) != pdTRUE) {
    return;
  }
  requestPending_ = false;
  if (exchange_->error[0] != 0) {
    reportError("%s", exchange_->error);
  } else {
    clearError();
  }
  transport_.send(exchange_->responseCommand, exchange_->response,
                  exchange_->responseLength);
  if (restartAfterOnlineUpdate_ &&
      exchange_->responseCommand == kRespOnlineUpdate &&
      exchange_->responseLength != 0 && exchange_->response[0] != 0) {
    restartAfterOnlineUpdate_ = false;
    // Сначала физически отправляем Z80 финальный SHA-verified ответ. Только
    // после этого можно перезапускаться в уже выбранный OTA-раздел.
    transport_.flush();
    delay(250);
    ESP.restart();
  }
}

void Application::pollNetworkEvent() {
  if (eventQueue_ == nullptr) {
    return;
  }
  IntercoreEvent event{};
  if (xQueueReceive(eventQueue_, &event, 0) == pdTRUE) {
    transport_.send(event.command, event.data, event.length);
  }
}

void Application::handleSysInfo() {
  transport_.sendAck();
  const char* proxyStr = proxyStatus_ == 1 ? activeProxy_
                       : (proxyStatus_ == 2 ? "UNREACHABLE" : "OFF");
  // Поля разделены пробелом, значение — до следующего пробела. FW: должно
  // оставаться последним: плагины берут версию от него до конца строки.
  char extra[128] = {};
#if ZIFI_HAS_SPI_LINK
  {
    const SpiLinkCore::Stats link = link_.stats();
    char audio[16] = "off";
#if ZIFI_HAS_AUDIO
    audioApp_.describe(audio, sizeof(audio));
#endif
    // LINK: обменов / повторов запроса / испорченных запросов / блоков DMA к
    // Z80 / из них не прошедших сравнение / блоков DMA от Z80 / из них
    // отвергнутых.
    snprintf(extra, sizeof(extra),
             "BOARD:%s IDF:%s LINK:%u/%u/%u/%u/%u/%u/%u AUDIO:%s ", board::kName,
             esp_get_idf_version(), static_cast<unsigned>(link.turns),
             static_cast<unsigned>(link.repeats),
             static_cast<unsigned>(link.badRequests),
             static_cast<unsigned>(link.bulkBlocks),
             static_cast<unsigned>(link.bulkBad),
             static_cast<unsigned>(link.putBlocks),
             static_cast<unsigned>(link.putBad), audio);
  }
#endif
  const int length = snprintf(
      reinterpret_cast<char*>(response_), sizeof(response_),
      "RAM:%uB PSRAM:%u/%uB Flash:%uB Sketch:%uB RST:%u "
      "CORE:%s+VFS1/NET+EVT0 NETSTK:%uB CTRL:%s "
      "VFSBUF:%s:%u+%u %sPROXY:%s FW:%s",
      // uint32_t в новых ядрах — unsigned long, а формат %u ждёт unsigned.
      static_cast<unsigned>(ESP.getFreeHeap()),
      static_cast<unsigned>(ESP.getFreePsram()),
      static_cast<unsigned>(ESP.getPsramSize()),
      static_cast<unsigned>(ESP.getFlashChipSize()),
      static_cast<unsigned>(ESP.getSketchSize()),
      static_cast<unsigned>(esp_reset_reason()),
      transport_.linkName(),
      static_cast<unsigned>(networkTask_ != nullptr
                                ? uxTaskGetStackHighWaterMark(networkTask_)
                                : 0),
      exchangeInPsram_ ? "PSRAM" : "RAM",
      vfsBridge_.buffersInPsram() ? "PSRAM" : "RAM",
      static_cast<unsigned>(vfsBridge_.ringCapacity()),
      static_cast<unsigned>(vfsBridge_.ringCapacity()),
      extra, proxyStr, ZIFI_BUILD_VERSION);
  if (length <= 0) {
    reportError("sysinfo format");
    transport_.send(kRespSysInfo);
    return;
  }
  const size_t used = static_cast<size_t>(length) < sizeof(response_)
                          ? static_cast<size_t>(length)
                          : sizeof(response_) - 1;
  transport_.send(kRespSysInfo, response_, static_cast<uint16_t>(used));
  // Native-плагины считают этап 8 признаком успешно полученного SYS_INFO.
  lastStep_ = 8;
  clearError();
}

void Application::applyTimezone() {
  // В строке TZ POSIX знак обратный: «UTC-3» — это UTC+3. Так же ставит пояс и
  // configTime при подключении к сети; здесь — сразу, как известен zifi.ini.
  char zone[12];
  snprintf(zone, sizeof(zone), "UTC%+d", -static_cast<int>(config_.timezoneHours()));
  setenv("TZ", zone, 1);
  tzset();
}

// [год LE16][месяц][число][часы][минуты][секунды] — местное время → [1 / 0].
// Часы Evo, которые сбились (батарейка, первое включение), показывают давнюю дату:
// годным считается только время позже 1 октября 2026 года.
void Application::handleSysClock(const PacketView& packet) {
  uint8_t ok = 0;
  if (packet.length >= 7) {
    const uint8_t* d = packet.data;
    const unsigned year = d[0] | d[1] << 8;
    struct tm fields = {};
    fields.tm_year = static_cast<int>(year) - 1900;
    fields.tm_mon = d[2] - 1;
    fields.tm_mday = d[3];
    fields.tm_hour = d[4];
    fields.tm_min = d[5];
    fields.tm_sec = d[6];
    fields.tm_isdst = -1;
    const uint32_t date = year * 10000u + d[2] * 100u + d[3];
    const bool valid = year <= 2099 && d[2] >= 1 && d[2] <= 12 && d[3] >= 1 && d[3] <= 31 &&
                       d[4] <= 23 && d[5] <= 59 && d[6] <= 59 && date > 20261001u;
    const time_t when = valid ? mktime(&fields) : static_cast<time_t>(-1);
    if (when != static_cast<time_t>(-1)) {
      const timeval now = {when, 0};
      ok = settimeofday(&now, nullptr) == 0 ? 1 : 0;
    }
    bringupLog("zifi-clock", "clock from Evo RTC %04u-%02u-%02u %02u:%02u:%02u UTC%+d: %s", year,
               d[2], d[3], d[4], d[5], d[6], static_cast<int>(config_.timezoneHours()),
               ok ? "set" : "REFUSED");
  }
  transport_.send(kRespSysClock, &ok, 1);
}

void Application::handleReset() {
  transport_.sendAck();
  transport_.flush();
  delay(100);
  ESP.restart();
}

void Application::handle(const PacketView& packet) {
  if (packet.command != kGetStep) {
    lastStep_ = packet.command;
  }
  switch (packet.command) {
    case kEcho:
      transport_.send(kEcho, packet.data, packet.length);
      clearError();
      break;
    case kPing:
      transport_.send(kReady);
      break;
    case kGetStep: {
      response_[0] = lastStep_;
      const size_t errorLength = strlen(lastError_);
      memcpy(response_ + 1, lastError_, errorLength);
      transport_.send(kRespGetStep, response_,
                      static_cast<uint16_t>(errorLength + 1));
      break;
    }
    case kSysInfo:
      handleSysInfo();
      break;
    case kWifiConnect:
      transport_.sendAck();
      if (!submitNetwork(packet.command, packet.data, packet.length)) {
        reportError("network busy");
        sendWifiFailure(kRespWifiConnect);
      }
      break;
    case kWifiIni:
      transport_.sendAck();
      if (!submitNetwork(packet.command, packet.data, packet.length)) {
        reportError("network busy");
        sendWifiFailure(kRespWifiIni);
      }
      break;
    case kFtpStart:
    case kFtpStop:
    case kFtpRamStats:
    case kSmbStart:
    case kSmbStop:
    case kUpdateStart:
    case kUpdateStop:
    case kOnlineUpdateCheck:
    case kOnlineUpdate:
    case kNetOpen:
    case kNetSend:
    case kNetClose:
    case kNetHttpGet:
    case kNetPing:
    case kNetIpConfig:
    case kNetProxyStatus:
      transport_.sendAck();
      if (!submitNetwork(packet.command, packet.data, packet.length)) {
        reportError("network busy");
        sendNetworkFailure(packet.command);
      }
      break;
    case kNetRecv:
      // NET_RECV по историческому контракту отвечает сразу, без ACK.
      if (!submitNetwork(packet.command, packet.data, packet.length)) {
        reportError("network busy");
        sendNetworkFailure(packet.command);
      }
      break;
    case kSysReset:
      handleReset();
      break;
    case kSysClock:
      handleSysClock(packet);
      break;
#if ZIFI_HAS_SPI_LINK
    case kLinkProbe: {
      // [действие][длина LE16][зерно узора] -> SpiLink::dmaProbe.
      SpiLink::DmaProbeResult probe = {SpiLink::kDmaProbeFailed, 0, 0, 0};
      const uint8_t action = packet.length >= 1 ? packet.data[0] : 0;
      if (packet.length >= 4) {
        probe = link_.dmaProbe(
            action, static_cast<size_t>(packet.data[1]) | (packet.data[2] << 8),
            packet.data[3]);
      }
      response_[0] = probe.status;
      response_[1] = action;
      writeLe16(response_ + 2, probe.length);
      writeLe16(response_ + 4, probe.errors);
      writeLe16(response_ + 6, probe.firstBad);
      transport_.send(kRespLinkProbe, response_, 8);
      break;
    }
#endif
    default:
      if (!dispatchToApps(packet)) {
        reportError("unsupported:%02X", packet.command);
      }
      break;
  }
}

void Application::pollLink() {
  // Сначала обслуживаем запрос ядра 0 к файловой системе. Внутри VfsClient
  // канал остаётся под единственным владельцем ядра 1 до получения ответа Z80.
#if ZIFI_HAS_SPI_LINK
  coreStep(CoreStep::kVfs);
#endif
  vfsBridge_.pollCore1();
#if ZIFI_HAS_SPI_LINK
  coreStep(CoreStep::kNetwork);
#endif
  pollNetworkResponse();
  pollNetworkEvent();
  PacketView packet;
#if ZIFI_HAS_SPI_LINK
  coreStep(CoreStep::kPoll);
#endif
  if (transport_.poll(packet)) {
#if ZIFI_HAS_SPI_LINK
    coreStep(CoreStep::kCommand, packet.command);
#endif
    handle(packet);
  } else {
    // Та же пауза до 1 мс, что прежний delay(1), но заявка моста будит сразу.
    vfsBridge_.waitForRequest(1);
  }
}

Application application;

}  // namespace
}  // namespace zifi

void setup() {
  zifi::application.begin();
}

void loop() {
  // Arduino loopTask у ESP32-S3 закреплён за ядром 1. Только этот путь владеет
  // каналом SPI; сетевая задача создана отдельно на ядре 0.
  zifi::application.pollLink();
}
