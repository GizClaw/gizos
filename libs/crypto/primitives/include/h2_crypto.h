#ifndef H2_CRYPTO_H
#define H2_CRYPTO_H

#include "os/h2_pal_crypto.h"
#include "os/h2_pal_mem.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Synchronous operations using the caller-selected Crypto PAL. These
 * functions select no concrete provider, retain no inputs and create no
 * global state. The provider and all input/output storage remain borrowed
 * through the call. Missing provider operations return UNSUPPORTED.
 * Buffer, key and output requirements otherwise follow the public PAL
 * contract. Random, MD5, HMAC, AES and elliptic-curve algorithms are supplied
 * by that provider, without a second software implementation here. */
h2_pal_result_t h2_crypto_random(const h2_pal_crypto_api_t* crypto,
                                 uint8_t* output, size_t length);
h2_pal_result_t h2_crypto_md5(const h2_pal_crypto_api_t* crypto,
                              const uint8_t* data, size_t length,
                              uint8_t output[16]);
/** Legacy SHA-1 and HMAC-MD5 through the injected provider. Synchronous,
 * borrowed inputs; NULL is valid only at zero length. Output is zero on
 * failure. These operations select no provider and duplicate no algorithm.
 */
h2_pal_result_t h2_crypto_sha1(const h2_pal_crypto_api_t* crypto,
                               const uint8_t* input, size_t input_length,
                               uint8_t output[20]);
h2_pal_result_t h2_crypto_hmac_md5(const h2_pal_crypto_api_t* crypto,
                                   const uint8_t* key, size_t key_length,
                                   const uint8_t* input, size_t input_length,
                                   uint8_t output[16]);
/** Allocation-free SHA-256 and SM3 using the established portable packages.
 * Inputs remain borrowed; NULL input is valid only at zero length. Output
 * contains 32 bytes and is zero on validation failure. */
h2_pal_result_t h2_crypto_sha256(const uint8_t* input, size_t input_length,
                                 uint8_t output[32]);
h2_pal_result_t h2_crypto_sm3(const uint8_t* input, size_t input_length,
                              uint8_t output[32]);

/** Apply a repeating byte mask, optionally reading each mask cycle in reverse.
 * Input/output may be identical; other overlap and overlap with mask are not
 * supported. Empty input permits NULL input/output. A nonempty mask is always
 * required. Invalid arguments preserve output. This legacy obfuscation
 * primitive provides neither confidentiality nor authentication by itself. */
h2_pal_result_t h2_crypto_xor_mask(const uint8_t* input, size_t length,
                                   const uint8_t* mask, size_t mask_length,
                                   bool reverse, uint8_t* output);

/** Compare all bytes without an early exit based on their values. Empty
 * sequences are equal; NULL is otherwise invalid and returns false. */
bool h2_crypto_bytes_equal(const uint8_t* first, const uint8_t* second,
                           size_t length);

/** Legacy byte-wise multiply-by-31 hash with unsigned 64-bit wraparound.
 * This is a protocol compatibility hash, not a cryptographic digest.
 * Empty input permits NULL; errors preserve output. */
h2_pal_result_t h2_crypto_hash31(const uint8_t* input, size_t length,
                                 uint64_t* output);
h2_pal_result_t h2_crypto_hmac_sha1(const h2_pal_crypto_api_t* crypto,
                                    const uint8_t* key, size_t key_length,
                                    const uint8_t* data, size_t length,
                                    uint8_t output[20]);
h2_pal_result_t h2_crypto_aes_cbc(const h2_pal_crypto_api_t* crypto,
                                  h2_pal_crypto_cipher_direction_t direction,
                                  const uint8_t* key, size_t key_length,
                                  const uint8_t iv[16], const uint8_t* input,
                                  size_t length, h2_pal_crypto_buf_t* output);

/** Legacy DES/EDE2/EDE3 CBC without padding. Borrowing, overlap, length and
 * failure behavior follow Crypto PAL. This selects no concrete provider. */
h2_pal_result_t h2_crypto_des_cbc(const h2_pal_crypto_api_t* crypto,
                                  h2_pal_crypto_cipher_direction_t direction,
                                  const uint8_t* key, size_t key_length,
                                  const uint8_t iv[8], const uint8_t* input,
                                  size_t length, h2_pal_crypto_buf_t* output);

/** ISO/IEC 9797-1 algorithm 3 retail MAC, padding method 2, zero initial
 * chaining value and a 16-byte K1||K2 key. Returns the full 8-byte MAC; wire
 * truncation is caller-owned. Empty message permits NULL. Synchronous, no
 * allocation, borrowed inputs, output zeroed on failure. Reuses injected
 * DES/EDE2 operations instead of implementing a cipher. */
h2_pal_result_t h2_crypto_iso9797_alg3_mac(const h2_pal_crypto_api_t* crypto,
                                           const uint8_t key[16],
                                           const uint8_t* message,
                                           size_t length, uint8_t output[8]);
h2_pal_result_t h2_crypto_p256_keypair_generate(
    const h2_pal_crypto_api_t* crypto, h2_pal_p256_keypair_t* output);
h2_pal_result_t h2_crypto_p256_public_key_export(
    const h2_pal_crypto_api_t* crypto,
    const h2_pal_p256_public_key_t* public_key, uint8_t output[65]);
h2_pal_result_t h2_crypto_p256_shared_secret(
    const h2_pal_crypto_api_t* crypto,
    const h2_pal_p256_private_key_t* private_key,
    const h2_pal_p256_public_key_t* public_key, uint8_t output[32]);

/** Verify a raw r||s ECDSA-P256 signature over SHA-256(message), using
 * the injected provider. Inputs are borrowed for the call. Invalid
 * signatures report the provider's validation error; missing verification
 * returns UNSUPPORTED. */
h2_pal_result_t h2_crypto_ecdsa_p256_sha256_verify(
    const h2_pal_crypto_api_t* crypto,
    const h2_pal_p256_public_key_t* public_key, const uint8_t* message,
    size_t message_length, const h2_pal_p256_signature_t* signature);

/** Sign SHA-256(message) with the injected P-256 provider. The output is
 * fixed-width big-endian r||s. Inputs remain borrowed through the call;
 * nonce generation and scalar validation belong to that provider. */
h2_pal_result_t h2_crypto_ecdsa_p256_sha256_sign(
    const h2_pal_crypto_api_t* crypto,
    const h2_pal_p256_private_key_t* private_key, const uint8_t* message,
    size_t message_length, h2_pal_p256_signature_t* signature);

/** Encode the two unsigned integers in a raw P-256 signature as canonical
 * ASN.1 DER. This converts representation without validating scalar ranges.
 * At most 72 output bytes are needed. length becomes zero on failure;
 * invalid arguments or insufficient capacity preserve output. */
h2_pal_result_t h2_crypto_ecdsa_p256_der_encode(
    const h2_pal_p256_signature_t* signature, uint8_t* output, size_t capacity,
    size_t* length);

/** Canonical ASN.1 DER for a fixed-width big-endian ECDSA r||s value.
 * scalar_length is 1..32 and signature contains twice that many bytes.
 * Output needs at most 2*scalar_length+8 bytes; failure resets length and
 * preserves output. This converts representation without scalar validation.
 */
h2_pal_result_t h2_crypto_ecdsa_der_encode(const uint8_t* signature,
                                           size_t scalar_length,
                                           uint8_t* output, size_t capacity,
                                           size_t* length);

/** Legacy secp192k1 ECDSA over SHA-1(message), supplied by Crypto PAL.
 * Inputs are borrowed synchronously. Output is fixed-width big-endian r||s
 * and is zeroed on failure. This operation selects no concrete provider. */
h2_pal_result_t h2_crypto_ecdsa_secp192k1_sha1_sign(
    const h2_pal_crypto_api_t* crypto,
    const h2_pal_secp192k1_private_key_t* private_key, const uint8_t* message,
    size_t message_length, h2_pal_secp192k1_signature_t* signature);

/** Standard identity-bound SM2 signature over SM3(Z || message), where Z
 * binds the supplied identity, curve parameters and public point. Identity
 * has at most 8191 bytes; empty identity/message permit NULL. Inputs remain
 * borrowed through the call. Uses existing portable SM3 and injected Crypto
 * PAL for curve operations. No provider is selected; failures zero output. */
h2_pal_result_t h2_crypto_sm2_sign(const h2_pal_crypto_api_t* crypto,
                                   const h2_pal_sm2_private_key_t* private_key,
                                   const uint8_t* identity,
                                   size_t identity_length,
                                   const uint8_t* message,
                                   size_t message_length,
                                   h2_pal_sm2_signature_t* signature);

/** Verify the same identity-bound SM2/SM3 construction. Inputs are borrowed;
 * invalid signatures report FORMAT and missing operations UNSUPPORTED. */
h2_pal_result_t h2_crypto_sm2_verify(const h2_pal_crypto_api_t* crypto,
                                     const h2_pal_sm2_public_key_t* public_key,
                                     const uint8_t* identity,
                                     size_t identity_length,
                                     const uint8_t* message,
                                     size_t message_length,
                                     const h2_pal_sm2_signature_t* signature);

/** Import a 65-byte uncompressed SEC1 P-256 point, including its 0x04 prefix.
 * Validate both supplied coordinates using the injected provider before
 * writing the compressed point to output. Invalid prefix/coordinates return
 * FORMAT; other provider errors propagate. Outputs remain unchanged on error.
 */
h2_pal_result_t h2_crypto_p256_public_key_import(
    const h2_pal_crypto_api_t* crypto, const uint8_t input[65],
    h2_pal_p256_public_key_t* output);

/** AES-CBC with PKCS#7 padding. Keys contain 16, 24 or 32 bytes; the IV
 * contains 16 bytes and is never modified. Output may not overlap inputs.
 * length is set to zero before validation; success reports bytes excluding
 * padding for decrypt and including padding for encrypt. Capacity includes
 * the entire ciphertext length, even when decrypting.
 * Invalid arguments/short capacity do not modify output. Once processing
 * starts, an error wipes the output range that could have been written.
 * Encryption uses and wipes one temporary buffer through the supplied
 * allocator. Decryption makes no allocation. These functions provide no
 * authentication; callers authenticate their protocol separately. */
h2_pal_result_t h2_crypto_aes_cbc_pkcs7_encrypt(
    const h2_pal_crypto_api_t* crypto, const h2_pal_mem_api_t* memory,
    const uint8_t* key, size_t key_length, const uint8_t iv[16],
    const uint8_t* input, size_t input_length, uint8_t* output, size_t capacity,
    size_t* length);
h2_pal_result_t h2_crypto_aes_cbc_pkcs7_decrypt(
    const h2_pal_crypto_api_t* crypto, const uint8_t* key, size_t key_length,
    const uint8_t iv[16], const uint8_t* input, size_t input_length,
    uint8_t* output, size_t capacity, size_t* length);

/** RFC 4226 HOTP with an unsigned 64-bit counter and 6..9 decimal digits.
 * Uses the injected HMAC-SHA1 operation and wipes its temporary digest.
 * A nonempty key and non-NULL output are required. The numeric output has
 * no formatting or terminator; callers provide leading zeros for display.
 * Errors preserve output. No time, storage or I/O operation is performed. */
h2_pal_result_t h2_crypto_hotp_sha1(const h2_pal_crypto_api_t* crypto,
                                    const uint8_t* key, size_t key_length,
                                    uint64_t counter, unsigned digits,
                                    uint32_t* output);

typedef struct h2_crypto_md5_aes_keyset h2_crypto_md5_aes_keyset_t;

typedef struct h2_crypto_key_material {
  const uint8_t* data;
  size_t length;
} h2_crypto_key_material_t;

/** Prepare legacy AES-CBC/PKCS#7 keys from caller-supplied material. Each
 * key and IV is the first 16 lowercase hexadecimal characters of MD5(material),
 * without NUL. The keyset copies only derived bytes, borrows memory/crypto
 * until close and retains no material pointers. All allocations use memory.
 * The caller serializes access and closes only after operations return.
 * out is set to NULL on every error. No provider is selected here. */
h2_pal_result_t h2_crypto_md5_aes_keyset_open(
    const h2_pal_mem_api_t* memory, const h2_pal_crypto_api_t* crypto,
    const h2_crypto_key_material_t* materials, size_t count,
    h2_crypto_md5_aes_keyset_t** out);

/** Encrypt/decrypt with one prepared key. Output and failure semantics follow
 * the AES-CBC/PKCS#7 functions above. The index must be less than count
 * supplied at open. Output is raw ciphertext/plaintext; no text encoding is
 * performed.
 */
h2_pal_result_t h2_crypto_md5_aes_keyset_encrypt(
    h2_crypto_md5_aes_keyset_t* keys, size_t index, const uint8_t* input,
    size_t input_length, uint8_t* output, size_t capacity, size_t* length);
h2_pal_result_t h2_crypto_md5_aes_keyset_decrypt(
    h2_crypto_md5_aes_keyset_t* keys, size_t index, const uint8_t* input,
    size_t input_length, uint8_t* output, size_t capacity, size_t* length);

/** Wipe derived keys and free the keyset. NULL is safe. */
void h2_crypto_md5_aes_keyset_close(h2_crypto_md5_aes_keyset_t* keys);

#ifdef __cplusplus
}
#endif
#endif
