#include "zifi/elf_probe.hpp"

#include "zifi/board.hpp"

#if ZIFI_HAS_AUDIO && defined(ZIFI_ELF_PROBE)

#include <stdlib.h>

#include <esp_heap_caps.h>
#include <esp_timer.h>

#include "esp_elf.h"
#include "zifi/bringup_log.hpp"

// Пробный модуль: target_add_binary_data в main/CMakeLists.txt.
extern const uint8_t kHelloElfStart[] asm("_binary_hello_app_elf_start");
extern const uint8_t kHelloElfEnd[] asm("_binary_hello_app_elf_end");

namespace zifi {

void runElfProbe() {
  const size_t size = static_cast<size_t>(kHelloElfEnd - kHelloElfStart);
  const size_t freeBefore = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const int64_t started = esp_timer_get_time();
  esp_elf_t elf;
  int result = esp_elf_init(&elf);
  if (result == 0) {
    result = esp_elf_relocate(&elf, kHelloElfStart);
  }
  int returned = -1;
  // Модуль пишет сюда подробности: сколько проверок прошло, сдвиг localtime_r от UTC
  // (0.1.122: функции zm_symbols.cpp, слабые символы, пояс TZ из zifi.ini).
  char detail[96] = "-";
  if (result == 0 && elf.entry != nullptr) {
    // esp_elf_request код возврата модуля теряет: точка входа вызывается сама.
    char name[] = "hello";
    char* argv[] = {name, detail, nullptr};
    returned = elf.entry(2, argv);
  }
  detail[sizeof(detail) - 1] = 0;
  const unsigned elapsed = static_cast<unsigned>(esp_timer_get_time() - started);
  esp_elf_deinit(&elf);
  const char* zone = getenv("TZ");
  bringupLog("zifi-elf", "probe: %u bytes, relocate %d, returned %d (expected 41), %u us, PSRAM %u -> %u",
             static_cast<unsigned>(size), result, returned, elapsed,
             static_cast<unsigned>(freeBefore),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  bringupLog("zifi-elf", "probe: %s; firmware TZ %s", detail, zone != nullptr ? zone : "(none)");
}

}  // namespace zifi

#else

namespace zifi {

void runElfProbe() {}

}  // namespace zifi

#endif
