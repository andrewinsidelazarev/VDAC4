#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "zifi/protocol.hpp"

namespace zifi {

class IniConfig;
class NetClient;
class Z80Transport;

// Ядро прошивки — канал с Z80, настройки, сеть, мост VFS, файловые серверы и
// обновление — и приложения поверх него (src/apps): звук, время, погода,
// обновление WC. Приложение получает команды Z80 своего набора; ядро о них
// ничего не знает и раздаёт кадры по списку приложений.
//
// Исполняется приложение на двух ядрах ESP32-S3, как и вся прошивка:
// - ядро 1 — владелец канала: команды, на которые приложение отвечает сразу;
// - ядро 0 — сетевая задача: команды, ждущие сеть или карту Evo. Ядро 1 шлёт
//   на них ACK, передаёт тело через обменник и пересылает ответ.

// Обменник ядра 1 с сетевым ядром: тело команды Z80 и ответ на неё. Живёт в
// PSRAM; в очередях между ядрами — только однобайтовый сигнал.
struct IntercoreExchange {
  uint8_t requestCommand;
  uint16_t requestLength;
  alignas(4) uint8_t request[kMaxPayload];
  uint8_t responseCommand;
  uint16_t responseLength;
  alignas(4) uint8_t response[kMaxPayload];
  char error[49];

  // Причина неудачи: ядро 1 доложит её Z80 кадром #EE перед ответом.
  void setError(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, sizeof(error), format, arguments);
    va_end(arguments);
  }
};

// Что ядро даёт приложениям. Каждый метод помечен ядром, с которого его можно
// звать.
class AppHost {
 public:
  // Ядро 1: канал до Z80, чтобы ответить на команду.
  virtual Z80Transport& transport() = 0;
  // Настройки из zifi.ini (меняет их только сетевое ядро, по WIFI_INI).
  virtual const IniConfig& config() const = 0;
  // Ядро 0: TCP-клиент, общий с командами NET_*.
  virtual NetClient& netClient() = 0;
  // Ядро 0: идёт ли приём прошивки по сети (тогда сеть и карту не трогать).
  virtual bool otaRunning() const = 0;
  // Ядро 0: остановить FTP и SMB. false — сервер не остановился.
  virtual bool stopFileServers() = 0;
  // Ядро 0: событие для Z80. waitMs — сколько ждать места в очереди (0 —
  // нисколько: событие только для индикации).
  virtual bool sendEvent(uint8_t command, const uint8_t* data, uint16_t length,
                         uint32_t waitMs) = 0;
  // Ядро 0: был обмен с картой Evo по сети — радио пока не засыпает.
  virtual void noteFileActivity() = 0;
  // Есть ли PSRAM на самом деле.
  virtual bool psram() const = 0;

 protected:
  ~AppHost() = default;
};

// Как ядро принимает команду, которую исполняет сетевое ядро.
struct NetworkCommand {
  bool ack = true;      // сначала ACK; NET_RECV по старому контракту — без него
  bool payload = true;  // передать тело (NET_NTP его не ждёт)
};

class App {
 public:
  virtual ~App() = default;

  // Короткое имя для сообщений об ошибках ("wc update").
  virtual const char* name() const = 0;

  // Ядро 1, при запуске: канал, настройки и журнал уже работают.
  virtual void begin() {}

  // Ядро 1: команда, которую приложение исполняет сразу и на которую само
  // отвечает. false — команда не его.
  virtual bool handleCommand(const PacketView& packet) {
    (void)packet;
    return false;
  }

  // Ядро 1: команда, которую исполняет сетевое ядро (processNetwork).
  virtual bool networkCommand(uint8_t command, NetworkCommand& how) const {
    (void)command;
    (void)how;
    return false;
  }
  // Ядро 1: ответ-отказ на неё, когда сетевое ядро занято.
  virtual void sendNetworkFailure(uint8_t command) { (void)command; }
  // Ядро 0: исполнить её; ответ — в exchange.
  virtual void processNetwork(IntercoreExchange& exchange) { (void)exchange; }

  // Ядро 0, каждый проход сетевой задачи (около 2 мс).
  virtual void pollNetworkCore(uint32_t now) { (void)now; }

  // Ядро 0: приложение пользуется мостом VFS монопольно (как обновление WC) —
  // остановиться перед запуском FTP или SMB. false — не остановилось.
  virtual bool stopForFileServer() { return true; }
  // Идёт работа, при которой радио не должно спать.
  virtual bool keepsWifiAwake() const { return false; }
  // Идёт работа, которой помешает запись во flash (журнала): звук.
  virtual bool blocksFlashWrites() const { return false; }
};

}  // namespace zifi
