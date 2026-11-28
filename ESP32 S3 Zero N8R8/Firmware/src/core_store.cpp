#include "zifi/core_store.hpp"

#if ZIFI_HAS_SPI_LINK

#include <Arduino.h>
#include <atomic>
#include <dirent.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "zifi/bringup_log.hpp"
#include "zifi/sha256.hpp"
#include "zifi/tang_flash.hpp"
#include "zifi/tang_link.hpp"
#include "zifi/tang_sd.hpp"
#if ZIFI_HAS_USB_DRIVE
#include "zifi/usb_drive.hpp"
#endif

namespace zifi {
namespace core_store {
namespace {

constexpr const char* kTag = "cores";
constexpr uint32_t kSwitchMs = 15000;   // LOAD: новое ядро на LINK
constexpr uint32_t kRegCpInfo = 0x30;   // CP_INFO: ответ BL616 {0xC2, состояние, номер команды}
constexpr uint32_t kCcInfoMagic = 0xC2;
constexpr uint32_t kCcLoadFail = 0x20;  // LOAD_FAIL | код core_jtag (core_mailbox.h)
// Кусок чтения файла ядра для SHA-256 (fileSha). Не больше буфера драйвера флешки
// (UsbDrive::kTransferBytes): до 0.1.179 здесь было 8 КБ, и 27.11.2026 чтение ядра Q2 с флешки при
// буфере 4 КБ заставило драйвер растить буфер; во внутреннем ОЗУ игры места не нашлось, и драйвер
// освободил буфер дважды (паника в zm-coreload). Предел передачи FatFs (usb_drive.cpp) теперь
// делит такие чтения и сам; здесь — чтобы не делить зря. Буфер статический, во внутреннем ОЗУ:
// 4 КБ вместо 8 освобождают ещё 4 КБ всем.
constexpr size_t kReadChunk = 4096;
#if ZIFI_HAS_USB_DRIVE
static_assert(kReadChunk <= UsbDrive::kTransferBytes, "fileSha reads the USB drive in buffer-sized pieces");
#endif

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

// --- ядра с карты Evo (0.1.164) ------------------------------------------------------------------
// Отметки «ядро с карты Evo» и их образы в PSRAM. Пишет задача сети (MOD_QUERY, MOD_COMMIT), читает
// загрузчик ядер (load) — под g_evoLock. Образ, который загрузчик взял в работу, из таблицы вынут
// (data = nullptr): освобождает или возвращает его загрузчик же (evoDone). Таблица — в PSRAM:
// внутренняя память ESP на счету. Освобождение памяти — вне замка.
struct EvoCore {
  char name[16];          // пусто — место свободно
  uint32_t size;          // длина и время файла на карте Evo
  uint32_t stamp;
  uint8_t* data;          // образ в PSRAM; nullptr — он на флешке (или уже только в кэше BL616)
  bool hasSha;            // ядро уже клали в кэш BL616: sha известен, считать заново не нужно
  uint8_t sha[32];
  bool claimed;           // заявлено в этом запуске ESP (MOD_QUERY): load и CORE_APP берут его отсюда
};
constexpr size_t kEvoCores = 4;
constexpr uint32_t kEvoStoreWaitMs = 3000;   // MOD_COMMIT ждёт мост Tang (ящик BL616) не дольше
std::atomic<EvoCore*> g_evo{nullptr};
size_t g_evoNext = 0;     // какую отметку вытеснять, когда мест нет
portMUX_TYPE g_evoLock = portMUX_INITIALIZER_UNLOCKED;

// Отметки с известным SHA — в NVS ESP («zifi-evo»/«cores», до 4 записей по 56 байт): после
// перезапуска ESP MOD_QUERY по ним узнаёт, что этот файл карты Evo уже в кэше BL616, — и без флешки
// ядро не идёт с карты заново. Заявок («грузить отсюда») в NVS нет: их даёт только MOD_QUERY.
constexpr char kEvoNvs[] = "zifi-evo";
constexpr char kEvoKey[] = "cores";
// NVS лежит во встроенной флеши ESP. Пока идёт запись или чтение флеши, кэш процессора выключен,
// и стек задачи, которая обращается к флеши, обязан быть во внутренней памяти: стек в PSRAM в это
// время недоступен, и ESP-IDF останавливает прошивку (assert esp_task_stack_is_sane_cache_disabled,
// cache_utils.c:126). Загрузчик ядер zm-coreload нарочно со стеком в PSRAM (внутренней памяти
// мало) — и 24.11.2026 на 0.1.169 упал так при первой загрузке нового Q2.BIN: syncEvo → evoDone →
// evoSave → nvs_set_blob. С 0.1.170 evoDone только ставит этот флаг, а записывает отметки
// evoFlush() — его зовёт задача обмена с Z80 (Arduino loopTask, стек во внутренней памяти).
std::atomic<bool> g_evoSavePending{false};
struct EvoSaved {
  char name[16];
  uint32_t size;
  uint32_t stamp;
  uint8_t sha[32];
};

void evoLoadSaved(EvoCore* table) {
  nvs_handle_t handle;
  if (nvs_open(kEvoNvs, NVS_READONLY, &handle) != ESP_OK) {
    return;                               // записей ещё не было
  }
  EvoSaved saved[kEvoCores] = {};
  size_t length = sizeof saved;
  if (nvs_get_blob(handle, kEvoKey, saved, &length) == ESP_OK) {
    for (size_t i = 0; i < kEvoCores && i < length / sizeof(EvoSaved); ++i) {
      if (saved[i].name[0] == 0) {
        continue;
      }
      memcpy(table[i].name, saved[i].name, sizeof table[i].name);
      table[i].name[sizeof table[i].name - 1] = 0;
      table[i].size = saved[i].size;
      table[i].stamp = saved[i].stamp;
      memcpy(table[i].sha, saved[i].sha, sizeof table[i].sha);
      table[i].hasSha = true;
    }
  }
  nvs_close(handle);
}

void evoSave() {
  EvoCore* table = g_evo.load();
  if (table == nullptr) {
    return;
  }
  EvoSaved saved[kEvoCores] = {};
  portENTER_CRITICAL(&g_evoLock);
  for (size_t i = 0; i < kEvoCores; ++i) {
    if (table[i].name[0] != 0 && table[i].hasSha) {
      memcpy(saved[i].name, table[i].name, sizeof saved[i].name);
      saved[i].size = table[i].size;
      saved[i].stamp = table[i].stamp;
      memcpy(saved[i].sha, table[i].sha, sizeof saved[i].sha);
    }
  }
  portEXIT_CRITICAL(&g_evoLock);
  nvs_handle_t handle;
  if (nvs_open(kEvoNvs, NVS_READWRITE, &handle) != ESP_OK) {
    bringupLog(kTag, "Evo card cores: NVS not opened, marks not saved");
    return;
  }
  const esp_err_t err = nvs_set_blob(handle, kEvoKey, saved, sizeof saved);
  if (err != ESP_OK || nvs_commit(handle) != ESP_OK) {
    bringupLog(kTag, "Evo card cores: marks not saved to NVS (%d)", static_cast<int>(err));
  }
  nvs_close(handle);
}

// Таблицу заводят только evoOffer и evoClaimCached — их зовёт задача обмена с Z80 (MOD_QUERY,
// MOD_COMMIT), у которой стек во внутренней памяти: поэтому evoLoadSaved здесь может читать NVS.
// zm-coreload таблицу не заводит (syncEvo и evoFind берут только готовую через g_evo).
EvoCore* evoTable() {
  EvoCore* table = g_evo.load();
  if (table == nullptr) {
    table = static_cast<EvoCore*>(
        heap_caps_calloc(kEvoCores, sizeof(EvoCore), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (table != nullptr) {
      evoLoadSaved(table);                // до публикации: другие задачи видят таблицу уже полной
    }
    EvoCore* expected = nullptr;
    if (table != nullptr && !g_evo.compare_exchange_strong(expected, table)) {
      heap_caps_free(table);
      table = expected;
    }
  }
  return table;
}

// Отметка name (имя уже нормализовано): копия полей в *out; take — образ вынимается из таблицы и
// теперь у вызвавшего; claimedOnly — только заявленная в этом запуске ESP.
bool evoFind(const char* name, EvoCore* out, bool take, bool claimedOnly) {
  EvoCore* table = g_evo.load();
  if (table == nullptr) {
    return false;
  }
  bool found = false;
  portENTER_CRITICAL(&g_evoLock);
  for (size_t i = 0; i < kEvoCores; ++i) {
    if (table[i].name[0] != 0 && strcmp(table[i].name, name) == 0) {
      if (claimedOnly && !table[i].claimed) {
        break;
      }
      *out = table[i];
      if (take) {
        table[i].data = nullptr;
      }
      found = true;
      break;
    }
  }
  portEXIT_CRITICAL(&g_evoLock);
  return found;
}

// После попытки загрузки: sha (ядро легло в кэш BL616) и образ назад — пусть полежит до повтора.
// Отметку за это время могли сменить (пришёл другой файл) — тогда образ освобождается.
void evoDone(const char* name, const EvoCore& was, const uint8_t* sha, uint8_t* data) {
  EvoCore* table = g_evo.load();
  uint8_t* drop = data;
  bool save = false;
  portENTER_CRITICAL(&g_evoLock);
  for (size_t i = 0; table != nullptr && i < kEvoCores; ++i) {
    EvoCore& e = table[i];
    if (e.name[0] != 0 && strcmp(e.name, name) == 0 && e.size == was.size && e.stamp == was.stamp) {
      if (sha != nullptr) {
        save = !e.hasSha || memcmp(e.sha, sha, sizeof e.sha) != 0;
        memcpy(e.sha, sha, sizeof e.sha);
        e.hasSha = true;
      }
      if (data != nullptr && e.data == nullptr) {
        e.data = data;
        drop = nullptr;
      }
      break;
    }
  }
  portEXIT_CRITICAL(&g_evoLock);
  if (drop != nullptr) {
    heap_caps_free(drop);
  }
  if (save) {
    // SHA нового файла — в NVS, чтобы пережил перезапуск ESP. Не здесь: evoDone зовут и из
    // zm-coreload (стек в PSRAM, флешь ей трогать нельзя) — запишет evoFlush (см. g_evoSavePending).
    g_evoSavePending = true;
  }
}

struct MemImage {
  const uint8_t* data;
  uint32_t size;
};

bool memRead(void* ctx, uint32_t offset, uint8_t* out, uint32_t size) {
  const MemImage* image = static_cast<const MemImage*>(ctx);
  if (offset > image->size || size > image->size - offset) {
    return false;
  }
  memcpy(out, image->data + offset, size);
  return true;
}

const char* const kNotEvo = "not a core from the Evo card";

// Ядро с карты Evo — в кэш BL616, если его там нет: образ из PSRAM или копия с флешки. kNotEvo —
// отметки нет (ядро не с карты Evo: как прежде, с SD Tang).
const char* syncEvo(const char* name, Progress progress, void* ctx) {
  EvoCore mark;
  if (!evoFind(name, &mark, true, true)) {
    return kNotEvo;
  }
  char path[64];
  bool usb = false;
#if ZIFI_HAS_USB_DRIVE
  snprintf(path, sizeof path, "%s/%s.BIN", UsbDrive::kCoreFolder, name);
  usb = mark.data == nullptr && usbDrive().ready();
#else
  path[0] = 0;
#endif
  uint8_t digest[32];
  bool haveSha = mark.hasSha;
  const char* problem = nullptr;
  if (haveSha) {
    memcpy(digest, mark.sha, sizeof digest);
  } else if (mark.data != nullptr) {
    Sha256 sha;
    haveSha = sha.begin() && sha.update(mark.data, mark.size) && sha.finish(digest);
  } else if (usb) {
    uint32_t size = 0;
    haveSha = fileSha(path, &size, digest) && size == mark.size;
  }
  if (!haveSha) {
    problem = mark.data != nullptr ? "SHA-256 of the Evo card core failed"
                                   : "the Evo card core is neither in PSRAM nor on the USB drive: send it again";
  }
  if (problem == nullptr) {
    ProgressCtx pc = {progress, ctx};
    const bl616::PushIo io = {&pc, linkRead, linkWrite, sleepMs, nowMs, pushProgress};
    core_cache::Client cache(io, esp_random());
    core_cache::Found found = core_cache::Found::kNone;
    problem = cache.find(name, digest, &found);
    if (problem == nullptr && found != core_cache::Found::kFound) {
      const uint32_t start = static_cast<uint32_t>(millis());
      const char* from = "PSRAM";
      if (mark.data != nullptr) {
        MemImage image = {mark.data, mark.size};
        problem = cache.put(name, mark.size, digest, core_cache::ImageReader{&image, memRead});
      } else if (FILE* f = usb ? fopen(path, "rb") : nullptr) {
        from = "the USB drive";
        problem = cache.put(name, mark.size, digest, core_cache::ImageReader{f, fileRead});
        fclose(f);
      } else {
        problem = "the Evo card core left the BL616 cache and has no copy here: send it again";
      }
      bringupLog(kTag, "core %s (%u bytes, Evo card, from %s) to the BL616 cache: %s, %u ms", name,
                 static_cast<unsigned>(mark.size), from, problem ? problem : "OK",
                 static_cast<unsigned>(millis() - start));
    }
  }
  // Ядро в кэше BL616 — образ в PSRAM больше не нужен; не вышло — пусть полежит до повтора
  if (problem == nullptr && mark.data != nullptr) {
    heap_caps_free(mark.data);
    mark.data = nullptr;
  }
  evoDone(name, mark, problem == nullptr ? digest : nullptr, mark.data);
  return problem;
}

// Новое ядро на LINK после LOAD или BOOT (seq — номер команды): удача — личность 3+, а если want не 0 —
// именно want. Смена на другое (TS-Config → FT812: перезагрузка ПЛИС после неудачной загрузки взяла
// адрес перехода TS-Config — слот 1) — не удача: Tang — в слот 0. Отказ BL616 до команд настройки
// ПЛИС (прошивка 15: ответ LOAD_FAIL, ПЛИС не тронута) виден в CP_INFO — ждать перезагрузку, которой
// не будет, незачем.
const char* waitCore(const char* n, uint32_t seq, uint32_t reloads, uint32_t want, uint32_t* version) {
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
  const uint32_t personality = *version >> 24;
  if (personality < 0x03U || (want != 0 && personality != want)) {
    if (personality != tang::kPersonalityTs) {
      tang::backToSlot0(10000);
    }
    bringupLog(kTag, "core %s: VERSION %08X instead, back to slot 0", n, static_cast<unsigned>(*version));
    return personality < 0x03U ? "the core did not start, Tang is back in slot 0"
                               : "another core started (not the one asked for), Tang is back in slot 0";
  }
  bringupLog(kTag, "core %s runs: VERSION %08X, %u ms", n, static_cast<unsigned>(*version),
             static_cast<unsigned>(millis() - start));
  return nullptr;
}

// Ядро из своего слота flash Tang (команда BOOT BL616). kNoSlot — ПЛИС не тронута, грузить с SD через
// кэш: в слоте нет годного начала битстрима (синхрослово, IDCODE, адрес перехода 0), flash не
// читается (у ядра FT812 1.31+ моста нет) или прошивка BL616 старше 35.
const char* const kNoSlot = "no core in its Tang flash slot";

const char* loadSlot(const FlashCore& core, uint32_t* version) {
  const uint32_t address = core.slot * tang::kSlotBytes;
  uint8_t head[128];
  if (!tang_flash::available() || !tang_flash::read(address, head, sizeof head)) {
    bringupLog(kTag, "core %s: Tang flash is not readable, slot %u skipped", core.name,
               static_cast<unsigned>(core.slot));
    return kNoSlot;
  }
  const char* bad = core_cache::checkHead(head, sizeof head);
  if (bad != nullptr) {
    bringupLog(kTag, "core %s: slot %u (%06X): %s", core.name, static_cast<unsigned>(core.slot),
               static_cast<unsigned>(address), bad);
    return kNoSlot;
  }
  tang_sd::unmount();                 // у ядра слота моста к SD нет
  const bl616::PushIo io = {nullptr, linkRead, linkWrite, sleepMs, nowMs, nullptr};
  core_cache::Client cache(io, esp_random());
  const uint32_t reloads = tang::reloads();
  tang_flash::flush();                // несохранённые настройки TS-Config — до смены ядра
  const char* problem = cache.boot(address);
  if (problem == core_cache::kNoBoot) {
    bringupLog(kTag, "core %s: %s", core.name, problem);
    return kNoSlot;
  }
  if (problem != nullptr) {
    return problem;
  }
  bringupLog(kTag, "core %s: BL616 boots Tang flash slot %u (%06X)", core.name, static_cast<unsigned>(core.slot),
             static_cast<unsigned>(address));
  return waitCore(core.name, cache.lastSeq(), reloads, core.personality, version);
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
  // Карта Tang у WC (SD2): только ядро видео из слота 3 — карту оно само не трогает, мост к ней в нём
  // есть (1.47+): видео с SD2; удержание подключится к нему само (tang_sd::attachHeld)
  const bool held = tang_sd::heldByWc();
  if (held && strcmp(n, "VIDEO") != 0) {
    return tang_sd::kHeldByWc;
  }
  // у ядра свой слот во flash Tang — оттуда; слот пуст (или BL616 старше 35) — как прочие, с SD
  for (const FlashCore& core : kFlashCores) {
    if (strcmp(core.name, n) == 0) {
      const char* problem = loadSlot(core, version);
      if (problem == nullptr && held) {
        // карта у WC: сразу к мосту ядра видео (иначе ждало бы первое чтение SD2 — инициализация
        // карты ~0,2 с); ядро старше 1.47 моста не имеет — SD2 до возврата в слот 0 не читается
        const char* attach = tang_sd::holdForWc();
        bringupLog(kTag, "core %s: Tang SD held by WC %s", n, attach ? attach : "attached to the video core");
      }
      if (problem != kNoSlot) {
        return problem;
      }
      if (held) {
        return tang_sd::kHeldByWc;    // с SD (кэш BL616) — нельзя: FatFs ESP карту не монтирует
      }
      bringupLog(kTag, "core %s: not from Tang flash slot %u, trying the Tang SD and the BL616 cache", n,
                 static_cast<unsigned>(core.slot));
      break;
    }
  }
  // ядро с карты Evo (MOD_QUERY с флагом 2, 0.1.164) — из PSRAM или с флешки, не с SD Tang
  const char* evo = syncEvo(n, progress, ctx);
  if (evo != kNotEvo) {
    return evo != nullptr ? evo : loadCached(n, version);
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

// Удача — ядро из кэша на LINK (личность 3+, waitCore).
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
  tang_flash::flush();                // несохранённые настройки TS-Config — до смены ядра
  const char* problem = cache.load(n, nullptr);
  if (problem != nullptr) {
    return problem;
  }
  return waitCore(n, cache.lastSeq(), reloads, 0, version);
}

const char* checkEvoImage(const uint8_t* data, uint32_t size) {
  if (data == nullptr || size < 128 + 64) {
    return "too short for a core bitstream";
  }
  if (size > tang::kSlotBytes) {
    return "longer than a Tang flash slot (1 MB)";
  }
  uint32_t usercode = 0;
  return core_cache::checkImage(data, 128, data + size - 64, 64, &usercode);
}

void evoOffer(const char* name, uint8_t* data, uint32_t size, uint32_t stamp) {
  char n[16];
  EvoCore* table = core_cache::normalizeName(name, n) ? evoTable() : nullptr;
  if (table == nullptr) {
    if (data != nullptr) {
      heap_caps_free(data);
    }
    return;
  }
  uint8_t* drop = nullptr;
  uint8_t* dropOld = nullptr;
  portENTER_CRITICAL(&g_evoLock);
  EvoCore* slot = nullptr;
  EvoCore* empty = nullptr;
  for (size_t i = 0; i < kEvoCores; ++i) {
    if (table[i].name[0] != 0 && strcmp(table[i].name, n) == 0) {
      slot = &table[i];
    } else if (table[i].name[0] == 0 && empty == nullptr) {
      empty = &table[i];
    }
  }
  if (slot == nullptr) {
    slot = empty;
    if (slot == nullptr) {
      slot = &table[g_evoNext];
      g_evoNext = (g_evoNext + 1) % kEvoCores;
    }
    drop = slot->data;
    *slot = EvoCore{};
    memcpy(slot->name, n, sizeof slot->name);
  } else if (slot->size != size || slot->stamp != stamp) {
    drop = slot->data;                // другой файл: прежние образ и sha не годятся
    slot->data = nullptr;
    slot->hasSha = false;
  }
  slot->size = size;
  slot->stamp = stamp;
  slot->claimed = true;                 // программа Z80 прислала или подтвердила его сейчас
  if (data != nullptr) {
    dropOld = slot->data;
    slot->data = data;
  }
  portEXIT_CRITICAL(&g_evoLock);
  if (drop != nullptr) {
    heap_caps_free(drop);
  }
  if (dropOld != nullptr && dropOld != drop) {
    heap_caps_free(dropOld);
  }
}

bool evoMarked(const char* name) {
  char n[16];
  EvoCore mark;
  return core_cache::normalizeName(name, n) && evoFind(n, &mark, false, true);
}

bool evoHeld(const char* name, uint32_t size, uint32_t stamp) {
  char n[16];
  EvoCore mark;
  return core_cache::normalizeName(name, n) && evoFind(n, &mark, false, true) && mark.data != nullptr &&
         mark.size == size && mark.stamp == stamp;
}

bool evoClaimCached(const char* name, uint32_t size, uint32_t stamp) {
  char n[16];
  EvoCore mark;
  if (!core_cache::normalizeName(name, n) || evoTable() == nullptr || !evoFind(n, &mark, false, false) ||
      !mark.hasSha || mark.size != size || mark.stamp != stamp) {
    return false;                         // этот файл карты в кэш BL616 ещё не клали
  }
  tang_sd::Session session(tang_sd::kQuickMs);
  if (!session.ok()) {
    return false;
  }
  const bl616::PushIo io = {nullptr, linkRead, linkWrite, sleepMs, nowMs, nullptr};
  core_cache::Client cache(io, esp_random());
  core_cache::Found found = core_cache::Found::kNone;
  if (cache.find(n, mark.sha, &found) != nullptr || found != core_cache::Found::kFound) {
    return false;                         // кэш BL616 его уже вытеснил (или BL616 не ответил)
  }
  EvoCore* table = g_evo.load();
  bool claimed = false;
  portENTER_CRITICAL(&g_evoLock);
  for (size_t i = 0; i < kEvoCores; ++i) {
    EvoCore& e = table[i];
    if (e.name[0] != 0 && strcmp(e.name, n) == 0 && e.size == size && e.stamp == stamp && e.hasSha) {
      e.claimed = true;
      claimed = true;
      break;
    }
  }
  portEXIT_CRITICAL(&g_evoLock);
  return claimed;
}

const char* evoStoreToCache(const char* name, uint8_t* data, uint32_t size, uint32_t stamp) {
  char n[16];
  if (!core_cache::normalizeName(name, n)) {
    heap_caps_free(data);
    return "bad core name";
  }
  uint8_t digest[32];
  Sha256 sha;
  const char* problem = sha.begin() && sha.update(data, size) && sha.finish(digest) ? nullptr : "SHA-256 failed";
  if (problem == nullptr) {
    tang_sd::Session session(kEvoStoreWaitMs);
    if (!session.ok()) {
      problem = tang_sd::kBusy;
    } else {
      const bl616::PushIo io = {nullptr, linkRead, linkWrite, sleepMs, nowMs, nullptr};
      core_cache::Client cache(io, esp_random());
      core_cache::Found found = core_cache::Found::kNone;
      problem = cache.find(n, digest, &found);
      if (problem == nullptr && found != core_cache::Found::kFound) {
        const uint32_t start = static_cast<uint32_t>(millis());
        MemImage image = {data, size};
        problem = cache.put(n, size, digest, core_cache::ImageReader{&image, memRead});
        bringupLog(kTag, "core %s (%u bytes, Evo card, no USB drive) to the BL616 cache: %s, %u ms", n,
                   static_cast<unsigned>(size), problem ? problem : "OK",
                   static_cast<unsigned>(millis() - start));
      }
    }
  }
  if (problem != nullptr) {
    evoOffer(n, data, size, stamp);       // образ — в PSRAM: core_load положит его в кэш сам
    return problem;
  }
  heap_caps_free(data);
  evoOffer(n, nullptr, size, stamp);
  EvoCore was = {};
  was.size = size;
  was.stamp = stamp;
  evoDone(n, was, digest, nullptr);       // SHA — в отметку и в NVS
  return nullptr;
}

void evoReleaseClaims() {
  EvoCore* table = g_evo.load();
  if (table == nullptr) {
    return;
  }
  uint8_t* drop[kEvoCores] = {};
  portENTER_CRITICAL(&g_evoLock);
  for (size_t i = 0; i < kEvoCores; ++i) {
    table[i].claimed = false;
    drop[i] = table[i].data;
    table[i].data = nullptr;
  }
  portEXIT_CRITICAL(&g_evoLock);
  for (uint8_t* data : drop) {
    if (data != nullptr) {
      heap_caps_free(data);
    }
  }
}

uint32_t evoReleaseMemory() {
  EvoCore* table = g_evo.load();
  if (table == nullptr) {
    return 0;
  }
  uint8_t* drop[kEvoCores] = {};
  uint32_t bytes = 0;
  portENTER_CRITICAL(&g_evoLock);
  for (size_t i = 0; i < kEvoCores; ++i) {
    if (table[i].data != nullptr) {
      drop[i] = table[i].data;
      bytes += table[i].size;
      table[i].data = nullptr;
    }
  }
  portEXIT_CRITICAL(&g_evoLock);
  for (uint8_t* data : drop) {
    if (data != nullptr) {
      heap_caps_free(data);
    }
  }
  return bytes;
}

// Отложенная запись отметок в NVS (0.1.170, почему — у g_evoSavePending). Зовёт Application::pollLink
// (задача обмена с Z80, Arduino loopTask: стек во внутренней памяти) на каждом проходе: флага нет —
// сразу выход, так что ничего не стоит.
void evoFlush() {
  if (!g_evoSavePending.load()) {
    return;
  }
  // Страховка: адрес локальной переменной — это адрес стека самой задачи. Он не во внутренней
  // памяти (кто-то позвал evoFlush из задачи со стеком в PSRAM) — не писать, флаг оставить: запишет
  // следующий вызов из правильной задачи. Иначе ESP-IDF остановила бы прошивку, как 24.11.2026.
  int probe = 0;
  if (!esp_ptr_internal(&probe)) {
    return;
  }
  // Флаг снимается до записи: если за время записи придёт новый SHA, флаг встанет снова, и следующий
  // проход запишет и его.
  g_evoSavePending = false;
  evoSave();
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
