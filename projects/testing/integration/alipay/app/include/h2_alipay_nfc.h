#ifndef H2_ALIPAY_NFC_H
#define H2_ALIPAY_NFC_H

#include "core/h2_pal_errors.h"

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

#ifdef __cplusplus
}
#endif

#endif
