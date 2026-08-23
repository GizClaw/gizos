#include "utils.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

void h2_peer_log_write(const h2_pal_log_api_t *log, h2_pal_log_level_t level,
                       const char *file_name, int line_number, const char *fmt,
                       ...) {
  if (log == NULL || log->vtable == NULL || log->vtable->write == NULL ||
      file_name == NULL || fmt == NULL) {
    return;
  }
  char message[H2_PAL_LOG_MESSAGE_MAX];
  int prefix_len =
      snprintf(message, sizeof(message), "%s:%d ", file_name, line_number);
  if (prefix_len < 0) {
    return;
  }
  size_t offset = (size_t)prefix_len;
  if (offset >= sizeof(message)) {
    offset = sizeof(message) - 1u;
  }
  va_list args;
  va_start(args, fmt);
  int format_result =
      vsnprintf(message + offset, sizeof(message) - offset, fmt, args);
  va_end(args);
  if (format_result < 0) {
    return;
  }
  message[sizeof(message) - 1u] = '\0';
  (void)h2_pal_log_write(log, level, "h2peer", message);
}

void utils_random_string(const h2_pal_crypto_api_t *crypto, char *s,
                         const int len) {
  int i;

  static const char alphanum[] =
      "0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz";

  uint8_t random[256];
  if (len < 0 || (size_t)len > sizeof(random) ||
      h2_pal_crypto_random(crypto, random, (size_t)len) != H2_PAL_OK) {
    if (s != NULL) {
      s[0] = '\0';
    }
    return;
  }
  for (i = 0; i < len; ++i) {
    s[i] = alphanum[random[i] % (sizeof(alphanum) - 1)];
  }

  s[len] = '\0';
}

int utils_get_hmac_sha1(const h2_pal_crypto_api_t* crypto,
                        const char* input, size_t input_len,
                        const char* key, size_t key_len,
                        unsigned char* output) {
  return h2_pal_crypto_hmac_sha1(
      crypto, (const uint8_t*)key, key_len, (const uint8_t*)input,
      input_len, output);
}

int utils_get_md5(const h2_pal_crypto_api_t* crypto,
                  const char* input, size_t input_len,
                  unsigned char* output) {
  return h2_pal_crypto_md5(
      crypto, (const uint8_t*)input, input_len, output);
}
