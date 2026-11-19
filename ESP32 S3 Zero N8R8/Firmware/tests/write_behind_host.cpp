// Стенд zifi::WriteBehind (include/zifi/write_behind.hpp) для tests/test_write_behind.py.
//
// Приёмник — «файл» в памяти и список кусков, которые ему пришли. Проверяется:
// - копирование файла секторами подряд (как WC) — куски ровно по 4 КБ, хвост — при drain;
// - случайная запись (переходы, повторы секторов, куски не по 512) после drain даёт тот же файл,
//   что прямая запись тех же байтов, и ни один кусок не длиннее буфера;
// - отказ приёмника получает тот вызов, который выталкивал; накопленное при этом отброшено;
// - drop() забывает накопленное; без буфера запись идёт приёмнику сразу.
// Печатает «PASS <проверок>» и выходит с 0; при первой ошибке — «FAIL …» и 1.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <random>
#include <vector>

#include "zifi/write_behind.hpp"

namespace {

unsigned long checks = 0;

void check(bool ok, const char* what, long long a = 0, long long b = 0) {
  ++checks;
  if (!ok) {
    printf("FAIL %s (%lld, %lld)\n", what, a, b);
    exit(1);
  }
}

struct Sink {
  std::vector<uint8_t> file;
  std::vector<size_t> chunks;
  long failAt = -1;   // номер вызова (с 0), который откажет

  static bool write(void* user, uint64_t offset, const uint8_t* data, size_t n) {
    Sink& s = *static_cast<Sink*>(user);
    const long call = static_cast<long>(s.chunks.size());
    s.chunks.push_back(n);
    if (call == s.failAt) return false;
    if (s.file.size() < offset + n) s.file.resize(offset + n, 0);
    memcpy(s.file.data() + offset, data, n);
    return true;
  }
};

void direct(std::vector<uint8_t>& file, uint64_t offset, const uint8_t* data, size_t n) {
  if (file.size() < offset + n) file.resize(offset + n, 0);
  memcpy(file.data() + offset, data, n);
}

constexpr size_t kCap = 4096;

void sequentialCopy() {
  std::vector<uint8_t> buffer(kCap);
  for (size_t sectors : {1u, 7u, 8u, 9u, 64u, 200u, 2048u}) {
    zifi::WriteBehind wb;
    wb.attach(buffer.data(), kCap);
    Sink sink;
    std::vector<uint8_t> want;
    uint8_t sector[512];
    for (size_t i = 0; i < sectors; ++i) {
      for (size_t k = 0; k < 512; ++k) sector[k] = static_cast<uint8_t>(i * 31 + k * 7);
      check(wb.put(i * 512, sector, 512, Sink::write, &sink), "put sequential");
      direct(want, i * 512, sector, 512);
      check(wb.pending() == (i + 1) * 512 % kCap, "pending after sector", wb.pending(), i);
    }
    check(sink.chunks.size() == sectors * 512 / kCap, "full chunks only before drain", sink.chunks.size(), sectors);
    check(wb.drain(Sink::write, &sink), "drain");
    check(wb.pending() == 0, "empty after drain");
    const size_t calls = (sectors * 512 + kCap - 1) / kCap;
    check(sink.chunks.size() == calls, "chunks for sequential copy", sink.chunks.size(), calls);
    for (size_t i = 0; i < sink.chunks.size(); ++i) {
      const size_t expect = i + 1 < calls || sectors * 512 % kCap == 0 ? kCap : sectors * 512 % kCap;
      check(sink.chunks[i] == expect, "chunk size", sink.chunks[i], expect);
    }
    check(sink.file == want, "sequential file");
    check(wb.drain(Sink::write, &sink), "second drain");
    check(sink.chunks.size() == calls, "second drain writes nothing");
  }
}

void randomWrites(uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<uint8_t> buffer(kCap);
  zifi::WriteBehind wb;
  wb.attach(buffer.data(), kCap);
  Sink sink;
  std::vector<uint8_t> want;
  std::vector<uint8_t> data(3 * kCap);
  uint64_t next = 0;
  for (int step = 0; step < 4000; ++step) {
    const unsigned kind = rng() % 100;
    uint64_t offset;
    size_t n;
    if (kind < 70) {              // подряд, сектор
      offset = next;
      n = 512;
    } else if (kind < 80) {       // повтор прежнего сектора (APPEND переписывает хвост)
      offset = next >= 512 ? next - 512 : 0;
      n = 512;
    } else if (kind < 90) {       // переход в другое место файла
      offset = (rng() % 512) * 512;
      n = 512;
    } else {                      // кусок не по сектору, иногда длиннее буфера
      offset = next + rng() % 3;
      n = 1 + rng() % (2 * kCap + 100);
    }
    for (size_t k = 0; k < n; ++k) data[k] = static_cast<uint8_t>(rng());
    check(wb.put(offset, data.data(), n, Sink::write, &sink), "put random");
    direct(want, offset, data.data(), n);
    next = offset + n;
    check(wb.pending() <= kCap, "pending within buffer", wb.pending());
    if (rng() % 50 == 0) {
      check(wb.drain(Sink::write, &sink), "random drain");
      check(sink.file == want, "file after random drain", step);
    }
  }
  check(wb.drain(Sink::write, &sink), "final drain");
  check(sink.file == want, "random file");
  for (size_t n : sink.chunks) check(n <= kCap, "chunk within buffer", n);
}

void failures() {
  std::vector<uint8_t> buffer(kCap);
  uint8_t sector[512];
  memset(sector, 0x5A, sizeof sector);
  // отказ на втором куске: его получает put, вытолкнувший буфер
  {
    zifi::WriteBehind wb;
    wb.attach(buffer.data(), kCap);
    Sink sink;
    sink.failAt = 1;
    size_t failedAt = 0;
    for (size_t i = 0; i < 24; ++i) {
      const bool ok = wb.put(i * 512, sector, 512, Sink::write, &sink);
      if (!ok) {
        check(failedAt == 0, "only one failure");
        failedAt = i;
      }
    }
    check(failedAt == 15, "failure reported by the filling put", failedAt);
    check(wb.drain(Sink::write, &sink), "drain after failure");
    check(sink.chunks.size() == 3, "chunks after failure", sink.chunks.size());
    check(sink.file.size() == 24 * 512, "file size after failure", sink.file.size());
    // байты отказавшего куска не легли
    bool zero = true;
    for (size_t k = kCap; k < 2 * kCap; ++k) zero = zero && sink.file[k] == 0;
    check(zero, "failed chunk not written");
  }
  // отказ при переходе: его получает put не подряд
  {
    zifi::WriteBehind wb;
    wb.attach(buffer.data(), kCap);
    Sink sink;
    sink.failAt = 0;
    check(wb.put(0, sector, 512, Sink::write, &sink), "buffered");
    check(!wb.put(10 * 512, sector, 512, Sink::write, &sink), "jump reports the failure");
    check(wb.pending() == 0, "nothing pending after failed jump");
    check(wb.put(10 * 512, sector, 512, Sink::write, &sink), "next put works");
    check(wb.drain(Sink::write, &sink), "drain works");
    check(sink.file.size() == 11 * 512, "file after jump failure");
  }
  // drop
  {
    zifi::WriteBehind wb;
    wb.attach(buffer.data(), kCap);
    Sink sink;
    check(wb.put(0, sector, 512, Sink::write, &sink), "buffered for drop");
    wb.drop();
    check(wb.pending() == 0, "drop forgets");
    check(wb.drain(Sink::write, &sink), "drain after drop");
    check(sink.chunks.empty(), "nothing written after drop");
  }
  // без буфера — сразу приёмнику
  {
    zifi::WriteBehind wb;
    Sink sink;
    check(!wb.attached(), "not attached");
    check(wb.put(512, sector, 512, Sink::write, &sink), "direct put");
    check(sink.chunks.size() == 1 && sink.chunks[0] == 512, "direct chunk");
    wb.attach(nullptr, kCap);
    check(!wb.attached(), "attach nullptr");
    sink.failAt = 1;
    check(!wb.put(1024, sector, 512, Sink::write, &sink), "direct failure");
  }
}

}  // namespace

int main() {
  sequentialCopy();
  for (uint32_t seed = 1; seed <= 20; ++seed) randomWrites(seed);
  failures();
  printf("PASS %lu\n", checks);
  return 0;
}
