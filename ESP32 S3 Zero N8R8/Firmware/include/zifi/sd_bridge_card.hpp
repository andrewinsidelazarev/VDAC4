#pragma once

#include <stddef.h>
#include <stdint.h>

namespace zifi {

// microSD самой Tang Nano 20K через мост ядра TS-Config 1.30 (fpga/vdac2/rtl/fb_sdspi.sv):
// карта в режиме SPI, мост делает побайтный обмен, ожидание ответа и приём блока за маркером;
// протокол SD — здесь. Код переносимый: LINK и время — через SdBridgeIo (на ESP — tang::read/
// write; на ПК — модель моста и карты, tests/sd_bridge_host.cpp).
//
// Регистры моста (LINK): 0x100 SD_CTRL, 0x104 SD_PTR, 0x108 SD_CMD / SD_STAT; буферы TX и RX —
// окно 0x2000…0x27FC (запись — TX, чтение — RX), байт 0 — старший байт слова. В ядре видео 1.47
// (там 0x100… и окно 0x2000 — звук) — 0x110…, TX в окне 0x2000 (16 слов: только команды), RX — 0x3000,
// и операция STREAM: блоки CMD18 мимо ESP — в медиа-FIFO ядра (stream). Раскладку выбирает
// вызывающий (setLayout) по ядру на LINK.
//
// Блок — 512 байтов. Чтение — CMD17 на блок, CRC16 сверяется (не сошлось — ещё две попытки),
// запись — CMD24 с CRC16, ответ данных и ожидание конца записи; несколько блоков — серией CMD25
// (0.1.137), при отказе посреди серии остаток — по одному блоку; кадр следующего блока серии
// готовится, пока мост передаёт текущий (0.1.139). SDSC адресуется байтами, SDHC и SDXC — блоками.
struct SdBridgeIo {
  void* ctx;
  bool (*read)(void* ctx, uint32_t address, uint32_t* words, size_t count);         // до 16 слов
  bool (*write)(void* ctx, uint32_t address, const uint32_t* words, size_t count);  // до 16 слов
  uint32_t (*nowMs)(void* ctx);
  // Отдать процессор, пока операция моста ждёт (конец записи блока — до сотен мс): без этого запись
  // файла держит ядро ESP дольше сторожевого таймера задач (5 с). nullptr — не нужно (ПК).
  void (*yield)(void* ctx);
};

// Раскладка моста в ядре: адреса LINK (байтовые)
struct SdBridgeLayout {
  uint32_t ctrl, ptr, cmd;      // SD_CTRL, SD_PTR, SD_CMD / SD_STAT
  uint32_t strm;                // SD_STRM операции STREAM; 0 — её нет
  uint32_t txWindow, rxWindow;  // окно TX (запись) и RX (чтение)
  uint32_t txWords;             // слов TX: меньше 129 — блок не записать (ядро видео)
};
// TS-Config 1.30+, NES 0.7+, Amiga, Mega Drive и прочие ядра с мостом по прежним адресам
extern const SdBridgeLayout kSdLayoutStd;
// Ядро видео 1.47+ (личность 05, HAS_VIDEO): только чтение и STREAM в медиа-FIFO
extern const SdBridgeLayout kSdLayoutVideo;

class SdBridgeCard {
 public:
  static constexpr uint32_t kRegCtrl = 0x100;
  static constexpr uint32_t kRegPtr = 0x104;
  static constexpr uint32_t kRegCmd = 0x108;
  static constexpr uint32_t kWindow = 0x2000;
  static constexpr uint32_t kStatMagic = 0x5D01;
  static constexpr uint32_t kBlock = 512;

  explicit SdBridgeCard(const SdBridgeIo& io) : io_(io) {}

  // Раскладка ядра на LINK (по умолчанию — kSdLayoutStd). Ядро сменилось — снова init().
  void setLayout(const SdBridgeLayout& layout) { layout_ = &layout; }
  const SdBridgeLayout& layout() const { return *layout_; }

  // Мост есть (SD_STAT[31:16] = 0x5D01; в ядре FT812 и до 1.30 его нет).
  bool present();
  // Карта: 80 тактов, CMD0, CMD8, ACMD41, CMD58, CSD — объём; затем быстрый SCK (≤ 25 МГц).
  // nullptr — готова; иначе — что не так.
  const char* init();
  bool ready() const { return ready_; }
  bool highCapacity() const { return sdhc_; }
  uint32_t sectors() const { return sectors_; }
  uint32_t sckKhz() const { return sckKhz_; }

  bool read(uint32_t lba, uint8_t* data, uint32_t count);
  bool write(uint32_t lba, const uint8_t* data, uint32_t count);   // TX мал (ядро видео) — false
  // Поток (ядро видео 1.47, операция STREAM): CMD18 с блока lba, blocks целых блоков (1…4095) — в
  // медиа-FIFO ядра, в конце CMD12. Ход операции опрашивается с yield между опросами: LINK не
  // занят на всё время потока (звук и оверлеи ядра идут). *crcError — в каком-то блоке CRC16 не
  // сошёлся (данные уже в FIFO: повтора нет). false — STREAM нет, карта не ответила или поток
  // оборвался (сколько блоков ушло — не известно: поток начинать заново).
  bool stream(uint32_t lba, uint32_t blocks, bool* crcError);
  // CS неактивен (между сеансами карта не держит шину); открытую серию записи — закрыть
  void release();
  // Закрыть открытую серию записи CMD25 (0.1.138): маркер «стоп» и конец «занята». Серия остаётся
  // открытой между вызовами write(), пока запись идёт подряд; её закрывает и любая другая
  // операция с картой. FatFs зовёт это по CTRL_SYNC (закрытие файла). false — карта не дописала.
  bool flush();
  // Серия записи CMD25 открыта (карта ждёт следующий блок или «стоп»)
  bool writing() const { return seriesOpen_; }

  // Счётчики: повторов чтения по CRC, ошибок
  uint32_t crcRetries() const { return crcRetries_; }
  uint32_t errors() const { return errors_; }

 private:
  static constexpr uint32_t kXfer = 0, kFill = 1, kWait = 2, kReadOp = 3, kBusy = 4, kStream = 5;
  // Кадр блока серии — 516 байтов (129 слов); второй кадр — с этого слова TX (0.1.139)
  static constexpr uint32_t kFrameWords = 129;
  static constexpr uint32_t kTwinWord = 256;
  // Кадр блока серии (0xFF, маркер 0xFC, 512 байтов, CRC16) — в TX моста со слова word
  bool putFrame(const uint8_t* block, uint32_t word);
  // Блоки в серию CMD25 (открыть, если не открыта; 0.1.137/0.1.138). done — сколько блоков карта
  // приняла; false — серия оборвана (и уже закрыта).
  bool writeMulti(uint32_t lba, const uint8_t* data, uint32_t count, uint32_t& done);
  bool seriesOpen_ = false;             // CMD25 открыта: следующий блок серии — seriesNext_
  uint32_t seriesNext_ = 0;
  uint32_t lastYieldMs_ = 0;            // когда операция моста последний раз отдавала процессор

  bool setCtrl(uint32_t div, bool cs);
  bool setPtr(uint32_t word, uint32_t blockBytes);
  // операция; *stat — SD_STAT после неё. opStart — только запустить, opWait — дождаться конца
  // (между ними можно писать в TX то, что мост сейчас не передаёт).
  bool op(uint32_t kind, uint32_t count, uint32_t* stat);
  bool opStart(uint32_t kind, uint32_t count);
  bool opWait(uint32_t* stat);
  // байты — в TX со слова word
  bool txPut(const uint8_t* bytes, uint32_t count, uint32_t word = 0);
  bool rxGet(uint8_t* bytes, uint32_t count);
  // команда: R1 (0xFF — ответа нет)
  uint8_t command(uint8_t index, uint32_t arg, uint8_t crc);
  uint8_t appCommand(uint8_t index, uint32_t arg);
  bool readBlock(uint32_t lba, uint8_t* data);
  bool writeBlock(uint32_t lba, const uint8_t* data);
  bool readCsd(uint8_t csd[16]);
  bool stopTransmission();      // CMD12: команда, байт мусора, R1, конец «занята»

  const SdBridgeIo& io_;
  const SdBridgeLayout* layout_ = &kSdLayoutStd;
  uint32_t clk10k_ = 0;
  uint32_t divFast_ = 0;
  uint32_t div_ = 0xFF;      // полупериод SCK − 1, что сейчас в SD_CTRL
  bool ready_ = false;
  bool sdhc_ = false;
  uint32_t sectors_ = 0;
  uint32_t sckKhz_ = 0;
  uint32_t crcRetries_ = 0;
  uint32_t errors_ = 0;
};

// CRC16-CCITT (полином 0x1021, начальное 0) блока данных SD и CRC7 команды
uint16_t sdCrc16(const uint8_t* data, size_t size);
uint8_t sdCrc7(const uint8_t* data, size_t size);

}  // namespace zifi
