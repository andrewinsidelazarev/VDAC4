#include "zifi/resource_disk_cache.hpp"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <sys/utime.h>
#include <io.h>
#else
#include <unistd.h>
#include <utime.h>
#endif

namespace zifi {

namespace {

// Флешка читается и пишется кусками: FatFs отдаёт драйверу USB до кластера за раз
// прямо из буфера вызывающего, а драйвер копирует передачу через свой буфер во
// внутренней памяти и растит его под размер запроса. Кластер exFAT бывает 128 КБ и
// больше — столько внутренней памяти у ESP32-S3 свободной может не быть.
constexpr uint32_t kChunk = 16 * 1024;

#ifdef _WIN32
// Программа ESP для Unreal и тест собираются MSVC: те же вызовы под своими именами.
int statFile(const char* path, long long& size, time_t& when) {
  struct _stat64 info;
  if (_stat64(path, &info) != 0) {
    return -1;
  }
  size = info.st_size;
  when = info.st_mtime;
  return 0;
}
int setFileTime(const char* path, time_t when) {
  struct __utimbuf64 times = {when, when};
  return _utime64(path, &times);
}
void localTime(const time_t* when, struct tm* out) { localtime_s(out, when); }
#else
int statFile(const char* path, long long& size, time_t& when) {
  struct stat info;
  if (stat(path, &info) != 0) {
    return -1;
  }
  size = static_cast<long long>(info.st_size);
  when = info.st_mtime;
  return 0;
}
int setFileTime(const char* path, time_t when) {
  struct utimbuf times = {when, when};
  return utime(path, &times);
}
void localTime(const time_t* when, struct tm* out) { localtime_r(when, out); }
#endif

bool sameText(const char* a, const char* b) {
  for (;; ++a, ++b) {
    char x = *a;
    char y = *b;
    if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 'a' + 'A');
    if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 'a' + 'A');
    if (x != y) {
      return false;
    }
    if (x == 0) {
      return true;
    }
  }
}

}  // namespace

ResourceDiskCache::ResourceDiskCache(const char* directory) {
  snprintf(directory_, sizeof(directory_), "%s", directory != nullptr ? directory : "");
}

bool ResourceDiskCache::cacheable(const char* name) {
  if (name == nullptr || *name == 0) {
    return false;
  }
  const char* dot = strrchr(name, '.');
  return dot == nullptr || !sameText(dot, ".MWK");
}

void ResourceDiskCache::pathOf(const char* name, char* path, size_t size) const {
  snprintf(path, size, "%s/%s", directory_, name);
}

time_t ResourceDiskCache::timeOf(uint32_t stamp) {
  const uint16_t time = static_cast<uint16_t>(stamp & 0xFFFFu);
  const uint16_t date = static_cast<uint16_t>(stamp >> 16);
  struct tm fields = {};
  fields.tm_year = ((date >> 9) & 0x7F) + 80;
  fields.tm_mon = ((date >> 5) & 0x0F) - 1;
  fields.tm_mday = date & 0x1F;
  fields.tm_hour = (time >> 11) & 0x1F;
  fields.tm_min = (time >> 5) & 0x3F;
  fields.tm_sec = (time & 0x1F) * 2;
  fields.tm_isdst = -1;                  // как vfs_fat_stat: пояс решает mktime
  return mktime(&fields);
}

uint32_t ResourceDiskCache::stampOf(time_t when) {
  struct tm fields = {};
  localTime(&when, &fields);
  if (fields.tm_year < 80) {
    return 0;                            // раньше 1980 года FAT не умеет
  }
  const uint32_t date = static_cast<uint32_t>(((fields.tm_year - 80) << 9) |
                                              ((fields.tm_mon + 1) << 5) | fields.tm_mday);
  const uint32_t time = static_cast<uint32_t>((fields.tm_hour << 11) | (fields.tm_min << 5) |
                                              (fields.tm_sec / 2));
  return time | (date << 16);
}

bool ResourceDiskCache::fresh(const char* name, uint32_t size, uint32_t stamp) const {
  char path[300];
  pathOf(name, path, sizeof(path));
  long long length = 0;
  time_t when = 0;
  if (statFile(path, length, when) != 0 || length != static_cast<long long>(size)) {
    return false;
  }
  return stampOf(when) >= stamp;
}

bool ResourceDiskCache::read(const char* name, uint8_t* data, uint32_t size) const {
  char path[300];
  pathOf(name, path, sizeof(path));
  FILE* input = fopen(path, "rb");
  if (input == nullptr) {
    return false;
  }
  uint32_t done = 0;
  while (done < size) {
    const uint32_t part = size - done < kChunk ? size - done : kChunk;
    if (fread(data + done, 1, part, input) != part) {
      break;
    }
    done += part;
  }
  const int extra = done == size ? fgetc(input) : 0;
  fclose(input);
  return done == size && extra == EOF;
}

bool ResourceDiskCache::write(const char* name, const uint8_t* data, uint32_t size,
                              uint32_t stamp) const {
  char temp[300];
  char path[300];
  pathOf(kTempName, temp, sizeof(temp));
  pathOf(name, path, sizeof(path));
  FILE* output = fopen(temp, "wb");
  if (output == nullptr) {
    return false;
  }
  uint32_t done = 0;
  while (done < size) {
    const uint32_t part = size - done < kChunk ? size - done : kChunk;
    if (fwrite(data + done, 1, part, output) != part) {
      break;
    }
    done += part;
  }
  const bool closed = fclose(output) == 0;
  if (done != size || !closed) {
    remove(temp);
    return false;
  }
  // FatFs не переименовывает поверх существующего файла: прежняя копия уходит.
  remove(path);
  if (rename(temp, path) != 0) {
    remove(temp);
    return false;
  }
  // Дата FAT без месяца или числа (у файлов, записанных без времени) негодна: время
  // копии тогда остаётся временем записи — она и так не старее файла на карте.
  const uint32_t date = stamp >> 16;
  if (((date >> 5) & 0x0F) == 0 || (date & 0x1F) == 0) {
    return true;
  }
  return setFileTime(path, timeOf(stamp)) == 0;
}

}  // namespace zifi
