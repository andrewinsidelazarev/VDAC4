#include "zifi/vdac4_board_esp.hpp"

#if ZIFI_HAS_SPI_LINK

#include <Arduino.h>
#include <errno.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <new>

#include "zifi/bl616_push.hpp"
#include "zifi/bringup_log.hpp"
#include "zifi/net_client.hpp"
#include "zifi/online_updater.hpp"
#include "zifi/sha256.hpp"
#include "zifi/tang_flash.hpp"
#include "zifi/tang_link.hpp"
#include "zifi/tang_sd.hpp"

namespace zifi {
namespace {

constexpr char kTag[] = "zifi-vup";
constexpr size_t kChunk = 4096;
constexpr uint32_t kRegCpInfo = 0x30;          // CP_INFO: {C0, версия BL616, …}
// Карту держит загрузка ядра (десятки секунд): столько и ждём, дальше — «занята».
constexpr uint32_t kSdWaitMs = 60000;
// Заголовок битстрима GW2AR-18: команда 0x06 «проверить IDCODE» и сам IDCODE (как служба 8268)
constexpr uint8_t kIdcodeCheck[] = {0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x1B};

uint8_t* allocate(size_t bytes) {
  return static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

// Слово синхронизации A5C3 и проверка IDCODE GW2AR-18 в первых 64 байтах: битстрим этой ПЛИС.
const char* checkBitstream(const uint8_t* image, size_t size) {
  if (size < 64) {
    return "image too small";
  }
  bool sync = false;
  bool idcode = false;
  for (size_t i = 0; i + 1 < 64; ++i) {
    sync = sync || (image[i] == 0xA5 && image[i + 1] == 0xC3);
    idcode = idcode || (i + sizeof(kIdcodeCheck) <= 64 &&
                        memcmp(image + i, kIdcodeCheck, sizeof(kIdcodeCheck)) == 0);
  }
  if (!sync) {
    return "no Gowin sync word A5C3";
  }
  if (!idcode) {
    return "not a GW2AR-18 bitstream";
  }
  return nullptr;
}

// --- ход ESP: проценты OnlineUpdater -> ход обновлятора ----------------------------------
struct EspProgress {
  VupProgress progress;
  void* context;
};

bool espProgress(void* context, uint8_t stage, uint8_t percent) {
  const EspProgress* p = static_cast<const EspProgress*>(context);
  if (p->progress != nullptr && stage == kOnlineUpdateFirmware) {
    p->progress(p->context, percent, 100);
  }
  return true;
}

// --- BL616: LINK и время FreeRTOS (как служба 8268) ----------------------------------------
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

// --- microSD Tang --------------------------------------------------------------------------
// Путь от корня карты: буквы, цифры, «_ - .» и «/» между частями, без «..» (как SDPUT).
bool sdPathOk(const char* path) {
  const size_t length = strlen(path);
  if (length == 0 || length > 80 || path[0] == '/' || path[length - 1] == '/' ||
      strstr(path, "..") != nullptr || strstr(path, "//") != nullptr) {
    return false;
  }
  for (const char* p = path; *p != 0; ++p) {
    const char c = *p;
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '_' || c == '-' || c == '.' || c == '/')) {
      return false;
    }
  }
  return true;
}

bool sdFull(const char* path, char* full, size_t size) {
  if (!sdPathOk(path)) {
    return false;
  }
  const int written = snprintf(full, size, "%s/%s", tang_sd::kMountPoint, path);
  return written > 0 && static_cast<size_t>(written) < size;
}

bool sdMakeParents(char* full) {
  for (char* p = full + strlen(tang_sd::kMountPoint) + 1; *p != 0; ++p) {
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

// Файл ровно size байтов и совпадает с data.
bool sdSame(const char* full, const uint8_t* data, uint32_t size, uint8_t* chunk) {
  FILE* f = fopen(full, "rb");
  if (f == nullptr) {
    return false;
  }
  bool ok = true;
  for (uint32_t done = 0; ok && done < size;) {
    const size_t want = (size - done) < kChunk ? (size - done) : kChunk;
    ok = fread(chunk, 1, want, f) == want && memcmp(chunk, data + done, want) == 0;
    done += want;
  }
  ok = ok && fgetc(f) == EOF;
  fclose(f);
  return ok;
}

}  // namespace

// --- ESP -------------------------------------------------------------------------------------

const char* Vdac4BoardEsp::espVersion() {
  return ZIFI_BUILD_VERSION;
}

const char* Vdac4BoardEsp::releaseFormat() {
  return board::kReleaseVersionFormat;
}

bool Vdac4BoardEsp::espImageSha256(uint32_t size, uint8_t digest[32]) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running == nullptr || size == 0 || size > running->size) {
    return false;
  }
  uint8_t* chunk = allocate(kChunk);
  if (chunk == nullptr) {
    return false;
  }
  Sha256 sha;
  bool ok = sha.begin();
  for (uint32_t done = 0; ok && done < size;) {
    const uint32_t part = (size - done) < kChunk ? (size - done) : kChunk;
    ok = esp_partition_read(running, done, chunk, part) == ESP_OK && sha.update(chunk, part);
    done += part;
  }
  heap_caps_free(chunk);
  return ok && sha.finish(digest);
}

bool Vdac4BoardEsp::espInstall(const char* rawPath, const uint8_t sha256[32],
                               VupProgress progress, void* context, char* error,
                               size_t errorSize) {
  // Свой клиент TLS: сетевой клиент ядра (NET_*) сеанс обновления не трогает.
  NetClient* client = new (std::nothrow) NetClient();
  uint8_t* scratch = allocate(kChunk);
  if (client == nullptr || scratch == nullptr) {
    delete client;
    heap_caps_free(scratch);
    snprintf(error, errorSize, "no memory");
    return false;
  }
  OnlineUpdater updater(*client);
  EspProgress hook = {progress, context};
  uint8_t digest[32];
  const bool ok = updater.installImage(rawPath, scratch, kChunk, sha256, digest, espProgress,
                                       &hook, error, errorSize);
  client->close();
  delete client;
  heap_caps_free(scratch);
  bringupLog(kTag, "ESP firmware: %s", ok ? "written to the OTA partition and verified" : error);
  return ok;
}

void Vdac4BoardEsp::espRestart() {
  ESP.restart();
}

// --- Tang ------------------------------------------------------------------------------------

bool Vdac4BoardEsp::tangVersion(uint32_t& version) {
  version = tang::ready() ? tang::coreVersion() : 0;
  return version != 0;
}

bool Vdac4BoardEsp::bl616Info(uint32_t& info) {
  info = 0;
  return tang::ready() && tang::read(kRegCpInfo, &info, 1);
}

const char* Vdac4BoardEsp::flashReady() {
  if (!tang::ready()) {
    return "LINK to Tang is down";
  }
  if ((tang::coreVersion() >> 24) != tang::kPersonalityTs) {
    return "Tang does not run TS-Config (slot 0)";
  }
  if (!tang_flash::available()) {
    return "Tang flash bridge does not answer";
  }
  return nullptr;
}

bool Vdac4BoardEsp::flashRead(uint32_t address, uint8_t* data, size_t length) {
  return tang_flash::read(address, data, length);
}

bool Vdac4BoardEsp::flashWriteSlot(uint32_t base, const uint8_t* image, size_t size,
                                   VupProgress progress, void* context, char* error,
                                   size_t errorSize) {
  if (base % tang::kSlotBytes != 0 || size == 0 || size > tang::kSlotBytes ||
      base + size > tang_flash::kSettingsBlock) {
    snprintf(error, errorSize, "slot %06lX: bad size", static_cast<unsigned long>(base));
    return false;
  }
  const char* problem = checkBitstream(image, size);
  if (problem != nullptr) {
    snprintf(error, errorSize, "%s", problem);
    return false;
  }
  uint8_t* back = allocate(kChunk);
  if (back == nullptr) {
    snprintf(error, errorSize, "no memory");
    return false;
  }
  bringupLog(kTag, "Tang slot %06lX: writing %u bytes", static_cast<unsigned long>(base),
             static_cast<unsigned>(size));
  // Пока слот переписывается, TS-Config не должен уйти в него по обмену с FT812 (как BEGIN)
  if (base != 0) {
    tang::disarmBoot();
  }
  // ход: стирание — четверть, запись — половина, сверка — четверть
  const uint32_t total = static_cast<uint32_t>(size);
  bool ok = true;
  const uint32_t blocks = (size + tang_flash::kBlockBytes - 1) / tang_flash::kBlockBytes;
  for (uint32_t block = 0; ok && block < blocks; ++block) {
    ok = tang_flash::eraseBlock(base + block * tang_flash::kBlockBytes);
    if (!ok) {
      snprintf(error, errorSize, "erase at %06lX failed",
               static_cast<unsigned long>(base + block * tang_flash::kBlockBytes));
    } else if (progress != nullptr) {
      progress(context, static_cast<uint32_t>(static_cast<uint64_t>(total) * (block + 1) / blocks / 4),
               total);
    }
  }
  for (size_t done = 0; ok && done < size;) {
    const size_t part = (size - done) < kChunk ? (size - done) : kChunk;
    ok = tang_flash::program(base + done, image + done, part);
    if (!ok) {
      snprintf(error, errorSize, "write at %06lX failed", static_cast<unsigned long>(base + done));
    }
    done += part;
    if (ok && progress != nullptr) {
      progress(context, total / 4 + static_cast<uint32_t>(done / 2), total);
    }
  }
  for (size_t done = 0; ok && done < size;) {
    const size_t part = (size - done) < kChunk ? (size - done) : kChunk;
    if (!tang_flash::read(base + done, back, part)) {
      snprintf(error, errorSize, "read back at %06lX failed", static_cast<unsigned long>(base + done));
      ok = false;
    } else if (memcmp(back, image + done, part) != 0) {
      size_t at = 0;
      while (at < part && back[at] == image[done + at]) {
        ++at;
      }
      snprintf(error, errorSize, "verify: flash differs at %06lX",
               static_cast<unsigned long>(base + done + at));
      ok = false;
    }
    done += part;
    if (ok && progress != nullptr) {
      progress(context, total * 3 / 4 + static_cast<uint32_t>(done / 4), total);
    }
  }
  if (base != 0) {
    tang::rearmBoot();   // разрешит, только если в слоте снова битстрим
  }
  heap_caps_free(back);
  bringupLog(kTag, "Tang slot %06lX: %s", static_cast<unsigned long>(base),
             ok ? (base == 0 ? "written and verified; loads at next power-up"
                             : "written and verified")
                : error);
  return ok;
}

bool Vdac4BoardEsp::bl616Push(const uint8_t* image, size_t size, uint32_t& newInfo, char* error,
                              size_t errorSize) {
  newInfo = 0;
  if (!tang::ready()) {
    snprintf(error, errorSize, "LINK to Tang is down");
    return false;
  }
  const char* problem = bl616::checkImage(image, size);
  if (problem != nullptr) {
    snprintf(error, errorSize, "%s", problem);
    return false;
  }
  uint8_t digest[32];
  Sha256 sha;
  if (!sha.begin() || !sha.update(image, size) || !sha.finish(digest)) {
    snprintf(error, errorSize, "sha256");
    return false;
  }
  bringupLog(kTag, "BL616: sending %u bytes", static_cast<unsigned>(size));
  const bl616::PushIo io = {nullptr, pushRead, pushWrite, pushSleep, pushNow, nullptr};
  tang_sd::Session session;            // ящик BL616 — один на обновление и кэш ядер
  problem = bl616::push(io, image, size, digest, static_cast<uint32_t>(esp_random()), &newInfo);
  if (problem != nullptr) {
    snprintf(error, errorSize, "%s", problem);
    bringupLog(kTag, "BL616 FAILED: %s", problem);
    return false;
  }
  bringupLog(kTag, "BL616: written, new firmware runs (CP_INFO %08lX)",
             static_cast<unsigned long>(newInfo));
  return true;
}

// --- microSD Tang ----------------------------------------------------------------------------

const char* Vdac4BoardEsp::tangSdReady() {
  if (!tang::ready()) {
    return "LINK to Tang is down";
  }
  if (tang_sd::heldByWc()) {
    return "Tang SD is held by WC (SD2)";
  }
  tang_sd::Session session(kSdWaitMs);
  if (!session.ok()) {
    return "Tang SD is busy";
  }
  return tang_sd::mount();
}

bool Vdac4BoardEsp::tangSdStat(const char* path, bool& exists, uint32_t& size) {
  exists = false;
  size = 0;
  char full[128];
  if (!sdFull(path, full, sizeof(full))) {
    return false;
  }
  tang_sd::Session session(kSdWaitMs);
  if (!session.ok() || tang_sd::mount() != nullptr) {
    return false;
  }
  struct stat st = {};
  if (stat(full, &st) != 0) {
    return errno == ENOENT;            // нет файла — не ошибка; прочий отказ — ошибка
  }
  if (!S_ISREG(st.st_mode)) {
    return false;
  }
  exists = true;
  size = static_cast<uint32_t>(st.st_size);
  return true;
}

bool Vdac4BoardEsp::tangSdRead(const char* path, uint8_t* data, uint32_t size) {
  char full[128];
  if (!sdFull(path, full, sizeof(full))) {
    return false;
  }
  tang_sd::Session session(kSdWaitMs);
  if (!session.ok() || tang_sd::mount() != nullptr) {
    return false;
  }
  FILE* f = fopen(full, "rb");
  if (f == nullptr) {
    return false;
  }
  bool ok = true;
  for (uint32_t done = 0; ok && done < size;) {
    const size_t want = (size - done) < kChunk ? (size - done) : kChunk;
    ok = fread(data + done, 1, want, f) == want;
    done += want;
  }
  ok = ok && fgetc(f) == EOF;
  fclose(f);
  return ok;
}

bool Vdac4BoardEsp::tangSdWrite(const char* path, const uint8_t* data, uint32_t size, char* error,
                                size_t errorSize) {
  char full[128];
  char temp[136];
  if (!sdFull(path, full, sizeof(full))) {
    snprintf(error, errorSize, "bad Tang SD path");
    return false;
  }
  snprintf(temp, sizeof(temp), "%s.tmp", full);
  uint8_t* chunk = allocate(kChunk);
  if (chunk == nullptr) {
    snprintf(error, errorSize, "no memory");
    return false;
  }
  const char* problem = nullptr;
  {
    tang_sd::Session session(kSdWaitMs);
    problem = !session.ok() ? "Tang SD is busy" : tang_sd::mount();
    if (problem == nullptr && !sdMakeParents(full)) {
      problem = "cannot create the folder";
    }
    if (problem == nullptr) {
      FILE* f = fopen(temp, "wb");
      if (f == nullptr) {
        problem = "cannot create the file";
      } else {
        for (uint32_t done = 0; done < size && problem == nullptr;) {
          const size_t want = (size - done) < kChunk ? (size - done) : kChunk;
          if (fwrite(data + done, 1, want, f) != want) {
            problem = "write failed";
          }
          done += want;
        }
        if (fclose(f) != 0 && problem == nullptr) {
          problem = "write failed (close)";
        }
      }
    }
    if (problem == nullptr && !sdSame(temp, data, size, chunk)) {
      problem = "read-back does not match";
    }
    if (problem == nullptr) {
      remove(full);                     // FatFs не переименовывает поверх прежнего
      if (rename(temp, full) != 0) {
        problem = "rename failed";
      }
    }
    if (problem == nullptr && !sdSame(full, data, size, chunk)) {
      problem = "file differs after rename";
    }
    if (problem != nullptr) {
      remove(temp);
    }
  }
  heap_caps_free(chunk);
  bringupLog(kTag, "Tang SD %s: %u bytes: %s", path, static_cast<unsigned>(size),
             problem != nullptr ? problem : "written and read back");
  if (problem != nullptr) {
    snprintf(error, errorSize, "Tang SD: %s", problem);
    return false;
  }
  return true;
}

}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
