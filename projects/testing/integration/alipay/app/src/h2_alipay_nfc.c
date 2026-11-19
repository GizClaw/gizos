#include "h2_alipay_nfc.h"

#include <stdbool.h>
#include <string.h>

static h2_pal_result_t encode_text(const uint8_t* data, size_t length,
                                   uint8_t* ndef, size_t capacity,
                                   size_t* ndef_length, bool wallet_reply) {
  if (ndef_length == NULL) return H2_PAL_ERR_INVALID_ARG;
  *ndef_length = 0u;
  if (data == NULL || ndef == NULL || length == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  if (wallet_reply && length != 39u) return H2_PAL_ERR_FORMAT;
  if (length > H2_ALIPAY_NFC_MESSAGE_CAPACITY) return H2_PAL_ERR_NO_SPACE;
  size_t payload = length + 3u;
  bool short_record = payload <= 255u;
  size_t header = short_record ? 4u : 7u;
  size_t total = header + payload;
  if (total > H2_ALIPAY_NFC_MESSAGE_CAPACITY || total > capacity)
    return H2_PAL_ERR_NO_SPACE;
  if (!wallet_reply) {
    for (size_t index = 0u; index < length; ++index)
      if (data[index] < 0x20u || data[index] > 0x7eu) return H2_PAL_ERR_FORMAT;
  }
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

h2_pal_result_t h2_alipay_nfc_encode_text_reply(void* user, const uint8_t* data,
                                                size_t length, uint8_t* ndef,
                                                size_t capacity,
                                                size_t* ndef_length) {
  (void)user;
  return encode_text(data, length, ndef, capacity, ndef_length, false);
}

h2_pal_result_t h2_alipay_nfc_encode_wallet_reply(void* user,
                                                  const uint8_t* data,
                                                  size_t length, uint8_t* ndef,
                                                  size_t capacity,
                                                  size_t* ndef_length) {
  (void)user;
  return encode_text(data, length, ndef, capacity, ndef_length, true);
}

h2_pal_result_t h2_alipay_nfc_copy_raw_reply(void* user, const uint8_t* data,
                                             size_t length, uint8_t* out,
                                             size_t capacity,
                                             size_t* out_length) {
  (void)user;
  if (out_length == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_length = 0u;
  if (data == NULL || out == NULL || length == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  if (length > H2_ALIPAY_NFC_MESSAGE_CAPACITY || length > capacity)
    return H2_PAL_ERR_NO_SPACE;
  memmove(out, data, length);
  *out_length = length;
  return H2_PAL_OK;
}

h2_pal_result_t h2_alipay_nfc_inspect_text_reply(
    const uint8_t* data, size_t length, const uint8_t* ndef, size_t ndef_length,
    h2_alipay_nfc_text_diagnostics_t* out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  if (data == NULL || ndef == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (ndef_length < 3u || ndef_length > H2_ALIPAY_NFC_MESSAGE_CAPACITY)
    return H2_PAL_ERR_FORMAT;
  uint8_t flags = ndef[0];
  out->tnf = flags & 7u;
  out->single_record = (flags & 0xe0u) == 0xc0u;
  out->short_record = (flags & 0x10u) != 0u;
  size_t cursor = 2u, payload = 0u;
  if (out->short_record)
    payload = ndef[cursor++];
  else {
    if (ndef_length < 6u) return H2_PAL_ERR_FORMAT;
    payload = ((size_t)ndef[2] << 24u) | ((size_t)ndef[3] << 16u) |
              ((size_t)ndef[4] << 8u) | ndef[5];
    cursor += 4u;
  }
  size_t id_length = 0u;
  if ((flags & 8u) != 0u) {
    if (cursor == ndef_length) return H2_PAL_ERR_FORMAT;
    id_length = ndef[cursor++];
  }
  if (ndef[1] > ndef_length - cursor) return H2_PAL_ERR_FORMAT;
  out->type_text = ndef[1] == 1u && ndef[cursor] == 'T';
  cursor += ndef[1];
  if (id_length > ndef_length - cursor) return H2_PAL_ERR_FORMAT;
  cursor += id_length;
  out->payload_offset = cursor;
  out->payload_length = payload;
  if (payload > ndef_length - cursor) return H2_PAL_ERR_FORMAT;
  out->exact_length = payload == ndef_length - cursor;
  if (out->tnf != 1u || !out->single_record || !out->type_text ||
      !out->exact_length || payload == 0u)
    return H2_PAL_ERR_FORMAT;
  uint8_t status = ndef[cursor];
  out->utf8 = (status & 0x80u) == 0u;
  out->language_length = status & 0x3fu;
  if ((status & 0x40u) != 0u || out->language_length > payload - 1u)
    return H2_PAL_ERR_FORMAT;
  out->language_en =
      out->language_length == 2u && memcmp(ndef + cursor + 1u, "en", 2u) == 0;
  out->text_offset = cursor + 1u + out->language_length;
  out->text_length = payload - 1u - out->language_length;
  out->text_matches = out->utf8 && out->text_length == length &&
                      memcmp(ndef + out->text_offset, data, length) == 0;
  return H2_PAL_OK;
}
