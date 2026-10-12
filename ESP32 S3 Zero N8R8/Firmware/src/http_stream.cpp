#include "zifi/http_stream.hpp"

#include <Arduino.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ZIFI_BUILD_VERSION
#define ZIFI_BUILD_VERSION "vdac4"
#endif

namespace zifi {
namespace {

// Корневые сертификаты — как в net_client.cpp: ядро 2.x (сборка переходника ZiFi)
// берёт вшитый в прошивку data/cert/x509_crt_bundle.bin, ядро 3.x — набор, который
// ESP-IDF собирает вместе с прошивкой (прежний файл оно отвергнет).
#if !defined(ESP_ARDUINO_VERSION_MAJOR) || ESP_ARDUINO_VERSION_MAJOR < 3
extern const uint8_t kCaBundleStart[]
    asm("_binary_data_cert_x509_crt_bundle_bin_start");
#endif

bool startsWithNoCase(const char* text, const char* prefix) {
  for (; *prefix != 0; ++text, ++prefix) {
    if (tolower(static_cast<unsigned char>(*text)) !=
        tolower(static_cast<unsigned char>(*prefix))) {
      return false;
    }
  }
  return true;
}

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// «http[s]://[пользователь@]хост[:порт][/путь]»: путь — указатель внутрь url
// (пустой — «/» при запросе).
bool parseUrl(const char* url, bool& tls, char* host, size_t hostSize, uint16_t& port,
              const char*& path) {
  const char* p = nullptr;
  if (startsWithNoCase(url, "http://")) {
    tls = false;
    port = 80;
    p = url + 7;
  } else if (startsWithNoCase(url, "https://")) {
    tls = true;
    port = 443;
    p = url + 8;
  } else {
    return false;
  }
  const char* end = p + strcspn(p, "/?#");
  const char* at = static_cast<const char*>(memchr(p, '@', end - p));
  if (at != nullptr) {
    p = at + 1;                          // логин и пароль серверу не нужны
  }
  size_t length = 0;
  while (p < end && *p != ':' && length + 1 < hostSize) {
    host[length++] = *p++;
  }
  host[length] = 0;
  if (length == 0) {
    return false;
  }
  if (p < end && *p == ':') {
    char* after = nullptr;
    const long value = strtol(p + 1, &after, 10);
    if (value <= 0 || value > 65535 || after != end) {
      return false;
    }
    port = static_cast<uint16_t>(value);
  }
  path = end;
  return true;
}

}  // namespace

void HttpStream::setError(const char* text) {
  snprintf(error_, sizeof(error_), "%s", text);
}

bool HttpStream::connectTo(bool tls, const char* host, uint16_t port, uint32_t timeoutMs) {
  if (tls) {
    if (!caLoaded_) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
      secure_.useBuiltinCACertBundle();
#else
      secure_.setCACertBundle(kCaBundleStart);
#endif
      caLoaded_ = true;
    }
    secure_.setHandshakeTimeout((timeoutMs + 999) / 1000);
    if (!secure_.connect(host, port, static_cast<int32_t>(timeoutMs))) {
      char detail[48] = {};
      secure_.lastError(detail, sizeof(detail));
      snprintf(error_, sizeof(error_), "tls: %.40s", detail[0] != 0 ? detail : "connect failed");
      secure_.stop();
      return false;
    }
    transport_ = &secure_;
  } else {
    if (!plain_.connect(host, port, static_cast<int32_t>(timeoutMs))) {
      setError("no connection");
      plain_.stop();
      return false;
    }
    transport_ = &plain_;
  }
  transport_->setNoDelay(true);
  return true;
}

bool HttpStream::sendRequest(const char* host, uint16_t port, bool tls, const char* path,
                             const char* extra) {
  char hostField[144];
  if (port == (tls ? 443 : 80)) {
    snprintf(hostField, sizeof(hostField), "%s", host);
  } else {
    snprintf(hostField, sizeof(hostField), "%s:%u", host, static_cast<unsigned>(port));
  }
  const int length = snprintf(
      request_, sizeof(request_),
      "GET %s%s HTTP/1.0\r\nHost: %s\r\nUser-Agent: VDAC4/%s\r\nAccept: */*\r\n%sConnection: "
      "close\r\n\r\n",
      path[0] == '/' ? "" : "/", path, hostField, ZIFI_BUILD_VERSION,
      extra != nullptr ? extra : "");
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(request_)) {
    setError("request too long");
    return false;
  }
  size_t sent = 0;
  while (sent < static_cast<size_t>(length)) {
    const size_t part = transport_->write(reinterpret_cast<const uint8_t*>(request_) + sent,
                                          static_cast<size_t>(length) - sent);
    if (part == 0) {
      setError("send failed");
      return false;
    }
    sent += part;
  }
  return true;
}

int HttpStream::readByte(uint32_t deadline) {
  for (;;) {
    // Срок — до чтения: сервер, шлющий байты без конца, иначе держал бы вечно.
    if (static_cast<int32_t>(millis() - deadline) >= 0) {
      return -2;
    }
    if (transport_->available() > 0) {
      const int c = transport_->read();
      if (c >= 0) {
        return c;
      }
    }
    if (!transport_->connected()) {
      return -1;
    }
    if (static_cast<int32_t>(millis() - deadline) >= 0) {
      return -2;
    }
    vTaskDelay(1);
  }
}

bool HttpStream::readHead(uint32_t timeoutMs, int& status) {
  const uint32_t deadline = millis() + timeoutMs;
  char line[512];
  size_t length = 0;
  bool first = true;
  headLength_ = 0;
  head_[0] = 0;
  // Заголовок длиннее этого — не заголовок: незавершённая строка без конца.
  constexpr size_t kMaxHeadBytes = 16384;
  size_t total = 0;
  for (;;) {
    const int c = readByte(deadline);
    if (c < 0) {
      setError(c == -2 ? "no answer" : "closed before the header");
      return false;
    }
    if (++total > kMaxHeadBytes) {
      setError("header too long");
      return false;
    }
    if (c == '\r') {
      continue;
    }
    if (c != '\n') {
      if (length + 1 < sizeof(line)) {
        line[length++] = static_cast<char>(c);
      }
      continue;
    }
    line[length] = 0;
    if (first) {
      // «HTTP/1.0 200 OK» или «ICY 200 OK» (Shoutcast 1).
      first = false;
      const char* code = nullptr;
      if (startsWithNoCase(line, "HTTP/")) {
        code = strchr(line, ' ');
      } else if (startsWithNoCase(line, "ICY ")) {
        code = line + 3;
      }
      if (code == nullptr) {
        setError("not an HTTP answer");
        return false;
      }
      status = atoi(code);
      length = 0;
      continue;
    }
    if (length == 0) {
      return true;                       // пустая строка — конец заголовка
    }
    char* colon = strchr(line, ':');
    if (colon != nullptr) {
      *colon = 0;
      const char* value = colon + 1;
      while (*value == ' ' || *value == '\t') {
        ++value;
      }
      const int written = snprintf(head_ + headLength_, sizeof(head_) - headLength_, "%s:%s\n",
                                   line, value);
      if (written > 0 && headLength_ + static_cast<size_t>(written) < sizeof(head_)) {
        headLength_ += static_cast<size_t>(written);
      } else {
        head_[headLength_] = 0;          // не помещается — лишние строки прочь
      }
    }
    length = 0;
  }
}

bool HttpStream::open(const char* url, const char* extra, uint32_t timeoutMs, int& status) {
  close();
  status = 0;
  error_[0] = 0;
  if (url == nullptr || strlen(url) >= sizeof(url_)) {
    setError("bad url");
    return false;
  }
  strcpy(url_, url);
  for (int hop = 0; hop <= kMaxRedirects; ++hop) {
    bool tls = false;
    char host[128];
    uint16_t port = 0;
    const char* path = nullptr;
    if (!parseUrl(url_, tls, host, sizeof(host), port, path)) {
      setError("bad url");
      return false;
    }
    if (!connectTo(tls, host, port, timeoutMs)) {
      return false;
    }
    if (!sendRequest(host, port, tls, path, extra) || !readHead(timeoutMs, status)) {
      close();
      return false;
    }
    if (!isRedirect(status)) {
      if (status >= 200 && status < 300) {
        return true;
      }
      snprintf(error_, sizeof(error_), "HTTP %d", status);
      close();
      return false;
    }
    // Переадресация: полный адрес или путь на том же сервере.
    const char* location = header("location");
    if (location == nullptr || location[0] == 0) {
      setError("redirect without location");
      close();
      return false;
    }
    char next[sizeof(url_)];
    int length = 0;
    if (startsWithNoCase(location, "http://") || startsWithNoCase(location, "https://")) {
      length = snprintf(next, sizeof(next), "%s", location);
    } else {
      length = snprintf(next, sizeof(next), "%s://%s:%u%s%s", tls ? "https" : "http", host,
                        static_cast<unsigned>(port), location[0] == '/' ? "" : "/", location);
    }
    close();
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(url_)) {
      setError("redirect too long");
      return false;
    }
    strcpy(url_, next);
  }
  setError("too many redirects");
  return false;
}

const char* HttpStream::header(const char* name) {
  const size_t nameLength = strlen(name);
  for (const char* line = head_; *line != 0;) {
    const char* end = strchr(line, '\n');
    if (end == nullptr) {
      end = line + strlen(line);
    }
    if (static_cast<size_t>(end - line) > nameLength && line[nameLength] == ':' &&
        strncasecmp(line, name, nameLength) == 0) {
      const char* value = line + nameLength + 1;
      size_t length = static_cast<size_t>(end - value);
      if (length >= sizeof(value_)) {
        length = sizeof(value_) - 1;
      }
      memcpy(value_, value, length);
      value_[length] = 0;
      return value_;
    }
    line = *end == '\n' ? end + 1 : end;
  }
  return nullptr;
}

int HttpStream::read(uint8_t* data, size_t count, uint32_t timeoutMs) {
  if (transport_ == nullptr) {
    return -1;
  }
  if (data == nullptr || count == 0) {
    return 0;
  }
  const uint32_t start = millis();
  for (;;) {
    const int available = transport_->available();
    if (available > 0) {
      const int got =
          transport_->read(data, count < static_cast<size_t>(available) ? count
                                                                        : static_cast<size_t>(available));
      if (got > 0) {
        return got;
      }
    }
    if (!transport_->connected()) {
      return -1;
    }
    if (millis() - start >= timeoutMs) {
      return 0;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void HttpStream::close() {
  // Оба явно, как NetClient: у защищённого stop ещё и освобождает контекст TLS.
  if (transport_ != nullptr) {
    plain_.stop();
    secure_.stop();
    transport_ = nullptr;
  }
  headLength_ = 0;
  head_[0] = 0;
}

}  // namespace zifi
