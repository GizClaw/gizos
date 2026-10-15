#include "h2_loader_sha256.h"
#include "h2_sha256.h"
#include <string.h>

_Static_assert(sizeof(h2_loader_sha256_t) == sizeof(h2_sha256_t),
               "SHA-256 state layout");
_Static_assert(offsetof(h2_loader_sha256_t, state) ==
                   offsetof(h2_sha256_t, state),
               "SHA-256 state fields");
_Static_assert(offsetof(h2_loader_sha256_t, total_bytes) ==
                   offsetof(h2_sha256_t, total_bytes),
               "SHA-256 byte count");
_Static_assert(offsetof(h2_loader_sha256_t, block) ==
                   offsetof(h2_sha256_t, block),
               "SHA-256 block");
_Static_assert(offsetof(h2_loader_sha256_t, block_len) ==
                   offsetof(h2_sha256_t, block_len),
               "SHA-256 block count");

/* Copy rather than alias unrelated struct types, preserving the public Loader
 * struct tag without violating strict aliasing on firmware toolchains. */
void h2_loader_sha256_init(h2_loader_sha256_t *sha) {
  if (!sha)
    return;
  h2_sha256_t shared;
  h2_sha256_init(&shared);
  memcpy(sha, &shared, sizeof(shared));
}
void h2_loader_sha256_update(h2_loader_sha256_t *sha, const uint8_t *data,
                             size_t len) {
  if (!sha)
    return;
  h2_sha256_t shared;
  memcpy(&shared, sha, sizeof(shared));
  h2_sha256_update(&shared, data, len);
  memcpy(sha, &shared, sizeof(shared));
}
void h2_loader_sha256_finish(h2_loader_sha256_t *sha, uint8_t out[32]) {
  if (!sha || !out)
    return;
  h2_sha256_t shared;
  memcpy(&shared, sha, sizeof(shared));
  h2_sha256_finish(&shared, out);
  memcpy(sha, &shared, sizeof(shared));
}
void h2_loader_sha256_hex(const uint8_t digest[32], char out[65]) {
  h2_sha256_hex(digest, out);
}
