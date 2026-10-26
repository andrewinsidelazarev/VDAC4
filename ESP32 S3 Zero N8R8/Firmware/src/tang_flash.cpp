#include "zifi/tang_flash.hpp"

#if ZIFI_HAS_SPI_LINK

#include <stdio.h>
#include <string.h>

#include <LittleFS.h>
#include <esp_cpu.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "zifi/bringup_log.hpp"
#include "zifi/tang_link.hpp"

namespace zifi {
namespace tang_flash {
namespace {

constexpr char kTag[] = "zifi-tang";
constexpr uint32_t kMinCore = 0x0101;           // VERSION[15:0]: мост к flash с 1.1
// Ядро FT812 (личность 02) с 1.31 — без моста: ASPECT и SIZE ему пишет ESP (pushSettings)
constexpr uint32_t kFtNoBridge = 0x011F;
// Последние настройки, которые ESP видела в TS-Config, — на случай её запуска, когда Tang
// уже в FT812 (моста нет, а сектор настроек ESP прочитать негде)
constexpr char kSettingsFile[] = "/tang_set.bin";
constexpr size_t kChunk = 64;                   // буфер моста

// Команды flash (XT25F64B; общие для SPI-flash)
constexpr uint8_t kRead = 0x03;
constexpr uint8_t kPageProgram = 0x02;
constexpr uint8_t kSectorErase = 0x20;
constexpr uint8_t kBlockErase = 0xD8;
constexpr uint8_t kWriteEnable = 0x06;
constexpr uint8_t kReadStatus = 0x05;
constexpr uint8_t kJedec = 0x9F;

// Ячейка настроек (fb_flash.sv)
constexpr uint32_t kMagic = 0x56445354;         // "VDST"
constexpr uint32_t kFormat = 1;
constexpr size_t kSlotBytes = 32;

bool g_available = false;
uint32_t g_jedec = 0;

const char* sizeName(uint32_t size) {
  return size == 1 ? "95%" : size == 2 ? "90%" : "100%";
}
uint32_t g_linkErrors = 0;          // буфер моста прочитан не таким, каким записан
uint32_t g_coreVersion = 0;
// Что сейчас лежит в последней ячейке и куда писать следующую
bool g_haveSaved = false;
uint32_t g_saved[8] = {};
uint32_t g_freeSlot = 0;
int g_savedSlot = -1;
uint32_t g_saves = 0;
// Изменение, увиденное прошлым опросом: пишется, если повторилось
bool g_havePending = false;
uint32_t g_pending[8] = {};
// Последние настройки ядра TS-Config, какими их видел опрос (ещё не записанные — тоже)
bool g_haveSeen = false;
uint32_t g_seen[8] = {};

// ------------------------------------------------------------------
// Мост
// ------------------------------------------------------------------
bool bridge(const uint8_t* tx, size_t txLength, uint8_t* rx, size_t rxLength, bool keepCs) {
  if (txLength > kChunk || rxLength > kChunk) {
    return false;
  }
  if (txLength > 0) {
    uint32_t words[16] = {};
    const size_t count = (txLength + 3) / 4;
    for (size_t i = 0; i < txLength; ++i) {
      words[i / 4] |= static_cast<uint32_t>(tx[i]) << (24 - 8 * (i % 4));
    }
    // Буфер читается обратно: команда или адрес flash, испорченные на линии, записали бы
    // не туда. Сбой — повтор записи буфера (до трёх раз).
    bool landed = false;
    for (int attempt = 0; attempt < 3 && !landed; ++attempt) {
      uint32_t back[16] = {};
      if (!tang::write(tang::kRegFlashBuf, words, count) ||
          !tang::read(tang::kRegFlashBuf, back, count)) {
        return false;
      }
      landed = memcmp(back, words, count * sizeof(uint32_t)) == 0;
      if (!landed) {
        ++g_linkErrors;
      }
    }
    if (!landed) {
      bringupLog(kTag, "Tang flash: the bridge buffer does not take the data");
      return false;
    }
  }
  const uint32_t command = static_cast<uint32_t>(txLength) |
                           (static_cast<uint32_t>(rxLength) << 8) | (keepCs ? 1U << 16 : 0U);
  if (!tang::write(tang::kRegFlashCmd, &command, 1)) {
    return false;
  }
  // 64 байта при 18,6 МГц — 30 мкс; чтение состояния по LINK само занимает ~10 мкс.
  // Предел — по времени FreeRTOS и с последней проверкой после него: задачу могут вытеснить
  // надолго (Wi-Fi, запись журнала во flash ESP останавливает кэш), а мост тем временем
  // давно закончил — по счётчику тактов такое ожидание ложно считалось сбоем.
  const TickType_t started = xTaskGetTickCount();
  for (;;) {
    const bool late = xTaskGetTickCount() - started > pdMS_TO_TICKS(50);
    uint32_t status = 1;
    if (!tang::read(tang::kRegFlashStatus, &status, 1)) {
      return false;
    }
    if ((status & 1) == 0) {
      break;
    }
    if (late) {
      bringupLog(kTag, "Tang flash: the bridge does not finish (status %08X)",
                 static_cast<unsigned>(status));
      return false;
    }
  }
  if (rxLength > 0) {
    uint32_t words[16] = {};
    if (!tang::read(tang::kRegFlashBuf, words, (rxLength + 3) / 4)) {
      return false;
    }
    for (size_t i = 0; i < rxLength; ++i) {
      rx[i] = static_cast<uint8_t>(words[i / 4] >> (24 - 8 * (i % 4)));
    }
  }
  return true;
}

bool simple(uint8_t command) { return bridge(&command, 1, nullptr, 0, false); }

bool withAddress(uint8_t command, uint32_t address) {
  const uint8_t header[4] = {command, static_cast<uint8_t>(address >> 16),
                             static_cast<uint8_t>(address >> 8), static_cast<uint8_t>(address)};
  return bridge(header, 4, nullptr, 0, false);
}

// Дождаться конца записи/стирания. Стирание идёт десятки миллисекунд — между опросами
// задача отдаёт процессор.
// Предел — как в bridge(): «поздно» определяется до чтения состояния, и после предела
// состояние читается ещё раз. Под нагрузкой (FLAC, OPL4 на ядре 0) задачу вытесняют на
// десятки миллисекунд; в 0.1.68 запись страницы, давно законченная, считалась сбоем.
bool waitReady(uint32_t timeoutMs) {
  const TickType_t started = xTaskGetTickCount();
  int polls = 0;
  for (;;) {
    const bool late = xTaskGetTickCount() - started > pdMS_TO_TICKS(timeoutMs);
    uint8_t command = kReadStatus;
    uint8_t status = 0xFF;
    if (!bridge(&command, 1, &status, 1, false)) {
      return false;
    }
    if ((status & 1) == 0) {
      return true;
    }
    if (late) {
      bringupLog(kTag, "Tang flash: still busy after %u ms (status %02X)",
                 static_cast<unsigned>(timeoutMs), status);
      return false;
    }
    if (++polls > 8) {
      vTaskDelay(1);
    }
  }
}

bool programPage(uint32_t address, const uint8_t* data, size_t length) {
  if (!simple(kWriteEnable)) {
    return false;
  }
  uint8_t chunk[kChunk];
  chunk[0] = kPageProgram;
  chunk[1] = static_cast<uint8_t>(address >> 16);
  chunk[2] = static_cast<uint8_t>(address >> 8);
  chunk[3] = static_cast<uint8_t>(address);
  size_t first = length < kChunk - 4 ? length : kChunk - 4;
  memcpy(chunk + 4, data, first);
  size_t done = first;
  if (!bridge(chunk, 4 + first, nullptr, 0, done < length)) {
    return false;
  }
  while (done < length) {
    const size_t part = (length - done) < kChunk ? (length - done) : kChunk;
    if (!bridge(data + done, part, nullptr, 0, done + part < length)) {
      return false;
    }
    done += part;
  }
  return waitReady(100);   // запись страницы: до 2,4 мс у XT25F64B
}

// ------------------------------------------------------------------
// Ячейки настроек
// ------------------------------------------------------------------
void buildRecord(uint32_t ctrl, const uint32_t* table, uint32_t* record) {
  record[0] = kMagic;
  // CTRL: 2 PHASE, [4:3] DELAY, 5 ALIGN, [13:10] CLOCK, 14 ASPECT, [16:15] SIZE (ядро 1.2),
  // 17 FRAME BLEND (1.32) → слово 1: 19 FRAME BLEND, [18:17] SIZE, 16 ASPECT, [15:8] формат, …
  record[1] = (((ctrl >> 17) & 1) << 19) | (((ctrl >> 15) & 3) << 17) | (((ctrl >> 14) & 1) << 16) |
              (kFormat << 8) |
              (((ctrl >> 5) & 1) << 7) | (((ctrl >> 2) & 1) << 6) | (((ctrl >> 3) & 3) << 4) |
              ((ctrl >> 10) & 0xF);
  for (int k = 0; k < 4; ++k) {
    record[2 + k] = (table[2 * k] & 0xFFFF) | ((table[2 * k + 1] & 0xFFFF) << 16);
  }
  record[6] = 0;
  uint32_t sum = 0;
  for (int i = 0; i < 7; ++i) {
    sum += record[i];
  }
  record[7] = ~sum;
}

bool recordValid(const uint32_t* record) {
  if (record[0] != kMagic) {
    return false;
  }
  uint32_t sum = 0;
  for (int i = 0; i < 7; ++i) {
    sum += record[i];
  }
  return record[7] == ~sum;
}

void toBytes(const uint32_t* words, uint8_t* bytes, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    bytes[4 * i] = static_cast<uint8_t>(words[i] >> 24);
    bytes[4 * i + 1] = static_cast<uint8_t>(words[i] >> 16);
    bytes[4 * i + 2] = static_cast<uint8_t>(words[i] >> 8);
    bytes[4 * i + 3] = static_cast<uint8_t>(words[i]);
  }
}

void fromBytes(const uint8_t* bytes, uint32_t* words, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    words[i] = (static_cast<uint32_t>(bytes[4 * i]) << 24) |
               (static_cast<uint32_t>(bytes[4 * i + 1]) << 16) |
               (static_cast<uint32_t>(bytes[4 * i + 2]) << 8) | bytes[4 * i + 3];
  }
}

// Сектор целиком: последняя целая ячейка и первая чистая — так же, как читает ядро.
bool scanSettings() {
  static uint8_t sector[kSectorBytes];
  if (!read(kSettingsAddr, sector, sizeof(sector))) {
    return false;
  }
  g_haveSaved = false;
  g_savedSlot = -1;
  g_freeSlot = kSettingsSlots;
  for (uint32_t slot = 0; slot < kSettingsSlots; ++slot) {
    uint32_t record[8];
    fromBytes(sector + slot * kSlotBytes, record, 8);
    if (record[0] == 0xFFFFFFFF) {
      g_freeSlot = slot;
      break;
    }
    if (recordValid(record)) {
      memcpy(g_saved, record, sizeof(g_saved));
      g_haveSaved = true;
      g_savedSlot = static_cast<int>(slot);
    }
  }
  return true;
}

// Ядро на месте и закончило загрузку настроек: ID и версия — те, что при запуске LINK,
// мост не занят. Пока Tang перезагружается (~3 с: битстрим 0,9 МБ из flash), линии LINK
// висят на подтяжках и читается мусор — его нельзя принять за настройки.
bool coreAlive() {
  uint32_t head[2] = {};
  uint32_t status = 0;
  return tang::read(tang::kRegId, head, 2) && head[0] == tang::kIdValue &&
         head[1] == g_coreVersion && tang::read(tang::kRegFlashStatus, &status, 1) &&
         (status & 3) == 2;
}

bool currentRecord(uint32_t* record) {
  uint32_t ctrl = 0;
  uint32_t table[8] = {};
  tang::LinkGuard guard;
  if (!coreAlive() || !tang::read(tang::kRegCtrl, &ctrl, 1) ||
      !tang::read(tang::kRegAdjTable, table, 8) || !coreAlive()) {
    return false;
  }
  buildRecord(ctrl, table, record);
  return true;
}

// Копия настроек в LittleFS ESP (ячейка как во flash Tang)
void rememberRecord(const uint32_t* record) {
  File file = LittleFS.open(kSettingsFile, "w");
  if (file) {
    file.write(reinterpret_cast<const uint8_t*>(record), 8 * sizeof(uint32_t));
    file.close();
  }
}

bool recallRecord(uint32_t* record) {
  File file = LittleFS.open(kSettingsFile, "r");
  if (!file) {
    return false;
  }
  const bool ok = file.read(reinterpret_cast<uint8_t*>(record), 8 * sizeof(uint32_t)) == 8 * sizeof(uint32_t);
  file.close();
  return ok && recordValid(record);
}

// CTRL из ячейки: источник AUTO, калибровка SD READ не запускается (CTRL[9] = 0)
uint32_t ctrlFromRecord(const uint32_t* record) {
  const uint32_t r = record[1];
  return (((r >> 19) & 1) << 17) | (((r >> 17) & 3) << 15) | (((r >> 16) & 1) << 14) | ((r & 0xF) << 10) |
         (((r >> 7) & 1) << 5) | (((r >> 4) & 3) << 3) | (((r >> 6) & 1) << 2);
}

// Ядро FT812 без моста: ASPECT и SIZE — те, что были в TS-Config (остальное ему не нужно)
void pushSettings() {
  uint32_t record[8];
  const char* from = "TS-Config";
  if (g_haveSeen) {
    memcpy(record, g_seen, sizeof(record));
  } else if (recallRecord(record)) {
    from = "the ESP copy";
  } else {
    bringupLog(kTag, "Tang settings: FT812 core has no flash bridge and no settings are known yet: defaults");
    return;
  }
  const uint32_t ctrl = ctrlFromRecord(record);
  const bool ok = tang::write(tang::kRegCtrl, &ctrl, 1);
  bringupLog(kTag, "Tang settings: FT812 core (no flash bridge) gets aspect %s, size %s from %s: %s",
             ((record[1] >> 16) & 1) ? "16:9" : "4:3", sizeName((record[1] >> 17) & 3), from,
             ok ? "written" : "FAILED");
}

bool saveRecord(const uint32_t* record) {
  tang::LinkGuard guard;
  if (g_freeSlot >= kSettingsSlots) {
    if (!eraseSector(kSettingsAddr)) {
      return false;
    }
    g_freeSlot = 0;
  }
  const uint32_t address = kSettingsAddr + g_freeSlot * kSlotBytes;
  uint8_t bytes[kSlotBytes];
  toBytes(record, bytes, 8);
  // Ячейка занята, даже если запись не удалась: следующая пойдёт в новую
  const uint32_t slot = g_freeSlot++;
  if (!program(address, bytes, sizeof(bytes))) {
    return false;
  }
  uint8_t back[kSlotBytes];
  if (!read(address, back, sizeof(back)) || memcmp(back, bytes, sizeof(bytes)) != 0) {
    bringupLog(kTag, "Tang settings: slot %u reads back wrong", static_cast<unsigned>(slot));
    return false;
  }
  memcpy(g_saved, record, sizeof(g_saved));
  g_haveSaved = true;
  g_savedSlot = static_cast<int>(slot);
  ++g_saves;
  rememberRecord(record);
  return true;
}

}  // namespace

bool available() { return g_available; }

uint32_t jedecId() { return g_jedec; }

bool read(uint32_t address, uint8_t* data, size_t length) {
  if (length == 0) {
    return true;
  }
  tang::LinkGuard guard;
  const uint8_t header[4] = {kRead, static_cast<uint8_t>(address >> 16),
                             static_cast<uint8_t>(address >> 8), static_cast<uint8_t>(address)};
  // Заголовок — отдельной командой с удержанием CS, дальше данные по 64 байта
  if (!bridge(header, 4, nullptr, 0, true)) {
    return false;
  }
  size_t done = 0;
  while (done < length) {
    const size_t part = (length - done) < kChunk ? (length - done) : kChunk;
    if (!bridge(nullptr, 0, data + done, part, done + part < length)) {
      return false;
    }
    done += part;
  }
  return true;
}

bool eraseSector(uint32_t address) {
  tang::LinkGuard guard;
  return simple(kWriteEnable) && withAddress(kSectorErase, address & ~(kSectorBytes - 1)) &&
         waitReady(1000);
}

bool eraseBlock(uint32_t address) {
  tang::LinkGuard guard;
  return simple(kWriteEnable) && withAddress(kBlockErase, address & ~(kBlockBytes - 1)) &&
         waitReady(5000);
}

bool program(uint32_t address, const uint8_t* data, size_t length) {
  tang::LinkGuard guard;
  while (length > 0) {
    const uint32_t room = kPageBytes - (address % kPageBytes);
    const size_t part = length < room ? length : room;
    if (!programPage(address, data, part)) {
      return false;
    }
    address += part;
    data += part;
    length -= part;
  }
  return true;
}

bool slotHasBitstream(uint32_t address) {
  static const uint8_t kIdcode[] = {0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x1B};
  uint8_t head[64] = {};
  if (!read(address, head, sizeof(head))) {
    return false;
  }
  bool sync = false;
  bool idcode = false;
  for (size_t i = 0; i + sizeof(kIdcode) <= sizeof(head); ++i) {
    sync = sync || (head[i] == 0xA5 && head[i + 1] == 0xC3);
    idcode = idcode || memcmp(head + i, kIdcode, sizeof(kIdcode)) == 0;
  }
  return sync && idcode;
}

void begin(uint32_t coreVersion) {
  g_available = false;
  if ((coreVersion >> 24) == 0x02 && (coreVersion & 0xFFFF) >= kFtNoBridge) {
    pushSettings();
    return;
  }
  if ((coreVersion & 0xFFFF) < kMinCore) {
    bringupLog(kTag, "Tang settings: core %u.%u has no flash bridge, settings are not saved",
               static_cast<unsigned>((coreVersion >> 8) & 0xFF),
               static_cast<unsigned>(coreVersion & 0xFF));
    return;
  }
  g_coreVersion = coreVersion;
  tang::LinkGuard guard;
  uint32_t status = 0;
  // Ядро читает настройки ~2 мс после старта; LINK к этому времени давно работает
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (tang::read(tang::kRegFlashStatus, &status, 1) && (status & 3) == 2) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if ((status & 3) != 2) {
    bringupLog(kTag, "Tang settings: the flash bridge does not answer (status %08X)",
               static_cast<unsigned>(status));
    return;
  }
  // JEDEC ID — три раза подряд одинаковый
  uint32_t ids[3] = {};
  for (uint32_t& value : ids) {
    uint8_t command = kJedec;
    uint8_t id[3] = {};
    if (!bridge(&command, 1, id, 3, false)) {
      bringupLog(kTag, "Tang settings: the flash bridge fails");
      return;
    }
    value = (static_cast<uint32_t>(id[0]) << 16) | (static_cast<uint32_t>(id[1]) << 8) | id[2];
  }
  g_jedec = ids[0];
  if (ids[1] != ids[0] || ids[2] != ids[0]) {
    bringupLog(kTag, "Tang settings: JEDEC ID unstable (%06X %06X %06X)",
               static_cast<unsigned>(ids[0]), static_cast<unsigned>(ids[1]),
               static_cast<unsigned>(ids[2]));
    return;
  }
  if (g_jedec == 0 || g_jedec == 0xFFFFFF) {
    bringupLog(kTag, "Tang settings: no flash (JEDEC %06X)", static_cast<unsigned>(g_jedec));
    return;
  }
  if (!scanSettings()) {
    bringupLog(kTag, "Tang settings: cannot read the settings sector");
    return;
  }
  g_available = true;
  if (!g_haveSaved) {
    // Сохранять есть смысл только изменения: текущие (значения по умолчанию) — исходная точка
    currentRecord(g_saved);
    g_haveSaved = true;
  }
  if ((coreVersion >> 24) == 0x01) {
    memcpy(g_seen, g_saved, sizeof(g_seen));
    g_haveSeen = true;
    uint32_t copy[8];
    if (!recallRecord(copy) || memcmp(copy, g_saved, sizeof(copy)) != 0) {
      rememberRecord(g_saved);
    }
  }
  bringupLog(kTag,
             "Tang settings: flash JEDEC %06X, core loaded %s (slot %u), last saved slot %d, "
             "free slot %u of %u",
             static_cast<unsigned>(g_jedec), (status & 4) ? "saved settings" : "defaults",
             static_cast<unsigned>((status >> 8) & 0xFF), g_savedSlot,
             static_cast<unsigned>(g_freeSlot), static_cast<unsigned>(kSettingsSlots));
}

void poll() {
  if (!g_available) {
    return;
  }
  uint32_t record[8];
  if (!currentRecord(record)) {
    g_havePending = false;
    return;
  }
  if ((g_coreVersion >> 24) == 0x01) {
    memcpy(g_seen, record, sizeof(g_seen));
    g_haveSeen = true;
  }
  if (memcmp(record, g_saved, sizeof(record)) == 0) {
    g_havePending = false;
    return;
  }
  if (!g_havePending || memcmp(record, g_pending, sizeof(record)) != 0) {
    memcpy(g_pending, record, sizeof(record));
    g_havePending = true;
    return;
  }
  g_havePending = false;
  if (saveRecord(record)) {
    bringupLog(kTag,
               "Tang settings saved to flash slot %d: clock %X delay %u phase %u align %u aspect %s "
               "size %s",
               g_savedSlot, static_cast<unsigned>(record[1] & 0xF),
               static_cast<unsigned>(((record[1] >> 4) & 3) + 1),
               static_cast<unsigned>((record[1] >> 6) & 1), static_cast<unsigned>((record[1] >> 7) & 1),
               ((record[1] >> 16) & 1) ? "16:9" : "4:3", sizeName((record[1] >> 17) & 3));
  } else {
    bringupLog(kTag, "Tang settings: saving FAILED");
  }
}

void describe(char* out, size_t size) {
  if (!g_available) {
    snprintf(out, size, "settings storage off");
    return;
  }
  snprintf(out, size,
           "flash %06X, last saved slot %d, free slot %u/%u, saves since start %u, "
           "bridge buffer re-sent %u times",
           static_cast<unsigned>(g_jedec), g_savedSlot, static_cast<unsigned>(g_freeSlot),
           static_cast<unsigned>(kSettingsSlots), static_cast<unsigned>(g_saves),
           static_cast<unsigned>(g_linkErrors));
}

}  // namespace tang_flash
}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
