#include "zifi/ft_rom.hpp"

#if ZIFI_HAS_SPI_LINK

#include <string.h>

#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "zifi/bringup_log.hpp"
#include "zifi/ft_host.hpp"
#include "zifi/tang_flash.hpp"

namespace zifi {
namespace ft_rom {

namespace {

constexpr char kTag[] = "zifi-ftrom";
constexpr size_t kChunk = 1024;                 // байт за раз: копия → SDRAM
constexpr size_t kFlashChunk = 4096;            // байт за раз: flash Tang → копия
constexpr int kChunkTries = 3;
constexpr size_t kHead = 12 + 12 * kMaxSpans;   // заголовок с запасом под все куски
constexpr size_t kMaxImage = 0x100000;          // слот flash Tang
// 0.1.85 держал копию и в LittleFS — там она отнимала 286 КБ у кэша модулей-декодеров (форматы не
// играли): файл удаляется при первом обращении, копия — только в PSRAM
constexpr char kCacheFile[] = "/ftrom.bin";
bool g_cleaned = false;

void dropOldFile() {
  if (!g_cleaned) {
    g_cleaned = true;
    if (LittleFS.exists(kCacheFile) && LittleFS.remove(kCacheFile)) {
      bringupLog("zifi-ftrom", "FT812 ROM fonts: old ESP copy removed from LittleFS (room for the module cache)");
    }
  }
}

// Копия образа (PSRAM): её пишет в SDRAM ядра FT812 load(). Берётся из flash Tang, пока Tang в
// TS-Config (refresh), или от службы обновления.
uint8_t* g_image = nullptr;
size_t g_size = 0;

SemaphoreHandle_t cacheLock() {
  static SemaphoreHandle_t lock = nullptr;
  static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
  if (lock == nullptr) {
    SemaphoreHandle_t made = xSemaphoreCreateMutex();
    portENTER_CRITICAL(&guard);
    if (lock == nullptr) {
      lock = made;
      made = nullptr;
    }
    portEXIT_CRITICAL(&guard);
    if (made != nullptr) {
      vSemaphoreDelete(made);
    }
  }
  return lock;
}

struct CacheGuard {
  CacheGuard() { xSemaphoreTake(cacheLock(), portMAX_DELAY); }
  ~CacheGuard() { xSemaphoreGive(cacheLock()); }
};

uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

struct Span {
  uint32_t address, length, offset;
};

// Заголовок: число кусков и сами куски; false — не образ шрифтов или куски негодные.
bool parse(const uint8_t* head, size_t headSize, size_t imageSize, Span* spans, size_t* count) {
  if (headSize < 12 || memcmp(head, kMagic, sizeof(kMagic)) != 0) {
    return false;
  }
  const uint32_t n = le32(head + 8);
  if (n == 0 || n > kMaxSpans || 12 + 12 * n > headSize) {
    return false;
  }
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* e = head + 12 + 12 * i;
    Span s{le32(e), le32(e + 4), le32(e + 8)};
    // только адреса ПЗУ FT812 (0x200000…0x2FFFFF, видны в режиме RAM_G 2 МБ), всё кратно 4
    if (s.length == 0 || ((s.address | s.length | s.offset) & 3U) != 0 || s.address < 0x200000 ||
        s.address + s.length > 0x300000 || s.offset < 12 + 12 * n || s.offset + s.length > imageSize) {
      return false;
    }
    spans[i] = s;
  }
  *count = n;
  return true;
}

// Длина образа по заголовку (конец самого дальнего куска); 0 — заголовок негодный
size_t extent(const uint8_t* head, size_t headSize, size_t imageSize) {
  Span spans[kMaxSpans];
  size_t n = 0;
  if (!parse(head, headSize, imageSize, spans, &n)) {
    return 0;
  }
  size_t end = 0;
  for (size_t i = 0; i < n; ++i) {
    end = spans[i].offset + spans[i].length > end ? spans[i].offset + spans[i].length : end;
  }
  return end;
}

// Новая копия (буфер в PSRAM переходит сюда)
void adoptBuffer(uint8_t* data, size_t size) {
  CacheGuard guard;
  heap_caps_free(g_image);
  g_image = data;
  g_size = size;
}

// Копия из flash Tang (мост ядра TS-Config): заголовок тот же — читать не нужно
bool readFlash() {
  uint8_t head[kHead];
  if (!tang_flash::read(kFlashBase, head, sizeof(head))) {
    return false;
  }
  const size_t size = extent(head, sizeof(head), kMaxImage);
  if (size == 0) {
    bringupLog(kTag, "FT812 ROM fonts: no font image in Tang flash slot 2 (CMD_TEXT waits, then draws blanks)");
    return false;
  }
  {
    CacheGuard guard;
    if (g_image != nullptr && g_size == size && memcmp(g_image, head, kHead) == 0) {
      return true;
    }
  }
  const int64_t t0 = esp_timer_get_time();
  uint8_t* data = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (data == nullptr) {
    bringupLog(kTag, "FT812 ROM fonts: no PSRAM for the %u-byte copy", static_cast<unsigned>(size));
    return false;
  }
  bool ok = true;
  for (size_t done = 0; ok && done < size; done += kFlashChunk) {
    const size_t part = size - done < kFlashChunk ? size - done : kFlashChunk;
    ok = tang_flash::read(kFlashBase + done, data + done, part);
  }
  if (!ok || extent(data, kHead, size) != size) {
    heap_caps_free(data);
    bringupLog(kTag, "FT812 ROM fonts: reading Tang flash slot 2 FAILED");
    return false;
  }
  adoptBuffer(data, size);
  bringupLog(kTag, "FT812 ROM fonts: %u bytes copied from Tang flash slot 2, %u ms",
             static_cast<unsigned>(size), static_cast<unsigned>((esp_timer_get_time() - t0) / 1000));
  return true;
}

}  // namespace

const char* checkImage(const uint8_t* image, size_t size) {
  Span spans[kMaxSpans];
  size_t n = 0;
  if (!parse(image, size < kHead ? size : kHead, size, spans, &n)) {
    return "not a VD4FONT1 font image (header or spans are bad)";
  }
  return nullptr;
}

bool refresh() {
  dropOldFile();
  return tang_flash::available() && readFlash();
}

void adopt(const uint8_t* image, size_t size) {
  if (checkImage(image, size) != nullptr) {
    return;
  }
  uint8_t* data = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (data == nullptr) {
    return;
  }
  memcpy(data, image, size);
  adoptBuffer(data, size);
}

bool load() {
  bool have;
  {
    CacheGuard guard;
    have = g_image != nullptr;
  }
  dropOldFile();
  // копии нет: у ядра FT812 с мостом (до 1.31) — прямо из flash Tang
  if (!have && !refresh()) {
    bringupLog(kTag, "FT812 ROM fonts: no copy on the ESP (it is taken in TS-Config; CMD_TEXT waits, then draws blanks)");
    return false;
  }
  CacheGuard guard;
  Span spans[kMaxSpans];
  size_t n = 0;
  if (g_image == nullptr || !parse(g_image, kHead, g_size, spans, &n)) {
    return false;
  }
  const int64_t t0 = esp_timer_get_time();
  if (!ft_host::setRomWrite(true)) {
    return false;
  }
  static uint32_t words[kChunk / 4];
  bool ok = true;
  bool haveRoot = false;
  uint32_t root = 0;
  size_t total = 0;
  for (size_t i = 0; ok && i < n; ++i) {
    for (uint32_t done = 0; ok && done < spans[i].length; done += kChunk) {
      const uint32_t part = spans[i].length - done < kChunk ? spans[i].length - done : kChunk;
      const uint32_t at = spans[i].address + done;
      memcpy(words, g_image + spans[i].offset + done, part);
      uint32_t count = part / 4;
      // ROM_FONTROOT — последним: сопроцессор начнёт CMD_TEXT, только когда шрифты на месте
      if (at + part == kFontRoot + 4) {
        root = words[count - 1];
        haveRoot = true;
        --count;
      }
      // сорвался обмен LINK — кусок заново
      for (int attempt = 0; attempt < kChunkTries; ++attempt) {
        ok = count == 0 || ft_host::writeBlock(at, words, count);
        if (ok) {
          break;
        }
        ESP_LOGW(kTag, "chunk at %06X failed (try %d of %d)", static_cast<unsigned>(at), attempt + 1, kChunkTries);
      }
      total += part;
    }
  }
  if (ok && haveRoot) {
    ok = ft_host::write32(kFontRoot, root);
  }
  ft_host::setRomWrite(false);
  bringupLog(kTag, "FT812 ROM fonts: %u bytes in %u spans from the ESP copy, %u ms, %s",
             static_cast<unsigned>(total), static_cast<unsigned>(n),
             static_cast<unsigned>((esp_timer_get_time() - t0) / 1000), ok ? "ready" : "FAILED");
  return ok;
}

}  // namespace ft_rom
}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
