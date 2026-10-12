// Обновлятор WC (плагин WC Update) для vdac4_esp_host — ESP платы VDAC4 в Unreal.
//
// Здесь настоящий код прошивки: WcUpdater (src/wc_updater.cpp), мост VfsBridge и
// VfsClient (обмен с картой Evo), Z80Transport и кадры протокола. Собран он на
// заглушках Arduino и FreeRTOS стендов ПК (tests/stubs_host), как tools/host_wcu, и
// с ZIFI_BOARD_VDAC4=1, как прошивка платы. Отдельная единица трансляции: заглушки
// Arduino не должны встречаться с кодом хоста.
//
// Как в прошивке:
// - мост VFS исполняется на «ядре 1» — в главном потоке хоста, там же, где
//   разбираются кадры Z80 (pollCore1). Запрос к карте уходит кадром, ответ Z80
//   транспорт берёт из того же потока кадров; чужой кадр во время обмена разбирает
//   VfsClient::handleUnexpected (PING получает READY, остальное отбрасывается);
// - сеанс — своя задача «ядра 0», здесь поток: run() повторяет WcUpdateService::run
//   (проверка, затем команды APPLY/SYNC/STOP из очереди на два места);
// - события STATE/ENTRY уходят Z80 сразу, каждое — целым кадром.
// HTTPS к GitHub — WinHTTP хоста (Callbacks::httpGet) вместо NetClient прошивки.

#include "wcu_glue.hpp"

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "zifi/byte_link.hpp"
#include "zifi/z80_transport.hpp"
#include "zifi/vfs_bridge.hpp"
#include "zifi/wc_updater.hpp"

// Объекты заглушек Arduino, на которые ссылается код прошивки.
HardwareSerial Serial;
HardwareSerial Serial0;
EspClass ESP;
WiFiClassHost WiFi;
LittleFSClass LittleFS;

namespace wcuhost {
namespace {

Callbacks host = {};

void logf(const char* format, ...) {
  if (host.log == nullptr) {
    return;
  }
  char text[256];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(text, sizeof(text), format, arguments);
  va_end(arguments);
  host.log(text);
}

// Канал до Z80 для Z80Transport моста. Кадр уходит хосту только целиком: так
// его не разорвёт кадр события, который в это время шлёт поток сеанса.
class PipeLink : public zifi::ByteLink {
 public:
  bool begin() override { return true; }

  size_t read(uint8_t* output, size_t capacity) override {
    return host.readFromZ80(output, capacity);
  }

  size_t write(const uint8_t* data, size_t length) override {
    frame_.insert(frame_.end(), data, data + length);
    // [5A][CMD][LEN LE16][данные][сумма] и байт выравнивания, если длина нечётная.
    while (frame_.size() >= 4) {
      size_t total = 5 + (static_cast<size_t>(frame_[2]) | frame_[3] << 8);
      total += total & 1U;
      if (frame_.size() < total) {
        break;
      }
      host.writeToZ80(frame_.data(), total);
      frame_.erase(frame_.begin(), frame_.begin() + static_cast<std::ptrdiff_t>(total));
    }
    return length;
  }

  bool reserve(size_t, uint32_t) override { return true; }
  void flush() override {}
  const char* name() const override { return "SPI"; }
  bool padsFrames() const override { return true; }

 private:
  std::vector<uint8_t> frame_;
};

// Загрузчик обновлятора поверх WinHTTP хоста.
class HostFetcher : public zifi::WcFetcher {
 public:
  void setCancel(const volatile bool* cancel) { cancel_ = cancel; }

  bool get(const char* hostName, const char* path, uint8_t* buffer,
           size_t capacity, size_t& length, uint16_t& status, char* error,
           size_t errorSize) override {
    length = 0;
    status = 0;
    if (cancel_ != nullptr && *cancel_) {
      snprintf(error, errorSize, "stopped");
      return false;
    }
    const bool ok = host.httpGet(hostName, path, buffer, capacity, length, status,
                                 error, errorSize, cancel_);
    logf("GET https://%s%.120s: %s %u, %u bytes", hostName, path, ok ? "HTTP" : error,
         static_cast<unsigned>(status), static_cast<unsigned>(length));
    return ok;
  }

 private:
  const volatile bool* cancel_ = nullptr;
};

PipeLink pipeLink;
zifi::Z80Transport* transport = nullptr;
zifi::VfsBridge* bridge = nullptr;

// Сеанс — как WcUpdateService.
constexpr uint8_t kCommandApply = 1;
constexpr uint8_t kCommandStop = 2;
constexpr uint8_t kCommandSync = 3;
constexpr size_t kQueueSlots = 2;   // xQueueCreate(2, ...) у прошивки

struct Command {
  uint8_t kind = 0;
  uint8_t count = 0;
  uint8_t indices[zifi::WcUpdater::kMaxFiles] = {};
};

std::mutex sessionLock;             // очередь команд и состояние сеанса
std::condition_variable wake;
std::deque<Command> commands;
std::thread task;
HostFetcher* fetcher = nullptr;
zifi::WcUpdater* updater = nullptr;
volatile bool finished = true;
volatile bool applying = false;

bool eventEntry(void*, uint8_t command, const uint8_t* data, uint16_t length) {
  // Строки списка терять нельзя: кадр уходит сразу, без очереди событий.
  return host.sendEvent(command, data, length);
}

void release() {
  if (task.joinable()) {
    task.join();
  }
  delete updater;
  updater = nullptr;
  delete fetcher;
  fetcher = nullptr;
  std::lock_guard<std::mutex> guard(sessionLock);
  commands.clear();
}

bool push(const Command& command) {
  std::lock_guard<std::mutex> guard(sessionLock);
  if (commands.size() >= kQueueSlots) {
    return false;
  }
  commands.push_back(command);
  wake.notify_all();
  return true;
}

void run() {
  // Итог проверки плагину уже сообщён событиями. Даже после ошибки сеанс ждёт
  // STOP: плагин держит на экране причину, пока пользователь не выйдет.
  const bool checked = updater->check();
  logf("check %s: %u files, commit %.12s%s%s", checked ? "done" : "failed",
       static_cast<unsigned>(updater->fileCount()), updater->commit(),
       checked ? "" : ", error ", checked ? "" : updater->lastError());
  while (!updater->stopRequested()) {
    Command command;
    {
      std::unique_lock<std::mutex> guard(sessionLock);
      wake.wait(guard, [] { return !commands.empty() || updater->stopRequested(); });
      if (commands.empty()) {
        break;
      }
      command = commands.front();
      commands.pop_front();
    }
    if (command.kind == kCommandStop || updater->stopRequested()) {
      break;
    }
    if (command.kind == kCommandApply) {
      const bool applied = updater->apply(command.indices, command.count);
      logf("apply %u files: %s%s", static_cast<unsigned>(command.count),
           applied ? "done" : "failed, ", applied ? "" : updater->lastError());
      applying = false;
    } else if (command.kind == kCommandSync) {
      updater->resend();
    }
  }
  logf("session finished");
  finished = true;
}

}  // namespace

bool begin(const Callbacks& callbacks) {
  host = callbacks;
  transport = new zifi::Z80Transport(pipeLink);
  transport->begin();
  bridge = new zifi::VfsBridge(*transport);
  return bridge->begin(true);
}

bool running() { return updater != nullptr; }

bool start(const uint8_t* payload, uint16_t length, char* error, size_t errorSize) {
  if (updater != nullptr) {
    snprintf(error, errorSize, "already running");
    return false;
  }
  fetcher = new HostFetcher();
  updater = new zifi::WcUpdater(*bridge, *fetcher, eventEntry, nullptr);
  if (!updater->configure(payload, length, error, errorSize)) {
    delete updater;
    updater = nullptr;
    delete fetcher;
    fetcher = nullptr;
    return false;
  }
  fetcher->setCancel(updater->stopFlag());
  finished = false;
  applying = false;
  // Что сверяется: репозиторий, ветка и каталог — первые строки запроса плагина.
  logf("session started: %.80s", reinterpret_cast<const char*>(payload));
  task = std::thread(run);
  return true;
}

bool apply(const uint8_t* payload, uint16_t length) {
  if (updater == nullptr || finished || applying || updater->stopRequested() ||
      length > zifi::WcUpdater::kMaxFiles) {
    return false;
  }
  Command command;
  command.kind = kCommandApply;
  command.count = static_cast<uint8_t>(length);
  if (length != 0) {
    memcpy(command.indices, payload, length);
  }
  applying = true;
  if (!push(command)) {
    applying = false;
    return false;
  }
  return true;
}

bool sync() {
  if (updater == nullptr || finished || updater->stopRequested()) {
    return false;
  }
  Command command;
  command.kind = kCommandSync;
  return push(command);
}

void requestStop() {
  if (updater == nullptr) {
    return;
  }
  updater->requestStop();
  std::lock_guard<std::mutex> guard(sessionLock);
  wake.notify_all();
}

bool stop(uint32_t timeoutMs) {
  if (updater == nullptr) {
    return true;
  }
  requestStop();
  Command command;
  command.kind = kCommandStop;
  push(command);
  const auto started = std::chrono::steady_clock::now();
  while (!finished) {
    if (std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(timeoutMs)) {
      return false;  // сеанс ещё в обмене; память освободит следующий stop
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  release();
  return true;
}

void pollCore1() {
  if (bridge != nullptr) {
    bridge->pollCore1();
  }
}

}  // namespace wcuhost
