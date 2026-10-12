#include "zifi/crash_record.hpp"

#if ZIFI_HAS_SPI_LINK

#include <esp_attr.h>
#include <stdio.h>
#include <string.h>

#include "zifi/bringup_log.hpp"

extern "C" bool spi_flash_cache_enabled(void);
extern "C" int Cache_Invalidate_Addr(uint32_t addr, uint32_t size);   // ПЗУ ESP32-S3

#if ZIFI_CRASH_RECORD
#include <esp32-hal.h>
#include <esp_private/panic_internal.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Сторож задач ESP-IDF (task_wdt.c) ставит его, когда отдаёт обработчику паники
// сохранённый кадр задачи, которую прервал: причина исключения в таком кадре —
// мусор (IllegalInstruction, Syscall…), а PC — где задача была.
extern "C" bool g_twdt_isr;
#endif

namespace zifi {
namespace {

constexpr char kTag[] = "zifi-crash";
constexpr uint32_t kMagic = 0x48535243;   // "CRSH"
// Код модулей лежит в PSRAM: шина данных видит его с 0x3C000000, шина команд
// (PC, адреса возврата) — с 0x42000000.
constexpr uint32_t kCodeBusOffset = 0x42000000 - 0x3C000000;
constexpr size_t kCodeBytes = 16;
constexpr size_t kModules = 6;
constexpr size_t kDepth = 12;

struct ModuleRange {
  char name[13];
  uint32_t base;       // где код модуля (шина данных)
  uint32_t size;
  uint32_t vaddr;      // адрес этого кода в самом ELF
  uint32_t pristine;   // копия кода при загрузке (0 — нет)
};

struct CrashRecord {
  uint32_t magic;
  int32_t core;
  uint32_t pc;
  uint32_t depth;      // адресов в backtrace
  uint32_t backtrace[kDepth];
  uint8_t corrupt;     // стек разобрать до конца не удалось
  uint8_t more;        // адресов больше, чем поместилось
  char reason[96];
  char task[16];
  ModuleRange modules[kModules];
  uint32_t check;      // после включения питания RTC-память случайна
  // PC в коде модуля: 16 байтов с codeAt (шина данных) — в памяти и в копии.
  uint32_t codeAt;
  uint8_t code[kCodeBytes];
  uint8_t pristine[kCodeBytes];
  uint8_t codeState;   // 0 — нет, 1 — только память, 2 — память и копия
  uint8_t fresh[kCodeBytes];   // те же байты из PSRAM (строки кэша данных сброшены)
  uint8_t freshValid;
  int16_t temperature;         // кристалл перед паникой, десятые доли °C (INT16_MIN — нет)
};

RTC_NOINIT_ATTR CrashRecord g_crash;
// Где модули сейчас (при панике копируется в запись).
ModuleRange g_modules[kModules];
// Последняя температура кристалла.
volatile int16_t g_temperature = INT16_MIN;

uint32_t IRAM_ATTR checksum(const CrashRecord& record) {
  uint32_t sum = kMagic ^ record.pc ^ record.depth;
  for (size_t index = 0; index < kDepth; ++index) {
    sum = (sum << 1 | sum >> 31) ^ record.backtrace[index];
  }
  return sum;
}

void IRAM_ATTR copyText(char* out, size_t size, const char* text) {
  size_t index = 0;
  if (text != nullptr) {
    for (; index + 1 < size && text[index] != 0; ++index) {
      out[index] = text[index];
    }
  }
  out[index] = 0;
}

const ModuleRange* moduleOf(const CrashRecord& record, uint32_t address) {
  if (address >= 0x42000000 && address < 0x44000000) {
    address -= kCodeBusOffset;           // PC и адреса возврата — шина команд
  }
  for (const ModuleRange& range : record.modules) {
    if (range.base != 0 && address >= range.base && address - range.base < range.size) {
      return &range;
    }
  }
  return nullptr;
}

#if ZIFI_CRASH_RECORD
// Вызывается из обработчика паники (обёртка Arduino-ядра над esp_panic_handler).
// Сначала то, что не зовёт кода во flash; имя задачи — последним.
void IRAM_ATTR onPanic(arduino_panic_info_t* info, void*) {
  CrashRecord& record = g_crash;
  record.magic = 0;
  record.core = info->core;
  record.pc = reinterpret_cast<uint32_t>(info->pc);
  record.depth = info->backtrace_len < kDepth ? info->backtrace_len : kDepth;
  for (size_t index = 0; index < kDepth; ++index) {
    record.backtrace[index] = index < record.depth ? info->backtrace[index] : 0;
  }
  record.corrupt = info->backtrace_corrupt ? 1 : 0;
  record.more = info->backtrace_continues || info->backtrace_len > kDepth ? 1 : 0;
  copyText(record.reason, sizeof(record.reason),
           g_twdt_isr ? "Task watchdog (a task kept the core busy for 5 s; PC is where it was)"
           : g_panic_abort && g_panic_abort_details != nullptr ? g_panic_abort_details
                                                               : info->reason);
  for (size_t index = 0; index < kModules; ++index) {
    record.modules[index] = g_modules[index];
  }
  record.temperature = g_temperature;
  // Код у PC, если PC в модуле: что в памяти и что было при загрузке. У сторожа
  // задач PC — просто где задача была: байты не нужны.
  record.codeState = 0;
  const uint32_t pc = record.pc >= 0x42000000 && record.pc < 0x44000000 && !g_twdt_isr
                          ? record.pc - kCodeBusOffset
                          : 0;
  for (size_t index = 0; index < kModules && pc != 0 && spi_flash_cache_enabled(); ++index) {
    const ModuleRange& range = record.modules[index];
    if (range.base == 0 || pc < range.base || pc - range.base >= range.size) {
      continue;
    }
    uint32_t at = (pc & ~3u) >= range.base + 4 ? (pc & ~3u) - 4 : range.base;
    if (at + kCodeBytes > range.base + range.size) {
      at = range.base + range.size - kCodeBytes;
    }
    record.codeAt = at;
    const volatile uint8_t* code = reinterpret_cast<const volatile uint8_t*>(at);
    const volatile uint8_t* copy =
        range.pristine != 0 ? reinterpret_cast<const volatile uint8_t*>(range.pristine + (at - range.base))
                            : nullptr;
    for (size_t byte = 0; byte < kCodeBytes; ++byte) {
      record.code[byte] = code[byte];
      record.pristine[byte] = copy != nullptr ? copy[byte] : 0;
    }
    record.codeState = copy != nullptr ? 2 : 1;
    // И из самой PSRAM: строки кэша данных над ними — прочь (строки внутри кода
    // модуля чужих несохранённых данных не держат).
    record.freshValid = 0;
    const uint32_t line = at & ~31u;
    if (line >= range.base + 32 && line + 64 <= range.base + range.size - 32) {
      Cache_Invalidate_Addr(line, 64);
      for (size_t byte = 0; byte < kCodeBytes; ++byte) {
        record.fresh[byte] = code[byte];
      }
      record.freshValid = 1;
    }
    break;
  }
  record.task[0] = 0;
  record.check = checksum(record);
  record.magic = kMagic;
  // Номер ядра у паники высоким прерыванием (сторож прерываний, ошибка кэша) бывает
  // недопустимым: xTaskGetCurrentTaskHandleForCore с ним — assert, и вложенная паника
  // затирала эту запись (сброс MIDI.ELF 1.7 записался как assert в обработчике).
  const int core = (info->core >= 0 && info->core < portNUM_PROCESSORS) ? info->core : static_cast<int>(xPortGetCoreID());
  TaskHandle_t task = xTaskGetCurrentTaskHandleForCore(core);
  if (task != nullptr) {
    copyText(record.task, sizeof(record.task), pcTaskGetName(task));
  }
}
#endif

}  // namespace

void crashRecordModule(const char* name, const void* base, size_t size, uint32_t vaddr,
                       const void* pristine) {
  for (ModuleRange& range : g_modules) {
    if (range.base == 0) {
      copyText(range.name, sizeof(range.name), name);
      range.size = static_cast<uint32_t>(size);
      range.vaddr = vaddr;
      range.pristine = reinterpret_cast<uint32_t>(pristine);
      range.base = reinterpret_cast<uint32_t>(base);
      return;
    }
  }
}

void crashRecordTemperature(int16_t tenths) { g_temperature = tenths; }

void crashRecordModuleGone(const void* base) {
  for (ModuleRange& range : g_modules) {
    if (range.base == reinterpret_cast<uint32_t>(base)) {
      memset(&range, 0, sizeof(range));
    }
  }
}

void crashRecordReport() {
#if ZIFI_CRASH_RECORD
  set_arduino_panic_handler(onPanic, nullptr);
#endif
  CrashRecord record = g_crash;
  g_crash.magic = 0;
  if (record.magic != kMagic || record.depth > kDepth || record.check != checksum(record)) {
    return;
  }
  record.reason[sizeof(record.reason) - 1] = 0;
  record.task[sizeof(record.task) - 1] = 0;
  bringupLog(kTag, "last panic: %s", record.reason);
  bringupLog(kTag, "core %d, task %s, PC %08X", static_cast<int>(record.core),
             record.task[0] != 0 ? record.task : "?", static_cast<unsigned>(record.pc));
  if (record.temperature != INT16_MIN) {
    const int tenths = record.temperature;
    bringupLog(kTag, "chip temperature before the panic: %d.%d C", tenths / 10,
               (tenths < 0 ? -tenths : tenths) % 10);
  }
  char line[128];
  size_t used = 0;
  for (size_t index = 0; index < record.depth && used + 10 < sizeof(line); ++index) {
    used += static_cast<size_t>(snprintf(line + used, sizeof(line) - used, " %08X",
                                         static_cast<unsigned>(record.backtrace[index])));
  }
  line[used] = 0;
  bringupLog(kTag, "backtrace%s%s", line,
             record.corrupt ? " CORRUPTED" : (record.more ? " ..." : ""));
  // Адреса из модулей ESP — как «ИМЯ+адрес в ELF» для addr2line.
  for (size_t index = 0; index <= record.depth; ++index) {
    const uint32_t address = index == 0 ? record.pc : record.backtrace[index - 1];
    const ModuleRange* range = moduleOf(record, address);
    if (range != nullptr) {
      const uint32_t data = address >= 0x42000000 && address < 0x44000000
                                ? address - kCodeBusOffset
                                : address;
      bringupLog(kTag, "%08X = %s+%X", static_cast<unsigned>(address), range->name,
                 static_cast<unsigned>(data - range->base + range->vaddr));
    }
  }
  for (const ModuleRange& range : record.modules) {
    if (range.base != 0) {
      bringupLog(kTag, "module %s: code at %08X, %u bytes, ELF %X", range.name,
                 static_cast<unsigned>(range.base), static_cast<unsigned>(range.size),
                 static_cast<unsigned>(range.vaddr));
    }
  }
  if (record.codeState != 0) {
    const ModuleRange* range = moduleOf(record, record.codeAt);
    char now[3 * kCodeBytes + 1];
    char was[3 * kCodeBytes + 1];
    bool same = true;
    for (size_t byte = 0; byte < kCodeBytes; ++byte) {
      snprintf(now + 3 * byte, 4, " %02X", record.code[byte]);
      snprintf(was + 3 * byte, 4, " %02X", record.pristine[byte]);
      same = same && record.code[byte] == record.pristine[byte];
    }
    bringupLog(kTag, "code at %08X (%s+%X) in memory:%s", static_cast<unsigned>(record.codeAt),
               range != nullptr ? range->name : "?",
               range != nullptr ? static_cast<unsigned>(record.codeAt - range->base + range->vaddr) : 0u,
               now);
    if (record.freshValid) {
      char fresh[3 * kCodeBytes + 1];
      for (size_t byte = 0; byte < kCodeBytes; ++byte) {
        snprintf(fresh + 3 * byte, 4, " %02X", record.fresh[byte]);
      }
      bringupLog(kTag, "code in PSRAM itself:%s", fresh);
    }
    if (record.codeState == 2) {
      bringupLog(kTag, "code as loaded:%s%s", was,
                 same ? " (memory intact: the CPU fetched other bytes)" : " (MEMORY CHANGED)");
    }
  }
}

}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
