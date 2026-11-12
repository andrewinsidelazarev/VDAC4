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
// Буква диска FatFs смонтированной карты ("0:") для прямых вызовов f_* (TangVfs — шара SMB
// «TANG»); "" — не смонтирована. Вызывающий держит Session.
const char* fatDrive();

// Сырые блоки по 512 байт (консоль: проверка скорости); карта должна быть смонтирована.
bool readBlocks(uint32_t lba, uint8_t* data, uint32_t count);

// Файл как диск: секторы по 512 байт на месте, длина не меняется. Вызывающий держит Session.
// diskOpen: 0 — открыт, 1 — файла нет, 2 — отказ.
struct DiskFile;
int diskOpen(const char* path, bool writable, DiskFile** file, uint32_t* sectors);
bool diskRead(DiskFile* file, uint32_t sector, uint8_t* data, uint32_t count);
bool diskWrite(DiskFile* file, uint32_t sector, const uint8_t* data, uint32_t count);
void diskClose(DiskFile* file);
// Новый файл длиной sectors: 0 — создан, 1 — уже есть, 2 — отказ, 3 — нет места на карте.
int diskCreate(const char* path, uint32_t sectors);

// Для консоли: состояние карты.
void describe(char* out, size_t size);

// Карта отдана WC посекторно (VDAC4 VFS, режим «SD2 = карта Tang»): файловую систему ведёт CORE32
// WC, поэтому FatFs ESP карту не монтирует (mount отвечает kHeldByWc), а ядро Tang не меняется
// (core_store, переход в ядро FT, tang boot): чужие записи под кэшем WC испортили бы FAT. Одно
// исключение (1.47, 11.11.2026) — ядро видео из слота 3: карту оно само не трогает, мост в нём
// есть (только чтение и поток STREAM в медиа-FIFO) — видео с SD2 WC. После смены ядра удержание
// подключается к мосту нового ядра само, при первом обращении (только TS-Config 1.30+ и ядро
// видео 1.47+; прочие ядра карту пишут сами — их не ждём). Снимает переключатель (F10) и резидент
// при старте WC.
constexpr const char* kHeldByWc = "the Tang SD card is held by WC (SD2): switch SD2 back in F10 VDAC4 VFS";
bool heldByWc();
void setHeldByWc(bool held);  // только для tang_sd.cpp (флаг — в tang_sd_hold.cpp)
const char* holdForWc();      // nullptr — карта готова к секторам WC; повторный вызов — проверка
void releaseFromWc();
bool rawRead(uint32_t lba, uint8_t* data);           // один блок 512 байт; false — нет захвата,
bool rawWrite(uint32_t lba, const uint8_t* data);    // ядро без моста к ней или карта не ответила
uint32_t rawSectors();
// Удерживаемая карта: blocks целых блоков CMD18 с lba мимо ESP — в медиа-FIFO ядра видео 1.47
// (SdBridgeCard::stream). Session держится всё время потока (обращения WC ждут его конца, kQuickMs),
// LINK — нет. *crcError — CRC16 какого-то блока не сошёлся. false — не ядро видео 1.47+, нет
// захвата, отказ карты.
bool rawStream(uint32_t lba, uint32_t blocks, bool* crcError);
// Ядро на LINK — ядро видео с мостом и потоком STREAM (1.47+).
bool videoBridge();

}  // namespace tang_sd
}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
