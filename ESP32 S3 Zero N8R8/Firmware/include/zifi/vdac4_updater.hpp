#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zifi/git_sha1.hpp"
#include "zifi/github_tree.hpp"
#include "zifi/online_manifest.hpp"
#include "zifi/vfs_file_store.hpp"
#include "zifi/wc_updater.hpp"

namespace zifi {

// Обновлятор всей платы VDAC4 с GitHub (плагин VDAC4 Online Update 2, устройство —
// Network/Online Update/DESIGN.md). Эталон — git-SHA-1 файлов одного коммита ветки main
// репозитория andrewinsidelazarev/VDAC4: ссылка на ветку и дерево всего репозитория — два запроса
// к API. Каждая строка списка — часть платы:
//   1 ESP     — прошивка ESP (раздел OTA), версия и SHA-256 из firmware.sha256;
//   2 BL616   — прошивка BL616 модуля Tang, номер из CP_INFO против FW_VERSION в app/main.c;
//   3 слот    — слоты 0/1/3 flash ПЛИС Tang, git-SHA содержимого слота против .bin в репозитории;
//   4 Tang SD — файлы CORES на microSD Tang;
//   5 Evo SD  — плагины VDAC4 в /WC и модули ESP в /WC/ESP на карте Evo (VFS через Z80, как WC Update).
// Ставится всё со сверкой: что не сошлось — не ставится, прежнее остаётся. ESP — последней:
// поставив её, обновлятор просит перезапуск (restartPending).

enum class VupKind : uint8_t {
  kEsp = 1,
  kBl616 = 2,
  kSlot = 3,
  kTangFile = 4,
  kEvoFile = 5,
};

// Номера — как WcStatus (плагин держит одну таблицу слов), плюс kOlder.
enum class VupStatus : uint8_t {
  kUnknown = 0,
  kSame = 1,
  kDifferent = 2,
  kNew = 3,
  kReadError = 7,   // не прочиталось (нет связи с Tang, карты и т. п.): ставить нельзя
  kUpdated = 8,
  kFailed = 9,      // поставить не удалось, прежнее цело: можно повторить
  kOlder = 10,      // опубликованная старше стоящей: понижение запрещено
};

// Флаги строки в событии #68.
constexpr uint8_t kVupFlagRemote = 1;
constexpr uint8_t kVupFlagLocal = 2;
constexpr uint8_t kVupFlagCan = 8;
constexpr uint8_t kVupFlagAuto = 16;
constexpr uint8_t kVupFlagReboot = 32;

// Ход установки части платы: сделано из всего (байты).
using VupProgress = void (*)(void* context, uint32_t done, uint32_t total);

// Плата — всё, что не GitHub и не карта Evo. На ESP — vdac4_board_esp.cpp (tang_flash, tang_sd,
// bl616_push, OTA), на ПК — модель стенда. Ошибки — текстом в error.
class Vdac4Board {
 public:
  virtual ~Vdac4Board() = default;

  // --- ESP -------------------------------------------------------------------------------
  virtual const char* espVersion() = 0;          // "vdac4-0.1.123"
  virtual const char* releaseFormat() = 0;       // board::kReleaseVersionFormat
  // SHA-256 первых size байтов работающего раздела приложения: образ ESP целиком, как
  // firmware.bin (OTA и прошивальщик пишут его как есть).
  virtual bool espImageSha256(uint32_t size, uint8_t digest[32]) = 0;
  // firmware.bin (путь на raw.githubusercontent.com) — в свободный раздел OTA со сверкой SHA-256;
  // загрузочным он становится только при совпадении. Перезапуск — отдельно (espRestart).
  virtual bool espInstall(const char* rawPath, const uint8_t sha256[32], VupProgress progress,
                          void* context, char* error, size_t errorSize) = 0;
  virtual void espRestart() = 0;

  // --- Tang: LINK, flash, BL616 ------------------------------------------------------------
  virtual bool tangVersion(uint32_t& version) = 0;   // регистр VERSION (0x04)
  virtual bool bl616Info(uint32_t& info) = 0;        // регистр CP_INFO (0x30)
  // Мост к flash Tang есть (работает TS-Config с мостом): nullptr; иначе — причина.
  virtual const char* flashReady() = 0;
  virtual bool flashRead(uint32_t address, uint8_t* data, size_t length) = 0;
  // Слот целиком: стереть блоки, записать, прочитать обратно и сравнить.
  virtual bool flashWriteSlot(uint32_t base, const uint8_t* image, size_t size,
                              VupProgress progress, void* context, char* error,
                              size_t errorSize) = 0;
  // Прошивка BL616 через почтовый ящик ядра Tang; newInfo — CP_INFO новой прошивки.
  virtual bool bl616Push(const uint8_t* image, size_t size, uint32_t& newInfo, char* error,
                         size_t errorSize) = 0;

  // --- microSD Tang ------------------------------------------------------------------------
  // Карта готова к файлам (TS-Config 1.30+, не отдана WC): nullptr; иначе — причина.
  virtual const char* tangSdReady() = 0;
  // Пути — от корня карты ("CORES/NES.BIN").
  virtual bool tangSdStat(const char* path, bool& exists, uint32_t& size) = 0;
  virtual bool tangSdRead(const char* path, uint8_t* data, uint32_t size) = 0;
  // Через <путь>.tmp: записать, прочитать обратно и сверить, затем заменить прежний.
  virtual bool tangSdWrite(const char* path, const uint8_t* data, uint32_t size, char* error,
                           size_t errorSize) = 0;
};

struct VupRow {
  static constexpr size_t kNameSize = 31;      // до 30 знаков в событии
  static constexpr size_t kTextSize = 12;      // «текущее», «доступное» — до 11 знаков
  VupKind kind = VupKind::kEsp;
  VupStatus status = VupStatus::kUnknown;
  bool remote = false;
  bool local = false;
  bool autoMark = false;
  char name[kNameSize] = {};
  char current[kTextSize] = {};
  char available[kTextSize] = {};
  char repo[GitTreeEntry::kPathSize] = {};      // путь в репозитории
  char target[GitTreeEntry::kPathSize] = {};    // путь на карте (вид 4, 5) от корня, без '/'
  uint8_t sha[Sha1::kDigestSize] = {};          // git-SHA-1 эталона
  uint32_t size = 0;                            // длина эталона
  uint32_t localSize = 0;
  uint8_t slot = 0;                             // вид 3
  uint8_t espSha256[32] = {};                   // вид 1: SHA-256 firmware.bin из описи
  unsigned bl616Version = 0;                    // вид 2: опубликованный номер
  char reason[40] = {};                         // причина отказа или сомнения
};

class Vdac4Updater {
 public:
  static constexpr size_t kMaxRows = 96;
  static constexpr size_t kMaxTreeJson = 2U * 1024U * 1024U;
  static constexpr size_t kMaxFile = 2U * 1024U * 1024U;
  static constexpr size_t kMaxTreeEntries = 128;   // отобранных правилами записей
  static constexpr unsigned kDownloadAttempts = 3;
  static constexpr uint32_t kProgressIntervalMs = 250;
  static constexpr const char* kRepo = "andrewinsidelazarev/VDAC4";
  static constexpr const char* kBranch = "main";
  static constexpr uint8_t kStartEvo = 1;          // флаг START: проверять и карту Evo

  // Этапы события #67: как у WC Update, плюс 7 — ESP перезапускается своей новой прошивкой.
  static constexpr uint8_t kPhaseGithub = 1;
  static constexpr uint8_t kPhaseLocal = 2;
  static constexpr uint8_t kPhaseCheck = 3;
  static constexpr uint8_t kPhaseReady = 4;
  static constexpr uint8_t kPhaseApply = 5;
  static constexpr uint8_t kPhaseSync = 6;
  static constexpr uint8_t kPhaseRestart = 7;
  static constexpr uint8_t kPhaseError = 0xFF;

  using EventSink = WcUpdater::EventSink;
  using Clock = uint32_t (*)();

  // bridge == nullptr — карты Evo нет (флаг START без бита 0 её и не просит).
  Vdac4Updater(VfsBridge* bridge, WcFetcher& fetcher, Vdac4Board& board, EventSink sink,
               void* context, Clock clock);
  ~Vdac4Updater();

  Vdac4Updater(const Vdac4Updater&) = delete;
  Vdac4Updater& operator=(const Vdac4Updater&) = delete;

  // [флаги]: бит 0 — и карта Evo.
  bool configure(const uint8_t* payload, uint16_t length, char* error, size_t errorSize);
  // Эталон с GitHub, опись платы, сверка; события строк и итог kPhaseReady.
  bool check();
  // Поставить строки с указанными номерами (неподходящие пропускаются) в безопасном порядке:
  // карта Evo, карта Tang, BL616, слоты 1, 3, 0, ESP. ESP поставлена — restartPending().
  bool apply(const uint8_t* indices, size_t count);
  bool resend();
  void requestStop() { stop_ = true; }
  const volatile bool* stopFlag() const { return &stop_; }
  bool restartPending() const { return restart_; }

  size_t rowCount() const { return rowCount_; }
  const VupRow* row(size_t index) const { return index < rowCount_ ? &rows_[index] : nullptr; }
  const char* commit() const { return commit_; }
  const char* lastError() const { return error_; }

  static bool canInstall(const VupRow& row);
  static uint8_t flagsOf(const VupRow& row);
  // Правила раздела 2 DESIGN.md: нужна ли запись дерева и куда она ставится.
  // kind — вид строки; target — путь на карте (виды 4, 5) или пусто.
  static bool classify(const char* repoPath, VupKind& kind, char* target, size_t targetSize,
                       uint8_t& slot);
  // '*' — любая часть имени в пределах одной части пути (без '/').
  static bool globMatch(const char* pattern, const char* text);
  // Последний #define FW_VERSION в тексте app/main.c BL616; -1 — нет.
  static int parseBl616Version(const char* text, size_t length);
  // Версия слота из таблицы README прошивки Tang: «](firmware/<основа имени>.» и затем «| x.yz».
  static bool parseSlotVersion(const char* text, size_t length, const char* fileName,
                               char* version, size_t versionSize);

 private:
  bool fetchTree();
  bool buildRows();
  bool addRow(const GitTreeEntry& entry, VupKind kind, const char* target, uint8_t slot);
  bool fetchText(const char* repoPath, char* buffer, size_t capacity, size_t& length);
  bool rawGet(const char* repoPath, uint8_t* buffer, size_t capacity, size_t& length);
  bool download(VupRow& row, uint8_t* buffer);
  void checkEsp(VupRow& row);
  void checkBl616(VupRow& row);
  void checkSlot(VupRow& row);
  void checkTangFile(VupRow& row);
  bool checkEvoFiles();
  bool installRow(size_t index);
  bool installEvo(VupRow& row);
  bool installTang(VupRow& row);
  bool installBl616(VupRow& row);
  bool installSlot(VupRow& row);
  bool installEsp(VupRow& row);

  bool sendState(uint8_t phase, uint16_t current, uint16_t total, const char* format, ...);
  bool sendEntry(size_t index);
  bool sendReady();
  void beginProgress(uint32_t total);
  void addProgress(uint32_t bytes);
  static void progressHook(void* context, uint32_t done, uint32_t total);
  static void storeProgress(void* context, uint32_t bytes);
  static bool storeDownload(void* context, uint8_t* buffer);
  void fail(const char* format, ...);
  void setReason(VupRow& row, const char* format, ...);
  bool rawPath(const char* repoPath, char* output, size_t capacity) const;

  VfsBridge* bridge_;
  WcFetcher& fetcher_;
  Vdac4Board& board_;
  EventSink sink_;
  void* context_;
  Clock clock_;
  VfsFileStore* store_;
  volatile bool stop_;
  bool evo_;
  bool restart_;
  char commit_[41];
  char error_[128];
  VupRow* rows_;
  size_t rowCount_;
  GitTreeEntry* tree_;
  size_t treeCount_;
  uint8_t* buffer_;            // скачанный файл или образ, kMaxFile + 1
  char* json_;                 // ответ API дерева
  OnlineUpdateManifest manifest_;
  bool manifestOk_;
  char slotVersions_[3][VupRow::kTextSize];
  // последнее состояние — для SYNC
  uint8_t lastState_[64];
  uint16_t lastStateLength_;
  // ход
  uint32_t progressTotal_;
  uint32_t progressDone_;
  uint32_t progressBase_;      // progressDone_ до части, о которой сообщает progressHook
  uint32_t progressPart_;      // её длина в байтах (ход части приходит в своих единицах)
  uint8_t progressPercent_;
  uint32_t progressAt_;
  size_t currentRow_;          // строка, которую ставит installEvo (для storeDownload)
  uint16_t applyIndex_;
  uint16_t applyTotal_;
};

}  // namespace zifi
