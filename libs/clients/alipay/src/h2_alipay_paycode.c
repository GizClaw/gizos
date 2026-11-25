#include "h2_alipay_client_internal.h"

#include <limits.h>
#include <string.h>

typedef struct paycode_workspace {
  uint8_t first[144], plain[144], iv[16], otp_key[20], digits[6];
  char code[20];
} paycode_workspace_t;

/* Public IV construction in this delivery: six MAC bytes followed by the
 * fixed ten-byte protocol suffix. This is not a device key or credential. */
static const uint8_t iv_suffix[10] = {0x45, 0x72, 0x49, 0x55, 0x6a,
                                      0x59, 0x37, 0x57, 0x6b, 0x6d};

static int hex_digit(unsigned char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

int h2_alipay_paycode_iv(const char* mac, uint8_t out[16]) {
  if (mac == NULL || out == NULL || strlen(mac) != 17u)
    return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0u; i < 6u; ++i) {
    int high = hex_digit((unsigned char)mac[i * 3u]);
    int low = hex_digit((unsigned char)mac[i * 3u + 1u]);
    if (high < 0 || low < 0 || (i < 5u && mac[i * 3u + 2u] != ':'))
      return H2_PAL_ERR_FORMAT;
    out[i] = (uint8_t)((high << 4) | low);
  }
  memcpy(out + 6u, iv_suffix, sizeof(iv_suffix));
  return H2_PAL_OK;
}

static int encode(const h2_alipay_client_config_t* config,
                  paycode_workspace_t* work, size_t plain_length,
                  size_t* code_length) {
  if (plain_length == 0u || plain_length >= sizeof(work->plain))
    return H2_PAL_ERR_FORMAT;
  char* index = (char*)work->plain;
  char* first = strstr(index, "###");
  if (first == NULL) return H2_PAL_ERR_FORMAT;
  *first = '\0';
  char* secret = first + 3u;
  char* second = strstr(secret, "###");
  if (second == NULL) return H2_PAL_ERR_FORMAT;
  *second = '\0';
  char* interval_text = second + 3u;
  size_t index_length = strlen(index), secret_length = strlen(secret);
  if (index_length == 0u || index_length > 11u || secret_length == 0u ||
      secret_length > 40u || secret_length % 2u != 0u)
    return H2_PAL_ERR_FORMAT;
  for (size_t i = 0u; i < index_length; ++i)
    if (index[i] < '0' || index[i] > '9') return H2_PAL_ERR_FORMAT;
  for (size_t i = 0u; i < secret_length / 2u; ++i) {
    int high = hex_digit((unsigned char)secret[i * 2u]);
    int low = hex_digit((unsigned char)secret[i * 2u + 1u]);
    if (high < 0 || low < 0) return H2_PAL_ERR_FORMAT;
    work->otp_key[i] = (uint8_t)((high << 4) | low);
  }
  uint32_t interval = 0u;
  char* cursor = interval_text;
  while (*cursor >= '0' && *cursor <= '9') {
    unsigned digit = (unsigned)(*cursor - '0');
    if (interval > ((uint32_t)INT_MAX - digit) / 10u) return H2_PAL_ERR_FORMAT;
    interval = interval * 10u + digit;
    ++cursor;
  }
  if (cursor == interval_text || interval == 0u) return H2_PAL_ERR_FORMAT;
  uint64_t utc_ms = 0u;
  int rc = h2_pal_time_get_wall_ms(config->time, &utc_ms);
  if (rc != H2_PAL_OK) return rc;
  uint64_t seconds = utc_ms / 1000u;
  if (seconds > UINT32_MAX) return H2_PAL_ERR_INVALID_STATE;
  uint32_t value = 0u;
  rc = h2_crypto_hotp_sha1(config->crypto, work->otp_key, secret_length / 2u,
                           seconds / interval, 6u, &value);
  if (rc != H2_PAL_OK) return rc;
  /* The delivery's decimal packing uses the same six HOTP digits cyclically
   * to transform a left-zero-padded index, then appends the original HOTP. */
  for (size_t i = 0u; i < 6u; ++i) {
    work->digits[5u - i] = (uint8_t)(value % 10u);
    value /= 10u;
  }
  work->code[0] = '2';
  work->code[1] = '8';
  size_t digits = index_length < 10u ? 10u : index_length;
  size_t leading = digits - index_length;
  for (size_t i = 0u; i < digits; ++i) {
    unsigned digit = i < leading ? 0u : (unsigned)(index[i - leading] - '0');
    work->code[2u + i] =
        (char)('0' + (digit * 107u + work->digits[i % 6u]) % 10u);
  }
  for (size_t i = 0u; i < 6u; ++i)
    work->code[2u + digits + i] = (char)('0' + work->digits[i]);
  *code_length = 2u + digits + 6u;
  return H2_PAL_OK;
}

int h2_alipay_paycode_generate(const h2_alipay_client_config_t* config,
                               const binding_record_t* binding, uint8_t* out,
                               size_t capacity, size_t* length,
                               h2_alipay_client_stage_t* stage) {
  if (config == NULL || binding == NULL || out == NULL || length == NULL ||
      stage == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *length = 0u;
  *stage = H2_ALIPAY_CLIENT_PAYCODE_DECRYPT;
  if (binding->seed_length == 0u ||
      binding->seed_length > sizeof(binding->seed) ||
      binding->seed_length % 16u != 0u)
    return H2_PAL_ERR_FORMAT;
  paycode_workspace_t* work = h2_pal_mem_alloc(config->mem, sizeof(*work));
  if (work == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(work, 0, sizeof(*work));
  int rc = h2_alipay_paycode_iv(config->identity.mac, work->iv);
  size_t first_length = 0u, plain_length = 0u, size = 0u;
  if (rc == H2_PAL_OK)
    rc = h2_crypto_aes_cbc_pkcs7_decrypt(
        config->crypto, binding->secret + 16u, 16u, work->iv, binding->seed,
        binding->seed_length, work->first, sizeof(work->first), &first_length);
  if (rc == H2_PAL_OK)
    rc = h2_crypto_aes_cbc_pkcs7_decrypt(
        config->crypto, binding->secret, 16u, work->iv, work->first,
        first_length, work->plain, sizeof(work->plain) - 1u, &plain_length);
  if (rc == H2_PAL_OK) {
    *stage = H2_ALIPAY_CLIENT_PAYCODE;
    work->plain[plain_length] = '\0';
    rc = encode(config, work, plain_length, &size);
  }
  if (rc == H2_PAL_OK && capacity <= size) rc = H2_PAL_ERR_NO_SPACE;
  if (rc == H2_PAL_OK) {
    memcpy(out, work->code, size + 1u);
    *length = size;
  } else if (rc == H2_PAL_ERR_NO_SPACE)
    *length = size;
  h2_alipay_client_wipe(work, sizeof(*work));
  h2_pal_mem_free(config->mem, work);
  return rc;
}
