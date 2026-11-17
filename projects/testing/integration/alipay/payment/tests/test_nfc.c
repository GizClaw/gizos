#include "h2_alipay_nfc.h"

#include <assert.h>
#include <string.h>

static void unchanged(const uint8_t* bytes, size_t length) {
  for (size_t index = 0u; index < length; ++index)
    assert(bytes[index] == 0xa5u);
}

int main(void) {
  const uint8_t reply[] = "NW310abcd";
  const uint8_t expected[] = {0xd1u, 1u,  12u, 'T', 2u,  'e', 'n', 'N',
                              'W',   '3', '1', '0', 'a', 'b', 'c', 'd'};
  uint8_t message[H2_ALIPAY_NFC_MESSAGE_CAPACITY];
  memset(message, 0xa5, sizeof(message));
  size_t length = 99u;
  assert(h2_alipay_nfc_encode_text_reply(NULL, reply, sizeof(reply) - 1u,
                                         message, sizeof(message),
                                         &length) == 0);
  assert(length == sizeof(expected) &&
         memcmp(message, expected, sizeof(expected)) == 0);
  assert(message[length] == 0xa5u);
  memset(message, 0xa5, sizeof(message));
  assert(h2_alipay_nfc_encode_text_reply(NULL, reply, sizeof(reply) - 1u,
                                         message, sizeof(expected) - 1u,
                                         &length) == H2_PAL_ERR_NO_SPACE);
  assert(length == 0u);
  unchanged(message, sizeof(message));
  const uint8_t malformed[] = {'N', 0u, 'W'};
  assert(h2_alipay_nfc_encode_text_reply(NULL, malformed, sizeof(malformed),
                                         message, sizeof(message),
                                         &length) == H2_PAL_ERR_FORMAT);
  unchanged(message, sizeof(message));
  uint8_t payload[503];
  memset(payload, 'x', sizeof(payload));
  assert(h2_alipay_nfc_encode_text_reply(NULL, payload, 252u, message,
                                         sizeof(message), &length) == 0);
  assert(length == 259u && message[0] == 0xd1u && message[2] == 255u &&
         memcmp(message + 7u, payload, 252u) == 0);
  assert(h2_alipay_nfc_encode_text_reply(NULL, payload, 253u, message,
                                         sizeof(message), &length) == 0);
  assert(length == 263u && message[0] == 0xc1u && message[4] == 1u &&
         message[5] == 0u && memcmp(message + 10u, payload, 253u) == 0);
  assert(h2_alipay_nfc_encode_text_reply(NULL, payload, 502u, message,
                                         sizeof(message), &length) == 0);
  assert(length == sizeof(message) && message[4] == 1u && message[5] == 249u &&
         memcmp(message + 10u, payload, 502u) == 0);
  memset(message, 0xa5, sizeof(message));
  assert(h2_alipay_nfc_encode_text_reply(NULL, payload, sizeof(payload),
                                         message, sizeof(message),
                                         &length) == H2_PAL_ERR_NO_SPACE);
  unchanged(message, sizeof(message));
  assert(h2_alipay_nfc_encode_text_reply(NULL, NULL, 1u, message,
                                         sizeof(message),
                                         &length) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_alipay_nfc_encode_text_reply(NULL, reply, 0u, message,
                                         sizeof(message),
                                         &length) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_alipay_nfc_encode_text_reply(NULL, reply, sizeof(reply) - 1u, NULL,
                                         sizeof(message),
                                         &length) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_alipay_nfc_encode_text_reply(NULL, reply, sizeof(reply) - 1u,
                                         message, sizeof(message),
                                         NULL) == H2_PAL_ERR_INVALID_ARG);
  return 0;
}
