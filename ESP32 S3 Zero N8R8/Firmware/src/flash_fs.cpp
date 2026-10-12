#include "zifi/flash_fs.hpp"

#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace zifi {
namespace {

constexpr char kPartitionLabel[] = "littlefs";
constexpr char kMountPath[] = "/littlefs";
// Открыты одновременно бывают файл журнала, настроек и модуля из кэша.
constexpr uint8_t kMaxOpenFiles = 5;

SemaphoreHandle_t lock() {
  static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
  return mutex;
}

int users = 0;

}  // namespace

bool flashFsMount(bool formatOnFail) {
  xSemaphoreTake(lock(), portMAX_DELAY);
  bool mounted = users > 0 ||
                 LittleFS.begin(formatOnFail, kMountPath, kMaxOpenFiles, kPartitionLabel);
  if (mounted) {
    ++users;
  }
  xSemaphoreGive(lock());
  return mounted;
}

void flashFsUnmount() {
  xSemaphoreTake(lock(), portMAX_DELAY);
  if (users > 0 && --users == 0) {
    LittleFS.end();
  }
  xSemaphoreGive(lock());
}

}  // namespace zifi
