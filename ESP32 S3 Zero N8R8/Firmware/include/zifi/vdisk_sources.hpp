#pragma once
// Источники тома VDAC4 VFS в прошивке. Сейчас один: TANG — microSD Tang (FatFs в /tangsd, мост
// fb_sdspi ядра TS-Config 1.30+, tang_sd::Session на каждое обращение). Флешка USB (/usb) и сетевые
// источники подключаются так же — отдельной папкой корня.
//
// Имена FatFs прошивки — в кодовой странице 437 (CONFIG_FATFS_API_ENCODING_ANSI_OEM): в том они
// идут UTF-8 через таблицу CP437, а путь обратно к FatFs — снова CP437. Знаки, которых в CP437 нет
// (кириллица), FatFs уже отдал как '?': такой файл виден, но не открывается.
#include <stddef.h>
#include <stdint.h>

#include "zifi/board.hpp"   // ZIFI_HAS_SPI_LINK: без него источник TANG молча выпадал (0.1.97/0.1.98)
#include "zifi/vdisk.hpp"

#ifndef ZIFI_HAS_SPI_LINK
#error "zifi/board.hpp не задал ZIFI_HAS_SPI_LINK"
#endif

namespace zifi {
namespace vdisk_sources {

// CP437 -> UTF-8: out — не меньше 3 * n + 1 байт; UTF-8 -> CP437 (нет в таблице — '?'): out — n + 1
size_t cp437ToUtf8(const char* in, size_t n, char* out);
size_t utf8ToCp437(const char* in, char* out, size_t capacity);

// POSIX-каталог точки монтирования как папка тома
class PosixSource : public vdisk::Source {
 public:
  PosixSource(const char* folder, const char* mount) : folder_(folder), mount_(mount) {}
  const char* folder() const override { return folder_; }
  bool list(const char* path, Each each, void* user) override;
  bool read(const char* path, uint64_t offset, uint8_t* out, size_t n, size_t* got) override;

 private:
  bool full(const char* path, char* out, size_t size) const;
  const char* folder_;
  const char* mount_;
};

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
