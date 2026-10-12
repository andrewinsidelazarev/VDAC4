#pragma once

// Канал стендов ПК до эмулятора Z80 (tools/host_ftp, host_smb, host_wcu): байты
// идут через подмену HardwareSerial из Arduino.h этих заглушек, на другом конце —
// эмулятор Wild Commander поверх обычной папки. У прошивки платы такого канала
// нет: там SpiLink (канал SPI платы Evo).
#include <Arduino.h>

#include "zifi/byte_link.hpp"

namespace zifi {
namespace host {

class SerialLink : public ByteLink {
 public:
  explicit SerialLink(HardwareSerial& serial) : serial_(serial) {}

  bool begin() override { return true; }

  size_t read(uint8_t* output, size_t capacity) override {
    const int available = serial_.available();
    if (available <= 0) {
      return 0;
    }
    const size_t wanted = static_cast<size_t>(available) < capacity
                              ? static_cast<size_t>(available)
                              : capacity;
    return serial_.read(output, wanted);
  }

  size_t write(const uint8_t* data, size_t length) override {
    return serial_.write(data, length);
  }

  // Подмена передаёт кадр эмулятору сразу и целиком: места ждать не нужно.
  bool reserve(size_t, uint32_t) override { return true; }
  void flush() override { serial_.flush(); }
  const char* name() const override { return "HOST"; }

 private:
  HardwareSerial& serial_;
};

}  // namespace host
}  // namespace zifi
