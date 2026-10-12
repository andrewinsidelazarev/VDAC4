#pragma once

#include <stddef.h>
#include <stdint.h>

#if __has_include(<mbedtls/build_info.h>)
#include <mbedtls/build_info.h>
#else
#include <mbedtls/version.h>
#endif

#if MBEDTLS_VERSION_MAJOR >= 4
#include <psa/crypto.h>
#else
#include <mbedtls/sha256.h>
#endif

namespace zifi {

// Потоковый SHA-256 для проверки образа прошивки и журнала.
// Отдельный класс нужен из-за смены API: mbedTLS 2.x (Arduino-ядро 2.x) даёт функции
// mbedtls_sha256_*_ret, mbedTLS 3.x — те же без суффикса, а в mbedTLS 4.x (ESP-IDF 6)
// прежний интерфейс убран и остался только PSA Crypto.
// Контекст освобождается деструктором, поэтому ранний выход из функции утечки не создаёт.
class Sha256 {
 public:
  Sha256();
  ~Sha256();
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;

  bool begin();
  bool update(const uint8_t* data, size_t length);
  bool finish(uint8_t digest[32]);

 private:
#if MBEDTLS_VERSION_MAJOR >= 4
  psa_hash_operation_t operation_;
#else
  mbedtls_sha256_context context_;
#endif
};

}  // namespace zifi
