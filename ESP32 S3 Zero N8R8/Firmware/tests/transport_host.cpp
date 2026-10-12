// Стенд Z80Transport для test_transport_host.py: кадр уходит в поддельный
// канал, на выходе — его байты в шестнадцатеричном виде и что просили занять
// в очереди. Канал SPI (padsFrames) дополняет кадр нечётной длины нулём.
#include <stdio.h>
#include <stdlib.h>

#include <vector>

#include "zifi/z80_transport.hpp"

namespace {

class FakeLink : public zifi::ByteLink {
 public:
  explicit FakeLink(bool pads) : pads_(pads) {}

  bool begin() override { return true; }
  size_t read(uint8_t*, size_t) override { return 0; }
  size_t write(const uint8_t* data, size_t length) override {
    bytes.insert(bytes.end(), data, data + length);
    return length;
  }
  bool reserve(size_t length, uint32_t) override {
    reserved += length;
    return true;
  }
  void flush() override {}
  const char* name() const override { return "fake"; }
  bool padsFrames() const override { return pads_; }

  std::vector<uint8_t> bytes;
  size_t reserved = 0;

 private:
  bool pads_;
};

}  // namespace

// Аргументы: pads (0/1), длина данных. Данные — i * 7 + 1.
int main(int argc, char** argv) {
  if (argc != 3) {
    return 2;
  }
  FakeLink link(atoi(argv[1]) != 0);
  zifi::Z80Transport transport(link);
  const int length = atoi(argv[2]);
  std::vector<uint8_t> payload(static_cast<size_t>(length));
  for (int i = 0; i < length; ++i) {
    payload[static_cast<size_t>(i)] = static_cast<uint8_t>(i * 7 + 1);
  }
  const bool sent = transport.send(0x42, payload.data(),
                                   static_cast<uint16_t>(length));
  printf("%d %u ", sent ? 1 : 0, static_cast<unsigned>(link.reserved));
  for (uint8_t value : link.bytes) {
    printf("%02X", value);
  }
  printf("\n");
  return 0;
}
