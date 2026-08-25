#include "gzc_platform.h"
#include "gzc_rpc_frame.h"

#include <stdint.h>
#include <stdlib.h>

static void *test_malloc(void *userdata, size_t size) {
  (void)userdata;
  return malloc(size);
}

static void *test_realloc(void *userdata, void *ptr, size_t size) {
  (void)userdata;
  return realloc(ptr, size);
}

static void test_free(void *userdata, void *ptr) {
  (void)userdata;
  free(ptr);
}

static int64_t test_time(void *userdata) {
  (void)userdata;
  return 1;
}

static int test_random(void *userdata, uint8_t *out, size_t len) {
  (void)userdata;
  for (size_t i = 0; i < len; i++) {
    out[i] = (uint8_t)i;
  }
  return GZC_OK;
}

static void test_log(void *userdata, gzc_log_level_t level, gzc_str_t message) {
  (void)userdata;
  (void)level;
  (void)message;
}

const gzc_platform_t *gzc_default_platform(void) {
  static const gzc_platform_t platform = {
      NULL,
      test_malloc,
      test_realloc,
      test_free,
      test_time,
      test_time,
      test_random,
      test_log,
  };
  return &platform;
}

int main(void) {
  gzc_buf_t encoded;
  gzc_buf_init(&encoded);
  const gzc_rpc_frame_t frame = {
      .type = GZC_RPC_FRAME_EOS,
      .data = NULL,
      .len = 0u,
  };
  if (gzc_rpc_frame_encode(NULL, &frame, &encoded) != GZC_OK ||
      encoded.len != 4u) {
    return 1;
  }
  gzc_buf_free(&encoded, gzc_default_platform());
  return 0;
}
