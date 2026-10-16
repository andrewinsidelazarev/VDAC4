#include "zifi/net_console.hpp"

#if ZIFI_HAS_SPI_LINK

#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "zifi/diagnostic_log.hpp"
#include "zifi/tang_flash.hpp"
#include "zifi/tang_link.hpp"

namespace zifi {
namespace {

constexpr uint16_t kPort = 23;
constexpr size_t kRingBytes = 32 * 1024;
constexpr size_t kLineBytes = 256;
constexpr uint32_t kTaskStack = 4096;
constexpr UBaseType_t kTaskPriority = 2;      // ниже звука и обмена с Z80
constexpr BaseType_t kTaskCore = 0;

// Кольцо вывода: пишут задачи через ESP_LOG*, читает задача консоли.
uint8_t* ring = nullptr;
volatile uint32_t ringHead = 0;              // всего записано байтов
SemaphoreHandle_t ringLock = nullptr;
char lineBuffer[kLineBytes];                 // под ringLock: стек задач не трогаем
vprintf_like_t previousVprintf = nullptr;
const char* firmwareName = "";

void ringWrite(const char* data, size_t length) {
  uint32_t head = ringHead;
  for (size_t index = 0; index < length; ++index) {
    ring[(head + index) % kRingBytes] = static_cast<uint8_t>(data[index]);
  }
  ringHead = head + static_cast<uint32_t>(length);
}

// Копия вывода ESP_LOG* — в кольцо. Занято (пишет другая задача) — строка в
// консоль не попадёт, но в USB уйдёт как обычно: ждать здесь нельзя.
int consoleVprintf(const char* format, va_list arguments) {
  if (ring != nullptr && xSemaphoreTake(ringLock, 0) == pdTRUE) {
    va_list copy;
    va_copy(copy, arguments);
    const int length = vsnprintf(lineBuffer, sizeof(lineBuffer), format, copy);
    va_end(copy);
    if (length > 0) {
      ringWrite(lineBuffer, static_cast<size_t>(length) < sizeof(lineBuffer)
                                ? static_cast<size_t>(length)
                                : sizeof(lineBuffer) - 1);
    }
    xSemaphoreGive(ringLock);
  }
  return previousVprintf != nullptr ? previousVprintf(format, arguments) : 0;
}

bool sendAll(int client, const void* data, size_t length) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  while (length > 0) {
    const int sent = send(client, bytes, length, 0);
    if (sent <= 0) {
      return false;
    }
    bytes += sent;
    length -= static_cast<size_t>(sent);
  }
  return true;
}

bool sendText(int client, const char* format, ...) {
  char text[200];
  va_list arguments;
  va_start(arguments, format);
  const int length = vsnprintf(text, sizeof(text), format, arguments);
  va_end(arguments);
  return length > 0 && sendAll(client, text, static_cast<size_t>(length) < sizeof(text)
                                                 ? static_cast<size_t>(length)
                                                 : sizeof(text) - 1);
}

bool sendStoredLog(int client) {
  DiagnosticLogSnapshot snapshot{};
  if (!diagnosticLogSnapshot(snapshot)) {
    return sendText(client, "stored log is not available\n");
  }
  const bool ok = sendAll(client, snapshot.data, snapshot.length);
  diagnosticLogFreeSnapshot(snapshot);
  return ok;
}

// Регистры Tang по LINK (tang_link.hpp): сводка, чтение и запись одного слова.
bool runTang(int client, const char* arguments) {
  if (!tang::ready()) {
    return sendText(client, "tang: LINK is not ready\n");
  }
  unsigned address = 0;
  unsigned value = 0;
  char verb[8] = {};
  if (sscanf(arguments, " %7s", verb) == 1 && strcmp(verb, "flash") == 0) {
    char text[120];
    tang_flash::describe(text, sizeof(text));
    return sendText(client, "tang %s\n", text);
  }
  unsigned rounds = 0;
  if (sscanf(arguments, " test %u", &rounds) == 1) {
    // Проверка канала: запись и чтение SCRATCH и чтение 16 слов со сверкой ID и SCRATCH
    if (rounds == 0 || rounds > 1000000) {
      rounds = 10000;
    }
    unsigned bad = 0;
    const uint32_t started = millis();
    for (unsigned round = 0; round < rounds; ++round) {
      const uint32_t pattern = esp_random();
      uint32_t back = 0;
      uint32_t block[16] = {};
      if (!tang::write(tang::kRegScratch, &pattern, 1) || !tang::read(tang::kRegScratch, &back, 1) ||
          back != pattern || !tang::read(tang::kRegId, block, 16) ||
          block[0] != tang::kIdValue || block[tang::kRegScratch / 4] != pattern) {
        ++bad;
      }
      if ((round & 255) == 255) {
        vTaskDelay(1);
      }
    }
    return sendText(client, "tang test: %u of %u rounds bad, %u ms, %u kHz\n", bad, rounds,
                    static_cast<unsigned>(millis() - started),
                    static_cast<unsigned>(tang::clockHz() / 1000));
  }
  if (sscanf(arguments, " wr %x %x", &address, &value) == 2) {
    const uint32_t word = value;
    uint32_t back = 0;
    if (!tang::write(address & ~3U, &word, 1) || !tang::read(address & ~3U, &back, 1)) {
      return sendText(client, "tang: transfer failed\n");
    }
    return sendText(client, "tang %02X <- %08X, reads %08X\n", address & ~3U,
                    static_cast<unsigned>(word), static_cast<unsigned>(back));
  }
  if (sscanf(arguments, " rd %x", &address) == 1) {
    uint32_t word = 0;
    if (!tang::read(address & ~3U, &word, 1)) {
      return sendText(client, "tang: transfer failed\n");
    }
    return sendText(client, "tang %02X = %08X\n", address & ~3U, static_cast<unsigned>(word));
  }
  if (arguments[0] != 0) {
    return sendText(client,
                    "tang [rd ADDR | wr ADDR VALUE | flash], hex; no arguments: summary\n");
  }
  uint32_t r[8] = {};
  uint32_t c[2] = {};
  if (!tang::read(tang::kRegStatus, r, 8) || !tang::read(tang::kRegCtrl, c, 2)) {
    return sendText(client, "tang: transfer failed\n");
  }
  static const char* const kSources[] = {"AUTO", "EVO", "TEST", "?"};
  return sendText(client, "tang 08..24: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                  static_cast<unsigned>(r[0]), static_cast<unsigned>(r[1]),
                  static_cast<unsigned>(r[2]), static_cast<unsigned>(r[3]),
                  static_cast<unsigned>(r[4]), static_cast<unsigned>(r[5]),
                  static_cast<unsigned>(r[6]), static_cast<unsigned>(r[7])) &&
         sendText(client,
                  "tang CTRL %08X: source %s, MUX phase %u delay %u clock %X, align %u, "
                  "SD read %u, OSD %u, aspect %s, size %s; ADJUST %08X: h %d v %d; %u kHz\n",
                  static_cast<unsigned>(c[0]), kSources[c[0] & 3],
                  static_cast<unsigned>((c[0] >> 2) & 1), static_cast<unsigned>(((c[0] >> 3) & 3) + 1),
                  static_cast<unsigned>((c[0] >> 10) & 0xF), static_cast<unsigned>((c[0] >> 5) & 1),
                  static_cast<unsigned>((c[0] >> 6) & 3), static_cast<unsigned>((c[0] >> 8) & 1),
                  ((c[0] >> 14) & 1) ? "16:9" : "4:3",
                  ((c[0] >> 15) & 3) == 1 ? "95%" : ((c[0] >> 15) & 3) == 2 ? "90%" : "100%",
                  static_cast<unsigned>(c[1]), static_cast<int>(static_cast<int8_t>(c[1] & 0xFF)),
                  static_cast<int>(static_cast<int8_t>((c[1] >> 8) & 0xFF)),
                  static_cast<unsigned>(tang::clockHz() / 1000));
}

bool runCommand(int client, const char* command) {
  if (strcmp(command, "mem") == 0) {
    return sendText(client,
                    "internal %u free, %u min, %u largest; PSRAM %u free, %u min, %u largest\n",
                    static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                    static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
                    static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                    static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                    static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)),
                    static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
  }
  if (strcmp(command, "ver") == 0) {
    return sendText(client, "%s, up %u s, %u tasks\n", firmwareName,
                    static_cast<unsigned>(millis() / 1000),
                    static_cast<unsigned>(uxTaskGetNumberOfTasks()));
  }
  if (strcmp(command, "log") == 0) {
    return sendStoredLog(client);
  }
  if (strncmp(command, "tang", 4) == 0 && (command[4] == 0 || command[4] == ' ')) {
    return runTang(client, command + 4);
  }
  if (command[0] != 0) {
    return sendText(client,
                    "commands: help, mem, ver, log (stored log from flash), "
                    "tang [rd ADDR | wr ADDR VALUE] (Tang registers over LINK)\n");
  }
  return true;
}

// Один клиент: история, потом новое; команды — строками.
void serve(int client) {
  timeval timeout = {2, 0};
  setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  const int noDelay = 1;
  setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
  if (!sendText(client, "VDAC4 console, %s. Commands: help, mem, ver, log, tang\n",
                firmwareName)) {
    return;
  }
  uint32_t position = ringHead > kRingBytes ? ringHead - kRingBytes : 0;
  char command[48];
  size_t commandLength = 0;
  uint8_t chunk[512];
  for (;;) {
    const uint32_t head = ringHead;
    if (head - position > kRingBytes) {
      const uint32_t lost = head - position - kRingBytes;
      position = head - kRingBytes;
      if (!sendText(client, "\n[console: %u bytes lost]\n", static_cast<unsigned>(lost))) {
        return;
      }
    }
    while (position != head) {
      size_t count = 0;
      while (position + count != head && count < sizeof(chunk)) {
        chunk[count] = ring[(position + count) % kRingBytes];
        ++count;
      }
      if (!sendAll(client, chunk, count)) {
        return;
      }
      position += static_cast<uint32_t>(count);
    }
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(client, &readable);
    timeval wait = {0, 100000};
    const int ready = select(client + 1, &readable, nullptr, nullptr, &wait);
    if (ready < 0) {
      return;
    }
    if (ready == 0) {
      continue;
    }
    char input[64];
    const int got = recv(client, input, sizeof(input), 0);
    if (got <= 0) {
      return;
    }
    for (int index = 0; index < got; ++index) {
      const char c = input[index];
      if (c == '\r' || c == '\n') {
        command[commandLength] = 0;
        commandLength = 0;
        if (!runCommand(client, command)) {
          return;
        }
      } else if (commandLength + 1 < sizeof(command)) {
        command[commandLength++] = c;
      }
    }
  }
}

void consoleTask(void*) {
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  int server = -1;
  for (;;) {
    if (server < 0) {
      server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (server < 0) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }
      const int reuse = 1;
      setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
      sockaddr_in address = {};
      address.sin_family = AF_INET;
      address.sin_port = htons(kPort);
      address.sin_addr.s_addr = htonl(INADDR_ANY);
      if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
          listen(server, 1) != 0) {
        close(server);
        server = -1;
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }
      ESP_LOGI("zifi-console", "listening on port %u", static_cast<unsigned>(kPort));
    }
    const int client = accept(server, nullptr, nullptr);
    if (client < 0) {
      close(server);
      server = -1;
      continue;
    }
    serve(client);
    shutdown(client, SHUT_RDWR);
    close(client);
  }
}

}  // namespace

void netConsoleBegin(const char* firmware) {
  if (ring != nullptr) {
    return;
  }
  firmwareName = firmware != nullptr ? firmware : "";
  ringLock = xSemaphoreCreateMutex();
  ring = static_cast<uint8_t*>(heap_caps_malloc(kRingBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (ring == nullptr || ringLock == nullptr) {
    ring = nullptr;
    return;
  }
  previousVprintf = esp_log_set_vprintf(consoleVprintf);
  xTaskCreatePinnedToCore(consoleTask, "zifi-console", kTaskStack, nullptr, kTaskPriority,
                          nullptr, kTaskCore);
}

}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
