// ESP32-S3 платы VDAC4 для эмулятора Unreal (режим TS_ZIFI32=VDAC4,
// zifi32/vdac4_esp.cpp в E:\zx\unreal-vdac4): Unreal эмулирует порты SPI
// TS-Config, общий буфер SPI Slave HD и канальный уровень прошивки
// (src/spi_link_core.cpp), а поток байтов канала в обе стороны отдаёт сюда
// через два именованных канала.
//
// Здесь — настоящий код прошивки там, где он не трогает железо:
// - кадры: FrameParser и writeFrame (src/protocol.cpp); нечётный кадр
//   добивается нулём, как у канала SPI (Z80Transport::send, padsFrames);
// - разбор команд ядра в порядке Application::handlePacket (src/main.cpp):
//   свои команды ядра (среди них LINK_PROBE #3E) раньше приложений; дальше
//   ModuleApp, затем встроенный плеер (здесь его нет — команда отвергается с
//   записью в журнал), иначе «unsupported:XX»;
// - ModuleApp (src/apps/module_app.cpp): MOD_QUERY/PUT/COMMIT, APP_START/STOP
//   и кэш модулей ZmCache (src/zm_cache.cpp) в каталоге на диске — он, как
//   flash ESP, переживает перезапуск; ресурсы (банк MIDI, ПЗУ OPL4) — в памяти до выхода;
// - PLAYER.ELF, MP3.ELF, AAC.ELF, OGG.ELF, FLAC.ELF, TRACKER.ELF, MIDI.ELF, OPL.ELF,
//   SAA.ELF, TSFM.ELF — их исходники,
//   собранные для ПК: вместо загрузки ELF вызывается та же точка входа, и только если модуль
//   лежит в кэше (как на ESP);
// - PSRAM: память ZM_MEM_PSRAM, ресурсов и доставки считается против
//   kPsramBytes — сколько её у модулей на плате, — чтобы и здесь не хватало
//   там же, где на ESP;
// - сеть для заставок погоды (VDAC4WTH.WMF, VDAC4WT2.WMF): WIFI_INI и WEATHER_GET —
//   модель сетевого ядра прошивки (connectWifi 0.1.33: Wi-Fi в начале выключен, если
//   не задано VDAC4_WIFI=on, подключение — VDAC4_WIFI_MS мс, ждать не дольше 10 с,
//   начатое не обрывается; VDAC4_WIFI_OLD=1 — как до 0.1.33), погода настоящая —
//   запросы WeatherService через WinHTTP, разбор src/weather_parse.cpp;
// - сеть zifi.spg: NET_HTTP_GET, NET_RECV, NET_CLOSE (WinHTTP, HTTPS у порта 443, любой
//   код ответа; ошибка чтения — доклад, а не конец тела), NET_NTP (UTC ПК плюс пояс из
//   time:, VDAC4_NTP=fail — отказ), SYS_INFO, статус прокси (выключен);
//   VDAC4_NET_LOCAL=1 — только 127.0.0.1 и localhost;
// - обложка: cover_glue.cpp и cover_art.cpp; JPEG сводит WIC Windows вместо
//   TJpgDec ПЗУ ESP32-S3, с теми же ограничениями (baseline YCbCr, масштаб
//   1/1..1/8 по тому же правилу);
// - обновлятор WC (плагин WC Update, WCU_START/APPLY/STOP/SYNC): настоящие WcUpdater,
//   мост VFS и транспорт прошивки (wcu_glue.cpp). Сеанс читает и пишет карту Evo
//   запросами VFS к Z80, как на плате, а GitHub спрашивает по-настоящему, через WinHTTP;
// - SYS_RESET — как перезапуск ESP: ACK, сеанс обновлятора и приложение
//   останавливаются, Wi-Fi с уже известной сетью подключается заново;
// - флешка в разъёме USB модуля (0.1.43): VDAC4_USB=<папка ПК> — её корень. Ресурсы
//   (банк MIDI, ПЗУ OPL4) ядро ищет и кладёт в <папка>/SYSTEM/CACHE/MUSIC тем же кодом,
//   что прошивка (src/resource_disk_cache.cpp); без переменной флешки нет;
// - SYS_CLOCK (часы ESP по RTC Evo): проверка, как в прошивке, и запись в журнал —
//   часы ПК не трогаются;
// - обновление прошивки с GitHub (плагин VDAC4 Online Update, ONLINE_UPDATE_CHECK и
//   ONLINE_UPDATE): манифест и firmware.bin качаются по-настоящему с того же адреса,
//   что у прошивки (ZIFI_UPDATE_DIRECTORY в board.hpp), заголовок образа проверяет
//   src/ota_image.cpp, SHA-256 — BCrypt Windows; в flash ничего не пишется, после
//   успеха — модель перезапуска. Версия «установленной» прошивки — VDAC4_FW_VERSION
//   (по умолчанию vdac4-unreal: отношение «другая сборка»); после установки — та,
//   что поставлена.
// Кадры от Unreal принимает отдельный поток. Главный поток — «ядро 1» прошивки: по
// очереди исполняет заявку сеанса обновлятора к карте (мост VFS) и разбирает команду
// Z80, поэтому кадр, пришедший во время обмена с картой, видит транспорт моста.
// Очередь вывода звука — модель AudioPlayer (16 КБ, сливается со скоростью
// звука), звук — ещё и в колонки ПК (waveOut).
//
// Вызов (его делает Unreal): vdac4_esp_host <канал к ESP> <канал от ESP> <журнал> <кэш>.

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <bcrypt.h>
#include <mmsystem.h>
#include <wincodec.h>
#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "zifi/board.hpp"
#include "zifi/config.hpp"
#include "zifi/cover_art.hpp"
#include "zifi/ota_image.hpp"
#include "zifi/protocol.hpp"
#include "zifi/resource_disk_cache.hpp"
#include "zifi/weather_parse.hpp"
#include "zifi/zm_cache.hpp"

#include "wcu_glue.hpp"

extern "C" {
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

using namespace zifi;
using Clock = std::chrono::steady_clock;

namespace {

// --- журнал (как консоль прошивки: «I (мс) тег: текст») -------------------------

FILE* logFile = nullptr;
std::mutex logLock;
const Clock::time_point started = Clock::now();

uint32_t uptimeMs() {
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
}

void logLine(const char* tag, const char* format, va_list args) {
  char text[512];
  vsnprintf(text, sizeof(text), format, args);
  std::lock_guard<std::mutex> lock(logLock);
  if (logFile != nullptr) {
    fprintf(logFile, "I (%u) %s: %s\n", uptimeMs(), tag, text);
    fflush(logFile);
  }
}

void hostLog(const char* tag, const char* format, ...) {
  va_list args;
  va_start(args, format);
  logLine(tag, format, args);
  va_end(args);
}

// --- канал к Unreal ------------------------------------------------------------------

HANDLE fromUnreal = INVALID_HANDLE_VALUE;
HANDLE toUnreal = INVALID_HANDLE_VALUE;

bool writeAll(const uint8_t* data, size_t length) {
  while (length != 0) {
    DWORD written = 0;
    if (!WriteFile(toUnreal, data, static_cast<DWORD>(length), &written, nullptr)) {
      return false;
    }
    data += written;
    length -= written;
  }
  return true;
}

bool writerThunk(void*, const uint8_t* data, size_t length) { return writeAll(data, length); }

// Кадры к Z80 шлют главный поток, сетевой поток и сеанс обновлятора: каждый кадр
// уходит под этим замком целиком, чужой его не разорвёт.
std::mutex sendLock;

// Как Z80Transport::send на канале SPI: кадр нечётной длины добивается нулём.
bool send(uint8_t command, const uint8_t* data = nullptr, uint16_t length = 0) {
  const size_t frameLength = static_cast<size_t>(length) + 5;
  static const uint8_t kPad = 0;
  std::lock_guard<std::mutex> lock(sendLock);
  return writeFrame(writerThunk, nullptr, command, data, length) &&
         ((frameLength & 1U) == 0 || writeAll(&kPad, 1));
}

// Готовый кадр (его собрал транспорт моста VFS) — целиком.
bool sendRaw(const uint8_t* data, size_t length) {
  std::lock_guard<std::mutex> lock(sendLock);
  return writeAll(data, length);
}

// --- принятые кадры: поток приёма -> ядро 1 -------------------------------------------
// Поток приёма разбирает байты канала и кладёт каждый целый кадр сюда заново
// собранным (без выравнивания). Берёт их главный поток: разбор команд — целыми
// кадрами, транспорт моста VFS — байтами, но не дальше конца кадра.

std::mutex inLock;
std::condition_variable inSignal;
std::deque<std::vector<uint8_t>> incoming;
bool channelClosed = false;
std::vector<uint8_t> inFrame;          // кадр, который читает транспорт моста
size_t inOffset = 0;

bool appendThunk(void* context, const uint8_t* data, size_t length) {
  auto* out = static_cast<std::vector<uint8_t>*>(context);
  out->insert(out->end(), data, data + length);
  return true;
}

void receiveLoop() {
  FrameParser parser;
  uint8_t data[4096];
  for (;;) {
    DWORD got = 0;
    if (!ReadFile(fromUnreal, data, sizeof(data), &got, nullptr) || got == 0) {
      break;
    }
    for (DWORD index = 0; index < got; ++index) {
      PacketView packet;
      if (parser.feed(data[index], uptimeMs(), packet)) {
        std::vector<uint8_t> frame;
        writeFrame(appendThunk, &frame, packet.command, packet.data, packet.length);
        std::lock_guard<std::mutex> lock(inLock);
        incoming.push_back(std::move(frame));
        inSignal.notify_all();
      }
    }
  }
  std::lock_guard<std::mutex> lock(inLock);
  channelClosed = true;
  inSignal.notify_all();
}

// Следующий целый кадр; false — за timeoutMs кадров не было или канал закрыт.
bool takeFrame(std::vector<uint8_t>& frame, uint32_t timeoutMs) {
  std::unique_lock<std::mutex> lock(inLock);
  inSignal.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                    [] { return !incoming.empty() || channelClosed; });
  if (incoming.empty()) {
    return false;
  }
  frame = std::move(incoming.front());
  incoming.pop_front();
  return true;
}

bool channelDone() {
  std::lock_guard<std::mutex> lock(inLock);
  return channelClosed && incoming.empty();
}

// Транспорт моста VFS (ядро 1): байты очередного кадра.
size_t readForBridge(uint8_t* output, size_t capacity) {
  if (inOffset == inFrame.size()) {
    std::lock_guard<std::mutex> lock(inLock);
    if (incoming.empty()) {
      return 0;
    }
    inFrame = std::move(incoming.front());
    incoming.pop_front();
    inOffset = 0;
  }
  const size_t count = inFrame.size() - inOffset < capacity ? inFrame.size() - inOffset : capacity;
  memcpy(output, inFrame.data() + inOffset, count);
  inOffset += count;
  return count;
}

void sendError(const char* text) {
  const size_t length = strlen(text) < 48 ? strlen(text) : 48;
  send(kError, reinterpret_cast<const uint8_t*>(text), static_cast<uint16_t>(length));
}

// --- ядро для приложения: память, задачи, синхронизация ----------------------------

// Модель PSRAM платы: свободно после загрузки ~8,0 МБ, из них код модулей ELF и
// мелкая память libc модулей здесь не считаются — остаток с запасом.
constexpr size_t kPsramBytes = 7500 * 1000;
std::mutex psramLock;
std::vector<std::pair<void*, size_t>> psramBlocks;
size_t psramUsed = 0;

void* psramAlloc(size_t size) {
  std::lock_guard<std::mutex> lock(psramLock);
  if (psramUsed + size > kPsramBytes) {
    hostLog("zifi-zm", "PSRAM: %u bytes refused, %u of %u used", static_cast<unsigned>(size),
            static_cast<unsigned>(psramUsed), static_cast<unsigned>(kPsramBytes));
    return nullptr;
  }
  void* pointer = calloc(1, size != 0 ? size : 1);
  if (pointer != nullptr) {
    psramBlocks.emplace_back(pointer, size);
    psramUsed += size;
  }
  return pointer;
}

// Вернуть блок PSRAM; false — блок не из PSRAM.
bool psramFree(void* pointer) {
  std::lock_guard<std::mutex> lock(psramLock);
  for (size_t i = 0; i < psramBlocks.size(); ++i) {
    if (psramBlocks[i].first == pointer) {
      psramUsed -= psramBlocks[i].second;
      psramBlocks.erase(psramBlocks.begin() + static_cast<std::ptrdiff_t>(i));
      free(pointer);
      return true;
    }
  }
  return false;
}

void* coreAlloc(size_t size, uint32_t caps) {
  if ((caps & ZM_MEM_INTERNAL) == 0) {
    return psramAlloc(size);
  }
  return calloc(1, size);
}

void* coreRealloc(void* pointer, size_t size) {
  {
    std::lock_guard<std::mutex> lock(psramLock);
    for (auto& block : psramBlocks) {
      if (block.first == pointer) {
        if (psramUsed - block.second + size > kPsramBytes) {
          return nullptr;
        }
        void* fresh = realloc(pointer, size);
        if (fresh != nullptr) {
          psramUsed = psramUsed - block.second + size;
          block = {fresh, size};
        }
        return fresh;
      }
    }
  }
  return realloc(pointer, size);
}

void coreFree(void* pointer) {
  if (pointer != nullptr && !psramFree(pointer)) {
    free(pointer);
  }
}

void coreLog(const char* format, ...) {
  va_list args;
  va_start(args, format);
  logLine("zifi-app", format, args);
  va_end(args);
}

uint32_t coreMillis() { return uptimeMs(); }
void coreDelay(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int coreTaskStart(zm_task_fn fn, void* arg, uint32_t, uint8_t, int8_t) {
  std::thread([fn, arg] { fn(arg); }).detach();
  return ZM_OK;
}

void* coreMutexCreate() { return new std::mutex(); }

// Ожидание блокировки дольше секунды — в журнал (наладка зависаний).
void coreMutexLock(void* mutex) {
  std::mutex* m = static_cast<std::mutex*>(mutex);
  if (m->try_lock()) {
    return;
  }
  const uint32_t begin = uptimeMs();
  while (!m->try_lock()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const uint32_t waited = uptimeMs() - begin;
    if (waited == 1000 || waited == 5000) {
      hostLog("zifi-host", "thread %lu waits %u ms for mutex %p", GetCurrentThreadId(),
              static_cast<unsigned>(waited), mutex);
    }
  }
}
void coreMutexUnlock(void* mutex) { static_cast<std::mutex*>(mutex)->unlock(); }

struct Signal {
  std::mutex lock;
  std::condition_variable wake;
  bool given = false;
};

void* coreSignalCreate() { return new Signal(); }

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

bool inCommand = false;
std::atomic<int> currentCommand{-1};     // команда в обработке (сторож)
std::atomic<uint32_t> commandSince{0};

int coreReply(uint8_t response, const void* data, uint16_t length) {
  if (!inCommand) {
    return ZM_ERROR;                    // ответ возможен только на текущую команду
  }
  return send(response, static_cast<const uint8_t*>(data), length) ? ZM_OK : ZM_ERROR;
}

int coreEvent(uint8_t, const void*, uint16_t, uint32_t) { return ZM_BUSY; }

// --- вывод звука ----------------------------------------------------------------------
// Очередь — как AudioPlayer: 4096 кадров, сливается со скоростью звука. Кадры
// ещё и играются в колонки ПК: waveOut с запасом в несколько буферов.

struct Output {
  std::mutex lock;
  uint32_t rate = 0;
  uint32_t channels = 0;
  double queued = 0;
  const double capacity = 4096;
  Clock::time_point last = Clock::now();
  bool paused = false;
  uint8_t volume = 255;
  HWAVEOUT wave = nullptr;
  static constexpr int kBuffers = 8;
  static constexpr int kBufferFrames = 2048;
  WAVEHDR headers[kBuffers] = {};
  std::vector<int16_t> data[kBuffers];
  int next = 0;
  std::vector<int16_t> pending;

  void drainLocked() {
    const Clock::time_point now = Clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - last).count();
    last = now;
    if (!paused && rate != 0) {
      queued -= ms * rate / 1000.0;
      if (queued < 0) {
        queued = 0;
      }
    }
  }

  void closeWaveLocked() {
    if (wave != nullptr) {
      waveOutReset(wave);
      for (WAVEHDR& header : headers) {
        if (header.dwFlags & WHDR_PREPARED) {
          waveOutUnprepareHeader(wave, &header, sizeof(header));
        }
        header = {};
      }
      waveOutClose(wave);
      wave = nullptr;
    }
    pending.clear();
  }

  void openWaveLocked() {
    closeWaveLocked();
    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = static_cast<WORD>(channels == 1 ? 1 : 2);
    format.nSamplesPerSec = rate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * 2);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    if (waveOutOpen(&wave, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
      wave = nullptr;
    }
    next = 0;
  }

  // Последний неполный буфер — в колонки, и дождаться, пока они доиграют (не дольше
  // 2 с). Прежде waveOutWrite шёл только целыми буферами по kBufferFrames, а
  // закрытие делало waveOutReset: конец файла (до 2047 кадров) не звучал вовсе.
  void finishLocked(std::unique_lock<std::mutex>& lock) {
    if (wave == nullptr) {
      return;
    }
    if (!pending.empty()) {
      WAVEHDR& header = headers[next];
      if (header.dwFlags & WHDR_PREPARED) {
        while (!(header.dwFlags & WHDR_DONE)) {
          lock.unlock();
          Sleep(5);
          lock.lock();
        }
        waveOutUnprepareHeader(wave, &header, sizeof(header));
      }
      data[next].assign(pending.begin(), pending.end());
      pending.clear();
      header = {};
      header.lpData = reinterpret_cast<LPSTR>(data[next].data());
      header.dwBufferLength = static_cast<DWORD>(data[next].size() * 2);
      waveOutPrepareHeader(wave, &header, sizeof(header));
      waveOutWrite(wave, &header, sizeof(header));
      next = (next + 1) % kBuffers;
    }
    for (int waited = 0; waited < 2000; waited += 10) {
      bool playing = false;
      for (const WAVEHDR& header : headers) {
        if ((header.dwFlags & WHDR_PREPARED) && !(header.dwFlags & WHDR_DONE)) {
          playing = true;
        }
      }
      if (!playing || paused) {
        break;
      }
      lock.unlock();
      Sleep(10);
      lock.lock();
    }
  }

  void playLocked(const float* frames, uint32_t count) {
    if (wave == nullptr) {
      return;
    }
    const uint32_t ch = channels == 1 ? 1 : 2;
    const float gain = volume / 255.0f;
    for (uint32_t i = 0; i < count * ch; ++i) {
      float s = frames[i] * gain;
      s = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
      pending.push_back(static_cast<int16_t>(s * 32767.0f));
    }
    while (pending.size() >= static_cast<size_t>(kBufferFrames) * ch) {
      WAVEHDR& header = headers[next];
      if ((header.dwFlags & WHDR_PREPARED) && !(header.dwFlags & WHDR_DONE)) {
        pending.erase(pending.begin(), pending.begin() + kBufferFrames * ch);  // колонки не успевают
        continue;
      }
      if (header.dwFlags & WHDR_PREPARED) {
        waveOutUnprepareHeader(wave, &header, sizeof(header));
      }
      data[next].assign(pending.begin(), pending.begin() + kBufferFrames * ch);
      pending.erase(pending.begin(), pending.begin() + kBufferFrames * ch);
      header = {};
      header.lpData = reinterpret_cast<LPSTR>(data[next].data());
      header.dwBufferLength = static_cast<DWORD>(data[next].size() * 2);
      waveOutPrepareHeader(wave, &header, sizeof(header));
      waveOutWrite(wave, &header, sizeof(header));
      next = (next + 1) % kBuffers;
    }
  }
} output;

int corePcmOpen(const zm_pcm_format_t* format) {
  std::lock_guard<std::mutex> lock(output.lock);
  output.drainLocked();
  if (format->rate != output.rate || format->channels != output.channels || output.wave == nullptr) {
    output.rate = format->rate;
    output.channels = format->channels;
    output.openWaveLocked();
  }
  return ZM_OK;
}

int corePcmWrite(const float* frames, uint32_t count) {
  uint32_t done = 0;
  double waited = 0;
  while (done < count) {
    std::unique_lock<std::mutex> lock(output.lock);
    if (output.rate == 0) {
      return 0;
    }
    output.drainLocked();
    const double room = output.capacity - output.queued;
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
    const uint32_t ch = output.channels == 1 ? 1 : 2;
    output.playLocked(frames + done * ch, part);
    output.queued += part;
    done += part;
  }
  return static_cast<int>(done);
}

uint32_t corePcmQueuedMs() {
  std::lock_guard<std::mutex> lock(output.lock);
  output.drainLocked();
  return output.rate == 0 ? 0 : static_cast<uint32_t>(output.queued * 1000.0 / output.rate);
}

void corePcmFlush() {
  std::lock_guard<std::mutex> lock(output.lock);
  output.queued = 0;
  output.pending.clear();
  if (output.wave != nullptr) {
    waveOutReset(output.wave);
  }
}

void corePcmClose() {
  std::unique_lock<std::mutex> lock(output.lock);
  output.finishLocked(lock);             // после pcm_flush (стоп) доигрывать нечего
  output.closeWaveLocked();
  output.rate = 0;
  output.channels = 0;
  output.queued = 0;
}

void corePcmPause(int paused) {
  // В журнал: по нему видно, что пришло от клавиши паузы плагина.
  hostLog("zifi-host", "pcm pause %d", paused);
  std::lock_guard<std::mutex> lock(output.lock);
  output.drainLocked();
  output.paused = paused != 0;
  if (output.wave != nullptr) {
    if (output.paused) {
      waveOutPause(output.wave);
    } else {
      waveOutRestart(output.wave);
    }
  }
}

void coreSetVolume(uint8_t value) {
  std::lock_guard<std::mutex> lock(output.lock);
  output.volume = value;
}

uint8_t coreVolume() {
  std::lock_guard<std::mutex> lock(output.lock);
  return output.volume;
}

uint16_t corePcmFaults() { return 0; }

// Температура кристалла для индикатора плагина, десятые °C: 45,2 °C или
// VDAC4_TEMP=<°C> (проверка жёлтого и красного цвета в плеере).
int16_t coreTemperature() {
  const char* text = getenv("VDAC4_TEMP");
  return text ? static_cast<int16_t>(atof(text) * 10.0) : 452;
}

// --- сеть (net_*): интернет-радио через WinHTTP ------------------------------------------
// Как ядро прошивки (src/http_stream.cpp): GET с заголовками модуля, переадресации — сам
// WinHTTP, тело — кусками; время ожидания чтения — своё у каждого вызова.
struct HostNet {
  HINTERNET session = nullptr;
  HINTERNET connect = nullptr;
  HINTERNET request = nullptr;
  std::string headers;                 // «имя:значение\n»
  std::string value;
};

std::wstring widen(const char* text) {
  std::wstring out;
  for (const char* p = text; *p != 0; ++p) {
    out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
  }
  return out;
}

void netFree(HostNet* n) {
  if (n->request != nullptr) WinHttpCloseHandle(n->request);
  if (n->connect != nullptr) WinHttpCloseHandle(n->connect);
  if (n->session != nullptr) WinHttpCloseHandle(n->session);
  delete n;
}

// anyStatus: вернуть поток и при коде вне 2xx — так делает NET_HTTP_GET прошивки, код
// ответа zifi.spg показывает сам («Error 404»). Модулям (net_open) нужен только 2xx.
HostNet* netOpen(const char* url, const char* extra, uint32_t timeoutMs, int* status, bool anyStatus) {
  if (status != nullptr) {
    *status = 0;
  }
  if (url == nullptr) {
    return nullptr;
  }
  const std::wstring wideUrl = widen(url);
  URL_COMPONENTS parts = {};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256] = {};
  wchar_t path[2048] = {};
  parts.lpszHostName = host;
  parts.dwHostNameLength = 255;
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = 2047;
  wchar_t extraInfo[1024] = {};
  parts.lpszExtraInfo = extraInfo;
  parts.dwExtraInfoLength = 1023;
  if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
    hostLog("zifi-zm", "net: %.60s: bad url", url);
    return nullptr;
  }
  HostNet* n = new HostNet;
  n->session = WinHttpOpen(L"VDAC4/unreal", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
  if (n->session == nullptr) {
    netFree(n);
    return nullptr;
  }
  const int timeout = static_cast<int>(timeoutMs);
  WinHttpSetTimeouts(n->session, timeout, timeout, timeout, timeout);
  n->connect = WinHttpConnect(n->session, host, parts.nPort, 0);
  std::wstring object = std::wstring(path) + extraInfo;
  if (object.empty()) {
    object = L"/";
  }
  if (n->connect != nullptr) {
    n->request = WinHttpOpenRequest(n->connect, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
  }
  const std::wstring headers = extra != nullptr ? widen(extra) : std::wstring();
  if (n->request == nullptr ||
      !WinHttpSendRequest(n->request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                          headers.empty() ? 0 : static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(n->request, nullptr)) {
    hostLog("zifi-zm", "net: %.60s: no connection (%u)", url, static_cast<unsigned>(GetLastError()));
    netFree(n);
    return nullptr;
  }
  DWORD code = 0;
  DWORD size = sizeof(code);
  WinHttpQueryHeaders(n->request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
  if (status != nullptr) {
    *status = static_cast<int>(code);
  }
  // Заголовки ответа: строки «Имя: значение» — в «имя:значение\n».
  DWORD length = 0;
  WinHttpQueryHeaders(n->request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                      WINHTTP_NO_OUTPUT_BUFFER, &length, WINHTTP_NO_HEADER_INDEX);
  std::wstring raw(length / sizeof(wchar_t) + 1, L'\0');
  if (WinHttpQueryHeaders(n->request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                          &raw[0], &length, WINHTTP_NO_HEADER_INDEX)) {
    std::string line;
    for (wchar_t c : raw) {
      if (c == L'\r') {
        continue;
      }
      if (c == L'\n' || c == L'\0') {
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
          size_t start = colon + 1;
          while (start < line.size() && line[start] == ' ') ++start;
          n->headers += line.substr(0, colon) + ":" + line.substr(start) + "\n";
        }
        line.clear();
        if (c == L'\0') break;
        continue;
      }
      line.push_back(c < 256 ? static_cast<char>(c) : '?');
    }
  }
  if (!anyStatus && (code < 200 || code >= 300)) {
    hostLog("zifi-zm", "net: %.60s: HTTP %u", url, static_cast<unsigned>(code));
    netFree(n);
    return nullptr;
  }
  return n;
}

void* coreNetOpen(const char* url, const char* extra, uint32_t timeoutMs, int* status) {
  return netOpen(url, extra, timeoutMs, status, false);
}

const char* coreNetHeader(void* stream, const char* name) {
  HostNet* n = static_cast<HostNet*>(stream);
  const size_t length = strlen(name);
  for (size_t at = 0; at < n->headers.size();) {
    const size_t end = n->headers.find('\n', at);
    const std::string line = n->headers.substr(at, end - at);
    if (line.size() > length && line[length] == ':' && _strnicmp(line.c_str(), name, length) == 0) {
      n->value = line.substr(length + 1);
      return n->value.c_str();
    }
    at = end + 1;
  }
  return nullptr;
}

int coreNetRead(void* stream, void* data, uint32_t count, uint32_t timeoutMs) {
  HostNet* n = static_cast<HostNet*>(stream);
  DWORD timeout = timeoutMs;
  WinHttpSetOption(n->request, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
  DWORD available = 0;
  if (!WinHttpQueryDataAvailable(n->request, &available)) {
    return GetLastError() == ERROR_WINHTTP_TIMEOUT ? 0 : -1;
  }
  if (available == 0) {
    return -1;                         // конец потока
  }
  DWORD read = 0;
  if (!WinHttpReadData(n->request, data, available < count ? available : count, &read) || read == 0) {
    return -1;
  }
  return static_cast<int>(read);
}

void coreNetClose(void* stream) { netFree(static_cast<HostNet*>(stream)); }

// --- кэш модулей и ModuleApp ----------------------------------------------------------

class DiskStorage : public ZmStorage {
 public:
  explicit DiskStorage(const std::string& dir) : dir_(dir) { CreateDirectoryA(dir.c_str(), nullptr); }
  bool read(const char* file, uint8_t* data, size_t size) override {
    FILE* f = fopen(path(file).c_str(), "rb");
    if (f == nullptr) {
      return false;
    }
    fseek(f, 0, SEEK_END);
    const long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    const bool ok = length == static_cast<long>(size) && fread(data, 1, size, f) == size;
    fclose(f);
    return ok;
  }
  bool write(const char* file, const uint8_t* data, size_t size) override {
    FILE* f = fopen(path(file).c_str(), "wb");
    if (f == nullptr) {
      return false;
    }
    const bool ok = fwrite(data, 1, size, f) == size;
    fclose(f);
    return ok;
  }
  bool remove(const char* file) override { return DeleteFileA(path(file).c_str()) != 0; }
  size_t capacityBytes() override { return 1536 * 1024; }

 private:
  std::string path(const char* file) const { return dir_ + "\\" + file; }
  std::string dir_;
};

DiskStorage* storage = nullptr;
ZmCache* cache = nullptr;
zm_core_t core = {};
const zm_app_t* app = nullptr;
char appName[13] = {};
bool appStale = false;
// Запись модуля во flash ESP (LittleFS) идёт секунды, и всё это время ESP
// молчит: VDAC4_COMMIT_MS — сколько так стоит MOD_COMMIT модуля (на диске ПК — мгновенно).
uint32_t commitDelayMs = 0;

constexpr uint32_t kMaxModuleBytes = 2 * 1024 * 1024;   // как module_app.cpp
constexpr uint32_t kMaxResourceBytes = 7 * 1024 * 1024;
uint8_t* delivery = nullptr;                             // в PSRAM, как на ESP
bool deliveryOpen = false;
bool deliveryResource = false;
char deliveryName[13] = {};
uint32_t deliverySize = 0;
uint32_t deliveryStamp = 0;
uint32_t deliveryReceived = 0;

struct Resource {
  char name[13];
  uint8_t* data;
  size_t size;
  uint32_t stamp;                        // время FAT файла, как в прошивке
};
Resource resources[4] = {};

// «Флешка»: папка VDAC4_USB. Как UsbDrive::ready прошивки: есть — каталог кэша
// создаётся при первом обращении (запись о нём — в журнал один раз).
std::string usbCacheDir;
bool usbChecked = false;

bool usbReady() {
  if (!usbChecked) {
    usbChecked = true;
    const char* root = getenv("VDAC4_USB");
    if (root == nullptr || *root == 0) {
      hostLog("zifi-usb", "no drive (VDAC4_USB not set)");
    } else {
      const std::string base(root);
      CreateDirectoryA((base + "\\SYSTEM").c_str(), nullptr);
      CreateDirectoryA((base + "\\SYSTEM\\CACHE").c_str(), nullptr);
      CreateDirectoryA((base + "\\SYSTEM\\CACHE\\MUSIC").c_str(), nullptr);
      const DWORD attributes = GetFileAttributesA((base + "\\SYSTEM\\CACHE\\MUSIC").c_str());
      if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        usbCacheDir = base + "\\SYSTEM\\CACHE\\MUSIC";
      }
      hostLog("zifi-usb", "drive folder %s: %s", root,
              usbCacheDir.empty() ? "cache folder NOT made" : "music cache /SYSTEM/CACHE/MUSIC");
    }
  }
  return !usbCacheDir.empty();
}

// Точка входа модуля по имени — вместо загрузки ELF; модуль обязан лежать в кэше.
int (*entryOf(const char* name))(int, char**) {
  if (strcmp(name, "PLAYER.ELF") == 0) {
    return player_main;
  }
  if (strcmp(name, "MP3.ELF") == 0) {
    return mp3_main;
  }
  if (strcmp(name, "AAC.ELF") == 0) {
    return aac_main;
  }
  if (strcmp(name, "OGG.ELF") == 0) {
    return ogg_main;
  }
  if (strcmp(name, "TRACKER.ELF") == 0) {
    return tracker_main;
  }
  if (strcmp(name, "MIDI.ELF") == 0) {
    return midi_main;
  }
  if (strcmp(name, "OPL.ELF") == 0) {
    return opl_main;
  }
  if (strcmp(name, "FLAC.ELF") == 0) {
    return flac_main;
  }
  if (strcmp(name, "SAA.ELF") == 0) {
    return saa_main;
  }
  if (strcmp(name, "TSFM.ELF") == 0) {
    return tsfm_main;
  }
  return nullptr;
}

int loadModule(const char* name, void* arg1, void* arg2, int* entryResult) {
  const ZmCacheEntry* entry = cache->find(name);
  if (entry == nullptr) {
    return ZM_NOT_FOUND;
  }
  std::vector<uint8_t> file(entry->size);
  if (!cache->read(name, file.data(), entry->size)) {
    return ZM_NOT_FOUND;
  }
  int (*main)(int, char**) = entryOf(name);
  if (main == nullptr || file.size() < 4 || memcmp(file.data(), "\x7F" "ELF", 4) != 0) {
    hostLog("zifi-zm", "%s: relocate failed (no PC build of this module)", name);
    return ZM_ERROR;
  }
  char program[13];
  strncpy(program, name, sizeof(program) - 1);
  program[sizeof(program) - 1] = 0;
  char* argv[] = {program, static_cast<char*>(arg1), static_cast<char*>(arg2), nullptr};
  *entryResult = main(3, argv);
  hostLog("zifi-zm", "%s: PC build of the module, %u bytes in the cache", name,
          static_cast<unsigned>(entry->size));
  return ZM_OK;
}

int coreModuleOpen(const char* name, void* arg1, void* arg2, void** handle, int* entry) {
  *handle = nullptr;
  const int result = loadModule(name, arg1, arg2, entry);
  if (result == ZM_OK) {
    *handle = new int(1);
  }
  return result;
}

void coreModuleClose(void* handle) { delete static_cast<int*>(handle); }
int coreResource(const char* name, const uint8_t** data, size_t* size) {
  for (const Resource& item : resources) {
    if (item.data != nullptr && name != nullptr && strcmp(item.name, name) == 0) {
      if (data != nullptr) {
        *data = item.data;
      }
      if (size != nullptr) {
        *size = item.size;
      }
      return ZM_OK;
    }
  }
  return ZM_NOT_FOUND;
}

void coreResourceRelease(const char* name) {
  for (Resource& item : resources) {
    if (item.data != nullptr && name != nullptr && strcmp(item.name, name) == 0) {
      hostLog("zifi-zm", "resource %s: %u bytes released", item.name,
              static_cast<unsigned>(item.size));
      psramFree(item.data);
      item = Resource{};
    }
  }
}

void replyQuery(uint8_t state) {
  uint8_t reply[5];
  reply[0] = state;
  writeLe32(reply + 1, static_cast<uint32_t>(cache->freeBytes()));
  send(kRespModQuery, reply, sizeof(reply));
}

void dropDelivery() {
  if (delivery != nullptr) {
    psramFree(delivery);
  }
  delivery = nullptr;
  deliveryOpen = false;
  deliveryResource = false;
  deliverySize = deliveryReceived = 0;
  deliveryName[0] = 0;
}

void stopApp();
int startApp(const char* name);

// Место под доставку, как ModuleApp::allocateDelivery: не нашлось — приложение
// прочь (ресурс — потом запустить снова), другие ресурсы — по одному, сначала
// самый большой, подождать временные буферы (здесь их нет: только та же пауза).
uint8_t* allocateDelivery(const char* name, uint32_t size, bool resource) {
  uint8_t* block = static_cast<uint8_t*>(psramAlloc(size));
  if (block != nullptr) {
    return block;
  }
  size_t used = 0;
  {
    std::lock_guard<std::mutex> lock(psramLock);
    used = psramUsed;
  }
  char stopped[sizeof(appName)] = {};
  if (app != nullptr) {
    memcpy(stopped, appName, sizeof(stopped));
    stopApp();
    block = static_cast<uint8_t*>(psramAlloc(size));
  }
  unsigned released = 0;
  while (block == nullptr) {
    Resource* largest = nullptr;
    for (Resource& item : resources) {
      if (item.data != nullptr && (largest == nullptr || item.size > largest->size)) {
        largest = &item;
      }
    }
    if (largest == nullptr) {
      break;
    }
    hostLog("zifi-zm", "resource %s: %u bytes released for room", largest->name,
            static_cast<unsigned>(largest->size));
    psramFree(largest->data);
    *largest = Resource{};
    ++released;
    block = static_cast<uint8_t*>(psramAlloc(size));
  }
  uint32_t waited = 0;
  while (block == nullptr && waited < 2000) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    waited += 50;
    block = static_cast<uint8_t*>(psramAlloc(size));
  }
  hostLog("zifi-zm",
          "room for %s, %u KB: %u KB of %u KB used; app %s stopped, %u resources released, "
          "waited %u ms: %s",
          name, static_cast<unsigned>(size / 1024), static_cast<unsigned>(used / 1024),
          static_cast<unsigned>(kPsramBytes / 1024), stopped[0] != 0 ? stopped : "-", released,
          static_cast<unsigned>(waited), block != nullptr ? "got it" : "NO ROOM");
  if (resource && stopped[0] != 0) {
    startApp(stopped);
  }
  return block;
}

// Ресурс — в свободное место таблицы.
bool adoptResource(const char* name, uint8_t* data, size_t size, uint32_t stamp) {
  for (Resource& item : resources) {
    if (item.data == nullptr) {
      snprintf(item.name, sizeof(item.name), "%s", name);
      item.data = data;
      item.size = size;
      item.stamp = stamp;
      return true;
    }
  }
  return false;
}

uint32_t msSince(Clock::time_point started) {
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
}

// Как ModuleApp::loadResourceFromDrive: годная копия на флешке — в delivery и в таблицу.
bool loadResourceFromDrive(const char* name, uint32_t size, uint32_t stamp) {
  if (!zifi::ResourceDiskCache::cacheable(name) || !usbReady()) {
    return false;
  }
  const zifi::ResourceDiskCache disk(usbCacheDir.c_str());
  if (!disk.fresh(name, size, stamp)) {
    return false;
  }
  const Clock::time_point started = Clock::now();
  const bool read = disk.read(name, delivery, size);
  if (!read || !adoptResource(name, delivery, size, stamp)) {
    hostLog("zifi-zm", "resource %s: %s from the USB drive after %u ms, sent from the card",
            name, read ? "no slot" : "NOT read", static_cast<unsigned>(msSince(started)));
    return false;
  }
  delivery = nullptr;                    // теперь у ресурса
  hostLog("zifi-zm", "resource %s: %u bytes from the USB drive in %u ms", name,
          static_cast<unsigned>(size), static_cast<unsigned>(msSince(started)));
  return true;
}

// Как ModuleApp::copyResourceToDrive: только что принятый ресурс — на флешку.
void copyResourceToDrive(const char* name) {
  if (!zifi::ResourceDiskCache::cacheable(name) || !usbReady()) {
    return;
  }
  for (const Resource& item : resources) {
    if (item.data != nullptr && strcmp(item.name, name) == 0) {
      const zifi::ResourceDiskCache disk(usbCacheDir.c_str());
      const Clock::time_point started = Clock::now();
      const bool written =
          disk.write(item.name, item.data, static_cast<uint32_t>(item.size), item.stamp);
      hostLog("zifi-zm", "resource %s: %s the USB drive in %u ms", item.name,
              written ? "copied to" : "NOT copied to", static_cast<unsigned>(msSince(started)));
      return;
    }
  }
}

void handleQuery(const PacketView& packet) {
  dropDelivery();
  if (packet.length < 10) {
    replyQuery(3);
    return;
  }
  const bool resource = (packet.data[0] & 0x01) != 0;
  const uint32_t size = readLe32(packet.data + 1);
  const uint32_t stamp = readLe32(packet.data + 5);
  char name[13] = {};
  const size_t nameLength =
      strnlen(reinterpret_cast<const char*>(packet.data + 9), packet.length - 9);
  if (nameLength == 0 || nameLength >= sizeof(name)) {
    replyQuery(3);
    return;
  }
  memcpy(name, packet.data + 9, nameLength);
  for (char& c : name) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  if (!ZmCache::validName(name) || size == 0) {
    replyQuery(3);
    return;
  }
  if (resource) {
    for (const Resource& item : resources) {
      if (item.data != nullptr && strcmp(item.name, name) == 0 && item.size == size &&
          item.stamp == stamp) {
        replyQuery(0);
        return;
      }
    }
  } else if (cache->has(name, size, stamp)) {
    replyQuery(0);
    return;
  }
  if (size > (resource ? kMaxResourceBytes : kMaxModuleBytes) ||
      (!resource && !cache->canFit(size, app != nullptr ? appName : nullptr))) {
    replyQuery(2);
    return;
  }
  if (resource) {
    coreResourceRelease(name);
  }
  delivery = allocateDelivery(name, size, resource);
  if (delivery == nullptr) {
    replyQuery(2);
    return;
  }
  // Годная копия на флешке — ресурс уже есть, передавать его с карты не нужно.
  if (resource && loadResourceFromDrive(name, size, stamp)) {
    replyQuery(0);
    return;
  }
  deliveryOpen = true;
  deliveryResource = resource;
  memcpy(deliveryName, name, sizeof(deliveryName));
  deliverySize = size;
  deliveryStamp = stamp;
  deliveryReceived = 0;
  replyQuery(1);
}

void handlePut(const PacketView& packet) {
  uint8_t reply[5] = {0};
  if (deliveryOpen && packet.length >= 4) {
    const uint32_t offset = readLe32(packet.data);
    const uint32_t length = packet.length - 4u;
    if (offset == deliveryReceived && length <= deliverySize - deliveryReceived) {
      memcpy(delivery + offset, packet.data + 4, length);
      deliveryReceived += length;
      reply[0] = 1;
    }
  }
  writeLe32(reply + 1, deliveryReceived);
  send(kRespModPut, reply, sizeof(reply));
}

void handleCommit() {
  uint8_t ok = 0;
  const Clock::time_point started = Clock::now();
  if (deliveryOpen && deliveryReceived == deliverySize) {
    if (deliveryResource) {
      if (adoptResource(deliveryName, delivery, deliverySize, deliveryStamp)) {
        delivery = nullptr;              // теперь у ресурса
        ok = 1;
      }
    } else if (deliverySize >= 4 && memcmp(delivery, "\x7F" "ELF", 4) == 0) {
      ok = cache->store(deliveryName, deliveryStamp, delivery, deliverySize,
                        app != nullptr ? appName : nullptr)
               ? 1
               : 0;
      if (ok) {
        appStale = true;
      }
      if (commitDelayMs != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(commitDelayMs));
      }
    }
    hostLog("zifi-zm", "%s %s: %u bytes %s in %u ms", deliveryResource ? "resource" : "module",
            deliveryName, static_cast<unsigned>(deliverySize), ok ? "stored" : "REFUSED",
            static_cast<unsigned>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      Clock::now() - started)
                                      .count()));
    if (deliveryResource && ok != 0) {
      copyResourceToDrive(deliveryName);
    }
  }
  dropDelivery();
  send(kRespModCommit, &ok, 1);
}

void stopApp() {
  if (app == nullptr) {
    return;
  }
  if (app->stop != nullptr) {
    app->stop();
  }
  hostLog("zifi-zm", "app %s stopped", appName);
  app = nullptr;
  appName[0] = 0;
}

int startApp(const char* name) {
  if (app != nullptr && !appStale && strcmp(appName, name) == 0) {
    return ZM_OK;
  }
  stopApp();
  appStale = false;
  const zm_app_t* candidate = nullptr;
  int entryResult = 0;
  const int result = loadModule(name, &core, &candidate, &entryResult);
  if (result != ZM_OK) {
    hostLog("zifi-zm", "app %s: not loaded (%d)", name, result);
    return result;
  }
  if (entryResult != ZM_APP_ABI_MAJOR || candidate == nullptr ||
      candidate->abi_major != ZM_APP_ABI_MAJOR || candidate->start == nullptr) {
    hostLog("zifi-zm", "app %s: interface %d, not %d", name, entryResult, ZM_APP_ABI_MAJOR);
    return ZM_ERROR;
  }
  app = candidate;
  strncpy(appName, name, sizeof(appName) - 1);
  const Clock::time_point begin = Clock::now();
  const int start = app->start(&core);
  if (start != ZM_OK) {
    hostLog("zifi-zm", "app %s: start failed (%d)", name, start);
    stopApp();
    return start;
  }
  hostLog("zifi-zm", "app %s %s %s started in %u us, commands %02X..%02X", name,
          app->name != nullptr ? app->name : "?", app->version != nullptr ? app->version : "",
          static_cast<unsigned>(std::chrono::duration_cast<std::chrono::microseconds>(
                                    Clock::now() - begin).count()),
          app->command_first, app->command_last);
  return ZM_OK;
}

void handleStart(const PacketView& packet) {
  char name[13] = {};
  const size_t length = strnlen(reinterpret_cast<const char*>(packet.data), packet.length);
  int result = ZM_ERROR;
  if (length > 0 && length < sizeof(name)) {
    memcpy(name, packet.data, length);
    for (char& c : name) {
      if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
      }
    }
    result = startApp(name);
  }
  uint8_t reply[64];
  size_t used = 0;
  reply[used++] = static_cast<uint8_t>(static_cast<int8_t>(result));
  const char* name2 = app != nullptr && app->name != nullptr ? app->name : "";
  const char* version = app != nullptr && app->version != nullptr ? app->version : "";
  const size_t nameLength = strnlen(name2, 24);
  memcpy(reply + used, name2, nameLength);
  used += nameLength;
  reply[used++] = 0;
  const size_t versionLength = strnlen(version, 24);
  memcpy(reply + used, version, versionLength);
  used += versionLength;
  reply[used++] = 0;
  send(kRespAppStart, reply, static_cast<uint16_t>(used));
}

// ModuleApp::handleCommand.
bool moduleCommand(const PacketView& packet) {
  switch (packet.command) {
    case kModQuery:
      handleQuery(packet);
      return true;
    case kModPut:
      handlePut(packet);
      return true;
    case kModCommit:
      handleCommit();
      return true;
    case kAppStart:
      handleStart(packet);
      return true;
    case kAppStop: {
      stopApp();
      const uint8_t ok = 1;
      send(kRespAppStop, &ok, 1);
      return true;
    }
    default:
      break;
  }
  if (app == nullptr || packet.command < app->command_first ||
      packet.command > app->command_last || app->command == nullptr) {
    return false;
  }
  inCommand = true;
  const int result = app->command(packet.command, packet.data, packet.length);
  inCommand = false;
  return result == ZM_OK;
}

// --- Application::handlePacket ----------------------------------------------------------

// Счётчики команд за 2 секунды — в журнал: видно, чем занят плагин.
uint32_t commandCounts[256] = {};
uint32_t countsSince = 0;

void countCommand(uint8_t command) {
  ++commandCounts[command];
  const uint32_t now = uptimeMs();
  if (now - countsSince < 2000) {
    return;
  }
  char line[400];
  size_t used = 0;
  for (int code = 0; code < 256; ++code) {
    if (commandCounts[code] != 0 && used + 16 < sizeof(line)) {
      used += snprintf(line + used, sizeof(line) - used, " %02X:%u", code,
                       static_cast<unsigned>(commandCounts[code]));
    }
  }
  line[used] = 0;
  hostLog("zifi-host", "commands in %u ms:%s", static_cast<unsigned>(now - countsSince), line);
  memset(commandCounts, 0, sizeof(commandCounts));
  countsSince = now;
}

// --- Wi-Fi и погода: модель сетевого ядра прошивки (заставка погоды) ------------------
// Как Application::connectWifi (src/main.cpp, 0.1.33): уже в сети с той же сетью —
// сразу да; начатое подключение к той же сети не обрывается, а дожидается (Arduino
// доводит его само), заново — другая сеть или попытка дольше 30 с; ожидание — не
// дольше kWifiTimeoutMs = 10 с. Не успело — ответ «нет сети», подключение идёт
// дальше в фоне. VDAC4_WIFI_OLD=1 — как до 0.1.33: не в сети — подключение заново
// на каждой команде, а сеть запоминается только при успехе. VDAC4_WIFI=on — ESP
// уже в сети (загрузилась с сохранённым zifi.ini), VDAC4_WIFI_MS — сколько длится
// подключение (по умолчанию 4000 мс).
struct WifiModel {
  bool connected = false;
  bool oldFirmware = false;     // VDAC4_WIFI_OLD=1: connectWifi до 0.1.33
  uint32_t beganMs = 0;         // когда начато подключение (0 — не начиналось)
  bool bootOnline = false;      // VDAC4_WIFI=on: сеть — та, что в первом WIFI_INI
  uint32_t readyAt = 0;         // фоновое подключение кончится тогда (0 — не идёт)
  uint32_t connectMs = 4000;
  std::string activeSsid;
  std::string activePassword;
} wifi;
zifi::IniConfig hostIni;

void wifiPoll() {
  if (wifi.readyAt != 0 && static_cast<int32_t>(uptimeMs() - wifi.readyAt) >= 0) {
    wifi.connected = true;
    wifi.readyAt = 0;
  }
}

bool wifiConnect(const char* ssid, const char* password) {
  wifiPoll();
  const bool same = wifi.activeSsid == ssid && wifi.activePassword == password;
  if (wifi.connected && same) {
    return true;
  }
  const bool restart = wifi.oldFirmware || !same || wifi.beganMs == 0 ||
                       uptimeMs() - wifi.beganMs >= 30000;
  if (restart) {
    wifi.connected = false;
    wifi.readyAt = uptimeMs() + wifi.connectMs;
    wifi.beganMs = uptimeMs() | 1;
    if (!wifi.oldFirmware) {
      wifi.activeSsid = ssid;               // 0.1.33: сеть — та, к которой подключаемся
      wifi.activePassword = password;
    }
  }
  const uint32_t started = uptimeMs();
  while (!wifi.connected && uptimeMs() - started < 10000) {
    Sleep(25);
    wifiPoll();
  }
  if (!wifi.connected) {
    return false;
  }
  wifi.activeSsid = ssid;
  wifi.activePassword = password;
  return true;
}

void handleWifiIni(const PacketView& packet) {
  send(kAck);
  uint8_t reply[5] = {0, 0, 0, 0, 0};
  char error[64] = {};
  if (!hostIni.parse(packet.data, packet.length, error, sizeof(error))) {
    char text[80];
    snprintf(text, sizeof(text), "ini:%s", error[0] != 0 ? error : "invalid");
    sendError(text);
    send(kRespWifiIni, reply, sizeof(reply));
    return;
  }
  if (wifi.bootOnline && wifi.activeSsid.empty()) {
    wifi.activeSsid = hostIni.ssid();           // загрузилась с этой же сетью
    wifi.activePassword = hostIni.password();
  }
  const uint32_t started = uptimeMs();
  const bool connected = wifiConnect(hostIni.ssid(), hostIni.password());
  hostLog("zifi-host", "WIFI_INI: %s after %u ms", connected ? "connected" : "wifi timeout",
          static_cast<unsigned>(uptimeMs() - started));
  if (!connected) {
    sendError("wifi timeout");
  } else {
    reply[0] = 1;
    reply[1] = 192;
    reply[2] = 168;
    reply[3] = 1;
    reply[4] = 110;
  }
  send(kRespWifiIni, reply, sizeof(reply));
}

// GET http://host/path целиком (как NetClient::httpGet прошивки, тело до 4 КБ).
bool httpBody(const char* host, const char* path, std::string& body, char* error, size_t size) {
  std::string url = std::string("http://") + host + path;
  int status = 0;
  void* stream = coreNetOpen(url.c_str(), nullptr, 15000, &status);
  if (stream == nullptr) {
    snprintf(error, size, status != 0 ? "http %d" : "no connection", status);
    return false;
  }
  body.clear();
  char chunk[2048];
  for (;;) {
    const int got = coreNetRead(stream, chunk, sizeof(chunk), 15000);
    if (got <= 0) {
      break;
    }
    body.append(chunk, static_cast<size_t>(got));
  }
  coreNetClose(stream);
  if (status < 200 || status >= 300) {
    snprintf(error, size, "http %d", status);
    return false;
  }
  return true;
}

// Дописать текст в путь URL: буквы, цифры и '-' как есть, остальное — %XX (как в прошивке).
void appendEncoded(std::string& out, const char* text) {
  static const char kHex[] = "0123456789ABCDEF";
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p != 0; ++p) {
    if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
        *p == '-') {
      out += static_cast<char>(*p);
    } else {
      out += '%';
      out += kHex[*p >> 4];
      out += kHex[*p & 15];
    }
  }
}

// WeatherService::get прошивки: место по city: (геокодер) или zip:, затем прогноз.
bool weatherGet(uint8_t* record, char* error, size_t size) {
  wifiPoll();
  const char* city = hostIni.get("city", "");
  const char* country = hostIni.get("country", "");
  const char* zip = hostIni.get("zip", "");
  if (*city == 0 && (*zip == 0 || *country == 0)) {
    snprintf(error, size, "no city in ini");
    return false;
  }
  if (!wifi.connected) {
    snprintf(error, size, "no wifi");
    return false;
  }
  std::string body;
  char text[64] = {};
  zifi::GeoResult geo = {};
  if (*city != 0) {
    char name[zifi::kIniValueSize * 2];
    zifi::iniTextToUtf8(city, name, sizeof(name));
    bool cyrillic = false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p != 0; ++p) {
      cyrillic = cyrillic || (*p >= 0xD0 && *p <= 0xD3);
    }
    std::string path = "/v1/search?name=";
    appendEncoded(path, name);
    path += std::string("&count=1&language=") + (cyrillic ? "ru" : "en") + "&format=json";
    if (*country != 0) {
      path += "&countryCode=";
      appendEncoded(path, country);
    }
    if (!httpBody("geocoding-api.open-meteo.com", path.c_str(), body, text, sizeof(text))) {
      snprintf(error, size, "city: %s", text);
      return false;
    }
    bool notFound = false;
    if (!zifi::parseCitySearch(body.data(), body.size(), geo, notFound, error, size)) {
      return false;
    }
  } else {
    std::string path = "/";
    appendEncoded(path, country);
    path += "/";
    appendEncoded(path, zip);
    if (!httpBody("api.zippopotam.us", path.c_str(), body, text, sizeof(text))) {
      snprintf(error, size, "zip: %s", text);
      return false;
    }
    if (!zifi::parseZippopotam(body.data(), body.size(), geo, error, size)) {
      return false;
    }
  }
  char path[320];
  snprintf(path, sizeof(path),
           "/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,weather_code,is_day,wind_speed_10m,precipitation,surface_pressure"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum,sunrise,sunset"
           "&timezone=auto&forecast_days=%u&timeformat=unixtime",
           static_cast<double>(geo.latitude), static_cast<double>(geo.longitude),
           static_cast<unsigned>(zifi::kWeatherMaxDays));
  if (!httpBody("api.open-meteo.com", path, body, text, sizeof(text))) {
    snprintf(error, size, "meteo: %s", text);
    return false;
  }
  return zifi::parseOpenMeteo(body.data(), body.size(), geo.place, record, error, size);
}

void handleWeatherGet() {
  send(kAck);
  uint8_t record[zifi::kWeatherRecordSize];
  char error[96] = {};
  if (weatherGet(record, error, sizeof(error))) {
    hostLog("zifi-host", "WEATHER_GET: record for %.24s", reinterpret_cast<const char*>(record + 2));
    send(kRespWeatherGet, record, sizeof(record));
    return;
  }
  char text[112];
  snprintf(text, sizeof(text), "weather:%s", error);
  hostLog("zifi-host", "WEATHER_GET: %s", text);
  sendError(text);
  const uint8_t failed[2] = {0, zifi::kWeatherRecordVersion};
  send(kRespWeatherGet, failed, sizeof(failed));
}

// --- сеть zifi.spg: NET_HTTP_GET, NET_RECV, NET_CLOSE, NET_NTP, SYS_INFO, прокси ----------
// Как Application::processHttpGet / processNetRecv / processNetClose прошивки: ACK, затем
// ответ; тело Z80 забирает сам порциями NET_RECV. HTTPS — у порта 443, как NetClient.
// VDAC4_NET_LOCAL=1 — только 127.0.0.1 и localhost (стенд без внешней сети: самопроверка
// zifi.spg к zifi.vtrd.in получит отказ).
HostNet* zifiNet = nullptr;

void handleHttpGet(const PacketView& packet) {
  send(kAck);
  uint8_t reply[7] = {};
  if (zifiNet != nullptr) {
    netFree(zifiNet);
    zifiNet = nullptr;
  }
  const uint8_t* p = packet.data;
  const size_t n = packet.length;
  size_t hostEnd = 0;
  while (hostEnd < n && p[hostEnd] != 0) ++hostEnd;
  if (hostEnd == 0 || hostEnd + 3 > n) {
    sendError("get:bad payload");
    send(kRespNetHttpGet, reply, sizeof(reply));
    return;
  }
  const std::string host(reinterpret_cast<const char*>(p), hostEnd);
  const unsigned port = p[hostEnd + 1] | (p[hostEnd + 2] << 8);
  size_t pathEnd = hostEnd + 3;
  while (pathEnd < n && p[pathEnd] != 0) ++pathEnd;
  std::string path(reinterpret_cast<const char*>(p + hostEnd + 3), pathEnd - hostEnd - 3);
  if (path.empty() || path[0] != '/') {
    path.insert(path.begin(), '/');
  }
  wifiPoll();
  const char* local = getenv("VDAC4_NET_LOCAL");
  const bool localOnly = local != nullptr && strcmp(local, "1") == 0;
  if (!wifi.connected || (localOnly && host != "127.0.0.1" && host != "localhost")) {
    sendError(wifi.connected ? "get:no connection" : "get:no wifi");
    send(kRespNetHttpGet, reply, sizeof(reply));
    return;
  }
  char url[768];
  if (port == 443) {
    snprintf(url, sizeof(url), "https://%s%s", host.c_str(), path.c_str());
  } else {
    snprintf(url, sizeof(url), "http://%s:%u%s", host.c_str(), port, path.c_str());
  }
  int status = 0;
  zifiNet = netOpen(url, "Accept-Encoding: identity", 15000, &status, true);
  if (zifiNet == nullptr) {
    hostLog("zifi-host", "NET_HTTP_GET %.80s: no connection", url);
    sendError("get:no connection");
    send(kRespNetHttpGet, reply, sizeof(reply));
    return;
  }
  const char* header = coreNetHeader(zifiNet, "Content-Length");
  const uint32_t length = header != nullptr ? static_cast<uint32_t>(strtoul(header, nullptr, 10)) : 0;
  reply[0] = 1;
  reply[1] = static_cast<uint8_t>(status);
  reply[2] = static_cast<uint8_t>(status >> 8);
  for (int i = 0; i < 4; ++i) {
    reply[3 + i] = static_cast<uint8_t>(length >> (8 * i));
  }
  hostLog("zifi-host", "NET_HTTP_GET %.80s: HTTP %d, Content-Length %u", url, status,
          static_cast<unsigned>(length));
  send(kRespNetHttpGet, reply, sizeof(reply));
}

// NET_RECV без ACK: [конец][данные]. Пустая порция без признака конца — «данных пока нет».
// Конец тела и ошибка чтения различаются, как в processNetRecv прошивки: ошибка — доклад
// #EE «recv:…» перед ответом [1]. Иначе обрыв тела без Content-Length сошёл бы за весь файл.
void handleNetRecv(const PacketView& packet) {
  size_t limit = kMaxPayload - 1;
  if (packet.length >= 2) {
    limit = packet.data[0] | (packet.data[1] << 8);
    if (limit < 1) limit = 1;
    if (limit > kMaxPayload - 1) limit = kMaxPayload - 1;
  }
  uint8_t reply[kMaxPayload];
  reply[0] = 1;
  if (zifiNet == nullptr) {
    sendError("recv:not connected");
    send(kRespNetRecv, reply, 1);
    return;
  }
  DWORD timeout = 2000;
  WinHttpSetOption(zifiNet->request, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
  DWORD available = 0;
  if (!WinHttpQueryDataAvailable(zifiNet->request, &available)) {
    const DWORD code = GetLastError();
    if (code == ERROR_WINHTTP_TIMEOUT) {
      reply[0] = 0;
      send(kRespNetRecv, reply, 1);               // данных пока нет
      return;
    }
    char text[32];
    snprintf(text, sizeof(text), "recv:winhttp %u", static_cast<unsigned>(code));
    sendError(text);
    send(kRespNetRecv, reply, 1);
    return;
  }
  if (available == 0) {
    send(kRespNetRecv, reply, 1);                 // тело кончилось
    return;
  }
  DWORD got = 0;
  if (!WinHttpReadData(zifiNet->request, reply + 1, available < limit ? available : static_cast<DWORD>(limit),
                       &got) || got == 0) {
    char text[32];
    snprintf(text, sizeof(text), "recv:winhttp %u", static_cast<unsigned>(GetLastError()));
    sendError(text);
    send(kRespNetRecv, reply, 1);
    return;
  }
  reply[0] = 0;
  send(kRespNetRecv, reply, static_cast<uint16_t>(got + 1));
}

void handleNetClose() {
  send(kAck);
  if (zifiNet != nullptr) {
    netFree(zifiNet);
    zifiNet = nullptr;
  }
  const uint8_t done = 1;
  send(kRespNetClose, &done, 1);
}

// NET_NTP: ГГГГММДДЧЧММСС — как TimeApp прошивки: UTC (здесь — часы ПК) плюс пояс из time:
// принятого WIFI_INI. VDAC4_NTP=fail — отказ NTP: доклад #EE и строка из нулей.
void handleNetNtp() {
  send(kAck);
  const char* mode = getenv("VDAC4_NTP");
  if (mode != nullptr && _stricmp(mode, "fail") == 0) {
    sendError("ntp:timeout");
    send(kRespNetNtp, reinterpret_cast<const uint8_t*>("00000000000000"), 14);
    return;
  }
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  ULARGE_INTEGER ticks;
  ticks.LowPart = now.dwLowDateTime;
  ticks.HighPart = now.dwHighDateTime;
  ticks.QuadPart += static_cast<LONGLONG>(hostIni.timezoneHours()) * 3600LL * 10000000LL;
  now.dwLowDateTime = ticks.LowPart;
  now.dwHighDateTime = ticks.HighPart;
  SYSTEMTIME t;
  FileTimeToSystemTime(&now, &t);
  char text[16];
  snprintf(text, sizeof(text), "%04u%02u%02u%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour,
           t.wMinute, t.wSecond);
  send(kRespNetNtp, reinterpret_cast<const uint8_t*>(text), 14);
}

// Версия «установленной» прошивки: VDAC4_FW_VERSION или vdac4-unreal; модель
// онлайн-обновления меняет её на поставленную.
std::mutex firmwareVersionLock;
std::string& installedVersionSlot();
std::string firmwareVersion() {
  std::lock_guard<std::mutex> lock(firmwareVersionLock);
  return installedVersionSlot();
}

void handleSysInfo() {
  send(kAck);
  const std::string info = "RAM:host Flash:host FW:" + firmwareVersion();
  send(kRespSysInfo, reinterpret_cast<const uint8_t*>(info.data()), static_cast<uint16_t>(info.size()));
}

// Прокси выключен: [0].
void handleProxyStatus() {
  send(kAck);
  const uint8_t off = 0;
  send(kRespNetProxyStatus, &off, 1);
}

// --- обновлятор WC: WCU_START, WCU_APPLY, WCU_STOP, WCU_SYNC ---------------------------
// Как сетевые команды прошивки (WcUpdateApp::processNetwork): ядро 1 шлёт ACK и отдаёт
// команду сетевому ядру, а само продолжает обслуживать канал и мост VFS; ответ —
// когда сетевое ядро закончит, перед ним доклад #EE, если не удалось. Здесь сетевое
// ядро — поток на команду; вторая команда, пока первая не кончилась, — «network busy».
constexpr uint32_t kWcuStopTimeoutMs = 20000;     // WcUpdateApp::kStopTimeoutMs
std::atomic<bool> networkBusy{false};

// HTTPS GET для обновлятора: тело целиком, любой код ответа (WcUpdater разбирает его сам).
bool wcuHttpGet(const char* host, const char* path, uint8_t* buffer, size_t capacity,
                size_t& length, uint16_t& status, char* error, size_t errorSize,
                const volatile bool* cancel) {
  length = 0;
  status = 0;
  wifiPoll();
  if (!wifi.connected) {
    snprintf(error, errorSize, "no wifi");
    return false;
  }
  char url[1024];
  snprintf(url, sizeof(url), "https://%s%s", host, path);
  int code = 0;
  HostNet* net = netOpen(url, "Accept-Encoding: identity", 60000, &code, true);
  if (net == nullptr) {
    snprintf(error, errorSize, "no connection");
    return false;
  }
  status = static_cast<uint16_t>(code);
  for (;;) {
    if (cancel != nullptr && *cancel) {
      netFree(net);
      snprintf(error, errorSize, "stopped");
      return false;
    }
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(net->request, &available)) {
      netFree(net);
      snprintf(error, errorSize, "http read");
      return false;
    }
    if (available == 0) {
      break;                           // конец тела
    }
    if (length + available > capacity) {
      netFree(net);
      snprintf(error, errorSize, "body too long");
      return false;
    }
    DWORD read = 0;
    if (!WinHttpReadData(net->request, buffer + length, available, &read) || read == 0) {
      netFree(net);
      snprintf(error, errorSize, "http read");
      return false;
    }
    length += read;
  }
  netFree(net);
  return true;
}

void wcuLog(const char* text) { hostLog("zifi-wcu", "%s", text); }

void processWcu(uint8_t command, std::vector<uint8_t> payload) {
  uint8_t response = kRespWcuSync;
  uint8_t result = 0;
  char error[49] = {};
  const uint16_t length = static_cast<uint16_t>(payload.size());
  switch (command) {
    case kWcuStart: {
      response = kRespWcuStart;
      if (!wcuhost::stop(kWcuStopTimeoutMs)) {
        snprintf(error, sizeof(error), "wcu:previous stopping");
        break;
      }
      wifiPoll();
      if (!wifi.connected) {
        snprintf(error, sizeof(error), "wcu:no wifi");
        break;
      }
      char reason[64] = {};
      if (!wcuhost::start(payload.data(), length, reason, sizeof(reason))) {
        snprintf(error, sizeof(error), "wcu:%s", reason);
        break;
      }
      result = 1;
      break;
    }
    case kWcuApply:
      response = kRespWcuApply;
      result = wcuhost::apply(payload.data(), length) ? 1 : 0;
      break;
    case kWcuStop:
      response = kRespWcuStop;
      result = wcuhost::stop(kWcuStopTimeoutMs) ? 1 : 0;
      break;
    default:
      result = wcuhost::sync() ? 1 : 0;
      break;
  }
  if (error[0] != 0) {
    hostLog("zifi-wcu", "%02X: %s", command, error);
    sendError(error);
  }
  send(response, &result, 1);
  networkBusy = false;
}

void handleWcu(const PacketView& packet) {
  send(kAck);
  if (networkBusy.exchange(true)) {
    sendError("network busy");
    uint8_t response = kRespWcuSync;
    if (packet.command == kWcuStart) response = kRespWcuStart;
    if (packet.command == kWcuApply) response = kRespWcuApply;
    if (packet.command == kWcuStop) response = kRespWcuStop;
    const uint8_t failed = 0;
    send(response, &failed, 1);
    return;
  }
  std::thread(processWcu, packet.command,
              std::vector<uint8_t>(packet.data, packet.data + packet.length)).detach();
}

// --- обновление прошивки с GitHub: ONLINE_UPDATE_CHECK, ONLINE_UPDATE ---------------
// Как Application::processOnlineUpdateCheck/processOnlineUpdate и OnlineUpdater
// прошивки: ACK, работа на «сетевом ядре» (здесь — поток), события
// ONLINE_UPDATE_PROGRESS [этап][проценты] (1 — манифест, 2 — образ, 3 — проверка),
// отказ — доклад #EE «update-check:…» / «update:…» и ответ [0]. Ответ CHECK —
// [1][отношение][версия], ONLINE_UPDATE — [1][SHA-256 образа]. Образ проверяется
// целиком, но не пишется никуда; затем — модель перезапуска (как SYS_RESET).
constexpr char kGithubRawHost[] = "raw.githubusercontent.com";

std::string& installedVersionSlot() {
  static std::string version = getenv("VDAC4_FW_VERSION") ? getenv("VDAC4_FW_VERSION") : "vdac4-unreal";
  return version;
}

void onlineProgress(uint8_t stage, uint8_t percent) {
  const uint8_t data[2] = {stage, percent};
  send(kEventOnlineUpdateProgress, data, 2);
}

// Манифест: строка «VERSION версия» и строка sha256sum для firmware.bin (как
// parseManifest прошивки: повтор строки — отказ).
bool parseOnlineManifest(const std::string& text, std::string& version, uint8_t digest[32]) {
  bool haveVersion = false;
  bool haveDigest = false;
  size_t line = 0;
  while (line < text.size()) {
    size_t end = text.find_first_of("\r\n", line);
    if (end == std::string::npos) end = text.size();
    size_t at = line;
    while (at < end && (text[at] == ' ' || text[at] == '\t')) ++at;
    if (end - at > 7 && text.compare(at, 7, "VERSION") == 0 && (text[at + 7] == ' ' || text[at + 7] == '\t')) {
      if (haveVersion) return false;
      at += 7;
      while (at < end && (text[at] == ' ' || text[at] == '\t')) ++at;
      size_t stop = end;
      while (stop > at && (text[stop - 1] == ' ' || text[stop - 1] == '\t')) --stop;
      version = text.substr(at, stop - at);
      if (version.empty() || version.size() >= 32) return false;
      for (char c : version) {
        if (!isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != '+' && c != '-') return false;
      }
      haveVersion = true;
    } else if (end - at >= 64) {
      uint8_t parsed[32];
      bool valid = true;
      for (int i = 0; i < 32 && valid; ++i) {
        unsigned value = 0;
        valid = sscanf(text.substr(at + i * 2, 2).c_str(), "%2x", &value) == 1 &&
                isxdigit(static_cast<unsigned char>(text[at + i * 2])) &&
                isxdigit(static_cast<unsigned char>(text[at + i * 2 + 1]));
        parsed[i] = static_cast<uint8_t>(value);
      }
      at += 64;
      if (valid && at < end && (text[at] == ' ' || text[at] == '\t')) {
        while (at < end && (text[at] == ' ' || text[at] == '\t')) ++at;
        if (at < end && text[at] == '*') ++at;
        size_t stop = at;
        while (stop < end && text[stop] != ' ' && text[stop] != '\t') ++stop;
        if (text.compare(at, stop - at, "firmware.bin") == 0 && stop - at == 12) {
          if (haveDigest) return false;
          memcpy(digest, parsed, 32);
          haveDigest = true;
        }
      }
    }
    line = end;
    while (line < text.size() && (text[line] == '\r' || text[line] == '\n')) ++line;
  }
  return haveVersion && haveDigest;
}

// Формат номера версии VDAC4 — как board::kReleaseVersionFormat (namespace board
// есть только при ZIFI_BOARD_VDAC4, а эта программа собирается без него).
constexpr char kHostReleaseVersionFormat[] = "vdac4-%u.%u.%u%n";

// Отношение опубликованной версии к установленной — как comparePublishedVersion.
uint8_t compareOnlineVersion(const std::string& installed, const std::string& published) {
  if (installed == published) return kOnlineUpdateSame;
  unsigned a[3] = {}, b[3] = {};
  int used = 0;
  if (sscanf(installed.c_str(), kHostReleaseVersionFormat, &a[0], &a[1], &a[2], &used) != 3 ||
      installed[used] != 0) {
    return kOnlineUpdateDifferent;
  }
  used = 0;
  if (sscanf(published.c_str(), kHostReleaseVersionFormat, &b[0], &b[1], &b[2], &used) != 3 ||
      published[used] != 0) {
    return kOnlineUpdateDifferent;
  }
  for (int i = 0; i < 3; ++i) {
    if (a[i] != b[i]) return b[i] > a[i] ? kOnlineUpdateNewer : kOnlineUpdateOlder;
  }
  return kOnlineUpdateDifferent;
}

bool onlineLoadManifest(std::string& version, uint8_t digest[32], char* error, size_t size) {
  onlineProgress(1, 0);
  static uint8_t buffer[4096];
  size_t length = 0;
  uint16_t status = 0;
  if (!wcuHttpGet(kGithubRawHost, ZIFI_UPDATE_DIRECTORY "firmware.sha256", buffer, sizeof(buffer) - 1,
                  length, status, error, size, nullptr)) {
    return false;
  }
  if (status < 200 || status >= 300) {
    snprintf(error, size, "manifest http %u", static_cast<unsigned>(status));
    return false;
  }
  if (!parseOnlineManifest(std::string(reinterpret_cast<char*>(buffer), length), version, digest)) {
    snprintf(error, size, "manifest version or sha");
    return false;
  }
  onlineProgress(1, 100);
  return true;
}

// firmware.bin потоком: заголовок — проверка образа ESP32-S3 этой платы, всё — в SHA-256.
bool onlineDownload(const uint8_t expected[32], uint8_t digest[32], char* error, size_t size) {
  onlineProgress(2, 0);
  char url[512];
  snprintf(url, sizeof(url), "https://%s%sfirmware.bin", kGithubRawHost, ZIFI_UPDATE_DIRECTORY);
  int code = 0;
  HostNet* net = netOpen(url, "Accept-Encoding: identity", 60000, &code, true);
  if (net == nullptr) {
    snprintf(error, size, "no connection");
    return false;
  }
  if (code < 200 || code >= 300) {
    netFree(net);
    snprintf(error, size, "firmware http %d", code);
    return false;
  }
  const char* lengthText = coreNetHeader(net, "Content-Length");
  const unsigned long total = lengthText != nullptr ? strtoul(lengthText, nullptr, 10) : 0;
  if (total < 112 || total > 0x300000) {
    netFree(net);
    snprintf(error, size, "firmware does not fit");
    return false;
  }
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
      BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
    if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    netFree(net);
    snprintf(error, size, "sha256 init");
    return false;
  }
  std::vector<uint8_t> header;
  std::vector<uint8_t> chunk(16 * 1024);
  unsigned long received = 0;
  int shown = 0;
  bool ok = true;
  while (received < total) {
    const int got = coreNetRead(net, chunk.data(), static_cast<uint32_t>(chunk.size()), 30000);
    if (got <= 0) {
      snprintf(error, size, "firmware short body");
      ok = false;
      break;
    }
    if (header.size() < 112) {
      header.insert(header.end(), chunk.begin(), chunk.begin() + (got < 112 ? got : 112));
      if (header.size() >= 112 && !zifi::isEsp32S3ApplicationImage(header.data(), header.size())) {
        snprintf(error, size, "not ESP32-S3 application");
        ok = false;
        break;
      }
    }
    BCryptHashData(hash, chunk.data(), static_cast<ULONG>(got), 0);
    received += static_cast<unsigned long>(got);
    const int percent = static_cast<int>(received * 100ull / total);
    if (percent != shown) {
      shown = percent;
      onlineProgress(2, static_cast<uint8_t>(percent));
    }
  }
  netFree(net);
  if (ok) {
    onlineProgress(3, 0);
    BCryptFinishHash(hash, digest, 32, 0);
    if (memcmp(digest, expected, 32) != 0) {
      snprintf(error, size, "sha256 mismatch");
      ok = false;
    } else {
      onlineProgress(3, 100);
    }
  }
  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return ok;
}

void processOnlineUpdate(uint8_t command) {
  const bool check = command == kOnlineUpdateCheck;
  const uint8_t response = check ? kRespOnlineUpdateCheck : kRespOnlineUpdate;
  const char* prefix = check ? "update-check" : "update";
  char error[64] = {};
  std::string version;
  uint8_t expected[32] = {};
  uint8_t digest[32] = {};
  wifiPoll();
  bool ok = false;
  if (!wifi.connected) {
    snprintf(error, sizeof(error), "no wifi");
  } else if (onlineLoadManifest(version, expected, error, sizeof(error))) {
    ok = check || onlineDownload(expected, digest, error, sizeof(error));
  }
  if (!ok) {
    char text[80];
    snprintf(text, sizeof(text), "%s:%s", prefix, error);
    hostLog("zifi-ota", "%s", text);
    sendError(text);
    const uint8_t failed = 0;
    send(response, &failed, 1);
    networkBusy = false;
    return;
  }
  if (check) {
    std::vector<uint8_t> reply = {1, compareOnlineVersion(firmwareVersion(), version)};
    reply.insert(reply.end(), version.begin(), version.end());
    hostLog("zifi-ota", "check: installed %s, published %s, relation %u", firmwareVersion().c_str(),
            version.c_str(), reply[1]);
    send(response, reply.data(), static_cast<uint16_t>(reply.size()));
    networkBusy = false;
    return;
  }
  uint8_t reply[33] = {1};
  memcpy(reply + 1, digest, 32);
  send(response, reply, sizeof(reply));
  hostLog("zifi-ota", "update: firmware.bin %s verified (SHA-256 ok), nothing written; restart modelled",
          version.c_str());
  // Перезапуск: ответ ушёл, прошивка перезагружается в новый раздел. Модель — как
  // SYS_RESET (Wi-Fi с известной сетью заново) и новая версия в SYS_INFO.
  Sleep(1500);
  {
    std::lock_guard<std::mutex> lock(firmwareVersionLock);
    installedVersionSlot() = version;
  }
  const bool knownNetwork = !wifi.activeSsid.empty() || wifi.bootOnline;
  wifi.connected = false;
  wifi.readyAt = knownNetwork ? uptimeMs() + wifi.connectMs : 0;
  wifi.beganMs = knownNetwork ? (uptimeMs() | 1) : 0;
  networkBusy = false;
}

void handleOnlineUpdate(const PacketView& packet) {
  send(kAck);
  if (networkBusy.exchange(true)) {
    sendError("network busy");
    const uint8_t failed = 0;
    send(packet.command == kOnlineUpdateCheck ? kRespOnlineUpdateCheck : kRespOnlineUpdate, &failed, 1);
    return;
  }
  std::thread(processOnlineUpdate, packet.command).detach();
}

// SYS_CLOCK: [год LE16][месяц][число][часы][минуты][секунды] — местное время с RTC Evo.
// Проверка — как Application::handleSysClock прошивки (дата позже 1.10.2026); часы ПК
// не трогаются, итог — в журнал.
void handleSysClock(const PacketView& packet) {
  uint8_t ok = 0;
  if (packet.length >= 7) {
    const uint8_t* d = packet.data;
    const unsigned year = d[0] | d[1] << 8;
    const uint32_t date = year * 10000u + d[2] * 100u + d[3];
    ok = year <= 2099 && d[2] >= 1 && d[2] <= 12 && d[3] >= 1 && d[3] <= 31 && d[4] <= 23 &&
                 d[5] <= 59 && d[6] <= 59 && date > 20261001u
             ? 1
             : 0;
    hostLog("zifi-clock", "clock from Evo RTC %04u-%02u-%02u %02u:%02u:%02u: %s", year, d[2],
            d[3], d[4], d[5], d[6], ok ? "set (modelled)" : "REFUSED");
  }
  send(kRespSysClock, &ok, 1);
}

// SYS_RESET: прошивка шлёт ACK и перезапускается. Здесь — то, что от перезапуска
// видно Z80: сеанс обновлятора и приложение кончаются, Wi-Fi начинает подключение
// заново (сеть из прошлого zifi.ini прошивка помнит).
void handleReset() {
  send(kAck);
  wcuhost::requestStop();
  stopApp();
  const bool knownNetwork = !wifi.activeSsid.empty() || wifi.bootOnline;
  wifi.connected = false;
  wifi.readyAt = knownNetwork ? uptimeMs() + wifi.connectMs : 0;
  wifi.beganMs = knownNetwork ? (uptimeMs() | 1) : 0;
  hostLog("zifi-host", "SYS_RESET: ESP restart modelled, Wi-Fi %s",
          knownNetwork ? "reconnecting" : "off");
}

void handlePacketBody(const PacketView& packet);

void handlePacket(const PacketView& packet) {
  countCommand(packet.command);
  commandSince = uptimeMs();
  currentCommand = packet.command;
  handlePacketBody(packet);
  currentCommand = -1;
}

void handlePacketBody(const PacketView& packet) {
  switch (packet.command) {
    case kEcho:
      send(kEcho, packet.data, packet.length);
      break;
    case kPing:
      send(kReady);
      break;
    case kGetStep: {
      const uint8_t step = 0;
      send(kRespGetStep, &step, 1);
      break;
    }
    case kWifiIni:
      handleWifiIni(packet);
      break;
    case kWeatherGet:
      handleWeatherGet();
      break;
    case kNetHttpGet:
      handleHttpGet(packet);
      break;
    case kNetRecv:
      handleNetRecv(packet);
      break;
    case kNetClose:
      handleNetClose();
      break;
    case kNetNtp:
      handleNetNtp();
      break;
    case kSysInfo:
      handleSysInfo();
      break;
    case kNetProxyStatus:
      handleProxyStatus();
      break;
    case kWifiConnect:
    case kFtpStart:
    case kFtpStop:
    case kFtpRamStats:
    case kSmbStart:
    case kSmbStop:
    case kUpdateStart:
    case kUpdateStop:
    case kNetOpen:
    case kNetSend:
    case kNetPing:
    case kNetIpConfig:
    case kSysReset:
      handleReset();
      break;
    case kOnlineUpdateCheck:
    case kOnlineUpdate:
      handleOnlineUpdate(packet);
      break;
    case kSysClock:
      handleSysClock(packet);
      break;
    case kWcuStart:
    case kWcuApply:
    case kWcuStop:
    case kWcuSync:
      handleWcu(packet);
      break;
    case kLinkProbe: {
      // Проверка DMA ведомого (наладка): в эмуляторе драйвера DMA нет — отказ,
      // как у SpiLink::dmaProbe при занятом канале.
      uint8_t reply[8] = {0};
      reply[1] = packet.length >= 1 ? packet.data[0] : 0;
      send(kRespLinkProbe, reply, sizeof(reply));
      break;
    }
    default:
      if (!moduleCommand(packet)) {
        if (packet.command >= kAudioOpen && packet.command <= kAudioPicture) {
          hostLog("zifi-host", "command %02X: built-in player is not emulated", packet.command);
        }
        char text[24];
        snprintf(text, sizeof(text), "unsupported:%02X", packet.command);
        sendError(text);
      }
      break;
  }
}

}  // namespace

// --- JPEG обложки: WIC вместо TJpgDec ПЗУ и jpgd ---------------------------------------

namespace zifi {

// Прошивка (src/cover_jpeg.cpp) baseline YCbCr сводит TJpgDec из ПЗУ, а остальное —
// progressive, оттенки серого — программным jpgd, то есть берёт любой JPEG. Здесь
// и то и другое — WIC. yield — пауза для задачи звука на ESP; на ПК не нужна.
int decodeCoverJpeg(const uint8_t* jpeg, size_t size, CoverThumb& thumb, CoverDecodeInfo& info,
                    const volatile bool* cancel, void (*)()) {
  enum { JDR_INP = 2, JDR_FMT1 = 6, JDR_INTR = 1 };
  if (size < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
    return 100 + JDR_FMT1;
  }
  hostLog("zifi-host", "cover JPEG: WIC decode of %u bytes", static_cast<unsigned>(size));
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IWICImagingFactory* factory = nullptr;
  IWICStream* stream = nullptr;
  IWICBitmapDecoder* decoder = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICFormatConverter* converter = nullptr;
  int result = 100 + JDR_INP;
  UINT width = 0, height = 0;
  std::vector<uint8_t> rgb;
  if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&factory))) &&
      SUCCEEDED(factory->CreateStream(&stream)) &&
      SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(jpeg), static_cast<DWORD>(size))) &&
      SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand,
                                                 &decoder)) &&
      SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&width, &height)) &&
      SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
      SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat24bppRGB, WICBitmapDitherTypeNone,
                                      nullptr, 0, WICBitmapPaletteTypeCustom))) {
    rgb.resize(static_cast<size_t>(width) * height * 3);
    if (SUCCEEDED(converter->CopyPixels(nullptr, width * 3, static_cast<UINT>(rgb.size()),
                                        rgb.data()))) {
      result = 0;
    }
  }
  if (converter) converter->Release();
  if (frame) frame->Release();
  if (decoder) decoder->Release();
  if (stream) stream->Release();
  if (factory) factory->Release();
  hostLog("zifi-host", "cover JPEG: WIC result %d, %ux%u", result, width, height);
  if (result != 0) {
    return result;
  }
  info.width = width;
  info.height = height;
  // Масштаб — как cover_jpeg.cpp: наибольший, при котором меньшая сторона ≥ 64.
  const uint32_t shorter = width < height ? width : height;
  uint8_t scale = 0;
  while (scale < 3 && (shorter >> (scale + 1)) >= static_cast<uint32_t>(CoverThumb::kSide)) {
    ++scale;
  }
  info.scale = scale;
  const uint32_t step = 1u << scale;
  const uint32_t round = step - 1;
  const uint32_t sw = (width + round) >> scale;
  const uint32_t sh = (height + round) >> scale;
  if (!thumb.begin(sw, sh)) {
    return 2;
  }
  // Уменьшение 1/2..1/8 — среднее по квадрату, как DC-путь TJpgDec.
  std::vector<uint8_t> row(static_cast<size_t>(sw) * 3);
  for (uint32_t y = 0; y < sh; ++y) {
    if (cancel != nullptr && *cancel) {
      return 200 + JDR_INTR;
    }
    for (uint32_t x = 0; x < sw; ++x) {
      uint32_t sum[3] = {0, 0, 0}, count = 0;
      for (uint32_t dy = 0; dy < step && y * step + dy < height; ++dy) {
        for (uint32_t dx = 0; dx < step && x * step + dx < width; ++dx) {
          const uint8_t* p = rgb.data() + ((static_cast<size_t>(y * step + dy) * width) +
                                           (x * step + dx)) * 3;
          sum[0] += p[0];
          sum[1] += p[1];
          sum[2] += p[2];
          ++count;
        }
      }
      for (int c = 0; c < 3; ++c) {
        row[x * 3 + c] = static_cast<uint8_t>(count ? sum[c] / count : 0);
      }
    }
    thumb.addBlock(0, y, sw - 1, y, row.data());
  }
  return 0;
}

}  // namespace zifi

// --- main -------------------------------------------------------------------------------

int main(int argc, char** argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: vdac4_esp_host <pipe in> <pipe out> <log> <cache dir>\n");
    return 1;
  }
  logFile = fopen(argv[3], "a");
  hostLog("zifi-host", "VDAC4 ESP program for Unreal: firmware command handling, PLAYER.ELF PC build");
  // Модель Wi-Fi для заставок погоды: VDAC4_WIFI=on — ESP уже в сети,
  // VDAC4_WIFI_MS — сколько длится подключение, VDAC4_WIFI_OLD=1 — прошивка до 0.1.33.
  const char* online = getenv("VDAC4_WIFI");
  wifi.bootOnline = wifi.connected = online != nullptr && _stricmp(online, "on") == 0;
  const char* oldFirmware = getenv("VDAC4_WIFI_OLD");
  wifi.oldFirmware = oldFirmware != nullptr && strcmp(oldFirmware, "1") == 0;
  const char* connectMs = getenv("VDAC4_WIFI_MS");
  if (connectMs != nullptr) {
    wifi.connectMs = static_cast<uint32_t>(strtoul(connectMs, nullptr, 10));
  }
  hostLog("zifi-host", "Wi-Fi model: %s at start, connecting takes %u ms, firmware %s",
          wifi.connected ? "online" : "off", static_cast<unsigned>(wifi.connectMs),
          wifi.oldFirmware ? "before 0.1.33" : "0.1.33");
  const char* commitMs = getenv("VDAC4_COMMIT_MS");
  if (commitMs != nullptr) {
    commitDelayMs = static_cast<uint32_t>(strtoul(commitMs, nullptr, 10));
    hostLog("zifi-host", "module flash write model: MOD_COMMIT takes %u ms",
            static_cast<unsigned>(commitDelayMs));
  }
  fromUnreal = CreateFileA(argv[1], GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  toUnreal = CreateFileA(argv[2], GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (fromUnreal == INVALID_HANDLE_VALUE || toUnreal == INVALID_HANDLE_VALUE) {
    hostLog("zifi-host", "cannot open the pipes (%lu)", GetLastError());
    return 2;
  }
  storage = new DiskStorage(argv[4]);
  cache = new ZmCache(*storage);
  cache->load();
  hostLog("zifi-zm", "module cache %s: %u files, %u KB free", argv[4],
          static_cast<unsigned>(cache->count()), static_cast<unsigned>(cache->freeBytes() / 1024));

  core.size = sizeof(core);
  core.abi_major = ZM_APP_ABI_MAJOR;
  core.abi_minor = ZM_APP_ABI_MINOR;
  core.firmware = "vdac4-unreal";
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
  core.net_open = coreNetOpen;
  core.net_header = coreNetHeader;
  core.net_read = coreNetRead;
  core.net_close = coreNetClose;

  // Сторож: команда, которая обрабатывается дольше 2 с, — в журнал.
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      const int command = currentCommand;
      const uint32_t since = uptimeMs() - commandSince;
      if (command >= 0 && since > 2000) {
        hostLog("zifi-host", "command %02X is still running after %u ms", command,
                static_cast<unsigned>(since));
      }
    }
  }).detach();

  wcuhost::Callbacks wcu = {};
  wcu.writeToZ80 = sendRaw;
  wcu.readFromZ80 = readForBridge;
  wcu.sendEvent = [](uint8_t command, const uint8_t* data, uint16_t length) {
    return send(command, data, length);
  };
  wcu.httpGet = wcuHttpGet;
  wcu.log = wcuLog;
  if (!wcuhost::begin(wcu)) {
    hostLog("zifi-wcu", "VFS bridge did not start");
  }

  std::thread(receiveLoop).detach();
  // Ядро 1: заявка сеанса обновлятора к карте, затем очередной кадр Z80.
  FrameParser parser;
  std::vector<uint8_t> frame;
  while (!channelDone()) {
    wcuhost::pollCore1();
    if (!takeFrame(frame, wcuhost::running() ? 1 : 20)) {
      continue;
    }
    for (uint8_t value : frame) {
      PacketView packet;
      if (parser.feed(value, uptimeMs(), packet)) {
        handlePacket(packet);
      }
    }
  }
  hostLog("zifi-host", "Unreal closed the channel");
  wcuhost::requestStop();
  stopApp();
  return 0;
}
