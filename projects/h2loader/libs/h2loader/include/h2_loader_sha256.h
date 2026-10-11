#ifndef H2_LOADER_SHA256_H
#define H2_LOADER_SHA256_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Caller-owned streaming state. The legacy struct tag and layout remain part
 * of the Loader API; hashing delegates to the shared portable digest. */
typedef struct h2_loader_sha256 {
  uint32_t state[8];
  uint64_t total_bytes;
  uint8_t block[64];
  size_t block_len;
} h2_loader_sha256_t;
void h2_loader_sha256_init(h2_loader_sha256_t *sha);
void h2_loader_sha256_update(h2_loader_sha256_t *sha, const uint8_t *data,
                             size_t len);
void h2_loader_sha256_finish(h2_loader_sha256_t *sha, uint8_t out[32]);
void h2_loader_sha256_hex(const uint8_t digest[32], char out[65]);
#ifdef __cplusplus
}
#endif
#endif
