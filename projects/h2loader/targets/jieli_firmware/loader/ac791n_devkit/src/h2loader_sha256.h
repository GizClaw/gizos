#ifndef H2_JIELI_H2LOADER_SHA256_H
#define H2_JIELI_H2LOADER_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct h2_jieli_sha256 {
  uint32_t state[8];
  uint64_t total_bytes;
  uint8_t block[64];
  size_t block_len;
} h2_jieli_sha256_t;

void h2_jieli_sha256_init(h2_jieli_sha256_t *sha);
void h2_jieli_sha256_update(
    h2_jieli_sha256_t *sha, const uint8_t *data, size_t len);
void h2_jieli_sha256_finish(h2_jieli_sha256_t *sha, uint8_t out[32]);
void h2_jieli_sha256_hex(const uint8_t digest[32], char out[65]);

#endif
