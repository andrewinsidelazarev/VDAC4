#include "zifi/ft_host.hpp"

#if ZIFI_HAS_SPI_LINK

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "zifi/tang_flash.hpp"
#include "zifi/tang_link.hpp"
#include "zifi/tang_sd.hpp"

namespace zifi {
namespace ft_host {

namespace {

constexpr char kTag[] = "zifi-fthost";
constexpr uint32_t kRegFtHost = 0xE8;          // команда хоста (ядро 1.12)
constexpr uint32_t kBusyBit = 1U << 31;        // FT_STAT: окно занято
constexpr uint32_t kLostBit = 1U << 14;        // FT_STAT (ядро 1.25): слово пришло в занятое окно
constexpr uint32_t kBusyWaitUs = 2000;

uint32_t nowUs() { return static_cast<uint32_t>(esp_timer_get_time()); }

// Окно освободилось (предыдущее слово дошло до памяти FT812); status — последний FT_STAT.
bool waitIdle(uint32_t* status = nullptr) {
  const uint32_t start = nowUs();
  for (;;) {
    uint32_t st = 0;
    if (!tang::read(tang::kRegFtStat, &st, 1)) {
      return false;
    }
    if ((st & kBusyBit) == 0) {
      if (status != nullptr) {
        *status = st;
      }
      return true;
    }
    if (nowUs() - start > kBusyWaitUs) {
      return false;
    }
  }
}

// FT_ADDR: адрес слова; запись запускает его чтение — дождаться. FT_ADDR в занятое окно ядро
// отбрасывает (FT_STAT 14, ядро 1.25+): с 1.29 окно занято и пока очередь записи не дописана —
// например, после быстрой записи, оборванной по сроку. Поэтому окно ждётся и до FT_ADDR, а
// после — проверяется, принят ли он (принятый FT_ADDR бит 14 снимает)
bool setAddress(uint32_t address) {
  const uint32_t a = address & 0x3FFFFCU;
  uint32_t st = 0;
  return waitIdle() && tang::write(tang::kRegFtAddr, &a, 1) && waitIdle(&st) &&
         (st & kLostBit) == 0;
}

// Ядро FT812 1.29+: слова записи окна идут через очередь на 512 слов (FT_BURST, FT_FREE)
bool coreHasQueue() {
  const uint32_t version = tang::coreVersion();
  return (version >> 24) == 0x02 && (version & 0xFFFF) >= tang::kFtBurstCore;
}

uint32_t fromGray(uint32_t gray) {
  for (uint32_t shift = gray >> 1; shift != 0; shift >>= 1) {
    gray ^= shift;
  }
  return gray;
}

// Очередь окна (ядро 1.29): слова — транзакциями по 16 в окно FT_BURST, не больше свободных мест
// очереди. FT_FREE (код Грея: чтение посреди смены не рвёт число) перечитывается, когда запаса
// меньше 16 слов; места освобождаются, пока ESP пишет (очередь уходит в память FT812 быстрее,
// чем приходит по LINK). Места не освободились за kBusyWaitUs — ошибка (writeBlock повторит
// блок медленно).
bool writeQueued(const uint32_t* words, size_t count) {
  size_t credit = 0;
  uint32_t waitStart = 0;
  bool waiting = false;
  for (size_t at = 0; at < count;) {
    const size_t need = count - at < 16 ? count - at : 16;
    if (credit < need) {
      uint32_t gray = 0;
      if (!tang::read(tang::kRegFtFree, &gray, 1)) {
        return false;
      }
      credit = fromGray(gray & 0x3FFU);
      if (credit > tang::kFtQueueWords) {
        credit = 0;
      }
      if (credit < need) {
        if (!waiting) {
          waiting = true;
          waitStart = nowUs();
        } else if (nowUs() - waitStart > kBusyWaitUs) {
          return false;
        }
        continue;
      }
      waiting = false;
    }
    const size_t n = count - at < credit ? count - at : credit;
    if (!tang::writeBurst(tang::kRegFtBurst, words + at, n)) {
      return false;
    }
    credit -= n;
    at += n;
  }
  return true;
}

bool writeWords(uint32_t address, const uint32_t* words, size_t count, bool paced) {
  // Окно FT_ADDR/FT_DATA одно: адрес и слова — без чужих обменов (приложение ESP и заливка
  // шрифтов ПЗУ из задачи LINK — разные задачи)
  tang::LinkGuard guard;
  if (!setAddress(address)) {
    return false;
  }
  if (!paced) {
    // Быстро. Ядро 1.29+: пакетами по 16 слов в очередь окна (~0,4 мкс на слово). Старые ядра:
    // слова в FT_DATA подряд, контроллер настроен один раз — на слово только данные, старт и CS
    // (~3 мкс на слово; по tang::write на слово выходило ~9 мкс). Слово, пришедшее в занятое
    // окно (старые ядра) или в полную очередь, теряется: ядро 1.25+ отмечает это в FT_STAT 14
    // (до следующего FT_ADDR) — тогда блок повторяется медленно (writeBlock)
    const bool sent = coreHasQueue() ? writeQueued(words, count)
                                     : tang::writeRepeat(tang::kRegFtData, words, count);
    uint32_t st = 0;
    return sent && waitIdle(&st) && (st & kLostBit) == 0;
  }
  for (size_t i = 0; i < count; ++i) {
    if (!tang::write(tang::kRegFtData, &words[i], 1)) {
      return false;
    }
    // Слово, пришедшее в занятое окно, теряется: в медленном режиме — ждать каждое и смотреть,
    // не потеряно ли оно всё-таки (FT_STAT 14, ядро 1.25+; у старых ядер бит — 0). С 0.1.81:
    // раньше медленный повтор считался удачным без проверки (разбор Codex 20.10.2026)
    uint32_t st = 0;
    if (!waitIdle(&st) || (st & kLostBit) != 0) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool coreHasFt() {
  if (!tang::ready()) {
    return false;
  }
  uint32_t boot = 0;
  if (!tang::read(tang::kRegBoot, &boot, 1) || (boot >> 20) != tang::kBootTag) {
    return true;   // ядра до 1.11 — общие, FT812 в них есть
  }
  return ((boot >> 17) & 1U) != 0;
}

bool switchToFtCore(uint32_t timeoutMs) {
  if (coreHasFt()) {
    return true;
  }
  if (tang_sd::heldByWc()) {
    ESP_LOGW(kTag, "%s", tang_sd::kHeldByWc);   // у ядра FT812 нет моста к карте Tang
    return false;
  }
  uint32_t boot = 0;
  if (!tang::read(tang::kRegBoot, &boot, 1)) {
    return false;
  }
  const uint32_t next = (boot >> 8) & 0xFF;
  if (!tang_flash::slotHasBitstream(next * tang::kSlotBytes)) {
    ESP_LOGW(kTag, "slot %u holds no bitstream: no FT812 core to switch to", static_cast<unsigned>(next));
    return false;
  }
  const uint32_t reloads = tang::reloads();
  const uint32_t magic = tang::kBootMagic;
  if (!tang::write(tang::kRegBoot, &magic, 1)) {
    return false;
  }
  // Перезагрузку и новое ядро замечает задача LINK (SCRATCH, раз в 2 с)
  for (uint32_t waited = 0; waited < timeoutMs; waited += 100) {
    vTaskDelay(pdMS_TO_TICKS(100));
    if (tang::reloads() != reloads && tang::ready() && coreHasFt()) {
      ESP_LOGI(kTag, "Tang switched to the FT812 core (VERSION %08X)",
               static_cast<unsigned>(tang::coreVersion()));
      return true;
    }
  }
  return false;
}

namespace {

// FT_CFG бит 3 (ядро 1.18): окно пишет в образ ПЗУ — его FT_STAT не показывает, помним сами
bool g_romWrite = false;

// FT_CFG: хост и запись ПЗУ — как заданы, прочие биты — как были: RAM_G 3 МБ (FT_STAT 27),
// без дизеринга (13)
bool writeCfg(bool esp) {
  tang::LinkGuard guard;
  uint32_t st = 0;
  if (!tang::read(tang::kRegFtStat, &st, 1)) {
    return false;
  }
  const uint32_t cfg = ((st >> 27) & 1U) | (((st >> 13) & 1U) << 1) | (esp ? tang::kFtCfgEspHost : 0U) |
                       (g_romWrite ? tang::kFtCfgRomWrite : 0U);
  return tang::write(tang::kRegFtStat, &cfg, 1);
}

}  // namespace

bool setHost(bool esp) { return writeCfg(esp); }

bool setRomWrite(bool on) {
  tang::LinkGuard guard;
  uint32_t st = 0;
  if (!tang::read(tang::kRegFtStat, &st, 1)) {
    return false;
  }
  g_romWrite = on;
  return writeCfg(((st >> 12) & 1U) != 0);   // хост — как был (FT_STAT 12)
}

bool hostCommand(uint8_t command, uint8_t parameter) {
  const uint32_t word = command | (static_cast<uint32_t>(parameter) << 8);
  return tang::write(kRegFtHost, &word, 1);
}

bool write32(uint32_t address, uint32_t value) { return writeWords(address, &value, 1, true); }

bool read32(uint32_t address, uint32_t* value) {
  tang::LinkGuard guard;
  return setAddress(address) && tang::read(tang::kRegFtData, value, 1);
}

bool readWords(uint32_t address, uint32_t* words, size_t count) {
  tang::LinkGuard guard;
  for (size_t i = 0; i < count; ++i) {
    // FT_DATA при чтении адрес не двигает: адрес — на каждое слово
    if (!setAddress(address + 4U * i) || !tang::read(tang::kRegFtData, &words[i], 1)) {
      return false;
    }
  }
  return true;
}

bool writeBlock(uint32_t address, const uint32_t* words, size_t count) {
  if (count == 0) {
    return true;
  }
  tang::LinkGuard guard;   // запись, проверка и повтор — одним куском окна
  // Регистры (0x302000…) — по слову с ожиданием: у записи бывает действие (REG_DLSWAP,
  // REG_CPURESET, REG_CMD_WRITE), и проверка чтением с повтором записала бы его дважды —
  // например, смена листа, прошедшая между записью и чтением, повторилась бы
  if (address >= 0x302000U && address < 0x303000U) {
    return writeWords(address, words, count, true);
  }
  // RAM_DL — без проверки чтением: смена листа, ждущая с прошлого кадра, может пройти между
  // записью и чтением и переключить банк — чтение увидит другой лист (так ловилось «lost words»
  // по 0x300000 у экрана загрузки DOOM). Потерю слова и здесь показывает FT_STAT 14.
  const bool dl = address >= 0x300000U && address < 0x302000U;
  auto lastOk = [&]() {
    uint32_t back = 0;
    return dl || (read32(address + 4U * (count - 1), &back) && back == words[count - 1]);
  };
  // Быстро — подряд; проверка — последнее слово (потерянное слово сдвинуло бы хвост)
  if (writeWords(address, words, count, false) && lastOk()) {
    return true;
  }
  ESP_LOGW(kTag, "block at %06X: lost words, writing paced", static_cast<unsigned>(address));
  // медленно — с проверкой каждого слова и последнего чтением; не сошлось — ошибка, а не тихая
  // порча (электрические сбои LINK или памяти)
  if (writeWords(address, words, count, true) && lastOk()) {
    return true;
  }
  ESP_LOGE(kTag, "block at %06X: paced write failed the check", static_cast<unsigned>(address));
  return false;
}

bool writeBytes(uint32_t address, const uint8_t* bytes, size_t length) {
  uint32_t chunk[64];
  for (size_t done = 0; done < length;) {
    size_t n = (length - done) / 4;
    if (n > 64) {
      n = 64;
    }
    if (n == 0) {
      return false;   // длина не кратна 4
    }
    for (size_t i = 0; i < n; ++i) {
      const uint8_t* b = bytes + done + 4 * i;
      chunk[i] = b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<uint32_t>(b[3]) << 24);
    }
    if (!writeBlock(address + done, chunk, n)) {
      return false;
    }
    done += 4 * n;
  }
  return true;
}

bool boot1024x768() {
  // Как FT_Boot заставки Weather 2 (VDAC2): внешний клок ×8 = 64 МГц
  if (!hostCommand(kHostCoreReset) || !hostCommand(kHostClkExt) || !hostCommand(kHostSleep) ||
      !hostCommand(kHostClkSel, 8 | 0xC0) || !hostCommand(kHostActive)) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(20));
  uint32_t id = 0;
  for (int i = 0; i < 50 && (id & 0xFF) != 0x7C; ++i) {
    vTaskDelay(pdMS_TO_TICKS(10));
    if (!read32(kRegId, &id)) {
      id = 0;
    }
  }
  if ((id & 0xFF) != 0x7C) {
    ESP_LOGW(kTag, "FT812 does not answer: REG_ID %08X", static_cast<unsigned>(id));
    return false;
  }
  // Тайминги 1024×768@59: строка 24 + 136 + 160 + 1024, кадр 3 + 6 + 29 + 768
  struct Reg {
    uint32_t address, value;
  };
  static const Reg kMode[] = {
      {kRegPclk, 0},        {kRegHSync0, 24},    {kRegHSync1, 24 + 136}, {kRegHOffset, 24 + 136 + 160},
      {kRegHSize, 1024},    {kRegHCycle, 1344},  {kRegVSync0, 2},        {kRegVSync1, 8},
      {kRegVOffset, 37},    {kRegVSize, 768},    {kRegVCycle, 806},      {kRegPclkPol, 0},
      {kRegCSpread, 0},     {kRegPclk, 1},
  };
  for (const Reg& r : kMode) {
    if (!write32(r.address, r.value)) {
      return false;
    }
  }
  return true;
}

}  // namespace ft_host
}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
