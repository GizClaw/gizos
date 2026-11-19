#ifndef H2_ALIPAY_TAP_H
#define H2_ALIPAY_TAP_H

#include "h2_alipay_client.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  H2_ALIPAY_NFC_FRAME_DATA_MAX = 128u,
  H2_ALIPAY_NFC_FRAME_HEADER_SIZE = 4u,
  H2_ALIPAY_NFC_FRAME_MAX =
      H2_ALIPAY_NFC_FRAME_HEADER_SIZE + H2_ALIPAY_NFC_FRAME_DATA_MAX,
  H2_ALIPAY_NFC_REPLY_MAX = H2_ALIPAY_NFC_FRAME_DATA_MAX * 255u
};

typedef struct h2_alipay_nfc_capabilities {
  bool fast_read;
  bool private_write;
} h2_alipay_nfc_capabilities_t;
/** Decode the delivered tag header profile without I/O. Accept a flattened UID
 * or complete 5-byte ISO14443-A cascade blocks including CT/BCC. Unknown
 * manufacturers or invalid capability checksums yield no optional capabilities.
 */
h2_pal_result_t h2_alipay_nfc_capabilities(const uint8_t* uid,
                                           size_t uid_length,
                                           const uint8_t* header,
                                           size_t header_length,
                                           h2_alipay_nfc_capabilities_t* out);
/** Encode one 1-based private-write frame, carrying at most 128 unchanged
 * bytes. Input is borrowed; output is caller-owned. No NDEF or text encoding is
 * added. */
h2_pal_result_t h2_alipay_nfc_private_frame(const uint8_t* reply, size_t length,
                                            size_t frame_index, uint8_t* out,
                                            size_t capacity,
                                            size_t* out_length);
/** Full-byte ACK only. bits excludes parity; crc_valid describes retained
 * CRC-A. */
h2_pal_result_t h2_alipay_nfc_private_ack(const uint8_t* reply, size_t bits,
                                          bool crc_valid);

typedef struct h2_alipay_tap h2_alipay_tap_t;

enum { H2_ALIPAY_TAP_ERR_TOKEN = -2001 };

typedef struct h2_alipay_tap_payment {
  uint8_t type;
  uint8_t coupon_status;
  uint32_t order_amount_fen;
  uint32_t real_amount_fen;
} h2_alipay_tap_payment_t;

typedef struct h2_alipay_tap_tag {
  uint8_t type;
  char token[64];
  bool flags_present;
  bool trusted;
} h2_alipay_tap_tag_t;

/** Decode a bounded raw URL or an NDEF URI record, without I/O or state
 * changes. Types 2/3/4 identify standard/VIP/cabinet tokens; 5 identifies
 * online content. The caller owns out, which is cleared before parsing. Unknown
 * or truncated records fail explicitly. Input and config are borrowed for this
 * call only. */
h2_pal_result_t h2_alipay_tap_decode_tag(
    const h2_alipay_client_config_t* config, const uint8_t* data, size_t length,
    h2_alipay_tap_tag_t* out);

/** Open a caller-serialized, blocking tap context. Copies config and borrows
 * its dependencies until close. It creates no hardware activity or tasks. */
h2_pal_result_t h2_alipay_tap_open(const h2_alipay_client_config_t* config,
                                   h2_alipay_tap_t** out);

/** Wipe and free after every operation returns. NULL is safe. */
void h2_alipay_tap_close(h2_alipay_tap_t* tap);

/** Install the current locally generated payment context after an NFC/CPU
 * handoff. Text lengths exclude NUL; token/squeeze are at most 31 bytes and
 * paycode at most 31 bytes. Replaces/wipes the preceding context.
 * This does not verify a server payment or perform a physical write. */
h2_pal_result_t h2_alipay_tap_set_context(h2_alipay_tap_t* tap,
                                          const char* token,
                                          const char* squeeze,
                                          const char* paycode);

/** Verify an exact 27-byte BLE service-data result against the current
 * squeeze token and bound UID. Zeros out on every error. Uses Crypto PAL MD5;
 * no radio, SDK, network or device dependency is selected. */
h2_pal_result_t h2_alipay_tap_parse_result(h2_alipay_tap_t* tap,
                                           const uint8_t* data, size_t length,
                                           h2_alipay_tap_payment_t* out);

/** Get the stored plain payer code for an exact current token. Missing context
 * or a mismatched token returns NOT_FOUND and preserves output/length.
 * Output excludes NUL; exact capacity is sufficient. */
h2_pal_result_t h2_alipay_tap_plain_code(h2_alipay_tap_t* tap,
                                         const uint8_t* token,
                                         size_t token_length, uint8_t* out,
                                         size_t capacity, size_t* length);

/** Read or replace the local write-result byte for the exact current token.
 * No physical write occurs. A fresh context starts at zero. Nonmatching tokens
 * return NOT_FOUND and preserve the caller's value. Calls are serialized. */
h2_pal_result_t h2_alipay_tap_write_result(h2_alipay_tap_t* tap,
                                           const uint8_t* token,
                                           size_t token_length, bool write,
                                           uint8_t* value);

/** Copy the last local payer code and NFC token, including NUL, for a result
 * query. Empty contexts produce empty strings. Outputs are distinct owned
 * 32-byte buffers. No server payment is inferred from this local snapshot. */
h2_pal_result_t h2_alipay_tap_payment_context(h2_alipay_tap_t* tap,
                                              char code[32], char token[32]);

/** Generate the delivered deterministic debug token with Crypto PAL MD5.
 * Writes a NUL-terminated string of at most 127 bytes only on success.
 * It does not replace a payment context or issue a request. */
h2_pal_result_t h2_alipay_tap_debug_token(const h2_pal_crypto_api_t* crypto,
                                          char out[128]);

/** Build the original coupon URL with the current bound TID. Capacity includes
 * NUL; success length excludes it. Failures preserve output and length. */
h2_pal_result_t h2_alipay_tap_coupon_url(h2_alipay_tap_t* tap, uint8_t* out,
                                         size_t capacity, size_t* length);

#ifdef __cplusplus
}
#endif
#endif
