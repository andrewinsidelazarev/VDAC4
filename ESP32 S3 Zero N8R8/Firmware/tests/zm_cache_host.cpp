// Стенд кэша модулей ESP (src/zm_cache.cpp) на ПК (test_zm_cache_host.py):
// хранилище в памяти с заданной ёмкостью и отказом записи по заказу. Каждый
// сценарий печатает строку «ok <имя>» или «FAIL <имя>: <что не так>»; код
// возврата — число провалов.
#include <stdio.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "zifi/zm_cache.hpp"

using zifi::ZmCache;

namespace {

class MemoryStorage : public zifi::ZmStorage {
 public:
  explicit MemoryStorage(size_t capacity) : capacity_(capacity) {}
  bool read(const char* file, uint8_t* data, size_t size) override {
    auto found = files_.find(file);
    if (found == files_.end() || found->second.size() != size) {
      return false;
    }
    memcpy(data, found->second.data(), size);
    return true;
  }
  bool write(const char* file, const uint8_t* data, size_t size) override {
    if (failWrites_) {
      return false;
    }
    files_[file].assign(data, data + size);
    return true;
  }
  bool remove(const char* file) override { return files_.erase(file) != 0; }
  size_t capacityBytes() override { return capacity_; }

  bool exists(const char* file) const { return files_.count(file) != 0; }
  size_t moduleFiles() const { return files_.size() - (exists("index.bin") ? 1 : 0); }
  bool failWrites_ = false;

 private:
  size_t capacity_;
  std::map<std::string, std::vector<uint8_t>> files_;
};

int failures = 0;
std::string scenario;
bool scenarioOk = true;

void check(bool condition, const char* what) {
  if (!condition && scenarioOk) {
    printf("FAIL %s: %s\n", scenario.c_str(), what);
    scenarioOk = false;
    ++failures;
  }
}

void begin(const char* name) {
  scenario = name;
  scenarioOk = true;
}

void end() {
  if (scenarioOk) {
    printf("ok %s\n", scenario.c_str());
  }
}

std::vector<uint8_t> module(size_t size, uint8_t seed) {
  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<uint8_t>(seed + i * 7);
  }
  return data;
}

constexpr size_t kBlock = ZmCache::kBlockBytes;
// Указатель: 8 байт заголовка и 24 записи по 28 байт — один блок данных и блок метаданных.
constexpr size_t kIndexFootprint = 2 * kBlock;

}  // namespace

int main() {
  begin("footprint_counts_whole_blocks");
  check(ZmCache::footprint(1) == 2 * kBlock, "1 byte");
  check(ZmCache::footprint(kBlock) == 2 * kBlock, "one block");
  check(ZmCache::footprint(kBlock + 1) == 3 * kBlock, "one block and a byte");
  check(ZmCache::footprint(53112) == 14 * kBlock, "TRACKER.ELF");
  end();

  begin("empty_cache_has_its_capacity");
  {
    MemoryStorage storage(1664 * 1024);
    ZmCache cache(storage);
    cache.load();
    check(cache.count() == 0, "count");
    check(cache.freeBytes() == 1664 * 1024 - kIndexFootprint, "free");
    check(cache.canFit(53112, nullptr), "TRACKER.ELF fits");
  }
  end();

  begin("modules_are_stored_read_and_survive_reload");
  {
    MemoryStorage storage(1664 * 1024);
    ZmCache cache(storage);
    cache.load();
    const auto player = module(7004, 1);
    const auto mp3 = module(39344, 2);
    check(cache.store("PLAYER.ELF", 0x5D439C2B, player.data(), 7004, nullptr), "store player");
    check(cache.store("MP3.ELF", 0x5D439C2B, mp3.data(), 39344, "PLAYER.ELF"), "store mp3");
    check(cache.has("PLAYER.ELF", 7004, 0x5D439C2B), "has player");
    check(!cache.has("PLAYER.ELF", 7004, 0x5D439C2C), "other time is a miss");
    check(!cache.has("PLAYER.ELF", 7005, 0x5D439C2B), "other size is a miss");
    check(cache.freeBytes() ==
              1664 * 1024 - kIndexFootprint - ZmCache::footprint(7004) - ZmCache::footprint(39344),
          "free after two");
    ZmCache again(storage);
    again.load();
    check(again.count() == 2, "reload count");
    std::vector<uint8_t> back(39344);
    check(again.read("MP3.ELF", back.data(), 39344) && back == mp3, "read back");
    check(!again.read("MP3.ELF", back.data(), 39343), "wrong size refused");
  }
  end();

  begin("same_name_replaces_the_old_copy");
  {
    MemoryStorage storage(1664 * 1024);
    ZmCache cache(storage);
    cache.load();
    const auto first = module(5000, 3);
    const auto second = module(6000, 4);
    cache.store("MP3.ELF", 1, first.data(), 5000, nullptr);
    check(cache.store("MP3.ELF", 2, second.data(), 6000, nullptr), "store");
    check(cache.count() == 1, "one entry");
    check(cache.has("MP3.ELF", 6000, 2) && !cache.has("MP3.ELF", 5000, 1), "new one");
  }
  end();

  begin("least_recently_used_goes_first_busy_stays");
  {
    // Место ровно под три модуля по 40 КБ.
    const size_t capacity = kIndexFootprint + 3 * ZmCache::footprint(40000);
    MemoryStorage storage(capacity);
    ZmCache cache(storage);
    cache.load();
    const auto data = module(40000, 5);
    cache.store("PLAYER.ELF", 1, data.data(), 40000, nullptr);
    cache.store("A.ELF", 1, data.data(), 40000, "PLAYER.ELF");
    cache.store("B.ELF", 1, data.data(), 40000, "PLAYER.ELF");
    std::vector<uint8_t> back(40000);
    cache.read("A.ELF", back.data(), 40000);              // A брали недавно
    check(cache.store("C.ELF", 1, data.data(), 40000, "PLAYER.ELF"), "store C");
    check(cache.find("PLAYER.ELF") != nullptr, "busy app kept although oldest");
    check(cache.find("A.ELF") != nullptr, "recently read kept");
    check(cache.find("B.ELF") == nullptr && !storage.exists("B.ELF"), "B evicted");
    check(storage.moduleFiles() == 3, "files on storage");
  }
  end();

  begin("too_big_is_refused_without_evicting");
  {
    const size_t capacity = kIndexFootprint + 2 * ZmCache::footprint(40000);
    MemoryStorage storage(capacity);
    ZmCache cache(storage);
    cache.load();
    const auto data = module(90000, 6);
    cache.store("PLAYER.ELF", 1, data.data(), 40000, nullptr);
    check(!cache.canFit(90000, "PLAYER.ELF"), "canFit says no");
    check(cache.canFit(40000, "PLAYER.ELF"), "a smaller one would fit");
    check(!cache.store("BIG.ELF", 1, data.data(), 90000, "PLAYER.ELF"), "store refused");
    check(cache.find("PLAYER.ELF") != nullptr, "busy app still there");
  }
  end();

  begin("failed_write_leaves_no_entry");
  {
    MemoryStorage storage(1664 * 1024);
    ZmCache cache(storage);
    cache.load();
    const auto data = module(3000, 7);
    storage.failWrites_ = true;
    check(!cache.store("MP3.ELF", 1, data.data(), 3000, nullptr), "store refused");
    check(cache.find("MP3.ELF") == nullptr, "no entry");
    storage.failWrites_ = false;
    check(cache.store("MP3.ELF", 1, data.data(), 3000, nullptr), "works again");
  }
  end();

  begin("names_are_8_3");
  check(ZmCache::validName("PLAYER.ELF"), "PLAYER.ELF");
  check(ZmCache::validName("TIMGM6MB.SF2"), "TIMGM6MB.SF2");
  check(!ZmCache::validName("player.elf"), "lower case");
  check(!ZmCache::validName("TOOLONGNAME.ELF"), "9 letters");
  check(!ZmCache::validName("A.ELFX"), "4-letter extension");
  check(!ZmCache::validName("../X.ELF"), "path");
  check(!ZmCache::validName(".ELF"), "no stem");
  end();

  begin("no_capacity_no_store");
  {
    MemoryStorage storage(0);
    ZmCache cache(storage);
    cache.load();
    const auto data = module(100, 8);
    check(cache.freeBytes() == 0, "free 0");
    check(!cache.canFit(100, nullptr), "canFit");
    check(!cache.store("A.ELF", 1, data.data(), 100, nullptr), "store refused");
  }
  end();

  return failures;
}
