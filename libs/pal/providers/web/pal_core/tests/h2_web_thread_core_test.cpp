#include "h2_web_thread_core.h"

#include <atomic>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <unistd.h>
#include <vector>

namespace {
struct Allocation {
  void *pointer;
  size_t bytes;
};
struct Heap {
  std::mutex mutex;
  std::vector<Allocation> allocations;
  bool fail = false;
};
void *allocate(void *user, size_t size) {
  auto *heap = static_cast<Heap *>(user);
  std::lock_guard<std::mutex> lock(heap->mutex);
  if (heap->fail)
    return nullptr;
  void *pointer = std::malloc(size);
  if (pointer != nullptr)
    heap->allocations.push_back({pointer, size});
  return pointer;
}
void release(void *user, void *pointer) {
  auto *heap = static_cast<Heap *>(user);
  std::lock_guard<std::mutex> lock(heap->mutex);
  for (auto at = heap->allocations.begin(); at != heap->allocations.end();
       ++at) {
    if (at->pointer == pointer) {
      heap->allocations.erase(at);
      std::free(pointer);
      return;
    }
  }
  assert(false);
}
bool owns(Heap &heap, const void *pointer) {
  const auto address = reinterpret_cast<uintptr_t>(pointer);
  std::lock_guard<std::mutex> lock(heap.mutex);
  for (const auto &allocation : heap.allocations) {
    const auto base = reinterpret_cast<uintptr_t>(allocation.pointer);
    if (address >= base && address - base < allocation.bytes)
      return true;
  }
  return false;
}
h2_web_thread_core_t *core;
const h2_pal_task_api_t *tasks;
const h2_pal_sync_api_t *sync_api;
const h2_pal_queue_api_t *queues;
const h2_pal_timer_api_t *timers;
h2_pal_mem_api_t memory;

h2_web_thread_core_resource_stats_t stats() {
  h2_web_thread_core_resource_stats_t result{};
  assert(h2_web_thread_core_get_resource_stats(core, &result) == H2_PAL_OK);
  return result;
}
h2_pal_task_t *start(h2_pal_task_entry_t entry, void *user, size_t stack = 0u) {
  h2_pal_task_t *task = nullptr;
  const h2_pal_task_options_t options = {"thread-core-test", stack};
  assert(h2_pal_task_start(tasks, &options, entry, user, &task) == H2_PAL_OK);
  return task;
}
void join(h2_pal_task_t *task) {
  assert(h2_pal_task_join(tasks, task) == H2_PAL_OK);
}

struct Parallel {
  h2_pal_mutex_t *mutex = nullptr;
  h2_pal_cond_t *condition = nullptr;
  unsigned ready = 0u, counter = 0u;
  bool go = false;
  pthread_t identities[2]{};
  const void *stacks[2]{};
};
struct Worker {
  Parallel *state;
  size_t index;
};
void parallel_entry(void *user) {
  auto *worker = static_cast<Worker *>(user);
  Parallel &state = *worker->state;
  int actual_stack = 1;
  assert(h2_pal_mutex_lock(sync_api, state.mutex) == H2_PAL_OK);
  state.identities[worker->index] = pthread_self();
  state.stacks[worker->index] = &actual_stack;
  ++state.ready;
  // Both the owner and workers wait on this condition with different
  // predicates. Wake all so a worker cannot consume the readiness signal.
  assert(h2_pal_cond_broadcast(sync_api, state.condition) == H2_PAL_OK);
  while (!state.go)
    assert(h2_pal_cond_wait(sync_api, state.condition, state.mutex, 2000u) ==
           H2_PAL_OK);
  assert(h2_pal_mutex_unlock(sync_api, state.mutex) == H2_PAL_OK);
  for (unsigned i = 0; i < 10000u; ++i) {
    assert(h2_pal_mutex_lock(sync_api, state.mutex) == H2_PAL_OK);
    ++state.counter;
    assert(h2_pal_mutex_unlock(sync_api, state.mutex) == H2_PAL_OK);
  }
}
void test_parallel_and_real_stacks(Heap &heap) {
  Parallel state;
  const h2_pal_mutex_config_t mutex_config = {"parallel", &memory, 0u};
  const h2_pal_cond_config_t cond_config = {"parallel", &memory};
  assert(h2_pal_mutex_create(sync_api, &mutex_config, &state.mutex) ==
         H2_PAL_OK);
  assert(h2_pal_cond_create(sync_api, &cond_config, &state.condition) ==
         H2_PAL_OK);
  Worker workers[] = {{&state, 0u}, {&state, 1u}};
  h2_pal_task_t *first = start(parallel_entry, &workers[0], 4096u);
  h2_pal_task_t *second = start(parallel_entry, &workers[1], 65536u);
  assert(h2_pal_mutex_lock(sync_api, state.mutex) == H2_PAL_OK);
  while (state.ready != 2u)
    assert(h2_pal_cond_wait(sync_api, state.condition, state.mutex, 2000u) ==
           H2_PAL_OK);
  assert(!pthread_equal(state.identities[0], state.identities[1]));
  assert(!pthread_equal(state.identities[0], pthread_self()));
  assert(owns(heap, state.stacks[0]) && owns(heap, state.stacks[1]));
  auto live = stats();
  assert(live.live_tasks == 2u && live.task_stack_bytes >= 2u * 65536u);
  assert(h2_web_thread_core_destroy(&core) == H2_PAL_ERR_BUSY);
  state.go = true;
  assert(h2_pal_cond_broadcast(sync_api, state.condition) == H2_PAL_OK);
  assert(h2_pal_mutex_unlock(sync_api, state.mutex) == H2_PAL_OK);
  join(first);
  join(second);
  assert(state.counter == 20000u && stats().live_tasks == 0u);
  assert(h2_pal_cond_destroy(sync_api, state.condition) == H2_PAL_OK);
  assert(h2_pal_mutex_destroy(sync_api, state.mutex) == H2_PAL_OK);
}

void once(void *user) { ++*static_cast<unsigned *>(user); }
void test_churn_and_start_failure(Heap &heap) {
  for (unsigned i = 0u; i < 100u; ++i) {
    unsigned calls = 0u;
    auto *task = start(once, &calls);
    join(task);
    assert(calls == 1u && stats().live_tasks == 0u &&
           stats().task_stack_bytes == 0u);
    assert(heap.allocations.empty());
  }
  heap.fail = true;
  h2_pal_task_t *task = nullptr;
  unsigned calls = 0u;
  assert(h2_pal_task_start(tasks, nullptr, once, &calls, &task) ==
         H2_PAL_ERR_NO_MEMORY);
  assert(task == nullptr && calls == 0u && stats().live_tasks == 0u);
  heap.fail = false;
}

struct Cancel {
  h2_pal_queue_t *queue = nullptr;
  h2_pal_mutex_t *mutex = nullptr;
  h2_pal_cond_t *condition = nullptr;
  std::atomic<bool> entered{false};
  h2_pal_result_t result = H2_PAL_OK;
  bool owned_after = false;
};
void queue_wait(void *user) {
  auto *state = static_cast<Cancel *>(user);
  state->entered.store(true);
  int value;
  state->result = static_cast<h2_pal_result_t>(
      h2_pal_queue_recv(queues, state->queue, &value, UINT32_MAX));
}
void condition_wait(void *user) {
  auto *state = static_cast<Cancel *>(user);
  assert(h2_pal_mutex_lock(sync_api, state->mutex) == H2_PAL_OK);
  state->entered.store(true);
  state->result =
      h2_pal_cond_wait(sync_api, state->condition, state->mutex, UINT32_MAX);
  state->owned_after =
      h2_pal_mutex_try_lock(sync_api, state->mutex) == H2_PAL_ERR_BUSY;
  assert(h2_pal_mutex_unlock(sync_api, state->mutex) == H2_PAL_OK);
}
void await(std::atomic<bool> &flag) {
  for (int i = 0; i < 2000 && !flag.load(); ++i)
    usleep(1000);
  assert(flag.load());
}
void test_queue_and_cancel() {
  Cancel state;
  const h2_pal_queue_config_t queue_config = {"queue", sizeof(int), 2u,
                                              &memory};
  assert(h2_pal_queue_create(queues, &queue_config, &state.queue) == H2_PAL_OK);
  int one = 1, two = 2, value = 0;
  assert(h2_pal_queue_send(queues, state.queue, &one, 0u) == H2_PAL_OK);
  assert(h2_pal_queue_send(queues, state.queue, &two, 0u) == H2_PAL_OK);
  assert(h2_pal_queue_close(queues, state.queue) == H2_PAL_OK);
  assert(h2_pal_queue_recv(queues, state.queue, &value, 0u) == H2_PAL_OK &&
         value == 1);
  assert(h2_pal_queue_recv(queues, state.queue, &value, 0u) == H2_PAL_OK &&
         value == 2);
  assert(h2_pal_queue_recv(queues, state.queue, &value, 0u) ==
         H2_PAL_ERR_CLOSED);
  assert(h2_pal_queue_reset(queues, state.queue) == H2_PAL_OK);
  auto *task = start(queue_wait, &state);
  await(state.entered);
  assert(h2_web_thread_core_task_cancel(core, task) == H2_PAL_OK);
  join(task);
  assert(state.result == H2_PAL_EXIT);
  h2_pal_queue_destroy(queues, state.queue);
  const h2_pal_mutex_config_t mutex_config = {"cancel-cond", &memory, 0u};
  const h2_pal_cond_config_t cond_config = {"cancel-cond", &memory};
  assert(h2_pal_mutex_create(sync_api, &mutex_config, &state.mutex) ==
         H2_PAL_OK);
  assert(h2_pal_cond_create(sync_api, &cond_config, &state.condition) ==
         H2_PAL_OK);
  state.entered.store(false);
  task = start(condition_wait, &state);
  await(state.entered);
  assert(h2_pal_mutex_lock(sync_api, state.mutex) == H2_PAL_OK);
  assert(h2_pal_mutex_unlock(sync_api, state.mutex) == H2_PAL_OK);
  assert(h2_web_thread_core_task_cancel(core, task) == H2_PAL_OK);
  join(task);
  assert(state.result == H2_PAL_EXIT && state.owned_after);
  assert(h2_pal_cond_destroy(sync_api, state.condition) == H2_PAL_OK);
  assert(h2_pal_mutex_destroy(sync_api, state.mutex) == H2_PAL_OK);
}

struct Ticks {
  h2_pal_queue_t *queue;
};
void tick(void *user, h2_pal_timer_t *) {
  auto *ticks = static_cast<Ticks *>(user);
  int value = 81;
  (void)h2_pal_queue_send(queues, ticks->queue, &value, 0u);
}
void test_timer() {
  Ticks ticks{};
  const h2_pal_queue_config_t queue_config = {"timer", sizeof(int), 32u,
                                              &memory};
  assert(h2_pal_queue_create(queues, &queue_config, &ticks.queue) == H2_PAL_OK);
  const h2_pal_timer_config_t config = {
      "repeat", 2u, H2_PAL_TIMER_FLAG_REPEAT | H2_PAL_TIMER_FLAG_AUTO_START,
      tick, &ticks};
  h2_pal_timer_t *timer = nullptr;
  assert(h2_pal_timer_create(timers, &config, &timer) == H2_PAL_OK);
  for (int i = 0; i < 3; ++i) {
    int value = 0;
    assert(h2_pal_queue_recv(queues, ticks.queue, &value, 2000u) == H2_PAL_OK &&
           value == 81);
  }
  assert(h2_pal_timer_stop(timers, timer) == H2_PAL_OK);
  assert(h2_pal_queue_reset(queues, ticks.queue) == H2_PAL_OK);
  int value;
  assert(h2_pal_queue_recv(queues, ticks.queue, &value, 20u) ==
         H2_PAL_ERR_TIMEOUT);
  assert(h2_pal_timer_set_period_ms(timers, timer, 1u) == H2_PAL_OK);
  assert(h2_pal_timer_reset(timers, timer) == H2_PAL_OK);
  assert(h2_pal_queue_recv(queues, ticks.queue, &value, 2000u) == H2_PAL_OK);
  assert(h2_pal_timer_destroy(timers, timer) == H2_PAL_OK);
  h2_pal_queue_destroy(queues, ticks.queue);
}
} // namespace

int main() {
  alarm(30u);
  Heap heap;
  const h2_pal_mem_vtable_t vtable = {allocate, nullptr, release};
  memory = {&heap, &vtable};
  assert(h2_web_thread_core_create(&memory, &core) == H2_PAL_OK);
  tasks = h2_web_thread_core_task_api(core);
  queues = h2_web_thread_core_queue_api(core);
  sync_api = h2_web_thread_core_sync_api(core);
  timers = h2_web_thread_core_timer_api(core);
  test_parallel_and_real_stacks(heap);
  test_churn_and_start_failure(heap);
  test_queue_and_cancel();
  test_timer();
  auto final = stats();
  assert(final.live_tasks == 0 && final.task_stack_bytes == 0 &&
         final.live_queues == 0 && final.live_mutexes == 0 &&
         final.live_semaphores == 0 && final.live_conditions == 0 &&
         final.live_timers == 0);
  assert(heap.allocations.empty());
  assert(h2_web_thread_core_destroy(&core) == H2_PAL_OK && core == nullptr);
  return 0;
}
