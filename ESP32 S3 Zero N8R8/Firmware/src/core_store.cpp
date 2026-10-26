#include "zifi/core_store.hpp"

#if ZIFI_HAS_SPI_LINK

#include <Arduino.h>
#include <dirent.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "zifi/bringup_log.hpp"
#include "zifi/sha256.hpp"
#include "zifi/tang_link.hpp"
#include "zifi/tang_sd.hpp"

namespace zifi {
namespace core_store {
namespace {

constexpr const char* kTag = "cores";
constexpr uint32_t kSwitchMs = 15000;   // LOAD: новое ядро на LINK
constexpr uint32_t kRegCpInfo = 0x30;   // CP_INFO: ответ BL616 {0xC2, состояние, номер команды}
constexpr uint32_t kCcInfoMagic = 0xC2;
constexpr uint32_t kCcLoadFail = 0x20;  // LOAD_FAIL | код core_jtag (core_mailbox.h)
constexpr size_t kReadChunk = 8192;

bool linkRead(void*, uint32_t address, uint32_t* words, size_t count) { return tang::read(address, words, count); }
bool linkWrite(void*, uint32_t address, const uint32_t* words, size_t count) {
  return tang::write(address, words, count);
}
void sleepMs(void*, uint32_t ms) {
  const TickType_t ticks = pdMS_TO_TICKS(ms);
  vTaskDelay(ticks > 0 ? ticks : 1);
}
uint32_t nowMs(void*) { return static_cast<uint32_t>(millis()); }

struct ProgressCtx {
  Progress fn;
  void* ctx;
};
void pushProgress(void* ctx, const char*, uint32_t done, uint32_t total) {
  const ProgressCtx* p = static_cast<const ProgressCtx*>(ctx);
  if (p != nullptr && p->fn != nullptr) {
    p->fn(p->ctx, done, total);
  }
}

// Файл ядра: ИМЯ.<ext> в /CORES/ (регистр букв в имени файла не важен)
bool filePath(const char* name, const char* ext, char* path, size_t size) {
  DIR* dir = opendir(kFolder);
  if (dir == nullptr) {
    return false;
  }
  bool found = false;
  while (dirent* entry = readdir(dir)) {
    const size_t n = strlen(entry->d_name);
    if (n > 4 && strcasecmp(entry->d_name + n - 4, ext) == 0 && n - 4 == strlen(name) &&
        strncasecmp(entry->d_name, name, n - 4) == 0) {
      snprintf(path, size, "%s/%s", kFolder, entry->d_name);
      found = true;
      break;
    }
  }
  closedir(dir);
  return found;
}

bool corePath(const char* name, char* path, size_t size) { return filePath(name, ".BIN", path, size); }

bool fileSha(const char* path, uint32_t* size, uint8_t digest[32]) {
  FILE* f = fopen(path, "rb");
  if (f == nullptr) {
    return false;
  }
  static uint8_t buf[kReadChunk];
  Sha256 sha;
  bool ok = sha.begin();
  uint32_t total = 0;
  size_t n = 0;
  while (ok && (n = fread(buf, 1, sizeof buf, f)) > 0) {
    ok = sha.update(buf, n);
    total += static_cast<uint32_t>(n);
  }
  ok = ok && !ferror(f) && sha.finish(digest);
  fclose(f);
  *size = total;
  return ok;
}

bool fileRead(void* ctx, uint32_t offset, uint8_t* data, uint32_t size) {
  FILE* f = static_cast<FILE*>(ctx);
  return fseek(f, static_cast<long>(offset), SEEK_SET) == 0 && fread(data, 1, size, f) == size;
}

}  // namespace

const char* list(Entry* out, size_t max, size_t* count, bool checkCache, uint32_t waitMs) {
  *count = 0;
  tang_sd::Session session(waitMs);
  if (!session.ok()) {
    return tang_sd::kBusy;
  }
  const char* problem = tang_sd::mount();
  if (problem != nullptr) {
    return problem;
  }
  DIR* dir = opendir(kFolder);
  if (dir == nullptr) {
    return "no \\CORES folder on the Tang SD card";
  }
  while (dirent* entry = readdir(dir)) {
    const size_t n = strlen(entry->d_name);
    if (*count >= max || n <= 4 || n - 4 > 15 || strcasecmp(entry->d_name + n - 4, ".BIN") != 0) {
      continue;
    }
    Entry& e = out[*count];
    char raw[16] = {};
    memcpy(raw, entry->d_name, n - 4);
    if (!core_cache::normalizeName(raw, e.name)) {
      continue;
    }
    char path[96];
    snprintf(path, sizeof path, "%s/%s", kFolder, entry->d_name);
    struct stat st = {};
    stat(path, &st);
    e.size = static_cast<uint32_t>(st.st_size);
    e.cached = core_cache::Found::kNone;
    e.elf = false;
    ++*count;
  }
  closedir(dir);
  for (size_t i = 0; i < *count; ++i) {
    char path[96];
    out[i].elf = filePath(out[i].name, ".ELF", path, sizeof path);
  }
  if (!checkCache) {
    return nullptr;
  }
  const bl616::PushIo io = {nullptr, linkRead, linkWrite, sleepMs, nowMs, nullptr};
  core_cache::Client cache(io, esp_random());
  for (size_t i = 0; i < *count; ++i) {
    char path[96];
    uint8_t digest[32];
    uint32_t size = 0;
    if (!corePath(out[i].name, path, sizeof path) || !fileSha(path, &size, digest)) {
      continue;
    }
    problem = cache.find(out[i].name, digest, &out[i].cached);
    if (problem != nullptr) {
      return problem;
    }
  }
  return nullptr;
}

const char* sync(const char* name, Progress progress, void* ctx) {
  tang_sd::Session session;
  const char* problem = tang_sd::mount();
  if (problem != nullptr) {
    return problem;
  }
  char path[96];
  char n[16];
  if (!core_cache::normalizeName(name, n) || !corePath(n, path, sizeof path)) {
    return "no such core in \\CORES on the Tang SD card";
  }
  uint8_t digest[32];
  uint32_t size = 0;
  if (!fileSha(path, &size, digest)) {
    return "core file read failed";
  }
  ProgressCtx pc = {progress, ctx};
  const bl616::PushIo io = {&pc, linkRead, linkWrite, sleepMs, nowMs, pushProgress};
  core_cache::Client cache(io, esp_random());
  core_cache::Found found = core_cache::Found::kNone;
  problem = cache.find(n, digest, &found);
  if (problem != nullptr || found == core_cache::Found::kFound) {
    return problem;
  }
  FILE* f = fopen(path, "rb");
  if (f == nullptr) {
    return "core file open failed";
  }
  const core_cache::ImageReader reader = {f, fileRead};
  const uint32_t start = static_cast<uint32_t>(millis());
  problem = cache.put(n, size, digest, reader);
  fclose(f);
  bringupLog(kTag, "core %s (%u bytes) to the BL616 cache: %s, %u ms", n, static_cast<unsigned>(size),
             problem ? problem : "OK", static_cast<unsigned>(millis() - start));
  return problem;
}

const char* load(const char* name, uint32_t* version, Progress progress, void* ctx) {
  char n[16];
  if (!core_cache::normalizeName(name, n)) {
    return "bad core name";
  }
  tang_sd::Session session;
  if (tang_sd::heldByWc()) {
    return tang_sd::kHeldByWc;
  }
  // ядро есть на SD — сначала свежий образ в кэш; SD нет (мост пропал, карты нет) — что в кэше
  if (tang_sd::mount() == nullptr) {
    const char* problem = sync(n, progress, ctx);
    if (problem != nullptr && strstr(problem, "no such core") == nullptr) {
      return problem;
    }
  }
  return loadCached(n, version);
}

// Удача — ядро из кэша на LINK (личность 3+). Смена на другое (TS-Config → FT812: перезагрузка ПЛИС
// после неудачной загрузки взяла адрес перехода TS-Config — слот 1) — не удача: Tang — в слот 0.
// Отказ BL616 до команд настройки ПЛИС (прошивка 15: ответ LOAD_FAIL сразу, ПЛИС не тронута) виден
// в CP_INFO — ждать перезагрузку, которой не будет, незачем.
const char* loadCached(const char* name, uint32_t* version) {
  char n[16];
  if (!core_cache::normalizeName(name, n)) {
    return "bad core name";
  }
  tang_sd::Session session;
  if (tang_sd::heldByWc()) {
    return tang_sd::kHeldByWc;        // карта у WC: ядро без моста к ней не грузить
  }
  tang_sd::unmount();                 // у ядра из кэша моста к SD нет
  const bl616::PushIo io = {nullptr, linkRead, linkWrite, sleepMs, nowMs, nullptr};
  core_cache::Client cache(io, esp_random());
  const uint32_t reloads = tang::reloads();
  const char* problem = cache.load(n, nullptr);
  if (problem != nullptr) {
    return problem;
  }
  const uint32_t seq = cache.lastSeq();
  const uint32_t start = static_cast<uint32_t>(millis());
  while (tang::reloads() == reloads && static_cast<uint32_t>(millis()) - start < kSwitchMs) {
    uint32_t info = 0;
    if (tang::read(kRegCpInfo, &info, 1) && (info >> 24) == kCcInfoMagic && (info & 0xFFFF) == seq &&
        ((info >> 16) & 0xF0) == kCcLoadFail) {
      bringupLog(kTag, "core %s: BL616 could not load it (JTAG %u), the FPGA kept TS-Config", n,
                 static_cast<unsigned>((info >> 16) & 0x0F));
      return "the BL616 could not load the core, Tang stays in TS-Config";
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (tang::reloads() == reloads) {
    return "no core answered on LINK after the load";
  }
  *version = tang::coreVersion();
  if ((*version >> 24) < 0x03U) {
    if ((*version >> 24) != tang::kPersonalityTs) {
      tang::backToSlot0(10000);
    }
    return "the core did not start, Tang is back in slot 0";
  }
  bringupLog(kTag, "core %s runs: VERSION %08X, %u ms", n, static_cast<unsigned>(*version),
             static_cast<unsigned>(millis() - start));
  return nullptr;
}

const char* const kNoElf = "the core has no ESP layer (ELF) on the Tang SD";

const char* readElf(const char* name, uint8_t** data, uint32_t* size, uint32_t* stamp, uint32_t waitMs) {
  *data = nullptr;
  tang_sd::Session session(waitMs);
  if (!session.ok()) {
    return tang_sd::kBusy;
  }
  const char* problem = tang_sd::mount();
  if (problem != nullptr) {
    return problem;
  }
  char n[16];
  char path[96];
  if (!core_cache::normalizeName(name, n)) {
    return "bad core name";
  }
  if (!filePath(n, ".ELF", path, sizeof path)) {
    return kNoElf;
  }
  struct stat st = {};
  if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > 1024 * 1024) {
    return "core ELF has a bad size";
  }
  FILE* f = fopen(path, "rb");
  if (f == nullptr) {
    return "core ELF open failed";
  }
  uint8_t* buffer = static_cast<uint8_t*>(heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buffer == nullptr) {
    fclose(f);
    return "no memory for the core ELF";
  }
  const size_t got = fread(buffer, 1, st.st_size, f);
  fclose(f);
  if (got != static_cast<size_t>(st.st_size)) {
    heap_caps_free(buffer);
    return "core ELF read failed";
  }
  *data = buffer;
  *size = static_cast<uint32_t>(st.st_size);
  *stamp = static_cast<uint32_t>(st.st_mtime);
  return nullptr;
}

}  // namespace core_store
}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
