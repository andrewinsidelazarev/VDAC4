#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zifi/board.hpp"

#if ZIFI_HAS_SPI_LINK

namespace zifi {

// Оверлеи Tang (ядро 1.10+, fpga/vdac2/rtl/fb_ovl.sv): два прямоугольника с картинкой ESP
// поверх выхода HDMI — меню настроек Tang (tang_menu), обложка в плеере, спектр. Оверлей 1
// лежит поверх оверлея 0. Холсты — в регионе 512 КБ SDRAM Tang (131 072 слова): точка
// RGB565, в слове две ([15:0] — левая), строка холста — strideWords(w) слов. ESP пишет
// регион окном LINK 0x80000…0xFFFFF; настройки оверлея Tang берёт в начале кадра вывода.
// Чтение SDRAM на строку вывода: оба оверлея вместе разумно держать в ~640 точках холста
// при ×1 (×2 читает строку холста раз на две строки вывода).
namespace tang_overlay {

constexpr uint32_t kRegCtrl = 0xC0;       // оверлей k — kRegCtrl + 16·k: CTRL, POS, SIZE, BASE
constexpr uint32_t kRegStatus = 0xE4;     // 0 — очередь записи переполнялась
constexpr uint32_t kMemBase = 0x80000;    // адрес LINK слова 0 региона
constexpr uint32_t kRegionWords = 131072;
constexpr uint16_t kMaxWidth = 1024;

struct Config {
  bool on = false;
  bool x2 = false;          // точка холста — 2×2 точки вывода
  uint8_t alpha = 15;       // 0…15: вывод = картинка + ⌊(оверлей − картинка)·(alpha + 1)/16⌋
  uint16_t x = 0, y = 0;    // левый верхний угол на выводе
  uint16_t w = 0, h = 0;    // размер холста, w ≤ 1024
  uint32_t base = 0;        // начало холста — слово региона (кратно 8)
};

// Шаг строки холста: ширина, округлённая до 16 точек (8 слов).
constexpr uint32_t strideWords(uint16_t w) { return ((w + 15U) / 16U) * 8U; }

// Ядро Tang с оверлеями (VERSION ≥ 1.10) и канал работает.
bool supported();
// Настройки оверлея k (0, 1).
bool configure(int k, const Config& config);
bool hide(int k);
// Строки холста [row0, row0 + rows): точки pixels (rows × 2·stride, по строкам) — в регион
// с base + row·stride слов. Пишется кусками по 16 слов.
bool writeRows(uint32_t base, uint32_t stride, uint32_t row0, uint32_t rows,
               const uint16_t* pixels);

}  // namespace tang_overlay
}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
