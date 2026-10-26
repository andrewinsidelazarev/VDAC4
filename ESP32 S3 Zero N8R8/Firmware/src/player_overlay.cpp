#include "zifi/player_overlay.hpp"

#include <stdio.h>
#include <string.h>

namespace zifi {

namespace {

void* viewAllocate(void* context, size_t bytes) {
  return static_cast<PlayerOverlay::Port*>(context)->allocate(bytes);
}

void viewRelease(void* context, void* pointer) {
  static_cast<PlayerOverlay::Port*>(context)->release(pointer);
}

// Строки снимка — с нулём в конце, числа — в своих пределах: снимок пишет приложение.
void terminate(char* text, size_t size) { text[size - 1] = 0; }

void sanitize(zm_ui_model_t& m) {
  terminate(m.title, sizeof(m.title));
  terminate(m.artist, sizeof(m.artist));
  terminate(m.album, sizeof(m.album));
  terminate(m.year, sizeof(m.year));
  terminate(m.file, sizeof(m.file));
  terminate(m.station, sizeof(m.station));
  terminate(m.format, sizeof(m.format));
  terminate(m.detail, sizeof(m.detail));
  terminate(m.status, sizeof(m.status));
  const unsigned limit = m.view == ZM_UI_VIEW_STATIONS ? ZM_UI_STATIONS : ZM_UI_LEVELS;
  if (m.count > limit) m.count = static_cast<uint8_t>(limit);
  if (m.view == ZM_UI_VIEW_STATIONS) {
    for (unsigned i = 0; i < m.count; ++i) terminate(m.stations[i], sizeof(m.stations[i]));
    if (m.current >= m.count) m.current = 0;
  }
  if (m.load_percent > 100) m.load_percent = 100;
  if (m.volume > 100) m.volume = 100;
}

}  // namespace

bool PlayerOverlay::ensureMemory() {
  const size_t canvasBytes = static_cast<size_t>(kStride) * 2U * kHeight * sizeof(uint16_t);
  const size_t coverBytes =
      static_cast<size_t>(player_view::kCoverSide) * player_view::kCoverSide * sizeof(uint16_t);
  if (canvas_ == nullptr) canvas_ = static_cast<uint16_t*>(port_.allocate(canvasBytes));
  if (canvas_ != nullptr && cover_ == nullptr) {
    cover_ = static_cast<uint16_t*>(port_.allocate(coverBytes));
  }
  if (canvas_ != nullptr && cover_ != nullptr && !viewReady_) {
    viewReady_ = player_view::begin(viewAllocate, viewRelease, &port_);
  }
  if (canvas_ == nullptr || cover_ == nullptr || !viewReady_) {
    ++stats_.noMemory;
    freeMemory();
    return false;
  }
  return true;
}

void PlayerOverlay::freeMemory() {
  if (viewReady_) player_view::end();
  viewReady_ = false;
  if (cover_ != nullptr) port_.release(cover_);
  if (canvas_ != nullptr) port_.release(canvas_);
  cover_ = nullptr;
  canvas_ = nullptr;
  coverSerial_ = 0;
  coverOk_ = false;
}

bool PlayerOverlay::fetchModel(uint32_t nowMs) {
  memset(&model_, 0, sizeof(model_));
  model_.size = sizeof(model_);
  model_.temperature = ZM_UI_TEMPERATURE_UNKNOWN;
  if (port_.model(&model_) != ZM_OK) return false;
  sanitize(model_);
  haveModel_ = true;
  lastModelMs_ = nowMs;
  return true;
}

void PlayerOverlay::fetchCover() {
  if (model_.cover_serial == coverSerial_) return;
  coverSerial_ = model_.cover_serial;
  coverOk_ = coverSerial_ != 0 && cover_ != nullptr &&
             port_.cover(cover_, player_view::kCoverSide) == ZM_OK;
  coverChanged_ = true;
}

bool PlayerOverlay::setAlpha(uint8_t alpha) {
  if (!port_.configure(true, alpha)) {
    ++stats_.writeFails;
    return false;
  }
  alpha_ = alpha;
  return true;
}

void PlayerOverlay::off() {
  port_.configure(false, 0);
  state_ = State::kOff;
  alpha_ = 0;
  freeMemory();
}

// Холст целиком — в Tang, затем оверлей с прозрачностью 0: до первой ступени появления
// на экране уже вся картинка, а не её начало.
bool PlayerOverlay::show(uint32_t nowMs) {
  if (!ensureMemory()) return false;
  port_.menuYield();
  coverSerial_ = 0;
  fetchCover();
  player_view::Rect rects[kMaxDirty];
  const int count = player_view::render(model_, coverOk_ ? cover_ : nullptr, true, true, nowMs,
                                        canvas_, rects, kMaxDirty);
  coverChanged_ = false;
  (void)count;
  const player_view::Rect whole = {0, 0, kWidth, kHeight};
  const uint64_t started = port_.micros();
  if (!upload(&whole, 1) || !setAlpha(0)) {
    off();
    return false;
  }
  stats_.fullUploadUs = static_cast<uint32_t>(port_.micros() - started);
  ++stats_.shows;
  state_ = State::kFadeIn;
  return true;
}

void PlayerOverlay::frame(uint32_t nowMs) {
  const uint64_t started = port_.micros();
  fetchCover();
  player_view::Rect rects[kMaxDirty];
  const int count = player_view::render(model_, coverOk_ ? cover_ : nullptr, coverChanged_, false,
                                        nowMs, canvas_, rects, kMaxDirty);
  coverChanged_ = false;
  const uint64_t rendered = port_.micros();
  stats_.words = 0;
  if (count > 0 && !upload(rects, count)) {
    off();
    return;
  }
  const uint64_t uploaded = port_.micros();
  stats_.renderUs = static_cast<uint32_t>(rendered - started);
  stats_.uploadUs = static_cast<uint32_t>(uploaded - rendered);
  if (stats_.renderUs > stats_.worstRenderUs) stats_.worstRenderUs = stats_.renderUs;
  if (stats_.uploadUs > stats_.worstUploadUs) stats_.worstUploadUs = stats_.uploadUs;
  ++stats_.frames;
}

bool PlayerOverlay::upload(const player_view::Rect* rects, int count) {
  uint32_t words[16];
  for (int r = 0; r < count; ++r) {
    uint32_t x0 = rects[r].x0 & ~1U;
    uint32_t x1 = (rects[r].x1 + 1U) & ~1U;
    uint32_t y1 = rects[r].y1;
    if (x1 > kWidth) x1 = kWidth;
    if (y1 > kHeight) y1 = kHeight;
    for (uint32_t y = rects[r].y0; y < y1 && x0 < x1; ++y) {
      const uint16_t* row = canvas_ + static_cast<size_t>(y) * kWidth;
      uint32_t word = kBase + y * kStride + x0 / 2;
      for (uint32_t x = x0; x < x1;) {
        size_t n = 0;
        for (; n < 16 && x < x1; ++n, x += 2) {
          words[n] = row[x] | (static_cast<uint32_t>(row[x + 1]) << 16);
        }
        if (!port_.write(word, words, n)) {
          ++stats_.writeFails;
          return false;
        }
        word += static_cast<uint32_t>(n);
        stats_.words += static_cast<uint32_t>(n);
      }
    }
  }
  return true;
}

void PlayerOverlay::tick(uint32_t nowMs) {
  ++tick_;
  // Перезагрузка ПЛИС (multiboot, другое ядро): оверлеи выключены, холсты в SDRAM пропали
  const uint32_t epoch = port_.tangEpoch();
  if (!epochKnown_ || epoch != epoch_) {
    epochKnown_ = true;
    epoch_ = epoch;
    if (state_ != State::kOff) {
      state_ = State::kOff;
      alpha_ = 0;
      freeMemory();
    }
  }
  if (!port_.overlaySupported()) {
    if (state_ != State::kOff) {
      state_ = State::kOff;
      alpha_ = 0;
      freeMemory();
    }
    return;
  }
  switch (state_) {
    case State::kOff:
      // Пока экрана нет — приложение спрашивается реже: 10 раз в секунду
      if (tick_ % 5 != 0 || static_cast<int32_t>(nowMs - retryAtMs_) < 0) return;
      if (fetchModel(nowMs) && model_.visible && !show(nowMs)) {
        retryAtMs_ = nowMs + kRetryMs;
      }
      return;
    case State::kFadeIn: {
      const bool ok = fetchModel(nowMs);
      if ((ok && !model_.visible) || (!ok && nowMs - lastModelMs_ >= kModelLostMs)) {
        state_ = State::kFadeOut;
        return;
      }
      if (!setAlpha(static_cast<uint8_t>(alpha_ + 1))) {
        off();
      } else if (alpha_ >= kOpaque) {
        state_ = State::kShown;
      }
      return;
    }
    case State::kShown: {
      if (tick_ % kFrameTicks != 0) return;
      const bool ok = fetchModel(nowMs);
      if ((ok && !model_.visible) || (!ok && nowMs - lastModelMs_ >= kModelLostMs)) {
        state_ = State::kFadeOut;
        return;
      }
      if (ok) frame(nowMs);
      return;
    }
    case State::kFadeOut: {
      if (fetchModel(nowMs) && model_.visible) {
        state_ = State::kFadeIn;              // снова нужен — проявиться с той же ступени
        return;
      }
      if (alpha_ == 0 || !setAlpha(static_cast<uint8_t>(alpha_ - 1))) {
        off();
      }
      return;
    }
  }
}

void PlayerOverlay::releaseMemory(uint32_t nowMs) {
  appGone();
  retryAtMs_ = nowMs + kReleaseHoldMs;
}

void PlayerOverlay::appGone() {
  if (state_ != State::kOff) port_.configure(false, 0);
  state_ = State::kOff;
  alpha_ = 0;
  freeMemory();
}

}  // namespace zifi
