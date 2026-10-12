#pragma once

#include "zifi/app.hpp"
#include "zifi/board.hpp"

#if ZIFI_HAS_AUDIO

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>

#include "zifi/audio_player.hpp"
#include "zifi/cover_art.hpp"

namespace zifi {

// Звук платы VDAC4: команды AUDIO_* (Docs/VDAC4.md). Исполняются тут же, на
// ядре обмена: это запись в очередь декодера и чтение счётчиков, сети они не
// касаются. Обложку MP3 (AUDIO_COVER, AUDIO_THUMB) разбирает тоже ядро обмена,
// а JPEG в миниатюру сводит отдельная задача на сетевом ядре: на большой
// картинке это доли секунды и больше.
class AudioApp : public App {
 public:
  explicit AudioApp(AppHost& host) : host_(host) {}

  const char* name() const override { return "audio"; }
  void begin() override;
  bool handleCommand(const PacketView& packet) override;
  void pollNetworkCore(uint32_t now) override;
  bool blocksFlashWrites() const override { return player_.sounding(); }

  // Поле AUDIO: в SYS_INFO.
  void describe(char* output, size_t capacity) const {
    player_.describe(output, capacity);
  }
  const AudioPlayer& player() const { return player_; }
  AudioPlayer& player() { return player_; }

 private:
  // Самый длинный ответ — AUDIO_INFO: четыре байта, название до 32 знаков и ноль.
  static constexpr size_t kReplySize = 40;
  static_assert(kReplySize >= AudioPlayer::kStatusSize &&
                    kReplySize >= 1 + AudioPlayer::kProbeBytes,
                "audio reply buffer too small");

  enum ThumbState : uint8_t { kThumbPending = 0, kThumbReady = 1, kThumbNone = 2 };
  static constexpr size_t kThumbPart = 512;
  static constexpr uint8_t kThumbParts = CoverThumb::kBytes / kThumbPart;

  static void* coverAlloc(size_t size);
  static void coverFree(void* pointer);
  static void coverTaskEntry(void* context);
  void coverTask();
  void resetCover();
  void coverFinished(CoverParser::Result result);
  void replyThumb(uint8_t part);

  AppHost& host_;
  AudioPlayer player_;
  CoverParser* cover_ = nullptr;   // в PSRAM (begin): статическую память бережём
  SemaphoreHandle_t coverWake_ = nullptr;
  SemaphoreHandle_t coverLock_ = nullptr;
  TaskHandle_t coverTask_ = nullptr;
  // Под coverLock_: собранная картинка ждёт задачу; номер файла — чтобы
  // миниатюра прежнего файла не легла поверх нового.
  uint8_t* coverImage_ = nullptr;
  size_t coverImageSize_ = 0;
  uint32_t coverGeneration_ = 0;
  uint32_t coverImageGeneration_ = 0;
  volatile bool coverCancel_ = false;
  std::atomic<uint8_t> thumbState_{kThumbNone};
  // Буферы миниатюры — в куче (begin): объект приложения живёт в статической
  // памяти, а её бюджет (main.cpp) тесен.
  uint8_t* thumb_ = nullptr;           // готовая миниатюра, CoverThumb::kBytes
  uint8_t* thumbReply_ = nullptr;      // ответ AUDIO_THUMB, 2 + kThumbPart
  // Звук не запущен: при старте плата была без питания. Ждём его появления.
  bool waitsForPower_ = false;
  uint32_t lastPowerCheckMs_ = 0;
  uint8_t reply_[kReplySize] = {};
};

}  // namespace zifi

#endif  // ZIFI_HAS_AUDIO
