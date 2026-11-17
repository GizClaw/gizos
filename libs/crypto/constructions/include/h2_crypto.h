#ifndef H2_CRYPTO_H
#define H2_CRYPTO_H

#include "os/h2_pal_crypto.h"
#include "os/h2_pal_mem.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Portable cryptographic constructions over caller-injected Crypto PAL.
 * No concrete provider is selected. Provider and allocator APIs stay borrowed
 * for each synchronous call. Missing provider operations report UNSUPPORTED.
 * Standalone digests belong to their algorithm packages; raw provider
 * operations use the public Crypto PAL directly. */

/** Compare all bytes without an early exit based on their values. Empty
 * sequences are equal; NULL is otherwise invalid and returns false. */
bool h2_crypto_bytes_equal(const uint8_t* first, const uint8_t* second,
                           size_t length);

/** ISO/IEC 9797-1 algorithm 3 retail MAC, padding method 2, zero initial
 * chaining value and a 16-byte K1||K2 key. Returns the full 8-byte MAC; wire
 * truncation is caller-owned. Empty message permits NULL. Synchronous, no
 * allocation, borrowed inputs, output zeroed on failure. Reuses injected
 * DES/EDE2 operations instead of implementing a cipher. */
h2_pal_result_t h2_crypto_iso9797_alg3_mac(const h2_pal_crypto_api_t* crypto,
                                           const uint8_t key[16],
                                           const uint8_t* message,
                                           size_t length, uint8_t output[8]);

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

#ifdef __cplusplus
}
#endif
#endif
