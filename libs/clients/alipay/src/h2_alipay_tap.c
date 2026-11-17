#include "h2_alipay_tap.h"

#include "h2_alipay_client_internal.h"
#include "h2_alipay_tap_internal.h"
#include "h2_encoding.h"
#include "h2_sha256.h"
#include <stdio.h>
#include <string.h>

struct h2_alipay_tap {
  h2_alipay_client_config_t config;
  char token[32], squeeze[32], paycode[32];
  uint8_t write_result;
};

static int bounded_copy(char out[32], const char* input) {
  if (input == NULL) return H2_PAL_ERR_INVALID_ARG;
  for (size_t length = 0u; length < 32u; ++length) {
    if (input[length] == '\0') {
      memcpy(out, input, length);
      return H2_PAL_OK;
    }
  }
  return H2_PAL_ERR_NO_SPACE;
}

h2_pal_result_t h2_alipay_tap_open(const h2_alipay_client_config_t* config,
                                   h2_alipay_tap_t** out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  int rc = h2_alipay_client_validate_config(config);
  if (rc != H2_PAL_OK) return rc;
  h2_alipay_tap_t* tap = h2_pal_mem_alloc(config->mem, sizeof(*tap));
  if (tap == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(tap, 0, sizeof(*tap));
  tap->config = *config;
  *out = tap;
  return H2_PAL_OK;
}

void h2_alipay_tap_close(h2_alipay_tap_t* tap) {
  if (tap == NULL) return;
  const h2_pal_mem_api_t* memory = tap->config.mem;
  h2_alipay_client_wipe(tap, sizeof(*tap));
  h2_pal_mem_free(memory, tap);
}

h2_pal_result_t h2_alipay_tap_set_context(h2_alipay_tap_t* tap,
                                          const char* token,
                                          const char* squeeze,
                                          const char* paycode) {
  if (tap == NULL) return H2_PAL_ERR_INVALID_ARG;
  char next_token[32] = {0}, next_squeeze[32] = {0}, next_code[32] = {0};
  int rc = bounded_copy(next_token, token);
  if (rc == H2_PAL_OK) rc = bounded_copy(next_squeeze, squeeze);
  if (rc == H2_PAL_OK) rc = bounded_copy(next_code, paycode);
  if (rc == H2_PAL_OK && (next_token[0] == '\0' || next_squeeze[0] == '\0' ||
                          next_code[0] == '\0'))
    rc = H2_PAL_ERR_INVALID_ARG;
  if (rc == H2_PAL_OK) {
    memcpy(tap->token, next_token, sizeof(tap->token));
    memcpy(tap->squeeze, next_squeeze, sizeof(tap->squeeze));
    memcpy(tap->paycode, next_code, sizeof(tap->paycode));
    tap->write_result = 0u;
  }
  h2_alipay_client_wipe(next_token, sizeof(next_token));
  h2_alipay_client_wipe(next_squeeze, sizeof(next_squeeze));
  h2_alipay_client_wipe(next_code, sizeof(next_code));
  return rc;
}

h2_pal_result_t h2_alipay_tap_parse_result(h2_alipay_tap_t* tap,
                                           const uint8_t* data, size_t length,
                                           h2_alipay_tap_payment_t* out) {
  if (out != NULL) memset(out, 0, sizeof(*out));
  if (tap == NULL || out == NULL || data == NULL || length != 27u ||
      data[0] != 1u || data[3] > 1u)
    return H2_PAL_ERR_INVALID_ARG;
  binding_record_t binding = {0};
  int rc = h2_alipay_client_load_binding(&tap->config, &binding);
  size_t uid_length = strlen(binding.uid);
  if (rc == H2_PAL_OK && (!h2_alipay_client_binding_complete(&binding) ||
                          uid_length < 5u || uid_length > 32u))
    rc = H2_PAL_ERR_NOT_FOUND;
  uint8_t plain[19] = {0}, expected[16] = {0}, twice[64] = {0};
  size_t token_length = strlen(tap->squeeze);
  if (rc == H2_PAL_OK) {
    rc = h2_alipay_xor_mask(data + 4u, sizeof(plain),
                            (const uint8_t*)binding.uid + uid_length - 5u, 5u,
                            false, plain);
    if (token_length == 0u) rc = H2_ALIPAY_TAP_ERR_TOKEN;
  }
  if (rc == H2_PAL_OK) {
    memcpy(twice, tap->squeeze, token_length);
    memcpy(twice + token_length, tap->squeeze, token_length);
    rc = h2_pal_crypto_md5(tap->config.crypto, twice, token_length * 2u,
                           expected);
  }
  if (rc == H2_PAL_OK &&
      !h2_crypto_bytes_equal(expected, plain, sizeof(expected)))
    rc = H2_ALIPAY_TAP_ERR_TOKEN;
  if (rc == H2_PAL_OK) {
    out->type = data[3];
    out->coupon_status = data[26];
    out->order_amount_fen =
        ((uint32_t)plain[16] << 16u) | ((uint32_t)plain[17] << 8u) | plain[18];
    out->real_amount_fen =
        ((uint32_t)data[23] << 16u) | ((uint32_t)data[24] << 8u) | data[25];
  }
  h2_alipay_client_wipe(&binding, sizeof(binding));
  h2_alipay_client_wipe(plain, sizeof(plain));
  h2_alipay_client_wipe(expected, sizeof(expected));
  h2_alipay_client_wipe(twice, sizeof(twice));
  return rc;
}

h2_pal_result_t h2_alipay_tap_plain_code(h2_alipay_tap_t* tap,
                                         const uint8_t* token,
                                         size_t token_length, uint8_t* out,
                                         size_t capacity, size_t* length) {
  if (tap == NULL || token == NULL || token_length == 0u ||
      token_length >= 32u || out == NULL || length == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (strlen(tap->token) != token_length ||
      memcmp(tap->token, token, token_length) != 0 || tap->paycode[0] == '\0')
    return H2_PAL_ERR_NOT_FOUND;
  size_t size = strlen(tap->paycode);
  if (capacity < size) return H2_PAL_ERR_NO_SPACE;
  memcpy(out, tap->paycode, size);
  *length = size;
  return H2_PAL_OK;
}

h2_pal_result_t h2_alipay_tap_coupon_url(h2_alipay_tap_t* tap, uint8_t* out,
                                         size_t capacity, size_t* length) {
  static const char prefix[] =
      "alipays://platformapi/startapp?appId=2021005183656621&"
      "page=ages/smart-device-pay-manage/index?tid=";
  if (tap == NULL || out == NULL || length == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  binding_record_t binding = {0};
  int rc = h2_alipay_client_load_binding(&tap->config, &binding);
  size_t size = sizeof(prefix) - 1u + strlen(binding.tid);
  if (rc == H2_PAL_OK &&
      (!h2_alipay_client_binding_complete(&binding) || binding.tid[0] == '\0'))
    rc = H2_PAL_ERR_NOT_FOUND;
  if (rc == H2_PAL_OK && capacity <= size) rc = H2_PAL_ERR_NO_SPACE;
  if (rc == H2_PAL_OK) {
    memcpy(out, prefix, sizeof(prefix) - 1u);
    memcpy(out + sizeof(prefix) - 1u, binding.tid, strlen(binding.tid) + 1u);
    *length = size;
  }
  h2_alipay_client_wipe(&binding, sizeof(binding));
  return rc;
}

h2_pal_result_t h2_alipay_tap_write_result(h2_alipay_tap_t* tap,
                                           const uint8_t* token,
                                           size_t token_length, bool write,
                                           uint8_t* value) {
  if (tap == NULL || token == NULL || token_length == 0u ||
      token_length >= sizeof(tap->token) || value == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (tap->token[0] == '\0' || tap->paycode[0] == '\0' ||
      strlen(tap->token) != token_length ||
      memcmp(tap->token, token, token_length) != 0)
    return H2_PAL_ERR_NOT_FOUND;
  if (write)
    tap->write_result = *value;
  else
    *value = tap->write_result;
  return H2_PAL_OK;
}

h2_pal_result_t h2_alipay_tap_payment_context(h2_alipay_tap_t* tap,
                                              char code[32], char token[32]) {
  if (tap == NULL || code == NULL || token == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memcpy(code, tap->paycode, sizeof(tap->paycode));
  memcpy(token, tap->token, sizeof(tap->token));
  return H2_PAL_OK;
}

int h2_alipay_tap_generate(h2_alipay_tap_t* tap, const char* token,
                           bool reset_write_result,
                           h2_alipay_client_payment_t* diagnostics) {
  if (tap == NULL || token == NULL || diagnostics == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  char next_token[32] = {0}, code[32] = {0}, squeeze[16] = {0};
  int rc = bounded_copy(next_token, token);
  if (rc != H2_PAL_OK || next_token[0] == '\0') return H2_PAL_ERR_INVALID_ARG;
  memcpy(tap->token, next_token, sizeof(tap->token));
  if (reset_write_result) tap->write_result = 0u;
  size_t length = sizeof(code);
  rc = h2_alipay_client_get_paycode(&tap->config, (uint8_t*)code, &length,
                                    diagnostics);
  if (rc == H2_PAL_OK && (length == 0u || length >= sizeof(code)))
    rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK) {
    code[length] = '\0';
    rc = h2_alipay_tap_squeeze(token, squeeze);
  }
  if (rc == H2_PAL_OK) {
    memcpy(tap->paycode, code, sizeof(tap->paycode));
    memset(tap->squeeze, 0, sizeof(tap->squeeze));
    memcpy(tap->squeeze, squeeze, sizeof(squeeze));
  }
  h2_alipay_client_wipe(next_token, sizeof(next_token));
  h2_alipay_client_wipe(code, sizeof(code));
  h2_alipay_client_wipe(squeeze, sizeof(squeeze));
  return rc;
}

int h2_alipay_tap_nfc_reply(h2_alipay_tap_t* tap, bool vip,
                            const uint8_t watch[2], char output[128],
                            size_t* length) {
  if (tap == NULL || watch == NULL || output == NULL || length == NULL ||
      strlen(tap->squeeze) != 15u || tap->paycode[0] == '\0')
    return H2_PAL_ERR_INVALID_ARG;
  uint8_t digest[16], masked[32];
  char encoded[48] = {0};
  size_t size = strlen(tap->paycode), encoded_length = 0u;
  int rc = h2_pal_crypto_md5(tap->config.crypto, (const uint8_t*)tap->squeeze,
                             15u, digest);
  if (rc == H2_PAL_OK)
    rc = h2_alipay_xor_mask((const uint8_t*)tap->paycode, size, digest,
                            sizeof(digest), true, masked);
  if (rc == H2_PAL_OK)
    rc = h2_encoding_encode(&h2_encoding_base64_std, masked, size, encoded,
                            sizeof(encoded) - 1u, &encoded_length);
  if (rc == H2_PAL_OK) {
    encoded[encoded_length] = '\0';
    int written = snprintf(output, 128u, "%s310%s_%s_%02x%02x",
                           vip ? "MP" : "NW", encoded, tap->squeeze + 11u,
                           (unsigned)watch[0], (unsigned)watch[1]);
    if (written < 0 || written >= 128)
      rc = H2_PAL_ERR_NO_SPACE;
    else
      *length = (size_t)written;
  }
  h2_alipay_client_wipe(digest, sizeof(digest));
  h2_alipay_client_wipe(masked, sizeof(masked));
  h2_alipay_client_wipe(encoded, sizeof(encoded));
  return rc;
}

static unsigned packed_number(const char* text, size_t length, unsigned base) {
  unsigned value = 0u;
  for (size_t index = 0u; index < length; ++index) {
    unsigned digit = text[index] >= 'a' ? (unsigned)(text[index] - 'a') + 10u
                                        : (unsigned)(text[index] - '0');
    value = value * base + digit;
  }
  return value;
}

static void packed_append(uint8_t output[16], unsigned value, unsigned width) {
  for (unsigned bit = width; bit != 0u; --bit) {
    unsigned carry = (value >> (bit - 1u)) & 1u;
    for (size_t index = 0u; index < 16u; ++index) {
      unsigned next = ((unsigned)output[index] << 1u) | carry;
      output[index] = (uint8_t)next;
      carry = next >> 8u;
    }
  }
}

int h2_alipay_tap_ble_reply(h2_alipay_tap_t* tap,
                            h2_alipay_md5_aes_keyset_t* keys, bool vip,
                            uint8_t mode, const uint8_t watch[2],
                            uint8_t output[20]) {
  if (tap == NULL || keys == NULL || watch == NULL || output == NULL ||
      strlen(tap->paycode) != 18u || strlen(tap->squeeze) != 15u || mode > 2u)
    return H2_PAL_ERR_INVALID_ARG;
  char token[16] = {0}, text[27] = {0};
  binding_record_t binding = {0};
  int rc = H2_PAL_OK;
  if (vip) {
    rc = h2_alipay_client_load_binding(&tap->config, &binding);
    if (rc == H2_PAL_OK && strlen(binding.uid) >= 12u)
      (void)snprintf(token, sizeof(token), "%.12s000", binding.uid + 4u);
    else if (rc == H2_PAL_OK)
      rc = H2_PAL_ERR_FORMAT;
  } else {
    memcpy(token, tap->squeeze, sizeof(token));
  }
  memcpy(text, tap->paycode, 18u);
  memcpy(text + 18u, token, 8u);
  uint8_t packed[16] = {0}, cipher[16] = {0}, masked[17] = {0};
  uint8_t digest[16] = {0};
  for (size_t index = 0u; index < 24u; index += 3u)
    packed_append(packed, packed_number(text + index, 3u, 10u), 10u);
  packed_append(packed, packed_number(text + 24u, 2u, 10u), 7u);
  packed_append(packed, mode, 4u);
  uint64_t tail = (uint64_t)packed_number(token + 8u, 3u, 36u) << 21u;
  tail |= packed_number(token + 11u, 4u, 36u);
  for (unsigned bit = 0u; bit < 37u; ++bit)
    packed[(91u + bit) / 8u] |=
        (uint8_t)(((tail >> bit) & 1u) << ((91u + bit) % 8u));
  size_t cipher_length = 0u;
  if (rc == H2_PAL_OK)
    rc = h2_alipay_md5_aes_keyset_encrypt(keys, mode, packed + 1u, 15u, cipher,
                                          sizeof(cipher), &cipher_length);
  if (rc == H2_PAL_OK && cipher_length != sizeof(cipher))
    rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK) {
    masked[0] = packed[0];
    memcpy(masked + 1u, cipher, sizeof(cipher));
    rc = h2_pal_crypto_md5(tap->config.crypto, (const uint8_t*)tap->squeeze,
                           15u, digest);
  }
  if (rc == H2_PAL_OK)
    rc = h2_alipay_xor_mask(masked, sizeof(masked), digest, sizeof(digest),
                            false, masked);
  if (rc == H2_PAL_OK) {
    output[0] = vip ? 'M' : 'N';
    memcpy(output + 1u, watch, 2u);
    memcpy(output + 3u, masked, sizeof(masked));
  }
  h2_alipay_client_wipe(&binding, sizeof(binding));
  h2_alipay_client_wipe(token, sizeof(token));
  h2_alipay_client_wipe(text, sizeof(text));
  h2_alipay_client_wipe(packed, sizeof(packed));
  h2_alipay_client_wipe(cipher, sizeof(cipher));
  h2_alipay_client_wipe(masked, sizeof(masked));
  h2_alipay_client_wipe(digest, sizeof(digest));
  return rc;
}

int h2_alipay_tap_cabinet_reply(h2_alipay_tap_t* tap, bool wallet,
                                char output[128], size_t* length) {
  if (tap == NULL || output == NULL || length == NULL ||
      strlen(tap->squeeze) != 15u)
    return H2_PAL_ERR_INVALID_ARG;
  binding_record_t binding = {0};
  char plain[64] = {0}, encoded[88] = {0};
  uint8_t digest[16] = {0}, masked[64] = {0};
  int rc = h2_alipay_client_load_binding(&tap->config, &binding);
  if (rc == H2_PAL_OK && strlen(binding.uid) < 4u) rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK) {
    int written =
        snprintf(plain, sizeof(plain), "%s_%s", tap->paycode, binding.uid + 4u);
    if (written < 0 || (size_t)written >= sizeof(plain))
      rc = H2_PAL_ERR_NO_SPACE;
  }
  if (rc == H2_PAL_OK)
    rc = h2_pal_crypto_md5(tap->config.crypto, (const uint8_t*)tap->squeeze,
                           15u, digest);
  if (rc == H2_PAL_OK)
    rc = h2_alipay_xor_mask((const uint8_t*)plain, strlen(plain), digest,
                            sizeof(digest), true, masked);
  size_t encoded_length = 0u;
  if (rc == H2_PAL_OK)
    rc = h2_encoding_encode(&h2_encoding_base64_std, masked, strlen(plain),
                            encoded, sizeof(encoded) - 1u, &encoded_length);
  if (rc == H2_PAL_OK) {
    encoded[encoded_length] = '\0';
    int written =
        snprintf(output, 128u, "%s%s_%s", wallet ? "1301NFC" : "1311NFC",
                 encoded, tap->squeeze + 11u);
    if (written < 0 || written >= 128)
      rc = H2_PAL_ERR_NO_SPACE;
    else
      *length = (size_t)written;
  }
  h2_alipay_client_wipe(&binding, sizeof(binding));
  h2_alipay_client_wipe(plain, sizeof(plain));
  h2_alipay_client_wipe(encoded, sizeof(encoded));
  h2_alipay_client_wipe(digest, sizeof(digest));
  h2_alipay_client_wipe(masked, sizeof(masked));
  return rc;
}

static int tap_hex_point(const char* text, uint8_t point[65]) {
  if (text == NULL || strlen(text) != 128u) return H2_PAL_ERR_FORMAT;
  size_t length = 0u;
  point[0] = 4u;
  int rc = h2_encoding_decode(&h2_encoding_hex, text, 128u, point + 1u, 64u,
                              &length);
  return rc == H2_ENCODING_OK && length == 64u ? H2_PAL_OK : H2_PAL_ERR_FORMAT;
}

int h2_alipay_tap_verify_device(const h2_pal_crypto_api_t* crypto,
                                const char* public_key, const char* signature) {
  static const char roots[2][129] = {
      "404C01B5DB5F8C760A6A63F82CDDE4DCA8DDF9049DDCF3565AB8F1B600509C657"
      "CCC1B7C6C769EE79D9FAB0E1717474BD4AB5F54FF081F3AA07756083096EC3D",
      "6D70830D4C27B0FD4D512C4A1FDED637AA27A2A5830CBAF199B7BC05613C7755"
      "3CBBBAEA0A4E3084AE85C5BD100725851FAB943DB6BD3F4E2D854FD96D2832D2"};
  if (crypto == NULL || signature == NULL || strlen(signature) != 130u)
    return H2_PAL_ERR_FORMAT;
  uint8_t point[65] = {0}, root[65] = {0};
  h2_pal_p256_public_key_t trusted;
  h2_pal_p256_signature_t decoded = {0};
  int rc = tap_hex_point(public_key, point);
  if (rc == H2_PAL_OK)
    rc = tap_hex_point(roots[signature[0] == '1' ? 1u : 0u], root);
  if (rc == H2_PAL_OK)
    rc = h2_crypto_p256_public_key_import(crypto, root, &trusted);
  size_t length = 0u;
  if (rc == H2_PAL_OK)
    rc = h2_encoding_decode(&h2_encoding_hex, signature + 2u, 128u,
                            decoded.bytes, sizeof(decoded.bytes), &length);
  if (rc == H2_PAL_OK && length == sizeof(decoded.bytes))
    rc = h2_pal_crypto_ecdsa_p256_sha256_verify(crypto, &trusted, point + 1u,
                                                64u, &decoded);
  else if (rc == H2_PAL_OK)
    rc = H2_PAL_ERR_FORMAT;
  h2_alipay_client_wipe(point, sizeof(point));
  h2_alipay_client_wipe(root, sizeof(root));
  h2_alipay_client_wipe(&decoded, sizeof(decoded));
  return rc;
}

int h2_alipay_tap_connected_reply(h2_alipay_tap_t* tap, const char* public_key,
                                  const char* payment_token, bool vip,
                                  uint8_t output[256], size_t* length,
                                  h2_alipay_client_payment_t* diagnostics) {
  if (tap == NULL || payment_token == NULL || strlen(payment_token) > 63u ||
      output == NULL || length == NULL || diagnostics == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  uint8_t point[65] = {0}, shared[32] = {0}, digest[32] = {0};
  uint8_t cipher[48] = {0}, iv[16] = {0};
  char code[32] = {0};
  h2_pal_p256_keypair_t pair = {0};
  h2_pal_p256_public_key_t remote;
  int rc = tap_hex_point(public_key, point);
  if (rc == H2_PAL_OK)
    rc = h2_crypto_p256_public_key_import(tap->config.crypto, point, &remote);
  if (rc == H2_PAL_OK)
    rc = h2_pal_crypto_p256_keypair_generate(tap->config.crypto, &pair);
  if (rc == H2_PAL_OK)
    rc = h2_pal_crypto_p256_shared_secret(tap->config.crypto, &pair.private_key,
                                          &remote, shared);
  if (rc == H2_PAL_OK) {
    h2_sha256_t hash;
    h2_sha256_init(&hash);
    h2_sha256_update(&hash, shared, sizeof(shared));
    h2_sha256_update(&hash, (const uint8_t*)payment_token,
                     strlen(payment_token));
    h2_sha256_finish(&hash, digest);
    h2_alipay_client_wipe(&hash, sizeof(hash));
  }
  size_t code_length = sizeof(code), cipher_length = 0u, public_length = 0u;
  if (rc == H2_PAL_OK)
    rc = h2_alipay_client_get_paycode(&tap->config, (uint8_t*)code,
                                      &code_length, diagnostics);
  if (rc == H2_PAL_OK && (code_length == 0u || code_length >= sizeof(code)))
    rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK) {
    code[code_length] = '\0';
    memcpy(tap->paycode, code, sizeof(tap->paycode));
    rc = h2_crypto_aes_cbc_pkcs7_encrypt(
        tap->config.crypto, tap->config.mem, digest, sizeof(digest), iv,
        (const uint8_t*)code, code_length, cipher, sizeof(cipher),
        &cipher_length);
  }
  if (rc == H2_PAL_OK)
    rc = h2_pal_crypto_p256_public_key_uncompress(tap->config.crypto,
                                                  &pair.public_key, point);
  if (rc == H2_PAL_OK)
    rc = h2_encoding_encode(&h2_encoding_hex, point + 1u, 64u, (char*)output,
                            256u, &public_length);
  if (rc == H2_PAL_OK) {
    output[public_length++] = ',';
    size_t encoded = 0u;
    rc = h2_encoding_encode(&h2_encoding_hex, cipher, cipher_length,
                            (char*)output + public_length,
                            256u - public_length - 3u, &encoded);
    if (rc == H2_PAL_OK) {
      public_length += encoded;
      output[public_length++] = ',';
      for (size_t index = 0u; index < public_length; ++index)
        if (output[index] >= 'a' && output[index] <= 'f')
          output[index] = (uint8_t)(output[index] - 'a' + 'A');
      memcpy(output + public_length, vip ? "MP" : "NW", 2u);
      *length = public_length + 2u;
    }
  }
  h2_alipay_client_wipe(point, sizeof(point));
  h2_alipay_client_wipe(shared, sizeof(shared));
  h2_alipay_client_wipe(digest, sizeof(digest));
  h2_alipay_client_wipe(cipher, sizeof(cipher));
  h2_alipay_client_wipe(code, sizeof(code));
  h2_alipay_client_wipe(&pair, sizeof(pair));
  return rc;
}
