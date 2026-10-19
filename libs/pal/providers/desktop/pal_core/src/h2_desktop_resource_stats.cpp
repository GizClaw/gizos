#include "h2_desktop_platform.h"
#include "h2_desktop_resource_stats_internal.h"

#include <cassert>
#include <mutex>

namespace {
std::mutex stats_mutex;
h2_desktop_platform_resource_stats_t stats{};

size_t &counter(h2_desktop_resource_kind kind) {
  switch (kind) {
  case h2_desktop_resource_kind::task:
    return stats.live_tasks;
  case h2_desktop_resource_kind::queue:
    return stats.live_queues;
  case h2_desktop_resource_kind::mutex:
    return stats.live_mutexes;
  case h2_desktop_resource_kind::semaphore:
    return stats.live_semaphores;
  case h2_desktop_resource_kind::condition:
    return stats.live_conditions;
  case h2_desktop_resource_kind::timer:
    return stats.live_timers;
  case h2_desktop_resource_kind::firmware_info:
    return stats.live_firmware_infos;
  }
  // All callers are provider-local and pass the closed enum above.
  assert(false);
  return stats.live_tasks;
}
} // namespace

void h2_desktop_resource_acquire(h2_desktop_resource_kind kind) {
  std::lock_guard<std::mutex> lock(stats_mutex);
  ++counter(kind);
}

void h2_desktop_resource_release(h2_desktop_resource_kind kind) {
  std::lock_guard<std::mutex> lock(stats_mutex);
  assert(counter(kind) != 0u);
  --counter(kind);
}

void h2_desktop_task_stack_acquire(size_t bytes) {
  std::lock_guard<std::mutex> lock(stats_mutex);
  stats.task_stack_bytes += bytes;
}

void h2_desktop_task_stack_release(size_t bytes) {
  std::lock_guard<std::mutex> lock(stats_mutex);
  assert(stats.task_stack_bytes >= bytes);
  stats.task_stack_bytes -= bytes;
}

extern "C" h2_pal_result_t h2_desktop_platform_get_resource_stats(
    h2_desktop_platform_resource_stats_t *out) {
  if (out == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(stats_mutex);
  *out = stats;
  return H2_PAL_OK;
}
