#pragma once

#include <Arduino.h>

#include "zifi/byte_link.hpp"
#include "zifi/protocol.hpp"

namespace zifi {

// Обмен кадрами протокола с Z80 и единственный владелец канала на ядре 1.
// Канал — ByteLink: на плате VDAC4 это SpiLink (SPI платы Evo), на стендах ПК —
// подмена из tests/stubs_host. Любая текстовая печать в канал запрещена: Z80
// воспримет её как часть двоичного потока и потеряет синхронизацию.
class Z80Transport {
 public:
  using IdleHook = void (*)(void* context);
  using UnexpectedHandler = void (*)(void* context, const PacketView& packet);

  explicit Z80Transport(ByteLink& link);

  bool begin();
  bool poll(PacketView& packet);
  bool send(uint8_t command, const uint8_t* data = nullptr, uint16_t length = 0);
  bool sendAck() { return send(kAck); }
  bool sendError(const char* text);
  void flush();

  // Ждёт ответ Z80 на VFS-команду. Необязательная фоновая функция оставлена
  // для служебных вызовов; в двухъядерном STOR сеть работает независимо.
  bool waitFor(uint8_t wanted, uint32_t timeoutMs, PacketView& packet,
               IdleHook idleHook = nullptr, void* idleContext = nullptr,
               UnexpectedHandler unexpected = nullptr,
               void* unexpectedContext = nullptr);

  const FrameParser& parser() const { return parser_; }
  const char* linkName() const { return link_->name(); }

 private:
  static constexpr size_t kRxChunkSize = 256;
  static constexpr size_t kTxChunkSize = kMaxPayload;
  // Заголовок кадра (4 байта) и контрольная сумма.
  static constexpr size_t kFrameOverhead = 5;
  static constexpr uint32_t kSendTimeoutMs = 2000;

  static bool writerThunk(void* context, const uint8_t* data, size_t length);
  bool writeAll(const uint8_t* data, size_t length);

  ByteLink* link_;
  FrameParser parser_;
  // Канал отдаёт принятое блоком. Непрочитанный хвост сохраняется между
  // poll(), поэтому два кадра из одного блока не смешиваются и не теряются
  // после возврата первого PacketView.
  uint8_t rxChunk_[kRxChunkSize];
  size_t rxChunkOffset_;
  size_t rxChunkLength_;
};

}  // namespace zifi
