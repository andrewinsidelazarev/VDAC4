#pragma once
// Служба виртуального диска VDAC4 VFS для Z80: драйвер устройства 6 Wild Commander (плагин VDAC4 VFS)
// читает и пишет сектора SD2. Два режима (MODE):
//   0 — том vdisk: папки источников (TANG — карта Tang через FatFs ESP), только чтение;
//   1 — сама карта Tang посекторно (tang_sd::holdForWc): FAT32 ведёт CORE32 WC, запись есть.
// Команды — на ядре 1, ответ сразу (сектор собирается или пишется за время команды):
//   INFO  0x15 []                              -> 0x95 [итог][флаги][поколение LE32][секторов LE32][источников]
//   READ  0x16 [lba LE32][смещение LE16][длина][режим] -> 0x96 [итог][данные: длина байт]
//         (байт режима — с 0.1.100, необязательный: не тот режим — итог 5)
//   RESET 0x17 []                              -> 0x97 [итог][поколение LE32]
//   WRITE 0x18 [lba LE32][данные 512]          -> 0x98 [итог]
//   MODE  0x19 [режим]                         -> 0x99 [итог][флаги][секторов LE32]
//   READN 0x1E [lba LE32][секторов 1…255][режим] (0.1.127) -> на каждый сектор подряд два кадра
//         0x9E по 256 байт (половины сектора), без запроса на каждый; отказ — кадр 0x9E из одного
//         байта итога, и серия кончается. Кадры ждут в кольце канала, пока Z80 их выбирает: сектор —
//         один круг обмена вместо трёх (READ по 248 байт), SD2 = карта Tang читается в разы быстрее
//         (0.1.126 — 32 КБ/с: FLAC с SD2 не успевал, 11.11.2026).
// Итог: 0 — да, 1 — неверный запрос, 2 — источник не ответил (карта занята загрузкой ядра, вынута,
// ядро сменилось), 3 — только чтение (режим 0), 4 — карту не отдать (ядро без моста, нет карты),
// 5 — READ ждёт другой режим (ESP перезагрузилась, пока SD2 был картой Tang).
// READ отдаёт часть сектора 512 байт: длина до kReadChunk, смещение + длина не больше 512; сектор
// собирается при первой части и хранится до запроса другого LBA (кадр ESP -> Z80 — до 256 байт).
// Кадр Z80 -> ESP — до 1024 байт, поэтому WRITE несёт сектор целиком.
#include <stdint.h>

#include <vector>

#include "zifi/protocol.hpp"
#include "zifi/vdisk.hpp"
#include "zifi/z80_transport.hpp"

namespace zifi {
namespace vdisk_service {

constexpr uint16_t kReadChunk = 248;
enum Status : uint8_t { kOk = 0, kBadRequest = 1, kSourceFailed = 2, kReadOnly = 3, kNoCard = 4, kWrongMode = 5 };
// Флаги INFO и MODE: в режиме 0 — vdisk::Volume::flags() (бит 0 — том собран, бит 1 — только
// чтение, бит 2 — что-то не поместилось); в режиме 1 — kFlagReady (карта у WC) и kFlagRaw.
constexpr uint8_t kFlagReady = 0x01;
// Прошивка умеет READN с секторами через DMA (0.1.128): разряд kReadNDma в байте режима READN —
// ответа кадрами нет, секторы встают в очередь секторов канала (ByteLink::pushSector), резидент
// забирает их своим DMA прямо в буфер WC (SpiLinkCore::kFlagSector); отказ источника — конец
// серии там же. Сектор кадрами по 54 байта за обмен стоил Z80 ~89 000 тактов (не быстрее
// ~79 КБ/с даже при мгновенной ESP): FLAC с SD2 = карта Tang не успевал и с READN (11.11.2026).
constexpr uint8_t kFlagReadDma = 0x10;
constexpr uint8_t kReadNDma = 0x80;
constexpr uint8_t kFlagReadN = 0x20;   // прошивка умеет READN (0.1.127)
constexpr uint8_t kFlagRaw = 0x40;

void handle(const PacketView& packet, Z80Transport& transport);

// Для видео с SD2 (sd2_files; звать с ядра 1 — состояние службы живёт там): SD2 — сама карта Tang
// (режим 1)?; файл тома VFS по первому кластеру — источник, путь, длина (vdisk::Volume::fileAt).
bool rawMode();
bool volumeFile(uint32_t cluster, vdisk::Source** source, std::vector<char>& where, uint64_t* size);

}  // namespace vdisk_service
}  // namespace zifi
