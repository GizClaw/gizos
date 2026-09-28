#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "mobile_runner.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

h2_pal_result_t h2_mobile_core_clock(void *user, uint64_t *out) {
  (void)user;
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
    return H2_PAL_ERR_IO;
  *out = (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
  return H2_PAL_OK;
}
h2_pal_result_t h2_mobile_core_stack(void *user, size_t *out) {
  (void)user;
  uintptr_t local = (uintptr_t)&local;
#if defined(__APPLE__)
  *out = pthread_get_stacksize_np(pthread_self());
  uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
  if (!*out || local < top - *out || local >= top)
    return H2_PAL_ERR_INVALID_STATE;
#else
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr))
    return H2_PAL_ERR_IO;
  void *base = NULL;
  int rc = pthread_attr_getstack(&attr, &base, out);
  pthread_attr_destroy(&attr);
  if (rc || !*out || local < (uintptr_t)base || local >= (uintptr_t)base + *out)
    return H2_PAL_ERR_INVALID_STATE;
#endif
  printf("H2_MOBILE_CORE_STACK observed=%zu local_in_stack=1\n", *out);
  return H2_PAL_OK;
}
static void begin(void *user, const char *id) {
  (void)user;
  printf("H2_MOBILE_CORE_BEGIN id=%s\n", id);
  fflush(stdout);
}
int h2_mobile_core_run(const h2_mobile_core_fixture_t *f, const char *path) {
  h2_runtime_t *runtime = NULL;
  h2_runtime_config_t native = f->runtime_config;
  /* The test owns the actual event fixture exclusively. */
  native.system_event = h2_pal_unsupported_system_event_api();
  int rc = h2_runtime_init(&native, &runtime);
  h2_pal_core_e2e_result_t result = {0};
  if (rc != H2_PAL_OK) {
    /* Populate the complete NOT_RUN ledger even when Runtime init fails. */
    (void)h2_pal_core_e2e_run(NULL, &f->tests, &result);
  }
  h2_pal_core_resources_t before = {0}, after = {0};
  int before_rc = H2_PAL_ERR_UNAVAILABLE, after_rc = H2_PAL_ERR_UNAVAILABLE;
  int teardown = H2_PAL_ERR_UNAVAILABLE;
  if (rc == H2_PAL_OK) {
    h2_pal_core_e2e_config_t config = f->tests;
    config.timeout_ms = 3000;
    config.queue_latest = H2_PAL_CORE_QUEUE_LATEST_REPLACE;
    config.allow_wall_set = 1;
    config.firmware_version = f->image_version;
    config.observe_monotonic_us = h2_mobile_core_clock;
    config.observe_task_stack = h2_mobile_core_stack;
    config.case_begin = begin;
    before_rc = config.observe_resources(config.observer_user, &before);
    rc = h2_pal_core_e2e_run(runtime, &config, &result);
    after_rc = config.observe_resources(config.observer_user, &after);
    if (result.retained_cleanup == NULL &&
        result.baseline.retained_cleanup == NULL) {
      h2_runtime_deinit(runtime);
      teardown = f->shutdown();
    }
  }
  const int balanced = before_rc == 0 && after_rc == 0 &&
                       memcmp(&before, &after, sizeof(before)) == 0;
  FILE *out = fopen(path, "w");
  if (!out)
    return H2_PAL_ERR_IO;
  fprintf(out,
          "{\n  "
          "\"platform\":\"%s\",\"version\":\"%s\",\"contract\":%u,\"passed\":%"
          "zu,\"failed\":%zu,\"blocked\":%zu,\"not_run\":%zu,\"complete\":%d,"
          "\"qualified\":%d,\"cleanup\":%d,\"rc\":%d,\"teardown\":%d,"
          "\"balanced\":%d,\n  \"cases\":[\n",
          f->platform, f->image_version, H2_PAL_CORE_E2E_CONTRACT_VERSION,
          result.passed, result.failed, result.blocked, result.not_run,
          result.complete, result.qualified, result.cleanup_result, rc,
          teardown, balanced);
  for (size_t i = 0; i < H2_PAL_CORE_E2E_CASE_COUNT; ++i)
    fprintf(out, "    {\"id\":\"%s\",\"status\":\"%s\",\"rc\":%d}%s\n",
            result.cases[i].id ? result.cases[i].id : "",
            h2_pal_core_e2e_status_name(result.cases[i].status),
            result.cases[i].result,
            i + 1u < H2_PAL_CORE_E2E_CASE_COUNT ? "," : "");
  fprintf(out,
          "  "
          "],\"before\":{\"tasks\":%zu,\"stacks\":%zu,\"queues\":%zu,"
          "\"mutexes\":%zu,\"semaphores\":%zu,\"conditions\":%zu,\"timers\":%"
          "zu,\"allocations\":%zu,\"bytes\":%zu},\n",
          before.tasks, before.task_stack_bytes, before.queues, before.mutexes,
          before.semaphores, before.conditions, before.timers,
          before.allocations, before.allocation_bytes);
  fprintf(out,
          "  "
          "\"after\":{\"tasks\":%zu,\"stacks\":%zu,\"queues\":%zu,\"mutexes\":%"
          "zu,\"semaphores\":%zu,\"conditions\":%zu,\"timers\":%zu,"
          "\"allocations\":%zu,\"bytes\":%zu}\n}\n",
          after.tasks, after.task_stack_bytes, after.queues, after.mutexes,
          after.semaphores, after.conditions, after.timers, after.allocations,
          after.allocation_bytes);
  fclose(out);
  printf("H2_MOBILE_CORE_REPORT platform=%s passed=%zu failed=%zu blocked=%zu "
         "qualified=%d cleanup=%d teardown=%d balanced=%d\n",
         f->platform, result.passed, result.failed, result.blocked,
         result.qualified, result.cleanup_result, teardown, balanced);
  fflush(stdout);
  return rc == 0 && result.qualified && teardown == 0 && balanced
             ? 0
             : H2_PAL_ERR_INVALID_STATE;
}
