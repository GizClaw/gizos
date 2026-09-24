#ifndef H2_LOADER_SHA256_H
#define H2_LOADER_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Caller-owned streaming digest state. Initialize before use; do not share a
 * context between concurrent operations. No heap or platform APIs are used. */
typedef struct h2_loader_sha256 {
  uint32_t state[8];
  uint64_t total_bytes;
  uint8_t block[64];
  size_t block_len;
} h2_loader_sha256_t;

/** Initialize a non-NULL context for a new SHA-256 digest. */
void h2_loader_sha256_init(h2_loader_sha256_t *sha);
/** Consume len bytes synchronously. data may be NULL only when len is zero.
 * The context must be initialized and not yet finalized. */
void h2_loader_sha256_update(
    h2_loader_sha256_t *sha, const uint8_t *data, size_t len);
/** Write 32 digest bytes and clear the context. Both pointers must be non-NULL
 * and non-overlapping. Reinitialize the context before hashing another input. */
void h2_loader_sha256_finish(h2_loader_sha256_t *sha, uint8_t out[32]);
/** Write 64 lowercase hexadecimal characters and a NUL terminator.
 * digest and out must be non-NULL and non-overlapping. */
void h2_loader_sha256_hex(const uint8_t digest[32], char out[65]);

#ifdef __cplusplus
}
#endif

#endif
