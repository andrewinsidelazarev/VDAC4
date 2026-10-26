// Проверка драйвера microSD Tang (src/sd_bridge_card.cpp) на ПК: модель моста ядра TS-Config 1.30
// (fpga/vdac2/rtl/fb_sdspi.sv — регистры, буферы TX/RX, операции XFER, FILL, WAIT, READ, BUSY) и
// модель карты в режиме SPI (как tb/sd_card_model.sv: CMD0/8/9/16/17/24/55/58, ACMD41; SDHC —
// адрес блоками, SDSC — байтами). Помехи: искажённый байт в блоке (CRC16 не сходится), отказ записи,
// долгий ACMD41, карты нет, ядро без моста.
// Печатает итоги сценариев и «ТЕСТ ПРОЙДЕН».
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <deque>
#include <map>
#include <vector>

#include "zifi/sd_bridge_card.hpp"

using zifi::SdBridgeCard;
using zifi::SdBridgeIo;

namespace {

int failures = 0;
#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      ++failures;                                         \
      printf("ОШИБКА %s:%d: ", __FILE__, __LINE__);       \
      printf(__VA_ARGS__);                                \
      printf("\n");                                       \
    }                                                     \
  } while (0)

// ---- карта ----
struct Card {
  bool present = true;
  bool sdhc = true;            // false — SDSC v1 (CMD8 недопустима, адрес байтами)
  uint32_t sectors = 131072;   // 64 МБ
  int acmd41Needed = 2;
  int nac = 40;
  int busy = 30;
  int corruptReads = 0;        // столько чтений подряд — с искажённым байтом
  int rejectWrites = 0;        // столько записей — ответ «ошибка CRC»
  std::map<uint32_t, std::vector<uint8_t>> blocks;
  std::deque<uint8_t> q;
  uint8_t cmd[6] = {};
  int cn = 0;
  bool idle = true, app = false;
  int acmd41 = 0;
  int wstate = 0, wn = 0;
  uint32_t wblk = 0;
  uint8_t wbuf[514] = {};
  bool cs = false;
  std::vector<uint32_t> readAddrs, writeAddrs;
  int badCrc = 0;

  std::vector<uint8_t>& block(uint32_t b) {
    auto& v = blocks[b];
    if (v.empty()) {
      v.resize(512);
      for (int i = 0; i < 512; ++i) v[i] = static_cast<uint8_t>(i * 7 + b * 13 + 5);
    }
    return v;
  }
  void push(uint8_t b) { q.push_back(b); }
  uint8_t pop() {
    if (q.empty()) return 0xFF;
    uint8_t b = q.front();
    q.pop_front();
    return b;
  }
  void exec() {
    const uint8_t idx = cmd[0] & 0x3F;
    const uint32_t arg = (uint32_t(cmd[1]) << 24) | (uint32_t(cmd[2]) << 16) | (uint32_t(cmd[3]) << 8) | cmd[4];
    if (zifi::sdCrc7(cmd, 5) != cmd[5]) ++badCrc;
    push(0xFF);
    const bool wasApp = app;
    app = false;
    if (wasApp && idx == 41) {
      ++acmd41;
      if (acmd41 >= acmd41Needed) idle = false;
      push(idle ? 1 : 0);
      return;
    }
    switch (idx) {
      case 0: idle = true; acmd41 = 0; push(0x01); break;
      case 8:
        if (!sdhc) { push(0x05); break; }
        push(idle ? 1 : 0); push(0); push(0); push((arg >> 8) & 0x0F); push(arg & 0xFF);
        break;
      case 55: app = true; push(idle ? 1 : 0); break;
      case 58: push(idle ? 1 : 0); push(sdhc ? 0xC0 : 0x80); push(0xFF); push(0x80); push(0x00); break;
      case 16: push(idle ? 1 : 0); break;
      case 9: {
        uint8_t csd[16] = {};
        if (sdhc) {
          const uint32_t c = sectors / 1024 - 1;
          csd[0] = 0x40; csd[5] = 0x59; csd[7] = (c >> 16) & 0x3F; csd[8] = (c >> 8) & 0xFF; csd[9] = c & 0xFF;
        } else {
          // 2^(mult+2) · 2^READ_BL_LEN · (C_SIZE + 1) / 512: READ_BL_LEN 9, mult 7 → C_SIZE + 1 = sectors / 512
          const uint32_t c = sectors / 512 - 1;
          csd[0] = 0x00; csd[5] = 0x09; csd[6] = (c >> 10) & 3; csd[7] = (c >> 2) & 0xFF; csd[8] = (c & 3) << 6;
          csd[9] = 0x03; csd[10] = 0x80;
        }
        push(0x00);
        for (int k = 0; k < 4; ++k) push(0xFF);
        push(0xFE);
        for (int k = 0; k < 16; ++k) push(csd[k]);
        push(0); push(0);
        break;
      }
      case 17: {
        const uint32_t b = sdhc ? arg : arg / 512;
        readAddrs.push_back(arg);
        push(0x00);
        for (int k = 0; k < nac; ++k) push(0xFF);
        if (b >= sectors || (!sdhc && arg % 512)) { push(0x08); break; }
        push(0xFE);
        std::vector<uint8_t> d = block(b);
        const uint16_t crc = zifi::sdCrc16(d.data(), 512);
        if (corruptReads > 0) { --corruptReads; d[100] ^= 0x40; }
        for (int k = 0; k < 512; ++k) push(d[k]);
        push(crc >> 8); push(crc & 0xFF);
        break;
      }
      case 24:
        writeAddrs.push_back(arg);
        push(0x00);
        wstate = 1;
        wblk = sdhc ? arg : arg / 512;
        break;
      default: push(0x04 | (idle ? 1 : 0)); break;
    }
  }
  void got(uint8_t b) {
    if (wstate == 1) {
      if (b == 0xFE) { wstate = 2; wn = 0; }
      return;
    }
    if (wstate == 2) {
      wbuf[wn++] = b;
      if (wn == 514) {
        wstate = 0;
        const uint16_t crc = uint16_t((wbuf[512] << 8) | wbuf[513]);
        if (crc != zifi::sdCrc16(wbuf, 512) || rejectWrites > 0) {
          if (rejectWrites > 0) --rejectWrites;
          push(0xEB);
        } else {
          memcpy(block(wblk).data(), wbuf, 512);
          push(0xE5);
          for (int k = 0; k < busy; ++k) push(0x00);
        }
      }
      return;
    }
    if (cn == 0) {
      if ((b & 0xC0) == 0x40) { cmd[0] = b; cn = 1; }
    } else {
      cmd[cn++] = b;
      if (cn == 6) { cn = 0; exec(); }
    }
  }
  uint8_t exchange(uint8_t in) {
    if (!present || !cs) return 0xFF;
    const uint8_t out = pop();
    got(in);
    return out;
  }
};

// ---- мост ----
struct Bridge {
  bool exists = true;
  uint32_t clk10k = 7425;
  uint32_t div = 92, ptr = 0, blkLen = 514;
  bool csOn = false;
  uint32_t tx[512] = {}, rx[512] = {};
  uint8_t last = 0xFF;
  bool tmo = false;
  Card* card = nullptr;
  uint64_t sckCycles = 0;       // тактов SCK всего (для оценки времени)
  uint32_t linkOps = 0;

  uint8_t xchg(uint8_t b) {
    card->cs = csOn;
    sckCycles += 8;
    return card->exchange(b);
  }
  void run(uint32_t v) {
    const uint32_t kind = (v >> 20) & 7, n = v & 0xFFFFF;
    tmo = false;
    if (n == 0) { tmo = kind >= 2; return; }
    auto store = [&](uint32_t i, uint8_t b) {
      uint32_t& w = rx[(ptr + i / 4) & 511];
      const int sh = 24 - 8 * int(i % 4);
      if (i % 4 == 0) w = 0xFFFFFFFF;
      w = (w & ~(0xFFU << sh)) | (uint32_t(b) << sh);
    };
    if (kind == 0 || kind == 1) {
      for (uint32_t i = 0; i < n; ++i) {
        const uint8_t t = kind == 0 ? uint8_t(tx[(ptr + i / 4) & 511] >> (24 - 8 * (i % 4))) : 0xFF;
        const uint8_t r = xchg(t);
        store(i, r);
        last = r;
      }
      return;
    }
    for (uint32_t i = 0; i < n; ++i) {
      const uint8_t r = xchg(0xFF);
      last = r;
      const bool met = kind == 4 ? r == 0xFF : r != 0xFF;
      if (!met) continue;
      if (kind == 3 && r == 0xFE && blkLen) {
        for (uint32_t k = 0; k < blkLen; ++k) store(k, xchg(0xFF));
      }
      return;
    }
    tmo = true;
  }
  uint32_t stat() const { return exists ? (0x5D01U << 16) | (uint32_t(last) << 8) | (tmo ? 2 : 0) : 0; }
};

Bridge bridge;
uint32_t fakeMs = 0;

bool ioRead(void*, uint32_t a, uint32_t* w, size_t n) {
  ++bridge.linkOps;
  ++fakeMs;                     // грубо: по мс на обмен — таймауты драйвера срабатывают
  if (n > 16) return false;
  for (size_t i = 0; i < n; ++i, a += 4) {
    if (a >= 0x2000 && a < 0x4000) w[i] = bridge.exists ? bridge.rx[(a - 0x2000) / 4 % 512] : 0;
    else if (a == 0x100) w[i] = bridge.exists ? (bridge.clk10k << 16) | (bridge.csOn ? 0x100 : 0) | bridge.div : 0;
    else if (a == 0x104) w[i] = bridge.exists ? (bridge.blkLen << 16) | bridge.ptr : 0;
    else if (a == 0x108) w[i] = bridge.stat();
    else w[i] = 0;
  }
  return true;
}

bool ioWrite(void*, uint32_t a, const uint32_t* w, size_t n) {
  ++bridge.linkOps;
  ++fakeMs;
  if (n > 16) return false;
  for (size_t i = 0; i < n; ++i, a += 4) {
    if (!bridge.exists) continue;
    if (a >= 0x2000 && a < 0x2800) bridge.tx[(a - 0x2000) / 4] = w[i];
    else if (a == 0x100) { bridge.div = w[i] & 0xFF; bridge.csOn = (w[i] >> 8) & 1; bridge.card->cs = bridge.csOn; }
    else if (a == 0x104) { bridge.ptr = w[i] & 0x1FF; bridge.blkLen = (w[i] >> 16) & 0x7FF; }
    else if (a == 0x108) bridge.run(w[i]);
  }
  return true;
}

uint32_t nowMs(void*) { return fakeMs; }

const SdBridgeIo io = {nullptr, ioRead, ioWrite, nowMs};

void reset(Card& c) {
  bridge = Bridge();
  bridge.card = &c;
  fakeMs = 0;
}

void pattern(uint8_t* d, uint32_t n, uint32_t seed) {
  for (uint32_t i = 0; i < n; ++i) d[i] = uint8_t((i * 31 + seed * 17) ^ (i >> 8));
}

}  // namespace

int main() {
  // CRC7 команд: CMD0 — 0x95, CMD8(0x1AA) — 0x87
  {
    const uint8_t c0[5] = {0x40, 0, 0, 0, 0}, c8[5] = {0x48, 0, 0, 1, 0xAA};
    CHECK(zifi::sdCrc7(c0, 5) == 0x95, "CRC7 CMD0 %02X", zifi::sdCrc7(c0, 5));
    CHECK(zifi::sdCrc7(c8, 5) == 0x87, "CRC7 CMD8 %02X", zifi::sdCrc7(c8, 5));
  }

  // 1. SDHC: инициализация, объём, быстрый SCK, чтение и запись, адреса блоками
  {
    Card card;
    reset(card);
    SdBridgeCard sd(io);
    const char* e = sd.init();
    CHECK(e == nullptr, "SDHC init: %s", e ? e : "");
    CHECK(sd.highCapacity(), "SDHC не распознана");
    CHECK(sd.sectors() == card.sectors, "объём %u, ждали %u", sd.sectors(), card.sectors);
    CHECK(bridge.div == 1, "быстрый делитель %u (ждали 1: 74,25 МГц / 4 = 18,6 МГц)", bridge.div);
    CHECK(sd.sckKhz() == 18562, "SCK %u кГц", sd.sckKhz());
    CHECK(card.badCrc == 0, "команд с неверным CRC7: %d", card.badCrc);
    static uint8_t buf[8 * 512], back[8 * 512];
    CHECK(sd.read(100, buf, 4), "чтение 4 блоков");
    bool same = true;
    for (int b = 0; b < 4; ++b) same = same && memcmp(buf + 512 * b, card.block(100 + b).data(), 512) == 0;
    CHECK(same, "прочитано не то");
    CHECK(card.readAddrs.size() >= 4 && card.readAddrs.back() == 103, "SDHC: адрес блоками (%u)",
          card.readAddrs.empty() ? 0 : card.readAddrs.back());
    pattern(buf, sizeof buf, 1);
    const uint32_t ops0 = bridge.linkOps;
    const uint64_t sck0 = bridge.sckCycles;
    CHECK(sd.write(2000, buf, 8), "запись 8 блоков");
    const uint32_t opsW = bridge.linkOps - ops0;
    CHECK(sd.read(2000, back, 8) && memcmp(buf, back, sizeof buf) == 0, "записанное не читается");
    const uint32_t opsR = bridge.linkOps - ops0 - opsW;
    printf("SDHC 64 МБ: init за %u обменов LINK; блок: запись — %u обменов, чтение — %u; SCK на блок чтения ~%llu тактов\n",
           ops0, opsW / 8, opsR / 8, static_cast<unsigned long long>((bridge.sckCycles - sck0) / 16));
    CHECK(card.writeAddrs.back() == 2007, "SDHC: адрес записи %u", card.writeAddrs.back());
    sd.release();
    CHECK(!bridge.csOn, "release: CS остался активным");
  }

  // 2. Помехи: один искажённый блок — повтор; три подряд — ошибка
  {
    Card card;
    reset(card);
    SdBridgeCard sd(io);
    CHECK(sd.init() == nullptr, "init");
    static uint8_t buf[512];
    card.corruptReads = 1;
    CHECK(sd.read(7, buf, 1) && memcmp(buf, card.block(7).data(), 512) == 0, "чтение после одного сбоя CRC");
    CHECK(sd.crcRetries() == 1, "повторов %u", sd.crcRetries());
    card.corruptReads = 3;
    CHECK(!sd.read(8, buf, 1), "три сбоя CRC подряд — должна быть ошибка");
    CHECK(sd.errors() == 1, "ошибок %u", sd.errors());
    card.corruptReads = 0;
    CHECK(sd.read(8, buf, 1), "после сбоев чтение снова идёт");
    // запись: один отказ — повтор; два — ошибка
    pattern(buf, 512, 5);
    card.rejectWrites = 1;
    CHECK(sd.write(9, buf, 1) && memcmp(card.block(9).data(), buf, 512) == 0, "запись после одного отказа");
    card.rejectWrites = 2;
    CHECK(!sd.write(10, buf, 1), "два отказа записи — должна быть ошибка");
    // блок за концом карты — ошибка, не зависание
    CHECK(!sd.read(card.sectors + 5, buf, 1), "чтение за концом карты");
  }

  // 3. SDSC v1: CMD8 недопустима, адрес байтами, CMD16, CSD 1.0
  {
    Card card;
    card.sdhc = false;
    card.sectors = 2 * 1024 * 1024 / 512 * 64;   // 64 МБ
    reset(card);
    SdBridgeCard sd(io);
    const char* e = sd.init();
    CHECK(e == nullptr, "SDSC init: %s", e ? e : "");
    CHECK(!sd.highCapacity(), "SDSC принята за SDHC");
    CHECK(sd.sectors() == card.sectors, "SDSC объём %u, ждали %u", sd.sectors(), card.sectors);
    static uint8_t buf[512];
    CHECK(sd.read(33, buf, 1) && memcmp(buf, card.block(33).data(), 512) == 0, "SDSC чтение");
    CHECK(card.readAddrs.back() == 33 * 512, "SDSC: адрес байтами (%u)", card.readAddrs.back());
  }

  // 4. Долгий ACMD41 (готова на 50-й раз), карта не выходит из idle, карты нет, моста нет
  {
    Card card;
    card.acmd41Needed = 50;
    reset(card);
    SdBridgeCard sd(io);
    CHECK(sd.init() == nullptr, "долгий ACMD41");
    Card stuck;
    stuck.acmd41Needed = 1000000;
    reset(stuck);
    SdBridgeCard sd2(io);
    const char* e = sd2.init();
    CHECK(e != nullptr && strstr(e, "ACMD41"), "карта в idle: %s", e ? e : "nullptr");
    Card none;
    none.present = false;
    reset(none);
    SdBridgeCard sd3(io);
    e = sd3.init();
    CHECK(e != nullptr && strstr(e, "no SD card"), "карты нет: %s", e ? e : "nullptr");
    CHECK(!bridge.csOn, "карты нет: CS остался активным");
    Card c4;
    reset(c4);
    bridge.exists = false;
    SdBridgeCard sd4(io);
    CHECK(!sd4.present(), "мост найден в ядре без моста");
    e = sd4.init();
    CHECK(e != nullptr && strstr(e, "no SD bridge"), "ядро без моста: %s", e ? e : "nullptr");
  }

  printf("%s\n", failures ? "ТЕСТ НЕ ПРОЙДЕН" : "ТЕСТ ПРОЙДЕН");
  return failures ? 1 : 0;
}
