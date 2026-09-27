#include "h2_posix_core.h"
#include "h2_posix_thread_core.h"
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <pthread.h>

namespace {
pthread_mutex_t accounting = PTHREAD_MUTEX_INITIALIZER;
size_t allocations, allocation_bytes;
int stack_fail_after = -1;
/* The header preserves malloc's max_align_t guarantee and accounts exactly
 * the storage owned by callers of this allocator, including real task stacks.
 */
union Header {
  max_align_t alignment;
  struct {
    size_t size;
  } value;
};
void *allocate(void *, size_t size) {
  if (size > SIZE_MAX - sizeof(Header))
    return nullptr;
  Header *p = static_cast<Header *>(std::malloc(sizeof(Header) + size));
  if (!p)
    return nullptr;
  p->value.size = size;
  pthread_mutex_lock(&accounting);
  ++allocations;
  allocation_bytes += size;
  pthread_mutex_unlock(&accounting);
  return p + 1;
}
void release(void *, void *pointer) {
  if (!pointer)
    return;
  Header *p = static_cast<Header *>(pointer) - 1;
  pthread_mutex_lock(&accounting);
  assert(allocations && allocation_bytes >= p->value.size);
  --allocations;
  allocation_bytes -= p->value.size;
  pthread_mutex_unlock(&accounting);
  std::free(p);
}
void *resize(void *user, void *pointer, size_t size) {
  if (!pointer)
    return allocate(user, size);
  if (!size) {
    release(user, pointer);
    return nullptr;
  }
  if (size > SIZE_MAX - sizeof(Header))
    return nullptr;
  Header *p = static_cast<Header *>(pointer) - 1;
  const size_t old = p->value.size;
  Header *next = static_cast<Header *>(std::realloc(p, sizeof(Header) + size));
  if (!next)
    return nullptr;
  next->value.size = size;
  pthread_mutex_lock(&accounting);
  allocation_bytes = allocation_bytes - old + size;
  pthread_mutex_unlock(&accounting);
  return next + 1;
}
const h2_pal_mem_vtable_t memory_methods = {allocate, resize, release};
const h2_pal_mem_api_t memory = {nullptr, &memory_methods};
void *stack_allocate(void *user, size_t size) {
  pthread_mutex_lock(&accounting);
  bool reject = stack_fail_after == 0;
  if (stack_fail_after > 0)
    --stack_fail_after;
  pthread_mutex_unlock(&accounting);
  return reject ? nullptr : allocate(user, size);
}
const h2_pal_mem_vtable_t stack_methods = {stack_allocate, nullptr, release};
const h2_pal_mem_api_t stack_memory = {nullptr, &stack_methods};
pthread_mutex_t core_lock = PTHREAD_MUTEX_INITIALIZER;
h2_posix_thread_core_t *core;
h2_posix_thread_core_t *get_core() {
  pthread_mutex_lock(&core_lock);
  if (!core)
    (void)h2_posix_thread_core_create(&stack_memory, &core);
  auto *value = core;
  pthread_mutex_unlock(&core_lock);
  return value;
}
h2_pal_result_t monotonic_us(void *, uint64_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  timespec t{};
  if (clock_gettime(CLOCK_MONOTONIC, &t))
    return H2_PAL_ERR_IO;
  *out = static_cast<uint64_t>(t.tv_sec) * 1000000u +
         static_cast<uint64_t>(t.tv_nsec) / 1000u;
  return H2_PAL_OK;
}
h2_pal_result_t monotonic_ms(void *user, uint64_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  uint64_t us;
  auto rc = monotonic_us(user, &us);
  if (rc == H2_PAL_OK)
    *out = us / 1000u;
  return rc;
}
pthread_mutex_t wall_lock = PTHREAD_MUTEX_INITIALIZER;
bool wall_set;
uint64_t wall_base, wall_monotonic;
h2_pal_result_t wall_read(void *, uint64_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  pthread_mutex_lock(&wall_lock);
  if (wall_set) {
    uint64_t now;
    auto rc = monotonic_us(nullptr, &now);
    if (rc == H2_PAL_OK)
      *out = wall_base + (now - wall_monotonic) / 1000u;
    pthread_mutex_unlock(&wall_lock);
    return rc;
  }
  pthread_mutex_unlock(&wall_lock);
  timespec value{};
  if (clock_gettime(CLOCK_REALTIME, &value) || value.tv_sec < 0)
    return H2_PAL_ERR_IO;
  *out = static_cast<uint64_t>(value.tv_sec) * 1000u +
         static_cast<uint64_t>(value.tv_nsec) / 1000000u;
  return H2_PAL_OK;
}
h2_pal_result_t wall_write(void *, uint64_t value) {
  if (!value || value > INT64_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  uint64_t now;
  auto rc = monotonic_us(nullptr, &now);
  if (rc != H2_PAL_OK)
    return rc;
  pthread_mutex_lock(&wall_lock);
  wall_base = value;
  wall_monotonic = now;
  wall_set = true;
  pthread_mutex_unlock(&wall_lock);
  return H2_PAL_OK;
}
h2_pal_result_t wall_status(void *, h2_pal_time_wall_status_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  uint64_t value;
  auto rc = wall_read(nullptr, &value);
  if (rc != H2_PAL_OK)
    return rc;
  pthread_mutex_lock(&wall_lock);
  *out = {value != 0, wall_set ? H2_PAL_TIME_WALL_SOURCE_USER
                               : H2_PAL_TIME_WALL_SOURCE_RTC};
  pthread_mutex_unlock(&wall_lock);
  return H2_PAL_OK;
}
h2_pal_result_t sleep_ms(void *, uint32_t ms) {
  auto *owner = get_core();
  return owner ? h2_posix_thread_core_sleep_ms(owner, ms)
               : H2_PAL_ERR_NO_MEMORY;
}
const h2_pal_time_vtable_t time_methods = {
    monotonic_ms, monotonic_us, wall_read, wall_write, wall_status, sleep_ms};
const h2_pal_time_api_t time_api = {nullptr, &time_methods};
int log_write(void *, h2_pal_log_level_t level, const char *scope,
              const char *message) {
  static const char *names[] = {"debug", "info", "warn", "error"};
  if ((unsigned)level > H2_PAL_LOG_ERROR || !message)
    return H2_PAL_ERR_INVALID_ARG;
  return std::fprintf(stderr, "H2_LOG level=%s scope=%s message=%s\n",
                      names[level], scope ? scope : "h2", message) < 0
             ? H2_PAL_ERR_IO
             : H2_PAL_OK;
}
const h2_pal_log_vtable_t log_methods = {log_write};
const h2_pal_log_api_t log_api = {nullptr, &log_methods};
} // namespace
extern "C" const h2_pal_mem_api_t *h2_posix_core_mem_api(void) {
  return &memory;
}
extern "C" const h2_pal_time_api_t *h2_posix_core_time_api(void) {
  return &time_api;
}
extern "C" const h2_pal_log_api_t *h2_posix_core_log_api(void) {
  return &log_api;
}
extern "C" const h2_pal_task_api_t *h2_posix_core_task_api(void) {
  return h2_posix_thread_core_task_api(get_core());
}
extern "C" const h2_pal_queue_api_t *h2_posix_core_queue_api(void) {
  return h2_posix_thread_core_queue_api(get_core());
}
extern "C" const h2_pal_sync_api_t *h2_posix_core_sync_api(void) {
  return h2_posix_thread_core_sync_api(get_core());
}
extern "C" const h2_pal_timer_api_t *h2_posix_core_timer_api(void) {
  return h2_posix_thread_core_timer_api(get_core());
}
extern "C" h2_pal_result_t h2_posix_core_task_allocation_fault(int after) {
  if (after < -1)
    return H2_PAL_ERR_INVALID_ARG;
  pthread_mutex_lock(&accounting);
  stack_fail_after = after;
  pthread_mutex_unlock(&accounting);
  return H2_PAL_OK;
}
extern "C" h2_pal_result_t
h2_posix_core_resources(h2_posix_core_resources_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  h2_posix_thread_core_resource_stats_t values{};
  auto rc = h2_posix_thread_core_get_resource_stats(get_core(), &values);
  if (rc != H2_PAL_OK)
    return rc;
  *out = {values.live_tasks,
          values.task_stack_bytes,
          values.live_queues,
          values.live_mutexes,
          values.live_semaphores,
          values.live_conditions,
          values.live_timers,
          0,
          0};
  pthread_mutex_lock(&accounting);
  out->allocations = allocations;
  out->allocation_bytes = allocation_bytes;
  pthread_mutex_unlock(&accounting);
  return H2_PAL_OK;
}
extern "C" h2_pal_result_t h2_posix_core_shutdown(void) {
  pthread_mutex_lock(&core_lock);
  auto rc = h2_posix_thread_core_destroy(&core);
  pthread_mutex_unlock(&core_lock);
  return rc;
}
