#pragma once

#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <stddef.h>
#include <stdint.h>

namespace zifi {

// Поток HTTP(S) для модулей ESP (zm_core_t::net_*): интернет-радио. Запрос —
// HTTP/1.0, поэтому тело приходит без chunked; ответ — «HTTP/1.x NNN» или
// «ICY NNN» (Shoutcast первой версии); переадресации — до kMaxRedirects.
// Работает в задаче модуля, которая его открыла (не в сетевой задаче ядра), и
// только в ней: NetClient сетевой задачи и его соединение не затрагиваются.
class HttpStream {
 public:
  static constexpr int kMaxRedirects = 5;

  // GET url («http://…» или «https://…») с заголовками extra («Имя: значение\r\n»,
  // можно nullptr); status — код ответа (0 — не соединилось). true — ответ 2xx,
  // тело читает read.
  bool open(const char* url, const char* extra, uint32_t timeoutMs, int& status);
  // Значение заголовка ответа (имя без учёта регистра) или nullptr; строка живёт
  // до следующего вызова.
  const char* header(const char* name);
  // До count байтов тела: >0 — сколько, 0 — за timeoutMs ничего, −1 — конец/обрыв.
  int read(uint8_t* data, size_t count, uint32_t timeoutMs);
  void close();
  // Последняя ошибка — для журнала.
  const char* error() const { return error_; }

 private:
  bool connectTo(bool tls, const char* host, uint16_t port, uint32_t timeoutMs);
  bool sendRequest(const char* host, uint16_t port, bool tls, const char* path,
                   const char* extra);
  bool readHead(uint32_t timeoutMs, int& status);
  int readByte(uint32_t deadline);
  void setError(const char* text);

  WiFiClient plain_;
  WiFiClientSecure secure_;
  WiFiClient* transport_ = nullptr;
  bool caLoaded_ = false;
  char url_[512] = {};
  char request_[1024] = {};
  // Заголовки ответа — строками «имя:значение\n» (без пробелов вокруг двоеточия).
  char head_[1536] = {};
  size_t headLength_ = 0;
  char value_[256] = {};
  char error_[64] = {};
};

}  // namespace zifi
