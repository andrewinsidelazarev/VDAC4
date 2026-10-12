#include "zifi/apps/audio_app.hpp"

#if ZIFI_HAS_AUDIO

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <string.h>

#include <new>

#include "zifi/board_state.hpp"
#include "zifi/bringup_log.hpp"
#include "zifi/z80_transport.hpp"

namespace zifi {
namespace {

// Сигнал нужен после любого перезапуска, кроме программного: плагины FTP и SMB
// перезапускают ESP при каждом старте, и там он мешал бы. Причину не
// перебираем: при включении от блока питания Evo чип может стартовать и через
// сброс по просадке питания или сторожевому таймеру загрузчика, а после сбоя
// прошивки сигнал заодно даёт о нём знать.
bool bootChimeWanted() {
  const esp_reset_reason_t reason = esp_reset_reason();
  return reason != ESP_RST_SW && reason != ESP_RST_DEEPSLEEP;
}

}  // namespace

void* AudioApp::coverAlloc(size_t size) {
  return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void AudioApp::coverFree(void* pointer) { heap_caps_free(pointer); }

void AudioApp::begin() {
  // Обложка только с PSRAM: картинка бывает в мегабайты.
  if (host_.psram()) {
    coverWake_ = xSemaphoreCreateBinary();
    coverLock_ = xSemaphoreCreateMutex();
    thumb_ = static_cast<uint8_t*>(coverAlloc(CoverThumb::kBytes));
    thumbReply_ = static_cast<uint8_t*>(coverAlloc(2 + kThumbPart));
    void* parser = coverAlloc(sizeof(CoverParser));
    if (parser != nullptr) {
      cover_ = new (parser) CoverParser(coverAlloc, coverFree);
    }
    if (coverWake_ != nullptr && coverLock_ != nullptr && thumb_ != nullptr &&
        thumbReply_ != nullptr && cover_ != nullptr &&
        xTaskCreatePinnedToCore(coverTaskEntry, "zifi-cover", 6144, this, 1, &coverTask_,
                                0) != pdPASS) {
      coverTask_ = nullptr;
    }
  }
  // Отказ звука работе сети не мешает: состояние видно в SYS_INFO (AUDIO:off).
  if (probeBoardState() == BoardState::kUnpowered) {
    // Модуль стоит в плате и питается от USB, плата обесточена. Выходы I2S
    // питали бы ЦАП через его входы. Питание может появиться позже: сетевая
    // задача проверяет это раз в секунду и запускает звук.
    bringupLog("zifi-audio", "I2S is not started: the board has no power");
    waitsForPower_ = true;
  } else {
    player_.begin(host_.psram(), bootChimeWanted());
  }
}

void AudioApp::pollNetworkCore(uint32_t now) {
  if (!waitsForPower_ || static_cast<uint32_t>(now - lastPowerCheckMs_) < 1000) {
    return;
  }
  lastPowerCheckMs_ = now;
  if (probeBoardState() != BoardState::kUnpowered) {
    waitsForPower_ = false;
    bringupLog("zifi-audio", "the board got power: starting I2S");
    player_.begin(host_.psram(), false);
  }
}

bool AudioApp::handleCommand(const PacketView& packet) {
  Z80Transport& transport = host_.transport();
  switch (packet.command) {
    case kAudioOpen: {
      // Без тела — поток MP3, как до 0.1.20. [формат][длина LE32]: с битом
      // kFormatModule — трекерный модуль этой длины.
      resetCover();
      bool opened = false;
      if (packet.length >= 5 &&
          (packet.data[0] & AudioPlayer::kFormatModule) != 0) {
        opened = player_.openModule(readLe32(packet.data + 1));
      } else {
        opened = player_.open();
      }
      reply_[0] = opened ? 1 : 0;
      writeLe32(reply_ + 1, static_cast<uint32_t>(player_.freeSpace()));
      transport.send(kRespAudioOpen, reply_, 5);
      return true;
    }
    case kAudioData:
      reply_[0] = player_.feed(packet.data, packet.length);
      writeLe32(reply_ + 1, static_cast<uint32_t>(player_.freeSpace()));
      transport.send(kRespAudioData, reply_, 5);
      return true;
    case kAudioClose:
      player_.close(packet.length != 0 && packet.data[0] != 0);
      reply_[0] = 1;
      transport.send(kRespAudioClose, reply_, 1);
      return true;
    case kAudioStatus: {
      const size_t length = player_.status(reply_, sizeof(reply_));
      transport.send(kRespAudioStatus, reply_, static_cast<uint16_t>(length));
      return true;
    }
    case kAudioTone: {
      // [каналы][частота LE16][длительность, мс LE16][уровень 0..255]
      bool started = false;
      if (packet.length >= 6) {
        started = player_.tone(packet.data[0], readLe16(packet.data + 1),
                               readLe16(packet.data + 3), packet.data[5]);
      }
      reply_[0] = started ? 1 : 0;
      transport.send(kRespAudioTone, reply_, 1);
      return true;
    }
    case kAudioVolume:
      if (packet.length != 0) {
        player_.setVolume(packet.data[0]);
      }
      reply_[0] = packet.length != 0 ? 1 : 0;
      transport.send(kRespAudioVolume, reply_, 1);
      return true;
    case kAudioPause:
      player_.pause(packet.length != 0 && packet.data[0] != 0);
      reply_[0] = 1;
      transport.send(kRespAudioPause, reply_, 1);
      return true;
    case kAudioProbe:
      memset(reply_, 0, 1 + AudioPlayer::kProbeBytes);
      reply_[0] = player_.probeLines(reply_ + 1, AudioPlayer::kProbeBytes) ? 1 : 0;
      transport.send(kRespAudioProbe, reply_, 1 + AudioPlayer::kProbeBytes);
      return true;
    case kAudioSeek: {
      uint16_t position = 0;
      const bool moved = packet.length >= 1 &&
                         player_.seek(static_cast<int8_t>(packet.data[0]), position);
      reply_[0] = moved ? 1 : 0;
      writeLe16(reply_ + 1, position);
      transport.send(kRespAudioSeek, reply_, 3);
      return true;
    }
    case kAudioFlush:
      reply_[0] =
          packet.length >= 4 && player_.flush(readLe32(packet.data)) ? 1 : 0;
      writeLe32(reply_ + 1, static_cast<uint32_t>(player_.freeSpace()));
      transport.send(kRespAudioFlush, reply_, 5);
      return true;
    case kAudioInfo: {
      const size_t length = player_.info(reply_, sizeof(reply_));
      transport.send(kRespAudioInfo, reply_, static_cast<uint16_t>(length));
      return true;
    }
    case kAudioCover: {
      // [смещение LE32][данные] → [итог][откуда слать дальше LE32][свободно LE32].
      CoverParser::Result result = CoverParser::kNone;
      if (coverTask_ != nullptr && packet.length >= 4) {
        const bool wanted = cover_->result() == CoverParser::kWant;
        result = cover_->feed(readLe32(packet.data), packet.data + 4, packet.length - 4);
        if (wanted && result != CoverParser::kWant) {
          coverFinished(result);
        }
      }
      reply_[0] = result;
      writeLe32(reply_ + 1, cover_ != nullptr ? cover_->want() : 0);
      writeLe32(reply_ + 5, static_cast<uint32_t>(player_.freeSpace()));
      transport.send(kRespAudioCover, reply_, 9);
      return true;
    }
    case kAudioThumb:
      replyThumb(packet.length != 0 ? packet.data[0] : 0);
      return true;
    default:
      return false;
  }
}

// Новый файл: прежняя обложка не нужна, сведение прежней картинки прерывается.
void AudioApp::resetCover() {
  if (coverTask_ == nullptr) {
    return;
  }
  xSemaphoreTake(coverLock_, portMAX_DELAY);
  ++coverGeneration_;
  coverCancel_ = true;
  thumbState_.store(kThumbNone);
  if (coverImage_ != nullptr) {
    heap_caps_free(coverImage_);
    coverImage_ = nullptr;
  }
  xSemaphoreGive(coverLock_);
  cover_->reset();
}

void AudioApp::coverFinished(CoverParser::Result result) {
  if (result != CoverParser::kReceived) {
    bringupLog("zifi-audio", "cover: none (%s)", cover_->reason());
    return;
  }
  size_t size = 0;
  uint8_t* image = cover_->takeImage(size);
  if (image == nullptr) {
    return;
  }
  xSemaphoreTake(coverLock_, portMAX_DELAY);
  if (coverImage_ != nullptr) {
    heap_caps_free(coverImage_);
  }
  coverImage_ = image;
  coverImageSize_ = size;
  coverImageGeneration_ = coverGeneration_;
  coverCancel_ = false;
  thumbState_.store(kThumbPending);
  xSemaphoreGive(coverLock_);
  xSemaphoreGive(coverWake_);
}

void AudioApp::coverTaskEntry(void* context) {
  static_cast<AudioApp*>(context)->coverTask();
}

void AudioApp::coverTask() {
  for (;;) {
    xSemaphoreTake(coverWake_, portMAX_DELAY);
    xSemaphoreTake(coverLock_, portMAX_DELAY);
    uint8_t* image = coverImage_;
    const size_t size = coverImageSize_;
    const uint32_t generation = coverImageGeneration_;
    coverImage_ = nullptr;
    xSemaphoreGive(coverLock_);
    if (image == nullptr) {
      continue;
    }
    const int64_t started = esp_timer_get_time();
    CoverThumb thumb(coverAlloc, coverFree);
    CoverDecodeInfo info;
    uint8_t* work = static_cast<uint8_t*>(coverAlloc(CoverThumb::kBytes));
    int code = work != nullptr ? decodeCoverJpeg(image, size, thumb, info, &coverCancel_, [] { vTaskDelay(1); }) : 1;
    if (code == 0) {
      thumb.finish(work);
    }
    thumb.end();
    heap_caps_free(image);
    const unsigned elapsed = static_cast<unsigned>((esp_timer_get_time() - started) / 1000);
    xSemaphoreTake(coverLock_, portMAX_DELAY);
    const bool current = generation == coverGeneration_;
    if (current) {
      if (code == 0) {
        memcpy(thumb_, work, CoverThumb::kBytes);
      }
      thumbState_.store(code == 0 ? kThumbReady : kThumbNone);
    }
    xSemaphoreGive(coverLock_);
    if (work != nullptr) {
      heap_caps_free(work);
    }
    if (code == 0) {
      bringupLog("zifi-audio", "cover: JPEG %ux%u, %u KB, scale 1/%u, %u ms%s",
                 static_cast<unsigned>(info.width), static_cast<unsigned>(info.height),
                 static_cast<unsigned>(size >> 10), 1U << info.scale, elapsed,
                 current ? "" : " (file changed)");
    } else {
      // 1xx — отказ jd_prepare (108 — progressive или оттенки серого),
      // 2xx — отказ jd_decomp, 1 и 2 — нет памяти.
      bringupLog("zifi-audio", "cover: JPEG %ux%u, %u KB not decoded: code %d",
                 static_cast<unsigned>(info.width), static_cast<unsigned>(info.height),
                 static_cast<unsigned>(size >> 10), code);
    }
  }
}

// [часть] → [0 — сводится / 1 — вот / 2 — нет][часть][512 байт, если готова].
void AudioApp::replyThumb(uint8_t part) {
  part = static_cast<uint8_t>(part % kThumbParts);
  if (thumbReply_ == nullptr) {
    reply_[0] = kThumbNone;
    reply_[1] = part;
    host_.transport().send(kRespAudioThumb, reply_, 2);
    return;
  }
  const uint8_t state =
      coverTask_ != nullptr ? thumbState_.load() : static_cast<uint8_t>(kThumbNone);
  thumbReply_[0] = state;
  thumbReply_[1] = part;
  uint16_t length = 2;
  if (state == kThumbReady) {
    memcpy(thumbReply_ + 2, thumb_ + part * kThumbPart, kThumbPart);
    length += kThumbPart;
  }
  host_.transport().send(kRespAudioThumb, thumbReply_, length);
}

}  // namespace zifi

#endif  // ZIFI_HAS_AUDIO
