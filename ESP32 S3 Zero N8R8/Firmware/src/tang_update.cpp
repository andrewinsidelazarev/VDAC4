#include "zifi/tang_update.hpp"

#if ZIFI_HAS_SPI_LINK

#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "zifi/bringup_log.hpp"
#include "zifi/sha256.hpp"
#include "zifi/tang_flash.hpp"
#include "zifi/tang_link.hpp"

namespace zifi {
namespace {

constexpr char kTag[] = "zifi-tang";
constexpr uint16_t kPort = 8268;
constexpr uint32_t kMaxImage = tang_flash::kSettingsBlock;
constexpr size_t kChunk = 4096;
constexpr uint32_t kTaskStack = 6144;
constexpr UBaseType_t kTaskPriority = 1;
constexpr BaseType_t kTaskCore = 0;
// Заголовок битстрима GW2AR-18: команда 0x06 «проверить IDCODE» и сам IDCODE
constexpr uint8_t kIdcodeCheck[] = {0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x1B};

bool sendLine(int client, const char* format, ...) {
  char text[160];
  va_list arguments;
  va_start(arguments, format);
  const int length = vsnprintf(text, sizeof(text) - 1, format, arguments);
  va_end(arguments);
  if (length <= 0) {
    return false;
  }
  size_t size = static_cast<size_t>(length) < sizeof(text) - 1 ? static_cast<size_t>(length)
                                                                : sizeof(text) - 2;
  text[size++] = '\n';
  const char* data = text;
  while (size > 0) {
    const int sent = send(client, data, size, 0);
    if (sent <= 0) {
      return false;
    }
    data += sent;
    size -= static_cast<size_t>(sent);
  }
  return true;
}

bool receiveLine(int client, char* line, size_t size) {
  size_t length = 0;
  for (;;) {
    char c = 0;
    const int got = recv(client, &c, 1, 0);
    if (got <= 0) {
      return false;
    }
    if (c == '\n') {
      break;
    }
    if (c != '\r' && length + 1 < size) {
      line[length++] = c;
    }
  }
  line[length] = 0;
  return true;
}

bool parseHex(const char* text, uint8_t digest[32]) {
  if (strlen(text) != 64) {
    return false;
  }
  for (int i = 0; i < 32; ++i) {
    unsigned value = 0;
    if (sscanf(text + 2 * i, "%2x", &value) != 1) {
      return false;
    }
    digest[i] = static_cast<uint8_t>(value);
  }
  return true;
}

void toHex(const uint8_t digest[32], char* text) {
  for (int i = 0; i < 32; ++i) {
    snprintf(text + 2 * i, 3, "%02x", digest[i]);
  }
}

// Заголовок образа: слово синхронизации и IDCODE GW2AR-18 в первых 64 байтах.
const char* checkImage(const uint8_t* image, size_t size) {
  if (size < 64) {
    return "image too small";
  }
  bool sync = false;
  bool idcode = false;
  for (size_t i = 0; i + 1 < 64; ++i) {
    if (image[i] == 0xA5 && image[i + 1] == 0xC3) {
      sync = true;
    }
    if (i + sizeof(kIdcodeCheck) <= 64 && memcmp(image + i, kIdcodeCheck, sizeof(kIdcodeCheck)) == 0) {
      idcode = true;
    }
  }
  if (!sync) {
    return "no Gowin sync word A5C3 in the header";
  }
  if (!idcode) {
    return "the header does not check IDCODE 0x0000081B (not a GW2AR-18 bitstream)";
  }
  return nullptr;
}

bool writeImage(int client, const uint8_t* image, size_t size) {
  const uint32_t blocks = (size + tang_flash::kBlockBytes - 1) / tang_flash::kBlockBytes;
  for (uint32_t block = 0; block < blocks; ++block) {
    if (!tang_flash::eraseBlock(block * tang_flash::kBlockBytes)) {
      sendLine(client, "ERROR erase of block %u failed", static_cast<unsigned>(block));
      return false;
    }
    sendLine(client, "PROGRESS erase %u %u", static_cast<unsigned>(block + 1),
             static_cast<unsigned>(blocks));
  }
  for (size_t done = 0; done < size;) {
    const size_t part = (size - done) < kChunk ? (size - done) : kChunk;
    if (!tang_flash::program(done, image + done, part)) {
      sendLine(client, "ERROR write at %06X failed", static_cast<unsigned>(done));
      return false;
    }
    done += part;
    if (done % (16 * kChunk) == 0 || done == size) {
      sendLine(client, "PROGRESS write %u %u", static_cast<unsigned>(done),
               static_cast<unsigned>(size));
    }
  }
  static uint8_t back[kChunk];
  for (size_t done = 0; done < size;) {
    const size_t part = (size - done) < kChunk ? (size - done) : kChunk;
    if (!tang_flash::read(done, back, part)) {
      sendLine(client, "ERROR read back at %06X failed", static_cast<unsigned>(done));
      return false;
    }
    if (memcmp(back, image + done, part) != 0) {
      size_t at = 0;
      while (at < part && back[at] == image[done + at]) {
        ++at;
      }
      sendLine(client, "ERROR verify: flash differs at %06X", static_cast<unsigned>(done + at));
      return false;
    }
    done += part;
    if (done % (16 * kChunk) == 0 || done == size) {
      sendLine(client, "PROGRESS verify %u %u", static_cast<unsigned>(done),
               static_cast<unsigned>(size));
    }
  }
  return true;
}

void serve(int client) {
  timeval timeout = {30, 0};
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  if (!sendLine(client, "ZIFI-TANG/1 %s %u", ZIFI_BUILD_VERSION, static_cast<unsigned>(kMaxImage))) {
    return;
  }
  char line[160];
  if (!receiveLine(client, line, sizeof(line))) {
    return;
  }
  unsigned size = 0;
  char hex[80] = {};
  uint8_t expected[32];
  if (sscanf(line, "BEGIN %u %79s", &size, hex) != 2 || !parseHex(hex, expected)) {
    sendLine(client, "ERROR expected: BEGIN <size> <sha256>");
    return;
  }
  if (!tang_flash::available()) {
    sendLine(client, "ERROR Tang flash is not available (LINK down or core older than 1.1)");
    return;
  }
  if (size == 0 || size > kMaxImage) {
    sendLine(client, "ERROR size %u: must be 1..%u", size, static_cast<unsigned>(kMaxImage));
    return;
  }
  uint8_t* image = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (image == nullptr) {
    sendLine(client, "ERROR no memory for %u bytes", size);
    return;
  }
  Sha256 sha;
  bool ok = sha.begin() && sendLine(client, "READY %u", static_cast<unsigned>(kChunk));
  size_t received = 0;
  while (ok && received < size) {
    const size_t want = (size - received) < kChunk ? (size - received) : kChunk;
    const int got = recv(client, image + received, want, 0);
    if (got <= 0) {
      ok = false;
      break;
    }
    ok = sha.update(image + received, static_cast<size_t>(got));
    received += static_cast<size_t>(got);
  }
  uint8_t digest[32];
  char digestHex[65] = {};
  if (ok && sha.finish(digest)) {
    toHex(digest, digestHex);
    if (memcmp(digest, expected, sizeof(digest)) != 0) {
      sendLine(client, "ERROR sha256 mismatch: received %s", digestHex);
      ok = false;
    }
  } else {
    ok = false;
  }
  const char* problem = ok ? checkImage(image, size) : nullptr;
  if (problem != nullptr) {
    sendLine(client, "ERROR %s", problem);
    ok = false;
  }
  if (ok) {
    bringupLog(kTag, "Tang update: writing %u bytes, sha256 %s", size, digestHex);
    ok = writeImage(client, image, size);
    if (ok) {
      bringupLog(kTag, "Tang update: written and verified; the new core loads at next power-up");
      sendLine(client, "OK %s", digestHex);
    } else {
      bringupLog(kTag, "Tang update FAILED: the Tang flash may hold a broken core");
    }
  }
  heap_caps_free(image);
}

void updateTask(void*) {
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

void tangUpdateBegin() {
  static bool started = false;
  if (started) {
    return;
  }
  started = true;
  xTaskCreatePinnedToCore(updateTask, "zifi-tangupd", kTaskStack, nullptr, kTaskPriority, nullptr,
                          kTaskCore);
}

}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
