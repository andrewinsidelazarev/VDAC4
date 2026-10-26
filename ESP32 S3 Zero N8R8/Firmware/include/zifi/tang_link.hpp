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
// С ядра 1.4: окно в память FT812 для ESP-хоста (правила хоста FT812): FT_ADDR — адрес слова
// (запись запускает чтение), FT_DATA — слово; FT_STAT (чтение) / FT_CFG (запись) — 0x5C.
// С ядра 1.12 FT_CFG бит 2 (kFtCfgEspHost): хост FT812 — ESP, SPI Evo к FT812 отключён;
// FT_STAT 27 — RAM_G 3 МБ, 13 — без дизеринга, 12 — хост — ESP.
constexpr uint32_t kRegFtAddr = 0x54;
constexpr uint32_t kRegFtData = 0x58;
constexpr uint32_t kRegFtStat = 0x5C;
constexpr uint32_t kFtCfgEspHost = 1U << 2;
// С ядра 1.18 FT_CFG бит 3: окно FT_ADDR/FT_DATA пишет в образ ПЗУ FT812 (шрифты, ft_rom)
constexpr uint32_t kFtCfgRomWrite = 1U << 3;
// С ядра FT812 1.29: слова записи окна (FT_DATA и FT_BURST) идут через очередь на 512 слов.
// FT_BURST — окно LINK 0x40000…0x7FFFF: слова в очередь транзакцией любой длины (адрес внутри
// окна не важен); FT_FREE — свободных мест очереди, код Грея ([9:0])
constexpr uint32_t kRegFtFree = 0x38;
constexpr uint32_t kRegFtBurst = 0x40000;
constexpr uint32_t kFtQueueWords = 512;
constexpr uint32_t kFtBurstCore = 0x011D;   // VERSION[15:0] ядра FT812 с очередью окна
constexpr uint32_t kRegAdjTable = 0x60;    // 8 слов: растры 0…7, {V, H}
constexpr uint32_t kRegFlashBuf = 0x80;    // 16 слов
// С ядра 1.12: multiboot. Чтение — [31:20] kBootTag, 18 — автопереход разрешён, 17 — в ядре
// FT812, 16 — захват Evo, [15:8] слот перехода, [7:0] свой слот (слоты flash Tang по 1 МБ,
// слот 0 — TS-Config). Запись kBootMagic — ПЛИС перезагружается в слот перехода;
// kBootArm / kBootDisarm — ядро без FT812 само уходит в слот FT по первому обмену Evo с FT812.
constexpr uint32_t kRegBoot = 0xF0;
constexpr uint32_t kBootTag = 0xB00;
constexpr uint32_t kBootMagic = 0x424F4F54;   // "BOOT"
constexpr uint32_t kBootArm = 0x41524D31;     // "ARM1"
constexpr uint32_t kBootDisarm = 0x41524D30;  // "ARM0"
constexpr uint32_t kSlotBytes = 0x100000;

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
// Сколько раз ESP видела перезагрузку ПЛИС Tang (multiboot, RECONFIG_N) после запуска LINK.
uint32_t reloads();
// Автопереход ядра TS-Config в слот FT812: запретить (пока слот переписывается) и заново
// разрешить, если в слоте битстрим (после записи; само — при запуске LINK и перезагрузках).
void disarmBoot();
void rearmBoot();

// LINK занят задачей не дольше этого для остальных: кто ждёт дольше — отказ обмена (false) и
// запись в журнал, кто держит LINK (задача, где взяла, сколько держит).
constexpr uint32_t kLockWaitMs = 5000;

// Несколько обменов подряд без вмешательства других задач (операции с flash Tang):
// пока объект жив, LINK занят этой задачей. LINK не дождалась (kLockWaitMs) — ok() = false,
// и обмены этой задачи, пока объект жив, сразу отказывают. Место вызова (file, line) — для
// журнала: так видно, где взят LINK, который не отпускают.
class LinkGuard {
 public:
  explicit LinkGuard(const char* file = __builtin_FILE(), int line = __builtin_LINE());
  ~LinkGuard();
  LinkGuard(const LinkGuard&) = delete;
  LinkGuard& operator=(const LinkGuard&) = delete;
  bool ok() const { return taken_; }

 private:
  bool taken_;
};

// Обмен словами (адрес — байтовый, кратный 4; не больше 16 слов). Только после ready().
// Безопасно из любых задач: вызовы упорядочены мьютексом. Место вызова — для журнала (LinkGuard).
bool read(uint32_t address, uint32_t* words, size_t count, const char* file = __builtin_FILE(),
          int line = __builtin_LINE());
bool write(uint32_t address, const uint32_t* words, size_t count, const char* file = __builtin_FILE(),
           int line = __builtin_LINE());
// Слова подряд в один регистр (окно FT_DATA): по слову за транзакцию, слов — сколько угодно.
bool writeRepeat(uint32_t address, const uint32_t* words, size_t count, const char* file = __builtin_FILE(),
                 int line = __builtin_LINE());
// Слова транзакциями по 16 на один адрес (окно FT_BURST ядра FT812 1.29): слов — сколько угодно.
bool writeBurst(uint32_t address, const uint32_t* words, size_t count, const char* file = __builtin_FILE(),
                int line = __builtin_LINE());

// Короткое описание для SYS_INFO: "VDC4/1.0@40MHz", "none" или "off".
void describe(char* out, size_t size);
// Кто держит LINK: "free" или "held N ms by TASK from FILE:LINE (ADDR)"; и сколько ожиданий
// LINK кончились отказом с запуска.
void describeLock(char* out, size_t size);

// Ядро из кэша BL616 (личность VERSION[31:24] 3 и выше: ZX Next и другие) и его хозяин — тот, кто
// его загрузил и вернёт Tang в слот 0: приложение ESP (ModuleApp, слой ядра) или консоль. Хозяина
// нет (ESP перезапустилась при работающем ZX Next; хозяин выгружен, а возврат не вышел) — задача
// LINK через kOrphanMs пишет ядру BOOT, и BL616 возвращает слот 0 (разбор Codex 21.10.2026, № 1).
constexpr uint32_t kOrphanMs = 3000;
constexpr uint32_t kPersonalityTs = 0x01;     // TS-Config (слот 0)
constexpr uint32_t kPersonalityFt = 0x02;     // FT812 (слот 1)
void claimCore();                             // до загрузки ядра: оно будет чьё-то
void releaseCore();                           // хозяин ушёл (или загрузка не вышла)
bool coreClaimed();

// Назад в слот 0: Tang уже в TS-Config — сразу true; иначе запись BOOT текущему ядру (FT812 или
// ядро из кэша) и ожидание TS-Config на LINK до timeoutMs.
bool backToSlot0(uint32_t timeoutMs);

}  // namespace tang

// Для board_state.cpp: линия LINK CS занята каналом; была ли при его запуске запитана
// плата (на столе LINK работает от питания одной Tang — плата остаётся обесточенной).
bool tangLinkOwnsCs();
bool tangLinkBoardPowered();

}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
