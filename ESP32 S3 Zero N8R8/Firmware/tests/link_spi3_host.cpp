// Стенд записи по LINK через DMA (src/link_spi3.cpp, 0.1.176) на ПК: тот же исходник, что в прошивке,
// собирается MSVC с моделью контроллера SPI3 и канала GDMA (tests/stubs_link_spi3/link_model.h).
// Печатает OK или список расхождений; код возврата 0 — всё сошлось.
//
// Что проверяется:
//  - транзакции DMA — до kDmaWords слов, каждая с одного адреса, команда записи; слова на линии
//    (снятые моделью в конце транзакции) совпадают с исходными: буфер, который ещё уходит, никто
//    не переписал, хотя следующий кусок копируется во время передачи;
//  - каждая транзакция — в своей рамке CS, CS не поднимается посреди транзакции;
//  - «без ожидания»: вызов возвращается с идущей транзакцией; следующий обмен (чтение, writeBurst,
//    setClock, dmaWait, dmaRelease) сначала дожидается её, а обычные обмены после DMA снова идут
//    через буфер W0…W15;
//  - срок: транзакция, что не кончилась, даёт false; ошибка «хвоста» без ожидания не теряется
//    и возвращается один раз (dmaWait или следующий writeBurstDma, который тогда ничего не шлёт);
//  - память и канал: отказ при нехватке внутреннего ОЗУ или канала GDMA без утечки, освобождение.

#define ZIFI_BOARD_VDAC4 1
#include <link_model.h>

#include "../src/link_spi3.cpp"

#include <vector>

using namespace zifi;

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
  if (!condition) {
    ++g_failures;
    printf("FAIL: %s\n", what);
  }
}

void checkModel(const char* where) {
  for (const std::string& error : g_model.errors) {
    ++g_failures;
    printf("FAIL (%s): model: %s\n", where, error.c_str());
  }
  g_model.errors.clear();
}

std::vector<uint32_t> words(size_t count, uint32_t seed) {
  std::vector<uint32_t> out(count);
  uint32_t x = seed;
  for (uint32_t& w : out) {
    x = x * 1664525u + 1013904223u;
    w = x;
  }
  return out;
}

// Транзакции с номера from: только записи DMA с адреса address, вместе — ровно expect
bool sameOnWire(size_t from, uint32_t address, const std::vector<uint32_t>& expect, size_t* transactions) {
  std::vector<uint32_t> got;
  size_t count = 0;
  for (size_t i = from; i < g_model.log.size(); ++i) {
    const Transaction& t = g_model.log[i];
    if (!t.dma || t.reading || t.cmd != 0x01 || t.addr != address || t.words.empty() ||
        t.words.size() > link_spi3::kDmaWords) {
      return false;
    }
    got.insert(got.end(), t.words.begin(), t.words.end());
    ++count;
  }
  if (transactions != nullptr) *transactions = count;
  return got == expect;
}

}  // namespace

int main() {
  const link_spi3::Pins pins = {14, 15, 16, 17, 18, 21};
  g_model.csPin = pins.cs;
  check(link_spi3::begin(pins, 40000000), "begin");

  // --- память и канал ---
  check(!link_spi3::writeBurstDma(0x01, 0x40000, nullptr, 0, true), "DMA write before prepare refused");
  {
    const std::vector<uint32_t> w = words(4, 1);
    check(!link_spi3::writeBurstDma(0x01, 0x40000, w.data(), w.size(), true), "DMA write needs dmaPrepare");
  }
  // меньше двух буферов + дескрипторов + запаса 12 КБ (на сами дескрипторы не хватает)
  g_model.freeInternal = 2 * link_spi3::kDmaWords * sizeof(uint32_t) + 12 * 1024;
  check(!link_spi3::dmaPrepare(12 * 1024), "prepare refused: internal RAM below the reserve");
  check(g_model.liveAllocs == 0, "nothing allocated on the RAM refusal");
  g_model.freeInternal = 300000;
  g_model.channelsFree = 0;
  check(!link_spi3::dmaPrepare(12 * 1024), "prepare refused: no GDMA channel");
  check(g_model.liveAllocs == 0, "buffers freed when no GDMA channel");
  g_model.channelsFree = 1;
  check(link_spi3::dmaPrepare(12 * 1024), "prepare");
  check(link_spi3::dmaPrepared(), "prepared");
  check(g_model.liveAllocs == 1 && g_model.channelsLive == 1, "one block, one channel");
  check((g_model.lastCaps & MALLOC_CAP_DMA) && (g_model.lastCaps & MALLOC_CAP_INTERNAL), "DMA-capable internal RAM");
  check(link_spi3::dmaPrepare(12 * 1024) && g_model.liveAllocs == 1, "second prepare reuses");
  checkModel("prepare");

  // --- с ожиданием: два полных куска и хвост в 20 слов ---
  const size_t K = link_spi3::kDmaWords;
  {
    const std::vector<uint32_t> w = words(2 * K + 20, 2);
    const size_t from = g_model.log.size();
    check(link_spi3::writeBurstDma(0x01, 0x40000, w.data(), w.size(), true), "DMA write 2K + 20 words");
    size_t n = 0;
    check(sameOnWire(from, 0x40000, w, &n), "2K + 20 words on the wire as sent");
    check(n == 3 && g_model.log[from].words.size() == K && g_model.log[from + 2].words.size() == 20,
          "transactions K + K + 20");
    check(g_model.cs && !g_model.running, "wait: CS high, nothing running at return");
    check(!g_model.hw.dma_tx_ena, "controller back on W0..W15 after DMA");
    checkModel("wait 1300");
  }
  // долгие транзакции: копирование следующего куска идёт, пока предыдущий ещё на линии
  {
    g_model.flightPolls = 40;
    const std::vector<uint32_t> w = words(5000, 3);
    const size_t from = g_model.log.size();
    check(link_spi3::writeBurstDma(0x01, 0x40000, w.data(), w.size(), true), "DMA write 5000 words");
    size_t n = 0;
    check(sameOnWire(from, 0x40000, w, &n) && n == (5000 + K - 1) / K,
          "5000 words in ceil(5000 / K) transactions, buffers not overwritten");
    g_model.flightPolls = 3;
    checkModel("wait 5000");
  }
  // ровно K, 2K и одно слово
  for (size_t count : {K, 2 * K, static_cast<size_t>(1)}) {
    const std::vector<uint32_t> w = words(count, static_cast<uint32_t>(count));
    const size_t from = g_model.log.size();
    size_t n = 0;
    check(link_spi3::writeBurstDma(0x01, 0x40000, w.data(), w.size(), true) && sameOnWire(from, 0x40000, w, &n) &&
              n == (count + link_spi3::kDmaWords - 1) / link_spi3::kDmaWords,
          "exact sizes K / 2K / 1");
    checkModel("exact sizes");
  }

  // --- без ожидания: хвост дожидается следующий обмен ---
  {
    const std::vector<uint32_t> w = words(K + 188, 4);
    const size_t from = g_model.log.size();
    check(link_spi3::writeBurstDma(0x01, 0x40000, w.data(), w.size(), false), "NOWAIT write K + 188");
    check(g_model.running && !g_model.cs, "NOWAIT: last transaction still runs, CS low");
    uint32_t r[4] = {};
    check(link_spi3::transfer(0x02, 0x08, r, 4, true, 8), "read after NOWAIT");
    const Transaction& read = g_model.log.back();
    check(read.reading && read.addr == 0x08 && !read.dma, "read went through W0..W15");
    bool same = true;
    for (int i = 0; i < 4; ++i) same = same && r[i] == Model::pattern(0x08 + 4 * i);
    check(same, "read data");
    std::vector<Transaction> dma(g_model.log.begin() + static_cast<std::ptrdiff_t>(from), g_model.log.end() - 1);
    std::vector<uint32_t> got;
    for (const Transaction& t : dma) got.insert(got.end(), t.words.begin(), t.words.end());
    check(got == w && dma.size() == 2, "NOWAIT: K + 188 words on the wire before the read");
    checkModel("nowait + read");
  }
  {
    // writeBurst (по 16 через W0…W15) после хвоста DMA
    const std::vector<uint32_t> w = words(100, 5);
    check(link_spi3::writeBurstDma(0x01, 0x40000, w.data(), w.size(), false), "NOWAIT write 100");
    const std::vector<uint32_t> b = words(20, 6);
    const size_t from = g_model.log.size();
    check(link_spi3::writeBurst(0x01, 0x40000, b.data(), b.size()), "writeBurst after NOWAIT");
    check(g_model.log.size() == from + 3 && g_model.log[from].dma, "tail finished first");
    std::vector<uint32_t> got;
    for (size_t i = from + 1; i < g_model.log.size(); ++i) {
      check(!g_model.log[i].dma, "writeBurst via W0..W15");
      got.insert(got.end(), g_model.log[i].words.begin(), g_model.log[i].words.end());
    }
    check(got == b, "writeBurst words after DMA");
    checkModel("nowait + writeBurst");
  }
  {
    const std::vector<uint32_t> w = words(30, 7);
    check(link_spi3::writeBurstDma(0x01, 0x40000, w.data(), w.size(), false), "NOWAIT before setClock");
    check(link_spi3::setClock(20000000) && !g_model.running && g_model.cs, "setClock waits for the tail");
    check(link_spi3::writeBurstDma(0x01, 0x40000, w.data(), w.size(), false), "NOWAIT before dmaWait");
    check(link_spi3::dmaWait() && !g_model.running && g_model.cs, "dmaWait");
    check(link_spi3::dmaWait(), "dmaWait with nothing pending");
    checkModel("setClock / dmaWait");
  }
  {
    // два вызова без ожидания подряд: второй копирует в свободный буфер, пока хвост первого идёт
    g_model.flightPolls = 40;
    const std::vector<uint32_t> a = words(K, 8);
    const std::vector<uint32_t> b = words(K, 9);
    const size_t from = g_model.log.size();
    check(link_spi3::writeBurstDma(0x01, 0x40000, a.data(), a.size(), false) &&
              link_spi3::writeBurstDma(0x01, 0x40000, b.data(), b.size(), false) && link_spi3::dmaWait(),
          "two NOWAIT calls in a row");
    std::vector<uint32_t> ab = a;
    ab.insert(ab.end(), b.begin(), b.end());
    check(sameOnWire(from, 0x40000, ab, nullptr), "two NOWAIT calls: both on the wire in order");
    g_model.flightPolls = 3;
    checkModel("two nowait");
  }

  // --- сроки ---
  {
    const std::vector<uint32_t> w = words(100, 10);
    g_model.hang = true;
    check(!link_spi3::writeBurstDma(0x01, 0x40000, w.data(), w.size(), true), "wait: hung transaction -> false");
    g_model.hang = false;
    check(link_spi3::dmaWait(), "error already returned is not sticky");
    check(link_spi3::dmaStats().failed == 1, "failed counted");
    // без ожидания: хвост повис, его «дождался» чужой обмен
    g_model.hang = true;
    check(link_spi3::writeBurstDma(0x01, 0x40000, w.data(), w.size(), false), "NOWAIT start of a hung tail");
    g_model.hang = false;
    uint32_t r[1] = {};
    check(!link_spi3::transfer(0x02, 0x08, r, 1, true, 8), "read that met a hung tail fails");
    check(!link_spi3::dmaWait(), "tail error returned by dmaWait");
    check(link_spi3::dmaWait(), "...only once");
    // ошибку хвоста возвращает следующий writeBurstDma и тогда ничего не шлёт
    g_model.hang = true;
    check(link_spi3::writeBurstDma(0x01, 0x40000, w.data(), w.size(), false), "NOWAIT start of a hung tail 2");
    g_model.hang = false;
    check(!link_spi3::writeRepeat(0x01, 0x40, w.data(), 1), "writeRepeat that met a hung tail fails");
    const size_t before = g_model.log.size();
    check(!link_spi3::writeBurstDma(0x01, 0x40000, w.data(), w.size(), true), "next DMA write returns the tail error");
    check(g_model.log.size() == before, "...and sends nothing");
    const size_t from = g_model.log.size();
    check(link_spi3::writeBurstDma(0x01, 0x40000, w.data(), w.size(), true) && sameOnWire(from, 0x40000, w, nullptr),
          "after the error DMA works again");
    check(g_model.logErrors >= 3, "errors logged");
    checkModel("timeouts");
  }

  // --- голодание DMA: только счётчик ---
  {
    const uint32_t starvedBefore = link_spi3::dmaStats().starved;
    g_model.starve = true;
    const std::vector<uint32_t> w = words(K + 88, 11);
    check(link_spi3::writeBurstDma(0x01, 0x40000, w.data(), w.size(), true), "starved transactions still succeed");
    g_model.starve = false;
    check(link_spi3::dmaStats().starved == starvedBefore + 2, "starved counted per transaction");
    checkModel("starve");
  }

  // --- освобождение с идущим хвостом ---
  {
    const std::vector<uint32_t> w = words(300, 12);
    check(link_spi3::writeBurstDma(0x01, 0x40000, w.data(), w.size(), false), "NOWAIT before release");
    link_spi3::dmaRelease();
    check(!g_model.running && g_model.cs, "release waits for the tail");
    check(!link_spi3::dmaPrepared() && g_model.liveAllocs == 0 && g_model.channelsLive == 0 && g_model.channelsFree == 1,
          "release frees the block and the channel");
    check(!link_spi3::writeBurstDma(0x01, 0x40000, w.data(), w.size(), true), "DMA write after release refused");
    link_spi3::dmaRelease();   // второй раз — ничего
    check(link_spi3::dmaPrepare(12 * 1024) && link_spi3::writeBurstDma(0x01, 0x40000, w.data(), w.size(), true),
          "prepare again after release");
    link_spi3::dmaRelease();
    checkModel("release");
  }

  const link_spi3::DmaStats stats = link_spi3::dmaStats();
  check(stats.calls > 0 && stats.transactions >= stats.calls && stats.words > 0, "stats");

  if (g_failures == 0) {
    printf("OK\n");
    return 0;
  }
  return 1;
}
