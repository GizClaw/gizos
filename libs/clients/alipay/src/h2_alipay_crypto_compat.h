#ifndef H2_ALIPAY_CRYPTO_COMPAT_H
#define H2_ALIPAY_CRYPTO_COMPAT_H

#include "os/h2_pal_crypto.h"
#include "os/h2_pal_mem.h"
#include <stdbool.h>

/* Private Alipay compatibility rules for legacy wire formats. */

/** Apply a repeating byte mask, optionally reading each mask cycle in reverse.
 * Input/output may be identical; other overlap and overlap with mask are not
 * supported. Empty input permits NULL input/output. A nonempty mask is always
 * required. Invalid arguments preserve output. This legacy obfuscation
 * primitive provides neither confidentiality nor authentication by itself. */
h2_pal_result_t h2_alipay_xor_mask(const uint8_t* input, size_t length,
                                   const uint8_t* mask, size_t mask_length,
                                   bool reverse, uint8_t* output);

/** Legacy byte-wise multiply-by-31 hash with unsigned 64-bit wraparound.
 * This is a protocol compatibility hash, not a cryptographic digest.
 * Empty input permits NULL; errors preserve output. */
h2_pal_result_t h2_alipay_hash31(const uint8_t* input, size_t length,
                                 uint64_t* output);

typedef struct h2_alipay_md5_aes_keyset h2_alipay_md5_aes_keyset_t;

typedef struct h2_alipay_key_material {
  const uint8_t* data;
  size_t length;
} h2_alipay_key_material_t;

/** Prepare legacy AES-CBC/PKCS#7 keys from caller-supplied material. Each
 * key and IV is the first 16 lowercase hexadecimal characters of MD5(material),
 * without NUL. The keyset copies only derived bytes, borrows memory/crypto
 * until close and retains no material pointers. All allocations use memory.
 * The caller serializes access and closes only after operations return.
 * out is set to NULL on every error. No provider is selected here. */
h2_pal_result_t h2_alipay_md5_aes_keyset_open(
    const h2_pal_mem_api_t* memory, const h2_pal_crypto_api_t* crypto,
    const h2_alipay_key_material_t* materials, size_t count,
    h2_alipay_md5_aes_keyset_t** out);

/** Encrypt/decrypt with one prepared key. Output and failure semantics follow
 * h2_crypto_aes_cbc_pkcs7_encrypt/decrypt. The index must be less than count
 * supplied at open. Output is raw ciphertext/plaintext; no text encoding is
 * performed.
 */
h2_pal_result_t h2_alipay_md5_aes_keyset_encrypt(
    h2_alipay_md5_aes_keyset_t* keys, size_t index, const uint8_t* input,
    size_t input_length, uint8_t* output, size_t capacity, size_t* length);
h2_pal_result_t h2_alipay_md5_aes_keyset_decrypt(
    h2_alipay_md5_aes_keyset_t* keys, size_t index, const uint8_t* input,
    size_t input_length, uint8_t* output, size_t capacity, size_t* length);

/** Wipe derived keys and free the keyset. NULL is safe. */
void h2_alipay_md5_aes_keyset_close(h2_alipay_md5_aes_keyset_t* keys);

#endif
