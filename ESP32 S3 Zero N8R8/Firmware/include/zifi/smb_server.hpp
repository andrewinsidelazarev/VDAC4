#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zifi\vfs_bridge.hpp"

namespace zifi {

// Современный SMB2/SMB3-сервер работает в отдельной задаче сетевого ядра.
// Библиотека libsmb2 разбирает пакеты, NTLMSSP и подпись, а этот адаптер
// переводит файловые операции в VFS-команды Wild Commander по каналу Z80.
class SmbServer {
 public:
  using EventSink = bool (*)(void* context, uint8_t command,
                             const uint8_t* data, uint16_t length);

  SmbServer(VfsBridge& bridge, EventSink eventSink, void* eventContext);
  ~SmbServer();

  // Тело SMB_START:
  // [port LE16][share\0][hostname\0][workgroup\0][user\0][password\0].
  // Все строки можно опустить — тогда используются безопасные значения по
  // умолчанию, совпадающие с настройками готового WMF-плагина.
  bool start(const uint8_t* payload, uint16_t length, uint16_t& actualPort,
             bool& netbiosActive, char* error, size_t errorSize);
  bool stop();

  // Вторая шара «TANG» — microSD Tang через FatFs ESP (TangVfs, подключённый к мосту VFS
  // attachLocal с корнем SmbServer::kTangRoot). probe — есть ли она сейчас: Tang на связи, карта
  // вставлена и не отдана WC (SD2). Без вызова шары нет.
  static constexpr const char* kTangRoot = "/\x01TANG";
  static constexpr const char* kTangShare = "TANG";
  using SharePresence = bool (*)(void* context);
  void setTangShare(SharePresence probe, void* context);
  // Шара TANG сейчас есть (для окна плагина: ответ SMB_START).
  bool tangShareAvailable() const;

  // Третья шара «USB» (0.1.171, 25.11.2026; пользователь: «третью шару — USB OTG Flash ESP32. Шара
  // должна быть только если флешка в USB ESP32 вставлена») — флешка в разъёме USB-C ESP32 через
  // FatFs ESP: тот же TangVfs, свой том (usb_share::vfs(), корень SmbServer::kUsbRoot у моста VFS).
  // probe — есть ли она сейчас: флешка вставлена и смонтирована, WC не держит её как SD2 (VDAC4 VFS,
  // режим «USB Flash»). Без вызова шары нет.
  static constexpr const char* kUsbRoot = "/\x02USB";
  static constexpr const char* kUsbShare = "USB";
  void setUsbShare(SharePresence probe, void* context);
  bool usbShareAvailable() const;

  // Пояс из zifi.ini (time:, целые часы). В FAT хранится местное время, а SMB
  // передаёт FILETIME в UTC: пояс нужен для перевода в обе стороны.
  void setTimezoneHours(int8_t hours);

  // NBNS и WS-Discovery обслуживаются из уже существующей сетевой задачи.
  // Сам SMB listener живёт отдельно, потому что цикл libsmb2 блокирующий.
  void pollDiscovery();

  bool running() const;
  uint16_t port() const;
  bool netbiosActive() const;

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace zifi
