#include "zifi/z80_transport.hpp"

#include <string.h>

namespace zifi {

Z80Transport::Z80Transport(ByteLink& link)
    : link_(&link),
      parser_(),
      rxChunk_{},
      rxChunkOffset_(0),
      rxChunkLength_(0) {}

bool Z80Transport::begin() {
  rxChunkOffset_ = 0;
  rxChunkLength_ = 0;
  return link_->begin();
}

bool Z80Transport::poll(PacketView& packet) {
  for (;;) {
    if (rxChunkOffset_ == rxChunkLength_) {
      rxChunkOffset_ = 0;
      rxChunkLength_ = link_->read(rxChunk_, sizeof(rxChunk_));
      if (rxChunkLength_ == 0) {
        parser_.checkTimeout(millis());
        return false;
      }
    }

    // Для всех уже принятых байтов достаточно одного времени прихода. Раньше
    // millis() и чтение канала вызывались для каждого байта кадра.
    const uint32_t receivedAt = millis();
    while (rxChunkOffset_ < rxChunkLength_) {
      const uint8_t value = rxChunk_[rxChunkOffset_++];
      if (!parser_.feed(value, receivedAt, packet)) {
        continue;
      }
      return true;
    }
  }
}

bool Z80Transport::writerThunk(void* context, const uint8_t* data, size_t length) {
  return static_cast<Z80Transport*>(context)->writeAll(data, length);
}

bool Z80Transport::writeAll(const uint8_t* data, size_t length) {
  size_t offset = 0;
  uint32_t idleSince = millis();
  while (offset < length) {
    // Обмен с Z80 работает на core 1, а Wi-Fi и SMB — на core 0. Поэтому кадр
    // можно передать одним блоком до 1024 байт без шестнадцати лишних yield().
    const size_t remaining = length - offset;
    const size_t part =
        remaining < kTxChunkSize ? remaining : kTxChunkSize;
    const size_t written = link_->write(data + offset, part);
    if (written != 0) {
      offset += written;
      idleSince = millis();
      yield();
      continue;
    }
    if (static_cast<uint32_t>(millis() - idleSince) >= kSendTimeoutMs) {
      return false;
    }
    delay(1);
  }
  return true;
}

bool Z80Transport::send(uint8_t command, const uint8_t* data, uint16_t length) {
  const size_t frameLength = static_cast<size_t>(length) + kFrameOverhead;
  const bool pad = link_->padsFrames() && (frameLength & 1U) != 0;
  // Кадр уходит в канал целиком либо не уходит вовсе: обрывок кадра сбил бы
  // разбор на стороне Z80 до следующей синхронизации.
  if (!link_->reserve(frameLength + (pad ? 1 : 0), kSendTimeoutMs)) {
    return false;
  }
  static const uint8_t kPad = 0;
  return writeFrame(writerThunk, this, command, data, length) &&
         (!pad || writeAll(&kPad, 1));
}

bool Z80Transport::sendError(const char* text) {
  if (text == nullptr) {
    text = "error";
  }
  const size_t textLength = strlen(text);
  const size_t sendLength = textLength < 48 ? textLength : 48;
  return send(kError, reinterpret_cast<const uint8_t*>(text),
              static_cast<uint16_t>(sendLength));
}

void Z80Transport::flush() {
  link_->flush();
}

bool Z80Transport::waitFor(uint8_t wanted, uint32_t timeoutMs, PacketView& packet,
                            IdleHook idleHook, void* idleContext,
                            UnexpectedHandler unexpected,
                            void* unexpectedContext) {
  const uint32_t started = millis();
  // Вычитание беззнаковых millis() намеренно: оно корректно переживает
  // переполнение системного счётчика примерно раз в 49 суток.
  while (static_cast<uint32_t>(millis() - started) < timeoutMs) {
    if (idleHook != nullptr) {
      idleHook(idleContext);
    }
    if (poll(packet)) {
      if (packet.command == wanted) {
        return true;
      }
      if (unexpected != nullptr) {
        unexpected(unexpectedContext, packet);
      }
    } else {
      delay(1);
    }
    yield();
  }
  return false;
}

}  // namespace zifi
