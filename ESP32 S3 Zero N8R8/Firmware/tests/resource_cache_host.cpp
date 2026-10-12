// Стенд кэша ресурсов музыки на флешке (src/resource_disk_cache.cpp) на ПК.
//
// Каталог кэша — временная папка ПК: тот же переносимый код (stdio, stat, utime),
// что работает в прошивке поверх FatFs. Каждый сценарий печатает «ok имя» или
// причину неудачи и выход с кодом 1.

#include <direct.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utime.h>
#include <time.h>

#include <string>
#include <vector>

#include "zifi/resource_disk_cache.hpp"

using zifi::ResourceDiskCache;

namespace {

std::string g_dir;

// Время FAT: дата (год с 1980, месяц, число) в старших 16 битах, время — в младших.
uint32_t fatStamp(unsigned year, unsigned month, unsigned day, unsigned hour, unsigned minute,
                  unsigned second) {
  const uint32_t date = ((year - 1980) << 9) | (month << 5) | day;
  const uint32_t time = (hour << 11) | (minute << 5) | (second / 2);
  return time | (date << 16);
}

std::vector<uint8_t> pattern(size_t size, uint8_t seed) {
  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<uint8_t>(seed + i * 7 + (i >> 9));
  }
  return data;
}

std::string pathOf(const char* name) { return g_dir + "\\" + name; }

bool exists(const char* name) { return _access(pathOf(name).c_str(), 0) == 0; }

void removeAll() {
  for (const char* name : {"MIDI.SF2", "YRW801.ROM", "WRITING.TMP", "POWERKIT.MWK"}) {
    remove(pathOf(name).c_str());
  }
}

bool fail(const char* scenario, const char* why) {
  printf("FAIL %s: %s\n", scenario, why);
  return false;
}

bool storedCopyIsFreshAndReadsBack() {
  removeAll();
  const ResourceDiskCache disk(g_dir.c_str());
  const auto data = pattern(3 * 16384 + 123, 1);       // больше куска в 16 КБ, не кратно
  const uint32_t stamp = fatStamp(2026, 9, 20, 13, 45, 30);
  if (disk.fresh("MIDI.SF2", static_cast<uint32_t>(data.size()), stamp)) {
    return fail("stored_copy", "fresh before writing");
  }
  if (!disk.write("MIDI.SF2", data.data(), static_cast<uint32_t>(data.size()), stamp)) {
    return fail("stored_copy", "write failed");
  }
  if (!disk.fresh("MIDI.SF2", static_cast<uint32_t>(data.size()), stamp)) {
    return fail("stored_copy", "not fresh with its own stamp");
  }
  std::vector<uint8_t> back(data.size());
  if (!disk.read("MIDI.SF2", back.data(), static_cast<uint32_t>(back.size())) || back != data) {
    return fail("stored_copy", "read back differs");
  }
  if (exists(ResourceDiskCache::kTempName)) {
    return fail("stored_copy", "temporary file left");
  }
  return true;
}

bool newerCardFileMakesCopyStale() {
  const ResourceDiskCache disk(g_dir.c_str());
  const uint32_t size = 3 * 16384 + 123;
  // Копия лежит с временем 2026-09-20 13:45:30 (сценарий выше).
  if (disk.fresh("MIDI.SF2", size, fatStamp(2026, 9, 20, 13, 45, 32))) {
    return fail("newer_card_file", "card file 2 s newer, copy still fresh");
  }
  if (disk.fresh("MIDI.SF2", size, fatStamp(2026, 9, 21, 0, 0, 0))) {
    return fail("newer_card_file", "card file a day newer, copy still fresh");
  }
  if (!disk.fresh("MIDI.SF2", size, fatStamp(2026, 9, 20, 13, 45, 28))) {
    return fail("newer_card_file", "card file older, copy not fresh");
  }
  return true;
}

bool otherLengthIsStale() {
  const ResourceDiskCache disk(g_dir.c_str());
  const uint32_t stamp = fatStamp(2026, 9, 20, 13, 45, 30);
  if (disk.fresh("MIDI.SF2", 3 * 16384 + 124, stamp) || disk.fresh("MIDI.SF2", 1, stamp)) {
    return fail("other_length", "copy of another length is fresh");
  }
  if (disk.fresh("YRW801.ROM", 100, stamp)) {
    return fail("other_length", "missing file is fresh");
  }
  return true;
}

bool rewriteReplacesTheCopy() {
  const ResourceDiskCache disk(g_dir.c_str());
  const auto data = pattern(70000, 9);
  const uint32_t stamp = fatStamp(2026, 10, 2, 8, 0, 10);
  if (!disk.write("MIDI.SF2", data.data(), static_cast<uint32_t>(data.size()), stamp)) {
    return fail("rewrite", "write over the old copy failed");
  }
  std::vector<uint8_t> back(data.size());
  if (!disk.fresh("MIDI.SF2", static_cast<uint32_t>(data.size()), stamp) ||
      !disk.read("MIDI.SF2", back.data(), static_cast<uint32_t>(back.size())) || back != data) {
    return fail("rewrite", "new copy not in place");
  }
  return true;
}

bool shortOrLongReadFails() {
  const ResourceDiskCache disk(g_dir.c_str());
  std::vector<uint8_t> back(80000);
  if (disk.read("MIDI.SF2", back.data(), 69999) || disk.read("MIDI.SF2", back.data(), 70001)) {
    return fail("short_or_long_read", "read with a wrong length succeeded");
  }
  return true;
}

bool userCopyNewerIsUsed() {
  // Копия, положенная на флешку с ПК, новее файла на карте: годна (длина та же).
  const ResourceDiskCache disk(g_dir.c_str());
  const auto data = pattern(5000, 3);
  FILE* f = fopen(pathOf("YRW801.ROM").c_str(), "wb");
  fwrite(data.data(), 1, data.size(), f);
  fclose(f);
  const time_t when = ResourceDiskCache::timeOf(fatStamp(2026, 10, 5, 12, 0, 0));
  struct _utimbuf times = {when, when};
  _utime(pathOf("YRW801.ROM").c_str(), &times);
  if (!disk.fresh("YRW801.ROM", 5000, fatStamp(2026, 10, 4, 23, 59, 58))) {
    return fail("user_copy_newer", "newer copy of the same length is not fresh");
  }
  return true;
}

bool stampRoundTrip() {
  for (const uint32_t stamp : {fatStamp(1980, 1, 1, 0, 0, 0), fatStamp(2026, 12, 31, 23, 59, 58),
                               fatStamp(2026, 10, 25, 2, 30, 0), fatStamp(2107, 12, 31, 23, 59, 58)}) {
    if (ResourceDiskCache::stampOf(ResourceDiskCache::timeOf(stamp)) != stamp) {
      return fail("stamp_round_trip", "FAT stamp changed after time_t and back");
    }
  }
  return true;
}

bool kitsAreNotCached() {
  if (ResourceDiskCache::cacheable("POWERKIT.MWK") || ResourceDiskCache::cacheable("a.mwk")) {
    return fail("kits_not_cached", "MWK is cacheable");
  }
  if (!ResourceDiskCache::cacheable("MIDI.SF2") || !ResourceDiskCache::cacheable("YRW801.ROM") ||
      !ResourceDiskCache::cacheable("BANK")) {
    return fail("kits_not_cached", "SF2/ROM not cacheable");
  }
  if (ResourceDiskCache::cacheable("") || ResourceDiskCache::cacheable(nullptr)) {
    return fail("kits_not_cached", "empty name cacheable");
  }
  return true;
}

bool undatedCardFileKeepsWriteTime() {
  // Файл на карте без даты (время FAT 0): копия кладётся, её время — время записи,
  // и она годна (не старее). Прежде время ставилось из «даты» 0 — 30.11.1979.
  const ResourceDiskCache disk(g_dir.c_str());
  const auto data = pattern(1000, 5);
  if (!disk.write("YRW801.ROM", data.data(), 1000, 0)) {
    return fail("undated_card_file", "write failed");
  }
  struct _stat64 info;
  if (_stat64(pathOf("YRW801.ROM").c_str(), &info) != 0 || info.st_mtime < time(nullptr) - 600) {
    return fail("undated_card_file", "copy time is not the write time");
  }
  if (!disk.fresh("YRW801.ROM", 1000, 0)) {
    return fail("undated_card_file", "copy of an undated file is not fresh");
  }
  return true;
}

bool missingDirectoryFailsCleanly() {
  const ResourceDiskCache disk((g_dir + "\\NO_SUCH").c_str());
  const auto data = pattern(100, 1);
  if (disk.write("MIDI.SF2", data.data(), 100, fatStamp(2026, 10, 2, 0, 0, 0)) ||
      disk.fresh("MIDI.SF2", 100, 0)) {
    return fail("missing_directory", "write into a missing folder succeeded");
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: resource_cache_host <folder>\n");
    return 2;
  }
  g_dir = argv[1];
  _mkdir(g_dir.c_str());
  struct Scenario {
    const char* name;
    bool (*run)();
  };
  const Scenario scenarios[] = {
      {"stored_copy_is_fresh_and_reads_back", storedCopyIsFreshAndReadsBack},
      {"newer_card_file_makes_copy_stale", newerCardFileMakesCopyStale},
      {"other_length_is_stale", otherLengthIsStale},
      {"rewrite_replaces_the_copy", rewriteReplacesTheCopy},
      {"short_or_long_read_fails", shortOrLongReadFails},
      {"user_copy_newer_is_used", userCopyNewerIsUsed},
      {"stamp_round_trip", stampRoundTrip},
      {"kits_are_not_cached", kitsAreNotCached},
      {"undated_card_file_keeps_write_time", undatedCardFileKeepsWriteTime},
      {"missing_directory_fails_cleanly", missingDirectoryFailsCleanly},
  };
  for (const Scenario& scenario : scenarios) {
    if (!scenario.run()) {
      return 1;
    }
    printf("ok %s\n", scenario.name);
  }
  removeAll();
  return 0;
}
