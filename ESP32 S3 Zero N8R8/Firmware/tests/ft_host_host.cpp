// 0.1.180: стенд окна FT812 (src/ft_host.cpp) на ПК — тот же исходник, что в прошивке, с моделью
// очереди записи ядра Tang 1.29+ (tests/stubs_ft_host/ft_model.h) вместо LINK.
// Печатает OK или список расхождений; код возврата 0 — всё сошлось.
//
// Что проверяется (ft_host::writeBlock — путь ft_write приложений):
//  - с буферами DMA пачки больше 16 слов идут через writeBurstDma, а пачки по 16 (writeBurst) — только
//    короткие; без буферов DMA — как в 0.1.82, ни одного вызова DMA;
//  - ни одна пачка не длиннее свободных мест очереди (FT_FREE) на её начало, слово в полную очередь
//    не приходит — при любой скорости слива очереди в память FT812 (быстрее линии, вровень, медленнее);
//  - память FT812 после записи — ровно записанные слова (RAM_G, RAM_DL, регистр);
//  - транзакция DMA, что «не кончилась за срок», — блок повторяется медленно и всё равно ложится верно.
#define ZIFI_BOARD_VDAC4 1
#include <ft_model.h>

#include "../src/ft_host.cpp"

#include <random>

using namespace zifi;

// --- LINK и соседи ft_host — поверх модели ---
namespace zifi {
namespace tang {

LinkGuard::LinkGuard(const char*, int) : taken_(true) {}
LinkGuard::~LinkGuard() {}

bool ready() { return true; }
uint32_t coreVersion() { return 0x0201011DU; }   // ядро FT812 (личность 2), 1.29: очередь окна есть
uint32_t reloads() { return 0; }
bool burstDmaPrepared() { return g_ft.dmaPrepared; }

bool read(uint32_t address, uint32_t* words, size_t count, const char*, int) {
  g_ft.advance(FtModel::kExchange + static_cast<unsigned>(count));
  for (size_t i = 0; i < count; ++i) {
    uint32_t value = 0;
    if (address == kRegFtStat) {
      value = (g_ft.queue.empty() ? 0U : 1U << 31) | (g_ft.lost ? 1U << 14 : 0U);
    } else if (address == kRegFtFree) {
      const uint32_t free = g_ft.freeWords();
      value = free ^ (free >> 1);   // код Грея
    } else if (address == kRegFtData) {
      const auto it = g_ft.mem.find(g_ft.readAddress);
      value = it == g_ft.mem.end() ? 0U : it->second;
    } else if (address == kRegBoot) {
      value = (kBootTag << 20) | (1U << 17);   // ядро с FT812
    }
    words[i] = value;
  }
  return true;
}

bool write(uint32_t address, const uint32_t* words, size_t count, const char*, int) {
  g_ft.advance(FtModel::kExchange);
  for (size_t i = 0; i < count; ++i) {
    if (address == kRegFtAddr) {
      if (!g_ft.queue.empty()) {
        g_ft.lost = true;   // FT_ADDR в занятое окно ядро отбрасывает
      } else {
        g_ft.queueAddress = g_ft.readAddress = words[i] & 0x3FFFFCU;
        g_ft.lost = false;
      }
    } else if (address == kRegFtData) {
      g_ft.push(words[i]);
    }
  }
  return true;
}

bool writeRepeat(uint32_t address, const uint32_t* words, size_t count, const char*, int) {
  g_ft.errors.push_back("writeRepeat у ядра с очередью");
  (void)address;
  for (size_t i = 0; i < count; ++i) g_ft.push(words[i]);
  return true;
}

bool writeBurst(uint32_t address, const uint32_t* words, size_t count, const char*, int) {
  if (address < 0x40000U || address > 0x7FFFFU) g_ft.errors.push_back("writeBurst не в окно FT_BURST");
  if (g_ft.dmaPrepared && count > 16) ++g_ft.longBurstsWithDma;
  if (count > g_ft.freeWords()) ++g_ft.overCredit;
  ++g_ft.burstCalls;
  for (size_t at = 0; at < count; at += 16) {
    g_ft.advance(FtModel::kExchange);
    for (size_t i = at; i < count && i < at + 16; ++i) g_ft.push(words[i]);
  }
  return true;
}

bool writeBurstDma(uint32_t address, const uint32_t* words, size_t count, bool wait, const char*, int) {
  if (!g_ft.dmaPrepared) {
    g_ft.errors.push_back("writeBurstDma без буферов");
    return false;
  }
  if (address < 0x40000U || address + 4U * 256U - 4U > 0x7FFFFU) g_ft.errors.push_back("DMA не в окно FT_BURST");
  if (!wait) g_ft.errors.push_back("DMA без ожидания: ошибку хвоста следующий обмен не вернёт");
  if (count > g_ft.freeWords()) ++g_ft.overCredit;
  ++g_ft.dmaCalls;
  g_ft.dmaWords += count;
  if (g_ft.dmaCalls == g_ft.failDmaCall) {
    for (size_t i = 0; i < count / 2; ++i) g_ft.push(words[i]);   // половина дошла, транзакция встала
    return false;
  }
  for (size_t at = 0; at < count; at += 256) {
    ++g_ft.dmaTransactions;
    g_ft.advance(FtModel::kDmaStart);
    for (size_t i = at; i < count && i < at + 256; ++i) g_ft.push(words[i]);
  }
  return true;
}

}  // namespace tang

namespace tang_flash {
bool slotHasBitstream(uint32_t) { return true; }
}  // namespace tang_flash

namespace tang_sd {
bool heldByWc() { return false; }
}  // namespace tang_sd
}  // namespace zifi

namespace {

int g_failures = 0;
std::vector<std::string> g_modelErrors;   // ошибки модели всех случаев (reset её обнуляет)

void check(bool condition, const char* what, double drain = 0, bool dma = false) {
  if (!condition) {
    ++g_failures;
    printf("FAIL: %s (слив %.2f слова за слово линии, DMA %s)\n", what, drain, dma ? "да" : "нет");
  }
}

void reset(double drain, bool dma) {
  g_modelErrors.insert(g_modelErrors.end(), g_ft.errors.begin(), g_ft.errors.end());
  g_ft = FtModel{};
  g_ft.drain = drain;
  g_ft.dmaPrepared = dma;
}

struct Block {
  uint32_t address;
  std::vector<uint32_t> words;
};

std::vector<uint32_t> randomWords(size_t count, std::mt19937& rng) {
  std::vector<uint32_t> words(count);
  for (uint32_t& w : words) w = rng();
  return words;
}

// Записи кадра — как у HMM2 (трасса ходьбы: ~8 вызовов, ~1400 слов): полосы RAM_G по ~450 слов,
// короткие, список кадра RAM_DL, длинная выгрузка текстуры и пограничные длины 16 и 17.
std::vector<Block> frameBlocks(std::mt19937& rng) {
  std::vector<Block> blocks;
  const size_t sizes[] = {451, 451, 9, 1024, 3000, 1, 17, 16, 300, 513};
  uint32_t address = 0x080000;
  for (size_t n : sizes) {
    blocks.push_back({address, randomWords(n, rng)});
    address += static_cast<uint32_t>(4 * n + 64);
  }
  blocks.push_back({0x300000, randomWords(2048, rng)});   // RAM_DL: без сверки чтением
  return blocks;
}

bool memoryHas(const Block& block) {
  for (size_t i = 0; i < block.words.size(); ++i) {
    const auto it = g_ft.mem.find(block.address + 4U * static_cast<uint32_t>(i));
    if (it == g_ft.mem.end() || it->second != block.words[i]) return false;
  }
  return true;
}

// Время кадра записей — в словах линии
uint64_t runFrame(double drain, bool dma) {
  reset(drain, dma);
  std::mt19937 rng(static_cast<uint32_t>(drain * 1000) + (dma ? 7U : 0U));
  const std::vector<Block> blocks = frameBlocks(rng);
  for (const Block& block : blocks) {
    check(ft_host::writeBlock(block.address, block.words.data(), block.words.size()),
          "writeBlock вернул ошибку", drain, dma);
  }
  for (const Block& block : blocks) check(memoryHas(block), "память FT812 не та", drain, dma);
  check(g_ft.overflows == 0, "слово пришло в полную очередь", drain, dma);
  check(g_ft.overCredit == 0, "пачка длиннее свободных мест очереди", drain, dma);
  check(g_ft.longBurstsWithDma == 0, "при буферах DMA длинная пачка ушла по 16", drain, dma);
  if (dma) {
    // Очередь сливается не медленнее линии — места есть, и почти всё (блоки длиннее 16 — 7804
    // слова) идёт через DMA. Медленнее — пачки ограничивает FT_FREE, и часть уходит по 16
    check(g_ft.dmaCalls > 0 && (drain < 1.0 || g_ft.dmaWords > 7000), "DMA не использован", drain, dma);
    fprintf(stderr, "слив %.2f: DMA вызовов %d, транзакций %d, слов %llu; пачек по 16 — %d; время %llu слов линии\n",
           drain, g_ft.dmaCalls, g_ft.dmaTransactions, static_cast<unsigned long long>(g_ft.dmaWords),
           g_ft.burstCalls, static_cast<unsigned long long>(g_ft.now));
  } else {
    check(g_ft.dmaCalls == 0, "DMA без буферов", drain, dma);
    fprintf(stderr, "слив %.2f без DMA: пачек по 16 — %d; время %llu слов линии\n", drain, g_ft.burstCalls,
            static_cast<unsigned long long>(g_ft.now));
  }
  return g_ft.now;
}

}  // namespace

int main() {
  // Слив очереди: быстрее линии, вровень, медленнее (FT_FREE тогда ограничивает пачки)
  // DMA не медленнее пачек по 16 ни при какой скорости слива, а когда очередь успевает — быстрее
  for (double drain : {4.0, 1.0, 0.5, 0.2}) {
    const uint64_t bursts = runFrame(drain, false);
    const uint64_t dma = runFrame(drain, true);
    check(dma <= bursts, "с DMA запись кадра дольше", drain, true);
    check(drain < 1.0 || dma * 10 < bursts * 7, "с DMA при быстром сливе нет выигрыша 30%", drain, true);
  }

  // Короткая запись при буферах DMA — пачкой по 16, без DMA
  {
    reset(1.0, true);
    std::mt19937 rng(1);
    const Block block{0x090000, randomWords(10, rng)};
    check(ft_host::writeBlock(block.address, block.words.data(), block.words.size()), "короткая запись");
    check(memoryHas(block) && g_ft.dmaCalls == 0 && g_ft.burstCalls == 1, "короткая запись — не одной пачкой по 16");
  }

  // Регистр — по слову с ожиданием (у записи бывает действие), без пачек и DMA
  {
    reset(1.0, true);
    const uint32_t swap = 2;
    check(ft_host::writeBlock(0x302054, &swap, 1), "запись регистра");
    check(g_ft.mem[0x302054] == swap && g_ft.dmaCalls == 0 && g_ft.burstCalls == 0, "регистр — не по слову");
  }

  // Транзакция DMA не кончилась: блок повторяется медленно (writeBlock) и ложится верно
  {
    reset(1.0, true);
    g_ft.failDmaCall = 1;
    std::mt19937 rng(2);
    const Block block{0x0A0000, randomWords(3000, rng)};
    check(ft_host::writeBlock(block.address, block.words.data(), block.words.size()), "повтор после сбоя DMA");
    check(memoryHas(block), "после сбоя DMA память не та");
    check(g_ft.overflows == 0, "после сбоя DMA слово в полную очередь");
  }

  g_modelErrors.insert(g_modelErrors.end(), g_ft.errors.begin(), g_ft.errors.end());
  for (const std::string& error : g_modelErrors) {
    ++g_failures;
    printf("FAIL: model: %s\n", error.c_str());
  }
  if (g_failures == 0) {
    printf("OK\n");
    return 0;
  }
  return 1;
}
