#include "h2_alipay_nfc.h"

#include <assert.h>
#include <string.h>

int main(void) {
  uint8_t reply[40];
  memset(reply, 'x', sizeof(reply));
  reply[12] = 0u;
  const uint8_t header[] = {0xd1u, 0x01u, 0x2au, 'T', 2u, 'e', 'n'};
  uint8_t message[47];
  memset(message, 0xa5, sizeof(message));
  size_t length = 0u;
  assert(h2_alipay_nfc_encode_wallet_reply(NULL, reply, 39u, message,
                                           sizeof(message), &length) == 0);
  assert(length == 46u && message[length] == 0xa5u &&
         memcmp(message, header, sizeof(header)) == 0 &&
         memcmp(message + sizeof(header), reply, 39u) == 0);
  uint8_t expected[sizeof(message)];
  memcpy(expected, message, sizeof(expected));
  assert(h2_alipay_nfc_encode_wallet_reply(NULL, reply, 38u, message,
                                           sizeof(message),
                                           &length) == H2_PAL_ERR_FORMAT);
  assert(length == 0u && memcmp(message, expected, sizeof(message)) == 0);
  assert(h2_alipay_nfc_encode_wallet_reply(NULL, reply, 40u, message,
                                           sizeof(message),
                                           &length) == H2_PAL_ERR_FORMAT);
  assert(length == 0u && memcmp(message, expected, sizeof(message)) == 0);
  assert(h2_alipay_nfc_encode_wallet_reply(NULL, reply, 39u, message, 45u,
                                           &length) == H2_PAL_ERR_NO_SPACE);
  assert(length == 0u && memcmp(message, expected, sizeof(message)) == 0);
  assert(h2_alipay_nfc_encode_wallet_reply(NULL, NULL, 39u, message,
                                           sizeof(message),
                                           &length) == H2_PAL_ERR_INVALID_ARG);
  assert(length == 0u && memcmp(message, expected, sizeof(message)) == 0);
  return 0;
}
