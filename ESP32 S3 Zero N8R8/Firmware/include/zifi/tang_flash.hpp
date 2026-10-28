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
// С address начинается битстрим GW2AR-18 (слово синхронизации A5C3 и проверка IDCODE
// 0000081B в первых 64 байтах) — слот multiboot не пуст.
bool slotHasBitstream(uint32_t address);

// После готовности LINK: найти последние сохранённые настройки и первую чистую ячейку. У ядра
// FT812 1.31+ моста нет: тогда ESP сама пишет ему в CTRL ASPECT и SIZE — последние, что видела в
// TS-Config (или их копию в своей LittleFS, если запустилась, когда Tang уже был в FT812).
void begin(uint32_t coreVersion);
// Раз в период опроса Tang: если настройки (CTRL и поправки растров) изменились и держатся
// с прошлого опроса — записать их в следующую ячейку.
void poll();
// Перед тем как ESP заменит TS-Config другим ядром (слот flash Tang, кэш BL616): изменённые и
// ещё не записанные настройки — во flash сразу. poll() пишет изменение, только когда оно
// продержалось до следующего опроса (2–4 с); сменённое перед самым запуском ядра пропадало —
// новое ядро и TS-Config после возврата читали из flash прежнее (27.10.2026: ASPECT 4:3 за 1,5 с
// до ролика — ядро видео шло в 16:9, и после выхода TS-Config снова был 16:9).
void flush();
// Для консоли: состояние хранения настроек.
void describe(char* out, size_t size);

// FT SIZE (с 0.1.104): размер картинки ядра FT812 — 0 — 100 %, 1 — 95 %, 2 — 90 %. Ядро FT812 с
// 1.41 выводит 1366×768, и телевизор, обрезающий края 720p (SIZE слота 0), его кадр показывает
// целиком: размеры у них свои. Хранится в слове 6 ячейки настроек (ядра Tang его только
// суммируют), ядру FT812 пишется вместо SIZE слота 0. Меняет его меню (tang_menu), во flash его
// пишет poll() вместе с настройками TS-Config. Только из задачи Tang (tang_link).
uint32_t ftSize();
void setFtSize(uint32_t size);

}  // namespace tang_flash
}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
