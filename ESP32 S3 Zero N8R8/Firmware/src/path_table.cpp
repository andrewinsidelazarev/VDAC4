#include "zifi/path_table.hpp"

#include <stdio.h>
#include <string.h>

namespace zifi {

namespace {

bool hexValue(const char* text, size_t length, uint32_t& value) {
  if (length == 0 || length > 8) {
    return false;
  }
  uint32_t result = 0;
  for (size_t i = 0; i < length; ++i) {
    const char c = text[i];
    uint32_t digit;
    if (c >= '0' && c <= '9') {
      digit = static_cast<uint32_t>(c - '0');
    } else if (c >= 'A' && c <= 'F') {
      digit = static_cast<uint32_t>(c - 'A' + 10);
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<uint32_t>(c - 'a' + 10);
    } else {
      return false;
    }
    result = result << 4 | digit;
  }
  value = result;
  return true;
}

bool space(char c) { return c == ' ' || c == '\t'; }

}  // namespace

bool PathTable::normalizeName(const char* name, size_t length, char out[13]) {
  if (length == 0 || length > kMaxName) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    char c = name[i];
    if (c < 0x21 || c > 0x7E || c == ';' || c == '#') {
      return false;
    }
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
    out[i] = c;
  }
  out[length] = 0;
  return true;
}

void PathTable::parse(const char* text, size_t length) {
  count_ = 0;
  size_t at = 0;
  while (at < length) {
    size_t end = at;
    while (end < length && text[end] != '\n') {
      ++end;
    }
    // строка [at, end): ИМЯ СЕРИЙНЫЙ КЛАСТЕР
    const char* fields[3] = {};
    size_t sizes[3] = {};
    size_t found = 0;
    size_t i = at;
    while (i < end) {
      while (i < end && (space(text[i]) || text[i] == '\r')) {
        ++i;
      }
      if (i == end) {
        break;
      }
      if (found == 0 && (text[i] == ';' || text[i] == '#')) {
        break;
      }
      const size_t start = i;
      while (i < end && !space(text[i]) && text[i] != '\r') {
        ++i;
      }
      if (found < 3) {
        fields[found] = text + start;
        sizes[found] = i - start;
      }
      ++found;
    }
    PathEntry item;
    if (found == 3 && normalizeName(fields[0], sizes[0], item.name) &&
        hexValue(fields[1], sizes[1], item.serial) && hexValue(fields[2], sizes[2], item.cluster)) {
      set(item.name, item.serial, item.cluster);
    }
    at = end + 1;
  }
}

size_t PathTable::format(char* out, size_t size) const {
  size_t used = 0;
  int n = snprintf(out, size,
                   "; VDAC4: folders of Evo programs on SD cards (PATH_GET/PATH_SET).\n"
                   "; NAME, FAT32 volume serial, directory cluster (hex). A line may be deleted:\n"
                   "; the program then searches the card once and stores the folder again.\n");
  if (n < 0 || static_cast<size_t>(n) >= size) {
    return 0;
  }
  used = static_cast<size_t>(n);
  for (size_t i = 0; i < count_; ++i) {
    n = snprintf(out + used, size - used, "%-12s %08X %08X\n", entries_[i].name,
                 static_cast<unsigned>(entries_[i].serial),
                 static_cast<unsigned>(entries_[i].cluster));
    if (n < 0 || static_cast<size_t>(n) >= size - used) {
      return 0;
    }
    used += static_cast<size_t>(n);
  }
  return used;
}

int PathTable::indexOf(const char* name, uint32_t serial) const {
  for (size_t i = 0; i < count_; ++i) {
    if (entries_[i].serial == serial && strcmp(entries_[i].name, name) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void PathTable::removeAt(size_t index) {
  for (size_t i = index + 1; i < count_; ++i) {
    entries_[i - 1] = entries_[i];
  }
  --count_;
}

bool PathTable::find(const char* name, uint32_t serial, uint32_t& cluster) const {
  char key[13];
  if (!normalizeName(name, strlen(name), key)) {
    return false;
  }
  const int index = indexOf(key, serial);
  if (index < 0) {
    return false;
  }
  cluster = entries_[index].cluster;
  return true;
}

bool PathTable::set(const char* name, uint32_t serial, uint32_t cluster) {
  char key[13];
  if (!normalizeName(name, strlen(name), key)) {
    return false;
  }
  const int index = indexOf(key, serial);
  if (index >= 0) {
    removeAt(static_cast<size_t>(index));
  } else if (count_ == kMaxEntries) {
    removeAt(0);
  }
  PathEntry& item = entries_[count_++];
  memcpy(item.name, key, sizeof(item.name));
  item.serial = serial;
  item.cluster = cluster;
  return true;
}

}  // namespace zifi
