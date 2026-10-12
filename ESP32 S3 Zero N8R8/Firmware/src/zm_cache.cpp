#include "zifi/zm_cache.hpp"

#include <string.h>

namespace zifi {
namespace {

// Указатель кэша на диске: [магия][часы][записи].
struct IndexFile {
  uint32_t magic;
  uint32_t clock;
  ZmCacheEntry entries[ZmCache::kMaxEntries];
};

bool nameChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '~' ||
         c == '-';
}

}  // namespace

constexpr char ZmCache::kIndex[];

bool ZmCache::validName(const char* name) {
  if (name == nullptr) {
    return false;
  }
  size_t stem = 0;
  size_t extension = 0;
  bool dot = false;
  for (const char* c = name; *c != 0; ++c) {
    if (*c == '.') {
      if (dot || stem == 0) {
        return false;
      }
      dot = true;
    } else if (!nameChar(*c)) {
      return false;
    } else if (dot) {
      ++extension;
    } else {
      ++stem;
    }
  }
  return stem >= 1 && stem <= 8 && extension <= 3 && (!dot || extension >= 1);
}

void ZmCache::load() {
  memset(entries_, 0, sizeof(entries_));
  clock_ = 0;
  IndexFile index;
  if (!storage_.read(kIndex, reinterpret_cast<uint8_t*>(&index), sizeof(index)) ||
      index.magic != kMagic) {
    return;
  }
  clock_ = index.clock;
  bool changed = false;
  for (size_t i = 0; i < kMaxEntries; ++i) {
    ZmCacheEntry entry = index.entries[i];
    entry.name[sizeof(entry.name) - 1] = 0;
    if (entry.name[0] == 0 || !validName(entry.name)) {
      continue;
    }
    // Файл пропал или другой длины (питание пропало посреди записи прежней версии) —
    // записи нет: MOD_QUERY ответит «шли», а не «есть» модуль, который не загрузится.
    const long size = storage_.fileSize(entry.name);
    if (size == ZmStorage::kMissing || (size >= 0 && static_cast<unsigned long>(size) != entry.size)) {
      changed = true;
      continue;
    }
    entries_[i] = entry;
  }
  if (changed) {
    saveIndex();
  }
  // Файлы, которых в указателе нет (оборванная запись нового, вытеснение после
  // сохранения указателя), только занимают место.
  struct Sweep {
    ZmCache* cache;
    char names[kMaxEntries][13];
    size_t count;
  } sweep = {this, {}, 0};
  storage_.list(
      [](const char* file, void* context) {
        Sweep* s = static_cast<Sweep*>(context);
        if (strcmp(file, kIndex) != 0 && s->cache->find(file) == nullptr && s->count < kMaxEntries &&
            strlen(file) < sizeof(s->names[0])) {
          strcpy(s->names[s->count++], file);
        }
      },
      &sweep);
  for (size_t i = 0; i < sweep.count; ++i) {
    storage_.remove(sweep.names[i]);
  }
}

bool ZmCache::saveIndex() {
  IndexFile index;
  memset(&index, 0, sizeof(index));
  index.magic = kMagic;
  index.clock = clock_;
  memcpy(index.entries, entries_, sizeof(entries_));
  return storage_.write(kIndex, reinterpret_cast<const uint8_t*>(&index), sizeof(index));
}

const ZmCacheEntry* ZmCache::find(const char* name) const {
  if (name == nullptr) {
    return nullptr;
  }
  for (const ZmCacheEntry& entry : entries_) {
    if (entry.name[0] != 0 && strcmp(entry.name, name) == 0) {
      return &entry;
    }
  }
  return nullptr;
}

ZmCacheEntry* ZmCache::slot(const char* name) {
  return const_cast<ZmCacheEntry*>(find(name));
}

void ZmCache::drop(ZmCacheEntry* entry) {
  char name[sizeof(entry->name)];
  memcpy(name, entry->name, sizeof(name));
  memset(entry, 0, sizeof(*entry));
  saveIndex();
  storage_.remove(name);
}

bool ZmCache::has(const char* name, uint32_t size, uint32_t stamp) const {
  const ZmCacheEntry* entry = find(name);
  return entry != nullptr && entry->size == size && entry->stamp == stamp;
}

size_t ZmCache::footprint(size_t size) {
  return (size + kBlockBytes - 1) / kBlockBytes * kBlockBytes + kBlockBytes;
}

size_t ZmCache::freeBytes() const {
  size_t used = footprint(sizeof(IndexFile));
  for (const ZmCacheEntry& entry : entries_) {
    if (entry.name[0] != 0) {
      used += footprint(entry.size);
    }
  }
  const size_t capacity = storage_.capacityBytes();
  return capacity > used ? capacity - used : 0;
}

bool ZmCache::canFit(uint32_t size, const char* busy) const {
  size_t kept = footprint(sizeof(IndexFile));
  const ZmCacheEntry* held = busy != nullptr ? find(busy) : nullptr;
  if (held != nullptr) {
    kept += footprint(held->size);
  }
  const size_t capacity = storage_.capacityBytes();
  return capacity > kept && capacity - kept >= footprint(size);
}

size_t ZmCache::count() const {
  size_t total = 0;
  for (const ZmCacheEntry& entry : entries_) {
    if (entry.name[0] != 0) {
      ++total;
    }
  }
  return total;
}

// Вытеснить самый давно не бравшийся модуль, кроме busy.
bool ZmCache::evictOne(const char* busy) {
  ZmCacheEntry* oldest = nullptr;
  for (ZmCacheEntry& entry : entries_) {
    if (entry.name[0] == 0 || (busy != nullptr && strcmp(entry.name, busy) == 0)) {
      continue;
    }
    if (oldest == nullptr || entry.used < oldest->used) {
      oldest = &entry;
    }
  }
  if (oldest == nullptr) {
    return false;
  }
  drop(oldest);
  return true;
}

bool ZmCache::store(const char* name, uint32_t stamp, const uint8_t* data, uint32_t size,
                    const char* busy) {
  if (!validName(name) || data == nullptr || size == 0) {
    return false;
  }
  // Прежняя копия того же имени больше не нужна при любом исходе.
  if (ZmCacheEntry* old = slot(name)) {
    drop(old);
  }
  while (freeBytes() < footprint(size)) {
    if (!evictOne(busy)) {
      saveIndex();
      return false;
    }
  }
  ZmCacheEntry* free = nullptr;
  for (ZmCacheEntry& entry : entries_) {
    if (entry.name[0] == 0) {
      free = &entry;
      break;
    }
  }
  if (free == nullptr) {
    if (!evictOne(busy)) {
      saveIndex();
      return false;
    }
    for (ZmCacheEntry& entry : entries_) {
      if (entry.name[0] == 0) {
        free = &entry;
        break;
      }
    }
  }
  if (!storage_.write(name, data, size)) {
    storage_.remove(name);
    saveIndex();
    return false;
  }
  memset(free, 0, sizeof(*free));
  strncpy(free->name, name, sizeof(free->name) - 1);
  free->size = size;
  free->stamp = stamp;
  free->used = ++clock_;
  return saveIndex();
}

bool ZmCache::read(const char* name, uint8_t* data, uint32_t size) {
  ZmCacheEntry* entry = slot(name);
  if (entry == nullptr || entry->size != size) {
    return false;
  }
  if (!storage_.read(name, data, size)) {
    drop(entry);                         // не читается — прислать заново
    return false;
  }
  entry->used = ++clock_;
  saveIndex();
  return true;
}

}  // namespace zifi
