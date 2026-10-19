#include "h2_desktop_platform.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {
using namespace std::chrono_literals;
h2_desktop_platform_resource_stats_t snapshot() {
  h2_desktop_platform_resource_stats_t stats{};
  assert(h2_desktop_platform_get_resource_stats(&stats) == H2_PAL_OK);
  return stats;
}
struct Done {
  std::mutex mutex;
  std::condition_variable changed;
  bool value = false;
  uintptr_t stack_address = 0;
};
void finish(void *user) {
  auto *done = static_cast<Done *>(user);
  std::lock_guard<std::mutex> lock(done->mutex);
  int stack_local;
  done->stack_address = reinterpret_cast<uintptr_t>(&stack_local);
  done->value = true;
  done->changed.notify_all();
}
h2_pal_result_t stack_size(void *, const h2_pal_task_options_t *, size_t *out) {
  *out = 4096u;
  return H2_PAL_OK;
}
struct Allocator {
  size_t calls = 0u;
  size_t fail_at = 0u;
  size_t live = 0u;
  uintptr_t last_address = 0;
  size_t last_size = 0;
};
void *allocate(void *user, size_t len) {
  auto *allocator = static_cast<Allocator *>(user);
  if (++allocator->calls == allocator->fail_at)
    return nullptr;
  void *p = std::malloc(len);
  allocator->last_address = reinterpret_cast<uintptr_t>(p);
  allocator->last_size = len;
  if (p != nullptr)
    ++allocator->live;
  return p;
}
void release(void *user, void *p) {
  if (p != nullptr) {
    --static_cast<Allocator *>(user)->live;
    std::free(p);
  }
}
} // namespace

int main() {
  assert(h2_desktop_platform_get_resource_stats(nullptr) ==
         H2_PAL_ERR_INVALID_ARG);
  const auto baseline = snapshot();
  auto *mem = h2_desktop_platform_default_allocator();
  const auto *queue_api = h2_desktop_platform_queue_api();
  const auto *sync = h2_desktop_platform_sync_api();
  h2_pal_queue_t *queue = nullptr;
  h2_pal_mutex_t *mutex = nullptr;
  h2_pal_semaphore_t *semaphore = nullptr;
  h2_pal_cond_t *condition = nullptr;
  const h2_pal_queue_config_t queue_config = {"stats", sizeof(int), 2u, mem};
  const h2_pal_mutex_config_t mutex_config = {"stats", mem,
                                              H2_PAL_MUTEX_FLAG_NONE};
  const h2_pal_semaphore_config_t semaphore_config = {"stats", mem, 0u, 1u};
  const h2_pal_cond_config_t condition_config = {"stats", mem};
  assert(h2_pal_queue_create(queue_api, &queue_config, &queue) == H2_PAL_OK);
  assert(h2_pal_mutex_create(sync, &mutex_config, &mutex) == H2_PAL_OK);
  assert(h2_pal_semaphore_create(sync, &semaphore_config, &semaphore) ==
         H2_PAL_OK);
  assert(h2_pal_cond_create(sync, &condition_config, &condition) == H2_PAL_OK);
  auto active = snapshot();
  assert(active.live_queues == baseline.live_queues + 1u);
  assert(active.live_mutexes == baseline.live_mutexes + 1u);
  assert(active.live_semaphores == baseline.live_semaphores + 1u);
  assert(active.live_conditions == baseline.live_conditions + 1u);
  h2_pal_queue_destroy(queue_api, queue);
  assert(h2_pal_mutex_destroy(sync, mutex) == H2_PAL_OK);
  assert(h2_pal_semaphore_destroy(sync, semaphore) == H2_PAL_OK);
  assert(h2_pal_cond_destroy(sync, condition) == H2_PAL_OK);

  Allocator allocator;
  const h2_pal_mem_vtable_t allocator_vtable = {allocate, nullptr, release};
  const h2_pal_mem_api_t tracked = {&allocator, &allocator_vtable};
  h2_pal_queue_config_t fail_queue = queue_config;
  fail_queue.allocator = &tracked;
  allocator.fail_at = 2u; // real queue object allocated, backing storage fails
  assert(h2_pal_queue_create(queue_api, &fail_queue, &queue) ==
         H2_PAL_ERR_NO_MEMORY);
  assert(queue == nullptr && allocator.live == 0u);
  active = snapshot();
  assert(std::memcmp(&active, &baseline, sizeof(active)) == 0);

  const h2_desktop_task_stack_config_t stacks = {&tracked, stack_size, nullptr};
  assert(h2_desktop_platform_configure_task_stacks(&stacks) == H2_PAL_OK);
  allocator.calls = 0u;
  allocator.fail_at = 1u;
  Done done;
  const h2_pal_task_options_t options = {"stats", 4096u};
  const auto *task_api = h2_desktop_platform_task_api();
  h2_pal_task_t *task = nullptr;
  assert(h2_pal_task_start(task_api, &options, finish, &done, &task) ==
         H2_PAL_ERR_NO_MEMORY);
  assert(task == nullptr && allocator.live == 0u);
  active = snapshot();
  assert(std::memcmp(&active, &baseline, sizeof(active)) == 0);
  allocator.fail_at = 0u;
  assert(h2_pal_task_start(task_api, &options, finish, &done, &task) ==
         H2_PAL_OK);
  {
    std::unique_lock<std::mutex> lock(done.mutex);
    assert(done.changed.wait_for(lock, 2s, [&] { return done.value; }));
  }
  active = snapshot();
  assert(active.live_tasks == baseline.live_tasks + 1u);
  assert(active.task_stack_bytes >= baseline.task_stack_bytes + 4096u);
  assert(done.stack_address >= allocator.last_address);
  assert(done.stack_address < allocator.last_address + allocator.last_size);
  assert(h2_pal_task_join(task_api, task) == H2_PAL_OK);
  assert(allocator.live == 0u);
  assert(h2_desktop_platform_configure_task_stacks(nullptr) == H2_PAL_OK);
  active = snapshot();
  assert(std::memcmp(&active, &baseline, sizeof(active)) == 0);
  return 0;
}
