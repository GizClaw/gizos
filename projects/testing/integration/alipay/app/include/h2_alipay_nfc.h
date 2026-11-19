#ifndef H2_ALIPAY_NFC_H
#define H2_ALIPAY_NFC_H

#include "core/h2_pal_errors.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_ALIPAY_NFC_MESSAGE_CAPACITY 512u

/** Encode printable ASCII application reply bytes as one NFC Forum UTF-8 Text
 * record with language "en". This explicit transport profile requires merchant
 * terminal qualification. It does not imply terminal-specific acceptance.
 * All buffers are borrowed for this synchronous worker call; user is unused.
 * Success copies a complete NDEF message, excluding tag TLVs. Failure preserves
 * ndef and clears *ndef_length. Output, input and length pointers are required;
 * capacity must cover the complete message, at most MESSAGE_CAPACITY bytes. */
h2_pal_result_t h2_alipay_nfc_encode_text_reply(void* user, const uint8_t* data,
                                                size_t length, uint8_t* ndef,
                                                size_t capacity,
                                                size_t* ndef_length);

/** Encode exactly 39 SDK reply bytes as a 46-byte UTF-8 Text/en record.
 * Copy the reply as supplied. Buffer and result contracts match the Text
 * encoder. */
h2_pal_result_t h2_alipay_nfc_encode_wallet_reply(void* user,
                                                  const uint8_t* data,
                                                  size_t length, uint8_t* ndef,
                                                  size_t capacity,
                                                  size_t* ndef_length);

/** Diagnostic passthrough for merchant A/B tests: copy callback bytes exactly,
 * without a Text record header, language metadata, encoding or terminator.
 * These bytes need not form a valid NDEF record. The tag writer still owns
 * its TLV container. All buffers are borrowed synchronously; user is unused.
 * Failure preserves out and clears *out_length. No merchant acceptance is
 * implied. This profile is selected explicitly by the test artifact entry. */
h2_pal_result_t h2_alipay_nfc_copy_raw_reply(void* user, const uint8_t* data,
                                             size_t length, uint8_t* out,
                                             size_t capacity,
                                             size_t* out_length);

/** Non-payload software handoff observations. These do not establish RF
 * delivery, tag persistence or merchant acceptance. */
typedef struct h2_alipay_nfc_text_diagnostics {
  unsigned tnf;
  bool single_record, short_record, type_text, utf8, language_en;
  bool exact_length, text_matches;
  size_t payload_offset, payload_length, language_length, text_offset,
      text_length;
} h2_alipay_nfc_text_diagnostics_t;

/** Inspect a complete single Text record and compare its text with borrowed
 * callback bytes. Does not change either input or perform I/O/allocation.
 * Clears out before validation. Malformed/truncated records return FORMAT;
 * structurally valid text mismatch is reported by text_matches, not an error.
 * All pointed-to bytes remain private and are never retained in out. */
h2_pal_result_t h2_alipay_nfc_inspect_text_reply(
    const uint8_t* data, size_t length, const uint8_t* ndef, size_t ndef_length,
    h2_alipay_nfc_text_diagnostics_t* out);

#ifdef __cplusplus
}
#endif

#endif
