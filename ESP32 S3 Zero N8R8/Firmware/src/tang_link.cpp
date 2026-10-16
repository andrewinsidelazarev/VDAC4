#include "zifi/tang_link.hpp"

#if ZIFI_HAS_SPI_LINK

#include <atomic>
#include <stdio.h>
#include <string.h>

#include <driver/gpio.h>
#include <esp_cpu.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs.h>

#include "zifi/board_state.hpp"
#include "zifi/bringup_log.hpp"
#include "zifi/diagnostic_log.hpp"
#include "zifi/link_spi3.hpp"
#include "zifi/tang_flash.hpp"

namespace zifi {

namespace {

constexpr char kTag[] = "zifi-tang";
// Выводы LINK (board.hpp, Tang Nano 20k/Docs/a3-interface-pinmap.md).
constexpr int kSckPin = 5;
constexpr int kCsPin = board::kLinkCsPin;
constexpr int kIo0Pin = 4;
constexpr int kIo1Pin = 7;
constexpr int kIo2Pin = 6;
constexpr int kIo3Pin = 44;
constexpr int kIrqPin = 9;

// Канал — аппаратный SPI3 без драйвера ESP-IDF (link_spi3.cpp, там же — почему).
constexpr uint8_t kCmdWrite = 0x01;
constexpr uint8_t kCmdRead = 0x02;
constexpr int kDummyCycles = 8;       // DUMMY в fb_link.sv
constexpr size_t kMaxWords = 16;

// Частоты LINK_SCK по убыванию (делители 80 МГц). Поиск Tang — на самой медленной.
constexpr uint32_t kSpeeds[] = {40000000, 26666667, 20000000, 13333333, 10000000, 5000000};
constexpr uint32_t kFindHz = 1000000;
// Ответ Tang приходит к ESP с задержкой (матрица GPIO ESP, выход ПЛИС, проводники). На
// высокой частоте она дольше полутакта: ESP видит тетраду, выданную тактом раньше. Тогда
// ESP даёт на 1–2 такта разворота больше, чем считает Tang, — лишние тетрады в конце
// чтения просто отбрасываются (чтение регистров побочных действий не имеет).
constexpr int kExtraDummyMax = 2;
constexpr int kQuickRounds = 50;      // отбор частоты
constexpr int kScratchRounds = 1000;  // проверка выбранной частоты
constexpr int kFindAttempts = 20;     // ID ищется 5 с: Tang может ещё грузиться
constexpr uint32_t kStatusPeriodMs = 2000;
// Запуск — когда уже поднялись сеть и канал Z80: если что-то пойдёт не так, журнал
// во flash покажет, на каком шаге.
constexpr uint32_t kStartDelayMs = 10000;
// Защита: перед запуском LINK в NVS пишется версия прошивки, после успешного запуска
// запись стирается. Если при включении она уже есть — прошлый запуск этой прошивки
// не дошёл до конца, и LINK в этот раз не запускается.
constexpr char kNvsNamespace[] = "zifi-tang";
constexpr char kNvsKey[] = "try";

enum class State : uint8_t { kOff, kNone, kReady };

std::atomic<bool> g_ownsCs{false};
std::atomic<bool> g_boardPowered{false};
std::atomic<State> g_state{State::kOff};
std::atomic<uint32_t> g_version{0};
SemaphoreHandle_t g_lock = nullptr;
int g_dummy = kDummyCycles;

// Шаг запуска — в журнал и сразу во flash: если следующий шаг зависнет, этот останется
void step(const char* text) {
  bringupLog(kTag, "LINK step: %s", text);
  diagnosticLogFlush();
}

bool guardIsSet() {
  nvs_handle_t handle;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
    return false;
  }
  char stored[32] = {};
  size_t length = sizeof(stored);
  const bool found = nvs_get_str(handle, kNvsKey, stored, &length) == ESP_OK &&
                     strcmp(stored, ZIFI_BUILD_VERSION) == 0;
  nvs_close(handle);
  return found;
}

void setGuard(bool on) {
  nvs_handle_t handle;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    return;
  }
  if (on) {
    nvs_set_str(handle, kNvsKey, ZIFI_BUILD_VERSION);
  } else {
    nvs_erase_key(handle, kNvsKey);
  }
  nvs_commit(handle);
  nvs_close(handle);
}

// Tang под питанием: LINK_IRQ_N (FPGA85) на модуле Tang подтянут 10 кОм к его +3,3 В
// (подтяжка SD DAT1), ПЛИС эту линию не ведёт. Встроенная подтяжка ESP вниз (~45 кОм)
// её не перетягивает; у обесточенной Tang линия через те же 10 кОм лежит на нуле.
// Нужно для стола: Tang от USB, плата без Evo — R10 на линии CS тогда идёт к обесточенной
// шине логики, и probeBoardState() питания не видит.
bool tangIsPowered() {
  const gpio_num_t gpio = static_cast<gpio_num_t>(kIrqPin);
  gpio_set_direction(gpio, GPIO_MODE_INPUT);
  gpio_pullup_dis(gpio);
  gpio_pulldown_en(gpio);
  esp_rom_delay_us(300);
  const int level = gpio_get_level(gpio);
  gpio_pulldown_dis(gpio);
  return level == 1;
}

// Мьютекс рекурсивный: операция с flash Tang (tang_flash.cpp) держит LINK на все свои
// обмены (tang::LinkGuard), а внутри них снова берёт его на каждый обмен.
bool lockedRead(uint32_t address, uint32_t* words, size_t count) {
  xSemaphoreTakeRecursive(g_lock, portMAX_DELAY);
  const bool ok = link_spi3::transfer(kCmdRead, address, words, count, true, g_dummy);
  xSemaphoreGiveRecursive(g_lock);
  return ok;
}

bool lockedWrite(uint32_t address, const uint32_t* words, size_t count) {
  uint32_t copy[kMaxWords];
  if (count > kMaxWords) {
    return false;
  }
  memcpy(copy, words, count * sizeof(uint32_t));
  xSemaphoreTakeRecursive(g_lock, portMAX_DELAY);
  const bool ok = link_spi3::transfer(kCmdWrite, address, copy, count, false, 0);
  xSemaphoreGiveRecursive(g_lock);
  return ok;
}

// Проверка канала на текущей частоте: запись проверочного регистра и чтение его обратно
// одним словом и в конце чтения 16 слов с адреса 0 (там же сверяются ID и версия).
int countErrors(int rounds, uint32_t version) {
  int errors = 0;
  for (int round = 0; round < rounds; ++round) {
    const uint32_t value = esp_random();
    uint32_t back = 0;
    uint32_t block[16] = {};
    if (!lockedWrite(tang::kRegScratch, &value, 1) ||
        !lockedRead(tang::kRegScratch, &back, 1) || back != value ||
        !lockedRead(tang::kRegId, block, 16) || block[0] != tang::kIdValue ||
        block[1] != version || block[tang::kRegScratch / 4] != value) {
      ++errors;
    }
  }
  return errors;
}

// Выбор частоты: самая высокая, на которой проходят все проверки, и при ней — число
// тактов разворота. Возвращает false, если не подошла ни одна (канал остаётся на kFindHz).
// Частоты с номера from: первая, где проходят все проверки, — её номер (частота и такты
// разворота остаются установленными); -1 — ни одной (канал на kFindHz).
int pickSpeed(uint32_t version, int from) {
  char line[160];
  constexpr int kCount = sizeof(kSpeeds) / sizeof(kSpeeds[0]);
  for (int index = from; index < kCount; ++index) {
    const uint32_t hz = kSpeeds[index];
    if (!link_spi3::setClock(hz)) {
      continue;
    }
    int length = snprintf(line, sizeof(line), "%u kHz:",
                          static_cast<unsigned>(link_spi3::clockHz() / 1000));
    for (int extra = 0; extra <= kExtraDummyMax; ++extra) {
      g_dummy = kDummyCycles + extra;
      const int quick = countErrors(kQuickRounds, version);
      int full = -1;
      if (quick == 0) {
        full = countErrors(kScratchRounds, version);
      }
      if (length > 0 && length < static_cast<int>(sizeof(line))) {
        length += full < 0
                      ? snprintf(line + length, sizeof(line) - length, " +%d: %d/%d bad;", extra,
                                 quick, kQuickRounds)
                      : snprintf(line + length, sizeof(line) - length, " +%d: %d/%d bad;", extra,
                                 full, kScratchRounds);
      }
      if (full == 0) {
        bringupLog(kTag, "LINK speed %s passed", line);
        return index;
      }
    }
    bringupLog(kTag, "LINK speed %s no", line);
  }
  link_spi3::setClock(kFindHz);
  g_dummy = kDummyCycles;
  return -1;
}

// Скорость обмена: 1000 чтений по 16 слов.
void measureThroughput() {
  uint32_t block[16];
  const uint32_t started = esp_cpu_get_cycle_count();
  for (int i = 0; i < 1000; ++i) {
    lockedRead(tang::kRegId, block, 16);
  }
  const uint32_t us = (esp_cpu_get_cycle_count() - started) / 240;
  bringupLog(kTag, "LINK throughput: 1000 reads of 64 bytes in %u us (%u kB/s)",
             static_cast<unsigned>(us), static_cast<unsigned>(us ? 64000000ULL / us : 0));
}

const char* modeName(uint32_t mode) {
  static const char* const kNames[] = {"unknown", "TS Pentagon", "TS PENT_312", "Base Pentagon",
                                       "Base 48k", "Base 128k", "test A", "test B"};
  return kNames[mode & 7];
}

// Состояние Tang в журнал — когда меняется растр, окно, калибровка или настройки
// (r[8], r[9] — CTRL и ADJUST).
void reportStatus(const uint32_t* r, uint32_t* last) {
  const uint32_t status = r[0];
  const uint32_t key[] = {status & 0xF3FF, r[1], r[3], r[2] >> 16, r[8], r[9]};
  bool changed = false;
  for (size_t i = 0; i < 6; ++i) {
    if (key[i] != last[i]) {
      changed = true;
      last[i] = key[i];
    }
  }
  // PCLK дрожит в последнем знаке: заметной считается разница больше 50 кГц
  const uint32_t pclk = status >> 16;
  if (pclk + 50 < last[6] || pclk > last[6] + 50) {
    changed = true;
  }
  if (!changed) {
    return;
  }
  last[6] = pclk;
  const bool locked = (status & 1) != 0;
  bringupLog(kTag,
             "Tang: %s%s, PCLK %u kHz, line %u, %u lines, HS %u, sync %c%c, "
             "window %u/%u, picture x %u-%u y %u-%u, frames %u bad %u, %s%s, SD read map %X%s",
             locked ? "" : "no raster ", locked ? modeName(status >> 1) : "",
             static_cast<unsigned>(pclk), static_cast<unsigned>((r[1] & 0xFFF) / 2),
             static_cast<unsigned>((r[1] >> 16) & 0x7FF), static_cast<unsigned>((r[2] & 0xFFF) / 2),
             (status & 0x10) ? '-' : '+', (status & 0x20) ? '-' : '+',
             static_cast<unsigned>(r[3] & 0x7FF), static_cast<unsigned>((r[3] >> 16) & 0x7FF),
             static_cast<unsigned>(r[4] & 0x7FF), static_cast<unsigned>((r[4] >> 16) & 0x7FF),
             static_cast<unsigned>(r[5] & 0x7FF), static_cast<unsigned>((r[5] >> 16) & 0x7FF),
             static_cast<unsigned>(r[6] & 0xFFFF), static_cast<unsigned>(r[6] >> 16),
             (status & 0x200) ? "test pattern" : "Evo picture",
             (status & 0x1000) ? ", M_SEL=1" : "",
             static_cast<unsigned>((r[2] >> 16) & 0xF),
             (status & 0x8000) ? " (calibration failed)" : "");
  static const char* const kSources[] = {"AUTO", "EVO", "TEST", "?"};
  const uint32_t ctrl = r[8];
  bringupLog(kTag, "Tang settings: source %s, MUX phase %u delay %u clock %X, align %u, "
             "SD read %u, OSD %u, aspect %s, size %s, H %d V %d",
             kSources[ctrl & 3], static_cast<unsigned>((ctrl >> 2) & 1),
             static_cast<unsigned>(((ctrl >> 3) & 3) + 1), static_cast<unsigned>((ctrl >> 10) & 0xF),
             static_cast<unsigned>((ctrl >> 5) & 1), static_cast<unsigned>((ctrl >> 6) & 3),
             static_cast<unsigned>((ctrl >> 8) & 1), ((ctrl >> 14) & 1) ? "16:9" : "4:3",
             ((ctrl >> 15) & 3) == 1 ? "95%" : ((ctrl >> 15) & 3) == 2 ? "90%" : "100%",
             static_cast<int>(static_cast<int8_t>(r[9] & 0xFF)),
             static_cast<int>(static_cast<int8_t>((r[9] >> 8) & 0xFF)));
}

void taskMain(void*) {
  vTaskDelay(pdMS_TO_TICKS(kStartDelayMs));
  for (;;) {
    const BoardState board = probeBoardState();
    if (board == BoardState::kPowered) {
      g_boardPowered = true;
      break;
    }
    if (tangIsPowered()) {
      // Стол: Tang от USB, плата без питания. Признак питания платы для остальных
      // (звук) остаётся «нет питания» — см. tangLinkBoardPowered().
      break;
    }
    if (board == BoardState::kAbsent) {
      bringupLog(kTag, "LINK off: the module is out of the board");
      vTaskDelete(nullptr);
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  if (guardIsSet()) {
    g_state = State::kNone;
    bringupLog(kTag, "LINK skipped: the previous start of %s did not finish, see the steps above",
               ZIFI_BUILD_VERSION);
    vTaskDelete(nullptr);
    return;
  }
  setGuard(true);
  step(g_boardPowered ? "board is powered, starting SPI3"
                      : "Tang is powered (board is not: bench), starting SPI3");

  // С этой минуты GPIO3 — выход CS: проверки питания платы его больше не трогают
  g_ownsCs = true;
  const link_spi3::Pins pins = {kSckPin, kCsPin, kIo0Pin, kIo1Pin, kIo2Pin, kIo3Pin};
  if (!link_spi3::begin(pins, kFindHz)) {
    g_state = State::kNone;
    bringupLog(kTag, "LINK: SPI3 did not start");
    setGuard(false);
    vTaskDelete(nullptr);
    return;
  }
  step("SPI3 started, looking for Tang at 1 MHz");

  // Поиск Tang: она может ещё грузить битстрим
  bool found = false;
  uint32_t id = 0;
  for (int attempt = 0; attempt < kFindAttempts && !found; ++attempt) {
    found = lockedRead(tang::kRegId, &id, 1) && id == tang::kIdValue;
    if (!found) {
      vTaskDelay(pdMS_TO_TICKS(250));
    }
  }
  if (!found) {
    g_state = State::kNone;
    bringupLog(kTag, "LINK: no Tang answer (ID %08X): the Tang core has no LINK or is not loaded",
               static_cast<unsigned>(id));
    setGuard(false);
    vTaskDelete(nullptr);
    return;
  }
  uint32_t version = 0;
  lockedRead(tang::kRegVersion, &version, 1);
  g_version = version;
  step("Tang found, choosing the speed");

  // Рабочая частота — на ступень ниже предельной: в Evo на 40 МГц изредка проскакивала
  // ошибка (0.1.70: 1 на ~6000 обменов после 0 из 1000 при выборе), а для записи flash Tang
  // нужен запас. Ошибка в итоговой проверке — ещё ступень ниже, а не отказ LINK.
  int index = pickSpeed(version, 0);
  if (index >= 0) {
    const int lower = pickSpeed(version, index + 1);
    index = lower >= 0 ? lower : pickSpeed(version, index);
  }
  int errors = countErrors(kScratchRounds, version);
  while (errors != 0 && index >= 0) {
    bringupLog(kTag, "LINK: %d check errors at %u kHz, going lower", errors,
               static_cast<unsigned>(link_spi3::clockHz() / 1000));
    index = pickSpeed(version, index + 1);
    errors = countErrors(kScratchRounds, version);
  }
  const bool fast = index >= 0;
  g_state = errors == 0 ? State::kReady : State::kNone;
  setGuard(false);
  bringupLog(kTag,
             "LINK %s: Tang VDC4 core %02X, protocol %u, version %u.%u; %u kHz, %d turnaround "
             "cycles; check errors %d of %d",
             errors == 0 ? "ready" : "FAILED", static_cast<unsigned>(version >> 24),
             static_cast<unsigned>((version >> 16) & 0xFF),
             static_cast<unsigned>((version >> 8) & 0xFF), static_cast<unsigned>(version & 0xFF),
             static_cast<unsigned>(link_spi3::clockHz() / 1000), g_dummy, errors,
             kScratchRounds);
  if (!fast) {
    bringupLog(kTag, "LINK: no fast speed passed, staying at 1 MHz");
  }
  if (errors != 0) {
    vTaskDelete(nullptr);
    return;
  }
  measureThroughput();
  // Настройки во flash Tang: ядро 1.1 и новее
  tang_flash::begin(version);

  uint32_t last[7] = {0xFFFFFFFF, 0, 0, 0, 0, 0, 0};
  for (;;) {
    uint32_t regs[10] = {};
    if (lockedRead(tang::kRegStatus, regs, 8) && lockedRead(tang::kRegCtrl, regs + 8, 2)) {
      reportStatus(regs, last);
    }
    tang_flash::poll();
    vTaskDelay(pdMS_TO_TICKS(kStatusPeriodMs));
  }
}

}  // namespace

namespace tang {

void start() {
  if (g_lock != nullptr) {
    return;
  }
  g_lock = xSemaphoreCreateRecursiveMutex();
  if (g_lock == nullptr ||
      xTaskCreatePinnedToCore(taskMain, "zifi-tang", 4096, nullptr, 1, nullptr, 0) != pdPASS) {
    ESP_LOGE(kTag, "LINK task not started");
  }
}

bool ready() { return g_state == State::kReady; }

uint32_t coreVersion() { return ready() ? g_version.load() : 0; }

LinkGuard::LinkGuard() { xSemaphoreTakeRecursive(g_lock, portMAX_DELAY); }

LinkGuard::~LinkGuard() { xSemaphoreGiveRecursive(g_lock); }

uint32_t clockHz() { return ready() ? link_spi3::clockHz() : 0; }

bool read(uint32_t address, uint32_t* words, size_t count) {
  return ready() && lockedRead(address, words, count);
}

bool write(uint32_t address, const uint32_t* words, size_t count) {
  return ready() && lockedWrite(address, words, count);
}

void describe(char* out, size_t size) {
  switch (g_state.load()) {
    case State::kReady: {
      const uint32_t version = g_version;
      snprintf(out, size, "VDC4/%u.%u@%uMHz", static_cast<unsigned>((version >> 8) & 0xFF),
               static_cast<unsigned>(version & 0xFF),
               static_cast<unsigned>(link_spi3::clockHz() / 1000000));
      break;
    }
    case State::kNone:
      snprintf(out, size, "none");
      break;
    default:
      snprintf(out, size, "off");
      break;
  }
}

}  // namespace tang

bool tangLinkOwnsCs() { return g_ownsCs; }

bool tangLinkBoardPowered() { return g_boardPowered; }

}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
