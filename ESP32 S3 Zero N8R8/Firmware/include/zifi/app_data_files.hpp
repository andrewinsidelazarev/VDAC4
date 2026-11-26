#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <atomic>
#include <mutex>

namespace zifi {

// Файлы данных приложений с карты Evo на флешке USB (0.1.165, ABI 1.20) — общий механизм, как у ELF и
// ядер ПЛИС: программа Z80 (ESP App SPG и другие) шлёт файл приложения (у Quake 2 — PAK0.PAK, 50–180 МБ)
// тем же MOD_QUERY / MOD_PUT / MOD_COMMIT, но с флагом 0x04 и именем приложения; ESP кладёт его в
// <root>/<ПРИЛОЖЕНИЕ>/<ИМЯ> (на плате root — /usb/SYSTEM/CACHE, рядом с кэшем музыки) со временем файла
// с карты и в следующий раз, если длина та же и копия не старее файла на карте, его не просит.
// Приложение читает такие файлы функциями zm_core_t data_open / data_read / data_close.
//
// В отличие от ELF и ресурсов файл не держится в PSRAM целиком (её 8 МБ): каждый кусок MOD_PUT сразу
// пишется в WRITING.TMP папки приложения, MOD_COMMIT переименовывает его в настоящее имя. Пропало
// питание посреди передачи — под настоящим именем остаётся прежняя копия или ничего.
//
// Флешка пишется и читается кусками не больше kChunkBytes (буфер драйвера USB — во внутренней памяти,
// usb_drive.hpp). Открытых файлов у тома FatFs мало (max_files = 4 на всю прошивку), поэтому дескриптор
// приложения не держит файл открытым: открыт только последний читавшийся (один FILE*), остальные
// открываются заново при чтении.
//
// Флешки нет (0.1.166), а с 0.1.167 — всегда, если программа Z80 умеет, — файл остаётся на карте Evo, и
// ESP читает его по запросу у той программы Z80, что его прислала бы: та сама называет в MOD_QUERY номер
// файла (байт за именем приложения) — значит, умеет. (0.1.167: копия на флешку для таких программ не
// делается и при вставленной флешке — пользователь 24.11.2026: pak Quake 2 копировался часами.) ESP ставит файл на учёт (addRemote, ответ kRemote), а когда приложение читает, просит у
// программы Z80 куски через обработчик Fetch (на плате — событие kEventDataRead и ответ kDataPiece,
// module_app.cpp). Куски — целыми секторами карты: смещение кратно 512, до kRemoteRequestMax байтов за
// запрос; короткие и невыровненные чтения приложения берутся из блока kRemoteBlock байтов.
//
// Поиск на флешке (0.1.175; пользователь 26.11.2026: «Если игра находит файлы данных в любой из вложенных
// папок в OTG Flash Drive /SYSTEM/CACHE/* то начинает читать их оттуда. Сама игра эти файлы туда не
// копирует»). open ищет файл сначала в <root>/<ПРИЛОЖЕНИЕ>/, потом в самом <root> и во всех вложенных
// папках на любой глубине (до kMaxScanDepth), по алфавиту без учёта регистра. Нашёлся — читается с
// флешки, даже если такой же файл стоит на учёте с карты Evo. Папка, где нашёлся первый файл
// приложения, — его «домашняя»: остальные файлы ищутся только в ней (у Quake 2 PAK0.PAK демо и полной
// версии разные — смешивать нельзя). Найденное и ненайденное помнится до выгрузки приложения
// (closeAll), смены флешки (setVolume) или нового файла на ней (commit): Quake 2 открывает pak заново
// на каждый файл внутри и ищет несуществующие pak3…pak9 — обход флешки на каждый вызов замедлил бы
// загрузку уровня. В журнал — одна строка на файл: откуда он читается.
//
// Переносимый код (stdio, stat, utime, rename, обход каталогов): работает в прошивке поверх FatFs и в
// тесте на ПК (tests/app_data_files_host.cpp) поверх папки ПК.
class AppDataFiles {
 public:
  // root — каталог, в котором лежат папки приложений, без «/» в конце (на плате /usb/SYSTEM/CACHE).
  explicit AppDataFiles(const char* root);

  // Имя приложения — имя его ELF без «.ELF»: 1–8 знаков A–Z, 0–9, «_», «-» (регистр не важен).
  static bool validApp(const char* app);
  // Имя файла — 8.3: 1–8 знаков, точка и 0–3 знака расширения; те же знаки.
  static bool validName(const char* name);

  // Ответ на MOD_QUERY с флагом 0x04 (как у ELF: 0 есть, 1 шли, 2 нет места, 3 плохое имя; 4 — флешки
  // нет, 5 — флешки нет, файл читается по запросу у программы Z80; 4 и 5 решает вызывающий).
  enum : uint8_t { kHave = 0, kSend = 1, kNoRoom = 2, kBadName = 3, kNoDrive = 4, kRemote = 5 };

  // Начать доставку: годная копия есть — kHave; иначе открыть WRITING.TMP в папке приложения (папка
  // создаётся) — kSend. freeBytes — свободно на томе (прежняя копия того же имени уйдёт — её длина
  // тоже в счёт). Прежняя незаконченная доставка бросается.
  uint8_t begin(const char* app, const char* name, uint32_t size, uint32_t stamp, uint64_t freeBytes);
  // Кусок доставки — только по порядку (offset = сколько уже принято) и не за конец файла.
  bool put(uint32_t offset, const uint8_t* data, uint32_t length);
  uint32_t received() const { return received_; }
  bool active() const { return out_ != nullptr; }
  // Всё принято — закрыть, переименовать в настоящее имя, поставить время файла с карты. Иначе — бросить.
  bool commit();
  // Бросить доставку: WRITING.TMP удаляется.
  void drop();

  // Итоги функций приложения — как у zm_core_t (ZM_OK, ZM_ERROR, ZM_NOT_FOUND).
  enum : int { kOk = 0, kError = -1, kNotFound = -3 };
  // Открыть файл приложения app: *file — дескриптор, *size — длина. kNotFound — копии нет.
  int open(const char* app, const char* name, void** file, uint32_t* size);
  // До count байтов с offset: *got — сколько (меньше count только у конца файла). kError — отказ флешки.
  int read(void* file, uint32_t offset, void* data, uint32_t count, uint32_t* got);
  void close(void* file);
  // Закрыть все дескрипторы (выгрузка приложения, флешку вынули).
  void closeAll();

  // --- файлы на карте Evo, которые читаются по запросу у программы Z80 (0.1.166) ---
  // Поставить на учёт: index — номер файла у программы Z80 (его она увидит в запросе). Тот же файл
  // приложения заменяется. false — плохое имя или учёт полон (kMaxRemote).
  bool addRemote(const char* app, const char* name, uint32_t size, uint32_t stamp, uint8_t index);
  // Снять с учёта: файл пришёл на флешку.
  void forgetRemote(const char* app, const char* name);
  // Снять всё (выгрузка приложения: программа Z80 больше не отвечает).
  void clearRemote();
  // Достать у программы Z80 count байтов файла index с offset в data. Вызывающий обещает: offset
  // кратен kSectorBytes, count — от 1 до kRemoteRequestMax, offset + count — не за концом файла.
  // true — пришли все байты.
  using Fetch = bool (*)(void* context, uint8_t index, uint32_t offset, void* data, uint32_t count);
  // Обработчик и буфер блока на kRemoteBlock байтов (на плате — в PSRAM). Без них файлы с учёта не
  // открываются.
  void setFetch(Fetch fetch, void* context, uint8_t* block);

  // --- поиск на флешке (0.1.175) ---
  // Том для поиска: stamp — номер монтирования флешки (0 — флешки нет: на ней не ищется). Сменился —
  // всё найденное забыто. По умолчанию 1 (тест на ПК: «флешка» — папка ПК).
  void setVolume(uint32_t stamp);
  // Строки журнала: откуда читается файл (одна на файл за запуск приложения). На плате — bringupLog.
  using Log = void (*)(void* context, const char* line);
  void setLog(Log log, void* context);
  // Найти файл name приложения app на флешке (порядок — выше, у класса). true — полный путь в path,
  // длина в *length. *fresh — файл искали сейчас, а не взяли из запомненного (для журнала; можно nullptr).
  bool findOnDrive(const char* app, const char* name, char* path, size_t size, uint32_t* length,
                   bool* fresh);

  static constexpr size_t kMaxOpen = 8;
  static constexpr size_t kMaxRemote = 8;
  static constexpr uint32_t kChunkBytes = 4096;
  static constexpr uint32_t kSectorBytes = 512;
  static constexpr uint32_t kRemoteBlock = 8192;
  static constexpr uint32_t kRemoteRequestMax = 32768;
  static constexpr const char* kTempName = "WRITING.TMP";
  // Поиск: папок не больше kMaxScanDirs (с папкой приложения), глубина от <root> — до kMaxScanDepth,
  // путь папки — до kDirPath байтов; запомненных имён за запуск — до kMaxKnown.
  static constexpr size_t kMaxScanDirs = 64;
  static constexpr int kMaxScanDepth = 8;
  static constexpr size_t kDirPath = 192;
  static constexpr size_t kMaxKnown = 32;

 private:
  struct Handle {
    bool used = false;
    bool remote = false;         // файл на карте Evo: читается у программы Z80
    uint8_t remoteIndex = 0;     // его номер у программы Z80
    uint32_t size = 0;
    char path[300] = {};
  };
  struct Remote {
    bool used = false;
    uint8_t index = 0;
    uint32_t size = 0;
    uint32_t stamp = 0;
    char app[9] = {};
    char name[13] = {};
  };

  // Имя, которое уже искали за этот запуск: dir — номер папки в dirs_ (-1 — на флешке нет).
  struct Known {
    char name[13] = {};
    int dir = -1;
    uint32_t length = 0;
  };

  bool folderOf(const char* app, char* out, size_t size) const;
  void closeCached();
  // Поиск (под searchLock_): забыть всё; начать поиск файлов приложения upperApp; список папок.
  void resetSearch();
  bool prepareSearch(const char* upperApp);
  void listDirs();
  const char* dirAt(size_t index) const { return dirs_ + index * kDirPath; }
  void log(const char* format, ...);
  int readRemote(uint8_t index, uint32_t size, uint32_t offset, uint8_t* out, uint32_t count,
                 uint32_t* got);

  char root_[240];
  // доставка (поток с карты Evo — только из задачи обмена с Z80)
  FILE* out_ = nullptr;
  char outDir_[260] = {};
  char outName_[13] = {};
  uint32_t outSize_ = 0;
  uint32_t outStamp_ = 0;
  uint32_t received_ = 0;
  // чтение (задачи приложения)
  std::mutex lock_;
  Handle handles_[kMaxOpen];
  FILE* cached_ = nullptr;     // открытый файл последнего чтения
  int cachedIndex_ = -1;
  // учёт файлов на карте Evo (под lock_); поколение растёт при каждой его смене — блок прежнего
  // поколения негоден (под тем же номером у Z80 мог оказаться другой файл)
  Remote remotes_[kMaxRemote];
  std::atomic<uint32_t> generation_{1};
  // чтение у программы Z80: по одному запросу за раз (remoteLock_ держится и на время ожидания
  // ответа — поэтому lock_ им не занят, и closeAll из задачи обмена не ждёт Z80)
  std::mutex remoteLock_;
  Fetch fetch_ = nullptr;
  void* fetchContext_ = nullptr;
  uint8_t* block_ = nullptr;   // блок kRemoteBlock: последние прочитанные байты файла
  int blockIndex_ = -1;         // какого файла (номер у Z80), -1 — пуст
  uint32_t blockGeneration_ = 0;
  uint32_t blockAt_ = 0;
  uint32_t blockLength_ = 0;
  // поиск на флешке (0.1.175; задачи приложения и обмена с Z80 — под searchLock_)
  std::mutex searchLock_;
  std::atomic<uint32_t> volume_{1};
  std::atomic<bool> resetPending_{false};  // closeAll не дождался замка: сбросить при следующем поиске
  uint32_t searchVolume_ = 0;  // для какого тома запомненное
  bool searching_ = false;     // запомненное годно (searchApp_, searchVolume_)
  char searchApp_[9] = {};
  // Папки поиска (12 КБ — в самом объекте, а он на плате в PSRAM): [0] — папка приложения.
  char dirs_[kMaxScanDirs * kDirPath] = {};
  size_t dirCount_ = 0;
  bool dirsListed_ = false;    // остальные папки уже обойдены
  int home_ = -1;              // «домашняя» папка приложения (номер в dirs_), -1 — ещё нет
  Known known_[kMaxKnown];
  size_t knownCount_ = 0;
  Log log_ = nullptr;
  void* logContext_ = nullptr;
};

// Один запрос куска файла у программы Z80 и приём ответа (0.1.166). Событие ESP → Z80 kEventDataRead:
// [номер запроса][номер файла][смещение LE32][длина LE16]. Ответ Z80 → ESP kDataPiece: [номер запроса]
// [смещение LE32][данные] — одним куском DMA до 8 КБ (raw media) или кадрами до 1 КБ; кусков на запрос
// сколько угодно, по порядку. Кусок без данных — программа Z80 файл не прочитала. Номер запроса
// отсекает запоздавшие куски прежнего (повторённого) запроса. Замок — у вызывающего: begin и end — в
// задаче приложения, piece — в задаче обмена с Z80.
class AppDataRequest {
 public:
  static constexpr size_t kEventBytes = 8;
  static constexpr size_t kPiecePrefix = 5;
  enum Result { kIgnored, kTaken, kDone, kFailed };

  static void event(uint8_t* out, uint8_t seq, uint8_t index, uint32_t offset, uint32_t count);
  void begin(uint8_t seq, uint32_t offset, uint32_t count, uint8_t* dest);
  // Дальше куски не принимаются (ответ пришёл, срок вышел, приложение выгружается).
  void end() { active_ = false; }
  Result piece(const uint8_t* body, size_t length);
  bool complete() const { return count_ != 0 && received_ == count_; }
  bool failed() const { return failed_; }

 private:
  bool active_ = false;
  bool failed_ = false;
  uint8_t seq_ = 0;
  uint32_t offset_ = 0;
  uint32_t count_ = 0;
  uint32_t received_ = 0;
  uint8_t* dest_ = nullptr;
};

}  // namespace zifi
