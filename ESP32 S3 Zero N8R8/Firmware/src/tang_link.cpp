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
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs.h>

#include "zifi/board_state.hpp"
#include "zifi/bringup_log.hpp"
#include "zifi/diagnostic_log.hpp"
#include "zifi/ft_rom.hpp"
#include "zifi/link_spi3.hpp"
#include "zifi/tang_flash.hpp"
#include "zifi/tang_menu.hpp"
#include "zifi/tang_sd.hpp"

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
constexpr uint32_t kMenuPeriodMs = 20;     // меню Tang (tang_menu): нажатия плагина WC
constexpr int kReloadRounds = 100;         // проверка канала после перезагрузки Tang
constexpr int kReloadMisses = 3;           // столько проверок (по kStatusPeriodMs) без ID — канал заново
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
std::atomic<uint32_t> g_reloads{0};
std::atomic<bool> g_coreClaimed{false};   // у ядра из кэша есть хозяин (claimCore)
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
//
// Ожидание LINK ограничено (tang::kLockWaitMs): задача, которая не отпускает LINK, не должна
// останавливать навсегда остальных (консоль, обмен с Evo). Кто не дождался — отказ обмена и
// запись в журнал (во flash тоже: если сеть встанет, запись прочитается после перезапуска),
// кто держит LINK: задача, где взяла (файл и строка вызова), что первым делала, сколько держит.
// Поля держателя пишет только держатель, пока держит; читают их ожидающие — для журнала.
constexpr uint32_t kLockReportMs = 10000;      // запись «LINK занят» — не чаще
std::atomic<uint32_t> g_heldSince{0};         // мс, когда взят (0 — свободен)
std::atomic<const char*> g_heldFile{nullptr};
std::atomic<int> g_heldLine{0};
std::atomic<uint32_t> g_heldAddress{0};
int g_heldDepth = 0;                          // вложенность у держателя
std::atomic<uint32_t> g_lockFails{0};         // ожиданий с отказом с запуска
std::atomic<uint32_t> g_lockReportAt{0};
// LinkGuard этой задачи LINK не получила: её обмены, пока объект жив, сразу отказывают
// (иначе каждый ждал бы kLockWaitMs заново)
thread_local int t_failedGuards = 0;

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

const char* baseName(const char* path) {
  if (path == nullptr) {
    return "?";
  }
  const char* name = path;
  for (const char* p = path; *p != 0; ++p) {
    if (*p == '/' || *p == '\\') {
      name = p + 1;
    }
  }
  return name;
}

const char* holderName() {
  TaskHandle_t holder = xSemaphoreGetMutexHolder(g_lock);
  return holder != nullptr ? pcTaskGetName(holder) : "nobody";
}

void reportBusy(const char* file, int line, uint32_t address) {
  const uint32_t fails = ++g_lockFails;
  const uint32_t now = nowMs();
  const uint32_t last = g_lockReportAt.load();
  if (last != 0 && now - last < kLockReportMs) {
    return;
  }
  g_lockReportAt = now | 1U;
  const uint32_t since = g_heldSince.load();
  bringupLog(kTag, "LINK busy: %s (%s:%d, %02X) waited %u ms, fail %u", pcTaskGetName(nullptr),
             baseName(file), line, static_cast<unsigned>(address),
             static_cast<unsigned>(tang::kLockWaitMs), static_cast<unsigned>(fails));
  bringupLog(kTag, "LINK held %u ms by %s from %s:%d (%02X)",
             static_cast<unsigned>(since != 0 ? now - since : 0), holderName(), baseName(g_heldFile.load()),
             g_heldLine.load(), static_cast<unsigned>(g_heldAddress.load()));
}

bool takeLock(const char* file, int line, uint32_t address) {
  if (t_failedGuards > 0) {
    return false;
  }
  if (xSemaphoreTakeRecursive(g_lock, pdMS_TO_TICKS(tang::kLockWaitMs)) != pdTRUE) {
    reportBusy(file, line, address);
    return false;
  }
  if (g_heldDepth++ == 0) {
    g_heldFile = file;
    g_heldLine = line;
    g_heldAddress = address;
    g_heldSince = nowMs() | 1U;
  }
  return true;
}

void giveLock() {
  if (--g_heldDepth == 0) {
    g_heldSince = 0;
  }
  xSemaphoreGiveRecursive(g_lock);
}

bool lockedRead(uint32_t address, uint32_t* words, size_t count, const char* file = __builtin_FILE(),
                int line = __builtin_LINE()) {
  if (!takeLock(file, line, address)) {
    return false;
  }
  const bool ok = link_spi3::transfer(kCmdRead, address, words, count, true, g_dummy);
  giveLock();
  return ok;
}

bool lockedWrite(uint32_t address, const uint32_t* words, size_t count, const char* file = __builtin_FILE(),
                 int line = __builtin_LINE()) {
  uint32_t copy[kMaxWords];
  if (count > kMaxWords) {
    return false;
  }
  memcpy(copy, words, count * sizeof(uint32_t));
  if (!takeLock(file, line, address)) {
    return false;
  }
  const bool ok = link_spi3::transfer(kCmdWrite, address, copy, count, false, 0);
  giveLock();
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

// Рабочая частота — на ступень ниже предельной: в Evo на 40 МГц изредка проскакивала
// ошибка (0.1.70: 1 на ~6000 обменов после 0 из 1000 при выборе), а для записи flash Tang
// нужен запас. Ошибка в итоговой проверке — ещё ступень ниже, а не отказ LINK.
// Возвращает ошибки итоговой проверки (0 — канал готов); *fast — частота выше kFindHz.
int chooseSpeed(uint32_t version, bool* fast) {
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
  *fast = index >= 0;
  return errors;
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
             "SD read %u, OSD %u, aspect %s, size %s, frame blend %s, H %d V %d",
             kSources[ctrl & 3], static_cast<unsigned>((ctrl >> 2) & 1),
             static_cast<unsigned>(((ctrl >> 3) & 3) + 1), static_cast<unsigned>((ctrl >> 10) & 0xF),
             static_cast<unsigned>((ctrl >> 5) & 1), static_cast<unsigned>((ctrl >> 6) & 3),
             static_cast<unsigned>((ctrl >> 8) & 1), ((ctrl >> 14) & 1) ? "16:9" : "4:3",
             ((ctrl >> 15) & 3) == 1 ? "95%" : ((ctrl >> 15) & 3) == 2 ? "90%" : "100%",
             ((ctrl >> 17) & 1) ? "on" : "off",
             static_cast<int>(static_cast<int8_t>(r[9] & 0xFF)),
             static_cast<int>(static_cast<int8_t>((r[9] >> 8) & 0xFF)));
}

// Ядро без FT812 (TS-Config, 1.12+): разрешить автопереход в слот FT812, только если там
// битстрим — иначе Evo, обратившись к FT812, перезагрузила бы ПЛИС в пустой слот.
// Ядро из кэша без хозяина (tang::claimCore): через kOrphanMs — BOOT, повтор не чаще kOrphanRetryMs.
// Так Evo не остаётся без экрана, если ESP перезапустилась при работающем ZX Next или слой ядра
// выгружен, а его возврат не вышел.
constexpr uint32_t kOrphanRetryMs = 5000;
uint32_t g_orphanSince = 0;
uint32_t g_orphanBootAt = 0;

void orphanWatch(uint32_t now) {
  const uint32_t personality = g_version.load() >> 24;
  if (g_state != State::kReady || personality < 0x03U || g_coreClaimed.load()) {
    g_orphanSince = 0;
    return;
  }
  if (g_orphanSince == 0) {
    g_orphanSince = now | 1U;
    return;
  }
  if (now - g_orphanSince < tang::kOrphanMs ||
      (g_orphanBootAt != 0 && now - g_orphanBootAt < kOrphanRetryMs)) {
    return;
  }
  g_orphanBootAt = now | 1U;
  const uint32_t boot = tang::kBootMagic;
  const bool sent = lockedWrite(tang::kRegBoot, &boot, 1);
  bringupLog(kTag, "LINK: core %02X has no owner (ESP restarted?) — BOOT to slot 0 %s",
             static_cast<unsigned>(personality), sent ? "sent" : "FAILED");
}

void armBoot() {
  uint32_t boot = 0;
  if (!lockedRead(tang::kRegBoot, &boot, 1) || (boot >> 20) != tang::kBootTag ||
      ((boot >> 16) & 3U) != 1U) {
    return;  // нет BOOT или ядро с FT812: переходить некуда
  }
  const unsigned next = (boot >> 8) & 0xFF;
  const bool slot = tang_flash::slotHasBitstream(next * tang::kSlotBytes);
  const bool held = tang_sd::heldByWc();   // карта Tang у WC: из TS-Config (моста к ней) не уходить
  const bool ok = slot && !held;
  const uint32_t word = ok ? tang::kBootArm : tang::kBootDisarm;
  lockedWrite(tang::kRegBoot, &word, 1);
  bringupLog(kTag, "LINK: switch to the FT812 core %s (slot %u %s)", ok ? "armed" : "off", next,
             held ? "kept off: WC holds the Tang SD" : slot ? "holds a bitstream" : "is empty");
}

// После перезагрузки Tang в другое ядро (multiboot: другая трассировка ПЛИС) частота и такты
// разворота, выбранные для прежнего, могут не подойти: поиск на kFindHz и выбор заново. LINK
// занят на всё время — обмены других задач на меняющейся частоте не идут.
bool relink(uint32_t* version) {
  tang::LinkGuard guard;
  if (!guard.ok()) {
    return false;                // LINK занят: частоту под чужим обменом не менять
  }
  const uint32_t hz = link_spi3::clockHz();
  const int dummy = g_dummy;
  link_spi3::setClock(kFindHz);
  g_dummy = kDummyCycles;
  uint32_t idv[2] = {};
  if (!lockedRead(tang::kRegId, idv, 2) || idv[0] != tang::kIdValue) {
    link_spi3::setClock(hz);   // ещё грузится — в следующий раз, а до тех пор частота прежняя
    g_dummy = dummy;
    return false;
  }
  bool fast = false;
  const int errors = chooseSpeed(idv[1], &fast);
  bringupLog(kTag, "LINK re-chosen after a Tang reload: %u kHz, %d turnaround cycles, check errors %d",
             static_cast<unsigned>(link_spi3::clockHz() / 1000), g_dummy, errors);
  *version = idv[1];
  return errors == 0;
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
    // Прошлый запуск LINK не дошёл до конца. С 0.1.79 отметка снимается всегда (до того её
    // снимал только удачный запуск, и плата, у которой запуск однажды оборвался, так и жила без
    // LINK): после падения ESP (panic, сторожевые таймеры) LINK пропускается один раз — защита
    // от цикла падений, следующий запуск пробует снова; после включения питания, программного
    // или внешнего сброса — запуск сразу: его оборвал сброс, а не LINK (19.10.2026: Evo
    // выключили на «Tang found, choosing the speed», затем esp_restart на «looking for Tang»).
    const esp_reset_reason_t why = esp_reset_reason();
    const bool crash = why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT ||
                       why == ESP_RST_WDT;
    setGuard(false);
    if (crash) {
      g_state = State::kNone;
      bringupLog(kTag, "LINK skipped once: the previous start of %s did not finish and the ESP "
                 "restarted after a crash (reset reason %d), see the steps above; the next start "
                 "tries again", ZIFI_BUILD_VERSION, static_cast<int>(why));
      vTaskDelete(nullptr);
      return;
    }
    bringupLog(kTag, "LINK: the previous start of %s was cut by a reset (reason %d, not a crash), "
               "starting again", ZIFI_BUILD_VERSION, static_cast<int>(why));
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

  bool fast = false;
  const int errors = chooseSpeed(version, &fast);
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
  // Копия шрифтов ПЗУ FT812 на ESP — пока у Tang мост к flash (TS-Config)
  if ((version >> 24) == tang::kPersonalityTs) {
    ft_rom::refresh();
  }
  // Меню настроек Tang в оверлее: ядро 1.10 и новее
  tang_menu::begin();
  // Multiboot: ядро TS-Config уходит в ядро FT812 по обмену Evo с FT812 — если слот записан
  armBoot();

  // Перезагрузку ПЛИС (multiboot: регистр BOOT, ядро FT812 в слоте 1 и обратно) видно по
  // SCRATCH: после загрузки битстрима он 0, а ESP держит в нём свою метку. Тогда — заново
  // версия, настройки flash и меню (холсты оверлеев в SDRAM пропали). SCRATCH пишут ещё
  // проверка канала сетевой консоли и TANG_WRITE от Evo — это лишь лишний перезапуск.
  uint32_t marker = 0x5C000000U | (esp_random() & 0x00FFFFFFU);
  lockedWrite(tang::kRegScratch, &marker, 1);

  uint32_t last[7] = {0xFFFFFFFF, 0, 0, 0, 0, 0, 0};
  TickType_t statusAt = xTaskGetTickCount();
  int misses = 0;   // проверок подряд без метки и без ответа ID
  for (;;) {
    const TickType_t now = xTaskGetTickCount();
    if (now - statusAt >= pdMS_TO_TICKS(kStatusPeriodMs)) {
      statusAt = now;
      uint32_t mark = 0;
      if (lockedRead(tang::kRegScratch, &mark, 1) && mark != marker) {
        // ПЛИС перезагрузилась или ещё грузится: ждать ID, проверить канал (ядро может быть
        // другое — ядро FT812 после TS-Config), затем начать с её версией
        uint32_t idv[2] = {};
        uint32_t version = 0;
        bool ok = lockedRead(tang::kRegId, idv, 2) && idv[0] == tang::kIdValue &&
                  countErrors(kReloadRounds, idv[1]) == 0;
        if (ok) {
          version = idv[1];
        } else if (++misses >= kReloadMisses) {
          ok = relink(&version);
        }
        if (ok) {
          misses = 0;
          // ядра из кэша больше нет (вернулось ядро из flash Tang): хозяин ему не нужен, а старая
          // отметка не должна заслонить следующее ядро без хозяина
          if ((g_version.load() >> 24) >= 0x03U && (version >> 24) < 0x03U) {
            g_coreClaimed = false;
          }
          g_version = version;
          ++g_reloads;
          bringupLog(kTag, "LINK: Tang reloaded (SCRATCH %08X), core %02X version %u.%u",
                     static_cast<unsigned>(mark), static_cast<unsigned>(version >> 24),
                     static_cast<unsigned>((version >> 8) & 0xFF),
                     static_cast<unsigned>(version & 0xFF));
          tang_flash::begin(version);
          tang_menu::begin();
          armBoot();
          lockedWrite(tang::kRegScratch, &marker, 1);
          last[0] = 0xFFFFFFFF;
          // Ядро FT812 (личность 02): шрифты ПЗУ — из копии ESP в SDRAM (ft_rom); TS-Config —
          // копия обновляется из слота 2 flash Tang (заголовок тот же — не читается)
          if ((version >> 24) == tang::kPersonalityFt) {
            ft_rom::load();
          } else if ((version >> 24) == tang::kPersonalityTs) {
            ft_rom::refresh();
          }
        }
      } else {
        misses = 0;
        if ((g_version.load() >> 24) >= 0x03U) {
          // Ядро из кэша BL616 (ZX Next и другие): регистры у него свои, разбор TS-Config к ним
          // не подходит — в журнал только смена STATUS
          uint32_t status = 0;
          if (lockedRead(tang::kRegStatus, &status, 1) && status != last[0]) {
            last[0] = status;
            bringupLog(kTag, "Tang: core %02X from the BL616 cache, STATUS %08X",
                       static_cast<unsigned>(g_version.load() >> 24), static_cast<unsigned>(status));
          }
        } else {
          uint32_t regs[10] = {};
          if (lockedRead(tang::kRegStatus, regs, 8) && lockedRead(tang::kRegCtrl, regs + 8, 2)) {
            reportStatus(regs, last);
          }
          // Пока ПЛИС грузится, регистры — мусор: настройки во flash не трогать
          tang_flash::poll();
        }
      }
    }
    tang_menu::poll(static_cast<uint32_t>(pdTICKS_TO_MS(now)));
    orphanWatch(static_cast<uint32_t>(pdTICKS_TO_MS(now)));
    vTaskDelay(pdMS_TO_TICKS(kMenuPeriodMs));
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
      // 6 КБ: с 0.1.85 задача сама пишет в LittleFS (копия настроек, шрифты ПЗУ — чтение flash Tang)
      xTaskCreatePinnedToCore(taskMain, "zifi-tang", 6144, nullptr, 1, nullptr, 0) != pdPASS) {
    ESP_LOGE(kTag, "LINK task not started");
  }
}

bool ready() { return g_state == State::kReady; }

void claimCore() { g_coreClaimed = true; }

void releaseCore() { g_coreClaimed = false; }

bool coreClaimed() { return g_coreClaimed.load(); }

bool backToSlot0(uint32_t timeoutMs) {
  if (!ready()) {
    return false;
  }
  if ((coreVersion() >> 24) == kPersonalityTs) {
    return true;                       // уже TS-Config: BOOT увёл бы его в слот FT812
  }
  const uint32_t before = reloads();
  const uint32_t boot = kBootMagic;
  if (!write(kRegBoot, &boot, 1)) {
    return false;
  }
  const TickType_t start = xTaskGetTickCount();
  while (xTaskGetTickCount() - start < pdMS_TO_TICKS(timeoutMs)) {
    if (reloads() != before && (coreVersion() >> 24) == kPersonalityTs) {
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return false;
}

uint32_t coreVersion() { return ready() ? g_version.load() : 0; }

uint32_t reloads() { return g_reloads; }

void disarmBoot() {
  if (ready()) {
    const uint32_t word = kBootDisarm;
    lockedWrite(kRegBoot, &word, 1);
  }
}

void rearmBoot() {
  if (ready()) {
    armBoot();
  }
}

LinkGuard::LinkGuard(const char* file, int line) : taken_(takeLock(file, line, 0xFFFFFFFFU)) {
  if (!taken_) {
    ++t_failedGuards;
  }
}

LinkGuard::~LinkGuard() {
  if (taken_) {
    giveLock();
  } else {
    --t_failedGuards;
  }
}

uint32_t clockHz() { return ready() ? link_spi3::clockHz() : 0; }

bool read(uint32_t address, uint32_t* words, size_t count, const char* file, int line) {
  return ready() && lockedRead(address, words, count, file, line);
}

bool write(uint32_t address, const uint32_t* words, size_t count, const char* file, int line) {
  return ready() && lockedWrite(address, words, count, file, line);
}

bool writeRepeat(uint32_t address, const uint32_t* words, size_t count, const char* file, int line) {
  if (!ready() || !takeLock(file, line, address)) {
    return false;
  }
  const bool ok = link_spi3::writeRepeat(kCmdWrite, address, words, count);
  giveLock();
  return ok;
}

bool writeBurst(uint32_t address, const uint32_t* words, size_t count, const char* file, int line) {
  if (!ready() || !takeLock(file, line, address)) {
    return false;
  }
  const bool ok = link_spi3::writeBurst(kCmdWrite, address, words, count);
  giveLock();
  return ok;
}

void describeLock(char* out, size_t size) {
  const uint32_t since = g_heldSince.load();
  const uint32_t fails = g_lockFails.load();
  if (since == 0) {
    snprintf(out, size, "free; waits failed since start %u", static_cast<unsigned>(fails));
    return;
  }
  snprintf(out, size, "held %u ms by %s from %s:%d (%02X); waits failed since start %u",
           static_cast<unsigned>(nowMs() - since), holderName(), baseName(g_heldFile.load()),
           g_heldLine.load(), static_cast<unsigned>(g_heldAddress.load()), static_cast<unsigned>(fails));
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
