#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "h2_darwin_platform.h"
#include "h2_pal_core_e2e.h"
#include "host_config.h"
#include "host_observer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Runtime borrows canonical unsupported events. Its independent, exclusive
 * real host event fixture is owned by this process and the App's lifecycle. */
static h2_runtime_t *runtime;
static h2_pal_core_e2e_config_t config;
static h2_pal_core_e2e_result_t result;

typedef struct observer {
  FILE *sink;
  long offset;
} observer_t;
static observer_t observer;

static void case_begin(void *user, const char *id) {
  (void)user;
  printf("H2_PAL_CORE_E2E begin=%s\n", id);
  fflush(stdout);
}

static h2_pal_result_t monotonic(void *user, uint64_t *out_us) {
  (void)user;
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return H2_PAL_ERR_IO;
  *out_us = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
  return H2_PAL_OK;
}

static h2_pal_result_t observed_log(void *user, h2_pal_log_level_t level,
                                    const char *scope, const char *message) {
  observer_t *o = user;
  static const char *const levels[] = {"debug", "info", "warn", "error"};
  char expected[512], actual[512];
  if (level < H2_PAL_LOG_DEBUG || level > H2_PAL_LOG_ERROR)
    return H2_PAL_ERR_INVALID_ARG;
  snprintf(expected, sizeof(expected), "H2_LOG level=%s scope=%s message=%s\n",
           levels[level], scope, message);
  fflush(stderr);
  if (fseek(o->sink, o->offset, SEEK_SET) != 0 ||
      fgets(actual, sizeof(actual), o->sink) == NULL)
    return H2_PAL_ERR_IO;
  o->offset = ftell(o->sink);
  if (fseek(o->sink, 0, SEEK_END) != 0)
    return H2_PAL_ERR_IO;
  return strcmp(actual, expected) == 0 ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}

int main(void) {
  /* Independent OS watchdog also bounds a broken blocking PAL lock/join. */
  alarm(120u);
  if (h2_pal_core_host_observer_start() != H2_PAL_OK)
    return 2;
  h2_runtime_config_t runtime_config = h2_pal_core_host_config();
  if (h2_runtime_init(&runtime_config, &runtime) != H2_PAL_OK)
    return 2;
  config.timeout_ms = 2000u;
  config.observe_monotonic_us = monotonic;
  config.observe_log = observed_log;
  config.observer_user = &observer;
  config.case_begin = case_begin;
  config.observe_resources = h2_pal_core_host_resources;
  config.observe_task_stack = h2_pal_core_host_stack;
  config.task_allocation_fault = h2_pal_core_host_task_fault;
  config.firmware_version = h2_pal_core_host_build_version();
  /* Desktop changes its own offset; this does not change the OS wall clock. */
  config.allow_wall_set = 1;
  config.queue_latest = H2_PAL_CORE_QUEUE_LATEST_REPLACE;
  config.event_fixture = h2_darwin_system_event_api();
  observer.sink = tmpfile();
  if (observer.sink == NULL)
    return 2;
  int saved_stderr = dup(STDERR_FILENO);
  if (saved_stderr < 0 || dup2(fileno(observer.sink), STDERR_FILENO) < 0)
    return 2;
  h2_pal_result_t rc = h2_pal_core_e2e_run(runtime, &config, &result);
  fflush(stderr);
  if (dup2(saved_stderr, STDERR_FILENO) < 0)
    return 2;
  close(saved_stderr);
  fclose(observer.sink);
  for (size_t i = 0; i < H2_PAL_CORE_E2E_CASE_COUNT; ++i) {
    printf("H2_PAL_CORE_E2E case=%s status=%s result=%d\n", result.cases[i].id,
           h2_pal_core_e2e_status_name(result.cases[i].status),
           (int)result.cases[i].result);
  }
  printf("H2_PAL_CORE_E2E contract=%u passed=%zu failed=%zu blocked=%zu "
         "not_run=%zu complete=%d qualified=%d cleanup=%d\n",
         H2_PAL_CORE_E2E_CONTRACT_VERSION, result.passed, result.failed,
         result.blocked, result.not_run, result.complete, result.qualified,
         (int)result.cleanup_result);
  if (result.retained_cleanup != NULL ||
      result.baseline.retained_cleanup != NULL) {
    /* Process exit, rather than freeing any still-borrowed callback contexts.
     */
    fflush(stdout);
    _Exit(2);
  }
  h2_runtime_deinit(runtime);
  if (h2_pal_core_host_observer_stop() != H2_PAL_OK)
    return 2;
  return rc == H2_PAL_OK && result.qualified ? 0 : 1;
}
