#include "atomic_mobile_runner.h"
#include "h2_atomic_e2e.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef __ANDROID__
#include "h2_android_platform.h"
typedef h2_android_platform_resource_stats_t resource_stats_t;
#define get_stats h2_android_platform_get_resource_stats
#else
#include "h2_ios_platform.h"
typedef h2_ios_platform_resource_stats_t resource_stats_t;
#define get_stats h2_ios_platform_get_resource_stats
#endif
static void print_stats(FILE *f, const resource_stats_t *s) {
  fprintf(f,
          "{\"tasks\":%zu,\"stack_bytes\":%zu,\"queues\":%zu,\"mutexes\":%zu,"
          "\"semaphores\":%zu,\"conditions\":%zu,\"timers\":%zu,"
          "\"allocations\":%zu,\"allocation_bytes\":%zu}",
          s->tasks, s->task_stack_bytes, s->queues, s->mutexes, s->semaphores,
          s->conditions, s->timers, s->allocations, s->allocation_bytes);
}
int h2_atomic_mobile_run(h2_runtime_config_t config, const char *platform,
                         const char *version, const char *path,
                         int (*shutdown)(void)) {
  h2_atomic_qualification_config_t atomic = {.mem = config.mem,
                                             .task = config.task,
                                             .time = config.time,
                                             .expected_core = {-1, -1}};
  h2_atomic_qualification_result_t result;
  resource_stats_t before = {0}, after = {0};
  int baseline = get_stats(&before);
  int rc = h2_atomic_e2e_qualify(&atomic, &result);
  int snapshot = get_stats(&after);
  h2_atomic_e2e_print(platform, "native", &result);
  int teardown = shutdown();
  if (!teardown)
    teardown = result.teardown;
  if (!teardown)
    teardown = baseline   ? baseline
               : snapshot ? snapshot
               : memcmp(&before, &after, sizeof(before))
                   ? H2_PAL_ERR_INVALID_STATE
                   : 0;
  if (teardown)
    result.qualified = false;
  FILE *f = fopen(path, "w");
  if (!f)
    return H2_PAL_ERR_IO;
  fprintf(f,
          "{\"platform\":\"%s\",\"version\":\"%s\",\"pid\":%ld,\"contract\":1,"
          "\"passed\":%u,\"failed\":%u,\"not_run\":%u,\"complete\":%u,"
          "\"qualified\":%u,"
          "\"workers_started\":%u,\"workers_joined\":%u,\"rc\":%d,\"teardown\":"
          "%d,\"cases\":[",
          platform, version, (long)getpid(), result.passed, result.failed,
          result.not_run, result.complete, result.qualified,
          result.workers_started, result.workers_joined, rc, teardown);
  for (unsigned i = 0; i < H2_ATOMIC_QUALIFICATION_CASE_COUNT; ++i) {
    const h2_atomic_qualification_case_t *c = &result.cases[i];
    fprintf(f, "%s{\"id\":\"%s\",\"status\":\"%s\",\"rc\":%d}", i ? "," : "",
            c->id,
            c->status == 1   ? "PASS"
            : c->status == 2 ? "FAIL"
                             : "NOT_RUN",
            c->rc);
  }
  fputs("],\"before\":", f);
  print_stats(f, &before);
  fputs(",\"after\":", f);
  print_stats(f, &after);
  fputs("}\n", f);
  if (fclose(f))
    return H2_PAL_ERR_IO;
  return rc || teardown || !result.qualified ? H2_PAL_ERR_INVALID_STATE
                                             : H2_PAL_OK;
}
