#include "utils.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

typedef struct log_capture {
  size_t calls;
  int result;
  h2_pal_log_level_t level;
  const char *scope;
  char message[H2_PAL_LOG_MESSAGE_MAX];
} log_capture_t;

static int capture_log(void *user, h2_pal_log_level_t level, const char *scope,
                       const char *message) {
  log_capture_t *capture = user;
  capture->calls++;
  capture->level = level;
  capture->scope = scope;
  strncpy(capture->message, message, sizeof(capture->message) - 1u);
  capture->message[sizeof(capture->message) - 1u] = '\0';
  return capture->result;
}

static const h2_pal_log_vtable_t capture_vtable = {
    .write = capture_log,
};

static void test_routes_one_message_to_injected_log(void) {
  log_capture_t first = {0};
  log_capture_t second = {.result = H2_PAL_ERR_IO};
  const h2_pal_log_api_t log = {
      .user = &first,
      .vtable = &capture_vtable,
  };
  const h2_pal_log_api_t failing_log = {
      .user = &second,
      .vtable = &capture_vtable,
  };

  h2_peer_log_write(&log, H2_PAL_LOG_WARN, "portable.c", 42, "candidate %d", 7);
  h2_peer_log_write(&failing_log, H2_PAL_LOG_ERROR, "second.c", 9,
                    "independent");

  assert(first.calls == 1u);
  assert(first.level == H2_PAL_LOG_WARN);
  assert(strcmp(first.scope, "h2peer") == 0);
  assert(strcmp(first.message, "portable.c:42 candidate 7") == 0);
  assert(second.calls == 1u);
  assert(second.level == H2_PAL_LOG_ERROR);
  assert(strcmp(second.message, "second.c:9 independent") == 0);
}

static void test_truncates_message_at_pal_limit(void) {
  log_capture_t capture = {0};
  const h2_pal_log_api_t log = {
      .user = &capture,
      .vtable = &capture_vtable,
  };
  char long_message[H2_PAL_LOG_MESSAGE_MAX * 2u];
  memset(long_message, 'x', sizeof(long_message) - 1u);
  long_message[sizeof(long_message) - 1u] = '\0';

  h2_peer_log_write(&log, H2_PAL_LOG_INFO, "portable.c", 8, "%s", long_message);

  assert(capture.calls == 1u);
  assert(strlen(capture.message) == H2_PAL_LOG_MESSAGE_MAX - 1u);
}

static void test_missing_log_is_a_no_op(void) {
  const h2_pal_log_vtable_t incomplete_vtable = {0};
  const h2_pal_log_api_t incomplete_log = {
      .vtable = &incomplete_vtable,
  };
  h2_peer_log_write(NULL, H2_PAL_LOG_ERROR, "portable.c", 1, "ignored");
  h2_peer_log_write(&incomplete_log, H2_PAL_LOG_ERROR, "portable.c", 1,
                    "ignored");
}

static void test_debug_is_compiled_out_at_default_level(void) {
  log_capture_t capture = {0};
  const h2_pal_log_api_t log = {
      .user = &capture,
      .vtable = &capture_vtable,
  };

  H2_PEER_LOGD(&log, "disabled");
  (void)log;

  assert(capture.calls == 0u);
}

int main(void) {
  test_routes_one_message_to_injected_log();
  test_truncates_message_at_pal_limit();
  test_missing_log_is_a_no_op();
  test_debug_is_compiled_out_at_default_level();
  return 0;
}
