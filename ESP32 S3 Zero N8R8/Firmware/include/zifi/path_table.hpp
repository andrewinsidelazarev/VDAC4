#pragma once

#include <stddef.h>
#include <stdint.h>

namespace zifi {

// Папки программ Evo на SD — path.cfg во flash ESP (/littlefs/path.cfg). Программа один раз
// находит свою папку поиском по карте (DOOM VDAC4 — LocateGame) и отдаёт её ESP (PATH_SET),
// дальше берёт оттуда (PATH_GET) и только проверяет. На карте ничего не пишется.
//
// Ключ — имя программы (до 12 знаков ASCII, без пробелов, регистр не важен) и серийный номер
// тома FAT32 (BS_VolID): у каждой карты своя запись. Значение — кластер каталога. Текст,
// строка на запись: «ИМЯ СЕРИЙНЫЙ КЛАСТЕР», числа шестнадцатеричные; пустые строки, строки
// с «;» или «#» в начале и непонятные пропускаются. Записей не больше kMaxEntries: новая
// вытесняет самую давнюю, обновлённая становится последней.
struct PathEntry {
  char name[13] = {};
  uint32_t serial = 0;
  uint32_t cluster = 0;
};

class PathTable {
 public:
  static constexpr size_t kMaxEntries = 64;
  static constexpr size_t kMaxName = 12;
  // Столько вмещает файл полной таблицы с заголовком.
  static constexpr size_t kMaxText = 256 + kMaxEntries * 32;

  // Имя в ключ: прописные, 1..12 знаков 0x21..0x7E без «;» и «#». false — не годится.
  static bool normalizeName(const char* name, size_t length, char out[13]);

  void clear() { count_ = 0; }
  void parse(const char* text, size_t length);
  // Текст файла; 0 — не влез в size.
  size_t format(char* out, size_t size) const;
  bool find(const char* name, uint32_t serial, uint32_t& cluster) const;
  bool set(const char* name, uint32_t serial, uint32_t cluster);
  size_t count() const { return count_; }
  const PathEntry& entry(size_t index) const { return entries_[index]; }

 private:
  int indexOf(const char* name, uint32_t serial) const;
  void removeAt(size_t index);

  PathEntry entries_[kMaxEntries];
  size_t count_ = 0;
};

}  // namespace zifi
