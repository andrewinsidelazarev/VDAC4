#pragma once

#include <stddef.h>
#include <stdint.h>

namespace zifi {

// Конец JSON-документа в потоке байтов (0.1.162). Свежий ответ API GitHub на большой запрос (дерево
// папки после пуша) приходит без Content-Length: конец ответа — закрытие соединения, а NetClient на
// TLS его не замечал и ждал 30 с до «body timeout» (22.11.2026: VDAC4 Online Update не скачивал ничего
// ни в Италии, ни в Украине; с ПК тот же ответ — за доли секунды). Теперь чтение кончается на
// закрывающей скобке верхнего объекта или массива. Скобки внутри строк (с \" и \\) не в счёт.
// Документ не с '{' или '[' — конца здесь нет никогда: такой ответ читается до закрытия, как прежде.
class JsonEnd {
 public:
  // Принять следующие байты потока. true — документ кончился: used — сколько байтов из этих
  // length ему принадлежат (остальное — лишнее за ним).
  bool feed(const uint8_t* data, size_t length, size_t& used) {
    used = 0;
    for (size_t i = 0; i < length && !done_ && !other_; ++i) {
      const uint8_t c = data[i];
      if (!started_) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
          continue;
        }
        if (c != '{' && c != '[') {
          other_ = true;
          break;
        }
        started_ = true;
        depth_ = 1;
        continue;
      }
      if (string_) {
        if (escape_) {
          escape_ = false;
        } else if (c == '\\') {
          escape_ = true;
        } else if (c == '"') {
          string_ = false;
        }
        continue;
      }
      if (c == '"') {
        string_ = true;
      } else if (c == '{' || c == '[') {
        ++depth_;
      } else if ((c == '}' || c == ']') && --depth_ == 0) {
        done_ = true;
        used = i + 1;
      }
    }
    return done_;
  }
  bool done() const { return done_; }

 private:
  uint32_t depth_ = 0;
  bool started_ = false;
  bool string_ = false;
  bool escape_ = false;
  bool done_ = false;
  bool other_ = false;
};

}  // namespace zifi
