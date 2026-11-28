// Стенд журнала прошивки (src/diagnostic_log.cpp, 0.1.183) на ПК: тот же исходник, что в прошивке,
// собирается MSVC с моделью LittleFS (tests/stubs_diagnostic_log/LittleFS.h), которая считает каждую
// операцию с flash. Печатает OK или список расхождений; код возврата 0 — всё сошлось.
//
// Что проверяется:
//  - в обычной работе журнал flash не трогает: ни строка, ни снимок для консоли, ни 300 строк подряд;
//    при запуске — одно чтение прошлой записи о падении, записи нет;
//  - кольцо держит последние 256 строк по порядку, строка — в том же виде, что прежде
//    («seq=… ms=… ram=… ram_min=… psram=… psram_min=… текст»);
//  - запуск после паники пишет во flash ровно один файл: последние 32 строки прошлого запуска (хвост
//    в RTC-памяти), затем первые строки этого с причиной паники; старый кольцевой файл удаляется;
//  - обычный перезапуск запись о падении только читает, снимок показывает её перед журналом запуска;
//  - перезапуск прошивкой после зависания (diagnosticLogHangRestart) — тоже падение; сторож задач — тоже;
//    провал питания (brownout) и включение питания — нет;
//  - после выключения питания RTC-память — мусор: хвост не принимается, строк до падения — 0;
//  - строка, которую писали в миг паники (испорченная длина), не ломает запись о падении;
//  - журнал занят другой задачей: строка без ожидания теряется, снимок отказывает, ничего не портится.

#define ZIFI_DIAGNOSTIC_LOG 1
#define ZIFI_BUILD_VERSION "vdac4-test"
#include "../src/diagnostic_log.cpp"

#include <string>

FlashModel g_flash;
LittleFSModel LittleFS;

namespace {

uint32_t g_now = 0;
esp_reset_reason_t g_reason = ESP_RST_POWERON;
int g_mounts = 0;
int g_unmounts = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
  if (!condition) {
    ++g_failures;
    printf("FAIL: %s\n", what);
  }
}

}  // namespace

uint32_t millis() { return g_now; }
esp_reset_reason_t esp_reset_reason() { return g_reason; }

namespace zifi {
bool flashFsMount(bool) {
  ++g_mounts;
  return true;
}
void flashFsUnmount() { ++g_unmounts; }
}  // namespace zifi

namespace {

// Перезапуск ESP: ОЗУ и PSRAM — заново, RTC-память (zifi::g_tail) остаётся как была.
void restart(esp_reset_reason_t reason) {
  free(zifi::g_ring);
  zifi::g_ring = nullptr;
  zifi::g_capacity = 0;
  zifi::g_head = 0;
  zifi::g_count = 0;
  zifi::g_ready = false;
  zifi::g_nextSequence = 1;
  free(zifi::g_crashText);
  zifi::g_crashText = nullptr;
  zifi::g_crashLength = 0;
  free(zifi::g_previous);
  zifi::g_previous = nullptr;
  zifi::g_previousCount = 0;
  zifi::g_previousHang = false;
  g_reason = reason;
  g_now = 120;
}

// Выключение питания: в RTC-памяти — случайные байты.
void powerOff() { memset(&zifi::g_tail, 0xA5, sizeof(zifi::g_tail)); }

// Запуск — в том же порядке, что setup() в main.cpp.
void boot(esp_reset_reason_t reason, const char* panic = nullptr) {
  restart(reason);
  check(zifi::diagnosticLogBegin(), "diagnosticLogBegin");
  zifi::diagnosticLogEvent("BOOT firmware=%s reset_reason=%d", ZIFI_BUILD_VERSION, static_cast<int>(reason));
  if (panic != nullptr) {
    zifi::diagnosticLogEvent("zifi-crash: last panic: %s", panic);   // как crashRecordReport
  }
  zifi::diagnosticLogCrashFile();
}

void logLines(int first, int last) {
  for (int line = first; line <= last; ++line) {
    g_now += 7;
    zifi::diagnosticLogEvent("zifi-app: line %d", line);
  }
}

std::string snapshot() {
  zifi::DiagnosticLogSnapshot copy{};
  if (!zifi::diagnosticLogSnapshot(copy)) {
    return "(no snapshot)";
  }
  std::string text(reinterpret_cast<const char*>(copy.data), copy.length);
  zifi::diagnosticLogFreeSnapshot(copy);
  return text;
}

size_t countLines(const std::string& text) {
  size_t lines = 0;
  for (char c : text) {
    lines += c == '\n';
  }
  return lines;
}

bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

const std::string& crashFile() { return g_flash.files["/zifi-crash.log"]; }

}  // namespace

int main() {
  // 1. Включение питания, flash пуста: журнал работает, во flash ничего не пишет
  powerOff();
  boot(ESP_RST_POWERON);
  check(g_flash.writeOpens == 0 && g_flash.removes == 0, "power on: nothing written to flash");
  check(g_flash.readOpens == 1, "power on: one read of the crash file");
  logLines(1, 300);
  const std::string normal = snapshot();
  check(g_flash.writeOpens == 0 && g_flash.readOpens == 1 && g_flash.removes == 0,
        "300 lines and a snapshot do not touch flash");
  check(countLines(normal) == 256, "ring keeps the last 256 lines");
  check(normal.rfind("seq=46 ms=", 0) == 0, "oldest kept line is seq 46 (BOOT is seq 1)");
  check(has(normal, "seq=301 ms=2220 ram=40000 ram_min=30000 psram=7000000 psram_min=6000000 zifi-app: line 300\n"),
        "line format as before");
  check(!has(normal, "line 44\n") && has(normal, "line 45\n"), "line 44 dropped, line 45 kept");
  check(!has(normal, "-----"), "no crash section without a crash file");
  check(normal.find("line 299\n") < normal.find("line 300\n"), "ring order");

  // 2. Паника: один файл во flash — хвост прошлого запуска и начало этого; старый кольцевой файл — прочь
  g_flash.files["/zifi-ring.log"] = std::string(49152, '\0');
  boot(ESP_RST_PANIC, "LoadProhibited (WALL-E)");
  check(g_flash.writeOpens == 1, "panic: the crash file written once");
  check(g_flash.files.count("/zifi-ring.log") == 0, "panic: the old ring file removed");
  const std::string panic = crashFile();
  check(panic.rfind("----- crash: reset reason 4 (panic), firmware vdac4-test -----\n", 0) == 0, "panic: header");
  check(has(panic, "----- the last 32 lines before the restart -----\n"), "panic: 32 lines before");
  check(has(panic, "zifi-app: line 269\n") && has(panic, "zifi-app: line 300\n") && !has(panic, "line 268\n"),
        "panic: the tail is lines 269..300");
  check(panic.find("line 269\n") < panic.find("line 300\n"), "panic: tail order");
  check(panic.find("line 300\n") < panic.find("----- after the restart -----\n"), "panic: tail, then this run");
  check(has(panic, "BOOT firmware=vdac4-test reset_reason=4\n") &&
            has(panic, "zifi-crash: last panic: LoadProhibited (WALL-E)\n"),
        "panic: the first lines after the restart");
  const std::string afterPanic = snapshot();
  check(afterPanic.rfind("----- the last crash, kept in flash -----\n" + panic + "----- this run -----\n", 0) == 0,
        "panic: snapshot shows the crash first");
  check(has(afterPanic, "LOG crash record saved to flash: "), "panic: the save is logged");
  logLines(1, 100);
  snapshot();
  check(g_flash.writeOpens == 1, "after the crash save no more writes");

  // 3. Обычный перезапуск: запись о падении только читается и видна в консоли
  const int readsBefore = g_flash.readOpens;
  boot(ESP_RST_SW);
  check(g_flash.writeOpens == 1, "restart: nothing written");
  check(g_flash.readOpens == readsBefore + 1, "restart: the crash file read once");
  const std::string restarted = snapshot();
  check(restarted.rfind("----- the last crash, kept in flash -----\n" + panic, 0) == 0,
        "restart: the same crash record shown");
  check(countLines(restarted) == countLines(panic) + 3, "restart: crash, two markers and BOOT");

  // 4. Прошивка перезапускает ESP после зависания: это падение
  logLines(1, 3);
  zifi::diagnosticLogEvent("zifi-watchdog: link stuck 15 s; restarting");
  zifi::diagnosticLogHangRestart();
  boot(ESP_RST_SW);
  check(g_flash.writeOpens == 2, "hang: the crash file written");
  check(crashFile().rfind("----- crash: reset reason 3 (restart by the firmware) after a hang, ", 0) == 0,
        "hang: header");
  check(has(crashFile(), "link stuck 15 s; restarting\n"), "hang: the watchdog line kept");
  // следующий обычный перезапуск отметку о зависании не наследует
  boot(ESP_RST_SW);
  check(g_flash.writeOpens == 2, "hang mark is reset at the next start");

  // 5. Питание выключали: хвоста нет, но паника всё равно записана
  powerOff();
  boot(ESP_RST_PANIC, "x");
  check(g_flash.writeOpens == 3, "power off + panic: written");
  check(has(crashFile(), "----- the last 0 lines before the restart -----\n"), "power off: garbage tail refused");

  // 6. Строка, которую писали в миг паники: длина испорчена — запись о падении цела
  logLines(1, 2);
  zifi::g_tail.lines[0].length = 999;
  boot(ESP_RST_TASK_WDT);
  check(g_flash.writeOpens == 4, "task watchdog: written");
  check(has(crashFile(), "(task watchdog)") && has(crashFile(), "zifi-app: line 2\n"), "task watchdog: content");

  // 7. Провал питания — не падение программы
  boot(ESP_RST_BROWNOUT);
  check(g_flash.writeOpens == 4, "brownout: nothing written");

  // 8. Длинная строка обрезается, журнал занят — строка без ожидания теряется, снимок отказывает
  std::string longText(300, 'L');
  zifi::diagnosticLogEvent("%s", longText.c_str());
  check(has(snapshot(), (std::string(155, 'L') + "\n").c_str()), "long line cut to 155 characters");
  zifi::g_mutex->taken = true;
  zifi::diagnosticLogEvent("zifi-app: dropped");
  zifi::DiagnosticLogSnapshot busy{};
  check(!zifi::diagnosticLogSnapshot(busy) && busy.data == nullptr, "busy: snapshot refused");
  zifi::g_mutex->taken = false;
  check(!has(snapshot(), "dropped"), "busy: the line was not half-written");
  check(g_flash.writeOpens == 4 && g_mounts == g_unmounts, "flash: mounts balanced, no extra writes");

  if (g_failures == 0) {
    printf("OK\n");
  }
  return g_failures == 0 ? 0 : 1;
}
