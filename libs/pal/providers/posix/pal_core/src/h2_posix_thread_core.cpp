#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "h2_posix_thread_core.h"

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <new>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#if defined(__EMSCRIPTEN__)
#include <emscripten/threading.h>
#endif

struct h2_posix_thread_core {
  h2_pal_mem_api_t allocator{};
  pthread_key_t task_key{};
  pthread_mutex_t task_mutex{};
  void *task_head = nullptr;
  std::atomic<size_t> pending{0}, tasks{0}, stack_bytes{0}, queues{0},
      mutexes{0};
  std::atomic<size_t> semaphores{0}, conditions{0}, timers{0};
  h2_pal_task_api_t task_api{};
  h2_pal_queue_api_t queue_api{};
  h2_pal_sync_api_t sync_api{};
  h2_pal_timer_api_t timer_api{};
};

namespace {
constexpr uint64_t slice_ns = 10000000u;
constexpr size_t default_stack_size = 65536u;

bool ui_thread() {
#if defined(__EMSCRIPTEN__)
  return emscripten_is_main_browser_thread();
#else
  return false;
#endif
}

uint64_t mono_ns() {
  timespec value{};
  clock_gettime(CLOCK_MONOTONIC, &value);
  return static_cast<uint64_t>(value.tv_sec) * 1000000000u + value.tv_nsec;
}

uint64_t deadline(uint32_t timeout_ms) {
  if (timeout_ms == UINT32_MAX)
    return UINT64_MAX;
  const uint64_t now = mono_ns();
  const uint64_t duration = static_cast<uint64_t>(timeout_ms) * 1000000u;
  return now > UINT64_MAX - duration ? UINT64_MAX : now + duration;
}

h2_pal_result_t error(int rc) {
  switch (rc) {
  case 0:
    return H2_PAL_OK;
  case ENOMEM:
    return H2_PAL_ERR_NO_MEMORY;
  case EAGAIN:
    return H2_PAL_ERR_UNAVAILABLE;
  case EBUSY:
    return H2_PAL_ERR_BUSY;
  case ETIMEDOUT:
    return H2_PAL_ERR_TIMEOUT;
  case EINVAL:
    return H2_PAL_ERR_INVALID_ARG;
  case EDEADLK:
  case EPERM:
  case ESRCH:
    return H2_PAL_ERR_INVALID_STATE;
  default:
    return H2_PAL_ERR_IO;
  }
}

int init_cond(pthread_cond_t *cv) {
  pthread_condattr_t attr;
  int rc = pthread_condattr_init(&attr);
  if (rc != 0)
    return rc;
#if !defined(__APPLE__)
  rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
  if (rc == 0)
    rc = pthread_cond_init(cv, &attr);
  pthread_condattr_destroy(&attr);
  return rc;
}

// Always releases and reacquires the supplied real pthread mutex. Slicing is
// for cooperative cancellation, not polling without sleeping on a Worker.
int wait_slice(pthread_cond_t *cv, pthread_mutex_t *mutex, uint64_t until) {
  const uint64_t now = mono_ns();
  if (until <= now)
    return ETIMEDOUT;
  const uint64_t duration = until - now < slice_ns ? until - now : slice_ns;
#if defined(__APPLE__)
  const timespec relative = {static_cast<time_t>(duration / 1000000000u),
                             static_cast<long>(duration % 1000000000u)};
  return pthread_cond_timedwait_relative_np(cv, mutex, &relative);
#else
  const uint64_t end = now + duration;
  const timespec absolute = {static_cast<time_t>(end / 1000000000u),
                             static_cast<long>(end % 1000000000u)};
  return pthread_cond_timedwait(cv, mutex, &absolute);
#endif
}

struct Guard {
  pthread_mutex_t *mutex;
  int rc;
  bool held;
  explicit Guard(pthread_mutex_t *value, bool ui_safe = true)
      : mutex(value), rc(ui_safe && ui_thread() ? pthread_mutex_trylock(value)
                                                : pthread_mutex_lock(value)),
        held(rc == 0) {}
  ~Guard() {
    if (held)
      pthread_mutex_unlock(mutex);
  }
  void unlock() {
    if (held) {
      pthread_mutex_unlock(mutex);
      held = false;
    }
  }
};

struct Pending {
  h2_posix_thread_core_t *core;
  explicit Pending(h2_posix_thread_core_t *value) : core(value) {
    ++core->pending;
  }
  ~Pending() { --core->pending; }
};

bool valid_allocator(const h2_pal_mem_api_t *allocator) {
  return allocator != nullptr && allocator->vtable != nullptr &&
         allocator->vtable->alloc != nullptr &&
         allocator->vtable->free != nullptr;
}

template <typename T> T *make_object(const h2_pal_mem_api_t *allocator) {
  if (!valid_allocator(allocator))
    return nullptr;
  void *storage = h2_pal_mem_alloc(allocator, sizeof(T));
  return storage == nullptr ? nullptr : new (storage) T();
}

template <typename T> void free_object(T *object) {
  const h2_pal_mem_api_t allocator = object->allocator;
  object->~T();
  h2_pal_mem_free(&allocator, object);
}

struct Task {
  h2_posix_thread_core_t *core = nullptr;
  Task *next = nullptr;
  pthread_t thread{};
  pthread_mutex_t sleep_mutex{};
  pthread_cond_t sleep_changed{};
  std::atomic<bool> cancelled{false};
  std::atomic<bool> joining{false};
  std::atomic<bool> native_joined{false};
  bool sleep_cond_live = true, sleep_mutex_live = true;
  h2_pal_task_entry_t entry = nullptr;
  void *user = nullptr;
  void *stack_allocation = nullptr;
  size_t stack_size = 0u;
};

Task *current(h2_posix_thread_core_t *core) {
  return core == nullptr
             ? nullptr
             : static_cast<Task *>(pthread_getspecific(core->task_key));
}

bool cancelled(h2_posix_thread_core_t *core) {
  Task *task = current(core);
  return task != nullptr && task->cancelled.load(std::memory_order_acquire);
}

void *task_entry(void *user) {
  Task *task = static_cast<Task *>(user);
  pthread_setspecific(task->core->task_key, task);
  task->entry(task->user);
  pthread_setspecific(task->core->task_key, nullptr);
  return nullptr;
}

int native_join(pthread_t thread) {
#if defined(__EMSCRIPTEN__)
  if (ui_thread())
    return pthread_tryjoin_np(thread, nullptr);
#endif
  return pthread_join(thread, nullptr);
}

int task_start(void *user, const h2_pal_task_options_t *options,
               h2_pal_task_entry_t entry, void *context, h2_pal_task_t **out) {
  auto *core = static_cast<h2_posix_thread_core_t *>(user);
  if (out != nullptr)
    *out = nullptr;
  if (core == nullptr || out == nullptr || entry == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  Pending pending(core);
  size_t alignment = 16u;
#if !defined(__EMSCRIPTEN__)
  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size > 0)
    alignment = static_cast<size_t>(page_size);
#endif
  size_t bytes = options == nullptr ? 0u : options->min_stack_size;
  if (bytes < default_stack_size)
    bytes = default_stack_size;
  const size_t native_minimum = static_cast<size_t>(PTHREAD_STACK_MIN);
  if (bytes < native_minimum)
    bytes = native_minimum;
#if defined(__ANDROID__)
  // Bionic carves pthread_internal_t out of a caller-provided stack and
  // subtracts it from pthread_getattr_np's usable size. Reserve native thread
  // metadata separately so min_stack_size remains a usable-stack guarantee.
  const size_t native_overhead = native_minimum;
  if (bytes > SIZE_MAX - native_overhead)
    return H2_PAL_ERR_INVALID_ARG;
  bytes += native_overhead;
#endif
  if (bytes > SIZE_MAX - (alignment - 1u))
    return H2_PAL_ERR_INVALID_ARG;
  bytes = (bytes + alignment - 1u) / alignment * alignment;
  if (bytes > SIZE_MAX - (alignment - 1u))
    return H2_PAL_ERR_INVALID_ARG;
  Task *task = new (std::nothrow) Task();
  if (task == nullptr)
    return H2_PAL_ERR_NO_MEMORY;
  task->core = core;
  task->entry = entry;
  task->user = context;
  task->stack_size = bytes;
  int rc = pthread_mutex_init(&task->sleep_mutex, nullptr);
  if (rc != 0) {
    delete task;
    return error(rc);
  }
  rc = init_cond(&task->sleep_changed);
  if (rc != 0) {
    pthread_mutex_destroy(&task->sleep_mutex);
    delete task;
    return error(rc);
  }
  task->stack_allocation =
      h2_pal_mem_alloc(&core->allocator, bytes + alignment - 1u);
  if (task->stack_allocation == nullptr) {
    pthread_cond_destroy(&task->sleep_changed);
    pthread_mutex_destroy(&task->sleep_mutex);
    delete task;
    return H2_PAL_ERR_NO_MEMORY;
  }
  const uintptr_t raw = reinterpret_cast<uintptr_t>(task->stack_allocation);
  void *stack =
      reinterpret_cast<void *>((raw + alignment - 1u) / alignment * alignment);
  pthread_attr_t attr;
  rc = pthread_attr_init(&attr);
  if (rc == 0) {
    rc = pthread_attr_setstack(&attr, stack, bytes);
    if (rc == 0) {
      Guard registry(&core->task_mutex);
      rc = registry.rc;
      if (rc == 0) {
        task->next = static_cast<Task *>(core->task_head);
        core->task_head = task;
        ++core->tasks;
        core->stack_bytes += bytes;
        rc = pthread_create(&task->thread, &attr, task_entry, task);
        if (rc != 0) {
          core->task_head = task->next;
          --core->tasks;
          core->stack_bytes -= bytes;
        }
      }
    }
    pthread_attr_destroy(&attr);
  }
  if (rc != 0) {
    h2_pal_mem_free(&core->allocator, task->stack_allocation);
    pthread_cond_destroy(&task->sleep_changed);
    pthread_mutex_destroy(&task->sleep_mutex);
    delete task;
    return error(rc);
  }
  *out = reinterpret_cast<h2_pal_task_t *>(task);
  return H2_PAL_OK;
}

Task *find_task(h2_posix_thread_core_t *core, h2_pal_task_t *opaque) {
  for (Task *task = static_cast<Task *>(core->task_head); task != nullptr;
       task = task->next)
    if (reinterpret_cast<h2_pal_task_t *>(task) == opaque)
      return task;
  return nullptr;
}

int task_join(void *user, h2_pal_task_t *opaque) {
  auto *core = static_cast<h2_posix_thread_core_t *>(user);
  if (core == nullptr || opaque == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  Task *task;
  {
    Guard registry(&core->task_mutex);
    if (registry.rc != 0)
      return error(registry.rc);
    task = find_task(core, opaque);
    if (task == nullptr)
      return H2_PAL_ERR_INVALID_ARG;
    if (!task->native_joined.load() &&
        pthread_equal(task->thread, pthread_self()))
      return H2_PAL_ERR_INVALID_STATE;
    bool expected = false;
    if (!task->joining.compare_exchange_strong(expected, true))
      return H2_PAL_ERR_BUSY;
  }
  if (!task->native_joined.load()) {
    const int rc = native_join(task->thread);
    if (rc != 0) {
      task->joining.store(false);
      return error(rc);
    }
    task->native_joined.store(true);
  }
  {
    // Concurrent cancel is pinned by this registry lock. UI contention after a
    // successful native join is retryable; the native thread is not joined
    // twice.
    Guard registry(&core->task_mutex);
    if (registry.rc != 0) {
      task->joining.store(false);
      return error(registry.rc);
    }
    int rc = 0;
    if (task->sleep_cond_live) {
      rc = pthread_cond_destroy(&task->sleep_changed);
      if (rc == 0)
        task->sleep_cond_live = false;
    }
    if (rc == 0 && task->sleep_mutex_live) {
      rc = pthread_mutex_destroy(&task->sleep_mutex);
      if (rc == 0)
        task->sleep_mutex_live = false;
    }
    if (rc != 0) {
      task->joining.store(false);
      return error(rc);
    }
    Task *previous = nullptr;
    for (Task *cursor = static_cast<Task *>(core->task_head); cursor != task;
         cursor = cursor->next)
      previous = cursor;
    if (previous == nullptr)
      core->task_head = task->next;
    else
      previous->next = task->next;
  }
  const size_t bytes = task->stack_size;
  h2_pal_mem_free(&core->allocator, task->stack_allocation);
  delete task;
  core->stack_bytes -= bytes;
  --core->tasks;
  return H2_PAL_OK;
}

struct Queue {
  h2_posix_thread_core_t *core = nullptr;
  h2_pal_mem_api_t allocator{};
  pthread_mutex_t mutex{};
  pthread_cond_t not_empty{}, not_full{};
  unsigned char *items = nullptr;
  size_t item_size = 0u, capacity = 0u, head = 0u, count = 0u;
  size_t readers = 0u, writers = 0u;
  uint64_t close_generation = 0u, reset_generation = 0u;
  bool closed = false;
};

int queue_create(void *user, const h2_pal_queue_config_t *config,
                 h2_pal_queue_t **out) {
  auto *core = static_cast<h2_posix_thread_core_t *>(user);
  if (out != nullptr)
    *out = nullptr;
  if (core == nullptr || config == nullptr || out == nullptr ||
      !valid_allocator(config->allocator) || config->item_size == 0u ||
      config->item_count == 0u ||
      config->item_count > SIZE_MAX / config->item_size)
    return H2_PAL_ERR_INVALID_ARG;
  Pending pending(core);
  Queue *queue = make_object<Queue>(config->allocator);
  if (queue == nullptr)
    return H2_PAL_ERR_NO_MEMORY;
  queue->core = core;
  queue->allocator = *config->allocator;
  queue->capacity = config->item_count;
  queue->item_size = config->item_size;
  int rc = pthread_mutex_init(&queue->mutex, nullptr);
  if (rc != 0) {
    free_object(queue);
    return error(rc);
  }
  rc = init_cond(&queue->not_empty);
  if (rc != 0) {
    pthread_mutex_destroy(&queue->mutex);
    free_object(queue);
    return error(rc);
  }
  rc = init_cond(&queue->not_full);
  if (rc != 0) {
    pthread_cond_destroy(&queue->not_empty);
    pthread_mutex_destroy(&queue->mutex);
    free_object(queue);
    return error(rc);
  }
  queue->items = static_cast<unsigned char *>(
      h2_pal_mem_alloc(&queue->allocator, queue->capacity * queue->item_size));
  if (queue->items == nullptr) {
    pthread_cond_destroy(&queue->not_full);
    pthread_cond_destroy(&queue->not_empty);
    pthread_mutex_destroy(&queue->mutex);
    free_object(queue);
    return H2_PAL_ERR_NO_MEMORY;
  }
  ++core->queues;
  *out = reinterpret_cast<h2_pal_queue_t *>(queue);
  return H2_PAL_OK;
}

void queue_destroy(void *user, h2_pal_queue_t *opaque) {
  auto *core = static_cast<h2_posix_thread_core_t *>(user);
  auto *queue = reinterpret_cast<Queue *>(opaque);
  if (core == nullptr || queue == nullptr || queue->core != core)
    return;
  Guard guard(&queue->mutex);
  if (guard.rc != 0 || queue->readers != 0u || queue->writers != 0u)
    return;
  guard.unlock();
  pthread_cond_destroy(&queue->not_empty);
  pthread_cond_destroy(&queue->not_full);
  pthread_mutex_destroy(&queue->mutex);
  h2_pal_mem_free(&queue->allocator, queue->items);
  free_object(queue);
  --core->queues;
}

int queue_send(void *user, h2_pal_queue_t *opaque, const void *item,
               uint32_t timeout) {
  auto *core = static_cast<h2_posix_thread_core_t *>(user);
  auto *queue = reinterpret_cast<Queue *>(opaque);
  if (core == nullptr || queue == nullptr || queue->core != core ||
      item == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  if (ui_thread() && timeout != 0u)
    return H2_PAL_ERR_INVALID_STATE;
  Guard guard(&queue->mutex);
  if (guard.rc != 0)
    return guard.rc == EBUSY ? H2_PAL_ERR_TIMEOUT : error(guard.rc);
  if (queue->closed)
    return H2_PAL_ERR_CLOSED;
  const uint64_t until = deadline(timeout),
                 generation = queue->close_generation;
  while (queue->count == queue->capacity) {
    if (timeout == 0u)
      return H2_PAL_ERR_TIMEOUT;
    if (cancelled(core))
      return H2_PAL_EXIT;
    if (mono_ns() >= until)
      return H2_PAL_ERR_TIMEOUT;
    ++queue->writers;
    int rc = wait_slice(&queue->not_full, &queue->mutex, until);
    --queue->writers;
    if (queue->closed || generation != queue->close_generation)
      return H2_PAL_ERR_CLOSED;
    if (rc != 0 && rc != ETIMEDOUT && rc != EINTR)
      return error(rc);
  }
  std::memcpy(queue->items + ((queue->head + queue->count) % queue->capacity) *
                                 queue->item_size,
              item, queue->item_size);
  ++queue->count;
  pthread_cond_signal(&queue->not_empty);
  return H2_PAL_OK;
}

int queue_latest(void *user, h2_pal_queue_t *opaque, const void *item) {
  auto *core = static_cast<h2_posix_thread_core_t *>(user);
  auto *queue = reinterpret_cast<Queue *>(opaque);
  if (core == nullptr || queue == nullptr || queue->core != core ||
      item == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  Guard guard(&queue->mutex);
  if (guard.rc != 0)
    return guard.rc == EBUSY ? H2_PAL_ERR_TIMEOUT : error(guard.rc);
  if (queue->closed)
    return H2_PAL_ERR_CLOSED;
  if (queue->count == queue->capacity) {
    queue->head = (queue->head + 1u) % queue->capacity;
    --queue->count;
  }
  std::memcpy(queue->items + ((queue->head + queue->count) % queue->capacity) *
                                 queue->item_size,
              item, queue->item_size);
  ++queue->count;
  pthread_cond_signal(&queue->not_empty);
  return H2_PAL_OK;
}

int queue_recv(void *user, h2_pal_queue_t *opaque, void *item,
               uint32_t timeout) {
  auto *core = static_cast<h2_posix_thread_core_t *>(user);
  auto *queue = reinterpret_cast<Queue *>(opaque);
  if (core == nullptr || queue == nullptr || queue->core != core ||
      item == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  if (ui_thread() && timeout != 0u)
    return H2_PAL_ERR_INVALID_STATE;
  Guard guard(&queue->mutex);
  if (guard.rc != 0)
    return guard.rc == EBUSY ? H2_PAL_ERR_TIMEOUT : error(guard.rc);
  const uint64_t until = deadline(timeout),
                 generation = queue->reset_generation;
  while (queue->count == 0u) {
    if (queue->closed)
      return H2_PAL_ERR_CLOSED;
    if (timeout == 0u)
      return H2_PAL_ERR_TIMEOUT;
    if (cancelled(core))
      return H2_PAL_EXIT;
    if (mono_ns() >= until)
      return H2_PAL_ERR_TIMEOUT;
    ++queue->readers;
    int rc = wait_slice(&queue->not_empty, &queue->mutex, until);
    --queue->readers;
    if (generation != queue->reset_generation)
      return H2_PAL_ERR_CLOSED;
    if (rc != 0 && rc != ETIMEDOUT && rc != EINTR)
      return error(rc);
  }
  std::memcpy(item, queue->items + queue->head * queue->item_size,
              queue->item_size);
  queue->head = (queue->head + 1u) % queue->capacity;
  --queue->count;
  pthread_cond_signal(&queue->not_full);
  return H2_PAL_OK;
}

int queue_reset(void *user, h2_pal_queue_t *opaque) {
  auto *core = static_cast<h2_posix_thread_core_t *>(user);
  auto *queue = reinterpret_cast<Queue *>(opaque);
  if (core == nullptr || queue == nullptr || queue->core != core)
    return H2_PAL_ERR_INVALID_ARG;
  Guard guard(&queue->mutex);
  if (guard.rc != 0)
    return error(guard.rc);
  queue->head = queue->count = 0u;
  queue->closed = false;
  ++queue->reset_generation;
  pthread_cond_broadcast(&queue->not_empty);
  pthread_cond_broadcast(&queue->not_full);
  return H2_PAL_OK;
}

int queue_close(void *user, h2_pal_queue_t *opaque) {
  auto *core = static_cast<h2_posix_thread_core_t *>(user);
  auto *queue = reinterpret_cast<Queue *>(opaque);
  if (core == nullptr || queue == nullptr || queue->core != core)
    return H2_PAL_ERR_INVALID_ARG;
  Guard guard(&queue->mutex);
  if (guard.rc != 0)
    return error(guard.rc);
  if (!queue->closed) {
    queue->closed = true;
    ++queue->close_generation;
  }
  pthread_cond_broadcast(&queue->not_empty);
  pthread_cond_broadcast(&queue->not_full);
  return H2_PAL_OK;
}

struct Mutex {
  h2_posix_thread_core_t *core = nullptr;
  h2_pal_mem_api_t allocator{};
  pthread_mutex_t guard{};
  pthread_cond_t changed{};
  pthread_t owner{};
  size_t depth = 0u, waiters = 0u, condition_borrowers = 0u;
  bool recursive = false;
};

h2_pal_result_t mutex_create(void *user, const h2_pal_mutex_config_t *config,
                             h2_pal_mutex_t **out) {
  auto *core = static_cast<h2_posix_thread_core_t *>(user);
  if (out != nullptr)
    *out = nullptr;
  if (core == nullptr || config == nullptr || out == nullptr ||
      !valid_allocator(config->allocator) ||
      (config->flags & ~H2_PAL_MUTEX_FLAG_RECURSIVE) != 0u)
    return H2_PAL_ERR_INVALID_ARG;
  Pending pending(core);
  Mutex *mutex = make_object<Mutex>(config->allocator);
  if (mutex == nullptr)
    return H2_PAL_ERR_NO_MEMORY;
  mutex->core = core;
  mutex->allocator = *config->allocator;
  mutex->recursive = (config->flags & H2_PAL_MUTEX_FLAG_RECURSIVE) != 0u;
  int rc = pthread_mutex_init(&mutex->guard, nullptr);
  if (rc != 0) {
    free_object(mutex);
    return error(rc);
  }
  rc = init_cond(&mutex->changed);
  if (rc != 0) {
    pthread_mutex_destroy(&mutex->guard);
    free_object(mutex);
    return error(rc);
  }
  ++core->mutexes;
  *out = reinterpret_cast<h2_pal_mutex_t *>(mutex);
  return H2_PAL_OK;
}

h2_pal_result_t mutex_acquire(Mutex *mutex, bool try_only, bool cleanup) {
  if (!try_only && ui_thread())
    return H2_PAL_ERR_INVALID_STATE;
  Guard guard(&mutex->guard);
  if (guard.rc != 0)
    return guard.rc == EBUSY ? H2_PAL_ERR_WOULD_BLOCK : error(guard.rc);
  const pthread_t self = pthread_self();
  if (mutex->depth != 0u && pthread_equal(mutex->owner, self)) {
    if (!mutex->recursive)
      return H2_PAL_ERR_BUSY;
    if (mutex->depth == SIZE_MAX)
      return H2_PAL_ERR_FULL;
    ++mutex->depth;
    return H2_PAL_OK;
  }
  while (mutex->depth != 0u) {
    if (try_only)
      return H2_PAL_ERR_WOULD_BLOCK;
    if (!cleanup && cancelled(mutex->core))
      return H2_PAL_EXIT;
    ++mutex->waiters;
    int rc = wait_slice(&mutex->changed, &mutex->guard, UINT64_MAX);
    --mutex->waiters;
    if (rc != 0 && rc != ETIMEDOUT && rc != EINTR)
      return error(rc);
  }
  if (!cleanup && !try_only && cancelled(mutex->core))
    return H2_PAL_EXIT;
  mutex->owner = self;
  mutex->depth = 1u;
  return H2_PAL_OK;
}

h2_pal_result_t mutex_lock(void *user, h2_pal_mutex_t *opaque) {
  auto *mutex = reinterpret_cast<Mutex *>(opaque);
  if (mutex == nullptr || mutex->core != user)
    return H2_PAL_ERR_INVALID_ARG;
  return mutex_acquire(mutex, false, false);
}

h2_pal_result_t mutex_try(void *user, h2_pal_mutex_t *opaque) {
  auto *mutex = reinterpret_cast<Mutex *>(opaque);
  if (mutex == nullptr || mutex->core != user)
    return H2_PAL_ERR_INVALID_ARG;
  return mutex_acquire(mutex, true, false);
}

h2_pal_result_t mutex_unlock(void *user, h2_pal_mutex_t *opaque) {
  auto *mutex = reinterpret_cast<Mutex *>(opaque);
  if (mutex == nullptr || mutex->core != user)
    return H2_PAL_ERR_INVALID_ARG;
  Guard guard(&mutex->guard);
  if (guard.rc != 0)
    return error(guard.rc);
  if (mutex->depth == 0u || !pthread_equal(mutex->owner, pthread_self()))
    return H2_PAL_ERR_INVALID_STATE;
  if (--mutex->depth == 0u)
    pthread_cond_signal(&mutex->changed);
  return H2_PAL_OK;
}

h2_pal_result_t mutex_destroy(void *user, h2_pal_mutex_t *opaque) {
  auto *core = static_cast<h2_posix_thread_core_t *>(user);
  auto *mutex = reinterpret_cast<Mutex *>(opaque);
  if (core == nullptr || mutex == nullptr || mutex->core != core)
    return H2_PAL_ERR_INVALID_ARG;
  Guard guard(&mutex->guard);
  if (guard.rc != 0)
    return error(guard.rc);
  if (mutex->depth || mutex->waiters || mutex->condition_borrowers)
    return H2_PAL_ERR_BUSY;
  guard.unlock();
  int rc = pthread_cond_destroy(&mutex->changed);
  if (rc != 0)
    return error(rc);
  rc = pthread_mutex_destroy(&mutex->guard);
  if (rc != 0)
    return error(rc);
  free_object(mutex);
  --core->mutexes;
  return H2_PAL_OK;
}

struct Semaphore {
  h2_posix_thread_core_t *core = nullptr;
  h2_pal_mem_api_t allocator{};
  pthread_mutex_t mutex{};
  pthread_cond_t changed{};
  uint32_t count = 0u, maximum = 0u;
  size_t waiters = 0u;
};

h2_pal_result_t semaphore_create(void *user,
                                 const h2_pal_semaphore_config_t *config,
                                 h2_pal_semaphore_t **out) {
  auto *core = static_cast<h2_posix_thread_core_t *>(user);
  if (out != nullptr)
    *out = nullptr;
  if (core == nullptr || config == nullptr || out == nullptr ||
      !valid_allocator(config->allocator) || config->max_count == 0u ||
      config->initial_count > config->max_count)
    return H2_PAL_ERR_INVALID_ARG;
  Pending pending(core);
  Semaphore *sem = make_object<Semaphore>(config->allocator);
  if (sem == nullptr)
    return H2_PAL_ERR_NO_MEMORY;
  sem->core = core;
  sem->allocator = *config->allocator;
  sem->count = config->initial_count;
  sem->maximum = config->max_count;
  int rc = pthread_mutex_init(&sem->mutex, nullptr);
  if (rc != 0) {
    free_object(sem);
    return error(rc);
  }
  rc = init_cond(&sem->changed);
  if (rc != 0) {
    pthread_mutex_destroy(&sem->mutex);
    free_object(sem);
    return error(rc);
  }
  ++core->semaphores;
  *out = reinterpret_cast<h2_pal_semaphore_t *>(sem);
  return H2_PAL_OK;
}

h2_pal_result_t semaphore_take(void *user, h2_pal_semaphore_t *opaque,
                               uint32_t timeout) {
  auto *core = static_cast<h2_posix_thread_core_t *>(user);
  auto *sem = reinterpret_cast<Semaphore *>(opaque);
  if (core == nullptr || sem == nullptr || sem->core != core)
    return H2_PAL_ERR_INVALID_ARG;
  if (ui_thread() && timeout != 0u)
    return H2_PAL_ERR_INVALID_STATE;
  Guard guard(&sem->mutex);
  if (guard.rc != 0)
    return guard.rc == EBUSY ? H2_PAL_ERR_TIMEOUT : error(guard.rc);
  const uint64_t until = deadline(timeout);
  while (sem->count == 0u) {
    if (timeout == 0u)
      return H2_PAL_ERR_TIMEOUT;
    if (cancelled(core))
      return H2_PAL_EXIT;
    if (mono_ns() >= until)
      return H2_PAL_ERR_TIMEOUT;
    ++sem->waiters;
    int rc = wait_slice(&sem->changed, &sem->mutex, until);
    --sem->waiters;
    if (rc != 0 && rc != ETIMEDOUT && rc != EINTR)
      return error(rc);
  }
  --sem->count;
  return H2_PAL_OK;
}

h2_pal_result_t semaphore_give(void *user, h2_pal_semaphore_t *opaque) {
  auto *core = static_cast<h2_posix_thread_core_t *>(user);
  auto *sem = reinterpret_cast<Semaphore *>(opaque);
  if (core == nullptr || sem == nullptr || sem->core != core)
    return H2_PAL_ERR_INVALID_ARG;
  Guard guard(&sem->mutex);
  if (guard.rc != 0)
    return error(guard.rc);
  if (sem->count == sem->maximum)
    return H2_PAL_ERR_FULL;
  ++sem->count;
  pthread_cond_signal(&sem->changed);
  return H2_PAL_OK;
}

h2_pal_result_t semaphore_destroy(void *user, h2_pal_semaphore_t *opaque) {
  auto *core = static_cast<h2_posix_thread_core_t *>(user);
  auto *sem = reinterpret_cast<Semaphore *>(opaque);
  if (core == nullptr || sem == nullptr || sem->core != core)
    return H2_PAL_ERR_INVALID_ARG;
  Guard guard(&sem->mutex);
  if (guard.rc != 0)
    return error(guard.rc);
  if (sem->waiters != 0u)
    return H2_PAL_ERR_BUSY;
  guard.unlock();
  int rc = pthread_cond_destroy(&sem->changed);
  if (rc != 0)
    return error(rc);
  rc = pthread_mutex_destroy(&sem->mutex);
  if (rc != 0)
    return error(rc);
  free_object(sem);
  --core->semaphores;
  return H2_PAL_OK;
}

struct Waiter {
  Waiter *next = nullptr;
  pthread_cond_t changed{};
  bool signaled = false;
};
struct Condition {
  h2_posix_thread_core_t *core = nullptr;
  h2_pal_mem_api_t allocator{};
  pthread_mutex_t mutex{};
  Waiter *head = nullptr, *tail = nullptr;
};

h2_pal_result_t condition_create(void *user, const h2_pal_cond_config_t *config,
                                 h2_pal_cond_t **out) {
  auto *core = static_cast<h2_posix_thread_core_t *>(user);
  if (out != nullptr)
    *out = nullptr;
  if (core == nullptr || config == nullptr || out == nullptr ||
      !valid_allocator(config->allocator))
    return H2_PAL_ERR_INVALID_ARG;
  Pending pending(core);
  Condition *condition = make_object<Condition>(config->allocator);
  if (condition == nullptr)
    return H2_PAL_ERR_NO_MEMORY;
  condition->core = core;
  condition->allocator = *config->allocator;
  const int rc = pthread_mutex_init(&condition->mutex, nullptr);
  if (rc != 0) {
    free_object(condition);
    return error(rc);
  }
  ++core->conditions;
  *out = reinterpret_cast<h2_pal_cond_t *>(condition);
  return H2_PAL_OK;
}

h2_pal_result_t condition_wait(void *user, h2_pal_cond_t *opaque,
                               h2_pal_mutex_t *opaque_mutex, uint32_t timeout) {
  auto *core = static_cast<h2_posix_thread_core_t *>(user);
  auto *condition = reinterpret_cast<Condition *>(opaque);
  auto *mutex = reinterpret_cast<Mutex *>(opaque_mutex);
  if (core == nullptr || condition == nullptr || mutex == nullptr ||
      condition->core != core || mutex->core != core)
    return H2_PAL_ERR_INVALID_ARG;
  if (ui_thread())
    return H2_PAL_ERR_INVALID_STATE;
  Guard condition_guard(&condition->mutex);
  if (condition_guard.rc != 0)
    return error(condition_guard.rc);
  Guard mutex_guard(&mutex->guard);
  if (mutex_guard.rc != 0)
    return error(mutex_guard.rc);
  if (mutex->recursive || mutex->depth != 1u ||
      !pthread_equal(mutex->owner, pthread_self()))
    return H2_PAL_ERR_INVALID_STATE;
  if (cancelled(core))
    return H2_PAL_EXIT; // existing mutex ownership is preserved
  if (timeout == 0u)
    return H2_PAL_ERR_TIMEOUT;
  Waiter waiter;
  int rc = init_cond(&waiter.changed);
  if (rc != 0)
    return error(rc);
  if (condition->tail != nullptr)
    condition->tail->next = &waiter;
  else
    condition->head = &waiter;
  condition->tail = &waiter;
  ++mutex->condition_borrowers;
  mutex->depth = 0u;
  pthread_cond_signal(&mutex->changed);
  mutex_guard.unlock();
  const uint64_t until = deadline(timeout);
  h2_pal_result_t result = H2_PAL_OK;
  while (!waiter.signaled) {
    if (cancelled(core)) {
      result = H2_PAL_EXIT;
      break;
    }
    if (mono_ns() >= until) {
      result = H2_PAL_ERR_TIMEOUT;
      break;
    }
    rc = wait_slice(&waiter.changed, &condition->mutex, until);
    if (rc != 0 && rc != ETIMEDOUT && rc != EINTR) {
      result = error(rc);
      break;
    }
  }
  Waiter **cursor = &condition->head;
  Waiter *previous = nullptr;
  while (*cursor != &waiter) {
    previous = *cursor;
    cursor = &(*cursor)->next;
  }
  *cursor = waiter.next;
  if (condition->tail == &waiter)
    condition->tail = previous;
  condition_guard.unlock();
  pthread_cond_destroy(&waiter.changed);
  // Cancellation cannot skip reacquiring the caller's nonrecursive mutex.
  const h2_pal_result_t acquired = mutex_acquire(mutex, false, true);
  Guard final_guard(&mutex->guard);
  if (final_guard.rc != 0)
    return error(final_guard.rc);
  --mutex->condition_borrowers;
  return acquired == H2_PAL_OK ? result : acquired;
}

h2_pal_result_t condition_notify(void *user, h2_pal_cond_t *opaque, bool all) {
  auto *condition = reinterpret_cast<Condition *>(opaque);
  if (condition == nullptr || condition->core != user)
    return H2_PAL_ERR_INVALID_ARG;
  Guard guard(&condition->mutex);
  if (guard.rc != 0)
    return error(guard.rc);
  for (Waiter *waiter = condition->head; waiter != nullptr;
       waiter = waiter->next) {
    if (!waiter->signaled) {
      waiter->signaled = true;
      pthread_cond_signal(&waiter->changed);
      if (!all)
        break;
    }
  }
  return H2_PAL_OK;
}

h2_pal_result_t condition_signal(void *user, h2_pal_cond_t *condition) {
  return condition_notify(user, condition, false);
}
h2_pal_result_t condition_broadcast(void *user, h2_pal_cond_t *condition) {
  return condition_notify(user, condition, true);
}
h2_pal_result_t condition_destroy(void *user, h2_pal_cond_t *opaque) {
  auto *core = static_cast<h2_posix_thread_core_t *>(user);
  auto *condition = reinterpret_cast<Condition *>(opaque);
  if (core == nullptr || condition == nullptr || condition->core != core)
    return H2_PAL_ERR_INVALID_ARG;
  Guard guard(&condition->mutex);
  if (guard.rc != 0)
    return error(guard.rc);
  if (condition->head != nullptr)
    return H2_PAL_ERR_BUSY;
  guard.unlock();
  int rc = pthread_mutex_destroy(&condition->mutex);
  if (rc != 0)
    return error(rc);
  free_object(condition);
  --core->conditions;
  return H2_PAL_OK;
}

// Timer implementation follows below; it also uses real pthreads and native
// quiescence before releasing callback storage.
struct Timer {
  h2_posix_thread_core_t *core = nullptr;
  pthread_mutex_t mutex{};
  pthread_cond_t changed{};
  pthread_t thread{};
  h2_pal_timer_cb_t callback = nullptr;
  void *user = nullptr;
  uint32_t period_ms = 0u;
  uint64_t deadline_ns = 0u, generation = 0u;
  bool repeat = false, running = false, in_callback = false, ready = false;
  bool stopping = false, ui_stop = false, destroying = false,
       self_cleanup = false;
  std::atomic<bool> joining{false};
};

void dispose_timer(Timer *timer) {
  h2_posix_thread_core_t *core = timer->core;
  pthread_cond_destroy(&timer->changed);
  pthread_mutex_destroy(&timer->mutex);
  delete timer;
  --core->timers;
}

void arm_timer(Timer *timer) {
  timer->running = true;
  timer->deadline_ns =
      mono_ns() + static_cast<uint64_t>(timer->period_ms) * 1000000u;
  ++timer->generation;
  pthread_cond_broadcast(&timer->changed);
}

void *timer_worker(void *user) {
  Timer *timer = static_cast<Timer *>(user);
  pthread_mutex_lock(&timer->mutex);
  timer->ready = true;
  pthread_cond_broadcast(&timer->changed);
  while (!timer->destroying) {
    while (!timer->running && !timer->destroying)
      pthread_cond_wait(&timer->changed, &timer->mutex);
    if (timer->destroying)
      break;
    const uint64_t generation = timer->generation;
    const uint64_t due = timer->deadline_ns;
    while (timer->running && !timer->destroying &&
           timer->generation == generation && mono_ns() < due)
      (void)wait_slice(&timer->changed, &timer->mutex, due);
    if (timer->destroying || !timer->running || timer->generation != generation)
      continue;
    if (!timer->repeat)
      timer->running = false;
    timer->in_callback = true;
    pthread_mutex_unlock(&timer->mutex);
    timer->callback(timer->user, reinterpret_cast<h2_pal_timer_t *>(timer));
    pthread_mutex_lock(&timer->mutex);
    timer->in_callback = false;
    if (timer->ui_stop) {
      timer->ui_stop = false;
      timer->stopping = false;
    }
    pthread_cond_broadcast(&timer->changed);
    if (timer->repeat && timer->running && !timer->destroying &&
        timer->generation == generation) {
      const uint64_t period =
          static_cast<uint64_t>(timer->period_ms) * 1000000u;
      const uint64_t now = mono_ns();
      timer->deadline_ns = due + ((now - due) / period + 1u) * period;
    }
  }
  const bool self_cleanup = timer->self_cleanup;
  pthread_mutex_unlock(&timer->mutex);
  if (self_cleanup)
    dispose_timer(timer);
  return nullptr;
}

h2_pal_result_t timer_create(void *user, const h2_pal_timer_config_t *config,
                             h2_pal_timer_t **out) {
  auto *core = static_cast<h2_posix_thread_core_t *>(user);
  if (out != nullptr)
    *out = nullptr;
  if (core == nullptr || config == nullptr || out == nullptr ||
      config->cb == nullptr || config->period_ms == 0u ||
      (config->flags &
       ~(H2_PAL_TIMER_FLAG_REPEAT | H2_PAL_TIMER_FLAG_AUTO_START)) != 0u)
    return H2_PAL_ERR_INVALID_ARG;
  Pending pending(core);
  Timer *timer = new (std::nothrow) Timer();
  if (timer == nullptr)
    return H2_PAL_ERR_NO_MEMORY;
  timer->core = core;
  timer->callback = config->cb;
  timer->user = config->cb_user;
  timer->period_ms = config->period_ms;
  timer->repeat = (config->flags & H2_PAL_TIMER_FLAG_REPEAT) != 0u;
  int rc = pthread_mutex_init(&timer->mutex, nullptr);
  if (rc != 0) {
    delete timer;
    return error(rc);
  }
  rc = init_cond(&timer->changed);
  if (rc != 0) {
    pthread_mutex_destroy(&timer->mutex);
    delete timer;
    return error(rc);
  }
  Guard guard(&timer->mutex);
  if (guard.rc != 0) {
    pthread_cond_destroy(&timer->changed);
    pthread_mutex_destroy(&timer->mutex);
    delete timer;
    return error(guard.rc);
  }
  ++core->timers;
  rc = pthread_create(&timer->thread, nullptr, timer_worker, timer);
  if (rc != 0) {
    guard.unlock();
    dispose_timer(timer);
    return error(rc);
  }
  // Complete native Worker startup before arming on a Worker caller. Browser
  // thread creation otherwise only queues a message and consumes part of the
  // first period before the timer thread can run at all.
  if (!ui_thread())
    while (!timer->ready)
      pthread_cond_wait(&timer->changed, &timer->mutex);
  *out = reinterpret_cast<h2_pal_timer_t *>(timer);
  if ((config->flags & H2_PAL_TIMER_FLAG_AUTO_START) != 0u)
    arm_timer(timer);
  return H2_PAL_OK;
}

h2_pal_result_t timer_destroy(void *user, h2_pal_timer_t *opaque) {
  auto *core = static_cast<h2_posix_thread_core_t *>(user);
  auto *timer = reinterpret_cast<Timer *>(opaque);
  if (core == nullptr || timer == nullptr || timer->core != core)
    return H2_PAL_ERR_INVALID_ARG;
  Guard guard(&timer->mutex);
  if (guard.rc != 0)
    return error(guard.rc);
  if (timer->stopping || timer->joining.load() || timer->self_cleanup)
    return H2_PAL_ERR_BUSY;
  if (pthread_equal(timer->thread, pthread_self())) {
    const int rc = pthread_detach(timer->thread);
    if (rc != 0)
      return error(rc);
    timer->self_cleanup = true;
    timer->destroying = true;
    timer->running = false;
    ++timer->generation;
    return H2_PAL_OK;
  }
  timer->joining.store(true);
  timer->destroying = true;
  timer->running = false;
  ++timer->generation;
  pthread_cond_broadcast(&timer->changed);
  guard.unlock();
  const int rc = native_join(timer->thread);
  if (rc != 0) {
    timer->joining.store(false);
    return error(rc);
  }
  dispose_timer(timer);
  return H2_PAL_OK;
}

h2_pal_result_t timer_start(void *user, h2_pal_timer_t *opaque) {
  auto *timer = reinterpret_cast<Timer *>(opaque);
  if (timer == nullptr || timer->core != user)
    return H2_PAL_ERR_INVALID_ARG;
  Guard guard(&timer->mutex);
  if (guard.rc != 0)
    return error(guard.rc);
  if (timer->destroying || timer->running)
    return H2_PAL_ERR_INVALID_STATE;
  if (timer->stopping)
    return H2_PAL_ERR_BUSY;
  arm_timer(timer);
  return H2_PAL_OK;
}

h2_pal_result_t timer_stop(void *user, h2_pal_timer_t *opaque) {
  auto *timer = reinterpret_cast<Timer *>(opaque);
  if (timer == nullptr || timer->core != user)
    return H2_PAL_ERR_INVALID_ARG;
  Guard guard(&timer->mutex);
  if (guard.rc != 0)
    return error(guard.rc);
  if (timer->destroying)
    return H2_PAL_ERR_INVALID_STATE;
  const bool self = pthread_equal(timer->thread, pthread_self());
  if (timer->stopping && !self)
    return H2_PAL_ERR_BUSY;
  timer->running = false;
  ++timer->generation;
  pthread_cond_broadcast(&timer->changed);
  if (!self && timer->in_callback) {
    timer->stopping = true;
    if (ui_thread()) {
      timer->ui_stop = true;
      return H2_PAL_ERR_BUSY;
    }
    while (timer->in_callback)
      pthread_cond_wait(&timer->changed, &timer->mutex);
    timer->stopping = false;
  }
  return H2_PAL_OK;
}

h2_pal_result_t timer_reset(void *user, h2_pal_timer_t *opaque) {
  auto *timer = reinterpret_cast<Timer *>(opaque);
  if (timer == nullptr || timer->core != user)
    return H2_PAL_ERR_INVALID_ARG;
  Guard guard(&timer->mutex);
  if (guard.rc != 0)
    return error(guard.rc);
  if (timer->destroying)
    return H2_PAL_ERR_INVALID_STATE;
  if (timer->stopping)
    return H2_PAL_ERR_BUSY;
  arm_timer(timer);
  return H2_PAL_OK;
}

h2_pal_result_t timer_period(void *user, h2_pal_timer_t *opaque,
                             uint32_t period) {
  auto *timer = reinterpret_cast<Timer *>(opaque);
  if (timer == nullptr || timer->core != user || period == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  Guard guard(&timer->mutex);
  if (guard.rc != 0)
    return error(guard.rc);
  if (timer->destroying)
    return H2_PAL_ERR_INVALID_STATE;
  if (timer->stopping)
    return H2_PAL_ERR_BUSY;
  timer->period_ms = period;
  if (timer->running)
    arm_timer(timer);
  return H2_PAL_OK;
}

h2_pal_result_t timer_running(void *user, h2_pal_timer_t *opaque, int *out) {
  auto *timer = reinterpret_cast<Timer *>(opaque);
  if (out != nullptr)
    *out = 0;
  if (timer == nullptr || timer->core != user || out == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  Guard guard(&timer->mutex);
  if (guard.rc != 0)
    return error(guard.rc);
  if (timer->destroying)
    return H2_PAL_ERR_INVALID_STATE;
  *out = timer->running;
  return H2_PAL_OK;
}

const h2_pal_task_vtable_t task_vtable = {task_start, task_join};
const h2_pal_queue_vtable_t queue_vtable = {
    queue_create, queue_destroy, queue_send, queue_latest,
    queue_recv,   queue_reset,   queue_close};
const h2_pal_sync_vtable_t sync_vtable = {
    mutex_create,     mutex_destroy,      mutex_lock,        mutex_try,
    mutex_unlock,     semaphore_create,   semaphore_destroy, semaphore_take,
    semaphore_give,   condition_create,   condition_destroy, condition_wait,
    condition_signal, condition_broadcast};
const h2_pal_timer_vtable_t timer_vtable = {
    timer_create, timer_destroy, timer_start,  timer_stop,
    timer_reset,  timer_period,  timer_running};
} // namespace

extern "C" h2_pal_result_t
h2_posix_thread_core_create(const h2_pal_mem_api_t *allocator,
                            h2_posix_thread_core_t **out) {
  if (out != nullptr)
    *out = nullptr;
  if (out == nullptr || !valid_allocator(allocator))
    return H2_PAL_ERR_INVALID_ARG;
  auto *core = new (std::nothrow) h2_posix_thread_core_t();
  if (core == nullptr)
    return H2_PAL_ERR_NO_MEMORY;
  core->allocator = *allocator;
  int rc = pthread_key_create(&core->task_key, nullptr);
  if (rc != 0) {
    delete core;
    return error(rc);
  }
  rc = pthread_mutex_init(&core->task_mutex, nullptr);
  if (rc != 0) {
    pthread_key_delete(core->task_key);
    delete core;
    return error(rc);
  }
  core->task_api = {core, &task_vtable};
  core->queue_api = {core, &queue_vtable};
  core->sync_api = {core, &sync_vtable};
  core->timer_api = {core, &timer_vtable};
  *out = core;
  return H2_PAL_OK;
}

extern "C" h2_pal_result_t
h2_posix_thread_core_destroy(h2_posix_thread_core_t **out) {
  if (out == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  h2_posix_thread_core_t *core = *out;
  if (core == nullptr)
    return H2_PAL_OK;
  if (core->pending.load() || core->tasks.load() || core->queues.load() ||
      core->mutexes.load() || core->semaphores.load() ||
      core->conditions.load() || core->timers.load())
    return H2_PAL_ERR_BUSY;
  const int rc = pthread_mutex_destroy(&core->task_mutex);
  if (rc != 0)
    return error(rc);
  (void)pthread_key_delete(core->task_key);
  delete core;
  *out = nullptr;
  return H2_PAL_OK;
}

extern "C" const h2_pal_task_api_t *
h2_posix_thread_core_task_api(h2_posix_thread_core_t *core) {
  return core == nullptr ? nullptr : &core->task_api;
}
extern "C" const h2_pal_queue_api_t *
h2_posix_thread_core_queue_api(h2_posix_thread_core_t *core) {
  return core == nullptr ? nullptr : &core->queue_api;
}
extern "C" const h2_pal_sync_api_t *
h2_posix_thread_core_sync_api(h2_posix_thread_core_t *core) {
  return core == nullptr ? nullptr : &core->sync_api;
}
extern "C" const h2_pal_timer_api_t *
h2_posix_thread_core_timer_api(h2_posix_thread_core_t *core) {
  return core == nullptr ? nullptr : &core->timer_api;
}

extern "C" h2_pal_result_t
h2_posix_thread_core_task_cancel(h2_posix_thread_core_t *core,
                                 h2_pal_task_t *opaque) {
  if (core == nullptr || opaque == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  Guard registry(&core->task_mutex);
  if (registry.rc != 0)
    return error(registry.rc);
  Task *task = find_task(core, opaque);
  if (task == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  if (task->native_joined.load())
    return H2_PAL_ERR_INVALID_STATE;
  task->cancelled.store(true, std::memory_order_release);
  // The bounded predicate waits also poll this flag, so a busy UI try-lock
  // never requires spinning or proxying a wake back through a blocked Worker.
  if (pthread_mutex_trylock(&task->sleep_mutex) == 0) {
    pthread_cond_broadcast(&task->sleep_changed);
    pthread_mutex_unlock(&task->sleep_mutex);
  }
  return H2_PAL_OK;
}

extern "C" int
h2_posix_thread_core_is_current_task_cancelled(h2_posix_thread_core_t *core) {
  return cancelled(core);
}

extern "C" h2_pal_result_t
h2_posix_thread_core_sleep_ms(h2_posix_thread_core_t *core, uint32_t ms) {
  if (core == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  if (cancelled(core))
    return H2_PAL_EXIT;
  if (ms == 0u) {
    if (!ui_thread())
      sched_yield();
    return H2_PAL_OK;
  }
  if (ui_thread())
    return H2_PAL_ERR_INVALID_STATE;
  Task *task = current(core);
  const uint64_t until = mono_ns() + static_cast<uint64_t>(ms) * 1000000u;
  if (task == nullptr) {
    timespec requested{static_cast<time_t>(ms / 1000u),
                       static_cast<long>(ms % 1000u) * 1000000L};
    while (nanosleep(&requested, &requested) != 0)
      if (errno != EINTR)
        return H2_PAL_ERR_IO;
    return H2_PAL_OK;
  }
  Guard guard(&task->sleep_mutex);
  if (guard.rc != 0)
    return error(guard.rc);
  while (mono_ns() < until) {
    if (cancelled(core))
      return H2_PAL_EXIT;
    int rc = wait_slice(&task->sleep_changed, &task->sleep_mutex, until);
    if (rc != 0 && rc != ETIMEDOUT && rc != EINTR)
      return error(rc);
  }
  return cancelled(core) ? H2_PAL_EXIT : H2_PAL_OK;
}

extern "C" h2_pal_result_t h2_posix_thread_core_get_resource_stats(
    h2_posix_thread_core_t *core, h2_posix_thread_core_resource_stats_t *out) {
  if (out == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  std::memset(out, 0, sizeof(*out));
  if (core == nullptr)
    return H2_PAL_ERR_INVALID_ARG;
  out->live_tasks = core->tasks.load();
  out->task_stack_bytes = core->stack_bytes.load();
  out->live_queues = core->queues.load();
  out->live_mutexes = core->mutexes.load();
  out->live_semaphores = core->semaphores.load();
  out->live_conditions = core->conditions.load();
  out->live_timers = core->timers.load();
  return H2_PAL_OK;
}
