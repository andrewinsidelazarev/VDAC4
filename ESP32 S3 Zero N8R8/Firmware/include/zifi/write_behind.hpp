// Накопитель записи подряд (0.1.149, 19.11.2026) — для тома USB Flash.
//
// WC пишет файл по сектору (512 байт) на команду, и прежде каждый сектор уходил на флешку своей
// командой USB (SCSI WRITE(10) на один блок). Флешка на запись одного сектора тратит почти столько
// же, сколько на 4 КБ, а ответ Z80 ждал конца записи — копирование на флешку шло медленно (слово
// пользователя 19.11.2026). Секторы, идущие подряд, копятся здесь и уходят приёмнику одним куском
// до capacity байт: у флешки это одна команда на 8 секторов вместо восьми.
//
// Порядок записи сохраняется: байты не подряд к накопленному (другое место файла, повтор сектора)
// сначала выталкивают накопленное. Отказ приёмника получает тот вызов, который выталкивал, —
// накопленное при этом теряется (WC получит ошибку носителя на этом секторе, хотя не легли и
// прежние, до capacity байт). Без буфера (не выделен) запись идёт приёмнику сразу.
// Класс без зависимостей от ESP-IDF: стенд — tests/write_behind_host.cpp.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace zifi {

class WriteBehind {
 public:
  // Приёмник: записать n байтов с offset. false — отказ.
  using Sink = bool (*)(void* user, uint64_t offset, const uint8_t* data, size_t n);

  // Буфер — снаружи (у ESP в PSRAM); capacity — и наибольший кусок приёмнику.
  void attach(uint8_t* buffer, size_t capacity) {
    buffer_ = buffer;
    capacity_ = buffer != nullptr ? capacity : 0;
    length_ = 0;
  }
  bool attached() const { return buffer_ != nullptr; }
  size_t pending() const { return length_; }

  // n байтов с offset — в накопитель; полон — приёмнику.
  bool put(uint64_t offset, const uint8_t* data, size_t n, Sink sink, void* user) {
    if (buffer_ == nullptr) return sink(user, offset, data, n);
    if (length_ != 0 && offset != start_ + length_ && !drain(sink, user)) return false;
    while (n != 0) {
      if (length_ == 0) start_ = offset;
      size_t take = capacity_ - length_;
      if (take > n) take = n;
      memcpy(buffer_ + length_, data, take);
      length_ += take;
      offset += take;
      data += take;
      n -= take;
      if (length_ == capacity_ && !drain(sink, user)) return false;
    }
    return true;
  }

  // Накопленное — приёмнику. Отказ: накопленное отброшено.
  bool drain(Sink sink, void* user) {
    if (length_ == 0) return true;
    const size_t n = length_;
    length_ = 0;
    return sink(user, start_, buffer_, n);
  }

  // Забыть накопленное (файл закрыт без записи — например, удалён).
  void drop() { length_ = 0; }

 private:
  uint8_t* buffer_ = nullptr;
  size_t capacity_ = 0;
  size_t length_ = 0;
  uint64_t start_ = 0;
};

}  // namespace zifi
