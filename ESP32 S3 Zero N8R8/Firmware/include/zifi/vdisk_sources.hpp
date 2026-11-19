#pragma once
// Источники тома VDAC4 VFS в прошивке: TANG — microSD Tang (FatFs в /tangsd, мост fb_sdspi ядра
// TS-Config 1.30+ или ядра видео 1.47+, tang_sd::Session на каждое обращение); USB (1.47, 11.11.2026)
// — флешка FAT32 или exFAT в разъёме USB-C модуля (UsbDrive, /usb). Сетевые источники подключаются
// так же — отдельной папкой корня.
//
// Имена FatFs прошивки — в её кодовой странице OEM (CONFIG_FATFS_API_ENCODING_ANSI_OEM; с 0.1.148 —
// 866, как у WC, прежде 437): в том они идут UTF-8 через таблицы FatFs (ff_oem2uni), а путь обратно
// к FatFs — снова в OEM (ff_uni2oem). Знака нет в кодовой странице — '?': такой файл виден, но не
// открывается и не создаётся. С CP437 так было со всей кириллицей.
#include <stddef.h>
#include <stdint.h>

#include "zifi/board.hpp"   // ZIFI_HAS_SPI_LINK: без него источник TANG молча выпадал (0.1.97/0.1.98)
#include "zifi/vdisk.hpp"
#include "zifi/write_behind.hpp"

#ifndef ZIFI_HAS_SPI_LINK
#error "zifi/board.hpp не задал ZIFI_HAS_SPI_LINK"
#endif

namespace zifi {
namespace vdisk_sources {

// OEM FatFs -> UTF-8: out — не меньше 3 * n + 1 байт; UTF-8 -> OEM (нет в странице — '?'): out — n + 1
size_t oemToUtf8(const char* in, size_t n, char* out);
size_t utf8ToOem(const char* in, char* out, size_t capacity);

// POSIX-каталог точки монтирования как папка тома
class PosixSource : public vdisk::Source {
 public:
  PosixSource(const char* folder, const char* mount) : folder_(folder), mount_(mount) {}
  const char* folder() const override { return folder_; }
  bool list(const char* path, Each each, void* user) override;
  bool read(const char* path, uint64_t offset, uint8_t* out, size_t n, size_t* got) override;

 protected:
  bool full(const char* path, char* out, size_t size) const;

 private:
  const char* folder_;
  const char* mount_;
};

#if ZIFI_HAS_USB_DRIVE
// Флешка USB: каталог (WC открыл папку USB — ядро 1) показывается, когда флешка готова (UsbDrive::ready;
// хост и флешку с 0.1.146 включает при запуске ESP задача usb-drive). Чтение файла — из любой задачи
// (видео с SD2: задача подачи VIDEO.ELF): флешка уже смонтирована. Читается кусками не больше
// UsbDrive::kTransferBytes — буфер драйвера USB во внутренней памяти не растёт (0.1.146).
class UsbSource : public PosixSource {
 public:
  UsbSource();
  bool list(const char* path, Each each, void* user) override;
  bool read(const char* path, uint64_t offset, uint8_t* out, size_t n, size_t* got) override;
  // Запись — для тома USB Flash (0.1.148, vdisk::Volume::configureWritable): файлы флешки через
  // FatFs (exFAT тоже). Файл, в который идут секторы, остаётся открытым до другого файла или flush.
  // С 0.1.149 секторы подряд копятся в WriteBehind и уходят на флешку кусками по
  // UsbDrive::kTransferBytes (одна команда USB на 8 секторов); накопленное выталкивают другой файл,
  // чтение этого файла, операции над ним и flush (служба VDISK зовёт его через 1 с тишины).
  // С 0.1.150 куски пишет на флешку своя задача usb-write: кусок встаёт в кольцо kSlots x 4 КБ
  // (PSRAM), и WRITE от WC флешку не ждёт. На 0.1.149 копирование 37 МБ шло 559 с, из них 80 с WC
  // ждал флешку, в том числе ~33 паузы самой флешки по 0,6 с; кольцо (128 КБ) их прячет. Перед
  // закрытием файла, чтением его и операциями над ним кольцо дописывается (sync). Отказ флешки в
  // задаче — следующему write() (или закрытию), как и прежде отложенный отказ.
  bool create(const char* path, uint64_t size) override;
  bool write(const char* path, uint64_t offset, const uint8_t* data, size_t n) override;
  bool resize(const char* path, uint64_t size) override;
  bool makeDir(const char* path) override;
  bool remove(const char* path, bool dir) override;
  bool rename(const char* from, const char* to) override;
  bool setTime(const char* path, uint16_t date, uint16_t time) override;
  void flush() override;

 private:
  bool closeOpen();          // накопленное — на флешку, файл закрыт; false — запись не легла
  bool writeNow(uint64_t offset, const uint8_t* data, size_t n);
  bool queue(uint64_t offset, const uint8_t* data, size_t n);   // кусок — в кольцо задаче usb-write
  bool startWriter();
  void sync();                // ждать, пока задача допишет всё кольцо
  static void writerTask(void* self);
  static bool sink(void* user, uint64_t offset, const uint8_t* data, size_t n);
  static constexpr size_t kSlots = 32;
  struct Slot {
    uint64_t offset;
    uint32_t length;
  };
  void* lock_ = nullptr;      // SemaphoreHandle_t, рекурсивный: запись и чтение — из разных задач
  void* free_ = nullptr;      // SemaphoreHandle_t: свободные куски кольца
  void* ready_ = nullptr;     // QueueHandle_t: номера кусков к записи
  uint8_t* ring_ = nullptr;
  Slot* slots_ = nullptr;     // в PSRAM за кольцом: внутренняя память ESP на счету
  size_t nextSlot_ = 0;
  bool writerTried_ = false;
  volatile bool asyncFailed_ = false;   // задача usb-write: кусок не лёг
  uint32_t ringFull_ = 0;     // сколько раз WRITE ждал места в кольце (флешка не успевала)
  int fd_ = -1;
  char open_[600] = {};
  WriteBehind pending_;
  // скорость записи открытого файла: большой файл (от kSpeedLogBytes) — строка в журнале
  uint64_t bytes_ = 0;
  int64_t firstUs_ = 0;
  int64_t lastUs_ = 0;
  int64_t usbUs_ = 0;
  uint32_t commands_ = 0;
  bool failed_ = false;      // вытолкнутое без вызывающего не легло: отказ следующему write()
};
#endif

#if ZIFI_HAS_SPI_LINK
// microSD Tang: замок tang_sd::Session на время обращения, том монтируется по запросу
class TangSource : public PosixSource {
 public:
  TangSource() : PosixSource("TANG", "/tangsd") {}
  bool list(const char* path, Each each, void* user) override;
  bool read(const char* path, uint64_t offset, uint8_t* out, size_t n, size_t* got) override;
};
#endif

}  // namespace vdisk_sources
}  // namespace zifi
