#pragma once

#include <stddef.h>
#include <stdint.h>

namespace zifi {

// microSD самой Tang Nano 20K через мост ядра TS-Config 1.30 (fpga/vdac2/rtl/fb_sdspi.sv):
// карта в режиме SPI, мост делает побайтный обмен, ожидание ответа и приём блока за маркером;
// протокол SD — здесь. Код переносимый: LINK и время — через SdBridgeIo (на ESP — tang::read/
// write; на ПК — модель моста и карты, tests/sd_bridge_host.cpp).
//
// Регистры моста (LINK): 0x100 SD_CTRL, 0x104 SD_PTR, 0x108 SD_CMD / SD_STAT; буферы TX и RX —
// окно 0x2000…0x27FC (запись — TX, чтение — RX), байт 0 — старший байт слова.
//
// Блок — 512 байтов. Чтение — CMD17 на блок, CRC16 сверяется (не сошлось — ещё две попытки),
// запись — CMD24 с CRC16, ответ данных и ожидание конца записи. SDSC адресуется байтами, SDHC и
// SDXC — блоками.
struct SdBridgeIo {
  void* ctx;
  bool (*read)(void* ctx, uint32_t address, uint32_t* words, size_t count);         // до 16 слов
  bool (*write)(void* ctx, uint32_t address, const uint32_t* words, size_t count);  // до 16 слов
  uint32_t (*nowMs)(void* ctx);
  // Отдать процессор, пока операция моста ждёт (конец записи блока — до сотен мс): без этого запись
  // файла держит ядро ESP дольше сторожевого таймера задач (5 с). nullptr — не нужно (ПК).
  void (*yield)(void* ctx);
};

class SdBridgeCard {
 public:
  static constexpr uint32_t kRegCtrl = 0x100;
  static constexpr uint32_t kRegPtr = 0x104;
  static constexpr uint32_t kRegCmd = 0x108;
  static constexpr uint32_t kWindow = 0x2000;
  static constexpr uint32_t kStatMagic = 0x5D01;
  static constexpr uint32_t kBlock = 512;

  explicit SdBridgeCard(const SdBridgeIo& io) : io_(io) {}

  // Мост есть (SD_STAT[31:16] = 0x5D01; в ядре FT812 и до 1.30 его нет).
  bool present();
  // Карта: 80 тактов, CMD0, CMD8, ACMD41, CMD58, CSD — объём; затем быстрый SCK (≤ 25 МГц).
  // nullptr — готова; иначе — что не так.
  const char* init();
  bool ready() const { return ready_; }
  bool highCapacity() const { return sdhc_; }
  uint32_t sectors() const { return sectors_; }
  uint32_t sckKhz() const { return sckKhz_; }

  bool read(uint32_t lba, uint8_t* data, uint32_t count);
  bool write(uint32_t lba, const uint8_t* data, uint32_t count);
  // CS неактивен (между сеансами карта не держит шину)
  void release();

  // Счётчики: повторов чтения по CRC, ошибок
  uint32_t crcRetries() const { return crcRetries_; }
  uint32_t errors() const { return errors_; }

 private:
  static constexpr uint32_t kXfer = 0, kFill = 1, kWait = 2, kReadOp = 3, kBusy = 4;
  uint32_t lastYieldMs_ = 0;            // когда операция моста последний раз отдавала процессор

  bool setCtrl(uint32_t div, bool cs);
  bool setPtr(uint32_t word, uint32_t blockBytes);
  // операция; *stat — SD_STAT после неё
  bool op(uint32_t kind, uint32_t count, uint32_t* stat);
  bool txPut(const uint8_t* bytes, uint32_t count);
  bool rxGet(uint8_t* bytes, uint32_t count);
  // команда: R1 (0xFF — ответа нет)
  uint8_t command(uint8_t index, uint32_t arg, uint8_t crc);
  uint8_t appCommand(uint8_t index, uint32_t arg);
  bool readBlock(uint32_t lba, uint8_t* data);
  bool writeBlock(uint32_t lba, const uint8_t* data);
  bool readCsd(uint8_t csd[16]);

  const SdBridgeIo& io_;
  uint32_t clk10k_ = 0;
  uint32_t divFast_ = 0;
  uint32_t div_ = 0xFF;      // полупериод SCK − 1, что сейчас в SD_CTRL
  bool ready_ = false;
  bool sdhc_ = false;
  uint32_t sectors_ = 0;
  uint32_t sckKhz_ = 0;
  uint32_t crcRetries_ = 0;
  uint32_t errors_ = 0;
};

// CRC16-CCITT (полином 0x1021, начальное 0) блока данных SD и CRC7 команды
uint16_t sdCrc16(const uint8_t* data, size_t size);
uint8_t sdCrc7(const uint8_t* data, size_t size);

}  // namespace zifi
