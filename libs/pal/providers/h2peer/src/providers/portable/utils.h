#ifndef UTILS_H_
#define UTILS_H_

#include "config.h"
#include "h2/pal/os/h2_pal_crypto.h"
#include "h2/pal/os/h2_pal_log.h"
#include <stdint.h>

#define LEVEL_ERROR 0x00
#define LEVEL_WARN 0x01
#define LEVEL_INFO 0x02
#define LEVEL_DEBUG 0x03

#ifndef LOG_LEVEL
#define LOG_LEVEL LEVEL_INFO
#endif

void h2_peer_log_write(const h2_pal_log_api_t *log, h2_pal_log_level_t level,
                       const char *file_name, int line_number, const char *fmt,
                       ...);
#define H2_PEER_LOG(log, level, fmt, ...)                                      \
  h2_peer_log_write(log, level, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#if LOG_LEVEL >= LEVEL_DEBUG
#define H2_PEER_LOGD(log, fmt, ...)                                            \
  H2_PEER_LOG(log, H2_PAL_LOG_DEBUG, fmt, ##__VA_ARGS__)
#else
#define H2_PEER_LOGD(log, fmt, ...)
#endif

#if LOG_LEVEL >= LEVEL_INFO
#define H2_PEER_LOGI(log, fmt, ...)                                            \
  H2_PEER_LOG(log, H2_PAL_LOG_INFO, fmt, ##__VA_ARGS__)
#else
#define H2_PEER_LOGI(log, fmt, ...)
#endif

#if LOG_LEVEL >= LEVEL_WARN
#define H2_PEER_LOGW(log, fmt, ...)                                            \
  H2_PEER_LOG(log, H2_PAL_LOG_WARN, fmt, ##__VA_ARGS__)
#else
#define H2_PEER_LOGW(log, fmt, ...)
#endif

#if LOG_LEVEL >= LEVEL_ERROR
#define H2_PEER_LOGE(log, fmt, ...)                                            \
  H2_PEER_LOG(log, H2_PAL_LOG_ERROR, fmt, ##__VA_ARGS__)
#else
#define H2_PEER_LOGE(log, fmt, ...)
#endif

#define ALIGN32(num) ((num + 3) & ~3)

void utils_random_string(const h2_pal_crypto_api_t* crypto, char* s,
                         const int len);

int utils_get_hmac_sha1(const h2_pal_crypto_api_t* crypto,
                        const char* input, size_t input_len,
                        const char* key, size_t key_len,
                        unsigned char* output);

int utils_get_md5(const h2_pal_crypto_api_t* crypto,
                  const char* input, size_t input_len,
                  unsigned char* output);

#endif  // UTILS_H_
