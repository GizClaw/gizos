#include "h2_onenet.h"
#include "h2_onenet_internal.h"
#include "h2_encoding.h"

#include <string.h>

void h2_onenet_wipe(void* data, size_t len) {
  volatile uint8_t* bytes = data;
  while (len-- != 0u) *bytes++ = 0u;
}

void h2_onenet_write(h2_onenet_writer_t* writer, const char* data, size_t len) {
  if (writer->result != H2_PAL_OK) return;
  if (writer->length >= writer->capacity ||
      len >= writer->capacity - writer->length) {
    writer->result = H2_PAL_ERR_NO_SPACE;
    return;
  }
  memcpy(writer->data + writer->length, data, len);
  writer->length += len;
  writer->data[writer->length] = '\0';
}

void h2_onenet_text(h2_onenet_writer_t* writer, const char* text) {
  h2_onenet_write(writer, text, strlen(text));
}

void h2_onenet_decimal(h2_onenet_writer_t* writer, uint64_t value) {
  char reverse[20];
  size_t count = 0u;
  do {
    reverse[count++] = (char)('0' + value % 10u);
    value /= 10u;
  } while (value != 0u);
  while (count != 0u) h2_onenet_write(writer, &reverse[--count], 1u);
}

static bool alphanumeric(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9');
}

bool h2_onenet_segment_valid(const char* name) {
  if (name == NULL || name[0] == '\0') return false;
  size_t i = 0u;
  for (; i < H2_ONENET_NAME_CAPACITY && name[i] != '\0'; ++i)
    if (!alphanumeric(name[i]) && name[i] != '_' && name[i] != '-' &&
        name[i] != '.' && name[i] != ':' && name[i] != '@')
      return false;
  return i < H2_ONENET_NAME_CAPACITY;
}

static size_t bounded_length(const char* text, size_t bound) {
  size_t len = 0u;
  if (text != NULL)
    while (len < bound && text[len] != '\0') ++len;
  return len;
}

static bool present(const char* text) {
  return text != NULL && text[0] != '\0';
}

static bool decimal_length(const char* text, size_t length) {
  if (bounded_length(text, length + 1u) != length) return false;
  for (size_t i = 0u; i < length; ++i)
    if (text[i] < '0' || text[i] > '9') return false;
  return true;
}

static bool serial_valid(const char* text) {
  const size_t len = bounded_length(text, 33u);
  if (len < 10u || len > 32u) return false;
  for (size_t i = 0u; i < len; ++i)
    if (!alphanumeric(text[i]) && text[i] != '-' && text[i] != ':')
      return false;
  return true;
}

static bool identity_valid(const h2_onenet_identity_t* identity) {
  if (identity->network > 4u) return false;
  if (identity->network == 0u)
    return (present(identity->sn) || present(identity->mac)) &&
           (!present(identity->sn) || serial_valid(identity->sn)) &&
           (!present(identity->mac) || serial_valid(identity->mac)) &&
           !present(identity->imei) && !present(identity->imsi) &&
           !present(identity->iccid) && !present(identity->msisdn);
  if (!decimal_length(identity->imei, 15u) ||
      (!present(identity->iccid) && !present(identity->imsi) &&
       !present(identity->msisdn)) ||
      present(identity->sn) || present(identity->mac))
    return false;
  if (present(identity->imsi) && (!decimal_length(identity->imsi, 15u) ||
                                  memcmp(identity->imsi, "460", 3u) != 0))
    return false;
  if (present(identity->iccid) &&
      (bounded_length(identity->iccid, 21u) != 20u ||
       memcmp(identity->iccid, "8986", 4u) != 0))
    return false;
  if (present(identity->msisdn) && !decimal_length(identity->msisdn, 11u) &&
      !decimal_length(identity->msisdn, 13u))
    return false;
  return true;
}

static void url_encode(h2_onenet_writer_t* writer, const char* data,
                       size_t len) {
  static const char hex[] = "0123456789ABCDEF";
  for (size_t i = 0u; i < len; ++i) {
    const unsigned char ch = (unsigned char)data[i];
    if (alphanumeric((char)ch) || ch == '-' || ch == '_' || ch == '.' ||
        ch == '~') {
      h2_onenet_write(writer, data + i, 1u);
    } else {
      const char escaped[] = {'%', hex[ch >> 4u], hex[ch & 15u]};
      h2_onenet_write(writer, escaped, sizeof(escaped));
    }
  }
}

static void identity_field(h2_onenet_writer_t* writer, const char* key,
                           const char* value) {
  if (!present(value)) return;
  h2_onenet_text(writer, key);
  url_encode(writer, value, strlen(value));
}

h2_pal_result_t h2_onenet_build_client_id(const char* device_name,
                                          const h2_onenet_identity_t* identity,
                                          char* out, size_t capacity) {
  if (out == NULL || capacity == 0u) return H2_PAL_ERR_INVALID_ARG;
  out[0] = '\0';
  if (!h2_onenet_segment_valid(device_name) ||
      (identity != NULL && !identity_valid(identity)))
    return H2_PAL_ERR_INVALID_ARG;
  h2_onenet_writer_t writer = {out, capacity, 0u, H2_PAL_OK};
  h2_onenet_text(&writer, device_name);
  if (identity != NULL) {
    h2_onenet_text(&writer, "?network=");
    h2_onenet_decimal(&writer, identity->network);
    identity_field(&writer, "&imei=", identity->imei);
    identity_field(&writer, "&iccid=", identity->iccid);
    identity_field(&writer, "&imsi=", identity->imsi);
    identity_field(&writer, "&msisdn=", identity->msisdn);
    identity_field(&writer, "&sn=", identity->sn);
    identity_field(&writer, "&mac=", identity->mac);
  }
  if (writer.result != H2_PAL_OK) out[0] = '\0';
  return writer.result;
}

h2_pal_result_t h2_onenet_token_generate(const h2_pal_crypto_api_t* crypto,
                                         const char* product_id,
                                         const char* device_name,
                                         const char* key_base64,
                                         uint64_t expires_at_unix_s, char* out,
                                         size_t capacity) {
  if (out == NULL || capacity == 0u) return H2_PAL_ERR_INVALID_ARG;
  out[0] = '\0';
  if (!h2_onenet_segment_valid(product_id) ||
      !h2_onenet_segment_valid(device_name) || key_base64 == NULL ||
      expires_at_unix_s == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  const size_t key_len = bounded_length(key_base64, 173u);
  if (key_len == 0u || key_len == 173u) return H2_PAL_ERR_INVALID_ARG;
  uint8_t key[H2_ONENET_KEY_MAX_BYTES] = {0};
  uint8_t digest[H2_PAL_CRYPTO_SHA1_SIZE] = {0};
  size_t decoded_len = 0u;
  h2_encoding_err_t encoded =
      h2_encoding_decode(&h2_encoding_base64_std, key_base64, key_len, key,
                         sizeof(key), &decoded_len);
  h2_pal_result_t rc = H2_PAL_ERR_FORMAT;
  if (encoded != H2_ENCODING_OK || decoded_len == 0u) goto done;
  char resource[2u * H2_ONENET_NAME_CAPACITY + 20u] = {0};
  h2_onenet_writer_t path = {resource, sizeof(resource), 0u, H2_PAL_OK};
  h2_onenet_text(&path, "products/");
  h2_onenet_text(&path, product_id);
  h2_onenet_text(&path, "/devices/");
  h2_onenet_text(&path, device_name);
  char signature_input[sizeof(resource) + 64u] = {0};
  h2_onenet_writer_t signature = {signature_input, sizeof(signature_input), 0u,
                                  path.result};
  h2_onenet_decimal(&signature, expires_at_unix_s);
  h2_onenet_text(&signature, "\nsha1\n");
  h2_onenet_text(&signature, resource);
  h2_onenet_text(&signature, "\n2018-10-31");
  rc = signature.result;
  if (rc != H2_PAL_OK) goto done;
  rc = h2_pal_crypto_hmac_sha1(crypto, key, decoded_len,
                               (const uint8_t*)signature_input,
                               signature.length, digest);
  if (rc != H2_PAL_OK) goto done;
  char digest_base64[32] = {0};
  size_t digest_len = 0u;
  if (h2_encoding_encode(&h2_encoding_base64_std, digest, sizeof(digest),
                         digest_base64, sizeof(digest_base64),
                         &digest_len) != H2_ENCODING_OK) {
    rc = H2_PAL_ERR_NO_SPACE;
    goto done;
  }
  h2_onenet_writer_t token = {out, capacity, 0u, H2_PAL_OK};
  h2_onenet_text(&token, "version=2018-10-31&res=");
  url_encode(&token, resource, path.length);
  h2_onenet_text(&token, "&et=");
  h2_onenet_decimal(&token, expires_at_unix_s);
  h2_onenet_text(&token, "&method=sha1&sign=");
  url_encode(&token, digest_base64, digest_len);
  rc = token.result;
done:
  h2_onenet_wipe(key, sizeof(key));
  h2_onenet_wipe(digest, sizeof(digest));
  if (rc != H2_PAL_OK) out[0] = '\0';
  return rc;
}
