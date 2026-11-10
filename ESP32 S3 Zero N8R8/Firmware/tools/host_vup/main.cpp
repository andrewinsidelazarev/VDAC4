// Стенд обновлятора платы VDAC4 (VDAC4 Online Update 2) на ПК.
//
// НАСТОЯЩИЕ Vdac4Updater, VfsFileStore, VfsBridge, VfsClient и двоичный протокол — те же
// файлы, что в прошивке. Подменены края: вместо GitHub — папка с ответами API (_ref.json,
// _tree.json) и файлами репозитория; вместо платы — модель в папке «tang»; вместо Z80 — эмулятор
// Wild Commander поверх папки карты Evo (tools/host_smb/z80_sim).
//
// host_vup.exe <evo|-> <github> <tang> <check|apply|auto|sync>
//   evo «-» — без карты Evo (START без бита 0);
//   check — только сверка; apply — поставить всё, что можно; auto — отмеченное по умолчанию;
//   sync — сверка и повтор списка.
// Папка tang:
//   board.ini — строки ключ=значение: version (VERSION, hex), cpinfo (CP_INFO, hex), esp (версия
//               прошивки ESP), link (0 — LINK не работает), flash (0 — моста flash нет),
//               sd (0 — карта Tang не готова), held (1 — карта отдана WC);
//   esp.bin   — работающий раздел приложения ESP; flash.bin — flash Tang (слоты по 1 МБ);
//   sd/       — карта Tang.
// Итоги установки: esp_ota.bin (новый образ ESP), flash.bin, sd/, bl616.bin; печатаются строки
//   STATE <этап> <текущий> <всего> <процент> <текст>
//   ENTRY <номер> <состояние> <флаги> <вид>\t<текущее>\t<доступное>\t<имя>
//   HTTP <узел> <путь>, ESPINSTALL, ESPRESTART, BL616 <CP_INFO>, FLASH <адрес> <длина>,
//   TANGSD <путь> <длина>, RESULT <0|1> <ошибка>.
// Помехи (переменные среды):
//   ZIFI_VUP_CORRUPT=<подстрока пути> — первое скачивание испорчено (ZIFI_VUP_CORRUPT_TIMES=N);
//   ZIFI_VUP_API_STATUS=<код> — ответ API GitHub (403 — лимит);
//   ZIFI_VUP_FAIL_SLOT=<слот> — запись слота не проходит сверку;
//   ZIFI_VUP_FAIL_SD=<подстрока> — запись на карту Tang не проходит;
//   ZIFI_VUP_STOP_AFTER=<N> — STOP после N-го события строки.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <timeapi.h>

#include "host_serial_link.hpp"
#include "zifi/vdac4_updater.hpp"
#include "zifi/vfs_bridge.hpp"
#include "zifi/z80_transport.hpp"

#include "z80_sim.hpp"

#pragma comment(lib, "winmm.lib")

HardwareSerial Serial;
HardwareSerial Serial0;
EspClass ESP;
WiFiClassHost WiFi;
LittleFSClass LittleFS;

namespace fs = std::filesystem;

namespace {

std::mutex printLock;
zifi::Vdac4Updater* g_updater = nullptr;
int g_stopAfter = -1;
std::atomic<int> g_entries{0};

void printLine(const std::string& line) {
  std::lock_guard<std::mutex> guard(printLock);
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

uint32_t readLe(const uint8_t* data, int bytes) {
  uint32_t value = 0;
  for (int index = bytes - 1; index >= 0; --index) {
    value = value << 8 | data[index];
  }
  return value;
}

bool printEvent(void*, uint8_t command, const uint8_t* data, uint16_t length) {
  char head[96];
  if (length > 63) {
    std::snprintf(head, sizeof(head), "TOOLONG %02X %u", command, length);
    printLine(head);
  }
  if (command == 0x67 && length >= 7) {
    std::snprintf(head, sizeof(head), "STATE %u %u %u %u ", data[0],
                  static_cast<unsigned>(readLe(data + 1, 2)),
                  static_cast<unsigned>(readLe(data + 3, 2)), data[5]);
    printLine(head + std::string(reinterpret_cast<const char*>(data + 6)));
    return true;
  }
  if (command == 0x68 && length >= 7) {
    std::snprintf(head, sizeof(head), "ENTRY %u %u %u %u", data[0], data[1], data[2], data[3]);
    std::string line = head;
    size_t at = 4;
    for (int part = 0; part < 3; ++part) {
      const char* text = reinterpret_cast<const char*>(data + at);
      const size_t n = strnlen(text, length - at);
      line += '\t';
      line.append(text, n);
      at += n + 1;
      if (at > length) {
        printLine("BADENTRY");
        return true;
      }
    }
    printLine(line);
    if (g_stopAfter >= 0 && ++g_entries == g_stopAfter && g_updater != nullptr) {
      printLine("STOP");
      g_updater->requestStop();
    }
    return true;
  }
  std::snprintf(head, sizeof(head), "EVENT %02X %u", command, length);
  printLine(head);
  return true;
}

std::string decodePercent(const std::string& text) {
  std::string output;
  for (size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '%' && index + 2 < text.size()) {
      output.push_back(static_cast<char>(std::strtoul(text.substr(index + 1, 2).c_str(), nullptr, 16)));
      index += 2;
    } else {
      output.push_back(text[index]);
    }
  }
  return output;
}

bool readFile(const fs::path& path, std::vector<uint8_t>& data) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return false;
  }
  data.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  return true;
}

bool writeFile(const fs::path& path, const uint8_t* data, size_t size) {
  std::error_code ignored;
  fs::create_directories(path.parent_path(), ignored);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
  return static_cast<bool>(output);
}

// Файл репозитория по пути raw: /<владелец>/<репозиторий>/<коммит>/<путь>
std::string rawFile(const std::string& root, const char* path) {
  const std::string text(path);
  size_t slash = 0;
  for (int part = 0; part < 4 && slash != std::string::npos; ++part) {
    slash = text.find('/', slash + (part == 0 ? 0 : 1));
  }
  return slash == std::string::npos ? std::string() : root + "/" + decodePercent(text.substr(slash + 1));
}

// --- SHA-256 (FIPS 180-4) для модели ESP: на ПК нет mbedTLS ----------------------------------
class Sha256 {
 public:
  Sha256() {
    static const uint32_t kInit[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(h_, kInit, sizeof(h_));
  }
  void update(const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
      block_[used_++] = data[i];
      if (used_ == 64) {
        compress();
        used_ = 0;
      }
    }
    total_ += length;
  }
  void finish(uint8_t digest[32]) {
    const uint64_t bits = total_ * 8;
    const uint8_t one = 0x80;
    update(&one, 1);
    const uint8_t zero = 0;
    while (used_ != 56) {
      update(&zero, 1);
    }
    uint8_t length[8];
    for (int i = 0; i < 8; ++i) {
      length[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    }
    update(length, 8);
    for (int i = 0; i < 8; ++i) {
      for (int j = 0; j < 4; ++j) {
        digest[i * 4 + j] = static_cast<uint8_t>(h_[i] >> (24 - 8 * j));
      }
    }
  }

 private:
  static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void compress() {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = (uint32_t(block_[4 * i]) << 24) | (uint32_t(block_[4 * i + 1]) << 16) |
             (uint32_t(block_[4 * i + 2]) << 8) | block_[4 * i + 3];
    }
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6],
             h = h_[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) +
                          k[i] + w[i];
      const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
  }

  uint32_t h_[8];
  uint8_t block_[64] = {};
  size_t used_ = 0;
  uint64_t total_ = 0;
};

// «GitHub» из папки: ответы API лежат файлами, raw-запрос отдаёт файл репозитория.
class FolderFetcher : public zifi::WcFetcher {
 public:
  explicit FolderFetcher(std::string root) : root_(std::move(root)) {
    const char* corrupt = std::getenv("ZIFI_VUP_CORRUPT");
    corrupt_ = corrupt == nullptr ? "" : corrupt;
    const char* times = std::getenv("ZIFI_VUP_CORRUPT_TIMES");
    corruptTimes_ = times == nullptr ? 1 : std::atoi(times);
    const char* api = std::getenv("ZIFI_VUP_API_STATUS");
    apiStatus_ = api == nullptr ? 0 : static_cast<uint16_t>(std::atoi(api));
  }

  const std::string& root() const { return root_; }

  bool get(const char* host, const char* path, uint8_t* buffer, size_t capacity, size_t& length,
           uint16_t& status, char* error, size_t errorSize) override {
    printLine(std::string("HTTP ") + host + " " + path);
    length = 0;
    std::string file;
    if (std::strcmp(host, "api.github.com") == 0) {
      if (apiStatus_ != 0) {
        status = apiStatus_;
        return true;
      }
      if (std::strstr(path, "/git/ref/heads/") != nullptr) {
        file = root_ + "/_ref.json";
      } else if (std::strstr(path, "/git/trees/") != nullptr) {
        file = root_ + "/_tree.json";
      }
    } else if (std::strcmp(host, "raw.githubusercontent.com") == 0) {
      file = rawFile(root_, path);
    }
    std::FILE* input = nullptr;
    if (file.empty() || fopen_s(&input, file.c_str(), "rb") != 0 || input == nullptr) {
      status = 404;
      return true;
    }
    length = std::fread(buffer, 1, capacity, input);
    const bool longer = std::fgetc(input) != EOF;
    std::fclose(input);
    if (longer) {
      std::snprintf(error, errorSize, "body too long");
      return false;
    }
    status = 200;
    if (!corrupt_.empty() && std::strstr(path, corrupt_.c_str()) != nullptr && length != 0 &&
        corruptTimes_ > 0) {
      buffer[length / 2] ^= 0xA5;
      --corruptTimes_;
    }
    return true;
  }

 private:
  std::string root_;
  std::string corrupt_;
  int corruptTimes_ = 1;
  uint16_t apiStatus_ = 0;
};

// Модель платы: ESP, Tang (LINK, flash, BL616), карта Tang — в папке tang.
class FolderBoard : public zifi::Vdac4Board {
 public:
  FolderBoard(fs::path root, const FolderFetcher& github) : root_(std::move(root)), github_(github) {
    std::ifstream ini(root_ / "board.ini");
    std::string line;
    while (std::getline(ini, line)) {
      const size_t eq = line.find('=');
      if (eq != std::string::npos) {
        std::string value = line.substr(eq + 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) {
          value.pop_back();
        }
        values_[line.substr(0, eq)] = value;
      }
    }
    espVersion_ = value("esp", "vdac4-0.1.123");
    version_ = std::strtoul(value("version", "0101012E").c_str(), nullptr, 16);
    cpInfo_ = std::strtoul(value("cpinfo", "C0290000").c_str(), nullptr, 16);
    link_ = value("link", "1") != "0";
    flash_ = value("flash", "1") != "0";
    sd_ = value("sd", "1") != "0";
    held_ = value("held", "0") == "1";
    readFile(root_ / "flash.bin", flashImage_);
    const char* slot = std::getenv("ZIFI_VUP_FAIL_SLOT");
    failSlot_ = slot == nullptr ? -1 : std::atoi(slot);
    const char* sdFail = std::getenv("ZIFI_VUP_FAIL_SD");
    failSd_ = sdFail == nullptr ? "" : sdFail;
  }

  const char* espVersion() override { return espVersion_.c_str(); }
  const char* releaseFormat() override { return "vdac4-%u.%u.%u%n"; }

  bool espImageSha256(uint32_t size, uint8_t digest[32]) override {
    std::vector<uint8_t> image;
    if (!readFile(root_ / "esp.bin", image) || image.size() < size) {
      return false;
    }
    Sha256 sha;
    sha.update(image.data(), size);
    sha.finish(digest);
    return true;
  }

  bool espInstall(const char* rawPath, const uint8_t sha256[32], zifi::VupProgress progress,
                  void* context, char* error, size_t errorSize) override {
    printLine(std::string("HTTP raw.githubusercontent.com ") + rawPath);
    std::vector<uint8_t> image;
    if (!readFile(rawFile(github_.root(), rawPath), image)) {
      std::snprintf(error, errorSize, "firmware: http 404");
      return false;
    }
    const char* corrupt = std::getenv("ZIFI_VUP_CORRUPT");
    if (corrupt != nullptr && std::strstr(rawPath, corrupt) != nullptr && !image.empty()) {
      image[image.size() / 2] ^= 0xA5;
    }
    if (progress != nullptr) {
      progress(context, 50, 100);
    }
    uint8_t digest[32];
    Sha256 sha;
    sha.update(image.data(), image.size());
    sha.finish(digest);
    if (std::memcmp(digest, sha256, 32) != 0) {
      std::snprintf(error, errorSize, "sha256 mismatch");
      return false;
    }
    if (progress != nullptr) {
      progress(context, 100, 100);
    }
    writeFile(root_ / "esp_ota.bin", image.data(), image.size());
    printLine("ESPINSTALL " + std::to_string(image.size()));
    return true;
  }

  void espRestart() override { printLine("ESPRESTART"); }

  bool tangVersion(uint32_t& version) override {
    version = link_ ? version_ : 0;
    return link_;
  }

  bool bl616Info(uint32_t& info) override {
    info = link_ ? cpInfo_ : 0;
    return link_;
  }

  const char* flashReady() override {
    if (!link_) return "LINK to Tang is down";
    if ((version_ >> 24) != 0x01) return "Tang does not run TS-Config (slot 0)";
    if (!flash_) return "Tang flash bridge does not answer";
    return nullptr;
  }

  bool flashRead(uint32_t address, uint8_t* data, size_t length) override {
    if (address + length > flashImage_.size()) {
      std::memset(data, 0xFF, length);
      if (address < flashImage_.size()) {
        std::memcpy(data, flashImage_.data() + address, flashImage_.size() - address);
      }
      return true;
    }
    std::memcpy(data, flashImage_.data() + address, length);
    return true;
  }

  bool flashWriteSlot(uint32_t base, const uint8_t* image, size_t size, zifi::VupProgress progress,
                      void* context, char* error, size_t errorSize) override {
    if (image[0] != 0xFF && std::memcmp(image, "BITS", 4) != 0) {
      std::snprintf(error, errorSize, "not a GW2AR-18 bitstream");
      return false;
    }
    if (flashImage_.size() < base + 0x100000) {
      flashImage_.resize(base + 0x100000, 0xFF);
    }
    std::memset(flashImage_.data() + base, 0xFF, 0x100000);
    std::memcpy(flashImage_.data() + base, image, size);
    if (static_cast<int>(base >> 20) == failSlot_) {
      flashImage_[base + size / 2] ^= 0x5A;    // сбой записи: сверка чтением его ловит
      writeFile(root_ / "flash.bin", flashImage_.data(), flashImage_.size());
      std::snprintf(error, errorSize, "verify: flash differs at %06lX",
                    static_cast<unsigned long>(base + size / 2));
      return false;
    }
    if (progress != nullptr) {
      progress(context, static_cast<uint32_t>(size), static_cast<uint32_t>(size));
    }
    writeFile(root_ / "flash.bin", flashImage_.data(), flashImage_.size());
    char line[64];
    std::snprintf(line, sizeof(line), "FLASH %06lX %u", static_cast<unsigned long>(base),
                  static_cast<unsigned>(size));
    printLine(line);
    return true;
  }

  bool bl616Push(const uint8_t* image, size_t size, uint32_t& newInfo, char* error,
                 size_t errorSize) override {
    // Образ модели: «BFNP», затем номер прошивки
    if (!link_ || size < 8 || std::memcmp(image, "BFNP", 4) != 0) {
      std::snprintf(error, errorSize, "bad BL616 image");
      return false;
    }
    writeFile(root_ / "bl616.bin", image, size);
    cpInfo_ = 0xC0000000U | (static_cast<uint32_t>(image[4]) << 16);
    newInfo = cpInfo_;
    char line[32];
    std::snprintf(line, sizeof(line), "BL616 %08lX", static_cast<unsigned long>(newInfo));
    printLine(line);
    return true;
  }

  const char* tangSdReady() override {
    if (!link_) return "LINK to Tang is down";
    if (held_) return "Tang SD is held by WC (SD2)";
    if (!sd_) return "no Tang SD card";
    return nullptr;
  }

  bool tangSdStat(const char* path, bool& exists, uint32_t& size) override {
    const fs::path file = root_ / "sd" / path;
    std::error_code code;
    exists = fs::is_regular_file(file, code);
    size = exists ? static_cast<uint32_t>(fs::file_size(file, code)) : 0;
    return true;
  }

  bool tangSdRead(const char* path, uint8_t* data, uint32_t size) override {
    std::vector<uint8_t> file;
    if (!readFile(root_ / "sd" / path, file) || file.size() != size) {
      return false;
    }
    std::memcpy(data, file.data(), size);
    return true;
  }

  bool tangSdWrite(const char* path, const uint8_t* data, uint32_t size, char* error,
                   size_t errorSize) override {
    if (!failSd_.empty() && std::strstr(path, failSd_.c_str()) != nullptr) {
      std::snprintf(error, errorSize, "Tang SD: read-back does not match");
      return false;
    }
    if (!writeFile(root_ / "sd" / path, data, size)) {
      std::snprintf(error, errorSize, "Tang SD: write failed");
      return false;
    }
    printLine(std::string("TANGSD ") + path + " " + std::to_string(size));
    return true;
  }

 private:
  std::string value(const char* key, const char* fallback) const {
    const auto found = values_.find(key);
    return found == values_.end() ? fallback : found->second;
  }

  fs::path root_;
  const FolderFetcher& github_;
  std::map<std::string, std::string> values_;
  std::string espVersion_;
  uint32_t version_ = 0;
  uint32_t cpInfo_ = 0;
  bool link_ = true;
  bool flash_ = true;
  bool sd_ = true;
  bool held_ = false;
  std::vector<uint8_t> flashImage_;
  int failSlot_ = -1;
  std::string failSd_;
};

uint32_t clockMs() { return millis(); }

}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    std::printf("usage: host_vup <evo|-> <github> <tang> <check|apply|auto|sync>\n");
    return 2;
  }
  timeBeginPeriod(1);
  const std::string evo = argv[1];
  const std::string mode = argv[4];
  const char* stopAfter = std::getenv("ZIFI_VUP_STOP_AFTER");
  g_stopAfter = stopAfter == nullptr ? -1 : std::atoi(stopAfter);

  std::unique_ptr<zifi::host::Z80Simulator> simulator;
  std::unique_ptr<zifi::host::SerialLink> link;
  std::unique_ptr<zifi::Z80Transport> transport;
  std::unique_ptr<zifi::VfsBridge> bridge;
  if (evo != "-") {
    simulator = std::make_unique<zifi::host::Z80Simulator>(Serial, evo);
    simulator->setWriteNewOnly(true);   // как плагин: OPEN на запись занятое имя не берёт
    link = std::make_unique<zifi::host::SerialLink>(Serial);
    transport = std::make_unique<zifi::Z80Transport>(*link);
    transport->begin();
    bridge = std::make_unique<zifi::VfsBridge>(*transport);
    if (!bridge->begin(true)) {
      printLine("RESULT 2 bridge");
      return 2;
    }
  }
  FolderFetcher fetcher(argv[2]);
  FolderBoard board(argv[3], fetcher);
  zifi::Vdac4Updater updater(bridge.get(), fetcher, board, printEvent, nullptr, clockMs);
  g_updater = &updater;
  const uint8_t flags = evo != "-" ? zifi::Vdac4Updater::kStartEvo : 0;
  char error[64] = {};
  if (!updater.configure(&flags, 1, error, sizeof(error))) {
    printLine(std::string("RESULT 2 ") + error);
    return 2;
  }

  std::atomic<bool> done{false};
  bool ok = false;
  std::thread worker([&] {
    ok = updater.check();
    if (ok && mode == "sync") {
      ok = updater.resend();
    } else if (ok && mode != "check") {
      std::vector<uint8_t> indices;
      for (size_t index = 0; index < updater.rowCount(); ++index) {
        const zifi::VupRow* row = updater.row(index);
        if (zifi::Vdac4Updater::canInstall(*row) && (mode == "apply" || row->autoMark)) {
          indices.push_back(static_cast<uint8_t>(index));
        }
      }
      printLine("APPLY " + std::to_string(indices.size()));
      ok = updater.apply(indices.data(), indices.size());
      if (updater.restartPending()) {
        printLine("RESTARTPENDING");
        board.espRestart();
      }
    }
    done = true;
  });
  while (!done) {
    if (bridge) {
      bridge->pollCore1();
      bridge->waitForRequest(1);
    } else {
      delay(1);
    }
  }
  worker.join();
  printLine(std::string("RESULT ") + (ok ? "0 " : "1 ") + updater.lastError());
  return ok ? 0 : 1;
}
