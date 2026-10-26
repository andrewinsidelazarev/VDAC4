#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zifi/board.hpp"

#if ZIFI_HAS_SPI_LINK

namespace zifi {

// ESP — хост-машина FT812 эмулятора Tang (как Z80 на VDAC2): память и регистры FT812 —
// окном LINK FT_ADDR/FT_DATA (правила хоста те же, что у SPI), команды хоста — регистром
// FT_HOST, SPI Evo к FT812 отключается битом 2 FT_CFG. Нужно ядро Tang 1.12 с FT812 (общее
// или ядро FT812 слота 1); ядро TS-Config без FT812 переводится в слот FT812 (BOOT).
namespace ft_host {

// Адреса FT81x
constexpr uint32_t kRamG = 0x000000;
constexpr uint32_t kRamDl = 0x300000;
constexpr uint32_t kRegId = 0x302000;
constexpr uint32_t kRegFrames = 0x302004;
constexpr uint32_t kRegHCycle = 0x30202C;
constexpr uint32_t kRegHOffset = 0x302030;
constexpr uint32_t kRegHSize = 0x302034;
constexpr uint32_t kRegHSync0 = 0x302038;
constexpr uint32_t kRegHSync1 = 0x30203C;
constexpr uint32_t kRegVCycle = 0x302040;
constexpr uint32_t kRegVOffset = 0x302044;
constexpr uint32_t kRegVSize = 0x302048;
constexpr uint32_t kRegVSync0 = 0x30204C;
constexpr uint32_t kRegVSync1 = 0x302050;
constexpr uint32_t kRegDlSwap = 0x302054;
constexpr uint32_t kRegCSpread = 0x302068;
constexpr uint32_t kRegPclkPol = 0x30206C;
constexpr uint32_t kRegPclk = 0x302070;

// Команды хоста
constexpr uint8_t kHostActive = 0x00;
constexpr uint8_t kHostSleep = 0x42;
constexpr uint8_t kHostClkExt = 0x44;
constexpr uint8_t kHostClkSel = 0x61;
constexpr uint8_t kHostCoreReset = 0x68;

// В загруженном ядре Tang есть FT812 (ядро 1.12: бит BOOT; ядра до 1.11 — общие).
bool coreHasFt();
// Ядро без FT812 — перезагрузить ПЛИС в слот FT812 и дождаться его (до timeoutMs).
bool switchToFtCore(uint32_t timeoutMs);
// Хост FT812 — ESP (true) или Evo (false): FT_CFG бит 2, прочие биты как были.
bool setHost(bool esp);
// Окно пишет в образ ПЗУ FT812 (FT_CFG бит 3, ядро 1.18): так заливаются шрифты ПЗУ (ft_rom).
bool setRomWrite(bool on);
bool hostCommand(uint8_t command, uint8_t parameter = 0);
// Слова памяти FT812 (адрес кратен 4). Блок — подряд с address.
bool write32(uint32_t address, uint32_t value);
bool writeBlock(uint32_t address, const uint32_t* words, size_t count);
bool writeBytes(uint32_t address, const uint8_t* bytes, size_t length);   // длина кратна 4
bool read32(uint32_t address, uint32_t* value);
// Слова подряд с address — одним куском окна (захват кадра: tang_tool.py ftdump). Чтение окном
// побочных действий не имеет (REG_INT_FLAGS не сбрасывается — это делает только чтение хоста).
bool readWords(uint32_t address, uint32_t* words, size_t count);
// Запуск FT812 в 1024×768@59 (64 МГц, как Weather 2 и игры VDAC2): CORERST, CLKEXT, SLEEP,
// CLKSEL ×8, ACTIVE, ожидание REG_ID = 0x7C, тайминги, REG_PCLK = 1.
bool boot1024x768();

}  // namespace ft_host
}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
