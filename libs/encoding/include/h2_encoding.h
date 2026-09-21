#ifndef H2_ENCODING_H
#define H2_ENCODING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Algorithm family used by an encoding descriptor.
 * The kind fixes the radix and grouping; the descriptor alphabet names the
 * symbols. Hex uses 16 symbols, base32 32, base64 64, and base85 85.
 */
typedef enum h2_encoding_kind {
    H2_ENCODING_KIND_HEX = 1,    /**< 4 bits per symbol, 2 symbols per byte. */
    H2_ENCODING_KIND_BASE32 = 2, /**< 5 bits per symbol, 8-symbol blocks. */
    H2_ENCODING_KIND_BASE64 = 3, /**< 6 bits per symbol, 4-symbol blocks. */
    H2_ENCODING_KIND_BASE85 = 4, /**< 4-byte groups as 5 big-endian base-85 digits. */
} h2_encoding_kind_t;

/**
 * @brief Result of an encoding operation.
 */
typedef enum h2_encoding_err {
    H2_ENCODING_OK = 0,                /**< Operation completed. */
    H2_ENCODING_ERR_INVALID_ARG = -1,  /**< Bad descriptor, NULL pointer, or size overflow. */
    H2_ENCODING_ERR_NO_SPACE = -2,     /**< Output capacity is smaller than the result. */
    H2_ENCODING_ERR_CORRUPT = -3,      /**< Input is not a canonical encoding. */
} h2_encoding_err_t;

/**
 * @brief Immutable encoding descriptor, the C form of Go's Encoding values.
 * Callers use the predefined descriptors or fill one themselves; copying a
 * predefined descriptor and changing `padding` is the equivalent of Go's
 * WithPadding. A descriptor is valid when:
 * - `alphabet` is a NUL-terminated string of exactly radix distinct bytes in
 *   0x21..0x7e (printable ASCII without space), indexed by digit value.
 * - `padding` is '\0' (no padding) or a printable byte outside the alphabet;
 *   only base32 and base64 may set it.
 * - `zero_group` is '\0' or a printable byte outside the alphabet and
 *   different from `padding`; only base85 may set it, and it then stands for
 *   one all-zero 4-byte group, as 'z' does in Ascii85.
 * The library borrows `alphabet` for each call and never retains it.
 */
typedef struct h2_encoding {
    h2_encoding_kind_t kind; /**< Algorithm family. */
    const char *alphabet;    /**< Borrowed digit alphabet, radix bytes plus NUL. */
    char padding;            /**< Block padding byte, or '\0' for none. */
    char zero_group;         /**< Base85 all-zero group shortcut, or '\0' for none. */
} h2_encoding_t;

/** @brief Lowercase hex, RFC 4648 section 8; decoding also accepts uppercase. */
extern const h2_encoding_t h2_encoding_hex;
/** @brief RFC 4648 section 6 base32 with '=' padding. */
extern const h2_encoding_t h2_encoding_base32_std;
/** @brief RFC 4648 section 7 extended-hex base32 with '=' padding. */
extern const h2_encoding_t h2_encoding_base32_hex;
/** @brief RFC 4648 section 4 base64 with '=' padding. */
extern const h2_encoding_t h2_encoding_base64_std;
/** @brief RFC 4648 section 5 URL and filename safe base64 with '=' padding. */
extern const h2_encoding_t h2_encoding_base64_url;
/** @brief RFC 4648 section 4 base64 without padding. */
extern const h2_encoding_t h2_encoding_base64_raw_std;
/** @brief RFC 4648 section 5 base64 without padding. */
extern const h2_encoding_t h2_encoding_base64_raw_url;
/** @brief Adobe Ascii85 digits '!'..'u' with the 'z' zero group, no <~ ~> frame. */
extern const h2_encoding_t h2_encoding_base85_ascii85;
/** @brief RFC 1924 base85 alphabet, byte-compatible with Python base64.b85encode. */
extern const h2_encoding_t h2_encoding_base85_rfc1924;

/**
 * @brief Check whether a descriptor satisfies the h2_encoding_t rules.
 * No allocation, retention, or waiting occurs; concurrent calls are safe.
 * @param[in] enc Borrowed descriptor, or NULL; valid for the call.
 * @return true for a usable descriptor, false for NULL or any rule violation.
 */
bool h2_encoding_is_valid(const h2_encoding_t *enc);

/**
 * @brief Report the output size needed to encode src_len bytes.
 * The size is exact except for base85 with a zero group, where it is an upper
 * bound; h2_encoding_encode reports the exact size. No NUL terminator is
 * counted.
 * @param[in] enc Borrowed descriptor; valid for the call.
 * @param[in] src_len Number of input bytes.
 * @param[out] out_len Caller-owned result storage; valid for the call.
 * @return H2_ENCODING_OK, or H2_ENCODING_ERR_INVALID_ARG for an invalid
 *         descriptor, NULL out_len, or a size that does not fit in size_t.
 */
h2_encoding_err_t h2_encoding_max_encoded_len(const h2_encoding_t *enc,
                                              size_t src_len, size_t *out_len);

/**
 * @brief Report an upper bound on the bytes decoded from src_len symbols.
 * @param[in] enc Borrowed descriptor; valid for the call.
 * @param[in] src_len Number of encoded input bytes.
 * @param[out] out_len Caller-owned result storage; valid for the call.
 * @return H2_ENCODING_OK, or H2_ENCODING_ERR_INVALID_ARG for an invalid
 *         descriptor, NULL out_len, or a size that does not fit in size_t.
 */
h2_encoding_err_t h2_encoding_max_decoded_len(const h2_encoding_t *enc,
                                              size_t src_len, size_t *out_len);

/**
 * @brief Encode bytes into caller-provided text storage.
 * Output is the canonical encoding without a NUL terminator. Encoding a
 * stream in chunks matches one-shot output when every chunk except the last
 * is a multiple of the block size (3 bytes for base64, 5 for base32, 4 for
 * base85, any size for hex). dst is written only on H2_ENCODING_OK. On
 * H2_ENCODING_ERR_NO_SPACE, *out_len is the exact size required, so a NULL
 * dst with zero capacity queries the size. No allocation or retention occurs.
 * @param[in] enc Borrowed descriptor; valid for the call.
 * @param[in] src Borrowed input bytes, or NULL when src_len is 0.
 * @param[in] src_len Number of input bytes.
 * @param[out] dst Caller-owned output, or NULL when dst_cap is 0; must not
 *                 overlap src.
 * @param[in] dst_cap Capacity of dst in bytes.
 * @param[out] out_len Caller-owned storage for the written or required size.
 * @return H2_ENCODING_OK, H2_ENCODING_ERR_NO_SPACE, or
 *         H2_ENCODING_ERR_INVALID_ARG (out_len is then left unchanged).
 */
h2_encoding_err_t h2_encoding_encode(const h2_encoding_t *enc,
                                     const uint8_t *src, size_t src_len,
                                     char *dst, size_t dst_cap,
                                     size_t *out_len);

/**
 * @brief Decode canonical text into caller-provided byte storage.
 * Only the exact output of h2_encoding_encode is accepted: whitespace, NUL,
 * symbols outside the alphabet, misplaced or missing padding, incomplete
 * final groups, nonzero trailing bits, base85 groups above 0xffffffff,
 * non-canonical base85 final groups, and an all-zero base85 group written as
 * digits when a zero group byte is configured are all corrupt. Hex also
 * accepts the other ASCII case of an alphabet letter when that byte is not
 * itself in the alphabet. dst is written only on H2_ENCODING_OK. On
 * H2_ENCODING_ERR_NO_SPACE, *out_len is the exact size required; on
 * H2_ENCODING_ERR_CORRUPT, it is the offset of the first offending input
 * byte, or src_len when the input ends early.
 * @param[in] enc Borrowed descriptor; valid for the call.
 * @param[in] src Borrowed encoded text, or NULL when src_len is 0; no NUL
 *                terminator is required.
 * @param[in] src_len Number of encoded bytes.
 * @param[out] dst Caller-owned output, or NULL when dst_cap is 0; must not
 *                 overlap src.
 * @param[in] dst_cap Capacity of dst in bytes.
 * @param[out] out_len Caller-owned storage for the size or error offset.
 * @return H2_ENCODING_OK, H2_ENCODING_ERR_NO_SPACE, H2_ENCODING_ERR_CORRUPT,
 *         or H2_ENCODING_ERR_INVALID_ARG (out_len is then left unchanged).
 */
h2_encoding_err_t h2_encoding_decode(const h2_encoding_t *enc,
                                     const char *src, size_t src_len,
                                     uint8_t *dst, size_t dst_cap,
                                     size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
