// Стенд приложения PLAYER.ELF на ПК (test_player_host.py): заглушка ядра
// (zm_core_t) и настоящие декодеры MP3.ELF (Helix), AAC.ELF (Helix AAC), OGG.ELF
// (stb_vorbis), TRACKER.ELF, MIDI.ELF и OPL.ELF — те же исходники, что
// собираются в ELF, только точки входа переименованы. Банк MIDI (ресурс MIDI.SF2) — файл из --bank, ПЗУ OPL4
// (ресурс YRW801.ROM) — из --rom, другие ресурсы (набор MWK) — --resource
// ИМЯ=файл; --ext — расширение файла в AUDIO_OPEN (у потока — за длиной и флагами:
// AAC и OGG выбирают декодер); --no-aac 1, --no-ogg 1 — такого декодера нет в кэше.
// Сценарий radio — интернет-радио
// (плейлист M3U, станции — сеть стенда: --net-file MP3, --net-title, --net-cut и др.;
// --seek N — листнуть станции, --list-wait 1 — дождаться конца их списка).
// Модель вывода — как AudioPlayer::pcmWrite: очередь ограничена (--pcm-ms МС; без
// него 4096 кадров, как у очереди во внутренней памяти; в PSRAM у платы — 1486 мс),
// сливается со скоростью звука (умноженной на --speed), на паузе не сливается,
// запись ждёт места до 500 мс и возвращает, сколько кадров взято. Провалы — очередь
// опустела посреди звука (не до первого кадра и не после pcm_flush): pcm_gaps и
// pcm_gap_ms.
// Вызов: player_host <сценарий> [ключи] <файл>; на выходе строки ключ=значение.
//   mp3     — AUDIO_OPEN (с --sized 1 — с длиной потока), файл кадрами AUDIO_DATA
//             (--first N: первый блок N байт и пауза — хвост сектора за меткой ID3),
//             длительность из AUDIO_VIEW во время звука, AUDIO_CLOSE с доигрыванием;
//   flush   — то же, но после --cut байтов AUDIO_FLUSH(--base мс) и подача с --resume;
//   pause   — --pauses пауз посреди звука, каждая на --hold мс;
//   stop    — выгрузка приложения посреди звука;
//   module  — модуль (или MIDI с --midi 1) целиком, AUDIO_INFO, --seek позиций
//             (у MIDI — по 10 с), до конца песни;
//   missing — нет декодеров потока в кэше (с --ext — того, что по расширению):
//             AUDIO_OPEN должен отказать;
//   cover   — метка ID3 файла досылается AUDIO_COVER с того места, что просит
//             ESP; миниатюра — AUDIO_THUMB, название — AUDIO_INFO. JPEG сводит
//             модель (на ESP — TJpgDec из ПЗУ): «картинка» одного цвета;
//   picture — картинка файлом (cover.jpg из папки) кадрами AUDIO_PICTURE,
//             с одним повтором куска не с того места;
//   spectrum — анализатор спектра (view.c) на синусе --hz Гц;
//   release — AUDIO_OPEN [#20]: приложение освобождает ресурсы (банк, ПЗУ);
//   ui      — экран оверлеем Tang (ABI 1.5, --ui 1): тело AUDIO_STATUS → ui_model, метка ID3
//             файла (AUDIO_COVER) и звук, обложка из сети (--cover-json, --cover-jpeg).
// --psram N — PSRAM стенда, байтов (как у ESP: память кончается): в счёт идут
// core->alloc с ZM_MEM_PSRAM, память libxmp (xmp_host_alloc из xmp_esp_shim.c)
// и ресурсы (банк, ПЗУ, наборы), пока они не освобождены; на выходе psram_peak
// (с пробами make_room) и xmp_peak — пик памяти одной libxmp.
// --pause-at МС (module): после стольких мс звука — AUDIO_PAUSE [1] на --hold мс,
// затем [0]; на выходе frames_during_pause и paused_* (состояние на паузе).
// --cover-wait 1 (mp3): дождаться флага 64 (обложку нашёл декодер) и забрать
// миниатюру — cover_flag, thumb_state, thumb_uniform, thumb_byte.
// --stop-at N (module): после N байтов файла — AUDIO_CLOSE [0] посреди приёма;
// stop_ms — сколько он шёл (декодер в open потоком не должен держать стоп).
// --no-worker 1 — задачи на ядре 1 с приоритетом 5 (помощник PLAYER) ядро не даёт:
// декодеры считают всё на ядре 0, как с прошивкой без помощника.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>


#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "zifi/cover_art.hpp"

extern "C" {
#include "view.h"
#include "player_cover.h"
#include "screen.h"
#include "zm_app.h"
#include "zm_source.h"
int player_main(int argc, char* argv[]);
int mp3_main(int argc, char* argv[]);
int aac_main(int argc, char* argv[]);
int ogg_main(int argc, char* argv[]);
int flac_main(int argc, char* argv[]);
int tracker_main(int argc, char* argv[]);
int midi_main(int argc, char* argv[]);
int opl_main(int argc, char* argv[]);
int saa_main(int argc, char* argv[]);
int tsfm_main(int argc, char* argv[]);
}

// Модель TJpgDec: «JPEG» стенда — FF D8 FF и цвет RGB одним куском.
#ifndef ZIFI_REAL_COVER_JPEG
namespace zifi {
int decodeCoverJpeg(const uint8_t* jpeg, size_t size, CoverThumb& thumb, CoverDecodeInfo& info,
                    const volatile bool* cancel, void (*)()) {
  if (size < 6 || jpeg[0] != 0xFF || jpeg[1] != 0xD8 || jpeg[2] != 0xFF) {
    return 101;
  }
  if (cancel != nullptr && *cancel) {
    return 204;
  }
  info.width = info.height = CoverThumb::kSide;
  info.scale = 0;
  if (!thumb.begin(CoverThumb::kSide, CoverThumb::kSide)) {
    return 2;
  }
  static uint8_t rgb[CoverThumb::kSide * CoverThumb::kSide * 3];
  for (size_t index = 0; index < sizeof(rgb); index += 3) {
    rgb[index] = jpeg[3];
    rgb[index + 1] = jpeg[4];
    rgb[index + 2] = jpeg[5];
  }
  thumb.addBlock(0, 0, CoverThumb::kSide - 1, CoverThumb::kSide - 1, rgb);
  return 0;
}
}  // namespace zifi
#endif

namespace {

using Clock = std::chrono::steady_clock;

enum : uint8_t {
  kView = 0x2F,
  kOpen = 0x30, kData, kClose, kStatus, kTone, kVolume, kPause, kProbe, kSeek, kFlush, kInfo,
  kCover, kThumb, kPicture, kLinkProbe, kTags,
};

// --- память, задачи, синхронизация ---------------------------------------------

std::mutex allocLock;
std::set<void*> allocations;
size_t psramBudget = 0;                // --psram: 0 — без предела
size_t psramUsed = 0;                  // под allocLock
size_t psramPeak = 0;
// --internal N [--internal-block M]: внутренняя память ESP (как ModuleApp на плате): в счёт идут
// выделения ZM_MEM_INTERNAL (не влезли — PSRAM, как coreAlloc прошивки), стеки задач с их TCB
// (не влезли — задача не создаётся, ZM_NO_MEMORY) и мьютексы/сигналы FreeRTOS. M — самый большой
// свободный кусок: больший запрос не выделяется, даже если всего памяти хватает.
size_t internalBudget = 0;
size_t internalBlock = 0;
size_t internalUsed = 0;
size_t internalPeak = 0;
std::map<void*, size_t> internalBlocks;
constexpr size_t kTaskOverhead = 360;    // TCB FreeRTOS ESP-IDF
constexpr size_t kSyncBytes = 96;        // мьютекс или двоичный семафор
bool internalTakeLocked(size_t size) {
  if (internalBudget == 0) {
    return true;
  }
  if ((internalBlock != 0 && size > internalBlock) || internalUsed + size > internalBudget) {
    return false;
  }
  internalUsed += size;
  if (size >= 1024 && getenv("INTERNAL_TRACE") != nullptr) {
    fprintf(stderr, "internal +%zu = %zu\n", size, internalUsed);
  }
  if (internalUsed > internalPeak) {
    internalPeak = internalUsed;
  }
  return true;
}
size_t xmpUsed = 0;                    // память libxmp (xmp_host_alloc), под allocLock
size_t xmpPeak = 0;
std::map<void*, size_t> xmpBlocks;
std::map<void*, size_t> psramBlocks;   // под allocLock
size_t residentResources();            // байтов ресурсов в памяти (ниже)

// Взять size байтов PSRAM стенда (под allocLock). false — не помещается.
bool psramTakeLocked(size_t size) {
  if (psramBudget != 0 && psramUsed + residentResources() + size > psramBudget) {
    return false;
  }
  psramUsed += size;
  if (psramUsed + residentResources() > psramPeak) {
    psramPeak = psramUsed + residentResources();
  }
  return true;
}
std::atomic<int> tasks{0};
std::atomic<int> modulesOpen{0};
std::atomic<int> moduleLoads{0};
bool noMp3 = false;
bool noAac = false;
bool noOgg = false;
bool noFlac = false;
bool noWorker = false;

void* coreAlloc(size_t size, uint32_t caps) {
  bool psram = (caps & ZM_MEM_INTERNAL) == 0;
  std::lock_guard<std::mutex> lock(allocLock);
  bool internal = false;
  if (!psram) {
    internal = internalTakeLocked(size);
    psram = !internal;                   // внутренней нет — PSRAM (coreAlloc прошивки)
    if (psram && internalBudget != 0) {
      fprintf(stderr, "internal: %zu bytes -> PSRAM (used %zu of %zu)\n", size, internalUsed, internalBudget);
    }
  }
  if (psram && !psramTakeLocked(size)) {
    return nullptr;
  }
  void* pointer = malloc(size);
  if (pointer != nullptr) {
    allocations.insert(pointer);
    if (psram) {
      psramBlocks[pointer] = size;
    }
    if (internal && internalBudget != 0) {
      internalBlocks[pointer] = size;
    }
  } else if (psram) {
    psramUsed -= size;
  }
  return pointer;
}

void* coreRealloc(void* pointer, size_t size) {
  std::lock_guard<std::mutex> lock(allocLock);
  auto block = psramBlocks.find(pointer);
  const bool psram = block != psramBlocks.end();
  const size_t old = psram ? block->second : 0;
  if (psram) {
    psramUsed -= old;
    if (!psramTakeLocked(size)) {
      psramUsed += old;
      return nullptr;
    }
    psramBlocks.erase(block);
  }
  allocations.erase(pointer);
  void* moved = realloc(pointer, size);
  if (moved != nullptr) {
    allocations.insert(moved);
    if (psram) {
      psramBlocks[moved] = size;
    }
  } else {
    allocations.insert(pointer);
    if (psram) {
      psramUsed -= size;
      psramUsed += old;
      psramBlocks[pointer] = old;
    }
  }
  return moved;
}

void coreFree(void* pointer) {
  if (pointer == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(allocLock);
    if (allocations.erase(pointer) == 0) {
      fprintf(stderr, "free of a pointer the core did not give\n");
      exit(3);
    }
    auto block = psramBlocks.find(pointer);
    if (block != psramBlocks.end()) {
      psramUsed -= block->second;
      psramBlocks.erase(block);
    }
    auto inner = internalBlocks.find(pointer);
    if (inner != internalBlocks.end()) {
      internalUsed -= inner->second;
      internalBlocks.erase(inner);
    }
  }
  free(pointer);
}

void coreLog(const char* format, ...) {
  va_list args;
  va_start(args, format);
  fprintf(stderr, "log: ");
  vfprintf(stderr, format, args);
  fprintf(stderr, "\n");
  va_end(args);
}

uint32_t coreMillis() {
  return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   Clock::now().time_since_epoch()).count());
}

void coreDelay(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int coreTaskStart(zm_task_fn fn, void* arg, uint32_t stack, uint8_t priority, int8_t core) {
  if (noWorker && core == 1 && priority == 5) {
    return ZM_ERROR;                   // помощник PLAYER на ядре 1
  }
  const size_t bytes = (stack < 2048 ? 2048 : stack) + kTaskOverhead;
  {
    std::lock_guard<std::mutex> lock(allocLock);
    if (!internalTakeLocked(bytes)) {
      fprintf(stderr, "internal: task stack %u not allocated (used %zu of %zu, block %zu)\n",
              static_cast<unsigned>(stack), internalUsed, internalBudget, internalBlock);
      return ZM_NO_MEMORY;             // как coreTaskStart прошивки: xTaskCreate не создал
    }
  }
  ++tasks;
  std::thread([fn, arg, bytes] {
    fn(arg);
    std::lock_guard<std::mutex> lock(allocLock);
    if (internalBudget != 0) internalUsed -= bytes;
    --tasks;
  }).detach();
  return ZM_OK;
}

bool syncTake() {
  std::lock_guard<std::mutex> lock(allocLock);
  return internalTakeLocked(kSyncBytes);
}

void* coreMutexCreate() { return syncTake() ? new std::mutex() : nullptr; }
void coreMutexLock(void* mutex) { static_cast<std::mutex*>(mutex)->lock(); }
void coreMutexUnlock(void* mutex) { static_cast<std::mutex*>(mutex)->unlock(); }

struct Signal {
  std::mutex lock;
  std::condition_variable wake;
  bool given = false;
};

void* coreSignalCreate() { return syncTake() ? new Signal() : nullptr; }

void coreSignalGive(void* handle) {
  Signal* signal = static_cast<Signal*>(handle);
  std::lock_guard<std::mutex> lock(signal->lock);
  signal->given = true;
  signal->wake.notify_one();
}

int coreSignalWait(void* handle, uint32_t timeout) {
  Signal* signal = static_cast<Signal*>(handle);
  std::unique_lock<std::mutex> lock(signal->lock);
  if (!signal->wake.wait_for(lock, std::chrono::milliseconds(timeout),
                             [signal] { return signal->given; })) {
    return ZM_TIMEOUT;
  }
  signal->given = false;
  return ZM_OK;
}

// --- ответы Z80 -------------------------------------------------------------------

bool inCommand = false;
int replyCode = -1;
std::vector<uint8_t> replyData;
int repliesOutside = 0;

// app_done (ABI 1.6): как прошивка — только из command; ответив, ядро выгружает приложение.
bool appDoneAsked = false;
int appDones = 0;
bool appUnloaded = false;              // выгружено по app_done: команд и второго stop больше нет
void coreAppDone() {
  if (inCommand) appDoneAsked = true;
}

int coreReply(uint8_t response, const void* data, uint16_t length) {
  if (!inCommand) {
    ++repliesOutside;
    return ZM_ERROR;
  }
  replyCode = response;
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  replyData.assign(bytes, bytes + length);
  return ZM_OK;
}

int coreEvent(uint8_t, const void*, uint16_t, uint32_t) { return ZM_ERROR; }

// --- вывод ----------------------------------------------------------------------------

struct Sink {
  std::mutex lock;
  uint32_t rate = 0;
  uint32_t channels = 0;
  double queued = 0;                   // кадров в очереди
  double capacity = 4096;              // как input_ AudioPlayer: 16 КБ int16 стерео
  uint32_t capacityMs = 0;             // --pcm-ms: ёмкость по частоте pcm_open
  bool armed = false;                  // звук шёл: опустевшая очередь — провал
  bool inGap = false;
  int gaps = 0;
  double gapFrames = 0;
  Clock::time_point last = Clock::now();
  bool paused = false;
  double speed = 100;
  uint64_t frames = 0;                 // кадров принято за всё время
  // Принятые расходятся на проигранные (ушли из очереди со скоростью звука) и
  // выброшенные (лежали в очереди при сбросе или закрытии): «все кадры дошли до
  // выхода» — это проигранные, а не принятые.
  double played = 0;
  double discarded = 0;
  uint64_t writes = 0;
  uint64_t firstWrite = 0;             // кадров в первой записи (PLAYER 1.20 копит начало)
  uint64_t timeouts = 0;               // записей, вернувших меньше, чем просили
  float peak = 0;
  int64_t firstToneFrame = -1;          // первая слышимая нота: проверка AUDIO_VIEW
  int opens = 0;
  int flushes = 0;
  int closes = 0;
  int pauses = 0;
  FILE* raw = nullptr;

  void drainLocked() {
    const Clock::time_point now = Clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - last).count();
    last = now;
    if (!paused && rate != 0) {
      const double need = ms * rate * speed / 1000.0;
      if (armed && need > queued) {
        gapFrames += need - queued;
        if (!inGap) {
          ++gaps;
          inGap = true;
        }
      }
      played += need < queued ? need : queued;
      queued -= need;
      if (queued < 0) {
        queued = 0;
      }
    }
  }
} sink;

uint8_t volume = 255;

int corePcmOpen(const zm_pcm_format_t* format) {
  std::lock_guard<std::mutex> lock(sink.lock);
  sink.drainLocked();
  sink.rate = format->rate;
  sink.channels = format->channels;
  if (sink.capacityMs != 0) {
    sink.capacity = static_cast<double>(sink.capacityMs) * format->rate / 1000.0;
  }
  ++sink.opens;
  return ZM_OK;
}

int corePcmWrite(const float* frames, uint32_t count) {
  uint32_t done = 0;
  double waited = 0;                   // мс ожидания места, по часам: Sleep в Windows длиннее
  if (++sink.writes == 1) {
    sink.firstWrite = count;
  }
  while (done < count) {
    std::unique_lock<std::mutex> lock(sink.lock);
    if (sink.rate == 0) {
      return 0;
    }
    sink.drainLocked();
    const double room = sink.capacity - sink.queued;
    if (room < 1) {
      lock.unlock();
      if (waited >= 500) {
        break;
      }
      const Clock::time_point before = Clock::now();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      waited += std::chrono::duration<double, std::milli>(Clock::now() - before).count();
      continue;
    }
    uint32_t part = count - done;
    if (part > room) {
      part = static_cast<uint32_t>(room);
    }
    const uint32_t channels = sink.channels == 1 ? 1 : 2;
    for (uint32_t index = 0; index < part * channels; ++index) {
      const float sample = frames[done * channels + index];
      if (sink.firstToneFrame < 0 && fabsf(sample) > 0.001f) {
        sink.firstToneFrame = (int64_t)sink.frames + index / channels;
      }
      if (fabsf(sample) > sink.peak) {
        sink.peak = fabsf(sample);
      }
      if (sink.raw != nullptr) {
        float clipped = sample > 1.0f ? 1.0f : (sample < -1.0f ? -1.0f : sample);
        const int16_t value = static_cast<int16_t>(clipped * 32767.0f);
        fwrite(&value, 2, 1, sink.raw);
      }
    }
    sink.queued += part;
    sink.frames += part;
    done += part;
    if (part != 0) {
      sink.armed = true;
      sink.inGap = false;
    }
  }
  if (done < count) {
    ++sink.timeouts;
  }
  return static_cast<int>(done);
}

uint32_t corePcmQueuedMs() {
  std::lock_guard<std::mutex> lock(sink.lock);
  sink.drainLocked();
  return sink.rate == 0 ? 0 : static_cast<uint32_t>(sink.queued * 1000.0 / sink.rate);
}

void corePcmFlush() {
  std::lock_guard<std::mutex> lock(sink.lock);
  sink.drainLocked();
  sink.discarded += sink.queued;
  sink.queued = 0;
  sink.armed = false;                  // тишина после сброса (смена станции) — не провал
  ++sink.flushes;
}

void corePcmClose() {
  std::lock_guard<std::mutex> lock(sink.lock);
  sink.drainLocked();
  sink.discarded += sink.queued;
  sink.armed = false;
  sink.rate = 0;
  sink.queued = 0;
  ++sink.closes;
}

void corePcmPause(int paused) {
  std::lock_guard<std::mutex> lock(sink.lock);
  sink.drainLocked();
  if (paused && !sink.paused) {
    ++sink.pauses;
  }
  sink.paused = paused != 0;
}

void coreSetVolume(uint8_t value) { volume = value; }
uint8_t coreVolume() { return volume; }
uint16_t corePcmFaults() { return 0; }

int16_t coreTemperature() { return 475; }   // 47,5 °C: в AUDIO_STATUS — 48
int coreUiAvailable() { return 1; }          // --ui 1: Tang с оверлеями есть (ABI 1.5)

// --- LINK Tang (--ui 1): экран PLAYER (screen.cpp) пишет оверлей 1 и регион оверлеев ---------
// Модель — то, что видит Tang: регистры оверлея 1 (CTRL, POS, SIZE, BASE с 0xD0), регион оверлеев
// 131 072 слова (окно 0x80000…), каждое включение CTRL — его прозрачность. Запись куда-то ещё —
// ошибка экрана.
std::mutex linkLock;
std::vector<uint32_t> tangRegion(131072, 0xDEADBEEF);
std::vector<uint8_t> tangWritten(131072, 0);
uint32_t ovlRegs[4] = {};
std::vector<int> ovlAlphas;            // каждая запись CTRL: прозрачность или −1 (выкл.)
long long writtenBeforeOn = -1;        // слов региона записано к первому включению оверлея
long long linkWords = 0;
long long wordsAtOn = -1;              // linkWords к первому включению: дальше — кадры
int linkBad = 0;

int coreLinkWrite(uint32_t address, const uint32_t* words, uint32_t count) {
  std::lock_guard<std::mutex> lock(linkLock);
  if (words == nullptr || (address & 3) != 0 || count == 0 || count > 16) {
    ++linkBad;
    return ZM_ERROR;                   // как coreLinkWrite прошивки
  }
  if (address >= 0x80000 && address < 0x100000) {
    const uint32_t first = (address - 0x80000) / 4;
    if (first + count > tangRegion.size()) {
      ++linkBad;
      return ZM_ERROR;
    }
    for (uint32_t i = 0; i < count; ++i) {
      tangRegion[first + i] = words[i];
      tangWritten[first + i] = 1;
    }
    linkWords += count;
    return ZM_OK;
  }
  if (address >= 0xD0 && address + 4 * count <= 0xE0) {
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t reg = (address - 0xD0) / 4 + i;
      ovlRegs[reg] = words[i];
      if (reg != 0) continue;
      const bool on = (words[i] & 1) != 0;
      ovlAlphas.push_back(on ? static_cast<int>((words[i] >> 4) & 15) : -1);
      if (on && writtenBeforeOn < 0) {
        wordsAtOn = linkWords;
        writtenBeforeOn = 0;
        for (uint8_t w : tangWritten) writtenBeforeOn += w;
      }
    }
    return ZM_OK;
  }
  ++linkBad;
  return ZM_ERROR;
}

int coreLinkRead(uint32_t, uint32_t*, uint32_t) { return ZM_ERROR; }   // экран Tang не читает

// --- сеть (net_*): станции интернет-радио ------------------------------------------------
// Любой адрес отдаёт --net-file (MP3) по кругу; с «Icy-MetaData: 1» — метаданные ICY
// каждые --net-metaint байтов: первый блок — StreamTitle='--net-title', дальше пустые,
// а с --net-title2 третий — второе название. --net-type — Content-Type, --net-fail N —
// первые N соединений не удаются, --net-cut N — после N байтов звука обрыв,
// --no-net 1 — ядро без net_* (прошивка до 0.1.32). Скорость: --net-kbps N — звук
// идёт N кбит/с (с --speed — во столько же раз быстрее, как и вывод), сразу при
// подключении — --net-burst байтов (станции шлют 4–10 с звука); --net-stall-at N
// --net-stall-ms МС — после N байтов звука столько мс звука ничего (потом станция
// догоняет: её очередь копилась); --net-jitter МС — звук приходит пачками раз в
// столько мс звука (у платы — Wi-Fi и окно TCP 5760 байтов). Чтение ждёт байтов до
// своего timeoutMs.
std::vector<uint8_t> netFile;
uint32_t netMetaint = 16000;
std::string netTitle = "Test Artist - Test Song";
std::string netTitle2;
std::string netType = "audio/mpeg";
std::string netName = "Test Radio";
int netFail = 0;
size_t netCut = 0;
uint32_t netKbps = 0;                  // 0 — без ограничения
size_t netBurst = 0;
size_t netStallAt = 0;
uint32_t netStallMs = 0;
uint32_t netJitterMs = 0;
// Обложка из сети (online_cover.c): адреса api.deezer.com и *.dzcdn.net — подмена поиска
// (--cover-json ФАЙЛ) и картинки (--cover-jpeg ФАЙЛ); без них — 404. В счёт соединений радио
// (net_opens) не идут.
std::string coverJson;
std::vector<uint8_t> coverJpeg;
int coverSearches = 0;
int coverPictures = 0;
std::string coverSearchUrl;
std::string coverPictureUrl;
std::mutex netLock;
int netOpens = 0;
int netCloses = 0;
std::vector<std::string> netUrls;

struct FakeStream {
  std::string headers;                 // «имя:значение\n»
  std::string value;
  size_t position = 0;                 // в netFile
  size_t audio = 0;                    // байтов звука отдано
  uint32_t metaint = 0;
  uint32_t untilMeta = 0;
  int metaBlocks = 0;
  std::vector<uint8_t> pending;        // метаданные к отдаче
  Clock::time_point opened = Clock::now();
  Clock::time_point stalled{};         // когда началась задержка (--net-stall-at)
  bool isBody = false;                 // ответ сервиса обложек: тело целиком, потом конец
  std::vector<uint8_t> body;
};

// Сколько байтов звука станция успела отдать к этому мгновению (--net-kbps).
size_t netAllowed(FakeStream* s) {
  if (netKbps == 0) {
    return SIZE_MAX;
  }
  const Clock::time_point now = Clock::now();
  if (netStallMs != 0 && s->audio >= netStallAt) {
    if (s->stalled == Clock::time_point{}) {
      s->stalled = now;
    }
    const double ms = std::chrono::duration<double, std::milli>(now - s->stalled).count() * sink.speed;
    if (ms < netStallMs) {
      return s->audio;
    }
  }
  double ms = std::chrono::duration<double, std::milli>(now - s->opened).count() * sink.speed;
  if (netJitterMs != 0) {
    ms = floor(ms / netJitterMs) * netJitterMs;   // пачка — в начале каждого отрезка
  }
  return netBurst + static_cast<size_t>(netKbps * 0.125 * ms);
}

void* coreNetOpen(const char* url, const char* extra, uint32_t timeoutMs, int* status) {
  (void)timeoutMs;
  std::lock_guard<std::mutex> lock(netLock);
  const std::string address = url != nullptr ? url : "";
  const bool search = address.rfind("https://api.deezer.com/", 0) == 0;
  if (search || address.find(".dzcdn.net/") != std::string::npos) {
    std::vector<uint8_t> data;
    if (search) {
      ++coverSearches;
      coverSearchUrl = address;
      data.assign(coverJson.begin(), coverJson.end());
    } else {
      ++coverPictures;
      coverPictureUrl = address;
      data = coverJpeg;
    }
    if (data.empty()) {
      if (status != nullptr) *status = 404;
      return nullptr;
    }
    FakeStream* s = new FakeStream;
    s->isBody = true;
    s->body = data;
    s->headers = "content-length:" + std::to_string(data.size()) + "\n";
    if (status != nullptr) *status = 200;
    return s;
  }
  ++netOpens;
  netUrls.push_back(url != nullptr ? url : "");
  if (status != nullptr) {
    *status = 0;
  }
  if (netOpens <= netFail) {
    return nullptr;                    // «нет соединения»
  }
  if (netFile.empty()) {
    if (status != nullptr) {
      *status = 404;
    }
    return nullptr;
  }
  FakeStream* s = new FakeStream;
  s->metaint = extra != nullptr && strstr(extra, "Icy-MetaData: 1") != nullptr ? netMetaint : 0;
  s->untilMeta = s->metaint;
  s->headers = "content-type:" + netType + "\nicy-name:" + netName +
               "\nicy-description:Only test songs!\nicy-genre:Test\nicy-br:128\n";
  if (s->metaint != 0) {
    s->headers += "icy-metaint:" + std::to_string(s->metaint) + "\n";
  }
  if (status != nullptr) {
    *status = 200;
  }
  return s;
}

const char* coreNetHeader(void* stream, const char* name) {
  FakeStream* s = static_cast<FakeStream*>(stream);
  const size_t length = strlen(name);
  for (size_t at = 0; at < s->headers.size();) {
    const size_t end = s->headers.find('\n', at);
    const std::string line = s->headers.substr(at, end - at);
    if (line.size() > length && line[length] == ':' && _strnicmp(line.c_str(), name, length) == 0) {
      s->value = line.substr(length + 1);
      return s->value.c_str();
    }
    at = end + 1;
  }
  return nullptr;
}

int netReadNow(FakeStream* s, void* data, uint32_t count) {
  uint8_t* out = static_cast<uint8_t*>(data);
  if (s->isBody) {
    const size_t left = s->body.size() - s->position;
    if (left == 0) return -1;
    const size_t part = count < left ? count : left;
    memcpy(out, s->body.data() + s->position, part);
    s->position += part;
    return static_cast<int>(part);
  }
  const size_t allowed = netAllowed(s);
  uint32_t done = 0;
  while (done < count && done < 4096) {
    if (!s->pending.empty()) {
      out[done++] = s->pending.front();
      s->pending.erase(s->pending.begin());
      continue;
    }
    if (s->metaint != 0 && s->untilMeta == 0) {
      ++s->metaBlocks;
      std::string text;
      if (s->metaBlocks == 1) {
        text = "StreamTitle='" + netTitle + "';";
      } else if (s->metaBlocks == 3 && !netTitle2.empty()) {
        text = "StreamTitle='" + netTitle2 + "';";
      }
      const size_t blocks = (text.size() + 15) / 16;
      s->pending.push_back(static_cast<uint8_t>(blocks));
      for (size_t i = 0; i < blocks * 16; ++i) {
        s->pending.push_back(i < text.size() ? static_cast<uint8_t>(text[i]) : 0);
      }
      s->untilMeta = s->metaint;
      continue;
    }
    if (netCut != 0 && s->audio >= netCut) {
      break;                           // обрыв: дальше — конец потока
    }
    uint32_t part = count - done;
    if (s->metaint != 0 && part > s->untilMeta) {
      part = s->untilMeta;
    }
    if (netCut != 0 && part > netCut - s->audio) {
      part = static_cast<uint32_t>(netCut - s->audio);
    }
    if (allowed <= s->audio) {
      break;                           // станция ещё не прислала
    }
    if (part > allowed - s->audio) {
      part = static_cast<uint32_t>(allowed - s->audio);
    }
    if (part > netFile.size() - s->position) {
      part = static_cast<uint32_t>(netFile.size() - s->position);
    }
    memcpy(out + done, netFile.data() + s->position, part);
    s->position = (s->position + part) % netFile.size();
    s->audio += part;
    if (s->metaint != 0) {
      s->untilMeta -= part;
    }
    done += part;
  }
  if (done == 0) {
    return netCut != 0 && s->audio >= netCut ? -1 : 0;
  }
  return static_cast<int>(done);
}

int coreNetRead(void* stream, void* data, uint32_t count, uint32_t timeoutMs) {
  FakeStream* s = static_cast<FakeStream*>(stream);
  const Clock::time_point until = Clock::now() + std::chrono::milliseconds(timeoutMs);
  for (;;) {
    const int got = netReadNow(s, data, count);
    if (got != 0 || netKbps == 0 || Clock::now() >= until) {
      return got;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

void coreNetClose(void* stream) {
  std::lock_guard<std::mutex> lock(netLock);
  ++netCloses;
  delete static_cast<FakeStream*>(stream);
}

// --- модули ---------------------------------------------------------------------------

int coreModuleOpen(const char* name, void* arg1, void* arg2, void** handle, int* entry) {
  int (*main)(int, char**) = nullptr;
  if (strcmp(name, "MP3.ELF") == 0 && !noMp3) {
    main = mp3_main;
  } else if (strcmp(name, "AAC.ELF") == 0 && !noAac) {
    main = aac_main;
  } else if (strcmp(name, "OGG.ELF") == 0 && !noOgg) {
    main = ogg_main;
  } else if (strcmp(name, "FLAC.ELF") == 0 && !noFlac) {
    main = flac_main;
  } else if (strcmp(name, "TRACKER.ELF") == 0) {
    main = tracker_main;
  } else if (strcmp(name, "MIDI.ELF") == 0) {
    main = midi_main;
  } else if (strcmp(name, "OPL.ELF") == 0) {
    main = opl_main;
  } else if (strcmp(name, "SAA.ELF") == 0) {
    main = saa_main;
  } else if (strcmp(name, "TSFM.ELF") == 0) {
    main = tsfm_main;
  }
  if (main == nullptr) {
    return ZM_NOT_FOUND;
  }
  char* argv[3] = {const_cast<char*>(name), static_cast<char*>(arg1), static_cast<char*>(arg2)};
  *entry = main(3, argv);
  *handle = new int(1);
  ++modulesOpen;
  ++moduleLoads;
  return ZM_OK;
}

void coreModuleClose(void* handle) {
  delete static_cast<int*>(handle);
  --modulesOpen;
}

std::vector<uint8_t> bank;             // ресурс MIDI.SF2 (--bank)
std::vector<uint8_t> rom;              // ресурс YRW801.ROM (--rom)
int bankReleases = 0;
int romReleases = 0;
std::map<std::string, std::vector<uint8_t>> resources;   // --resource ИМЯ=файл
int resourceReleases = 0;

int coreResource(const char* name, const uint8_t** data, size_t* size) {
  const std::vector<uint8_t>* item = nullptr;
  if (name != nullptr && strcmp(name, "MIDI.SF2") == 0) {
    item = &bank;
  } else if (name != nullptr && strcmp(name, "YRW801.ROM") == 0) {
    item = &rom;
  } else if (name != nullptr && resources.count(name) != 0) {
    item = &resources[name];
  }
  if (item == nullptr || item->empty()) {
    return ZM_NOT_FOUND;
  }
  *data = item->data();
  *size = item->size();
  return ZM_OK;
}

size_t residentResources() {
  size_t bytes = bank.size() + rom.size();
  for (const auto& item : resources) {
    bytes += item.second.size();
  }
  return bytes;
}

void coreResourceRelease(const char* name) {
  std::lock_guard<std::mutex> lock(allocLock);
  if (name != nullptr && strcmp(name, "MIDI.SF2") == 0 && !bank.empty()) {
    bank.clear();
    ++bankReleases;
  }
  if (name != nullptr && strcmp(name, "YRW801.ROM") == 0 && !rom.empty()) {
    rom.clear();
    ++romReleases;
  }
  if (name != nullptr && resources.count(name) != 0) {
    resources.erase(name);
    ++resourceReleases;
  }
}

zm_core_t core = {};
const zm_app_t* app = nullptr;

}  // namespace

// Память libxmp (xmp_esp_shim.c, XMP_ESP_HOST_BUDGET) — в счёт PSRAM стенда.
extern "C" void* xmp_host_alloc(size_t size) {
  std::lock_guard<std::mutex> lock(allocLock);
  if (!psramTakeLocked(size)) {
    return nullptr;
  }
  void* pointer = malloc(size);
  if (pointer == nullptr) {
    psramUsed -= size;
    return nullptr;
  }
  psramBlocks[pointer] = size;
  xmpBlocks[pointer] = size;
  xmpUsed += size;
  if (xmpUsed > xmpPeak) {
    xmpPeak = xmpUsed;
  }
  return pointer;
}

extern "C" void xmp_host_free(void* pointer) {
  if (pointer == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(allocLock);
    auto block = psramBlocks.find(pointer);
    if (block != psramBlocks.end()) {
      psramUsed -= block->second;
      psramBlocks.erase(block);
    }
    auto own = xmpBlocks.find(pointer);
    if (own != xmpBlocks.end()) {
      xmpUsed -= own->second;
      xmpBlocks.erase(own);
    }
  }
  free(pointer);
}

namespace {

// --- плагин Z80 ----------------------------------------------------------------------

[[noreturn]] void fail(const char* format, ...) {
  va_list args;
  va_start(args, format);
  fprintf(stderr, "fail: ");
  vfprintf(stderr, format, args);
  fprintf(stderr, "\n");
  va_end(args);
  exit(2);
}

std::vector<uint8_t> command(uint8_t code, const std::vector<uint8_t>& data = {}) {
  if (appUnloaded) {
    return {0xFF, 0xFF};                // приложения нет: команду ядро не знает
  }
  inCommand = true;
  replyCode = -1;
  replyData.clear();
  const int result = app->command(code, data.empty() ? nullptr : data.data(),
                                  static_cast<uint16_t>(data.size()));
  inCommand = false;
  if (appDoneAsked) {                   // ответ ушёл — выгрузить (ModuleApp::handleCommand)
    appDoneAsked = false;
    ++appDones;
    appUnloaded = true;
    app->stop();
  }
  if (result != ZM_OK) {
    return {0xFF, 0xFF};                // команда не приложения: её выполнит ядро
  }
  if (replyCode != (code | 0x80)) {
    fail("command %02X: reply %02X", code, replyCode);
  }
  return replyData;
}

uint32_t le32(const uint8_t* p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

struct Status {
  uint8_t state, flags, channels, volume, format, row, voices, temperature;
  uint32_t free, queue;
  uint32_t rate;                      // с флагом ST_F_RATE_HIGH (бит 7 флагов) — +65536
  uint16_t bitrate, seconds, faults, errors, underruns, order, length;
  uint8_t screen;                     // флаги экрана Tang (1.26, SCREEN_F_*)
};

Status status() {
  const std::vector<uint8_t> r = command(kStatus);
  if (r.size() != 33) {
    fail("status of %u bytes", static_cast<unsigned>(r.size()));
  }
  Status s;
  s.state = r[0];
  s.flags = r[1];
  s.free = le32(&r[2]);
  s.queue = le32(&r[6]);
  s.rate = le16(&r[10]) + ((r[1] & 0x80) != 0 ? 0x10000u : 0u);
  s.channels = r[12];
  s.bitrate = le16(&r[13]);
  s.seconds = le16(&r[15]);
  s.faults = le16(&r[17]);
  s.errors = le16(&r[19]);
  s.underruns = le16(&r[21]);
  s.temperature = r[31];
  s.volume = r[23];
  s.format = r[24];
  s.row = r[25];
  s.order = le16(&r[26]);
  s.length = le16(&r[28]);
  s.voices = r[30];
  s.screen = r[32];
  return s;
}

void print(const char* key, long long value) { printf("%s=%lld\n", key, value); }

void printStatus(const char* prefix, const Status& s) {
  printf("%sstate=%u\n%sfinished=%u\n%sdecoded=%u\n%sbuffering=%u\n%spaused=%u\n", prefix,
         s.state, prefix, s.flags & 1, prefix, (s.flags >> 1) & 1, prefix, (s.flags >> 2) & 1,
         prefix, (s.flags >> 3) & 1);
  printf("%srate=%u\n%schannels=%u\n%sbitrate=%u\n%sseconds=%u\n%serrors=%u\n%sunderruns=%u\n",
         prefix, s.rate, prefix, s.channels, prefix, s.bitrate, prefix, s.seconds, prefix,
         s.errors, prefix, s.underruns);
  printf("%sformat=%u\n%sorder=%u\n%slength=%u\n%svoices=%u\n%sfree=%u\n%squeue=%u\n", prefix,
         s.format, prefix, s.order, prefix, s.length, prefix, s.voices, prefix, s.free, prefix,
         s.queue);
  printf("%swants_rom=%u\n", prefix, (s.flags >> 4) & 1);
  printf("%stemperature=%u\n", prefix, s.temperature);
}

void printSink() {
  std::lock_guard<std::mutex> lock(sink.lock);
  print("pcm_frames", static_cast<long long>(sink.frames));
  print("first_write", static_cast<long long>(sink.firstWrite));
  print("pcm_played", static_cast<long long>(sink.played + 0.5));
  print("pcm_discarded", static_cast<long long>(sink.discarded + 0.5));
  print("pcm_peak_permille", static_cast<long long>(sink.peak * 1000));
  print("pcm_opens", sink.opens);
  print("pcm_flushes", sink.flushes);
  print("pcm_pauses", sink.pauses);
  print("pcm_timeouts", static_cast<long long>(sink.timeouts));
  print("pcm_rate", sink.rate);
  print("pcm_gaps", sink.gaps);
  print("pcm_gap_ms", static_cast<long long>(sink.gapFrames * 1000.0 / 44100.0));   // потоки стенда — 44,1 кГц
}

uint64_t sinkFrames() {
  std::lock_guard<std::mutex> lock(sink.lock);
  return sink.frames;
}

// Подать байты [from, to) кадрами AUDIO_DATA; очередь полна — подождать, как
// плагин, опрашивая состояние. Модуль (whole): ответ 2 — ESP файл уже отвергла
// (IT потоком), дальше не слать, как плагин 1.1: refused_at — сколько отдано.
bool wholeFeed = false;

void feed(const std::vector<uint8_t>& file, size_t from, size_t to, size_t block) {
  size_t at = from;
  int busy = 0;
  while (at < to) {
    const size_t part = to - at < block ? to - at : block;
    const std::vector<uint8_t> data(file.begin() + at, file.begin() + at + part);
    const std::vector<uint8_t> r = command(kData, data);
    if (r.empty()) {
      fail("data: empty reply");
    }
    if (r[0] == 1) {
      at += part;
      busy = 0;
    } else if (r[0] == 0) {
      status();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      if (++busy > 20000) {
        fail("data: queue stays full at %u", static_cast<unsigned>(at));
      }
    } else if (r[0] == 2 && wholeFeed) {
      print("refused_at", static_cast<long long>(at));
      return;
    } else {
      fail("data: refused (%u) at %u", r[0], static_cast<unsigned>(at));
    }
  }
}

// Ждать конца звука (состояние IDLE с флагом «доиграл»).
Status waitFinished(int timeout_ms) {
  const Clock::time_point until = Clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    const Status s = status();
    if (s.state == 0 && (s.flags & 1)) {
      return s;
    }
    if (Clock::now() > until) {
      printStatus("timeout_", s);
      fail("not finished in %d ms", timeout_ms);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void waitSound(uint64_t frames = 1) {
  for (int wait = 0; sinkFrames() < frames; ++wait) {
    if (wait > 5000) {
      fail("no sound");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

std::vector<uint8_t> readFile(const char* path) {
  FILE* file = fopen(path, "rb");
  if (file == nullptr) {
    fail("cannot open %s", path);
  }
  std::vector<uint8_t> data;
  uint8_t buffer[65536];
  size_t count;
  while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0) {
    data.insert(data.end(), buffer, buffer + count);
  }
  fclose(file);
  return data;
}

void stopApp() {
  if (!appUnloaded) app->stop();
  print("app_dones", appDones);
  for (int wait = 0; wait < 200 && tasks != 0; ++wait) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  print("tasks_after_stop", tasks);
  print("modules_after_stop", modulesOpen);
  print("module_loads", moduleLoads);
  std::lock_guard<std::mutex> lock(allocLock);
  print("allocations_after_stop", static_cast<long long>(allocations.size()));
  int largeBuffers = 0;
  for (const auto& block : psramBlocks) if (block.second == 256u * 1024u || block.second == 512u * 1024u) ++largeBuffers;
  print("large_buffers_after_stop", largeBuffers);
}

}  // namespace

int main(int argc, char** argv) {
  timeBeginPeriod(1);
  if (argc < 2) {
    fail("usage: player_host <scenario> [--key value] [file]");
  }
  const std::string scenario = argv[1];
  const char* path = nullptr;
  size_t block = 1024;
  size_t cut = 0;
  size_t resume = 0;
  uint32_t base = 0;
  int seek = 0;
  int hold = 300;
  int pauses = 1;
  double hz = 1000;
  double seconds = 600;
  int midi = 0;
  int late = 0;
  int releaseAfter = 0;                 // --release 1: после звука AUDIO_OPEN [#20]
  size_t first = 0;                     // --first N: первый блок потока — N байт, потом пауза
  int sized = 0;                        // --sized 1: AUDIO_OPEN MP3 с длиной потока
  size_t stopAt = 0;                    // --stop-at N: стоп посреди приёма модуля
  int pauseAt = 0;                      // --pause-at МС: пауза посреди модуля
  int noNet = 0;                        // --no-net 1: ядро без net_* (старая прошивка)
  int uiOn = 0;                         // --ui 1: ядро с экраном оверлеем Tang (ui_available)
  std::vector<uint8_t> uiFirst;         // --ui-block ФАЙЛ: первое тело AUDIO_STATUS (от плагина Z80)
  int waitMs = 15000;                   // --wait-ms: радио — столько ждать первого звука
  int listWait = 0;                     // --list-wait 1: радио — дождаться конца списка станций
  int coverWait = 0;                    // --cover-wait 1: обложка от декодера (флаг 64)
  std::string ext;
  int viewSync = 0;                    // --view-sync 1: сравнить спектр с проигранным PCM
  for (int index = 2; index < argc; ++index) {
    const std::string key = argv[index];
    if (key.rfind("--", 0) == 0 && index + 1 < argc) {
      const char* value = argv[++index];
      if (key == "--block") block = strtoul(value, nullptr, 10);
      else if (key == "--cut") cut = strtoul(value, nullptr, 10);
      else if (key == "--resume") resume = strtoul(value, nullptr, 10);
      else if (key == "--base") base = strtoul(value, nullptr, 10);
      else if (key == "--seek") seek = atoi(value);
      else if (key == "--hold") hold = atoi(value);
      else if (key == "--pauses") pauses = atoi(value);
      else if (key == "--hz") hz = atof(value);
      else if (key == "--seconds") seconds = atof(value);
      else if (key == "--speed") sink.speed = atof(value);
      else if (key == "--raw") sink.raw = fopen(value, "wb");
      else if (key == "--midi") midi = atoi(value);
      else if (key == "--bank") bank = readFile(value);
      else if (key == "--rom") rom = readFile(value);
      else if (key == "--resource") {
        const char* eq = strchr(value, '=');
        if (eq == nullptr) fail("--resource NAME=file");
        resources[std::string(value, eq - value)] = readFile(eq + 1);
      }
      else if (key == "--ext") ext = value;
      else if (key == "--late") late = atoi(value);
      else if (key == "--release") releaseAfter = atoi(value);
      else if (key == "--first") first = strtoul(value, nullptr, 10);
      else if (key == "--sized") sized = atoi(value);
      else if (key == "--psram") psramBudget = strtoul(value, nullptr, 10);
      else if (key == "--internal") internalBudget = strtoul(value, nullptr, 10);
      else if (key == "--internal-block") internalBlock = strtoul(value, nullptr, 10);
      else if (key == "--stop-at") stopAt = strtoul(value, nullptr, 10);
      else if (key == "--pause-at") pauseAt = atoi(value);
      else if (key == "--net-file") netFile = readFile(value);
      else if (key == "--net-metaint") netMetaint = strtoul(value, nullptr, 10);
      else if (key == "--net-title") netTitle = value;
      else if (key == "--net-title2") netTitle2 = value;
      else if (key == "--net-type") netType = value;
      else if (key == "--net-name") netName = value;
      else if (key == "--net-fail") netFail = atoi(value);
      else if (key == "--net-cut") netCut = strtoul(value, nullptr, 10);
      else if (key == "--net-kbps") netKbps = strtoul(value, nullptr, 10);
      else if (key == "--net-burst") netBurst = strtoul(value, nullptr, 10);
      else if (key == "--net-stall-at") netStallAt = strtoul(value, nullptr, 10);
      else if (key == "--net-stall-ms") netStallMs = strtoul(value, nullptr, 10);
      else if (key == "--net-jitter") netJitterMs = strtoul(value, nullptr, 10);
      else if (key == "--pcm-ms") sink.capacityMs = strtoul(value, nullptr, 10);
      else if (key == "--view-sync") viewSync = atoi(value);
      else if (key == "--no-net") noNet = atoi(value);
      else if (key == "--ui") uiOn = atoi(value);
      else if (key == "--ui-block") uiFirst = readFile(value);
      else if (key == "--cover-json") {
        const std::vector<uint8_t> json = readFile(value);
        coverJson.assign(json.begin(), json.end());
      }
      else if (key == "--cover-jpeg") coverJpeg = readFile(value);
      else if (key == "--wait-ms") waitMs = atoi(value);
      else if (key == "--list-wait") listWait = atoi(value);
      else if (key == "--cover-wait") coverWait = atoi(value);
      else if (key == "--no-aac") noAac = atoi(value) != 0;
      else if (key == "--no-flac") noFlac = atoi(value) != 0;
      else if (key == "--no-ogg") noOgg = atoi(value) != 0;
      else if (key == "--no-worker") noWorker = atoi(value) != 0;
      else fail("unknown key %s", key.c_str());
    } else {
      path = argv[index];
    }
  }
  noMp3 = scenario == "missing";
  if (scenario == "missing") {
    noAac = noOgg = noFlac = true;               // в кэше нет ни одного декодера потока
  }

  core.size = sizeof(core);
  core.abi_major = ZM_APP_ABI_MAJOR;
  core.abi_minor = ZM_APP_ABI_MINOR;
  core.firmware = "vdac4-host";
  core.alloc = coreAlloc;
  core.realloc = coreRealloc;
  core.free = coreFree;
  core.log = coreLog;
  core.millis = coreMillis;
  core.delay = coreDelay;
  core.task_start = coreTaskStart;
  core.mutex_create = coreMutexCreate;
  core.mutex_lock = coreMutexLock;
  core.mutex_unlock = coreMutexUnlock;
  core.signal_create = coreSignalCreate;
  core.signal_give = coreSignalGive;
  core.signal_wait = coreSignalWait;
  core.reply = coreReply;
  core.event = coreEvent;
  core.pcm_open = corePcmOpen;
  core.pcm_write = corePcmWrite;
  core.pcm_queued_ms = corePcmQueuedMs;
  core.pcm_flush = corePcmFlush;
  core.pcm_close = corePcmClose;
  core.pcm_pause = corePcmPause;
  core.set_volume = coreSetVolume;
  core.volume = coreVolume;
  core.pcm_faults = corePcmFaults;
  core.module_open = coreModuleOpen;
  core.module_close = coreModuleClose;
  core.resource = coreResource;
  core.resource_release = coreResourceRelease;
  core.temperature = coreTemperature;
  if (!noNet) {
    core.net_open = coreNetOpen;
    core.net_header = coreNetHeader;
    core.net_read = coreNetRead;
    core.net_close = coreNetClose;
  }
  if (uiOn) {
    core.link_read = coreLinkRead;
    core.link_write = coreLinkWrite;
    core.ui_available = coreUiAvailable;
  }
  core.app_done = coreAppDone;

  const zm_app_t* table = nullptr;
  char* args[3] = {const_cast<char*>("PLAYER.ELF"), reinterpret_cast<char*>(&core),
                   reinterpret_cast<char*>(&table)};
  print("entry", player_main(3, args));
  if (table == nullptr) {
    fail("no app table");
  }
  app = table;
  printf("app=%s\nversion=%s\n", app->name, app->version);
  print("command_first", app->command_first);
  print("command_last", app->command_last);
  print("start", app->start(&core));
  const std::vector<uint8_t> tone = command(kTone, {1});
  print("tone_own", tone.size() == 2 && tone[0] == 0xFF && tone[1] == 0xFF ? 0 : 1);

  std::vector<uint8_t> file;
  if (path != nullptr) {
    file = readFile(path);
  }

  if (scenario == "spectrum") {
    // Приложение уже завело таблицы (view_begin в start): синус в два канала.
    std::vector<float> frames(2 * 1024);
    for (int part = 0; part < 4; ++part) {
      for (int i = 0; i < 1024; ++i) {
        const float value = 0.8f * sinf(6.2831853f * (float)hz * (part * 1024 + i) / 44100.0f);
        frames[2 * i] = frames[2 * i + 1] = value;
      }
      view_feed(frames.data(), 1024, 2, 44100);
    }
    uint8_t bands[VIEW_BANDS];
    view_bands(bands, 0);
    printf("bands=");
    for (int b = 0; b < VIEW_BANDS; ++b) {
      printf("%s%u", b ? "," : "", bands[b]);
    }
    printf("\n");
  } else if (scenario == "spectrum_timeline") {
    // Время вывода управляемое: звук уже принят, очередь ещё не доиграна.
    static uint32_t queuedMs = 0;
    core.pcm_queued_ms = []() -> uint32_t { return queuedMs; };
    auto peak = [](int paused = 0) {
      uint8_t values[VIEW_BANDS];
      view_bands(values, paused);
      int top = 0;
      for (uint8_t value : values) if (value > top) top = value;
      return top;
    };
    auto feedTone = [](uint32_t rate, int ms, bool tone) {
      std::vector<float> frames(rate / 100 * 2);
      for (int at = 0; at < ms; at += 10) {
        for (size_t i = 0; i < frames.size() / 2; ++i) {
          const float value = tone ? 0.8f * sinf(6.2831853f * 1000.0f * (float)i / rate) : 0;
          frames[2 * i] = frames[2 * i + 1] = value;
        }
        view_feed(frames.data(), (uint32_t)frames.size() / 2, 2, rate);
      }
    };
    feedTone(44100, 1000, false);
    feedTone(44100, 500, true);
    feedTone(44100, 500, false);
    queuedMs = 1800;
    print("before_note", peak());
    queuedMs = 800;
    print("during_note", peak());
    queuedMs = 0;                       // DMA мог доиграть на паузе: картинка стоит
    print("paused_note", peak(1));
    queuedMs = 300;
    print("after_note", peak());
    queuedMs = 0;                       // декодер уже кончил: очередь доигрывается
    print("drained", peak());
    feedTone(44100, 100, true);
    print("before_reset", peak());
    view_reset();                       // seek/flush/stop не сохраняют старые полосы
    print("after_reset", peak());
    feedTone(44100, 100, true);
    queuedMs = 100;
    print("new_stream_queued", peak());
    queuedMs = 0;
    print("new_stream_played", peak());
    feedTone(48000, 100, false);          // новый формат обнуляет и шкалу времени
    print("rate_changed", peak());
    view_reset();
    feedTone(8000, 13000, false);         // обход кольца снимков + максимум очереди
    feedTone(8000, 1000, true);
    queuedMs = 8200;
    print("wrapped_future", peak());
    queuedMs = 0;
    print("wrapped_played", peak());
  } else if (scenario == "missing") {
    std::vector<uint8_t> body = {0};
    if (!ext.empty()) {
      body.insert(body.end(), {0, 0, 0, 0, 3});
      for (size_t i = 0; i < 3; ++i) {
        body.push_back(i < ext.size() ? static_cast<uint8_t>(ext[i]) : ' ');
      }
    }
    const std::vector<uint8_t> r = command(kOpen, body);
    print("open", r[0]);
    printStatus("", status());
  } else if (scenario == "picture_cancel" || scenario == "picture_replace") {
    print("open", command(kOpen, {0})[0]);
    print("image", cover_image(file.data(), file.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (scenario == "picture_cancel") {
      const auto before = Clock::now();
      stopApp();
      print("stop_ms", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-before).count());
      return 0;
    }
    cover_reset();
    print("replacement", cover_image(file.data(), file.size()));
    uint8_t state = 0;
    for (int wait = 0; wait < 400 && state == 0; ++wait) {
      state = command(kThumb, {0})[0];
      if (!state) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    print("thumb_state", state);
  } else if (scenario == "ui") {
    // Экран оверлеем Tang (ABI 1.5): тело AUDIO_STATUS от плагина 1.19 → снимок ui_model;
    // поток с меткой ID3 (досылается AUDIO_COVER с начала, как у плагина) и звуком после неё;
    // файлу без своей обложки — обложка из сети (подмена Deezer), с флагом 64 окну Evo.
    auto uiBlock = [](uint8_t flags, uint8_t page, const char* name, const char* state) {
      std::vector<uint8_t> b = {1, flags, page, 3, 0, 10, 0, 0x40, 0xE2, 0x01, 0x00};   // 123456
      b.insert(b.end(), name, name + strlen(name) + 1);
      b.insert(b.end(), state, state + strlen(state) + 1);
      return b;
    };
    static zm_ui_model_t m;
    auto model = [&]() {
      memset(&m, 0, sizeof(m));
      m.size = sizeof(m);
      return player_ui_model(&m);
    };
    // Экран рисует сам PLAYER: экрану прошивки снимка нет (ui_model, ui_cover — NULL)
    print("ui_table", app->size >= offsetof(zm_app_t, ui_cover) + sizeof(app->ui_cover) &&
                          (app->ui_model != nullptr || app->ui_cover != nullptr) ? 1 : 0);
    print("ui_before_visible", model() == ZM_OK ? m.visible : -1);
    command(kStatus, uiFirst.empty() ? uiBlock(1, 2, "CHILDREN.MP3", "Buffering") : uiFirst);
    model();
    print("ui_visible", m.visible);
    print("ui_page", m.page);
    print("ui_track", m.track);
    print("ui_tracks", m.tracks);
    print("ui_sent_kb", m.sent_kb);
    printf("ui_file=%s\nui_status=%s\n", m.file, m.status);
    // Метка ID3 в начале файла: её длина — из заголовка (синхробезопасные 7 бит)
    size_t tag = 0;
    if (file.size() >= 10 && memcmp(file.data(), "ID3", 3) == 0) {
      tag = 10 + ((file[6] & 0x7F) << 21 | (file[7] & 0x7F) << 14 | (file[8] & 0x7F) << 7 |
                  (file[9] & 0x7F));
    }
    const uint32_t streamSize = static_cast<uint32_t>(file.size() - tag);
    const std::vector<uint8_t> opened = command(kOpen, {0, static_cast<uint8_t>(streamSize),
        static_cast<uint8_t>(streamSize >> 8), static_cast<uint8_t>(streamSize >> 16),
        static_cast<uint8_t>(streamSize >> 24), 4, 'M', 'P', '3'});
    print("open", opened[0]);
    print("open_screen", opened.size() >= 6 ? opened[5] : -1);
    if (tag != 0) {
      uint32_t want = 0;
      uint8_t state = 0;
      for (int feeds = 0; state == 0 && feeds < 2000; ++feeds) {
        const size_t length = want < tag ? std::min<size_t>(1020, tag - want) : 0;
        std::vector<uint8_t> data = {static_cast<uint8_t>(want), static_cast<uint8_t>(want >> 8),
                                     static_cast<uint8_t>(want >> 16),
                                     static_cast<uint8_t>(want >> 24)};
        data.insert(data.end(), file.begin() + want, file.begin() + want + length);
        const std::vector<uint8_t> r = command(kCover, data);
        state = r[0];
        want = le32(&r[1]);
      }
      print("tag_cover_state", state);
    }
    feed(file, tag, file.size(), block);
    command(kClose, {1});
    // Звук идёт: ждать 11 с позиции (обложку из сети PLAYER ищет через 10 с звука)
    for (int wait = 0; wait < 3000; ++wait) {
      const Status s = status();
      if (s.seconds >= 11 || (s.flags & 1)) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    command(kStatus, uiBlock(1, 0, "CHILDREN.MP3", ""));
    model();
    print("ui_view", m.view);
    print("ui_count", m.count);
    print("ui_state", m.state);
    printf("ui_title=%s\nui_artist=%s\nui_format=%s\n", m.title, m.artist, m.format);
    print("ui_volume", m.volume);
    print("ui_temperature", m.temperature);
    print("ui_played_ms", m.played_ms);
    int flag = 0;
    for (int wait = 0; wait < 600 && !flag; ++wait) {
      flag = (status().flags >> 6) & 1;
      if (!flag) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    print("cover_flag", flag);
    {
      std::lock_guard<std::mutex> lock(netLock);
      print("cover_searches", coverSearches);
      print("cover_pictures", coverPictures);
      printf("cover_search_url=%s\ncover_picture_url=%s\n", coverSearchUrl.c_str(),
             coverPictureUrl.c_str());
    }
    int serial = 0;
    for (int wait = 0; wait < 400 && serial == 0; ++wait) {
      model();
      serial = m.cover_serial;
      if (!serial) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    print("ui_cover_serial", serial);
    static uint16_t pixels[160 * 160];
    const int coverResult = cover_ui_copy(pixels, 160) ? ZM_OK : ZM_NOT_FOUND;
    print("ui_cover", coverResult);
    print("ui_cover_pixel", coverResult == ZM_OK ? pixels[0] : -1);
    print("ui_cover_corner", coverResult == ZM_OK ? pixels[160 * 160 - 1] : -1);
    // Экран PLAYER (screen.cpp: задача обложек сама ведёт оверлей 1) → LINK → Tang (модель).
    // Звук ещё идёт, плагин спрашивает состояние раз в 200 мс.
    {
      const std::vector<uint8_t> body = uiFirst.empty() ? uiBlock(1, 0, "CHILDREN.MP3", "") : uiFirst;
      int flags = 0;
      for (int t = 0; t < 15; ++t) {
        const std::vector<uint8_t> r = command(kStatus, body);
        if (r.size() > 32) flags |= r[32];
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
      {
        std::lock_guard<std::mutex> lock(linkLock);
        print("chain_ctrl", ovlRegs[0]);
        print("chain_pos", ovlRegs[1]);
        print("chain_size", ovlRegs[2]);
        print("chain_base", ovlRegs[3]);
        std::string alphas;
        for (size_t i = 0; i < ovlAlphas.size() && i < 24; ++i) {
          alphas += (i ? "," : "") + std::to_string(ovlAlphas[i]);
        }
        printf("chain_alphas=%s\n", alphas.c_str());
        print("chain_written_before_on", writtenBeforeOn);
        long long inside = 0, outside = 0;
        for (size_t i = 0; i < tangWritten.size(); ++i) {
          if (tangWritten[i]) (i >= ovlRegs[3] ? inside : outside) += 1;
        }
        print("chain_written", inside);
        print("chain_outside", outside);
        print("chain_frame_words", wordsAtOn < 0 ? -1 : linkWords - wordsAtOn);
        print("chain_bad", linkBad);
      }
      print("chain_flags", flags);
      std::vector<uint8_t> leave = body;
      leave[1] |= 2;                                // флаг ухода, как на выходе плагина
      size_t before = 0;
      {
        std::lock_guard<std::mutex> lock(linkLock);
        before = ovlAlphas.size();
      }
      command(kStatus, leave);              // ответ, затем выгрузка: stop гасит экран плавно
      {
        std::lock_guard<std::mutex> lock(linkLock);
        print("chain_left_on", ovlRegs[0] & 1);
        std::string alphas;
        for (size_t i = before; i < ovlAlphas.size(); ++i) {
          alphas += (i != before ? "," : "") + std::to_string(ovlAlphas[i]);
        }
        printf("chain_left_alphas=%s\n", alphas.c_str());
      }
      print("chain_app_dones", appDones);
    }
    command(kStatus, uiBlock(3, 0, "CHILDREN.MP3", ""));
    model();
    print("ui_left_visible", m.visible);
  } else if (scenario == "cover") {
    print("open", command(kOpen, {0})[0]);
    uint32_t want = 0;
    uint8_t state = 0;
    int feeds = 0;
    while (state == 0 && feeds < 2000) {
      const size_t length = want < file.size() ? std::min<size_t>(1020, file.size() - want) : 0;
      std::vector<uint8_t> data = {static_cast<uint8_t>(want), static_cast<uint8_t>(want >> 8),
                                   static_cast<uint8_t>(want >> 16),
                                   static_cast<uint8_t>(want >> 24)};
      data.insert(data.end(), file.begin() + want, file.begin() + want + length);
      const std::vector<uint8_t> r = command(kCover, data);
      state = r[0];
      want = le32(&r[1]);
      ++feeds;
    }
    print("cover_state", state);
    print("cover_feeds", feeds);
    uint8_t thumbState = 0;
    std::vector<uint8_t> part;
    for (int wait = 0; wait < 400; ++wait) {
      part = command(kThumb, {0});
      thumbState = part[0];
      if (thumbState != 0) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    print("thumb_state", thumbState);
    if (thumbState == 1) {
      std::vector<uint8_t> sprite;
      for (uint8_t index = 0; index < 4; ++index) {
        part = command(kThumb, {index});
        if (part.size() != 514 || part[1] != index) {
          fail("thumb part %u: %u bytes", index, static_cast<unsigned>(part.size()));
        }
        sprite.insert(sprite.end(), part.begin() + 2, part.end());
      }
      bool uniform = true;
      for (uint8_t byte : sprite) {
        uniform = uniform && byte == sprite[0];
      }
      print("thumb_bytes", static_cast<long long>(sprite.size()));
      print("thumb_uniform", uniform ? 1 : 0);
      print("thumb_byte", sprite[0]);
    }
    const std::vector<uint8_t> info = command(kInfo);
    printf("title_hex=");
    for (size_t index = 4; index < info.size() && info[index] != 0; ++index) {
      printf("%02x", info[index]);
    }
    printf("\n");
  } else if (scenario == "picture") {
    print("open", command(kOpen, {0})[0]);
    const uint32_t total = static_cast<uint32_t>(file.size());
    uint32_t offset = 0;
    uint8_t state = 0;
    int frames = 0;
    bool repeated = false;
    while (state == 0 && frames < 4000) {
      uint32_t at = offset;
      if (!repeated && offset > 2000) {
        at = offset - 500;               // кусок не с того места: ESP его не возьмёт
        repeated = true;
      }
      const size_t length = std::min<size_t>(1016, total - at);
      std::vector<uint8_t> data(8);
      for (int k = 0; k < 4; ++k) {
        data[k] = static_cast<uint8_t>(total >> (8 * k));
        data[4 + k] = static_cast<uint8_t>(at >> (8 * k));
      }
      data.insert(data.end(), file.begin() + at, file.begin() + at + length);
      const std::vector<uint8_t> r = command(kPicture, data);
      state = r[0];
      offset = le32(&r[1]);
      ++frames;
    }
    print("picture_state", state);
    print("picture_frames", frames);
    uint8_t thumbState = 0;
    for (int wait = 0; wait < 400 && thumbState == 0; ++wait) {
      thumbState = command(kThumb, {0})[0];
      if (thumbState == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
    print("thumb_state", thumbState);
    if (thumbState == 1) {
      print("thumb_byte", command(kThumb, {3})[2 + 511]);
    }
  } else if (scenario == "mp3" || scenario == "flush" || scenario == "pause" ||
             scenario == "stop" || scenario == "drainflush") {
    std::vector<uint8_t> body = {0};
    if (sized || !ext.empty()) {
      const uint32_t size = sized ? static_cast<uint32_t>(file.size()) : 0;
      body.insert(body.end(), {static_cast<uint8_t>(size), static_cast<uint8_t>(size >> 8),
                               static_cast<uint8_t>(size >> 16), static_cast<uint8_t>(size >> 24)});
    }
    if (!ext.empty()) {
      // [0][длина][флаги: перемотка, свой файл][расширение] — как плагин 1.4.
      body.push_back(3);
      for (size_t i = 0; i < 3; ++i) {
        body.push_back(i < ext.size() ? static_cast<uint8_t>(ext[i]) : ' ');
      }
    }
    const std::vector<uint8_t> r = command(kOpen, body);
    print("open", r[0]);
    print("open_free", r.size() >= 5 ? le32(&r[1]) : 0);
    print("open_buffering", (status().flags >> 2) & 1);
    if (r[0] != 1) {
      fail("open refused");
    }
    if (scenario == "flush") {
      feed(file, 0, cut, block);
      waitSound();
      const uint64_t before = sinkFrames();
      // [время, мс][место в файле] — как плагин 0.9.
      const std::vector<uint8_t> f = command(kFlush, {static_cast<uint8_t>(base),
                                                      static_cast<uint8_t>(base >> 8),
                                                      static_cast<uint8_t>(base >> 16),
                                                      static_cast<uint8_t>(base >> 24),
                                                      static_cast<uint8_t>(resume),
                                                      static_cast<uint8_t>(resume >> 8),
                                                      static_cast<uint8_t>(resume >> 16),
                                                      static_cast<uint8_t>(resume >> 24)});
      print("flush", f[0]);
      print("frames_before_flush", static_cast<long long>(before));
      const Status s = status();
      print("seconds_after_flush", s.seconds);
      feed(file, resume, file.size(), block);
    } else if (scenario == "drainflush") {
      // Файл отдан целиком, поток закрыт для доигрывания (AUDIO_CLOSE [1]) — и тут
      // перемотка (AUDIO_FLUSH): PLAYER 1.7 открывает поток снова.
      feed(file, 0, file.size(), block);
      command(kClose, {1});
      waitSound();
      print("state_draining", status().state);
      const std::vector<uint8_t> f = command(kFlush, {static_cast<uint8_t>(base),
                                                      static_cast<uint8_t>(base >> 8),
                                                      static_cast<uint8_t>(base >> 16),
                                                      static_cast<uint8_t>(base >> 24),
                                                      static_cast<uint8_t>(resume),
                                                      static_cast<uint8_t>(resume >> 8),
                                                      static_cast<uint8_t>(resume >> 16),
                                                      static_cast<uint8_t>(resume >> 24)});
      print("flush", f[0]);
      print("state_after_flush", status().state);
      feed(file, resume, file.size(), block);
    } else if (scenario == "stop") {
      feed(file, 0, file.size(), block);
      command(kClose, {1});
      waitSound();
      stopApp();
      printSink();
      return 0;
    } else if (first != 0) {
      // Первым — хвост сектора за меткой ID3: декодер успевает его увидеть один.
      feed(file, 0, first, block);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      print("frames_before_rest", sinkFrames());
      print("buffering_before_rest", (status().flags >> 2) & 1);
      print("input_before_rest", status().queue);
      print("pcm_ms_before_rest", corePcmQueuedMs());
      print("underruns_before_rest", status().underruns);
      feed(file, first, file.size(), block);
    } else {
      feed(file, 0, file.size(), block);
    }
    {
      // Like the WC plugin: signal EOF immediately after the last file byte.
      // Small FLACs can fit entirely below the prefill threshold, so this must
      // happen before waiting for decoded metadata or sound.
      if (ext == "FLA") {
        command(kClose, {1});
        for (int attempt = 0; attempt < 400; ++attempt) {
          const Status s = status();
          if (s.flags & 3) break;
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
      }
      // Длительность из AUDIO_VIEW, пока звук идёт (у VBR без заголовка она бы скакала).
      printf("stream_durations=");
      for (int probe = 0; probe < 3; ++probe) {
        const std::vector<uint8_t> view = command(kView);
        printf("%s%d", probe ? "," : "", view.size() >= 6 ? le16(&view[2]) : -1);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
      }
      printf("\n");
    }
    {
      // Метки потока, пока он играет (у WAV — LIST INFO).
      const std::vector<uint8_t> tags = command(kTags);
      const char* names[] = {"stream_title", "stream_artist", "stream_album", "stream_year", "stream_track"};
      size_t at = 0;
      for (const char* tag : names) {
        if (at >= tags.size()) {
          break;
        }
        const char* text = reinterpret_cast<const char*>(&tags[at]);
        const size_t length = strnlen(text, tags.size() - at);
        printf("%s=%.*s\n", tag, static_cast<int>(length), text);
        at += length + 1;
      }
    }
    if (coverWait) {
      // Обложка из самого файла: флаг 64 в AUDIO_STATUS, затем миниатюра.
      int flag = 0;
      for (int wait = 0; wait < 400 && !flag; ++wait) {
        flag = (status().flags >> 6) & 1;
        if (!flag) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
      }
      print("cover_flag", flag);
      uint8_t thumbState = 0;
      std::vector<uint8_t> part;
      for (int wait = 0; wait < 400 && flag; ++wait) {
        part = command(kThumb, {0});
        thumbState = part[0];
        if (thumbState != 0) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      print("cover_flag_after_thumb", (status().flags >> 6) & 1);
      print("thumb_state", thumbState);
      if (thumbState == 1) {
        std::vector<uint8_t> sprite;
        for (uint8_t index = 0; index < 4; ++index) {
          part = command(kThumb, {index});
          sprite.insert(sprite.end(), part.begin() + 2, part.end());
        }
        bool uniform = true;
        for (uint8_t byte : sprite) {
          uniform = uniform && byte == sprite[0];
        }
        print("thumb_uniform", uniform ? 1 : 0);
        print("thumb_byte", sprite[0]);
      }
    }
    command(kClose, {1});
    if (scenario == "pause") {
      // Посреди звука: очередь вывода полна, задача плеера чаще всего ждёт в
      // pcm_write (а если хвост куска влез, пауза её не застанет — поэтому
      // пауз несколько).
      long long during = 0;
      for (int pause = 0; pause < pauses; ++pause) {
        waitSound(sinkFrames() + 20000);
        command(kPause, {1});
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const uint64_t paused = sinkFrames();
        std::this_thread::sleep_for(std::chrono::milliseconds(hold));
        during += static_cast<long long>(sinkFrames() - paused);
        if (pause == 0) {
          printStatus("paused_", status());
        }
        command(kPause, {0});
      }
      print("frames_during_pause", during);
    }
    printStatus("", waitFinished(60000));
    printSink();
    if (ext == "FLA") {
      command(kClose, {0});
      print("free_after_close", status().free);
    }
  } else if (scenario == "release") {
    const std::vector<uint8_t> r = command(kOpen, {0x20});
    print("open", r.empty() ? -1 : r[0]);
  } else if (scenario == "module") {
    const uint32_t size = static_cast<uint32_t>(file.size());
    const uint8_t flags = midi ? 0xC0 : 0x80;
    std::vector<uint8_t> body = {flags, static_cast<uint8_t>(size), static_cast<uint8_t>(size >> 8),
                                 static_cast<uint8_t>(size >> 16), static_cast<uint8_t>(size >> 24)};
    for (size_t i = 0; i < ext.size() && i < 3; ++i) {
      body.push_back(static_cast<uint8_t>(ext[i]));
    }
    const std::vector<uint8_t> r = command(kOpen, body);
    print("open", r[0]);
    print("open_free", r.size() >= 5 ? le32(&r[1]) : 0);
    print("open_buffering", (status().flags >> 2) & 1);
    if (r[0] != 1) {
      fail("open refused");
    }
    wholeFeed = true;
    if (stopAt != 0 && stopAt < file.size()) {
      feed(file, 0, stopAt, block);
      const Clock::time_point before = Clock::now();
      command(kClose, {0});
      print("stop_ms", static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  Clock::now() - before).count()));
      printStatus("stopped_", status());
      print("psram_after_stop", static_cast<long long>(psramUsed));
      print("xmp_after_stop", static_cast<long long>(xmpUsed));
      stopApp();
      return 0;
    }
    feed(file, 0, file.size(), block);
    Status s = status();
    for (int wait = 0; s.state == 5; ++wait) {
      if (wait > 30000) {
        fail("module is still loading");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      s = status();
    }
    printStatus("loaded_", s);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    const std::vector<uint8_t> view = command(kView);
    print("view_kind", view[0]);
    print("view_count", view[1]);
    print("view_bytes", static_cast<long long>(view.size()));
    if (late > 0 && s.state == 1) {
      // Окно посреди звука: значения индикаторов через --late мс.
      std::this_thread::sleep_for(std::chrono::milliseconds(late));
      const std::vector<uint8_t> later = command(kView);
      printf("view_late=");
      for (size_t i = 6; i < later.size(); ++i) {
        printf("%s%u", i > 6 ? "," : "", later[i]);
      }
      printf("\n");
    }
    const std::vector<uint8_t> tags = command(kTags);
    printf("tags_title=%s\n", reinterpret_cast<const char*>(tags.data()));
    printf("tags_title_hex=");                  // CP866 байтами: кириллица в выводе не теряется
    for (size_t index = 0; index < tags.size() && tags[index] != 0; ++index) {
      printf("%02x", tags[index]);
    }
    printf("\n");
    print("tags_bytes", static_cast<long long>(tags.size()));
    {
      // Все строки меток подряд: исполнитель, альбом, год, номер, у OPL — микросхемы.
      const char* names[] = {"tags_artist", "tags_album", "tags_year", "tags_track", "tags_extra"};
      size_t at = strnlen(reinterpret_cast<const char*>(tags.data()), tags.size()) + 1;
      for (const char* tag : names) {
        if (at >= tags.size()) {
          break;
        }
        const char* text = reinterpret_cast<const char*>(&tags[at]);
        const size_t length = strnlen(text, tags.size() - at);
        printf("%s=%.*s\n", tag, static_cast<int>(length), text);
        at += length + 1;
      }
    }
    if (tags.size() >= 8) {
      print("tags_samples", le16(&tags[tags.size() - 3]));
      print("tags_effects", tags[tags.size() - 1]);
    }
    print("view_duration", view.size() >= 6 ? le16(&view[2]) : -1);
    const std::vector<uint8_t> info = command(kInfo);
    printf("title=%s\n", reinterpret_cast<const char*>(&info[4]));
    print("info_format", info[0]);
    print("info_voices", info[1]);
    print("info_length", le16(&info[2]));
    if (seek != 0 && s.state == 1) {
      const std::vector<uint8_t> k = command(kSeek, {static_cast<uint8_t>(seek)});
      print("seek", k[0]);
      print("seek_order", le16(&k[1]));
      print("frames_at_seek", static_cast<long long>(sinkFrames()));
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      print("seconds_after_seek", status().seconds);
    }
    if (s.state == 1) {
      const Clock::time_point until = Clock::now() + std::chrono::seconds(120);
      bool pausedOnce = false;
      double firstViewMs = -1, firstAudioMs = -1;
      const auto syncStart = Clock::now();
      for (;;) {
        if (viewSync) {
          const auto syncView = command(kView);
          int level = 0;
          for (size_t k = 6; k < syncView.size(); ++k) {
            if (syncView[k] > level) level = syncView[k];
          }
          std::lock_guard<std::mutex> syncLock(sink.lock);
          sink.drainLocked();
          const double now = std::chrono::duration<double, std::milli>(Clock::now() - syncStart).count();
          if (firstViewMs < 0 && level > 0) {
            firstViewMs = now;
            printf("sync_view_at_ms=%.3f\n", now);
          }
          if (firstAudioMs < 0 && sink.firstToneFrame >= 0 && sink.played >= sink.firstToneFrame) {
            firstAudioMs = now;
            printf("sync_audio_at_ms=%.3f\n", now);
            printf("sync_pcm_onset_ms=%.3f\n", sink.firstToneFrame * 1000.0 / sink.rate);
          }
        }
        s = status();
        if (s.state == 0 && (s.flags & 1)) {
          break;
        }
        if (pauseAt > 0 && !pausedOnce && sinkFrames() >= static_cast<uint64_t>(pauseAt) * 44100 / 1000) {
          pausedOnce = true;
          command(kPause, {1});
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          const uint64_t before = sinkFrames();
          std::this_thread::sleep_for(std::chrono::milliseconds(hold));
          print("frames_during_pause", static_cast<long long>(sinkFrames() - before));
          printStatus("paused_", status());
          command(kPause, {0});
        }
        if (sinkFrames() >= seconds * 44100) {
          command(kClose, {0});
          s = status();
          break;
        }
        if (Clock::now() > until) {
          fail("module did not end");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (viewSync) printf("sync_lead_ms=%.3f\n", firstAudioMs - firstViewMs);
    }
    printStatus("", s);
    printSink();
    if (releaseAfter != 0) {
      const std::vector<uint8_t> r = command(kOpen, {0x20});
      print("release_open", r.empty() ? -1 : r[0]);
    }
  } else if (scenario == "radio") {
    // Плейлист M3U целиком, как модуль (расширение M3U); станции — сеть стенда.
    const uint32_t size = static_cast<uint32_t>(file.size());
    const std::vector<uint8_t> r = command(kOpen, {0x80, static_cast<uint8_t>(size),
                                                   static_cast<uint8_t>(size >> 8),
                                                   static_cast<uint8_t>(size >> 16),
                                                   static_cast<uint8_t>(size >> 24), 'M', '3', 'U'});
    print("open", r[0]);
    print("open_free", r.size() >= 5 ? le32(&r[1]) : 0);
    print("open_buffering", (status().flags >> 2) & 1);
    if (r[0] != 1) {
      fail("open refused");
    }
    wholeFeed = true;
    feed(file, 0, file.size(), block);
    Status s = status();
    for (int wait = 0; s.state == 5 && wait < 30000; ++wait) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      s = status();
    }
    printStatus("loaded_", s);
    // Метки: [станция][песня или состояние][«Station N of M»][][][формат].
    auto printTags = [](const char* prefix) {
      const std::vector<uint8_t> tags = command(kTags);
      const char* names[] = {"title", "artist", "album", "year", "track", "extra"};
      size_t at = 0;
      for (const char* name : names) {
        if (at >= tags.size()) {
          break;
        }
        const char* text = reinterpret_cast<const char*>(&tags[at]);
        const size_t length = strnlen(text, tags.size() - at);
        printf("%s%s=%.*s\n", prefix, name, static_cast<int>(length), text);
        at += length + 1;
      }
    };
    // Звук секунду и метки с песней (флаг 32 — метки сменились).
    auto waitSound = [](uint64_t frames, int ms) {
      const Clock::time_point until = Clock::now() + std::chrono::milliseconds(ms);
      while (sinkFrames() < frames && Clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    };
    if (s.state == 1 && (s.flags & 2)) {        // пропуск файла (не плейлист) — сразу конец
      waitSound(44100, waitMs);
      s = status();
      print("first_tags_flag", (s.flags >> 5) & 1);
      printTags("first_");
      print("first_flag_after_tags", (status().flags >> 5) & 1);
      printStatus("first_", status());
      const std::vector<uint8_t> info = command(kInfo);
      printf("info_title=%s\n", reinterpret_cast<const char*>(&info[4]));
      if (seek != 0) {
        const uint64_t before = sinkFrames();
        const std::vector<uint8_t> k = command(kSeek, {static_cast<uint8_t>(seek)});
        print("seek", k[0]);
        print("seek_station", le16(&k[1]));
        printTags("switch_");
        // Список станций вместо столбиков: строки без кодов цвета (до #0D).
        const std::vector<uint8_t> list = command(kView);
        print("list_kind", list.empty() ? -1 : list[0]);
        int row = 0;
        std::string line;
        for (size_t i = 6; i <= list.size(); ++i) {
          const uint8_t c = i < list.size() ? list[i] : 0;
          if (c == 0 || c == 0x0D) {
            while (!line.empty() && line.back() == ' ') {
              line.pop_back();
            }
            printf("list_%d=%s\n", row++, line.c_str());
            line.clear();
            if (c == 0) {
              break;
            }
          } else if (c >= 0x20) {
            line.push_back(static_cast<char>(c));
          }
        }
        print("list_rows", row);
        waitSound(before + 44100, waitMs);
        if (listWait) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10300));
          const std::vector<uint8_t> after = command(kView);
          print("after_list_kind", after.empty() ? -1 : after[0]);
        }
        printTags("second_");
        printStatus("second_", status());
      }
      if (pauseAt > 0) {
        command(kPause, {1});
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const uint64_t before = sinkFrames();
        std::this_thread::sleep_for(std::chrono::milliseconds(hold));
        print("frames_during_pause", static_cast<long long>(sinkFrames() - before));
        command(kPause, {0});
      }
      waitSound(static_cast<uint64_t>(seconds * 44100), waitMs + static_cast<int>(seconds * 4000));
      printTags("last_");
      printStatus("last_", status());          // до стопа: стоп обнуляет счётчики
    }
    command(kClose, {0});
    printStatus("", status());
    printSink();
    {
      std::lock_guard<std::mutex> lock(netLock);
      print("net_opens", netOpens);
      for (size_t i = 0; i < netUrls.size() && i < 8; ++i) {
        printf("net_url_%u=%s\n", static_cast<unsigned>(i + 1), netUrls[i].c_str());
      }
    }
  } else {
    fail("unknown scenario %s", scenario.c_str());
  }
  print("replies_outside", repliesOutside);
  print("bank_releases", bankReleases);
  print("rom_releases", romReleases);
  print("resource_releases", resourceReleases);
  {
    std::lock_guard<std::mutex> lock(allocLock);
    print("psram_peak", static_cast<long long>(psramPeak));
    print("internal_peak", static_cast<long long>(internalPeak));
    print("xmp_peak", static_cast<long long>(xmpPeak));
  }
  stopApp();
  if (sink.raw != nullptr) {
    fclose(sink.raw);
  }
  return 0;
}
