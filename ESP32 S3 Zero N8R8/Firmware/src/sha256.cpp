#include "zifi/sha256.hpp"

namespace zifi {

#if MBEDTLS_VERSION_MAJOR >= 4

Sha256::Sha256() : operation_(psa_hash_operation_init()) {}

Sha256::~Sha256() {
  psa_hash_abort(&operation_);
}

bool Sha256::begin() {
  // Повторный вызов psa_crypto_init безвреден: библиотека запоминает, что уже готова.
  if (psa_crypto_init() != PSA_SUCCESS) {
    return false;
  }
  psa_hash_abort(&operation_);
  operation_ = psa_hash_operation_init();
  return psa_hash_setup(&operation_, PSA_ALG_SHA_256) == PSA_SUCCESS;
}

bool Sha256::update(const uint8_t* data, size_t length) {
  return psa_hash_update(&operation_, data, length) == PSA_SUCCESS;
}

bool Sha256::finish(uint8_t digest[32]) {
  size_t produced = 0;
  return psa_hash_finish(&operation_, digest, 32, &produced) == PSA_SUCCESS &&
         produced == 32;
}

#else

Sha256::Sha256() {
  mbedtls_sha256_init(&context_);
}

Sha256::~Sha256() {
  mbedtls_sha256_free(&context_);
}

#if MBEDTLS_VERSION_MAJOR >= 3

bool Sha256::begin() {
  return mbedtls_sha256_starts(&context_, 0) == 0;
}

bool Sha256::update(const uint8_t* data, size_t length) {
  return mbedtls_sha256_update(&context_, data, length) == 0;
}

bool Sha256::finish(uint8_t digest[32]) {
  return mbedtls_sha256_finish(&context_, digest) == 0;
}

#else

bool Sha256::begin() {
  return mbedtls_sha256_starts_ret(&context_, 0) == 0;
}

bool Sha256::update(const uint8_t* data, size_t length) {
  return mbedtls_sha256_update_ret(&context_, data, length) == 0;
}

bool Sha256::finish(uint8_t digest[32]) {
  return mbedtls_sha256_finish_ret(&context_, digest) == 0;
}

#endif

#endif

}  // namespace zifi
