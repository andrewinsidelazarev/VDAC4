#pragma once

#include <stddef.h>
#include <stdint.h>

namespace zifi {

// Байтовый канал до Z80. Кадры протокола (protocol.hpp) собирает и разбирает
// Z80Transport, а канал лишь переносит байты: SPI на плате VDAC4, подмена на
// стендах ПК. Ни один метод не ждёт дольше, чем сказано в его описании.
class ByteLink {
 public:
  virtual ~ByteLink() = default;

  virtual bool begin() = 0;

  // Забрать уже принятые байты. Возвращает их число, ноль — данных пока нет.
  virtual size_t read(uint8_t* output, size_t capacity) = 0;

  // Передать, сколько помещается прямо сейчас. Возвращает число принятых байтов.
  virtual size_t write(const uint8_t* data, size_t length) = 0;

  // Дождаться места под кадр длиной length. Нужно каналу с очередью (SPI): кадр
  // должен попасть в неё целиком либо не попасть вовсе, иначе Z80 получит начало
  // одного кадра и следом другой. Канал без очереди (подмена стендов ПК) места не ждёт.
  virtual bool reserve(size_t length, uint32_t timeoutMs) = 0;

  // Дождаться, пока переданное уйдёт из прошивки.
  virtual void flush() = 0;

  // Короткое имя канала для SYS_INFO.
  virtual const char* name() const = 0;

  // Дополнять кадры нулём до чётной длины. Нужно каналу SPI: так каждый кадр
  // начинается в потоке с чётного байта, и Z80 переносит его данные из порции
  // в буфер кадра через DMA, а DMA TS-Config ходит словами. Байт вне кадра Z80
  // пропускает, пока ищет SYNC, — и прежние программы тоже.
  virtual bool padsFrames() const { return false; }
};

}  // namespace zifi
