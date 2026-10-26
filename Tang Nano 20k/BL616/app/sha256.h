/* SPDX-License-Identifier: Apache-2.0 */
/* SHA-256 (FIPS 180-4) — проверка образа прошивки при обновлении по сети. */
#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    uint32_t n;
} sha256_t;

void sha256_init(sha256_t *s);
void sha256_update(sha256_t *s, const uint8_t *p, size_t n);
void sha256_final(sha256_t *s, uint8_t out[32]);
void sha256(const uint8_t *p, size_t n, uint8_t out[32]);

#endif
