#include "h2_alipay_nfc.h"

#include <assert.h>
#include <string.h>

static void unchanged(const uint8_t* bytes, size_t length) {
  for (size_t index = 0u; index < length; ++index)
    assert(bytes[index] == 0xa5u);
}

static void test_inspect_reply(void) {
  uint8_t source[253], message[512];
  memset(source, 'x', sizeof(source));
  size_t length = 0u;
  h2_alipay_nfc_text_diagnostics_t result;
  assert(h2_alipay_nfc_encode_text_reply(NULL, source, 39u, message,
                                         sizeof(message), &length) == 0);
  assert(h2_alipay_nfc_inspect_text_reply(source, 39u, message, length,
                                          &result) == 0);
  assert(result.tnf == 1u && result.single_record && result.short_record &&
         result.type_text && result.utf8 && result.language_en &&
         result.payload_offset == 4u && result.payload_length == 42u &&
         result.language_length == 2u && result.text_offset == 7u &&
         result.text_length == 39u && result.exact_length &&
         result.text_matches);
  source[0] = 'y';
  assert(h2_alipay_nfc_inspect_text_reply(source, 39u, message, length,
                                          &result) == 0);
  assert(!result.text_matches);
  source[0] = 'x';
  for (size_t truncated = 0u; truncated < length; ++truncated)
    assert(h2_alipay_nfc_inspect_text_reply(source, 39u, message, truncated,
                                            &result) == H2_PAL_ERR_FORMAT);
  assert(h2_alipay_nfc_inspect_text_reply(source, 39u, message, length + 1u,
                                          &result) == H2_PAL_ERR_FORMAT);
  assert(!result.exact_length);
  message[3] = 'U';
  assert(h2_alipay_nfc_inspect_text_reply(source, 39u, message, length,
                                          &result) == H2_PAL_ERR_FORMAT);
  message[3] = 'T';
  message[4] = 63u;
  assert(h2_alipay_nfc_inspect_text_reply(source, 39u, message, length,
                                          &result) == H2_PAL_ERR_FORMAT);
  message[4] = 0x82u;
  assert(h2_alipay_nfc_inspect_text_reply(source, 39u, message, length,
                                          &result) == 0);
  assert(!result.utf8 && !result.text_matches);
  assert(h2_alipay_nfc_encode_text_reply(NULL, source, sizeof(source), message,
                                         sizeof(message), &length) == 0);
  assert(h2_alipay_nfc_inspect_text_reply(source, sizeof(source), message,
                                          length, &result) == 0);
  assert(!result.short_record && result.payload_offset == 7u &&
         result.payload_length == 256u && result.text_offset == 10u &&
         result.text_matches);
  assert(h2_alipay_nfc_inspect_text_reply(NULL, 0u, message, length, &result) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_alipay_nfc_inspect_text_reply(source, sizeof(source), message,
                                          length,
                                          NULL) == H2_PAL_ERR_INVALID_ARG);
}

static void test_raw_reply(void) {
  /* Synthetic original-SDK callback: plain protocol text, not a complete
   * Text record. Test both transport profiles against the same exact bytes. */
  const uint8_t reply[] = "NW310bNpvwudy1Gy9zE7Cu5/Rc2rT_8z9o_0117";
  uint8_t output[512], text[512];
  memset(output, 0xa5, sizeof(output));
  size_t length = 0u, text_length = 0u;
  assert(sizeof(reply) - 1u == 39u);
  assert(h2_alipay_nfc_copy_raw_reply(NULL, reply, 39u, output, sizeof(output),
                                      &length) == 0);
  assert(length == 39u && memcmp(output, reply, length) == 0 &&
         output[length] == 0xa5u);
  assert(h2_alipay_nfc_encode_text_reply(NULL, reply, 39u, text, sizeof(text),
                                         &text_length) == 0);
  assert(text_length == length + 7u && memcmp(text + 7u, output, length) == 0);
  h2_alipay_nfc_text_diagnostics_t layout;
  assert(h2_alipay_nfc_inspect_text_reply(reply, 39u, output, length,
                                          &layout) == H2_PAL_ERR_FORMAT);
  assert(h2_alipay_nfc_inspect_text_reply(reply, 39u, text, text_length,
                                          &layout) == 0 &&
         layout.text_matches);
  /* Passthrough is binary-safe and supports overlapping borrowed buffers. */
  const uint8_t binary[] = {0u, 255u, 0xd1u, 0x80u};
  assert(h2_alipay_nfc_copy_raw_reply(NULL, binary, sizeof(binary), output,
                                      sizeof(output), &length) == 0);
  assert(length == sizeof(binary) && memcmp(output, binary, length) == 0);
  assert(h2_alipay_nfc_copy_raw_reply(NULL, output, length, output + 1u,
                                      sizeof(output) - 1u, &length) == 0);
  assert(memcmp(output + 1u, binary, sizeof(binary)) == 0);
  memset(output, 0xa5, sizeof(output));
  assert(h2_alipay_nfc_copy_raw_reply(NULL, reply, 39u, output, 38u, &length) ==
         H2_PAL_ERR_NO_SPACE);
  assert(length == 0u);
  unchanged(output, sizeof(output));
  uint8_t large[513] = {0};
  assert(h2_alipay_nfc_copy_raw_reply(NULL, large, sizeof(large), output,
                                      sizeof(output),
                                      &length) == H2_PAL_ERR_NO_SPACE);
  unchanged(output, sizeof(output));
  assert(h2_alipay_nfc_copy_raw_reply(NULL, NULL, 39u, output, sizeof(output),
                                      &length) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_alipay_nfc_copy_raw_reply(NULL, reply, 0u, output, sizeof(output),
                                      &length) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_alipay_nfc_copy_raw_reply(NULL, reply, 39u, NULL, sizeof(output),
                                      &length) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_alipay_nfc_copy_raw_reply(NULL, reply, 39u, output, sizeof(output),
                                      NULL) == H2_PAL_ERR_INVALID_ARG);
}

int main(void) {
  test_inspect_reply();
  test_raw_reply();
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
