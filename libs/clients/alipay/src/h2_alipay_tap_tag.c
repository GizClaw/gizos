#include "h2_alipay_tap.h"

#include "h2_alipay_client_internal.h"
#include "h2_alipay_tap_internal.h"
#include "h2_encoding.h"
#include "yyjson.h"
#include <string.h>

h2_pal_result_t h2_alipay_nfc_capabilities(const uint8_t* uid,
                                           size_t uid_length,
                                           const uint8_t* header,
                                           size_t header_length,
                                           h2_alipay_nfc_capabilities_t* out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = (h2_alipay_nfc_capabilities_t){0};
  if (uid == NULL || uid_length == 0u || header == NULL || header_length < 16u)
    return H2_PAL_ERR_INVALID_ARG;
  /* ISO14443-A raw cascade blocks include CT (0x88) and BCC. Existing
   * readers may retain those blocks instead of returning a flattened UID. */
  size_t manufacturer_offset =
      ((uid_length == 10u || uid_length == 15u) && uid[0] == 0x88u) ? 1u : 0u;
  if (uid[manufacturer_offset] != 0x1du || header[0] != 0x1du) return H2_PAL_OK;
  uint8_t id = header[9];
  if (id == 0xa3u || id == 0xa6u) {
    out->fast_read = true;
  } else {
    unsigned ones = 0u;
    for (unsigned bit = 3u; bit < 8u; ++bit) ones += (id >> bit) & 1u;
    if ((id & 7u) == ones) {
      out->fast_read = (id & 0x20u) != 0u;
      out->private_write = (id & 0x40u) != 0u;
    }
  }
  return H2_PAL_OK;
}
h2_pal_result_t h2_alipay_nfc_private_frame(const uint8_t* reply, size_t length,
                                            size_t frame_index, uint8_t* out,
                                            size_t capacity,
                                            size_t* out_length) {
  if (out_length == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_length = 0u;
  if (reply == NULL || out == NULL || length == 0u ||
      length > H2_ALIPAY_NFC_REPLY_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  size_t frames = (length + H2_ALIPAY_NFC_FRAME_DATA_MAX - 1u) /
                  H2_ALIPAY_NFC_FRAME_DATA_MAX;
  if (frame_index == 0u || frame_index > frames) return H2_PAL_ERR_INVALID_ARG;
  size_t offset = (frame_index - 1u) * H2_ALIPAY_NFC_FRAME_DATA_MAX;
  size_t bytes = length - offset;
  if (bytes > H2_ALIPAY_NFC_FRAME_DATA_MAX)
    bytes = H2_ALIPAY_NFC_FRAME_DATA_MAX;
  if (capacity < H2_ALIPAY_NFC_FRAME_HEADER_SIZE + bytes)
    return H2_PAL_ERR_NO_SPACE;
  memmove(out + H2_ALIPAY_NFC_FRAME_HEADER_SIZE, reply + offset, bytes);
  out[0] = 0xa3u;
  out[1] = (uint8_t)frames;
  out[2] = (uint8_t)frame_index;
  out[3] = (uint8_t)bytes;
  *out_length = H2_ALIPAY_NFC_FRAME_HEADER_SIZE + bytes;
  return H2_PAL_OK;
}
h2_pal_result_t h2_alipay_nfc_private_ack(const uint8_t* reply, size_t bits,
                                          bool crc_valid) {
  if (reply == NULL) return H2_PAL_ERR_INVALID_ARG;
  return ((bits == 8u || (bits == 24u && crc_valid)) && reply[0] == 0x0au)
             ? H2_PAL_OK
             : H2_PAL_ERR_IO;
}

typedef struct tag_workspace {
  char url[4096], scheme[1024], parameters[512], value[512];
} tag_workspace_t;

static unsigned tag_hex(unsigned char byte) {
  if (byte >= '0' && byte <= '9') return byte - '0';
  if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10u;
  if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10u;
  return 16u;
}

static size_t tag_split(char* text, char delimiter, char** parts,
                        size_t capacity) {
  size_t count = 0u;
  for (;;) {
    if (count < capacity) parts[count] = text;
    ++count;
    char* next = strchr(text, delimiter);
    if (next == NULL) return count;
    *next = '\0';
    text = next + 1;
  }
}

static int tag_token(h2_alipay_tap_tag_t* out, const char* token) {
  size_t length = strlen(token);
  if (length >= sizeof(out->token)) return H2_PAL_ERR_NO_SPACE;
  memcpy(out->token, token, length + 1u);
  return H2_PAL_OK;
}

static void tag_flags(h2_alipay_tap_tag_details_t* details, const char* flags) {
  h2_alipay_tap_tag_t* out = &details->tag;
  static const uint8_t positions[] = {
      3,  7,  1,  9,  5,  14, 2,  11, 8,  15, 0, 6,  13, 4,  10, 12, 2,
      8,  1,  9,  5,  11, 15, 4,  6,  0,  13, 7, 10, 3,  14, 1,  12, 5,
      8,  2,  11, 6,  9,  0,  13, 4,  7,  15, 1, 10, 3,  12, 6,  8,  2,
      14, 5,  9,  0,  11, 13, 7,  4,  1,  15, 3, 6,  10, 8,  12, 5,  0,
      9,  2,  11, 14, 7,  1,  6,  3,  13, 4,  8, 10, 15, 2,  9,  5,  0,
      7,  12, 1,  11, 3,  6,  14, 8,  4,  10, 2, 5,  9,  13, 7};
  size_t length = strlen(flags);
  if (length == 0u || length >= 64u) return;
  uint8_t last = 0u, previous = 0u;
  for (size_t index = 0u; index < length; ++index) {
    unsigned digit = tag_hex((unsigned char)flags[index]);
    if (digit > 15u) return;
    uint8_t value = (uint8_t)digit;
    previous = last;
    if (h2_alipay_xor_mask(&value, 1u,
                           (const uint8_t*)out->token + positions[index], 1u,
                           false, &last) != H2_PAL_OK)
      return;
    last &= 15u;
  }
  out->flags_present = true;
  out->trusted = (last & 1u) != 0u;
  if (length >= 4u) details->reply_mode = previous;
}

static int tag_url(const h2_alipay_client_config_t* config,
                   tag_workspace_t* work,
                   h2_alipay_tap_tag_details_t* details) {
  memset(details, 0, sizeof(*details));
  h2_alipay_tap_tag_t* out = &details->tag;
  out->type = 1u;
  int rc = h2_alipay_client_url_parameter(work->url, "scheme", work->scheme,
                                          sizeof(work->scheme));
  if (rc != H2_PAL_OK || work->scheme[0] == '\0') return H2_PAL_ERR_FORMAT;
  if (strstr(work->scheme, "codeContent=") != NULL) {
    out->type = 5u;
    return H2_PAL_OK;
  }
  if (strstr(work->scheme, "pageParams=") != NULL) {
    rc = h2_alipay_client_url_parameter(
        work->scheme, "pageParams", work->parameters, sizeof(work->parameters));
    if (rc != H2_PAL_OK) return rc;
    const yyjson_alc allocator = {
        h2_alipay_client_json_alloc, h2_alipay_client_json_realloc,
        h2_alipay_client_json_free, (void*)config->mem};
    yyjson_read_err error;
    yyjson_doc* document = yyjson_read_opts(
        work->parameters, strlen(work->parameters), 0u, &allocator, &error);
    if (document == NULL)
      return error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION
                 ? H2_PAL_ERR_NO_MEMORY
                 : H2_PAL_ERR_FORMAT;
    yyjson_val* key = yyjson_obj_get(yyjson_doc_get_root(document), "k");
    rc = H2_PAL_ERR_FORMAT;
    if (yyjson_is_str(key) && yyjson_get_len(key) < sizeof(work->value) &&
        strlen(yyjson_get_str(key)) == yyjson_get_len(key)) {
      memcpy(work->value, yyjson_get_str(key), yyjson_get_len(key) + 1u);
      char* parts[32];
      size_t count = tag_split(work->value, '_', parts, 32u);
      if (count >= 2u) {
        rc = tag_token(out, parts[0]);
        size_t amount_length = strlen(parts[1]);
        if (amount_length > sizeof(details->amount))
          rc = H2_PAL_ERR_FORMAT;
        else
          memcpy(details->amount, parts[1], amount_length);
        if (rc == H2_PAL_OK && count >= 7u) tag_flags(details, parts[6]);
      }
    }
    yyjson_doc_free(document);
    out->type = 2u;
    return rc;
  }
  char source[32];
  rc =
      h2_alipay_client_url_parameter(work->scheme, "s", source, sizeof(source));
  if (rc != H2_PAL_OK) return rc;
  rc = h2_alipay_client_url_parameter(work->scheme, "v", work->value, 128u);
  if (rc != H2_PAL_OK || work->value[0] == '\0') return H2_PAL_ERR_FORMAT;
  char* parts[32];
  size_t count = tag_split(work->value, '~', parts, 32u);
  if (strcmp(source, "hg") == 0) {
    if (count != 1u && count < 4u) return H2_PAL_ERR_FORMAT;
    out->type = 4u;
    return tag_token(out, parts[0]);
  }
  if (strcmp(source, "vip") != 0 || count < 5u) return H2_PAL_ERR_FORMAT;
  char* token_parts[32];
  size_t token_count = tag_split(parts[3], '_', token_parts, 32u);
  if (token_count < 2u) return H2_PAL_ERR_FORMAT;
  out->type = 3u;
  rc = tag_token(out, token_parts[0]);
  if (rc == H2_PAL_OK && token_count >= 7u) tag_flags(details, token_parts[6]);
  return rc;
}

int h2_alipay_tap_decode_details(const h2_alipay_client_config_t* config,
                                 const uint8_t* data, size_t length,
                                 h2_alipay_tap_tag_details_t* out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  out->tag.type = 1u;
  if (config == NULL || config->mem == NULL || data == NULL || length == 0u ||
      length >= UINT32_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  tag_workspace_t* work = h2_pal_mem_alloc(config->mem, sizeof(*work));
  if (work == NULL) return H2_PAL_ERR_NO_MEMORY;
  int rc = H2_PAL_ERR_FORMAT;
  size_t offset = 0u;
  bool raw = (data[0] & 128u) == 0u;
  while (offset < length) {
    const uint8_t* text = data;
    size_t size = length;
    bool last = true;
    if (!raw) {
      if (length - offset < 3u) break;
      uint8_t header = data[offset], type_length = data[offset + 1u];
      if ((header & 32u) != 0u || (header & 7u) >= 6u ||
          ((header & 128u) != 0u) != (offset == 0u))
        break;
      size_t cursor = offset + 2u;
      uint32_t payload = 0u;
      if ((header & 16u) != 0u) {
        payload = data[cursor++];
      } else {
        if (length - cursor < 4u) break;
        for (size_t index = 0u; index < 4u; ++index)
          payload = (payload << 8u) | data[cursor++];
      }
      size_t id_length = 0u;
      if ((header & 8u) != 0u) {
        if (cursor == length) break;
        id_length = data[cursor++];
      }
      if (type_length + id_length > length - cursor) break;
      bool uri =
          (header & 7u) == 1u && type_length == 1u && data[cursor] == 'U';
      cursor += type_length + id_length;
      if (payload > length - cursor) break;
      offset = cursor + payload;
      last = (header & 64u) != 0u;
      if (!uri || payload < 2u) {
        if (last) break;
        continue;
      }
      text = data + cursor + 1u;
      size = payload - 1u;
    }
    if (size >= sizeof(work->url)) {
      rc = H2_PAL_ERR_NO_SPACE;
      break;
    }
    memset(work, 0, sizeof(*work));
    memcpy(work->url, text, size);
    rc = tag_url(config, work, out);
    if (rc == H2_PAL_OK || raw || last) break;
  }
  h2_alipay_client_wipe(work, sizeof(*work));
  h2_pal_mem_free(config->mem, work);
  return rc;
}

h2_pal_result_t h2_alipay_tap_decode_tag(
    const h2_alipay_client_config_t* config, const uint8_t* data, size_t length,
    h2_alipay_tap_tag_t* out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_alipay_tap_tag_details_t details;
  int rc = h2_alipay_tap_decode_details(config, data, length, &details);
  *out = details.tag;
  return rc;
}

int h2_alipay_tap_squeeze(const char* token, char output[16]) {
  if (token == NULL || output == NULL) return H2_PAL_ERR_INVALID_ARG;
  uint64_t hash = 0u;
  int rc = h2_alipay_hash31((const uint8_t*)token, strlen(token), &hash);
  if (rc != H2_PAL_OK) return rc;
  uint64_t magnitude = hash > INT64_MAX ? UINT64_MAX - hash + 1u : hash;
  static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  for (size_t index = 0u; index < 8u; ++index) {
    output[index] = (char)('0' + magnitude % 10u);
    magnitude /= 10u;
  }
  for (size_t index = 8u; index < 15u; ++index) {
    output[index] = alphabet[magnitude % 36u];
    magnitude /= 36u;
  }
  output[15] = '\0';
  return H2_PAL_OK;
}

h2_pal_result_t h2_alipay_tap_debug_token(const h2_pal_crypto_api_t* crypto,
                                          char out[128]) {
  if (crypto == NULL || out == NULL) return H2_PAL_ERR_INVALID_ARG;
  static const char input[] = "286948222617361705";
  static const char payer[] = "285007165115442444";
  char squeeze[16] = {0};
  uint8_t digest[16], encoded[sizeof(payer) - 1u];
  int rc = h2_alipay_tap_squeeze(input, squeeze);
  if (rc == H2_PAL_OK)
    rc = h2_pal_crypto_md5(crypto, (const uint8_t*)squeeze, 15u, digest);
  if (rc == H2_PAL_OK)
    rc = h2_alipay_xor_mask((const uint8_t*)payer, sizeof(encoded), digest,
                            sizeof(digest), true, encoded);
  if (rc == H2_PAL_OK) {
    size_t size = 0u;
    rc = h2_encoding_encode(&h2_encoding_base64_std, encoded, sizeof(encoded),
                            out, 127u, &size);
    if (rc == H2_ENCODING_OK) out[size] = '\0';
  }
  h2_alipay_client_wipe(digest, sizeof(digest));
  h2_alipay_client_wipe(encoded, sizeof(encoded));
  h2_alipay_client_wipe(squeeze, sizeof(squeeze));
  return rc;
}
