#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct {
  int unused;
} mbedtls_sha256_context;
static inline void mbedtls_sha256_init(mbedtls_sha256_context *s) { (void)s; }
static inline int mbedtls_sha256_starts(mbedtls_sha256_context *s, int type) {
  (void)s;
  (void)type;
  return 0;
}
static inline int mbedtls_sha256_update(mbedtls_sha256_context *s,
                                        const uint8_t *data, size_t n) {
  (void)s;
  (void)data;
  (void)n;
  return 0;
}
static inline int mbedtls_sha256_finish(mbedtls_sha256_context *s,
                                        uint8_t *data) {
  (void)s;
  (void)data;
  return 0;
}
static inline void mbedtls_sha256_free(mbedtls_sha256_context *s) { (void)s; }
