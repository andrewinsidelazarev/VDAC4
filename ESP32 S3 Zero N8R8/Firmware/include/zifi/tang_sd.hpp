#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zifi/board.hpp"

#if ZIFI_HAS_SPI_LINK

namespace zifi {
namespace tang_sd {

// microSD самой Tang Nano 20K: в ядре TS-Config 1.30+ (слот 0) и NES 0.7+ — мост fb_sdspi (sd_bridge_card),
// FatFs (FAT32 и exFAT) в /tangsd. На карте — NextZXOS ядра ZX Next и ядра для кэша BL616
// (/CORES/, docs/next-core.md). Монтируется по запросу; ядро Tang сменилось (multiboot, ядро из
// кэша BL616) — мост пропал: обращения отвечают «не готов», том нужно смонтировать заново.
// Все обращения к карте держат LINK на время блока (tang::LinkGuard).
constexpr const char* kMountPoint = "/tangsd";

// Карта Tang, ящик BL616 и смена ядра Tang — по одному (разбор Codex 21.10.2026, № 8 и № 9): том и
// его файлы, команды кэша ядер BL616, загрузка ядра в ПЛИС и обновление прошивки BL616 держат
// Session на всю операцию. Иначе другая задача могла бы размонтировать том посреди чтения файла,
// сменить ядро под открытым томом или переключить страницу ящика посреди чужой команды. Замок
// рекурсивный (функции core_store зовут друг друга). Команды Z80 ждут его не дольше kQuickMs:
// задача обмена отвечает сразу, а загрузка ядра держит замок десятки секунд.
constexpr uint32_t kQuickMs = 300;
constexpr uint32_t kForever = 0xFFFFFFFFU;
class Session {
 public:
  explicit Session(uint32_t waitMs = kForever);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  bool ok() const { return ok_; }

 private:
  bool ok_ = false;
};
constexpr const char* kBusy = "the Tang SD is busy (a core is being loaded), try again";

// nullptr — смонтирована; иначе — что не так.
const char* mount();
void unmount();
bool mounted();

// Сырые блоки по 512 байт (консоль: проверка скорости); карта должна быть смонтирована.
bool readBlocks(uint32_t lba, uint8_t* data, uint32_t count);

// Для консоли: состояние карты.
void describe(char* out, size_t size);

// Карта отдана WC посекторно (VDAC4 VFS, режим «SD2 = карта Tang»): файловую систему ведёт CORE32
// WC, поэтому FatFs ESP карту не монтирует (mount отвечает kHeldByWc), а ядро Tang не меняется
// (core_store, переход в ядро FT, tang boot): мост к карте есть только в TS-Config, а чужие записи
// под кэшем WC испортили бы FAT. Снимает переключатель (F10) и резидент при старте WC.
constexpr const char* kHeldByWc = "the Tang SD card is held by WC (SD2): switch SD2 back in F10 VDAC4 VFS";
bool heldByWc();
void setHeldByWc(bool held);  // только для tang_sd.cpp (флаг — в tang_sd_hold.cpp)
const char* holdForWc();      // nullptr — карта готова к секторам WC; повторный вызов — проверка
void releaseFromWc();
bool rawRead(uint32_t lba, uint8_t* data);           // один блок 512 байт; false — нет захвата,
bool rawWrite(uint32_t lba, const uint8_t* data);    // ядро сменилось или карта не ответила
uint32_t rawSectors();

}  // namespace tang_sd
}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
