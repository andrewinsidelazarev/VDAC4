#include "zifi/vdac4_updater.hpp"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zifi/protocol.hpp"

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

namespace zifi {
namespace {

constexpr const char* kApiHost = "api.github.com";
constexpr const char* kRawHost = "raw.githubusercontent.com";
constexpr uint8_t kEventState = 0x67;
constexpr uint8_t kEventEntry = 0x68;
constexpr size_t kEventMax = 63;               // событие ESP -> Z80 длиннее не доходит
constexpr size_t kChunk = 4096;
constexpr uint32_t kSlotBytes = 0x100000;

constexpr const char* kEspRepo = "ESP32 S3 Zero N8R8/Firmware/firmware/vdac4/firmware.bin";
constexpr const char* kEspSums = "ESP32 S3 Zero N8R8/Firmware/firmware/vdac4/firmware.sha256";
constexpr const char* kBl616Repo = "Tang Nano 20k/BL616/firmware/vdac4_copro_bl616.bin";
constexpr const char* kBl616Main = "Tang Nano 20k/BL616/app/main.c";
constexpr const char* kTangReadme = "Tang Nano 20k/Firmware/README.md";

struct SlotRule {
  const char* repo;
  uint8_t slot;
  const char* name;
};
// Порядок строк — слоты 0, 1, 3; ставятся 1, 3, 0 (новый TS-Config — при следующем включении).
constexpr SlotRule kSlots[] = {
    {"Tang Nano 20k/Firmware/firmware/ts_fb_hdmi_top.bin", 0, "FPGA slot 0 TS-Config"},
    {"Tang Nano 20k/Firmware/firmware/ts_fb_slot1.bin", 1, "FPGA slot 1 FT812"},
    {"Tang Nano 20k/Firmware/firmware/ts_fb_video.bin", 3, "FPGA slot 3 video"},
};

struct FileRule {
  const char* glob;
  VupKind kind;
  const char* dir;                             // каталог на карте от корня, без '/'
};
// DESIGN.md, раздел 2. Дискета Amiga (Cores/SD/AMIGA) — данные пользователя: правил на неё нет.
constexpr FileRule kFileRules[] = {
    {"Tang Nano 20k/Cores/SD/CORES/*", VupKind::kTangFile, "CORES"},
    {"ESP32 S3 Zero N8R8/Network/*/build/vdac4/*.WMF", VupKind::kEvoFile, "WC"},
    {"ESP32 S3 Zero N8R8/Network/VDAC4 VFS/build/*.WMF", VupKind::kEvoFile, "WC"},
    {"ESP32 S3 Zero N8R8/Sound/MP3 Player/build/VDAC4MP3.WMF", VupKind::kEvoFile, "WC"},
    {"Tang Nano 20k/FTVIEW4/build/FTVIEW4.WMF", VupKind::kEvoFile, "WC"},
    {"ESP32 S3 Zero N8R8/Sound/modules/out/*.ELF", VupKind::kEvoFile, "WC/ESP"},
    {"ESP32 S3 Zero N8R8/Sound/modules/out/SIDLEN.BIN", VupKind::kEvoFile, "WC/ESP"},
    {"Tang Nano 20k/Cores/VIDEO/esp/dist/VIDEO.ELF", VupKind::kEvoFile, "WC/ESP"},
};
constexpr const char* kEvoDirs[] = {"WC", "WC/ESP"};

void* allocateLarge(size_t bytes) {
#if defined(ESP_PLATFORM)
  void* memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (memory != nullptr) {
    return memory;
  }
#endif
  return malloc(bytes);
}

void releaseLarge(void* memory) {
#if defined(ESP_PLATFORM)
  heap_caps_free(memory);
#else
  free(memory);
#endif
}

bool equalNoCase(const char* left, const char* right) {
  return VfsFileStore::equalNoCase(left, right);
}

const char* baseName(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash == nullptr ? path : slash + 1;
}

// Версия без префикса платы: «vdac4-0.1.124» -> «0.1.124» (поле строки — 11 знаков).
void shortVersion(const char* version, char* output, size_t capacity) {
  const char* dash = strchr(version, '-');
  snprintf(output, capacity, "%s", dash != nullptr && dash[1] != 0 ? dash + 1 : version);
}

// Имя строки: длиннее поля — его хвост (имя файла в конце пути важнее начала).
void tailText(char* output, size_t capacity, const char* prefix, const char* text) {
  char full[GitTreeEntry::kPathSize + 16];
  snprintf(full, sizeof(full), "%s%s", prefix, text);
  const size_t length = strlen(full);
  const char* tail = length < capacity ? full : full + (length - (capacity - 1));
  snprintf(output, capacity, "%s", tail);
}

void sizeText(uint32_t size, char* output, size_t capacity) {
  if (size < 10000U * 1024U) {
    snprintf(output, capacity, "%luK", static_cast<unsigned long>((size + 1023U) / 1024U));
  } else {
    snprintf(output, capacity, "%luM", static_cast<unsigned long>((size + 0xFFFFFU) >> 20));
  }
}

bool treeFilter(void*, const char* path, bool directory) {
  if (directory) {
    return false;
  }
  VupKind kind;
  char target[GitTreeEntry::kPathSize];
  uint8_t slot = 0;
  return Vdac4Updater::classify(path, kind, target, sizeof(target), slot);
}

int kindOrder(const VupRow& row) {
  // установка: карта Evo, карта Tang, BL616, слоты 1, 3, 0, ESP
  switch (row.kind) {
    case VupKind::kEvoFile: return 0;
    case VupKind::kTangFile: return 1;
    case VupKind::kBl616: return 2;
    case VupKind::kSlot: return row.slot == 0 ? 5 : row.slot == 1 ? 3 : 4;
    case VupKind::kEsp: return 6;
  }
  return 7;
}

}  // namespace

Vdac4Updater::Vdac4Updater(VfsBridge* bridge, WcFetcher& fetcher, Vdac4Board& board,
                           EventSink sink, void* context, Clock clock)
    : bridge_(bridge),
      fetcher_(fetcher),
      board_(board),
      sink_(sink),
      context_(context),
      clock_(clock),
      store_(nullptr),
      stop_(false),
      evo_(false),
      restart_(false),
      commit_{},
      error_{},
      rows_(nullptr),
      rowCount_(0),
      tree_(nullptr),
      treeCount_(0),
      buffer_(nullptr),
      json_(nullptr),
      manifest_{},
      manifestOk_(false),
      slotVersions_{},
      lastState_{},
      lastStateLength_(0),
      progressTotal_(0),
      progressDone_(0),
      progressBase_(0),
      progressPart_(0),
      progressPercent_(0),
      progressAt_(0),
      currentRow_(0),
      applyIndex_(0),
      applyTotal_(0) {
  rows_ = static_cast<VupRow*>(allocateLarge(sizeof(VupRow) * kMaxRows));
  tree_ = static_cast<GitTreeEntry*>(allocateLarge(sizeof(GitTreeEntry) * kMaxTreeEntries));
  buffer_ = static_cast<uint8_t*>(allocateLarge(kMaxFile + 1));
  json_ = static_cast<char*>(allocateLarge(kMaxTreeJson + 1));
  if (bridge_ != nullptr) {
    store_ = new VfsFileStore(*bridge_, &stop_, error_, sizeof(error_));
    store_->setProgress(storeProgress, this);
  }
}

Vdac4Updater::~Vdac4Updater() {
  delete store_;
  releaseLarge(rows_);
  releaseLarge(tree_);
  releaseLarge(buffer_);
  releaseLarge(json_);
}

bool Vdac4Updater::configure(const uint8_t* payload, uint16_t length, char* error,
                             size_t errorSize) {
  if (rows_ == nullptr || tree_ == nullptr || buffer_ == nullptr || json_ == nullptr) {
    snprintf(error, errorSize, "no memory for the update");
    return false;
  }
  evo_ = length >= 1 && (payload[0] & kStartEvo) != 0;
  if (evo_ && store_ == nullptr) {
    snprintf(error, errorSize, "no Evo SD bridge");
    return false;
  }
  return true;
}

// --- правила --------------------------------------------------------------------------------

bool Vdac4Updater::globMatch(const char* pattern, const char* text) {
  while (*pattern != 0) {
    if (*pattern == '*') {
      ++pattern;
      for (const char* rest = text;; ++rest) {
        if (globMatch(pattern, rest)) {
          return true;
        }
        if (*rest == 0 || *rest == '/') {
          return false;            // «*» не переходит в другую часть пути
        }
      }
    }
    if (*pattern != *text) {
      return false;
    }
    ++pattern;
    ++text;
  }
  return *text == 0;
}

bool Vdac4Updater::classify(const char* repoPath, VupKind& kind, char* target,
                            size_t targetSize, uint8_t& slot) {
  target[0] = 0;
  slot = 0;
  if (strcmp(repoPath, kEspRepo) == 0) {
    kind = VupKind::kEsp;
    return true;
  }
  if (strcmp(repoPath, kBl616Repo) == 0) {
    kind = VupKind::kBl616;
    return true;
  }
  for (const SlotRule& rule : kSlots) {
    if (strcmp(repoPath, rule.repo) == 0) {
      kind = VupKind::kSlot;
      slot = rule.slot;
      return true;
    }
  }
  for (const FileRule& rule : kFileRules) {
    if (globMatch(rule.glob, repoPath)) {
      kind = rule.kind;
      const int written = snprintf(target, targetSize, "%s/%s", rule.dir, baseName(repoPath));
      return written > 0 && static_cast<size_t>(written) < targetSize;
    }
  }
  return false;
}

int Vdac4Updater::parseBl616Version(const char* text, size_t length) {
  // последний #define FW_VERSION: первый — у моста FTC_BRIDGE (#ifdef), последний — у прошивки
  int version = -1;
  const char* end = text + length;
  for (const char* line = text; line < end;) {
    const char* next = static_cast<const char*>(memchr(line, '\n', end - line));
    const char* stop = next != nullptr ? next : end;
    const char* p = line;
    while (p < stop && (*p == ' ' || *p == '\t')) {
      ++p;
    }
    constexpr char kDefine[] = "#define";
    if (stop - p > 7 && memcmp(p, kDefine, 7) == 0) {
      p += 7;
      while (p < stop && (*p == ' ' || *p == '\t')) {
        ++p;
      }
      constexpr char kName[] = "FW_VERSION";
      if (stop - p > 10 && memcmp(p, kName, 10) == 0 && (p[10] == ' ' || p[10] == '\t')) {
        p += 10;
        while (p < stop && (*p == ' ' || *p == '\t')) {
          ++p;
        }
        char number[16] = {};
        size_t n = 0;
        while (p < stop && n + 1 < sizeof(number) &&
               ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
                (*p >= 'A' && *p <= 'F') || *p == 'x' || *p == 'X')) {
          number[n++] = *p++;
        }
        if (n != 0) {
          char* tail = nullptr;
          const long value = strtol(number, &tail, 0);
          if (tail != nullptr && *tail == 0 && value >= 0 && value <= 255) {
            version = static_cast<int>(value);
          }
        }
      }
    }
    line = next != nullptr ? next + 1 : end;
  }
  return version;
}

bool Vdac4Updater::parseSlotVersion(const char* text, size_t length, const char* fileName,
                                    char* version, size_t versionSize) {
  // «](firmware/ts_fb_hdmi_top.» — ссылка на образ в таблице слотов (.fs или .bin), затем «| 1.46»
  char stem[64];
  snprintf(stem, sizeof(stem), "](firmware/%s", fileName);
  char* dot = strrchr(stem, '.');
  if (dot != nullptr) {
    dot[1] = 0;
  }
  const size_t stemLength = strlen(stem);
  const char* end = text + length;
  for (const char* p = text; p + stemLength < end; ++p) {
    if (memcmp(p, stem, stemLength) != 0) {
      continue;
    }
    const char* q = p + stemLength;
    while (q < end && *q != '|' && *q != '\n') {
      ++q;
    }
    if (q >= end || *q != '|') {
      continue;
    }
    ++q;
    while (q < end && *q == ' ') {
      ++q;
    }
    size_t n = 0;
    while (q < end && n + 1 < versionSize && ((*q >= '0' && *q <= '9') || *q == '.')) {
      version[n++] = *q++;
    }
    version[n] = 0;
    if (n != 0 && strchr(version, '.') != nullptr) {
      return true;
    }
  }
  return false;
}

// --- события --------------------------------------------------------------------------------

bool Vdac4Updater::sendState(uint8_t phase, uint16_t current, uint16_t total,
                             const char* format, ...) {
  uint8_t data[kEventMax + 1];
  data[0] = phase;
  data[1] = static_cast<uint8_t>(current);
  data[2] = static_cast<uint8_t>(current >> 8);
  data[3] = static_cast<uint8_t>(total);
  data[4] = static_cast<uint8_t>(total >> 8);
  data[5] = phase == kPhaseReady ? 100 : progressPercent_;
  char text[kEventMax];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  size_t length = strlen(text);
  if (length > kEventMax - 7) {
    length = kEventMax - 7;
  }
  memcpy(data + 6, text, length);
  data[6 + length] = 0;
  const uint16_t size = static_cast<uint16_t>(7 + length);
  if (phase != kPhaseSync) {
    memcpy(lastState_, data, size);
    lastStateLength_ = size;
  }
  return sink_ == nullptr || sink_(context_, kEventState, data, size);
}

uint8_t Vdac4Updater::flagsOf(const VupRow& row) {
  uint8_t flags = 0;
  if (row.remote) flags |= kVupFlagRemote;
  if (row.local) flags |= kVupFlagLocal;
  if (canInstall(row)) {
    flags |= kVupFlagCan;
    if (row.autoMark) flags |= kVupFlagAuto;
  }
  if (row.kind == VupKind::kEsp) flags |= kVupFlagReboot;
  return flags;
}

bool Vdac4Updater::canInstall(const VupRow& row) {
  return row.remote && (row.status == VupStatus::kSame || row.status == VupStatus::kDifferent ||
                        row.status == VupStatus::kNew || row.status == VupStatus::kFailed);
}

bool Vdac4Updater::sendEntry(size_t index) {
  const VupRow& row = rows_[index];
  uint8_t data[kEventMax + 1];
  size_t length = 0;
  data[length++] = static_cast<uint8_t>(index);
  data[length++] = static_cast<uint8_t>(row.status);
  data[length++] = flagsOf(row);
  data[length++] = static_cast<uint8_t>(row.kind);
  const char* texts[3] = {row.current, row.available, row.name};
  const size_t limits[3] = {VupRow::kTextSize - 1, VupRow::kTextSize - 1, VupRow::kNameSize - 1};
  for (int part = 0; part < 3; ++part) {
    const char* text = texts[part];
    size_t n = strlen(text);
    if (n > limits[part]) {
      text += n - limits[part];        // хвост: имя файла в конце пути важнее
      n = limits[part];
    }
    memcpy(data + length, text, n);
    length += n;
    data[length++] = 0;
  }
  return sink_ == nullptr || sink_(context_, kEventEntry, data, static_cast<uint16_t>(length));
}

bool Vdac4Updater::sendReady() {
  size_t can = 0;
  size_t differs = 0;
  for (size_t index = 0; index < rowCount_; ++index) {
    if (canInstall(rows_[index])) {
      ++can;
      if (rows_[index].status != VupStatus::kSame) {
        ++differs;
      }
    }
  }
  progressPercent_ = 100;
  if (differs == 0) {
    return sendState(kPhaseReady, static_cast<uint16_t>(can), static_cast<uint16_t>(rowCount_),
                     "Everything is up to date");
  }
  return sendState(kPhaseReady, static_cast<uint16_t>(can), static_cast<uint16_t>(rowCount_),
                   "%u to update - mark and press Enter", static_cast<unsigned>(differs));
}

bool Vdac4Updater::resend() {
  sendState(kPhaseSync, 0, static_cast<uint16_t>(rowCount_), "");
  for (size_t index = 0; index < rowCount_; ++index) {
    sendEntry(index);
  }
  if (lastStateLength_ != 0 && sink_ != nullptr) {
    sink_(context_, kEventState, lastState_, lastStateLength_);
  }
  return true;
}

void Vdac4Updater::fail(const char* format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(error_, sizeof(error_), format, args);
  va_end(args);
  sendState(kPhaseError, 0, 0, "%s", stop_ ? "stopped" : error_);
}

void Vdac4Updater::setReason(VupRow& row, const char* format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(row.reason, sizeof(row.reason), format, args);
  va_end(args);
}

void Vdac4Updater::beginProgress(uint32_t total) {
  progressTotal_ = total;
  progressDone_ = 0;
  progressPercent_ = 0;
  progressAt_ = clock_ != nullptr ? clock_() : 0;
}

void Vdac4Updater::addProgress(uint32_t bytes) {
  progressDone_ += bytes;
  if (progressTotal_ == 0 || lastStateLength_ < 6) {
    return;
  }
  const uint64_t scaled = static_cast<uint64_t>(progressDone_) * 100U / progressTotal_;
  const uint8_t percent = static_cast<uint8_t>(scaled > 99 ? 99 : scaled);
  const uint32_t now = clock_ != nullptr ? clock_() : 0;
  if (percent == progressPercent_ || static_cast<uint32_t>(now - progressAt_) < kProgressIntervalMs) {
    return;
  }
  progressPercent_ = percent;
  progressAt_ = now;
  lastState_[5] = percent;
  if (sink_ != nullptr) {
    sink_(context_, kEventState, lastState_, lastStateLength_);
  }
}

void Vdac4Updater::progressHook(void* context, uint32_t done, uint32_t total) {
  // done из total — ход этой части (слот — байты, ESP — проценты); общий счёт — байты от
  // progressBase_
  auto* self = static_cast<Vdac4Updater*>(context);
  if (total == 0 || done > total) {
    return;
  }
  const uint32_t reached =
      self->progressBase_ +
      static_cast<uint32_t>(static_cast<uint64_t>(done) * self->progressPart_ / total);
  if (reached > self->progressDone_) {
    self->addProgress(reached - self->progressDone_);
  }
}

void Vdac4Updater::storeProgress(void* context, uint32_t bytes) {
  static_cast<Vdac4Updater*>(context)->addProgress(bytes);
}

// --- GitHub ---------------------------------------------------------------------------------

bool Vdac4Updater::rawPath(const char* repoPath, char* output, size_t capacity) const {
  int written = snprintf(output, capacity, "/%s/%s/", kRepo, commit_);
  if (written <= 0 || static_cast<size_t>(written) >= capacity) {
    return false;
  }
  size_t at = static_cast<size_t>(written);
  static const char kHex[] = "0123456789ABCDEF";
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(repoPath); *p != 0; ++p) {
    const bool plain = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                       (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' ||
                       *p == '~' || *p == '/';
    if (at + (plain ? 1 : 3) >= capacity) {
      return false;
    }
    if (plain) {
      output[at++] = static_cast<char>(*p);
    } else {
      output[at++] = '%';
      output[at++] = kHex[*p >> 4];
      output[at++] = kHex[*p & 15];
    }
  }
  output[at] = 0;
  return true;
}

bool Vdac4Updater::rawGet(const char* repoPath, uint8_t* buffer, size_t capacity,
                          size_t& length) {
  char path[GitTreeEntry::kPathSize * 3 + 80];
  if (!rawPath(repoPath, path, sizeof(path))) {
    snprintf(error_, sizeof(error_), "path too long");
    return false;
  }
  uint16_t status = 0;
  char problem[64] = {};
  length = 0;
  if (!fetcher_.get(kRawHost, path, buffer, capacity, length, status, problem, sizeof(problem))) {
    snprintf(error_, sizeof(error_), "%s: %s", baseName(repoPath), problem);
    return false;
  }
  if (status != 200) {
    snprintf(error_, sizeof(error_), "%s: http %u", baseName(repoPath), static_cast<unsigned>(status));
    return false;
  }
  return true;
}

bool Vdac4Updater::fetchText(const char* repoPath, char* buffer, size_t capacity,
                             size_t& length) {
  if (!rawGet(repoPath, reinterpret_cast<uint8_t*>(buffer), capacity - 1, length)) {
    return false;
  }
  buffer[length] = 0;
  return true;
}

bool Vdac4Updater::fetchTree() {
  sendState(kPhaseGithub, 0, 0, "GitHub: %s", kRepo);
  char path[160];
  snprintf(path, sizeof(path), "/repos/%s/git/ref/heads/%s", kRepo, kBranch);
  size_t length = 0;
  uint16_t status = 0;
  char problem[64] = {};
  if (!fetcher_.get(kApiHost, path, reinterpret_cast<uint8_t*>(json_), kMaxTreeJson, length,
                    status, problem, sizeof(problem))) {
    fail("GitHub: %s", problem);
    return false;
  }
  if (status == 403 || status == 429) {
    fail("GitHub API limit - try again in an hour");
    return false;
  }
  if (status != 200 || !parseGitRefCommit(json_, length, commit_)) {
    fail("GitHub branch: http %u", static_cast<unsigned>(status));
    return false;
  }
  if (stop_) {
    fail("stopped");
    return false;
  }
  sendState(kPhaseGithub, 0, 0, "GitHub: commit %.7s", commit_);
  snprintf(path, sizeof(path), "/repos/%s/git/trees/%s?recursive=1", kRepo, commit_);
  if (!fetcher_.get(kApiHost, path, reinterpret_cast<uint8_t*>(json_), kMaxTreeJson, length,
                    status, problem, sizeof(problem))) {
    fail("GitHub tree: %s", problem);
    return false;
  }
  if (status == 403 || status == 429) {
    fail("GitHub API limit - try again in an hour");
    return false;
  }
  bool truncated = false;
  size_t count = 0;
  if (status != 200 ||
      !parseGitTree(json_, length, tree_, kMaxTreeEntries, count, truncated, treeFilter, nullptr)) {
    fail("GitHub tree: bad answer (http %u)", static_cast<unsigned>(status));
    return false;
  }
  if (truncated) {
    fail("GitHub tree: truncated");
    return false;
  }
  treeCount_ = count;
  return true;
}

bool Vdac4Updater::addRow(const GitTreeEntry& entry, VupKind kind, const char* target,
                          uint8_t slot) {
  if (rowCount_ >= kMaxRows) {
    return false;
  }
  if (target[0] != 0) {
    for (size_t index = 0; index < rowCount_; ++index) {
      if (equalNoCase(rows_[index].target, target)) {
        return true;                   // два файла репозитория на одно имя карты: первый
      }
    }
  }
  VupRow& row = rows_[rowCount_++];
  row = VupRow();
  row.kind = kind;
  row.remote = true;
  row.slot = slot;
  row.size = entry.size;
  memcpy(row.sha, entry.sha, sizeof(row.sha));
  snprintf(row.repo, sizeof(row.repo), "%s", entry.path);
  snprintf(row.target, sizeof(row.target), "%s", target);
  switch (kind) {
    case VupKind::kEsp: snprintf(row.name, sizeof(row.name), "ESP32-S3 firmware"); break;
    case VupKind::kBl616: snprintf(row.name, sizeof(row.name), "BL616 firmware"); break;
    case VupKind::kSlot:
      for (const SlotRule& rule : kSlots) {
        if (rule.slot == slot) {
          snprintf(row.name, sizeof(row.name), "%s", rule.name);
        }
      }
      break;
    case VupKind::kTangFile: tailText(row.name, sizeof(row.name), "Tang SD: ", target); break;
    case VupKind::kEvoFile: tailText(row.name, sizeof(row.name), "", target); break;
  }
  return true;
}

bool Vdac4Updater::buildRows() {
  rowCount_ = 0;
  // Части платы — в постоянном порядке, затем файлы карт по имени. Чего нет в репозитории (образ
  // слота ещё не выложен), того нет и в списке: остальное сверяется как обычно.
  const VupKind fixed[] = {VupKind::kEsp, VupKind::kBl616};
  for (VupKind want : fixed) {
    bool found = false;
    for (size_t index = 0; index < treeCount_ && !found; ++index) {
      VupKind kind;
      char target[GitTreeEntry::kPathSize];
      uint8_t slot = 0;
      if (classify(tree_[index].path, kind, target, sizeof(target), slot) && kind == want) {
        found = addRow(tree_[index], kind, target, slot);
      }
    }
  }
  for (const SlotRule& rule : kSlots) {
    bool found = false;
    for (size_t index = 0; index < treeCount_ && !found; ++index) {
      if (strcmp(tree_[index].path, rule.repo) == 0) {
        found = addRow(tree_[index], VupKind::kSlot, "", rule.slot);
      }
    }
  }
  const VupKind files[] = {VupKind::kTangFile, VupKind::kEvoFile};
  for (VupKind want : files) {
    if (want == VupKind::kEvoFile && !evo_) {
      continue;
    }
    const size_t first = rowCount_;
    for (size_t index = 0; index < treeCount_; ++index) {
      VupKind kind;
      char target[GitTreeEntry::kPathSize];
      uint8_t slot = 0;
      if (classify(tree_[index].path, kind, target, sizeof(target), slot) && kind == want &&
          !addRow(tree_[index], kind, target, slot)) {
        fail("too many files on GitHub (more than %u)", static_cast<unsigned>(kMaxRows));
        return false;
      }
    }
    // по имени на карте
    for (size_t i = first + 1; i < rowCount_; ++i) {
      for (size_t j = i; j > first && strcmp(rows_[j - 1].target, rows_[j].target) > 0; --j) {
        VupRow swap = rows_[j];
        rows_[j] = rows_[j - 1];
        rows_[j - 1] = swap;
      }
    }
  }
  if (rowCount_ == 0) {
    fail("GitHub: no VDAC4 board files in %.7s", commit_);
    return false;
  }
  return true;
}

bool Vdac4Updater::download(VupRow& row, uint8_t* buffer) {
  if (row.size > kMaxFile) {
    snprintf(error_, sizeof(error_), "%s: too big", baseName(row.repo));
    return false;
  }
  for (unsigned attempt = 0; attempt < kDownloadAttempts && !stop_; ++attempt) {
    size_t length = 0;
    if (!rawGet(row.repo, buffer, kMaxFile, length)) {
      continue;
    }
    if (length != row.size) {
      snprintf(error_, sizeof(error_), "%s: %lu bytes, GitHub tree says %lu", baseName(row.repo),
               static_cast<unsigned long>(length), static_cast<unsigned long>(row.size));
      continue;
    }
    Sha1 sha;
    gitBlobBegin(sha, row.size);
    sha.update(buffer, length);
    uint8_t digest[Sha1::kDigestSize];
    sha.finish(digest);
    if (memcmp(digest, row.sha, sizeof(digest)) == 0) {
      return true;
    }
    snprintf(error_, sizeof(error_), "%s: download damaged", baseName(row.repo));
  }
  if (stop_) {
    snprintf(error_, sizeof(error_), "stopped");
  }
  return false;
}

// --- сверка ---------------------------------------------------------------------------------

void Vdac4Updater::checkEsp(VupRow& row) {
  const char* installed = board_.espVersion();
  shortVersion(installed, row.current, sizeof(row.current));
  row.local = true;
  if (!manifestOk_) {
    row.status = VupStatus::kReadError;
    setReason(row, "firmware.sha256 unreadable");
    snprintf(row.available, sizeof(row.available), "?");
    return;
  }
  shortVersion(manifest_.version, row.available, sizeof(row.available));
  uint8_t digest[32];
  if (board_.espImageSha256(row.size, digest) && memcmp(digest, manifest_.sha256, 32) == 0) {
    row.status = VupStatus::kSame;
    return;
  }
  const uint8_t relation = compareReleaseVersion(installed, manifest_.version, board_.releaseFormat());
  if (relation == kOnlineUpdateOlder) {
    row.status = VupStatus::kOlder;
    setReason(row, "published %s is older", manifest_.version);
    return;
  }
  row.status = VupStatus::kDifferent;
  row.autoMark = true;
}

void Vdac4Updater::checkBl616(VupRow& row) {
  if (row.bl616Version == 0) {
    row.status = VupStatus::kReadError;
    snprintf(row.available, sizeof(row.available), "?");
    snprintf(row.current, sizeof(row.current), "?");
    setReason(row, "BL616 main.c: no FW_VERSION");
    return;
  }
  snprintf(row.available, sizeof(row.available), "%u", row.bl616Version);
  uint32_t info = 0;
  // {C0, версия, …} — прошивка работает; C1 — она в режиме приёма образа, номера нет
  if (!board_.bl616Info(info) || (info >> 24) != 0xC0) {
    row.status = VupStatus::kReadError;
    snprintf(row.current, sizeof(row.current), "?");
    setReason(row, "BL616 does not answer (LINK)");
    return;
  }
  const unsigned current = (info >> 16) & 0xFF;
  row.local = true;
  snprintf(row.current, sizeof(row.current), "%u", current);
  if (current == row.bl616Version) {
    row.status = VupStatus::kSame;
  } else if (row.bl616Version > current) {
    row.status = VupStatus::kDifferent;
    row.autoMark = true;
  } else {
    row.status = VupStatus::kOlder;
    setReason(row, "published %u is older", row.bl616Version);
  }
}

void Vdac4Updater::checkSlot(VupRow& row) {
  const size_t which = row.slot == 0 ? 0 : row.slot == 1 ? 1 : 2;
  snprintf(row.available, sizeof(row.available), "%s",
           slotVersions_[which][0] != 0 ? slotVersions_[which] : "?");
  snprintf(row.current, sizeof(row.current), "?");
  const char* problem = board_.flashReady();
  if (problem != nullptr) {
    row.status = VupStatus::kReadError;
    setReason(row, "%s", problem);
    return;
  }
  Sha1 sha;
  gitBlobBegin(sha, row.size);
  uint8_t* chunk = buffer_;
  for (uint32_t done = 0; done < row.size;) {
    if (stop_) {
      row.status = VupStatus::kReadError;
      setReason(row, "stopped");
      return;
    }
    const uint32_t part = (row.size - done) < kChunk ? (row.size - done) : kChunk;
    if (!board_.flashRead(row.slot * kSlotBytes + done, chunk, part)) {
      row.status = VupStatus::kReadError;
      setReason(row, "Tang flash read failed at %06lX",
                static_cast<unsigned long>(row.slot * kSlotBytes + done));
      return;
    }
    sha.update(chunk, part);
    done += part;
    addProgress(part);
  }
  uint8_t digest[Sha1::kDigestSize];
  sha.finish(digest);
  row.local = true;
  const bool same = memcmp(digest, row.sha, sizeof(digest)) == 0;
  row.status = same ? VupStatus::kSame : VupStatus::kDifferent;
  row.autoMark = !same;
  uint32_t version = 0;
  if (row.slot == 0 && board_.tangVersion(version) && (version >> 24) == 0x01) {
    snprintf(row.current, sizeof(row.current), "%lu.%02lu",
             static_cast<unsigned long>((version >> 8) & 0xFF),
             static_cast<unsigned long>(version & 0xFF));
  } else if (same) {
    snprintf(row.current, sizeof(row.current), "%s", row.available);
  }
}

void Vdac4Updater::checkTangFile(VupRow& row) {
  sizeText(row.size, row.available, sizeof(row.available));
  snprintf(row.current, sizeof(row.current), "-");
  bool exists = false;
  uint32_t size = 0;
  if (!board_.tangSdStat(row.target, exists, size)) {
    row.status = VupStatus::kReadError;
    setReason(row, "Tang SD: cannot read %s", row.target);
    return;
  }
  if (!exists) {
    row.status = VupStatus::kNew;
    row.autoMark = true;            // новый файл части платы ставится по умолчанию
    return;
  }
  row.local = true;
  row.localSize = size;
  sizeText(size, row.current, sizeof(row.current));
  if (size != row.size) {
    row.status = VupStatus::kDifferent;
    row.autoMark = true;
    return;
  }
  if (size > kMaxFile || !board_.tangSdRead(row.target, buffer_, size)) {
    row.status = VupStatus::kReadError;
    setReason(row, "Tang SD: cannot read %s", row.target);
    return;
  }
  addProgress(size);
  Sha1 sha;
  gitBlobBegin(sha, size);
  sha.update(buffer_, size);
  uint8_t digest[Sha1::kDigestSize];
  sha.finish(digest);
  const bool same = memcmp(digest, row.sha, sizeof(digest)) == 0;
  row.status = same ? VupStatus::kSame : VupStatus::kDifferent;
  row.autoMark = !same;
}

namespace {
struct EvoScan {
  VupRow* rows;
  size_t count;
};

void evoVisitor(void* context, size_t, const char* relative, const VfsResult& entry) {
  if (relative == nullptr) {
    return;
  }
  EvoScan* scan = static_cast<EvoScan*>(context);
  for (size_t index = 0; index < scan->count; ++index) {
    VupRow& row = scan->rows[index];
    if (row.kind == VupKind::kEvoFile && VfsFileStore::equalNoCase(row.target, relative)) {
      row.local = true;
      row.localSize = entry.size;
    }
  }
}
}  // namespace

bool Vdac4Updater::checkEvoFiles() {
  if (!evo_ || store_ == nullptr) {
    return true;
  }
  store_->clearDirectories();
  for (const char* dir : kEvoDirs) {
    store_->addDirectory(dir);
  }
  EvoScan scan = {rows_, rowCount_};
  bool listed = true;
  for (size_t dir = 0; dir < store_->directoryCount() && listed; ++dir) {
    const VfsFileStore::Scan result = store_->scanDirectory(dir, evoVisitor, &scan);
    listed = result == VfsFileStore::Scan::kDone;
  }
  for (size_t index = 0; index < rowCount_; ++index) {
    VupRow& row = rows_[index];
    if (row.kind != VupKind::kEvoFile) {
      continue;
    }
    sizeText(row.size, row.available, sizeof(row.available));
    snprintf(row.current, sizeof(row.current), "-");
    if (!listed) {
      row.status = VupStatus::kReadError;
      setReason(row, "%s", error_[0] != 0 ? error_ : "Evo SD: cannot list");
    } else if (store_->leftoverIn(row.target)) {
      row.status = VupStatus::kReadError;
      setReason(row, "%s", VfsFileStore::kLeftoverReason);
    } else if (!row.local) {
      row.status = VupStatus::kNew;
      row.autoMark = true;
    } else {
      sizeText(row.localSize, row.current, sizeof(row.current));
      if (row.localSize != row.size) {
        row.status = VupStatus::kDifferent;
        row.autoMark = true;
      } else {
        char path[GitTreeEntry::kPathSize + 2];
        VfsFileStore::localPath(row.target, path, sizeof(path));
        uint8_t digest[Sha1::kDigestSize];
        if (!store_->hashLocal(path, row.size, digest)) {
          row.status = VupStatus::kReadError;
          setReason(row, "Evo SD: cannot read");
        } else {
          const bool same = memcmp(digest, row.sha, sizeof(digest)) == 0;
          row.status = same ? VupStatus::kSame : VupStatus::kDifferent;
          row.autoMark = !same;
        }
      }
    }
    sendEntry(index);
    if (stop_) {
      return false;
    }
  }
  error_[0] = 0;
  return true;
}

bool Vdac4Updater::check() {
  error_[0] = 0;
  restart_ = false;
  manifestOk_ = false;
  rowCount_ = 0;
  memset(slotVersions_, 0, sizeof(slotVersions_));
  if (!fetchTree() || !buildRows()) {
    return false;
  }
  // описи и версии — с того же коммита
  size_t length = 0;
  char* text = reinterpret_cast<char*>(buffer_);
  if (fetchText(kEspSums, text, kMaxFile, length)) {
    manifestOk_ = parseOnlineManifest(buffer_, length, manifest_);
  }
  int bl616 = -1;
  if (fetchText(kBl616Main, text, kMaxFile, length)) {
    bl616 = parseBl616Version(text, length);
  }
  if (fetchText(kTangReadme, text, kMaxFile, length)) {
    for (const SlotRule& rule : kSlots) {
      const size_t which = rule.slot == 0 ? 0 : rule.slot == 1 ? 1 : 2;
      parseSlotVersion(text, length, baseName(rule.repo), slotVersions_[which],
                       sizeof(slotVersions_[which]));
    }
  }
  error_[0] = 0;
  for (size_t index = 0; index < rowCount_; ++index) {
    if (rows_[index].kind == VupKind::kBl616) {
      rows_[index].bl616Version = bl616 > 0 ? static_cast<unsigned>(bl616) : 0;
    }
  }
  // ход сверки — по байтам: слоты, файлы карт
  uint32_t total = 0;
  for (size_t index = 0; index < rowCount_; ++index) {
    if (rows_[index].kind == VupKind::kSlot || rows_[index].kind == VupKind::kTangFile ||
        rows_[index].kind == VupKind::kEvoFile) {
      total += rows_[index].size;
    }
  }
  beginProgress(total);
  sendState(kPhaseCheck, 0, static_cast<uint16_t>(rowCount_), "Reading the board");
  const char* tangSd = nullptr;
  bool tangSdAsked = false;
  for (size_t index = 0; index < rowCount_ && !stop_; ++index) {
    VupRow& row = rows_[index];
    switch (row.kind) {
      case VupKind::kEsp: checkEsp(row); break;
      case VupKind::kBl616: checkBl616(row); break;
      case VupKind::kSlot: checkSlot(row); break;
      case VupKind::kTangFile:
        if (!tangSdAsked) {
          tangSd = board_.tangSdReady();
          tangSdAsked = true;
        }
        if (tangSd != nullptr) {
          sizeText(row.size, row.available, sizeof(row.available));
          snprintf(row.current, sizeof(row.current), "?");
          row.status = VupStatus::kReadError;
          setReason(row, "%s", tangSd);
        } else {
          checkTangFile(row);
        }
        break;
      case VupKind::kEvoFile: continue;      // ниже, одной описью каталогов
    }
    sendEntry(index);
  }
  if (stop_) {
    fail("stopped");
    return false;
  }
  if (!checkEvoFiles()) {
    fail("stopped");
    return false;
  }
  return sendReady();
}

// --- установка ------------------------------------------------------------------------------

bool Vdac4Updater::storeDownload(void* context, uint8_t* buffer) {
  auto* self = static_cast<Vdac4Updater*>(context);
  return self->download(self->rows_[self->currentRow_], buffer);
}

bool Vdac4Updater::installEvo(VupRow& row) {
  if (store_ == nullptr) {
    setReason(row, "no Evo SD bridge");
    return false;
  }
  if (store_->diskSuspect()) {
    setReason(row, "check the disk");
    return false;
  }
  VfsFileStore::Install result;
  store_->installFile(row.target, row.sha, row.size, false, buffer_, storeDownload, this, result);
  if (!result.done) {
    setReason(row, "%s", result.reason[0] != 0 ? result.reason : error_);
    return false;
  }
  row.local = true;
  row.localSize = row.size;
  return true;
}

bool Vdac4Updater::installTang(VupRow& row) {
  const char* problem = board_.tangSdReady();
  if (problem != nullptr) {
    setReason(row, "%s", problem);
    return false;
  }
  if (!download(row, buffer_)) {
    setReason(row, "%s", error_);
    return false;
  }
  char reason[64] = {};
  if (!board_.tangSdWrite(row.target, buffer_, row.size, reason, sizeof(reason))) {
    setReason(row, "%s", reason);
    return false;
  }
  addProgress(row.size);
  row.local = true;
  row.localSize = row.size;
  return true;
}

bool Vdac4Updater::installBl616(VupRow& row) {
  if (!download(row, buffer_)) {
    setReason(row, "%s", error_);
    return false;
  }
  uint32_t info = 0;
  char reason[64] = {};
  if (!board_.bl616Push(buffer_, row.size, info, reason, sizeof(reason))) {
    setReason(row, "%s", reason);
    return false;
  }
  addProgress(row.size);
  snprintf(row.current, sizeof(row.current), "%lu", static_cast<unsigned long>((info >> 16) & 0xFF));
  row.local = true;
  return true;
}

bool Vdac4Updater::installSlot(VupRow& row) {
  const char* problem = board_.flashReady();
  if (problem != nullptr) {
    setReason(row, "%s", problem);
    return false;
  }
  if (!download(row, buffer_)) {
    setReason(row, "%s", error_);
    return false;
  }
  char reason[64] = {};
  const uint32_t before = progressDone_;
  progressBase_ = before;
  progressPart_ = row.size;
  if (!board_.flashWriteSlot(row.slot * kSlotBytes, buffer_, row.size, progressHook, this, reason,
                             sizeof(reason))) {
    setReason(row, "%s", reason);
    return false;
  }
  progressDone_ = before + row.size;
  row.local = true;
  return true;
}

bool Vdac4Updater::installEsp(VupRow& row) {
  if (!manifestOk_) {
    setReason(row, "firmware.sha256 unreadable");
    return false;
  }
  // запрет понижения — и здесь, а не только в плагине
  if (compareReleaseVersion(board_.espVersion(), manifest_.version, board_.releaseFormat()) ==
      kOnlineUpdateOlder) {
    row.status = VupStatus::kOlder;
    setReason(row, "published %s is older", manifest_.version);
    return false;
  }
  char path[GitTreeEntry::kPathSize * 3 + 80];
  if (!rawPath(row.repo, path, sizeof(path))) {
    setReason(row, "path too long");
    return false;
  }
  char reason[64] = {};
  const uint32_t before = progressDone_;
  progressBase_ = before;
  progressPart_ = row.size;
  if (!board_.espInstall(path, manifest_.sha256, progressHook, this, reason, sizeof(reason))) {
    setReason(row, "%s", reason);
    return false;
  }
  progressDone_ = before + row.size;
  return true;
}

bool Vdac4Updater::installRow(size_t index) {
  VupRow& row = rows_[index];
  row.reason[0] = 0;
  error_[0] = 0;
  bool ok = false;
  switch (row.kind) {
    case VupKind::kEvoFile: currentRow_ = index; ok = installEvo(row); break;
    case VupKind::kTangFile: ok = installTang(row); break;
    case VupKind::kBl616: ok = installBl616(row); break;
    case VupKind::kSlot: ok = installSlot(row); break;
    case VupKind::kEsp: ok = installEsp(row); break;
  }
  if (ok) {
    row.status = VupStatus::kUpdated;
    row.autoMark = false;
    if (row.kind == VupKind::kEsp) {
      snprintf(row.current, sizeof(row.current), "%s", row.available);
    } else if (row.kind != VupKind::kBl616) {
      snprintf(row.current, sizeof(row.current), "%s", row.available);
    }
  } else if (row.status != VupStatus::kOlder) {
    row.status = VupStatus::kFailed;
  }
  sendEntry(index);
  return ok;
}

bool Vdac4Updater::apply(const uint8_t* indices, size_t count) {
  error_[0] = 0;
  restart_ = false;
  // отобрать и упорядочить: карта Evo, карта Tang, BL616, слоты 1, 3, 0, ESP
  uint8_t order[kMaxRows];
  size_t chosen = 0;
  for (size_t i = 0; i < count; ++i) {
    const size_t index = indices[i];
    if (index >= rowCount_ || !canInstall(rows_[index])) {
      continue;
    }
    bool repeated = false;
    for (size_t j = 0; j < chosen; ++j) {
      repeated = repeated || order[j] == index;
    }
    if (!repeated) {
      order[chosen++] = static_cast<uint8_t>(index);
    }
  }
  for (size_t i = 1; i < chosen; ++i) {
    for (size_t j = i; j > 0 && kindOrder(rows_[order[j - 1]]) > kindOrder(rows_[order[j]]); --j) {
      const uint8_t swap = order[j];
      order[j] = order[j - 1];
      order[j - 1] = swap;
    }
  }
  uint32_t total = 0;
  for (size_t i = 0; i < chosen; ++i) {
    total += rows_[order[i]].size;
  }
  beginProgress(total);
  applyTotal_ = static_cast<uint16_t>(chosen);
  size_t failed = 0;
  bool espDone = false;
  for (size_t i = 0; i < chosen; ++i) {
    if (stop_) {
      fail("stopped");
      return false;
    }
    VupRow& row = rows_[order[i]];
    applyIndex_ = static_cast<uint16_t>(i);
    sendState(kPhaseApply, static_cast<uint16_t>(i), static_cast<uint16_t>(chosen), "Installing %s",
              row.name);
    if (installRow(order[i])) {
      espDone = espDone || row.kind == VupKind::kEsp;
    } else {
      ++failed;
    }
  }
  if (espDone) {
    restart_ = true;
    progressPercent_ = 100;
    return sendState(kPhaseRestart, static_cast<uint16_t>(chosen), static_cast<uint16_t>(chosen),
                     "%s", manifest_.version);
  }
  if (failed != 0) {
    // строка итога — причина первой неудачи
    for (size_t i = 0; i < chosen; ++i) {
      const VupRow& row = rows_[order[i]];
      if (row.status == VupStatus::kFailed) {
        progressPercent_ = 100;
        return sendState(kPhaseReady, 0, static_cast<uint16_t>(rowCount_), "FAILED %s: %s",
                         row.name, row.reason[0] != 0 ? row.reason : "error");
      }
    }
  }
  return sendReady();
}

}  // namespace zifi
