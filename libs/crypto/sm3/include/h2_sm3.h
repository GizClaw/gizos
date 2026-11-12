#ifndef H2_SM3_H
#define H2_SM3_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Caller-owned streaming SM3 state. No allocation or platform calls.
 * Initialize before use and serialize access to each context. */
typedef struct h2_sm3 {
  uint32_t state[8];
  uint64_t total_bytes;
  uint8_t block[64];
  size_t block_len;
} h2_sm3_t;

/** All context and output pointers must be non-NULL. update accepts NULL
 * data only for zero length. finish writes 32 bytes and clears the context.
 * Input and output storage must not overlap the context. */
void h2_sm3_init(h2_sm3_t* context);
void h2_sm3_update(h2_sm3_t* context, const uint8_t* data, size_t length);
void h2_sm3_finish(h2_sm3_t* context, uint8_t out[32]);

/** Synchronous HMAC-SM3 into caller-owned storage. key and data may be NULL
 * only when their respective lengths are zero. out must be non-NULL and
 * must not overlap inputs. No storage is retained or allocated. */
void h2_sm3_hmac(const uint8_t* key, size_t key_length, const uint8_t* data,
                 size_t length, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
#endif
