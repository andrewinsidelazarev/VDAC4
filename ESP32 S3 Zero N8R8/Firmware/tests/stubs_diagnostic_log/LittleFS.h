#pragma once
// Модель LittleFS для стенда журнала: файлы — в памяти, каждая операция считается. По счётчикам стенд
// проверяет главное: в обычной работе журнал во flash не пишет и не читает.
#include <stddef.h>
#include <stdint.h>
#include <map>
#include <string>

struct FlashModel {
  std::map<std::string, std::string> files;
  int writeOpens = 0;   // open(..., "w")
  int readOpens = 0;    // open(..., "r")
  int removes = 0;
  int exists = 0;
};
extern FlashModel g_flash;

class File {
 public:
  File() = default;
  File(const std::string& path, bool write) : path_(path), write_(write), valid_(true) {}
  explicit operator bool() const { return valid_; }
  size_t write(const uint8_t* data, size_t length) {
    g_flash.files[path_].append(reinterpret_cast<const char*>(data), length);
    return length;
  }
  size_t read(uint8_t* data, size_t length) {
    const std::string& text = g_flash.files[path_];
    const size_t n = length < text.size() - at_ ? length : text.size() - at_;
    text.copy(reinterpret_cast<char*>(data), n, at_);
    at_ += n;
    return n;
  }
  size_t size() const { return g_flash.files[path_].size(); }
  void close() { valid_ = false; }

 private:
  std::string path_;
  bool write_ = false;
  bool valid_ = false;
  size_t at_ = 0;
};

struct LittleFSModel {
  File open(const char* path, const char* mode) {
    const bool write = mode[0] == 'w';
    if (write) {
      ++g_flash.writeOpens;
      g_flash.files[path].clear();
      return File(path, true);
    }
    ++g_flash.readOpens;
    if (g_flash.files.count(path) == 0) return File();
    return File(path, false);
  }
  bool exists(const char* path) { ++g_flash.exists; return g_flash.files.count(path) != 0; }
  bool remove(const char* path) { ++g_flash.removes; return g_flash.files.erase(path) != 0; }
};
extern LittleFSModel LittleFS;
