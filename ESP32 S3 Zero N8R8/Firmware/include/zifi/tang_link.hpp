#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zifi/board.hpp"

#if ZIFI_HAS_SPI_LINK

namespace zifi {

// LINK — канал ESP → Tang Nano 20K (QSPI, ESP ведущий; аппаратный SPI3 без драйвера
// ESP-IDF — link_spi3.hpp; частота выбирается при запуске, до 40 МГц).
// Протокол со стороны Tang — Tang Nano 20k/Firmware/rtl/fb_link.sv:
//   команда 8 бит по IO0; адрес 32 бита по IO0…IO3; для чтения 8 тактов разворота;
//   данные по IO0…IO3 словами по 32 бита, старший байт вперёд, адрес +4 за слово.
//   0x01 — запись регистров, 0x02 — чтение.
// Регистры кадрового буфера (ts_fb_core.sv): 0x00 ID "VDC4", 0x04 версия, 0x08 состояние,
// 0x0C…0x24 растр, окно, границы картинки, кадры, сверка; 0x3C проверочный; 0x40 CTRL,
// 0x44 ADJUST (поправки окна).
//
// Запуск — только когда Tang под питанием: иначе выходы ESP вели бы обесточенную
// Tang. Питание видно по линии LINK CS (GPIO3, R10 к шине логики платы — плата
// запитана) или по LINK_IRQ_N (GPIO9, подтяжка на модуле Tang — Tang запитана, например
// от USB на столе). Линию CS прошивка проверяет и для определения питания платы
// (board_state.hpp); после запуска LINK она занята, и probeBoardState() отвечает тем,
// что было видно при запуске, не трогая вывод.
namespace tang {

constexpr uint32_t kRegId = 0x00;
constexpr uint32_t kRegVersion = 0x04;
constexpr uint32_t kRegStatus = 0x08;
constexpr uint32_t kRegRaster = 0x0C;
constexpr uint32_t kRegSync = 0x10;
constexpr uint32_t kRegWindow = 0x14;
constexpr uint32_t kRegPicX = 0x18;
constexpr uint32_t kRegPicY = 0x1C;
constexpr uint32_t kRegFrames = 0x20;
constexpr uint32_t kRegCheck = 0x24;
constexpr uint32_t kRegScratch = 0x3C;
constexpr uint32_t kRegCtrl = 0x40;
constexpr uint32_t kRegAdjust = 0x44;
// С ядра 1.1: flash Tang (fb_flash.sv) и поправки окна по растрам
constexpr uint32_t kRegFlashCmd = 0x48;
constexpr uint32_t kRegFlashStatus = 0x4C;
// С ядра 1.3: нажатие для экранного меню Tang (1 следующий пункт, 2 предыдущий, 3 +1, 4 −1,
// 5 показать меню, 6 скрыть) — плагин WC управляет меню с клавиатуры Evo
constexpr uint32_t kRegUiKey = 0x50;
constexpr uint32_t kRegAdjTable = 0x60;    // 8 слов: растры 0…7, {V, H}
constexpr uint32_t kRegFlashBuf = 0x80;    // 16 слов

constexpr uint32_t kIdValue = 0x56444334;  // "VDC4"

// Запустить фоновую задачу: через 10 с после старта (когда сеть и канал Z80 уже работают)
// дождаться питания Tang, найти её по ID на 1 МГц, выбрать самую высокую частоту, на
// которой проходит проверка проверочным регистром, и раз в 2 с записывать в журнал
// наладки изменения состояния Tang. Шаги запуска сразу пишутся
// во flash; если запуск этой версии прошивки однажды не дошёл до конца (флажок в NVS),
// LINK больше не запускается.
void start();

// Канал проверен и работает.
bool ready();
// Частота LINK_SCK, примерно (0 — канал не работает).
uint32_t clockHz();
// VERSION ядра Tang (0x01010101 — 1.1); 0 — канал не работает.
uint32_t coreVersion();

// Несколько обменов подряд без вмешательства других задач (операции с flash Tang):
// пока объект жив, LINK занят этой задачей.
class LinkGuard {
 public:
  LinkGuard();
  ~LinkGuard();
  LinkGuard(const LinkGuard&) = delete;
  LinkGuard& operator=(const LinkGuard&) = delete;
};

// Обмен словами (адрес — байтовый, кратный 4; не больше 16 слов). Только после ready().
// Безопасно из любых задач: вызовы упорядочены мьютексом.
bool read(uint32_t address, uint32_t* words, size_t count);
bool write(uint32_t address, const uint32_t* words, size_t count);

// Короткое описание для SYS_INFO: "VDC4/1.0@40MHz", "none" или "off".
void describe(char* out, size_t size);

}  // namespace tang

// Для board_state.cpp: линия LINK CS занята каналом; была ли при его запуске запитана
// плата (на столе LINK работает от питания одной Tang — плата остаётся обесточенной).
bool tangLinkOwnsCs();
bool tangLinkBoardPowered();

}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
