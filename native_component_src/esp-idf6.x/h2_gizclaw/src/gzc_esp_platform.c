#include "gzc_platform.h"

#include "esp_random.h"
#include "esp_timer.h"

#include <stdlib.h>
#include <time.h>

static void *default_malloc(void *user, size_t size) {
  (void)user;
  return malloc(size);
}

static void *default_realloc(void *user, void *ptr, size_t size) {
  (void)user;
  return realloc(ptr, size);
}

static void default_free(void *user, void *ptr) {
  (void)user;
  free(ptr);
}

static int64_t default_time_instant_ms(void *user) {
  (void)user;
  return esp_timer_get_time() / 1000;
}

static int64_t default_time_unix_ms(void *user) {
  (void)user;
  return (int64_t)time(NULL) * 1000;
}

static int default_random(void *user, uint8_t *out, size_t len) {
  (void)user;
  if (out == NULL && len != 0u) {
    return GZC_ERR_INVALID_ARGUMENT;
  }
  if (len != 0u) {
    esp_fill_random(out, len);
  }
  return GZC_OK;
}

static void default_log(void *user, gzc_log_level_t level, gzc_str_t message) {
  (void)user;
  (void)level;
  (void)message;
}

const gzc_platform_t *gzc_default_platform(void) {
  static const gzc_platform_t platform = {
      .userdata = NULL,
      .malloc = default_malloc,
      .realloc = default_realloc,
      .free = default_free,
      .time_instant_ms = default_time_instant_ms,
      .time_unix_ms = default_time_unix_ms,
      .random = default_random,
      .log = default_log,
  };
  return &platform;
}
