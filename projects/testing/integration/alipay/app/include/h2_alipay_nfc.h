#ifndef H2_ALIPAY_NFC_H
#define H2_ALIPAY_NFC_H

#include "core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_ALIPAY_NFC_MESSAGE_CAPACITY 512u

/** Encode exactly 39 SDK reply bytes as a 46-byte UTF-8 Text/en record.
 * Copy the reply as supplied, excluding tag TLVs. Buffers are borrowed for
 * this call. Failure preserves ndef and sets *ndef_length to zero. */
h2_pal_result_t h2_alipay_nfc_encode_wallet_reply(void* user,
                                                  const uint8_t* data,
                                                  size_t length, uint8_t* ndef,
                                                  size_t capacity,
                                                  size_t* ndef_length);

#ifdef __cplusplus
}
#endif

#endif
