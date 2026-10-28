#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zifi/board.hpp"
#include "zifi/core_cache_client.hpp"
#include "zifi/tang_sd.hpp"

#if ZIFI_HAS_SPI_LINK

namespace zifi {
namespace core_store {

// Ядра ПЛИС для кэша BL616 (docs/next-core.md) — на microSD самой Tang, папка /CORES/ в корне: двоичные
// битстримы Gowin `ИМЯ.BIN` (с меткой VDAC4 в UserCode). Имя в кэше — имя файла без «.BIN».
// Рядом может лежать `ИМЯ.ELF` — слой ESP этого ядра (приложение ESP: клавиши, звук, сеть ядра;
// для ZX Next — NEXT.ELF). Его запускает плагин WC VDAC4 Cores (команда CORE_APP).
// Нужно ядро TS-Config 1.30+ (мост к SD, tang_sd) и прошивка BL616 14+ (кэш).
constexpr const char* kFolder = "/tangsd/CORES";
constexpr size_t kMaxCores = 16;

struct Entry {
  char name[16];
  uint32_t size;
  core_cache::Found cached;      // в кэше BL616: то же ядро / другое / нет
  bool elf;                      // рядом есть ИМЯ.ELF — слой ESP ядра
};

using Progress = void (*)(void* ctx, uint32_t done, uint32_t total);

// Все функции держат tang_sd::Session на всю операцию; waitMs — сколько ждать его (команды Z80 —
// tang_sd::kQuickMs: тогда занятая карта — tang_sd::kBusy).
//
// Ядра на SD Tang; checkCache — сверить каждое с кэшем BL616 (SHA-256 файла, ~1 с на ядро).
const char* list(Entry* out, size_t max, size_t* count, bool checkCache,
                 uint32_t waitMs = tang_sd::kForever);
// Ядро с SD — в кэш BL616 (если там его нет или оно другое).
const char* sync(const char* name, Progress progress, void* ctx);
// Загрузить ядро в ПЛИС (не в кэше — сначала с SD) и дождаться его на LINK; *version — VERSION
// нового ядра. SD Tang при этом снимается: у ядра из кэша моста к ней нет. Удача — только если
// работает ядро из кэша (личность 3+); не вышло — Tang возвращается в слот 0 (TS-Config).
// Ядро, у которого есть свой слот во flash Tang (kFlashCores: VIDEO — слот 3), грузится оттуда
// (прошивка BL616 35+, команда BOOT); в слоте нет битстрима или BL616 старше — как прежде, с SD
// через кэш.
const char* load(const char* name, uint32_t* version, Progress progress, void* ctx);

// Ядра во flash Tang (слоты multiboot, docs/video-core.md): имя, слот, личность в VERSION.
struct FlashCore {
  const char* name;
  uint32_t slot;
  uint32_t personality;
};
constexpr FlashCore kFlashCores[] = {{"VIDEO", 3, 0x05}};
// То же без SD: ядро уже в кэше (служба 8268, tang_tool.py core-load).
const char* loadCached(const char* name, uint32_t* version);
// Слой ESP ядра `ИМЯ.ELF` с SD Tang — в PSRAM (*data освобождает вызывающий, heap_caps_free);
// *stamp — время файла. nullptr — прочитан; kNoElf — у ядра своего ELF нет; иначе — ошибка.
extern const char* const kNoElf;
const char* readElf(const char* name, uint8_t** data, uint32_t* size, uint32_t* stamp,
                    uint32_t waitMs = tang_sd::kForever);

}  // namespace core_store
}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
