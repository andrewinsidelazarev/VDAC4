#include "zifi/diagnostic_log.hpp"

#if ZIFI_DIAGNOSTIC_LOG

#include <Arduino.h>
#include <LittleFS.h>

#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "zifi/flash_fs.hpp"

namespace zifi {

namespace {

// Журнал наладки (0.1.183, 28.11.2026): строки живут в ОЗУ, а во flash журнал попадает только в одном
// случае — при запуске после падения.
//
// Как было (0.1.49–0.1.182). Каждая строка не позже чем через секунду уходила в кольцевой файл LittleFS
// во flash. LittleFS на каждую такую запись переписывает целый блок flash в 4 КБ, а пока идёт запись,
// кэш обоих ядер ESP стоит — на десятки и сотни миллисекунд, — и вместе с ним стоят программы. WALL-E,
// например, пишет строку замера раз в 2 с: это около 1800 стираний блока в час игры и рывок каждые 2 с.
// Пользователь: «не нужно писать во флеш, а то он умрёт от перетирания», «во флеш пишем только при
// падениях программ — чтобы после сброса можно было прочитать, из-за чего они упали».
//
// Как теперь:
//  1. Кольцо в PSRAM — последние kRingCount строк этого запуска. Его отдают консоль («log»), OTA (порт
//     8267, запрос журнала) и USB модуля.
//  2. Хвост в RTC-памяти — последние kTailCount строк. RTC-память — отдельные 8 КБ ОЗУ ESP32-S3, которые
//     переживают перезапуск (паника, сторож, esp_restart), но не выключение питания. Строка кладётся туда
//     сразу, как пришла, поэтому хвост цел, даже если паника — ошибка кэша, при которой PSRAM уже не
//     прочитать.
//  3. Запуск после падения (паника, сторож задач или прерываний, аппаратный сторож, или прошивка сама
//     перезапустила ESP после зависания — diagnosticLogHangRestart) — хвост прошлого запуска и первые
//     строки этого (в них crashRecordReport уже записал причину паники) один раз пишутся во flash, в
//     файл kCrashPath. Он переживает и выключение питания; консоль показывает его перед журналом запуска.
//  4. Обычный запуск — этот файл (если он есть) только читается, один раз, в PSRAM. Сама консоль flash
//     не трогает.

constexpr char kCrashPath[] = "/zifi-crash.log";
// Кольцевой файл прежних прошивок (48 КБ): больше не нужен. Удаляется при первой записи о падении —
// во flash в этот момент пишем всё равно.
constexpr char kOldRingPath[] = "/zifi-ring.log";

constexpr size_t kTextCapacity = 156;        // знаков текста в строке (вместе с меткой источника)
constexpr size_t kRingCount = 256;           // строк в кольце PSRAM
constexpr size_t kInternalRingCount = 16;    // PSRAM нет — столько строк во внутреннем ОЗУ
constexpr size_t kTailCount = 32;            // строк в хвосте RTC
constexpr size_t kLineCapacity = 320;        // одна строка текстом: номер, время, память и сам текст
constexpr size_t kCrashCapacity = 24 * 1024; // запись о падении: до 32 строк до него и до 40 после

// Одна строка журнала: номер, время от запуска, память в этот момент и сам текст. Вид один — и в
// кольце PSRAM, и в хвосте RTC, поэтому строка переходит из одного в другое простым копированием.
struct Record {
  uint32_t sequence;   // номер строки с запуска ESP, с 1
  uint32_t ms;         // миллисекунд от запуска ESP
  uint32_t ram;        // свободно внутреннего ОЗУ, байт
  uint32_t ramMin;     // меньше всего его было с запуска
  uint32_t psram;      // свободно PSRAM
  uint32_t psramMin;
  uint16_t length;     // знаков в text, без нуля в конце
  char text[kTextCapacity];
};

// Хвост в RTC-памяти. RTC_NOINIT_ATTR — «не обнулять при запуске»: так содержимое прошлого запуска
// доживает до этого. После включения питания там случайные байты, поэтому есть метка и проверка.
constexpr uint32_t kTailMagic = 0x4C494154;   // «TAIL»
constexpr uint32_t kHangMark = 0x474E4148;    // «HANG»: прошивка перезапустила ESP сама после зависания
struct Tail {
  uint32_t magic;
  uint32_t check;      // tailCheck(): сходится — поля ниже записаны этой прошивкой
  uint32_t next;       // куда ляжет следующая строка (0…kTailCount-1)
  uint32_t count;      // сколько строк в хвосте (до kTailCount)
  uint32_t hang;       // kHangMark или 0
  Record lines[kTailCount];
};
// RTC-память для данных — 8 КБ, и там же запись о панике (crash_record.cpp, ~0,4 КБ): хвост берёт не
// больше 6 КБ (32 строки по 184 байта).
static_assert(sizeof(Tail) <= 6 * 1024, "хвост журнала не влезет в RTC-память");
RTC_NOINIT_ATTR Tail g_tail;

SemaphoreHandle_t g_mutex = nullptr;   // кольцо, хвост и запись о падении — под ним
bool g_ready = false;
uint32_t g_nextSequence = 1;

Record* g_ring = nullptr;    // кольцо строк этого запуска
size_t g_capacity = 0;       // строк в нём помещается
size_t g_head = 0;           // где самая старая
size_t g_count = 0;          // сколько строк сейчас

// Хвост прошлого запуска: забран из RTC в diagnosticLogBegin, нужен только до diagnosticLogCrashFile.
Record* g_previous = nullptr;
size_t g_previousCount = 0;
bool g_previousHang = false;

// Текст последней записи о падении (прочитан из flash или только что записан) — для консоли.
char* g_crashText = nullptr;
size_t g_crashLength = 0;

bool take(TickType_t wait) { return g_mutex != nullptr && xSemaphoreTake(g_mutex, wait) == pdTRUE; }

void give() {
  if (g_mutex != nullptr) {
    xSemaphoreGive(g_mutex);
  }
}

uint32_t tailCheck(const Tail& tail) { return tail.magic ^ tail.next ^ (tail.count << 8) ^ tail.hang; }

bool tailValid() {
  return g_tail.magic == kTailMagic && g_tail.next < kTailCount && g_tail.count <= kTailCount &&
         g_tail.check == tailCheck(g_tail);
}

void tailReset() {
  g_tail.magic = kTailMagic;
  g_tail.next = 0;
  g_tail.count = 0;
  g_tail.hang = 0;
  g_tail.check = tailCheck(g_tail);
}

// Строка — в хвост RTC: сначала сама строка, потом указатели и проверка. Паника посреди этого портит
// одну строку (её длину чинит copyTail), но не весь хвост.
void tailPut(const Record& record) {
  g_tail.lines[g_tail.next] = record;
  g_tail.next = (g_tail.next + 1) % kTailCount;
  if (g_tail.count < kTailCount) {
    ++g_tail.count;
  }
  g_tail.check = tailCheck(g_tail);
}

void* allocate(size_t bytes) {
  void* memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return memory != nullptr ? memory : heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

bool allocateRing() {
  if (g_ring != nullptr) {
    return true;
  }
  g_capacity = kRingCount;
  g_ring = static_cast<Record*>(heap_caps_malloc(g_capacity * sizeof(Record), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (g_ring == nullptr) {
    g_capacity = kInternalRingCount;
    g_ring = static_cast<Record*>(
        heap_caps_malloc(g_capacity * sizeof(Record), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (g_ring == nullptr) {
    g_capacity = 0;
    return false;
  }
  g_head = 0;
  g_count = 0;
  return true;
}

// Хвост прошлого запуска — из RTC в PSRAM, от старой строки к новой.
void copyTail() {
  g_previousCount = 0;
  g_previousHang = false;
  if (!tailValid() || g_tail.count == 0) {
    return;
  }
  g_previous = static_cast<Record*>(allocate(g_tail.count * sizeof(Record)));
  if (g_previous == nullptr) {
    return;
  }
  const size_t first = (g_tail.next + kTailCount - g_tail.count) % kTailCount;
  for (size_t index = 0; index < g_tail.count; ++index) {
    Record& line = g_previous[index];
    line = g_tail.lines[(first + index) % kTailCount];
    if (line.length > kTextCapacity) {
      line.length = kTextCapacity;   // строка, которую писали в миг паники
    }
  }
  g_previousCount = g_tail.count;
  g_previousHang = g_tail.hang == kHangMark;
}

// Строка журнала — текстом, как её показывает консоль. Возвращает длину или 0.
size_t formatRecord(const Record& record, char* out, size_t size) {
  const int length = snprintf(out, size, "seq=%lu ms=%lu ram=%lu ram_min=%lu psram=%lu psram_min=%lu %.*s\n",
                              static_cast<unsigned long>(record.sequence),
                              static_cast<unsigned long>(record.ms), static_cast<unsigned long>(record.ram),
                              static_cast<unsigned long>(record.ramMin),
                              static_cast<unsigned long>(record.psram),
                              static_cast<unsigned long>(record.psramMin), static_cast<int>(record.length),
                              record.text);
  return length > 0 && static_cast<size_t>(length) < size ? static_cast<size_t>(length) : 0;
}

// Дописать в буфер out (capacity байт, занято used) текст; не влезает — false, буфер прежний.
bool append(char* out, size_t capacity, size_t& used, const char* text, size_t length) {
  if (used + length > capacity) {
    return false;
  }
  memcpy(out + used, text, length);
  used += length;
  return true;
}

bool appendRecord(char* out, size_t capacity, size_t& used, const Record& record) {
  char line[kLineCapacity];
  const size_t length = formatRecord(record, line, sizeof(line));
  return length != 0 && append(out, capacity, used, line, length);
}

bool appendLine(char* out, size_t capacity, size_t& used, const char* format, ...) {
  char line[kLineCapacity];
  va_list arguments;
  va_start(arguments, format);
  const int length = vsnprintf(line, sizeof(line), format, arguments);
  va_end(arguments);
  return length > 0 && static_cast<size_t>(length) < sizeof(line) &&
         append(out, capacity, used, line, static_cast<size_t>(length));
}

const Record& ringRecord(size_t index) { return g_ring[(g_head + index) % g_capacity]; }

// Строку — в кольцо PSRAM и в хвост RTC; занятый журнал ждать не дольше wait.
void enqueueEvent(TickType_t wait, const char* format, va_list arguments) {
  if (!g_ready || format == nullptr) {
    return;
  }
  Record record = {};
  // Форматирование — до замка: оно не задерживает другую задачу, которая как раз пишет свою строку.
  const int formatted = vsnprintf(record.text, sizeof(record.text), format, arguments);
  record.length = formatted <= 0 ? 0 : static_cast<uint16_t>(strnlen(record.text, sizeof(record.text)));
  record.ms = millis();
  record.ram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  record.ramMin = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  record.psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  record.psramMin = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!take(wait)) {
    return;
  }
  record.sequence = g_nextSequence++;
  if (g_nextSequence == 0) {
    g_nextSequence = 1;
  }
  if (g_count == g_capacity) {
    g_head = (g_head + 1) % g_capacity;   // кольцо полно: самая старая строка уходит
    --g_count;
  }
  g_ring[(g_head + g_count) % g_capacity] = record;
  ++g_count;
  tailPut(record);
  give();
}

const char* resetName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "hardware watchdog";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    case ESP_RST_SW: return "restart by the firmware";
    default: return "other";
  }
}

// Падение ли это: ESP перезапустилась из-за ошибки программы или прошивки, а не по питанию, кнопке,
// OTA и прочим обычным перезапускам.
bool crashReset(esp_reset_reason_t reason, bool hang) {
  return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT ||
         reason == ESP_RST_WDT || reason == ESP_RST_CPU_LOCKUP || hang;
}

// Запуск после падения: запись о нём — текстом в PSRAM и одним файлом во flash.
void saveCrash(esp_reset_reason_t reason) {
  char* text = static_cast<char*>(allocate(kCrashCapacity));
  if (text == nullptr) {
    return;
  }
  size_t used = 0;
  appendLine(text, kCrashCapacity, used, "----- crash: reset reason %d (%s)%s, firmware %s -----\n",
             static_cast<int>(reason), resetName(reason),
             g_previousHang ? " after a hang" : "", ZIFI_BUILD_VERSION);
  appendLine(text, kCrashCapacity, used, "----- the last %u lines before the restart -----\n",
             static_cast<unsigned>(g_previousCount));
  for (size_t index = 0; index < g_previousCount; ++index) {
    if (!appendRecord(text, kCrashCapacity, used, g_previous[index])) {
      break;
    }
  }
  appendLine(text, kCrashCapacity, used, "----- after the restart -----\n");
  if (take(pdMS_TO_TICKS(1000))) {
    for (size_t index = 0; index < g_count; ++index) {
      if (!appendRecord(text, kCrashCapacity, used, ringRecord(index))) {
        break;
      }
    }
    give();
  }
  // Единственная запись журнала во flash: файл целиком, один раз за запуск после падения.
  bool saved = false;
  File file = LittleFS.open(kCrashPath, "w");
  if (file) {
    saved = file.write(reinterpret_cast<const uint8_t*>(text), used) == used;
    file.close();
  }
  if (LittleFS.exists(kOldRingPath)) {
    LittleFS.remove(kOldRingPath);
  }
  if (take(pdMS_TO_TICKS(1000))) {
    g_crashText = text;
    g_crashLength = used;
    give();
  } else {
    heap_caps_free(text);
  }
  diagnosticLogEvent("LOG crash record %s flash: %u bytes", saved ? "saved to" : "NOT saved to",
                     static_cast<unsigned>(used));
}

// Обычный запуск: прошлая запись о падении (если есть) — из flash в PSRAM, только чтение.
void loadCrash() {
  File file = LittleFS.open(kCrashPath, "r");
  if (!file) {
    return;
  }
  const size_t size = file.size();
  char* text = size > 0 && size <= kCrashCapacity ? static_cast<char*>(allocate(size)) : nullptr;
  if (text != nullptr && file.read(reinterpret_cast<uint8_t*>(text), size) == size && take(pdMS_TO_TICKS(1000))) {
    g_crashText = text;
    g_crashLength = size;
    give();
  } else {
    heap_caps_free(text);
  }
  file.close();
}

}  // namespace

bool diagnosticLogBegin() {
  if (g_mutex == nullptr) {
    g_mutex = xSemaphoreCreateMutex();
  }
  if (!take(pdMS_TO_TICKS(1000))) {
    return false;
  }
  const bool ok = allocateRing();
  copyTail();   // хвост прошлого запуска; было ли падение, решит diagnosticLogCrashFile
  tailReset();  // дальше в RTC — строки этого запуска
  g_ready = ok;
  give();
  return ok;
}

void diagnosticLogEvent(const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  enqueueEvent(0, format, arguments);
  va_end(arguments);
}

void diagnosticLogEventWait(uint32_t timeoutMs, const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  enqueueEvent(pdMS_TO_TICKS(timeoutMs), format, arguments);
  va_end(arguments);
}

void diagnosticLogCrashFile() {
  if (!g_ready) {
    return;
  }
  const esp_reset_reason_t reason = esp_reset_reason();
  // Раздел LittleFS не форматировать, если он не монтируется: в нём кэш модулей и настройки.
  if (flashFsMount(false)) {
    if (crashReset(reason, g_previousHang)) {
      saveCrash(reason);
    } else {
      loadCrash();
    }
    flashFsUnmount();
  }
  heap_caps_free(g_previous);
  g_previous = nullptr;
  g_previousCount = 0;
}

void diagnosticLogHangRestart() {
  // Без замка, если журнал занят: ESP сейчас перезапустится, а отметка — одно слово и проверка.
  const bool locked = take(pdMS_TO_TICKS(100));
  g_tail.hang = kHangMark;
  g_tail.check = tailCheck(g_tail);
  if (locked) {
    give();
  }
}

bool diagnosticLogSnapshot(DiagnosticLogSnapshot& snapshot) {
  snapshot.data = nullptr;
  snapshot.length = 0;
  if (!g_ready || !take(pdMS_TO_TICKS(1000))) {
    return false;
  }
  const size_t capacity = g_crashLength + 256 + g_count * kLineCapacity;
  char* output = static_cast<char*>(allocate(capacity));
  if (output == nullptr) {
    give();
    return false;
  }
  size_t used = 0;
  if (g_crashText != nullptr) {
    appendLine(output, capacity, used, "----- the last crash, kept in flash -----\n");
    append(output, capacity, used, g_crashText, g_crashLength);
    appendLine(output, capacity, used, "----- this run -----\n");
  }
  for (size_t index = 0; index < g_count; ++index) {
    if (!appendRecord(output, capacity, used, ringRecord(index))) {
      break;
    }
  }
  give();
  snapshot.data = reinterpret_cast<uint8_t*>(output);
  snapshot.length = used;
  return true;
}

void diagnosticLogFreeSnapshot(DiagnosticLogSnapshot& snapshot) {
  heap_caps_free(snapshot.data);
  snapshot.data = nullptr;
  snapshot.length = 0;
}

}  // namespace zifi

extern "C" void zifi_diagnostic_log_rpc_stage(const char* stage,
                                               int32_t valueA,
                                               int32_t valueB) {
  zifi::diagnosticLogEvent("RPC stage=%s a=%ld b=%ld",
                           stage == nullptr ? "(null)" : stage,
                           static_cast<long>(valueA),
                           static_cast<long>(valueB));
}

#else

// libsmb2 содержит несколько C-вызовов диагностических этапов DCE/RPC.
// Оставляем символ для независимой линковки, но в обычной прошивке он пуст.
extern "C" void zifi_diagnostic_log_rpc_stage(const char*, int32_t, int32_t) {}

#endif
