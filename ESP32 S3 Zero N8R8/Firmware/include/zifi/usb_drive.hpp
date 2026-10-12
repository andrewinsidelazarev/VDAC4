#pragma once

#include "zifi/board.hpp"

#if ZIFI_HAS_USB_DRIVE

#include <stddef.h>
#include <stdint.h>

namespace zifi {

// Флешка в разъёме USB-C модуля: USB OTG ESP32-S3 работает хостом, флешка — том
// FAT32 или exFAT в /usb. На ней ядро держит кэш ресурсов музыки
// (/usb/SYSTEM/CACHE/MUSIC, ResourceDiskCache).
//
// USB-хост включается только при первом обращении: до этого разъём остаётся
// консолью USB Serial/JTAG (журнал, прошивка по USB). После включения консоли и
// прошивки по USB через этот разъём нет до перезапуска; сетевые остаются.
//
// Флешку годной признаёт загрузочный сектор: том FAT32 или exFAT — сам по себе
// или первым разделом таблицы MBR с файловой системой (как ищет том FatFs).
// FAT12/16, NTFS, разметка GPT, неразмеченная флешка — не годятся: ядро работает
// без неё, как прежде.
//
// Все вызовы — с ядра 1 (команды Z80): подключение, проверка и монтирование
// идут там же. Событие драйвера (флешку вставили, вынули) только отмечается.
class UsbDrive {
 public:
  static constexpr const char* kMountPoint = "/usb";
  static constexpr const char* kMusicCache = "/usb/SYSTEM/CACHE/MUSIC";

  // Можно ли сейчас работать с кэшем на флешке. Первый вызов включает USB-хост
  // и ждёт подключения флешки до kFirstWaitMs; потом ответ — сразу: вставленная
  // позже флешка проверяется при следующем вызове.
  bool ready();

 private:
  static constexpr uint32_t kFirstWaitMs = 2000;

  bool startHost();
  void handleEvents();
  void attach(uint8_t address);
  void detach();

  bool hostStarted_ = false;
  bool hostFailed_ = false;
  void* device_ = nullptr;      // msc_host_device_handle_t
  void* vfs_ = nullptr;         // msc_host_vfs_handle_t
  bool usable_ = false;
};

UsbDrive& usbDrive();

}  // namespace zifi

#endif  // ZIFI_HAS_USB_DRIVE
