#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zifi/board.hpp"

#if ZIFI_HAS_SPI_LINK

namespace zifi {
namespace tang_flash {

// SPI-flash Tang Nano 20K (XT25F64B, 8 МБ) через мост ядра Tang (fb_flash.sv, ядро 1.1+):
// буфер 64 байта и команда «передать N байтов, принять M, держать CS». Протокол flash —
// здесь. Все функции держат LINK на время операции (tang::LinkGuard).
//
// Раскладка flash: 0x000000 — прошивка Tang (упакованный .fs, ~0,9 МБ); 0x7FF000 — сектор
// настроек кадрового буфера: 128 ячеек по 32 байта, формат — в fb_flash.sv. Ядро само
// читает настройки после сброса; пишет их ESP (poll()).

constexpr uint32_t kSettingsAddr = 0x7FF000;
constexpr uint32_t kSettingsSlots = 128;
constexpr uint32_t kSettingsBlock = 0x7F0000;   // блок 64 КБ с настройками: прошивка до него
constexpr uint32_t kSectorBytes = 4096;
constexpr uint32_t kBlockBytes = 65536;
constexpr uint32_t kPageBytes = 256;

// Мост есть (ядро 1.1+) и flash отвечает.
bool available();
uint32_t jedecId();

bool read(uint32_t address, uint8_t* data, size_t length);
// Стирание: сектор 4 КБ / блок 64 КБ (адрес выравнивается вниз).
bool eraseSector(uint32_t address);
bool eraseBlock(uint32_t address);
// Запись (только 1 → 0: область стирается заранее); страницы 256 байт разбиваются сами.
bool program(uint32_t address, const uint8_t* data, size_t length);

// После готовности LINK: найти последние сохранённые настройки и первую чистую ячейку.
void begin(uint32_t coreVersion);
// Раз в период опроса Tang: если настройки (CTRL и поправки растров) изменились и держатся
// с прошлого опроса — записать их в следующую ячейку.
void poll();
// Для консоли: состояние хранения настроек.
void describe(char* out, size_t size);

}  // namespace tang_flash
}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
