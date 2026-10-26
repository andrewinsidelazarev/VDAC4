#include "zifi/sd_bridge_card.hpp"

#include <string.h>

namespace zifi {
namespace {

constexpr uint32_t kSlowKhz = 400;      // инициализация
constexpr uint32_t kFastKhz = 25000;    // режим SPI карты — до 25 МГц
constexpr uint32_t kInitMs = 1000;      // ACMD41: карта готова не позже чем через 1 с
constexpr uint32_t kYieldAfterMs = 2;     // операция дольше — ждать, отдавая процессор (SdBridgeIo::yield)
constexpr uint32_t kYieldEveryMs = 50;    // и не реже раза за столько: файл — тысячи блоков подряд
constexpr uint32_t kOpMs = 2000;        // одна операция моста (BUSY записи — до 250 мс у SDHC)
constexpr uint32_t kTokenBytes = 250000;   // ожидание маркера данных (~100 мс на 18,6 МГц)
constexpr uint32_t kBusyBytes = 1000000;   // ожидание конца записи (~430 мс на 18,6 МГц)
constexpr uint32_t kChunk = 16;            // слов за обмен LINK

uint32_t ceilDiv(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

}  // namespace

uint16_t sdCrc16(const uint8_t* data, size_t size) {
  uint16_t crc = 0;
  for (size_t i = 0; i < size; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int b = 0; b < 8; ++b) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

uint8_t sdCrc7(const uint8_t* data, size_t size) {
  uint8_t crc = 0;
  for (size_t i = 0; i < size; ++i) {
    uint8_t byte = data[i];
    for (int b = 0; b < 8; ++b) {
      crc <<= 1;
      if (((byte & 0x80) != 0) != ((crc & 0x80) != 0)) {
        crc ^= 0x09;
      }
      byte <<= 1;
    }
  }
  return static_cast<uint8_t>(((crc & 0x7F) << 1) | 1);
}

bool SdBridgeCard::present() {
  uint32_t stat = 0;
  return io_.read(io_.ctx, kRegCmd, &stat, 1) && (stat >> 16) == kStatMagic;
}

bool SdBridgeCard::setCtrl(uint32_t div, bool cs) {
  div_ = div & 0xFF;
  const uint32_t v = (cs ? 0x100U : 0U) | div_;
  return io_.write(io_.ctx, kRegCtrl, &v, 1);
}

bool SdBridgeCard::setPtr(uint32_t word, uint32_t blockBytes) {
  const uint32_t v = ((blockBytes & 0x7FF) << 16) | (word & 0x1FF);
  return io_.write(io_.ctx, kRegPtr, &v, 1);
}

bool SdBridgeCard::op(uint32_t kind, uint32_t count, uint32_t* stat) {
  const uint32_t v = ((kind & 7) << 20) | (count & 0xFFFFF);
  if (!io_.write(io_.ctx, kRegCmd, &v, 1)) {
    return false;
  }
  // Команда доходит до моста раньше, чем следующее чтение LINK выдаёт слово (24 такта SCK)
  const uint32_t start = io_.nowMs(io_.ctx);
  if (io_.yield != nullptr && start - lastYieldMs_ >= kYieldEveryMs) {
    io_.yield(io_.ctx);
    lastYieldMs_ = start;
  }
  for (;;) {
    uint32_t s = 0;
    if (!io_.read(io_.ctx, kRegCmd, &s, 1) || (s >> 16) != kStatMagic) {
      return false;
    }
    if ((s & 1) == 0) {
      *stat = s;
      return true;
    }
    const uint32_t spent = io_.nowMs(io_.ctx) - start;
    if (spent > kOpMs) {
      return false;
    }
    if (spent >= kYieldAfterMs && io_.yield != nullptr) {
      io_.yield(io_.ctx);              // карта занята (конец записи): не держать ядро ESP
      lastYieldMs_ = io_.nowMs(io_.ctx);
    }
  }
}

bool SdBridgeCard::txPut(const uint8_t* bytes, uint32_t count) {
  uint32_t words[kChunk];
  const uint32_t total = ceilDiv(count, 4);
  for (uint32_t w = 0; w < total; w += kChunk) {
    const uint32_t n = total - w < kChunk ? total - w : kChunk;
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t v = 0;
      for (uint32_t k = 0; k < 4; ++k) {
        const uint32_t at = 4 * (w + i) + k;
        v = (v << 8) | (at < count ? bytes[at] : 0xFF);
      }
      words[i] = v;
    }
    if (!io_.write(io_.ctx, kWindow + 4 * w, words, n)) {
      return false;
    }
  }
  return true;
}

bool SdBridgeCard::rxGet(uint8_t* bytes, uint32_t count) {
  uint32_t words[kChunk];
  const uint32_t total = ceilDiv(count, 4);
  for (uint32_t w = 0; w < total; w += kChunk) {
    const uint32_t n = total - w < kChunk ? total - w : kChunk;
    if (!io_.read(io_.ctx, kWindow + 4 * w, words, n)) {
      return false;
    }
    for (uint32_t i = 0; i < n; ++i) {
      for (uint32_t k = 0; k < 4; ++k) {
        const uint32_t at = 4 * (w + i) + k;
        if (at < count) {
          bytes[at] = static_cast<uint8_t>(words[i] >> (24 - 8 * k));
        }
      }
    }
  }
  return true;
}

uint8_t SdBridgeCard::command(uint8_t index, uint32_t arg, uint8_t crc) {
  // 0xFF перед командой — 8 тактов после прошлого ответа; затем 6 байтов команды
  uint8_t frame[7] = {0xFF, static_cast<uint8_t>(0x40 | index), static_cast<uint8_t>(arg >> 24),
                      static_cast<uint8_t>(arg >> 16), static_cast<uint8_t>(arg >> 8),
                      static_cast<uint8_t>(arg), crc};
  if (crc == 0) {
    frame[6] = sdCrc7(frame + 1, 5);
  }
  uint32_t stat = 0;
  if (!setPtr(0, kBlock + 2) || !txPut(frame, 7) || !op(kXfer, 7, &stat) || !op(kWait, 16, &stat) ||
      (stat & 2) != 0) {
    return 0xFF;
  }
  return static_cast<uint8_t>(stat >> 8);
}

uint8_t SdBridgeCard::appCommand(uint8_t index, uint32_t arg) {
  const uint8_t r1 = command(55, 0, 0);
  if (r1 > 1) {
    return r1;
  }
  return command(index, arg, 0);
}

const char* SdBridgeCard::init() {
  ready_ = false;
  sdhc_ = false;
  sectors_ = 0;
  if (!present()) {
    return "no SD bridge in the Tang core (TS-Config 1.30+ only)";
  }
  uint32_t ctrl = 0;
  if (!io_.read(io_.ctx, kRegCtrl, &ctrl, 1)) {
    return "LINK read failed";
  }
  clk10k_ = ctrl >> 16;
  const uint32_t clkKhz = clk10k_ * 10;
  if (clkKhz == 0) {
    return "SD bridge reports no clock";
  }
  // полупериод SCK — div + 1 тактов моста
  const uint32_t divSlow = ceilDiv(clkKhz, 2 * kSlowKhz) - 1;
  const uint32_t fast = ceilDiv(clkKhz, 2 * kFastKhz);
  divFast_ = fast > 0 ? fast - 1 : 0;
  if (divSlow > 255) {
    return "SD bridge clock too fast for 400 kHz";
  }

  uint32_t stat = 0;
  if (!setCtrl(divSlow, false) || !op(kFill, 10, &stat)) {    // 80 тактов при неактивном CS
    return "LINK write failed";
  }
  if (!setCtrl(divSlow, true)) {
    return "LINK write failed";
  }
  uint8_t r1 = 0xFF;
  for (int i = 0; i < 10 && r1 != 0x01; ++i) {
    r1 = command(0, 0, 0x95);
  }
  if (r1 != 0x01) {
    release();
    return r1 == 0xFF ? "no SD card in the Tang slot" : "SD card does not enter SPI mode (CMD0)";
  }
  bool v2 = false;
  r1 = command(8, 0x1AA, 0x87);
  if (r1 == 0x01) {
    uint8_t r7[4];
    if (!op(kFill, 4, &stat) || !rxGet(r7, 4)) {
      return "LINK failed";
    }
    if ((r7[2] & 0x0F) != 0x01 || r7[3] != 0xAA) {
      release();
      return "SD card rejects 3.3 V (CMD8)";
    }
    v2 = true;
  } else if ((r1 & 0x04) == 0) {
    release();
    return "SD card: bad answer to CMD8";
  }
  const uint32_t start = io_.nowMs(io_.ctx);
  do {
    r1 = appCommand(41, v2 ? 0x40000000 : 0);
  } while (r1 == 0x01 && io_.nowMs(io_.ctx) - start < kInitMs);
  if (r1 != 0x00) {
    release();
    return "SD card does not leave idle state (ACMD41)";
  }
  if (v2) {
    if (command(58, 0, 0) != 0x00) {
      release();
      return "SD card: no OCR (CMD58)";
    }
    uint8_t ocr[4];
    if (!op(kFill, 4, &stat) || !rxGet(ocr, 4)) {
      return "LINK failed";
    }
    sdhc_ = (ocr[0] & 0x40) != 0;
  }
  if (!sdhc_ && command(16, kBlock, 0) != 0x00) {
    release();
    return "SD card: block length 512 not accepted (CMD16)";
  }
  if (!setCtrl(divFast_, true)) {
    return "LINK write failed";
  }
  sckKhz_ = clkKhz / (2 * (divFast_ + 1));
  uint8_t csd[16];
  if (!readCsd(csd)) {
    release();
    return "SD card: no CSD (CMD9)";
  }
  if ((csd[0] >> 6) == 1) {
    // CSD 2.0: C_SIZE [69:48], блоков — (C_SIZE + 1) · 1024
    const uint32_t cSize = (static_cast<uint32_t>(csd[7] & 0x3F) << 16) | (static_cast<uint32_t>(csd[8]) << 8) | csd[9];
    sectors_ = (cSize + 1) * 1024;
  } else {
    // CSD 1.0: (C_SIZE + 1) · 2^(C_SIZE_MULT + 2) · 2^READ_BL_LEN / 512
    const uint32_t readBlLen = csd[5] & 0x0F;
    const uint32_t cSize = (static_cast<uint32_t>(csd[6] & 0x03) << 10) | (static_cast<uint32_t>(csd[7]) << 2) | (csd[8] >> 6);
    const uint32_t mult = ((csd[9] & 0x03) << 1) | (csd[10] >> 7);
    sectors_ = (cSize + 1) << (mult + 2 + readBlLen - 9);
  }
  ready_ = true;
  return nullptr;
}

bool SdBridgeCard::readCsd(uint8_t csd[16]) {
  if (command(9, 0, 0) != 0x00) {
    return false;
  }
  uint32_t stat = 0;
  uint8_t raw[18];
  if (!setPtr(0, 18) || !op(kReadOp, kTokenBytes, &stat) || (stat & 2) != 0 || ((stat >> 8) & 0xFF) != 0xFE ||
      !rxGet(raw, 18)) {
    return false;
  }
  memcpy(csd, raw, 16);
  return true;
}

bool SdBridgeCard::readBlock(uint32_t lba, uint8_t* data) {
  static uint8_t raw[kBlock + 2];
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (attempt > 0) {
      ++crcRetries_;
    }
    if (command(17, sdhc_ ? lba : lba * kBlock, 0) != 0x00) {
      continue;
    }
    uint32_t stat = 0;
    if (!setPtr(0, kBlock + 2) || !op(kReadOp, kTokenBytes, &stat) || (stat & 2) != 0 ||
        ((stat >> 8) & 0xFF) != 0xFE || !rxGet(raw, kBlock + 2)) {
      continue;
    }
    const uint16_t crc = static_cast<uint16_t>((raw[kBlock] << 8) | raw[kBlock + 1]);
    if (sdCrc16(raw, kBlock) != crc) {
      continue;
    }
    memcpy(data, raw, kBlock);
    return true;
  }
  ++errors_;
  return false;
}

bool SdBridgeCard::writeBlock(uint32_t lba, const uint8_t* data) {
  static uint8_t frame[kBlock + 4];
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (command(24, sdhc_ ? lba : lba * kBlock, 0) != 0x00) {
      continue;
    }
    frame[0] = 0xFF;
    frame[1] = 0xFE;
    memcpy(frame + 2, data, kBlock);
    const uint16_t crc = sdCrc16(data, kBlock);
    frame[kBlock + 2] = static_cast<uint8_t>(crc >> 8);
    frame[kBlock + 3] = static_cast<uint8_t>(crc);
    uint32_t stat = 0;
    if (!setPtr(0, kBlock + 2) || !txPut(frame, kBlock + 4) || !op(kXfer, kBlock + 4, &stat) ||
        !op(kWait, 8, &stat) || (stat & 2) != 0) {
      continue;
    }
    const uint8_t response = static_cast<uint8_t>(stat >> 8) & 0x1F;
    // конец записи ждать и при отказе: карта занята, пока не закончит
    uint32_t busy = 0;
    if (!op(kBusy, kBusyBytes, &busy) || ((busy & 2) != 0 && (!op(kBusy, kBusyBytes, &busy) || (busy & 2) != 0))) {
      continue;
    }
    if (response == 0x05) {
      return true;
    }
  }
  ++errors_;
  return false;
}

bool SdBridgeCard::read(uint32_t lba, uint8_t* data, uint32_t count) {
  if (!ready_) {
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (!readBlock(lba + i, data + i * kBlock)) {
      return false;
    }
  }
  return true;
}

bool SdBridgeCard::write(uint32_t lba, const uint8_t* data, uint32_t count) {
  if (!ready_) {
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (!writeBlock(lba + i, data + i * kBlock)) {
      return false;
    }
  }
  return true;
}

void SdBridgeCard::release() {
  uint32_t stat = 0;
  setCtrl(div_, false);
  op(kFill, 1, &stat);
}

}  // namespace zifi
