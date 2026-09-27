#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "host_observer.h"
#include "h2_desktop_platform.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <pthread.h>

#ifndef H2_PAL_CORE_IMAGE_VERSION
#error "The launcher must embed its image version at build time"
#endif
#define H2_STRINGIFY_VALUE_(value) #value
#define H2_STRINGIFY_VALUE(value) H2_STRINGIFY_VALUE_(value)
namespace {
struct State {
  std::mutex mutex;
  std::map<void *, size_t> allocations;
  size_t bytes = 0;
  int fail_after = -1;
};
State &state() {
  static State s;
  return s;
}
void *allocate(void *, size_t size) {
  auto *p = h2_pal_mem_alloc(h2_desktop_platform_default_allocator(), size);
  if (p) {
    auto &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.allocations.emplace(p, size);
    s.bytes += size;
  }
  return p;
}
void deallocate(void *, void *p) {
  if (!p)
    return;
  auto &s = state();
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    auto i = s.allocations.find(p);
    if (i != s.allocations.end()) {
      s.bytes -= i->second;
      s.allocations.erase(i);
    }
  }
  h2_pal_mem_free(h2_desktop_platform_default_allocator(), p);
}
void *reallocate(void *, void *p, size_t size) {
  if (!p)
    return allocate(nullptr, size);
  auto &s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  auto i = s.allocations.find(p);
  if (i == s.allocations.end())
    return nullptr;
  size_t old = i->second;
  void *next =
      h2_pal_mem_realloc(h2_desktop_platform_default_allocator(), p, size);
  if (next || size == 0) {
    s.bytes -= old;
    s.allocations.erase(i);
  }
  if (next) {
    s.allocations.emplace(next, size);
    s.bytes += size;
  }
  return next;
}
const h2_pal_mem_vtable_t allocator_vtable = {allocate, reallocate, deallocate};
const h2_pal_mem_api_t allocator = {nullptr, &allocator_vtable};
void *stack_allocate(void *, size_t size) {
  {
    auto &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.fail_after == 0)
      return nullptr;
    if (s.fail_after > 0)
      --s.fail_after;
  }
  return allocate(nullptr, size);
}
const h2_pal_mem_vtable_t stack_vtable = {stack_allocate, reallocate,
                                          deallocate};
const h2_pal_mem_api_t stack_allocator = {nullptr, &stack_vtable};
h2_pal_result_t resolve_stack(void *, const h2_pal_task_options_t *options,
                              size_t *bytes) {
  *bytes = std::max<size_t>(4096u, options ? options->min_stack_size : 0u);
  return H2_PAL_OK;
}
struct Firmware {
  h2_desktop_firmware_info_t *owner = nullptr;
  Firmware() {
    (void)h2_desktop_firmware_info_create(
        H2_STRINGIFY_VALUE(H2_PAL_CORE_IMAGE_VERSION), &owner);
  }
  ~Firmware() { h2_desktop_firmware_info_destroy(owner); }
};
} // namespace
extern "C" const h2_pal_mem_api_t *h2_pal_core_host_allocator(void) {
  return &allocator;
}
extern "C" const h2_pal_firmware_info_api_t *
h2_pal_core_host_firmware_info(void) {
  static Firmware firmware;
  return h2_desktop_firmware_info_api(firmware.owner);
}
extern "C" const char *h2_pal_core_host_build_version(void) {
  return H2_STRINGIFY_VALUE(H2_PAL_CORE_IMAGE_VERSION);
}
extern "C" h2_pal_result_t h2_pal_core_host_observer_start(void) {
  const h2_desktop_task_stack_config_t cfg = {&stack_allocator, resolve_stack,
                                              nullptr};
  return h2_desktop_platform_configure_task_stacks(&cfg);
}
extern "C" h2_pal_result_t h2_pal_core_host_observer_stop(void) {
  (void)h2_pal_core_host_task_fault(nullptr, -1);
  return h2_desktop_platform_configure_task_stacks(nullptr);
}
extern "C" h2_pal_result_t
h2_pal_core_host_resources(void *, h2_pal_core_resources_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  h2_desktop_platform_resource_stats_t stats{};
  auto rc = h2_desktop_platform_get_resource_stats(&stats);
  if (rc != H2_PAL_OK)
    return rc;
  *out = {};
  out->tasks = stats.live_tasks;
  out->task_stack_bytes = stats.task_stack_bytes;
  out->queues = stats.live_queues;
  out->mutexes = stats.live_mutexes;
  out->semaphores = stats.live_semaphores;
  out->conditions = stats.live_conditions;
  out->timers = stats.live_timers;
  out->firmware_infos = stats.live_firmware_infos;
  auto &s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  out->allocations = s.allocations.size();
  out->allocation_bytes = s.bytes;
  return H2_PAL_OK;
}
extern "C" h2_pal_result_t h2_pal_core_host_stack(void *, size_t *bytes) {
  if (!bytes)
    return H2_PAL_ERR_INVALID_ARG;
#if defined(__APPLE__)
  *bytes = pthread_get_stacksize_np(pthread_self());
  return *bytes ? H2_PAL_OK : H2_PAL_ERR_IO;
#else
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) != 0)
    return H2_PAL_ERR_IO;
  int rc = pthread_attr_getstacksize(&attr, bytes);
  pthread_attr_destroy(&attr);
  return rc == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
#endif
}
extern "C" h2_pal_result_t h2_pal_core_host_task_fault(void *, int after) {
  if (after < -1)
    return H2_PAL_ERR_INVALID_ARG;
  auto &s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.fail_after = after;
  return H2_PAL_OK;
}
