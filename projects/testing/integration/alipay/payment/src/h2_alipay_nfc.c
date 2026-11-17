#include "h2_alipay_nfc.h"

#include <stdbool.h>
#include <string.h>

h2_pal_result_t h2_alipay_nfc_encode_text_reply(void* user, const uint8_t* data,
                                                size_t length, uint8_t* ndef,
                                                size_t capacity,
                                                size_t* ndef_length) {
  (void)user;
  if (ndef_length == NULL) return H2_PAL_ERR_INVALID_ARG;
  *ndef_length = 0u;
  if (data == NULL || ndef == NULL || length == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  if (length > H2_ALIPAY_NFC_MESSAGE_CAPACITY) return H2_PAL_ERR_NO_SPACE;
  size_t payload = length + 3u;
  bool short_record = payload <= 255u;
  size_t header = short_record ? 4u : 7u;
  size_t total = header + payload;
  if (total > H2_ALIPAY_NFC_MESSAGE_CAPACITY || total > capacity)
    return H2_PAL_ERR_NO_SPACE;
  for (size_t index = 0u; index < length; ++index)
    if (data[index] < 0x20u || data[index] > 0x7eu) return H2_PAL_ERR_FORMAT;
  memmove(ndef + header + 3u, data, length);
  ndef[0] = short_record ? 0xd1u : 0xc1u;
  ndef[1] = 1u;
  if (short_record) {
    ndef[2] = (uint8_t)payload;
  } else {
    ndef[2] = (uint8_t)(payload >> 24u);
    ndef[3] = (uint8_t)(payload >> 16u);
    ndef[4] = (uint8_t)(payload >> 8u);
    ndef[5] = (uint8_t)payload;
  }
  ndef[header - 1u] = 'T';
  ndef[header] = 2u;
  ndef[header + 1u] = 'e';
  ndef[header + 2u] = 'n';
  *ndef_length = total;
  return H2_PAL_OK;
}
