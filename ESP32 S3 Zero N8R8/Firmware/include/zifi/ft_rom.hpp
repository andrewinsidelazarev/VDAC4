#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zifi/board.hpp"

#if ZIFI_HAS_SPI_LINK

namespace zifi {

// Шрифты ПЗУ FT812 для ядра FT812 Tang (1.18+). Образ ПЗУ — данные Bridgetek: в репозитории его
// нет, его снимают с bt8xxemu на ПК (fpga/vdac2/scripts/ft_rom_fonts.py) и пишут во flash Tang в
// слот 2 (tang_tool.py update --slot 2). У ядра FT812 1.31+ моста к flash нет, поэтому у ESP своя
// копия образа в PSRAM: её берёт refresh() из flash Tang, пока Tang в TS-Config (ESP запустилась
// уже в FT812 1.31+ — шрифтов нет до следующего TS-Config). После каждой загрузки ядра FT812 ESP переписывает куски копии в SDRAM — в адреса ПЗУ
// FT812 (окно FT_ADDR/FT_DATA, FT_CFG бит 3); ROM_FONTROOT (0x2FFFFC) — последним: по нему
// сопроцессор BL616 ждёт шрифты в CMD_TEXT.
namespace ft_rom {

constexpr uint32_t kFlashBase = 0x200000;      // слот 2 flash Tang
constexpr char kMagic[8] = {'V', 'D', '4', 'F', 'O', 'N', 'T', '1'};
constexpr size_t kMaxSpans = 8;
constexpr uint32_t kFontRoot = 0x2FFFFC;

// Образ во flash: «VD4FONT1», число кусков (LE32), затем куски {адрес FT812, длина, смещение
// в образе} (LE32, кратны 4), затем их данные. Проверка заголовка — для службы обновления.
const char* checkImage(const uint8_t* image, size_t size);

// Копия из flash Tang (нужен мост: ядро TS-Config). Заголовок тот же, что у копии, — не читается;
// false — моста или образа нет.
bool refresh();
// Образ, только что записанный в слот 2 службой обновления, — сразу в копию.
void adopt(const uint8_t* image, size_t size);
// Переписать шрифты из копии в SDRAM ядра FT812 (копии нет — у ядра с мостом из flash Tang).
// false — образа нет или обмен сорвался.
bool load();

}  // namespace ft_rom
}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
