#include "zifi/bringup_log.hpp"

#if ZIFI_HAS_SPI_LINK

#include <driver/usb_serial_jtag.h>
#include <esp_log.h>
#include <stdarg.h>
#include <stdio.h>

#include "zifi/diagnostic_log.hpp"

namespace zifi {
namespace {

constexpr char kTag[] = "zifi-diag";
// В запись постоянного журнала входит 156 знаков вместе с меткой источника.
constexpr size_t kLineSize = 136;
constexpr uint32_t kStoreWaitMs = 200;

}  // namespace

void bringupLog(const char* tag, const char* format, ...) {
  char line[kLineSize];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(line, sizeof(line), format, arguments);
  va_end(arguments);
  ESP_LOGI(tag, "%s", line);
  diagnosticLogEventWait(kStoreWaitMs, "%s: %s", tag, line);
}

void bringupPrintStoredLog() {
#if ZIFI_DIAGNOSTIC_LOG
  if (!usb_serial_jtag_is_connected()) {
    return;
  }
  DiagnosticLogSnapshot snapshot{};
  if (!diagnosticLogSnapshot(snapshot)) {
    ESP_LOGW(kTag, "stored log is not available");
    return;
  }
  printf("----- stored log: %u bytes -----\n",
         static_cast<unsigned>(snapshot.length));
  fwrite(snapshot.data, 1, snapshot.length, stdout);
  printf("----- stored log end -----\n");
  fflush(stdout);
  diagnosticLogFreeSnapshot(snapshot);
#endif
}

}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
