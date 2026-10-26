#include "zifi/tang_update.hpp"

#if ZIFI_HAS_SPI_LINK

#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "zifi/bl616_push.hpp"
#include "zifi/bringup_log.hpp"
#include "zifi/core_cache_client.hpp"
#include "zifi/core_store.hpp"
#include "zifi/ft_host.hpp"
#include "zifi/ft_rom.hpp"
#include "zifi/sha256.hpp"
#include "zifi/tang_flash.hpp"
#include "zifi/tang_link.hpp"
#include "zifi/tang_sd.hpp"

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

// Адрес перехода multiboot из заголовка (команда D2 00 FF FF + 4 байта адреса, UG290):
// откуда ПЛИС грузится по RECONFIG_N; -1 — команды нет.
int32_t jumpAddress(const uint8_t* image, size_t size) {
  const size_t limit = size < 128 ? size : 128;
  for (size_t i = 0; i + 8 <= limit; ++i) {
    if (image[i] == 0xD2 && image[i + 1] == 0x00 && image[i + 2] == 0xFF && image[i + 3] == 0xFF) {
      return static_cast<int32_t>((static_cast<uint32_t>(image[i + 4]) << 24) |
                                  (static_cast<uint32_t>(image[i + 5]) << 16) |
                                  (static_cast<uint32_t>(image[i + 6]) << 8) | image[i + 7]);
    }
  }
  return -1;
}

bool writeImage(int client, const uint8_t* image, size_t size, uint32_t base) {
  const uint32_t blocks = (size + tang_flash::kBlockBytes - 1) / tang_flash::kBlockBytes;
  for (uint32_t block = 0; block < blocks; ++block) {
    if (!tang_flash::eraseBlock(base + block * tang_flash::kBlockBytes)) {
      sendLine(client, "ERROR erase of block %u failed", static_cast<unsigned>(block));
      return false;
    }
    sendLine(client, "PROGRESS erase %u %u", static_cast<unsigned>(block + 1),
             static_cast<unsigned>(blocks));
  }
  for (size_t done = 0; done < size;) {
    const size_t part = (size - done) < kChunk ? (size - done) : kChunk;
    if (!tang_flash::program(base + done, image + done, part)) {
      sendLine(client, "ERROR write at %06X failed", static_cast<unsigned>(base + done));
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
    if (!tang_flash::read(base + done, back, part)) {
      sendLine(client, "ERROR read back at %06X failed", static_cast<unsigned>(base + done));
      return false;
    }
    if (memcmp(back, image + done, part) != 0) {
      size_t at = 0;
      while (at < part && back[at] == image[done + at]) {
        ++at;
      }
      sendLine(client, "ERROR verify: flash differs at %06X", static_cast<unsigned>(base + done + at));
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

// --- BL616: образ через почтовый ящик ядра Tang (bl616_push.cpp) ----------------------------
bool pushRead(void*, uint32_t address, uint32_t* words, size_t count) {
  return tang::read(address, words, count);
}

bool pushWrite(void*, uint32_t address, const uint32_t* words, size_t count) {
  return tang::write(address, words, count);
}

void pushSleep(void*, uint32_t ms) {
  const TickType_t ticks = pdMS_TO_TICKS(ms);
  vTaskDelay(ticks > 0 ? ticks : 1);
}

uint32_t pushNow(void*) {
  return static_cast<uint32_t>(millis());
}

void pushProgress(void* ctx, const char* stage, uint32_t done, uint32_t total) {
  sendLine(*static_cast<int*>(ctx), "PROGRESS %s %u %u", stage, static_cast<unsigned>(done),
           static_cast<unsigned>(total));
}

// «BL616 <размер> <sha256>»: принять образ, проверить и передать BL616; он пишет его во flash
// и перезагружается — ответ OK приходит, когда новая прошивка уже работает.
void serveBl616(int client, const char* line) {
  unsigned size = 0;
  char hex[80] = {};
  uint8_t expected[32];
  if (sscanf(line, "BL616 %u %79s", &size, hex) != 2 || !parseHex(hex, expected)) {
    sendLine(client, "ERROR expected: BL616 <size> <sha256>");
    return;
  }
  if (!tang::ready()) {
    sendLine(client, "ERROR LINK to Tang is down");
    return;
  }
  if (size == 0 || size > bl616::kMaxImage) {
    sendLine(client, "ERROR size %u: must be 1..%u", size, static_cast<unsigned>(bl616::kMaxImage));
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
  const char* problem = ok ? bl616::checkImage(image, size) : nullptr;
  if (problem != nullptr) {
    sendLine(client, "ERROR %s", problem);
    ok = false;
  }
  if (ok) {
    bringupLog(kTag, "BL616 update: sending %u bytes, sha256 %s", size, digestHex);
    int ctx = client;
    const bl616::PushIo io = {&ctx, pushRead, pushWrite, pushSleep, pushNow, pushProgress};
    uint32_t info = 0;
    tang_sd::Session session;          // ящик BL616 — один на обновление и кэш ядер
    problem = bl616::push(io, image, size, digest, static_cast<uint32_t>(esp_random()), &info);
    if (problem == nullptr) {
      bringupLog(kTag, "BL616 update: written, new firmware runs (CP_INFO %08X)", static_cast<unsigned>(info));
      sendLine(client, "OK %s %08X", digestHex, static_cast<unsigned>(info));
    } else {
      bringupLog(kTag, "BL616 update FAILED: %s", problem);
      sendLine(client, "ERROR %s", problem);
    }
  }
  heap_caps_free(image);
}

// --- Кэш ядер ПЛИС во flash BL616 (core_cache_client.cpp, прошивка BL616 14+) ----------------
struct MemImage {
  const uint8_t* data;
  uint32_t size;
};

bool memRead(void* ctx, uint32_t offset, uint8_t* data, uint32_t size) {
  const MemImage* m = static_cast<const MemImage*>(ctx);
  if (offset > m->size || size > m->size - offset) {
    return false;
  }
  memcpy(data, m->data + offset, size);
  return true;
}

constexpr uint32_t kMaxCore = 2 * 1024 * 1024;

// «COREINFO»: объём flash BL616 (0 — кэша нет, 255 — нужен FORMAT).
// «COREPUT <имя> <размер> <sha256>»: принять ядро (двоичный битстрим Gowin), проверить и записать
// в кэш. «CORELOAD <имя>»: загрузить ядро из кэша в ПЛИС и дождаться его на LINK. «COREFORMAT»:
// пустой каталог кэша (только по слову пользователя: данные на месте кэша пропадут).
void serveCore(int client, const char* line) {
  if (!tang::ready()) {
    sendLine(client, "ERROR LINK to Tang is down");
    return;
  }
  int ctx = client;
  const bl616::PushIo io = {&ctx, pushRead, pushWrite, pushSleep, pushNow, pushProgress};
  core_cache::Client cache(io, static_cast<uint32_t>(esp_random()));
  tang_sd::Session session;            // ящик и смена ядра — одна операция за раз
  if (strcmp(line, "COREFORMAT") == 0) {
    // пустой каталог кэша (на месте кэша — чужие данные, INFO 255): только по слову пользователя
    const char* problem = cache.format();
    if (problem != nullptr) {
      sendLine(client, "ERROR %s", problem);
    } else {
      bringupLog(kTag, "core cache: formatted");
      sendLine(client, "OK");
    }
    return;
  }
  if (strcmp(line, "COREINFO") == 0) {
    uint32_t mb = 0;
    const char* problem = cache.info(&mb);
    if (problem != nullptr) {
      sendLine(client, "ERROR %s", problem);
    } else {
      sendLine(client, "OK %u", static_cast<unsigned>(mb));
    }
    return;
  }
  char name[24] = {};
  if (sscanf(line, "CORELOAD %23s", name) == 1) {
    // Тем же путём, что слой ядра (core_store::loadCached): том SD снимается, удача — только ядро из
    // кэша на LINK. Ядро ведёт консоль (хозяин — она, до перезагрузки ПЛИС или ESP): назад — BOOT.
    sendLine(client, "PROGRESS load 0 1");
    tang::claimCore();
    uint32_t version = 0;
    const char* problem = core_store::loadCached(name, &version);
    if (problem != nullptr) {
      tang::releaseCore();
      sendLine(client, "ERROR %s", problem);
    } else {
      sendLine(client, "OK %08X", static_cast<unsigned>(version));
    }
    return;
  }
  unsigned size = 0;
  char hex[80] = {};
  uint8_t expected[32];
  if (sscanf(line, "COREPUT %23s %u %79s", name, &size, hex) != 3 || !parseHex(hex, expected)) {
    sendLine(client, "ERROR expected: COREINFO | COREPUT <name> <size> <sha256> | CORELOAD <name> | COREFORMAT");
    return;
  }
  if (size < 4096 || size > kMaxCore) {
    sendLine(client, "ERROR size %u: must be 4 KB..%u", size, static_cast<unsigned>(kMaxCore));
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
  if (ok) {
    bringupLog(kTag, "core %s: %u bytes to the BL616 cache, sha256 %s", name, size, digestHex);
    MemImage mem = {image, size};
    const core_cache::ImageReader reader = {&mem, memRead};
    const char* problem = cache.put(name, size, digest, reader);
    if (problem == nullptr) {
      sendLine(client, "OK %s", digestHex);
    } else {
      bringupLog(kTag, "core %s: FAILED: %s", name, problem);
      sendLine(client, "ERROR %s", problem);
    }
  }
  heap_caps_free(image);
}

// --- microSD Tang по сети: «SDPUT <путь> <размер> <sha256>» ----------------------------------
// Файл на microSD самой Tang (мост ядра TS-Config 1.30+, tang_sd): ядра \CORES\ИМЯ.BIN и их слои
// ESP \CORES\ИМЯ.ELF без кардридера. Файл принимается целиком в PSRAM со сверкой SHA-256, пишется во
// временный «ИМЯ.tmp» рядом, читается обратно со сверкой и только тогда заменяет прежний. Путь — от
// корня карты: буквы, цифры, «_ - .» и «/» между частями, без «..».
constexpr size_t kMaxSdFile = 4U * 1024U * 1024U;

bool sdPathOk(const char* path) {
  const size_t length = strlen(path);
  if (length == 0 || length > 80 || path[0] == '/' || path[length - 1] == '/' ||
      strstr(path, "..") != nullptr || strstr(path, "//") != nullptr) {
    return false;
  }
  for (const char* p = path; *p != 0; ++p) {
    const char c = *p;
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '_' || c == '-' || c == '.' || c == '/';
    if (!ok) {
      return false;
    }
  }
  return true;
}

// Папки пути (кроме последней части — имени файла): создать недостающие
bool sdMakeParents(char* full, size_t rootLength) {
  for (char* p = full + rootLength + 1; *p != 0; ++p) {
    if (*p != '/') {
      continue;
    }
    *p = 0;
    struct stat st = {};
    const bool exists = stat(full, &st) == 0;
    const bool ok = exists ? S_ISDIR(st.st_mode) : mkdir(full, 0775) == 0;
    *p = '/';
    if (!ok) {
      return false;
    }
  }
  return true;
}

bool sdFileSha(const char* full, size_t size, uint8_t* chunk, uint8_t digest[32]) {
  FILE* f = fopen(full, "rb");
  if (f == nullptr) {
    return false;
  }
  Sha256 sha;
  bool ok = sha.begin();
  size_t done = 0;
  while (ok && done < size) {
    const size_t want = (size - done) < kChunk ? (size - done) : kChunk;
    ok = fread(chunk, 1, want, f) == want && sha.update(chunk, want);
    done += want;
  }
  ok = ok && fgetc(f) == EOF;           // файл не длиннее
  fclose(f);
  return ok && sha.finish(digest);
}

void serveSdPut(int client, const char* line) {
  char path[96] = {};
  unsigned size = 0;
  char hex[80] = {};
  uint8_t expected[32];
  if (sscanf(line, "SDPUT %95s %u %79s", path, &size, hex) != 3 || !parseHex(hex, expected) ||
      !sdPathOk(path)) {
    sendLine(client, "ERROR expected: SDPUT <path from the card root> <size> <sha256>");
    return;
  }
  if (size == 0 || size > kMaxSdFile) {
    sendLine(client, "ERROR size %u: must be 1..%u", size, static_cast<unsigned>(kMaxSdFile));
    return;
  }
  if (!tang::ready()) {
    sendLine(client, "ERROR LINK to Tang is down");
    return;
  }
  uint8_t* image = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  uint8_t* chunk = static_cast<uint8_t*>(heap_caps_malloc(kChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (image == nullptr || chunk == nullptr) {
    heap_caps_free(image);
    heap_caps_free(chunk);
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
  const char* problem = nullptr;
  if (ok) {
    tang_sd::Session session;
    problem = tang_sd::mount();
    char full[160];
    char temp[168];
    const size_t root = strlen(tang_sd::kMountPoint);
    snprintf(full, sizeof(full), "%s/%s", tang_sd::kMountPoint, path);
    snprintf(temp, sizeof(temp), "%s.tmp", full);
    if (problem == nullptr && !sdMakeParents(full, root)) {
      problem = "cannot create the folder on the Tang SD card";
    }
    if (problem == nullptr) {
      FILE* f = fopen(temp, "wb");
      if (f == nullptr) {
        problem = "cannot create the file on the Tang SD card";
      } else {
        size_t done = 0;
        while (done < size && problem == nullptr) {
          const size_t want = (size - done) < kChunk ? (size - done) : kChunk;
          if (fwrite(image + done, 1, want, f) != want) {
            problem = "write to the Tang SD card failed";
          }
          done += want;
          if ((done & 0xFFFF) == 0 || done == size) {
            sendLine(client, "PROGRESS write %u %u", static_cast<unsigned>(done), size);
          }
        }
        if (fclose(f) != 0 && problem == nullptr) {
          problem = "write to the Tang SD card failed (close)";
        }
      }
    }
    uint8_t back[32];
    if (problem == nullptr && (!sdFileSha(temp, size, chunk, back) || memcmp(back, digest, 32) != 0)) {
      problem = "read-back of the written file does not match";
    }
    if (problem == nullptr) {
      remove(full);                     // прежний файл (если был): FatFs не переименовывает поверх
      if (rename(temp, full) != 0) {
        problem = "rename on the Tang SD card failed";
      }
    }
    if (problem != nullptr) {
      remove(temp);
    }
    bringupLog(kTag, "Tang SD %s: %u bytes, sha256 %s: %s", path, size, digestHex,
               problem != nullptr ? problem : "written and read back");
  }
  heap_caps_free(image);
  heap_caps_free(chunk);
  if (!ok) {
    return;
  }
  if (problem != nullptr) {
    sendLine(client, "ERROR %s", problem);
  } else {
    sendLine(client, "OK %s", digestHex);
  }
}

bool sendAll(int client, const void* data, size_t size) {
  const char* p = static_cast<const char*>(data);
  while (size > 0) {
    const int sent = send(client, p, size, 0);
    if (sent <= 0) {
      return false;
    }
    p += sent;
    size -= static_cast<size_t>(sent);
  }
  return true;
}

// «FTREAD»: чтение памяти FT812 ядра Tang окном FT_ADDR/FT_DATA — для захвата кадра на ПК
// (tang_tool.py ftdump). Команды до «END» или разрыва:
//   R <адрес, hex> <слов>  →  «DATA <байт>» и слова LE32 (до 2 МБ; оборвался обмен LINK —
//                             соединение закрывается);
//   LINK <адрес, hex>      →  «VALUE <hex>» — регистр LINK Tang (FT_RTIME, FT_RSTAT, CP_INFO).
// Пишет только FT_ADDR; программа Evo, ведущая FT812, этого не замечает.
void serveFtRead(int client) {
  if (!ft_host::coreHasFt()) {
    sendLine(client, "ERROR no FT812 in the loaded Tang core");
    return;
  }
  sendLine(client, "READY");
  static uint32_t words[256];
  char line[80];
  while (receiveLine(client, line, sizeof(line))) {
    unsigned address = 0;
    unsigned count = 0;
    if (strcmp(line, "END") == 0) {
      return;
    }
    if (sscanf(line, "LINK %x", &address) == 1) {
      uint32_t value = 0;
      if ((address & 3U) != 0 || address >= 0x100U || !tang::read(address, &value, 1)) {
        sendLine(client, "ERROR LINK read %X", address);
        continue;
      }
      sendLine(client, "VALUE %08X", static_cast<unsigned>(value));
      continue;
    }
    if (sscanf(line, "R %x %u", &address, &count) != 2 || (address & 3U) != 0 || count == 0 ||
        count > 0x80000U || address + 4U * count > 0x400000U) {
      sendLine(client, "ERROR expected: R <address, hex, multiple of 4> <words, 1..524288> | LINK <address> | END");
      continue;
    }
    if (!sendLine(client, "DATA %u", 4U * count)) {
      return;
    }
    for (unsigned done = 0; done < count;) {
      const unsigned part = count - done < 256U ? count - done : 256U;
      if (!ft_host::readWords(address + 4U * done, words, part) || !sendAll(client, words, 4U * part)) {
        bringupLog(kTag, "FTREAD: LINK read at %06X failed", address + 4U * done);
        return;
      }
      done += part;
      // ~17 мс на кусок; без уступки ядра 1 МБ (~17 с) будил сторожевой таймер задач (0.1.78)
      if ((done & 0x3FFU) == 0) {
        vTaskDelay(1);
      }
    }
  }
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
  if (strncmp(line, "BL616 ", 6) == 0) {
    serveBl616(client, line);
    return;
  }
  if (strncmp(line, "CORE", 4) == 0) {
    serveCore(client, line);
    return;
  }
  if (strncmp(line, "SDPUT ", 6) == 0) {
    serveSdPut(client, line);
    return;
  }
  if (strcmp(line, "FTREAD") == 0) {
    serveFtRead(client);
    return;
  }
  // «BEGIN <размер> <sha256> [<адрес flash, hex>]»: без адреса — слот 0 (ядро при включении),
  // с адресом — начало слота multiboot (кратно 1 МБ; слот 1 — 0x100000, ядро FT812)
  unsigned size = 0;
  unsigned base = 0;
  char hex[80] = {};
  uint8_t expected[32];
  if (sscanf(line, "BEGIN %u %79s %x", &size, hex, &base) < 2 || !parseHex(hex, expected)) {
    sendLine(client, "ERROR expected: BEGIN <size> <sha256> [<flash address>] (Tang) or BL616 <size> <sha256>");
    return;
  }
  if (!tang_flash::available()) {
    if ((tang::coreVersion() >> 24) == tang::kPersonalityFt) {
      sendLine(client, "ERROR Tang runs the FT812 core: it has no flash bridge (1.31+) - leave the FT812 program on the Evo and retry");
    } else {
      sendLine(client, "ERROR Tang flash is not available (LINK down or core older than 1.1)");
    }
    return;
  }
  if (base % tang::kSlotBytes != 0 || base >= kMaxImage) {
    sendLine(client, "ERROR address %06X: must be a slot start, a multiple of %06X below %06X", base,
             static_cast<unsigned>(tang::kSlotBytes), static_cast<unsigned>(kMaxImage));
    return;
  }
  if (size == 0 || base + size > kMaxImage) {
    sendLine(client, "ERROR size %u: must be 1..%u", size, static_cast<unsigned>(kMaxImage - base));
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
  // Слоты 2+ — и образ шрифтов ПЗУ FT812 (VD4FONT1, ft_rom); слоты 0 и 1 — только битстрим
  const bool fonts = base >= 2 * tang::kSlotBytes && size >= 8 && memcmp(image, ft_rom::kMagic, 8) == 0;
  const char* problem = !ok ? nullptr : fonts ? ft_rom::checkImage(image, size) : checkImage(image, size);
  if (problem != nullptr) {
    sendLine(client, "ERROR %s", problem);
    ok = false;
  }
  if (ok) {
    const int32_t jump = jumpAddress(image, size);
    bringupLog(kTag, "Tang update: writing %u bytes at %06X (multiboot jump %06X), sha256 %s", size,
               base, static_cast<unsigned>(jump), digestHex);
    // Пока слот переписывается, ядро TS-Config не должно уйти в него по обмену с FT812
    if (base != 0) {
      tang::disarmBoot();
    }
    ok = writeImage(client, image, size, base);
    if (base != 0) {
      tang::rearmBoot();   // разрешит, только если в слоте снова битстрим
    }
    if (ok) {
      if (base == 0) {
        bringupLog(kTag, "Tang update: written and verified; the new core loads at next power-up");
      } else {
        bringupLog(kTag, "Tang update: slot %06X written and verified; it loads on BOOT",
                   base);
      }
      // новые шрифты ПЗУ — сразу и в копию ESP (ядро FT812 берёт их из неё)
      if (fonts && base == 2 * tang::kSlotBytes) {
        ft_rom::adopt(image, size);
      }
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
