#include "zifi/net_console.hpp"

#if ZIFI_HAS_SPI_LINK

#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <dirent.h>
#include <lwip/sockets.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "zifi/core_store.hpp"
#include "zifi/diagnostic_log.hpp"
#include "zifi/ft_host.hpp"
#include "zifi/player_overlay.hpp"
#include "zifi/tang_flash.hpp"
#include "zifi/tang_link.hpp"
#include "zifi/tang_sd.hpp"

namespace zifi {
namespace {

constexpr uint16_t kPort = 23;
constexpr size_t kRingBytes = 32 * 1024;
constexpr size_t kLineBytes = 256;
constexpr uint32_t kTaskStack = 6144;   // 4096 переполнялся (0.1.85: tang sd/core, FatFs, LittleFS)
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

// LINK для клиента кэша BL616 из консоли (tang core format)
bool consoleLinkRead(void*, uint32_t address, uint32_t* words, size_t count) {
  return tang::read(address, words, count);
}
bool consoleLinkWrite(void*, uint32_t address, const uint32_t* words, size_t count) {
  return tang::write(address, words, count);
}
void consoleSleep(void*, uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms == 0 ? 1 : ms)); }
uint32_t consoleNow(void*) { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// microSD Tang через мост ядра TS-Config 1.30 (tang_sd.hpp): смонтировать и показать, список
// папки, скорость чтения блоков, снять.
bool runTangSd(int client, const char* arguments) {
  tang_sd::Session session;            // том и файлы — пока команда не кончилась
  char verb[8] = {};
  char path[96] = {};
  sscanf(arguments, " %7s %95[^\n]", verb, path);
  if (strcmp(verb, "off") == 0) {
    tang_sd::unmount();
    return sendText(client, "tang sd: unmounted\n");
  }
  const char* problem = tang_sd::mount();
  if (problem != nullptr) {
    return sendText(client, "tang sd: %s\n", problem);
  }
  if (strcmp(verb, "ls") == 0) {
    char full[128];
    snprintf(full, sizeof(full), "%s/%s", tang_sd::kMountPoint, path[0] == '/' ? path + 1 : path);
    DIR* dir = opendir(full);
    if (dir == nullptr) {
      return sendText(client, "tang sd: no folder %s\n", full);
    }
    unsigned count = 0;
    while (dirent* entry = readdir(dir)) {
      char item[200];
      snprintf(item, sizeof(item), "%s/%s", full, entry->d_name);
      struct stat st = {};
      const bool folder = entry->d_type == DT_DIR;
      if (!folder) {
        stat(item, &st);
      }
      if (!sendText(client, folder ? "  %s/\n" : "  %s  %ld\n", entry->d_name, static_cast<long>(st.st_size))) {
        closedir(dir);
        return false;
      }
      ++count;
    }
    closedir(dir);
    return sendText(client, "tang sd: %u entries in %s\n", count, full);
  }
  if (strcmp(verb, "speed") == 0) {
    // 2 МБ подряд с блока 8192 (за таблицей разделов и началом FAT)
    constexpr uint32_t kBlocks = 64;
    uint8_t* buffer = static_cast<uint8_t*>(heap_caps_malloc(kBlocks * 512, MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
      return sendText(client, "tang sd speed: no memory\n");
    }
    const int64_t start = esp_timer_get_time();
    bool ok = true;
    for (uint32_t i = 0; i < 64 && ok; ++i) {
      ok = tang_sd::readBlocks(8192 + i * kBlocks, buffer, kBlocks);
    }
    const uint32_t us = static_cast<uint32_t>(esp_timer_get_time() - start);
    heap_caps_free(buffer);
    if (!ok) {
      return sendText(client, "tang sd speed: read failed\n");
    }
    return sendText(client, "tang sd speed: 2048 KB in %u ms, %u KB/s\n", static_cast<unsigned>(us / 1000),
                    static_cast<unsigned>(2048ULL * 1000000 / (us ? us : 1)));
  }
  char text[160];
  tang_sd::describe(text, sizeof(text));
  return sendText(client, "tang sd: %s\n", text);
}

// Ядра для кэша BL616 на SD Tang (core_store.hpp): список с отметкой «в кэше», запись в кэш,
// загрузка в ПЛИС.
bool runTangCore(int client, const char* arguments) {
  char verb[8] = {};
  char name[24] = {};
  sscanf(arguments, " %7s %23s", verb, name);
  if (strcmp(verb, "format") == 0) {
    // пустой каталог кэша (INFO 255: на месте кэша чьи-то данные) — только командой пользователя
    if (strcmp(name, "yes") != 0) {
      return sendText(client, "tang core format yes — erases the BL616 cache directory (data there is lost)\n");
    }
    tang_sd::Session session;
    const bl616::PushIo io = {nullptr, consoleLinkRead, consoleLinkWrite, consoleSleep, consoleNow, nullptr};
    core_cache::Client cache(io, esp_random());
    const char* problem = cache.format();
    return sendText(client, "tang core format: %s\n", problem != nullptr ? problem : "empty cache directory written");
  }
  if (strcmp(verb, "put") == 0 || strcmp(verb, "load") == 0) {
    if (name[0] == 0) {
      return sendText(client, "tang core %s NAME\n", verb);
    }
    const char* problem = nullptr;
    uint32_t version = 0;
    if (strcmp(verb, "put") == 0) {
      problem = core_store::sync(name, nullptr, nullptr);
    } else {
      // ядро ведёт консоль: назад — «tang boot»
      tang::claimCore();
      problem = core_store::load(name, &version, nullptr, nullptr);
      if (problem != nullptr) {
        tang::releaseCore();
      }
    }
    if (problem != nullptr) {
      return sendText(client, "tang core %s %s: %s\n", verb, name, problem);
    }
    return strcmp(verb, "put") == 0 ? sendText(client, "tang core put %s: in the BL616 cache\n", name)
                                    : sendText(client, "tang core load %s: runs, VERSION %08X\n", name,
                                               static_cast<unsigned>(version));
  }
  static core_store::Entry entries[core_store::kMaxCores];
  size_t count = 0;
  const char* problem = core_store::list(entries, core_store::kMaxCores, &count, true);
  if (problem != nullptr) {
    return sendText(client, "tang core: %s\n", problem);
  }
  for (size_t i = 0; i < count; ++i) {
    const char* mark = entries[i].cached == core_cache::Found::kFound   ? "in cache"
                       : entries[i].cached == core_cache::Found::kStale ? "cache has another build"
                                                                        : "not in cache";
    if (!sendText(client, "  %-15s %8u  %s\n", entries[i].name, static_cast<unsigned>(entries[i].size), mark)) {
      return false;
    }
  }
  return sendText(client, "tang core: %u cores in %s\n", static_cast<unsigned>(count), core_store::kFolder);
}

// Обмен не прошёл: LINK мог быть занят другой задачей дольше tang::kLockWaitMs — кто его держит
bool transferFailed(int client, const char* what) {
  char lock[128];
  tang::describeLock(lock, sizeof(lock));
  return sendText(client, "%s: transfer failed; LINK %s\n", what, lock);
}

// Регистры Tang по LINK (tang_link.hpp): сводка, чтение и запись одного слова.
bool runTang(int client, const char* arguments) {
  if (!tang::ready()) {
    return sendText(client, "tang: LINK is not ready\n");
  }
  unsigned address = 0;
  unsigned value = 0;
  char verb[8] = {};
  if (sscanf(arguments, " %7s", verb) == 1 && strcmp(verb, "sd") == 0) {
    return runTangSd(client, strstr(arguments, "sd") + 2);
  }
  if (strcmp(verb, "core") == 0) {
    return runTangCore(client, strstr(arguments, "core") + 4);
  }
  if (strcmp(verb, "lock") == 0) {
    // Кто держит LINK — без обмена по LINK: отвечает, даже когда LINK занят
    char lock[128];
    tang::describeLock(lock, sizeof(lock));
    return sendText(client, "tang LINK %s\n", lock);
  }
  if (strcmp(verb, "flash") == 0) {
    char text[120];
    tang_flash::describe(text, sizeof(text));
    return sendText(client, "tang %s\n", text);
  }
  if (strcmp(verb, "player") == 0) {
    // Экран проигрывателя оверлеем: состояние, кадры, время отрисовки и передачи
    char text[256];
    player_overlay::describe(text, sizeof(text));
    return sendText(client, "tang player: %s\n", text);
  }
  if (strcmp(verb, "fthost") == 0) {
    // Хост FT812 — ESP (FT_CFG бит 2, ядро 1.12+): SPI Evo к FT812 отключается
    char mode[8] = {};
    sscanf(arguments, " %*7s %7s", mode);
    const bool on = strcmp(mode, "on") == 0;
    if (!on && strcmp(mode, "off") != 0) {
      return sendText(client, "tang fthost on|off\n");
    }
    uint32_t boot = 0;
    if (tang::read(tang::kRegBoot, &boot, 1) && (boot >> 20) == tang::kBootTag &&
        ((boot >> 17) & 1U) == 0) {
      return sendText(client, "tang fthost: no FT812 in this core (tang boot to the FT812 core)\n");
    }
    uint32_t st = 0;
    if (!tang::read(tang::kRegFtStat, &st, 1)) {
      return transferFailed(client, "tang");
    }
    // Прочие биты FT_CFG — как были: RAM_G 3 МБ (FT_STAT 27), без дизеринга (13)
    const uint32_t cfg = ((st >> 27) & 1U) | (((st >> 13) & 1U) << 1) | (on ? tang::kFtCfgEspHost : 0U);
    if (!tang::write(tang::kRegFtStat, &cfg, 1) || !tang::read(tang::kRegFtStat, &st, 1)) {
      return transferFailed(client, "tang");
    }
    return sendText(client, "tang fthost: FT812 host is %s (FT_STAT %08X)\n",
                    ((st >> 12) & 1U) ? "ESP, Evo SPI disconnected" : "Evo",
                    static_cast<unsigned>(st));
  }
  if (strcmp(verb, "ftspeed") == 0) {
    // Скорость записи в RAM_G окном FT_ADDR/FT_DATA — тем же путём, что у приложений
    // (ft_host::writeBlock, блоки по 256 слов, как у DOOM): N КБ с 0x080000, затем сверка
    // чтением 64 случайных слов. Содержимое RAM_G там портится — только для проверки.
    unsigned kb = 0;
    sscanf(arguments, " %*7s %u", &kb);
    if (kb == 0 || kb > 512) {
      kb = 64;
    }
    uint32_t boot = 0;
    if (tang::read(tang::kRegBoot, &boot, 1) && (boot >> 20) == tang::kBootTag &&
        ((boot >> 17) & 1U) == 0) {
      return sendText(client, "tang ftspeed: no FT812 in this core (tang boot to the FT812 core)\n");
    }
    static uint32_t block[256];
    constexpr uint32_t kBase = 0x080000;
    const uint32_t words = kb * 256U;
    const uint32_t seed = esp_random();
    auto pattern = [seed](uint32_t index) { return seed ^ (index * 2654435761U); };
    const int64_t started = esp_timer_get_time();
    for (uint32_t done = 0; done < words; done += 256) {
      for (uint32_t i = 0; i < 256; ++i) {
        block[i] = pattern(done + i);
      }
      if (!ft_host::writeBlock(kBase + 4U * done, block, 256)) {
        return sendText(client, "tang ftspeed: write failed at %06X\n",
                        static_cast<unsigned>(kBase + 4U * done));
      }
    }
    const uint32_t us = static_cast<uint32_t>(esp_timer_get_time() - started);
    unsigned good = 0;
    for (unsigned k = 0; k < 64; ++k) {
      const uint32_t index = k == 63 ? words - 1 : esp_random() % words;
      uint32_t back = 0;
      if (ft_host::read32(kBase + 4U * index, &back) && back == pattern(index)) {
        ++good;
      }
    }
    return sendText(client, "tang ftspeed: %u KB in %u us, %u ns/word, %u KB/s; check %u/64 ok, %u kHz\n",
                    kb, static_cast<unsigned>(us), static_cast<unsigned>(1000ULL * us / words),
                    static_cast<unsigned>(us ? 1000000ULL * kb / us : 0), good,
                    static_cast<unsigned>(tang::clockHz() / 1000));
  }
  if (strcmp(verb, "boot") == 0) {
    const uint32_t personality = tang::coreVersion() >> 24;
    if (personality >= 0x03U) {
      // Ядро из кэша BL616 (ZX Next и другие): BOOT у него только на запись и не читается —
      // запись «BOOT» уходит BL616, тот перезагружает ПЛИС в слот 0 (TS-Config)
      if (!tang::backToSlot0(15000)) {
        return sendText(client, "tang boot: core %02X did not return to slot 0 in 15 s\n",
                        static_cast<unsigned>(personality));
      }
      tang::releaseCore();
      return sendText(client, "tang boot: core %02X -> slot 0, TS-Config %u.%u runs\n",
                      static_cast<unsigned>(personality),
                      static_cast<unsigned>((tang::coreVersion() >> 8) & 0xFF),
                      static_cast<unsigned>(tang::coreVersion() & 0xFF));
    }
    if (tang_sd::heldByWc()) {
      return sendText(client, "tang boot: %s\n", tang_sd::kHeldByWc);
    }
    // Multiboot (ядро 1.11+): перезагрузить ПЛИС в слот перехода, если там битстрим GW2AR-18
    uint32_t boot = 0;
    if (!tang::read(tang::kRegBoot, &boot, 1) || (boot >> 20) != tang::kBootTag) {
      return sendText(client, "tang boot: the core has no BOOT register (core 1.11+ needed)\n");
    }
    const unsigned own = boot & 0xFF;
    const unsigned next = (boot >> 8) & 0xFF;
    if (!tang_flash::slotHasBitstream(next * tang::kSlotBytes)) {
      return sendText(client, "tang boot: slot %u (%06X) holds no GW2AR-18 bitstream, not rebooting\n",
                      next, next * static_cast<unsigned>(tang::kSlotBytes));
    }
    const uint32_t magic = tang::kBootMagic;
    if (!tang::write(tang::kRegBoot, &magic, 1)) {
      return transferFailed(client, "tang boot");
    }
    return sendText(client, "tang boot: slot %u -> slot %u, Tang reloads (seen reloads so far: %u)\n",
                    own, next, static_cast<unsigned>(tang::reloads()));
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
      return transferFailed(client, "tang");
    }
    return sendText(client, "tang %02X <- %08X, reads %08X\n", address & ~3U,
                    static_cast<unsigned>(word), static_cast<unsigned>(back));
  }
  if (sscanf(arguments, " rd %x", &address) == 1) {
    uint32_t word = 0;
    if (!tang::read(address & ~3U, &word, 1)) {
      return transferFailed(client, "tang");
    }
    return sendText(client, "tang %02X = %08X\n", address & ~3U, static_cast<unsigned>(word));
  }
  if (arguments[0] != 0) {
    return sendText(client,
                    "tang [rd ADDR | wr ADDR VALUE | flash | boot | lock | fthost on|off | test N | ftspeed KB | "
                    "sd [ls DIR | speed | off] | core [put NAME | load NAME | format yes] | player], hex; "
                    "no arguments: "
                    "summary\n");
  }
  uint32_t r[8] = {};
  uint32_t c[2] = {};
  const uint32_t version = tang::coreVersion();
  if ((version >> 24) >= 0x03U) {
    // Ядро из кэша BL616: регистры свои — как есть, без разбора TS-Config
    if (!tang::read(tang::kRegStatus, r, 8)) {
      return transferFailed(client, "tang");
    }
    return sendText(client,
                    "tang core %02X %u.%u from the BL616 cache; 08..24: %08X %08X %08X %08X %08X %08X "
                    "%08X %08X; %u kHz\n",
                    static_cast<unsigned>(version >> 24), static_cast<unsigned>((version >> 8) & 0xFF),
                    static_cast<unsigned>(version & 0xFF), static_cast<unsigned>(r[0]),
                    static_cast<unsigned>(r[1]), static_cast<unsigned>(r[2]), static_cast<unsigned>(r[3]),
                    static_cast<unsigned>(r[4]), static_cast<unsigned>(r[5]), static_cast<unsigned>(r[6]),
                    static_cast<unsigned>(r[7]), static_cast<unsigned>(tang::clockHz() / 1000));
  }
  if (!tang::read(tang::kRegStatus, r, 8) || !tang::read(tang::kRegCtrl, c, 2)) {
    return transferFailed(client, "tang");
  }
  static const char* const kSources[] = {"AUTO", "EVO", "TEST", "?"};
  return sendText(client, "tang 08..24: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                  static_cast<unsigned>(r[0]), static_cast<unsigned>(r[1]),
                  static_cast<unsigned>(r[2]), static_cast<unsigned>(r[3]),
                  static_cast<unsigned>(r[4]), static_cast<unsigned>(r[5]),
                  static_cast<unsigned>(r[6]), static_cast<unsigned>(r[7])) &&
         sendText(client,
                  "tang CTRL %08X: source %s, MUX phase %u delay %u clock %X, align %u, "
                  "SD read %u, OSD %u, aspect %s, size %s, frame blend %s; ADJUST %08X: h %d v %d; %u kHz\n",
                  static_cast<unsigned>(c[0]), kSources[c[0] & 3],
                  static_cast<unsigned>((c[0] >> 2) & 1), static_cast<unsigned>(((c[0] >> 3) & 3) + 1),
                  static_cast<unsigned>((c[0] >> 10) & 0xF), static_cast<unsigned>((c[0] >> 5) & 1),
                  static_cast<unsigned>((c[0] >> 6) & 3), static_cast<unsigned>((c[0] >> 8) & 1),
                  ((c[0] >> 14) & 1) ? "16:9" : "4:3",
                  ((c[0] >> 15) & 3) == 1 ? "95%" : ((c[0] >> 15) & 3) == 2 ? "90%" : "100%",
                  ((c[0] >> 17) & 1) ? ((r[7] >> 18) & 1 ? "on (now)" : "on") : "off",
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
