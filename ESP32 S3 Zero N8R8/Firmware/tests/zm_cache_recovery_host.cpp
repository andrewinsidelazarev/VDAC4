// Кэш модулей ESP (src/zm_cache.cpp) после обрыва питания — на ПК.
// Хранилище в памяти умеет длину файла и перечень (как LittleFsStorage); питание
// пропадает после k-й операции записи или удаления (запись файла в LittleFS —
// целиком или никак). После «перезагрузки» (новый ZmCache, load) каждая запись
// указателя читается с прежним содержимым, а файлов вне указателя нет. Вывод:
// «ok <сценарий>» на строку.
#include <stdio.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "zifi/zm_cache.hpp"

namespace {

using Files = std::map<std::string, std::vector<uint8_t>>;
int failures = 0;

class MemoryStorage : public zifi::ZmStorage {
 public:
  explicit MemoryStorage(size_t capacity) : capacity_(capacity) {}
  Files files;
  int budget = -1;                     // сколько ещё операций до обрыва; −1 — без обрыва
  bool cut = false;
  bool corrupt_reads = false;

  bool read(const char* file, uint8_t* data, size_t size) override {
    auto it = files.find(file);
    if (it == files.end() || it->second.size() != size || corrupt_reads) return false;
    memcpy(data, it->second.data(), size);
    return true;
  }
  bool write(const char* file, const uint8_t* data, size_t size) override {
    if (!step()) return false;
    files[file] = std::vector<uint8_t>(data, data + size);
    return true;
  }
  bool remove(const char* file) override {
    if (!step()) return false;
    return files.erase(file) != 0;
  }
  size_t capacityBytes() override { return capacity_; }
  long fileSize(const char* file) override {
    auto it = files.find(file);
    return it == files.end() ? kMissing : static_cast<long>(it->second.size());
  }
  bool list(void (*each)(const char* file, void* context), void* context) override {
    std::vector<std::string> names;
    for (auto& f : files) names.push_back(f.first);
    for (auto& n : names) each(n.c_str(), context);
    return true;
  }

 private:
  bool step() {
    if (cut) return false;
    if (budget == 0) {
      cut = true;
      return false;
    }
    if (budget > 0) --budget;
    return true;
  }
  size_t capacity_;
};

std::vector<uint8_t> module(char tag, size_t size) {
  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; ++i) data[i] = static_cast<uint8_t>(tag + i * 7);
  return data;
}

// Указатель и файлы согласованы: записи читаются, лишних файлов нет.
bool consistent(MemoryStorage& storage, const char* scenario, int k) {
  zifi::ZmCache cache(storage);
  cache.load();
  bool ok = true;
  size_t entries = 0;
  for (const char* name : {"PLAYER.ELF", "MP3.ELF", "MIDI.ELF", "OPL.ELF", "TRACKER.ELF"}) {
    const zifi::ZmCacheEntry* entry = cache.find(name);
    if (entry == nullptr) continue;
    ++entries;
    std::vector<uint8_t> data(entry->size);
    if (!cache.read(name, data.data(), entry->size) || data != module(name[0], entry->size)) {
      printf("FAIL %s k=%d: %s listed but not readable\n", scenario, k, name);
      ok = false;
    }
  }
  for (auto& f : storage.files) {
    if (f.first != "index.bin" && cache.find(f.first.c_str()) == nullptr) {
      printf("FAIL %s k=%d: orphan file %s\n", scenario, k, f.first.c_str());
      ok = false;
    }
  }
  if (entries + 1 != storage.files.size() && storage.files.count("index.bin") != 0) {
    printf("FAIL %s k=%d: %zu entries, %zu files\n", scenario, k, entries, storage.files.size());
    ok = false;
  }
  return ok;
}

// Три модуля в кэше на четыре блока по 4 КБ каждый (ёмкость — под три с запасом).
MemoryStorage prepared() {
  MemoryStorage storage(4 * 3 * zifi::ZmCache::kBlockBytes + 3 * zifi::ZmCache::kBlockBytes);
  zifi::ZmCache cache(storage);
  cache.load();
  cache.store("PLAYER.ELF", 1, module('P', 9000).data(), 9000, nullptr);
  cache.store("MP3.ELF", 2, module('M', 9000).data(), 9000, nullptr);
  cache.store("MIDI.ELF", 3, module('M', 9000).data(), 9000, nullptr);
  return storage;
}

void sweep(const char* scenario, void (*action)(zifi::ZmCache&)) {
  int k = 0;
  bool all = true;
  for (;; ++k) {
    MemoryStorage storage = prepared();
    storage.budget = k;
    {
      zifi::ZmCache cache(storage);
      cache.load();
      action(cache);
    }
    const bool finished = !storage.cut;
    storage.cut = false;
    storage.budget = -1;
    if (!consistent(storage, scenario, k)) all = false;
    if (finished) break;
  }
  if (all) printf("ok %s\n", scenario);
  else ++failures;
}

}  // namespace

int main() {
  // Новый модуль с вытеснением давно не бравшегося.
  sweep("power_cut_during_store_with_eviction", [](zifi::ZmCache& cache) {
    cache.store("OPL.ELF", 4, module('O', 9000).data(), 9000, "PLAYER.ELF");
  });
  // Новая версия того же имени.
  sweep("power_cut_during_replace", [](zifi::ZmCache& cache) {
    cache.store("MP3.ELF", 5, module('M', 9000).data(), 9000, "PLAYER.ELF");
  });
  // Модуль, который не читается, выбрасывается: MOD_QUERY снова ответит «шли».
  {
    MemoryStorage storage = prepared();
    zifi::ZmCache cache(storage);
    cache.load();
    storage.corrupt_reads = true;
    std::vector<uint8_t> data(9000);
    const bool read = cache.read("MP3.ELF", data.data(), 9000);
    storage.corrupt_reads = false;
    if (read || cache.has("MP3.ELF", 9000, 2) || storage.files.count("MP3.ELF") != 0) {
      printf("FAIL unreadable_module_is_dropped\n");
      ++failures;
    } else {
      printf("ok unreadable_module_is_dropped\n");
    }
  }
  // Указатель ссылается на пропавший файл и на файл другой длины; лишний файл лежит.
  {
    MemoryStorage storage = prepared();
    storage.files.erase("MP3.ELF");
    storage.files["MIDI.ELF"].resize(100);
    storage.files["STRAY.BIN"] = module('S', 10);
    zifi::ZmCache cache(storage);
    cache.load();
    if (cache.find("MP3.ELF") != nullptr || cache.find("MIDI.ELF") != nullptr ||
        cache.find("PLAYER.ELF") == nullptr || storage.files.count("STRAY.BIN") != 0 ||
        storage.files.count("MIDI.ELF") != 0) {
      printf("FAIL load_reconciles_index_with_files\n");
      ++failures;
    } else {
      printf("ok load_reconciles_index_with_files\n");
    }
  }
  return failures == 0 ? 0 : 1;
}
