#include "h2_alipay_nfc.h"

#include <string.h>

h2_pal_result_t h2_alipay_nfc_encode_wallet_reply(void* user,
                                                  const uint8_t* data,
                                                  size_t length, uint8_t* ndef,
                                                  size_t capacity,
                                                  size_t* ndef_length) {
  (void)user;
  static const uint8_t header[] = {0xd1u, 0x01u, 0x2au, 'T', 2u, 'e', 'n'};
  if (ndef_length == NULL) return H2_PAL_ERR_INVALID_ARG;
  *ndef_length = 0u;
  if (data == NULL || ndef == NULL || length == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  if (length != 39u) return H2_PAL_ERR_FORMAT;
  if (capacity < sizeof(header) + length) return H2_PAL_ERR_NO_SPACE;
  memmove(ndef + sizeof(header), data, length);
  memcpy(ndef, header, sizeof(header));
  *ndef_length = sizeof(header) + length;
  return H2_PAL_OK;
}
