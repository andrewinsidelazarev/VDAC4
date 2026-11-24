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

// Ядра с карты Evo (0.1.164; пользователь 23.11.2026: «проброс ядра ПЛИС с SD card Evo с кэшем во
// флешке USB-C, как мы это делали для ELF», «должно работать и при отсутствии флешки»). Программа
// Z80 (SPG, плагин) шлёт ИМЯ.BIN теми же командами, что и ELF: MOD_QUERY с флагом 2 (имя, длина,
// время файла FAT), MOD_PUT, MOD_COMMIT. ESP проверяет битстрим (то же, что BL616) и кладёт его на
// флешку в /usb/cores с временем файла карты; флешки нет — сразу в кэш ядер BL616 (его flash и есть
// кэш). В следующий раз годная копия на флешке или то же ядро в кэше BL616 — и передачи нет.
// Ядро, заявленное так в этом запуске ESP, load() и ABI core_load берут отсюда, а не с SD Tang.
// Отметок — до 4 ядер (таблица в PSRAM); отметки с известным SHA хранятся в NVS — после перезапуска
// ESP они помнят, какой файл карты Evo уже лежит в кэше BL616, но ядро не заявляют.
//
// Годен ли образ ядра (битстрим GW2AR-18 с меткой VDAC4, не длиннее слота flash Tang): nullptr — да.
const char* checkEvoImage(const uint8_t* data, uint32_t size);
// Отметка «ядро name с карты Evo». data — образ в PSRAM: переходит к core_store, он его и освободит;
// nullptr — годная копия лежит на флешке. size, stamp — длина и время файла на карте Evo.
void evoOffer(const char* name, uint8_t* data, uint32_t size, uint32_t stamp);
// Ядро name с карты Evo заявлено в этом запуске ESP: CORE_APP не читает его ELF с SD Tang.
bool evoMarked(const char* name);
// MOD_QUERY без годной копии на флешке: этот файл карты (длина, время) уже в кэше BL616 — ядро
// заявляется, и передавать его не нужно. Мост Tang занят дольше kQuickMs — false (пусть шлют).
bool evoClaimCached(const char* name, uint32_t size, uint32_t stamp);
// MOD_COMMIT без флешки: ядро сразу в кэш BL616; data переходит к core_store. nullptr — легло; иначе
// причина, а образ остаётся в PSRAM (core_load положит его туда сам).
const char* evoStoreToCache(const char* name, uint8_t* data, uint32_t size, uint32_t stamp);
// Плагин берёт список ядер SD Tang (CORE_LIST): заявки ядер с карты Evo снимаются — ядро того же
// имени с карты Tang снова грузится с карты Tang. Образы в PSRAM освобождаются.
void evoReleaseClaims();
// Образ ядра name той же длины и времени уже в PSRAM: передавать с карты не нужно.
bool evoHeld(const char* name, uint32_t size, uint32_t stamp);
// Отдать PSRAM (нужна доставке): образы ядер освобождаются; ядро с копией на флешке или уже в кэше
// BL616 грузится и так, иначе его пришлют снова. Сколько байт освобождено.
uint32_t evoReleaseMemory();
// Записать в NVS отметки ядер с карты Evo, если с прошлого раза появился новый SHA (0.1.170). Звать
// только из задачи со стеком во внутренней памяти — её зовёт Application::pollLink (задача обмена с
// Z80). Загрузчик zm-coreload со стеком в PSRAM флешь ESP трогать не может: запись туда и отложена.
void evoFlush();

}  // namespace core_store
}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
