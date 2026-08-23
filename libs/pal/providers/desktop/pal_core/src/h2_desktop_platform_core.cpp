#include "h2_desktop_platform.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

namespace {

void *desktop_alloc(void *, size_t len) { return std::malloc(len); }

void *desktop_realloc(void *, void *ptr, size_t len) {
  return std::realloc(ptr, len);
}

void desktop_free(void *, void *ptr) { std::free(ptr); }

h2_pal_mem_vtable_t allocator_vtable = {};
h2_pal_mem_api_t allocators[4] = {};

int desktop_log_write(void *, h2_pal_log_level_t level, const char *scope,
                      const char *message) {
  if (message == nullptr) {
    return H2_PAL_LOG_ERR_INVALID_ARG;
  }
  const char *level_name = "unknown";
  switch (level) {
  case H2_PAL_LOG_DEBUG:
    level_name = "debug";
    break;
  case H2_PAL_LOG_INFO:
    level_name = "info";
    break;
  case H2_PAL_LOG_WARN:
    level_name = "warn";
    break;
  case H2_PAL_LOG_ERROR:
    level_name = "error";
    break;
  default:
    break;
  }
  std::fprintf(stderr, "H2_LOG level=%s scope=%s message=%s\n", level_name,
               scope != nullptr && scope[0] != '\0' ? scope : "h2", message);
  return H2_PAL_LOG_OK;
}

h2_pal_log_vtable_t log_vtable = {};
h2_pal_log_api_t log_api = {};

void *api_alloc(const h2_pal_mem_api_t *allocator, size_t len) {
  return allocator != nullptr ? h2_pal_mem_alloc(allocator, len)
                              : std::malloc(len);
}

void api_free(const h2_pal_mem_api_t *allocator, void *ptr) {
  if (ptr == nullptr) {
    return;
  }
  if (allocator != nullptr) {
    h2_pal_mem_free(allocator, ptr);
  } else {
    std::free(ptr);
  }
}

struct DesktopQueue {
  std::mutex mutex;
  std::condition_variable not_empty;
  std::condition_variable not_full;
  const h2_pal_mem_api_t *allocator = nullptr;
  size_t item_size = 0;
  size_t item_count = 0;
  size_t head = 0;
  size_t count = 0;
  bool closed = false;
  unsigned char *items = nullptr;
};

template <typename Predicate>
int wait_with_timeout(std::condition_variable &condition,
                      std::unique_lock<std::mutex> &lock, uint32_t timeout_ms,
                      Predicate predicate, int timeout_result) {
  if (predicate()) {
    return H2_PAL_OK;
  }
  if (timeout_ms == 0u) {
    return timeout_result;
  }
  if (timeout_ms == UINT32_MAX) {
    condition.wait(lock, predicate);
    return H2_PAL_OK;
  }
  const bool ready = condition.wait_for(
      lock, std::chrono::milliseconds(timeout_ms), predicate);
  return ready ? H2_PAL_OK : timeout_result;
}

int queue_create(void *, const h2_pal_queue_config_t *config,
                 h2_pal_queue_t **out_queue) {
  if (config == nullptr || out_queue == nullptr || config->item_size == 0u ||
      config->item_count == 0u ||
      config->item_size > SIZE_MAX / config->item_count) {
    return H2_PAL_QUEUE_ERR_INVALID_ARG;
  }
  *out_queue = nullptr;
  void *raw_queue = api_alloc(config->allocator, sizeof(DesktopQueue));
  if (raw_queue == nullptr) {
    return H2_PAL_QUEUE_ERR_NO_MEMORY;
  }
  DesktopQueue *queue = new (raw_queue) DesktopQueue();
  queue->allocator = config->allocator;
  queue->item_size = config->item_size;
  queue->item_count = config->item_count;
  queue->items = static_cast<unsigned char *>(
      api_alloc(config->allocator, config->item_size * config->item_count));
  if (queue->items == nullptr) {
    queue->~DesktopQueue();
    api_free(config->allocator, queue);
    return H2_PAL_QUEUE_ERR_NO_MEMORY;
  }
  *out_queue = reinterpret_cast<h2_pal_queue_t *>(queue);
  return H2_PAL_QUEUE_OK;
}

void queue_destroy(void *, h2_pal_queue_t *raw_queue) {
  if (raw_queue == nullptr) {
    return;
  }
  DesktopQueue *queue = reinterpret_cast<DesktopQueue *>(raw_queue);
  {
    std::lock_guard<std::mutex> lock(queue->mutex);
    queue->closed = true;
  }
  queue->not_empty.notify_all();
  queue->not_full.notify_all();
  const h2_pal_mem_api_t *allocator = queue->allocator;
  api_free(allocator, queue->items);
  queue->~DesktopQueue();
  api_free(allocator, queue);
}

int queue_send(void *, h2_pal_queue_t *raw_queue, const void *item,
               uint32_t timeout_ms) {
  if (raw_queue == nullptr || item == nullptr) {
    return H2_PAL_QUEUE_ERR_INVALID_ARG;
  }
  DesktopQueue *queue = reinterpret_cast<DesktopQueue *>(raw_queue);
  std::unique_lock<std::mutex> lock(queue->mutex);
  const int wait_result = wait_with_timeout(
      queue->not_full, lock, timeout_ms,
      [queue] { return queue->closed || queue->count < queue->item_count; },
      H2_PAL_QUEUE_ERR_TIMEOUT);
  if (wait_result != H2_PAL_OK) {
    return wait_result;
  }
  if (queue->closed) {
    return H2_PAL_QUEUE_ERR_CLOSED;
  }
  const size_t tail = (queue->head + queue->count) % queue->item_count;
  std::memcpy(queue->items + tail * queue->item_size, item, queue->item_size);
  ++queue->count;
  lock.unlock();
  queue->not_empty.notify_one();
  return H2_PAL_QUEUE_OK;
}

int queue_send_latest(void *, h2_pal_queue_t *raw_queue, const void *item) {
  if (raw_queue == nullptr || item == nullptr) {
    return H2_PAL_QUEUE_ERR_INVALID_ARG;
  }
  DesktopQueue *queue = reinterpret_cast<DesktopQueue *>(raw_queue);
  std::lock_guard<std::mutex> lock(queue->mutex);
  if (queue->closed) {
    return H2_PAL_QUEUE_ERR_CLOSED;
  }
  if (queue->count == queue->item_count) {
    queue->head = (queue->head + 1u) % queue->item_count;
    --queue->count;
  }
  const size_t tail = (queue->head + queue->count) % queue->item_count;
  std::memcpy(queue->items + tail * queue->item_size, item, queue->item_size);
  ++queue->count;
  queue->not_empty.notify_one();
  queue->not_full.notify_one();
  return H2_PAL_QUEUE_OK;
}

int queue_recv(void *, h2_pal_queue_t *raw_queue, void *out_item,
               uint32_t timeout_ms) {
  if (raw_queue == nullptr || out_item == nullptr) {
    return H2_PAL_QUEUE_ERR_INVALID_ARG;
  }
  DesktopQueue *queue = reinterpret_cast<DesktopQueue *>(raw_queue);
  std::unique_lock<std::mutex> lock(queue->mutex);
  const int wait_result = wait_with_timeout(
      queue->not_empty, lock, timeout_ms,
      [queue] { return queue->closed || queue->count != 0u; },
      H2_PAL_QUEUE_ERR_TIMEOUT);
  if (wait_result != H2_PAL_OK) {
    return wait_result;
  }
  if (queue->count == 0u) {
    return H2_PAL_QUEUE_ERR_CLOSED;
  }
  std::memcpy(out_item, queue->items + queue->head * queue->item_size,
              queue->item_size);
  queue->head = (queue->head + 1u) % queue->item_count;
  --queue->count;
  lock.unlock();
  queue->not_full.notify_one();
  return H2_PAL_QUEUE_OK;
}

int queue_reset(void *, h2_pal_queue_t *raw_queue) {
  if (raw_queue == nullptr) {
    return H2_PAL_QUEUE_ERR_INVALID_ARG;
  }
  DesktopQueue *queue = reinterpret_cast<DesktopQueue *>(raw_queue);
  std::lock_guard<std::mutex> lock(queue->mutex);
  queue->head = 0u;
  queue->count = 0u;
  queue->not_full.notify_all();
  return H2_PAL_QUEUE_OK;
}

int queue_close(void *, h2_pal_queue_t *raw_queue) {
  if (raw_queue == nullptr) {
    return H2_PAL_QUEUE_ERR_INVALID_ARG;
  }
  DesktopQueue *queue = reinterpret_cast<DesktopQueue *>(raw_queue);
  {
    std::lock_guard<std::mutex> lock(queue->mutex);
    queue->closed = true;
  }
  queue->not_empty.notify_all();
  queue->not_full.notify_all();
  return H2_PAL_QUEUE_OK;
}

h2_pal_queue_vtable_t queue_vtable = {};
h2_pal_queue_api_t queue_api = {};

struct DesktopMutex {
  std::mutex normal;
  std::recursive_mutex recursive;
  const h2_pal_mem_api_t *allocator = nullptr;
  bool is_recursive = false;
};

struct DesktopSemaphore {
  std::mutex mutex;
  std::condition_variable condition;
  const h2_pal_mem_api_t *allocator = nullptr;
  uint32_t count = 0u;
  uint32_t max_count = 0u;
};

struct DesktopCondition {
  std::condition_variable condition;
  const h2_pal_mem_api_t *allocator = nullptr;
};

h2_pal_result_t mutex_create(void *, const h2_pal_mutex_config_t *config,
                             h2_pal_mutex_t **out_mutex) {
  if (config == nullptr || out_mutex == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_mutex = nullptr;
  void *raw = api_alloc(config->allocator, sizeof(DesktopMutex));
  if (raw == nullptr) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  DesktopMutex *mutex = new (raw) DesktopMutex();
  mutex->allocator = config->allocator;
  mutex->is_recursive =
      (config->flags & H2_PAL_MUTEX_FLAG_RECURSIVE) != 0u;
  *out_mutex = reinterpret_cast<h2_pal_mutex_t *>(mutex);
  return H2_PAL_OK;
}

h2_pal_result_t mutex_destroy(void *, h2_pal_mutex_t *raw_mutex) {
  if (raw_mutex == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  DesktopMutex *mutex = reinterpret_cast<DesktopMutex *>(raw_mutex);
  const h2_pal_mem_api_t *allocator = mutex->allocator;
  mutex->~DesktopMutex();
  api_free(allocator, mutex);
  return H2_PAL_OK;
}

h2_pal_result_t mutex_lock(void *, h2_pal_mutex_t *raw_mutex) {
  if (raw_mutex == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  DesktopMutex *mutex = reinterpret_cast<DesktopMutex *>(raw_mutex);
  if (mutex->is_recursive) {
    mutex->recursive.lock();
  } else {
    mutex->normal.lock();
  }
  return H2_PAL_OK;
}

h2_pal_result_t mutex_try_lock(void *, h2_pal_mutex_t *raw_mutex) {
  if (raw_mutex == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  DesktopMutex *mutex = reinterpret_cast<DesktopMutex *>(raw_mutex);
  const bool locked = mutex->is_recursive ? mutex->recursive.try_lock()
                                          : mutex->normal.try_lock();
  return locked ? H2_PAL_OK : H2_PAL_ERR_WOULD_BLOCK;
}

h2_pal_result_t mutex_unlock(void *, h2_pal_mutex_t *raw_mutex) {
  if (raw_mutex == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  DesktopMutex *mutex = reinterpret_cast<DesktopMutex *>(raw_mutex);
  if (mutex->is_recursive) {
    mutex->recursive.unlock();
  } else {
    mutex->normal.unlock();
  }
  return H2_PAL_OK;
}

h2_pal_result_t semaphore_create(void *,
                                 const h2_pal_semaphore_config_t *config,
                                 h2_pal_semaphore_t **out_semaphore) {
  if (config == nullptr || out_semaphore == nullptr ||
      config->max_count == 0u || config->initial_count > config->max_count) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_semaphore = nullptr;
  void *raw = api_alloc(config->allocator, sizeof(DesktopSemaphore));
  if (raw == nullptr) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  DesktopSemaphore *semaphore = new (raw) DesktopSemaphore();
  semaphore->allocator = config->allocator;
  semaphore->count = config->initial_count;
  semaphore->max_count = config->max_count;
  *out_semaphore = reinterpret_cast<h2_pal_semaphore_t *>(semaphore);
  return H2_PAL_OK;
}

h2_pal_result_t semaphore_destroy(void *, h2_pal_semaphore_t *raw_semaphore) {
  if (raw_semaphore == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  DesktopSemaphore *semaphore =
      reinterpret_cast<DesktopSemaphore *>(raw_semaphore);
  const h2_pal_mem_api_t *allocator = semaphore->allocator;
  semaphore->~DesktopSemaphore();
  api_free(allocator, semaphore);
  return H2_PAL_OK;
}

h2_pal_result_t semaphore_take(void *, h2_pal_semaphore_t *raw_semaphore,
                               uint32_t timeout_ms) {
  if (raw_semaphore == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  DesktopSemaphore *semaphore =
      reinterpret_cast<DesktopSemaphore *>(raw_semaphore);
  std::unique_lock<std::mutex> lock(semaphore->mutex);
  const int wait_result = wait_with_timeout(
      semaphore->condition, lock, timeout_ms,
      [semaphore] { return semaphore->count != 0u; }, H2_PAL_ERR_TIMEOUT);
  if (wait_result != H2_PAL_OK) {
    return static_cast<h2_pal_result_t>(wait_result);
  }
  --semaphore->count;
  return H2_PAL_OK;
}

h2_pal_result_t semaphore_give(void *, h2_pal_semaphore_t *raw_semaphore) {
  if (raw_semaphore == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  DesktopSemaphore *semaphore =
      reinterpret_cast<DesktopSemaphore *>(raw_semaphore);
  std::lock_guard<std::mutex> lock(semaphore->mutex);
  if (semaphore->count == semaphore->max_count) {
    return H2_PAL_ERR_FULL;
  }
  ++semaphore->count;
  semaphore->condition.notify_one();
  return H2_PAL_OK;
}

h2_pal_result_t condition_create(void *, const h2_pal_cond_config_t *config,
                                 h2_pal_cond_t **out_condition) {
  if (config == nullptr || out_condition == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_condition = nullptr;
  void *raw = api_alloc(config->allocator, sizeof(DesktopCondition));
  if (raw == nullptr) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  DesktopCondition *condition = new (raw) DesktopCondition();
  condition->allocator = config->allocator;
  *out_condition = reinterpret_cast<h2_pal_cond_t *>(condition);
  return H2_PAL_OK;
}

h2_pal_result_t condition_destroy(void *, h2_pal_cond_t *raw_condition) {
  if (raw_condition == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  DesktopCondition *condition =
      reinterpret_cast<DesktopCondition *>(raw_condition);
  const h2_pal_mem_api_t *allocator = condition->allocator;
  condition->~DesktopCondition();
  api_free(allocator, condition);
  return H2_PAL_OK;
}

h2_pal_result_t condition_wait(void *, h2_pal_cond_t *raw_condition,
                               h2_pal_mutex_t *raw_mutex,
                               uint32_t timeout_ms) {
  if (raw_condition == nullptr || raw_mutex == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  DesktopCondition *condition =
      reinterpret_cast<DesktopCondition *>(raw_condition);
  DesktopMutex *mutex = reinterpret_cast<DesktopMutex *>(raw_mutex);
  if (mutex->is_recursive) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  std::unique_lock<std::mutex> lock(mutex->normal, std::adopt_lock);
  h2_pal_result_t result = H2_PAL_OK;
  if (timeout_ms == H2_PAL_SYNC_NO_WAIT) {
    result = H2_PAL_ERR_TIMEOUT;
  } else if (timeout_ms == H2_PAL_SYNC_WAIT_FOREVER) {
    condition->condition.wait(lock);
  } else if (condition->condition.wait_for(
                 lock, std::chrono::milliseconds(timeout_ms)) ==
             std::cv_status::timeout) {
    result = H2_PAL_ERR_TIMEOUT;
  }
  lock.release();
  return result;
}

h2_pal_result_t condition_signal(void *, h2_pal_cond_t *raw_condition) {
  if (raw_condition == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  reinterpret_cast<DesktopCondition *>(raw_condition)->condition.notify_one();
  return H2_PAL_OK;
}

h2_pal_result_t condition_broadcast(void *, h2_pal_cond_t *raw_condition) {
  if (raw_condition == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  reinterpret_cast<DesktopCondition *>(raw_condition)->condition.notify_all();
  return H2_PAL_OK;
}

h2_pal_sync_vtable_t sync_vtable = {};
h2_pal_sync_api_t sync_api = {};

struct DesktopTask {
  std::thread thread;
};

int task_start(void *, const h2_pal_task_options_t *,
               h2_pal_task_entry_t entry, void *context,
               h2_pal_task_t **out_task) {
  if (entry == nullptr || out_task == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_task = nullptr;
  DesktopTask *task = new (std::nothrow) DesktopTask();
  if (task == nullptr) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  try {
    task->thread = std::thread([entry, context] { entry(context); });
  } catch (...) {
    delete task;
    return H2_PAL_ERR_UNAVAILABLE;
  }
  *out_task = reinterpret_cast<h2_pal_task_t *>(task);
  return H2_PAL_OK;
}

int task_join(void *, h2_pal_task_t *raw_task) {
  if (raw_task == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  DesktopTask *task = reinterpret_cast<DesktopTask *>(raw_task);
  if (task->thread.joinable()) {
    task->thread.join();
  }
  delete task;
  return H2_PAL_OK;
}

h2_pal_task_vtable_t task_vtable = {};
h2_pal_task_api_t task_api = {};

std::mutex time_mutex;
const auto monotonic_start = std::chrono::steady_clock::now();
int64_t wall_offset_ms = 0;
bool wall_valid = true;
h2_pal_time_wall_source_t wall_source = H2_PAL_TIME_WALL_SOURCE_UNKNOWN;

h2_pal_result_t time_get_monotonic(void *, uint64_t *out_ms) {
  if (out_ms == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_ms = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - monotonic_start)
          .count());
  return H2_PAL_OK;
}

h2_pal_result_t time_get_monotonic_us(void *, uint64_t *out_us) {
  if (out_us == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - monotonic_start)
          .count());
  return H2_PAL_OK;
}

h2_pal_result_t time_get_wall(void *, uint64_t *out_ms) {
  if (out_ms == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  std::lock_guard<std::mutex> lock(time_mutex);
  const int64_t adjusted = now + wall_offset_ms;
  if (adjusted <= 0) {
    return H2_PAL_ERR_UNAVAILABLE;
  }
  *out_ms = static_cast<uint64_t>(adjusted);
  wall_valid = true;
  return H2_PAL_OK;
}

h2_pal_result_t time_set_wall(void *, uint64_t wall_ms) {
  if (wall_ms == 0u || wall_ms > static_cast<uint64_t>(INT64_MAX)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  std::lock_guard<std::mutex> lock(time_mutex);
  wall_offset_ms = static_cast<int64_t>(wall_ms) - now;
  wall_valid = true;
  wall_source = H2_PAL_TIME_WALL_SOURCE_NTP;
  return H2_PAL_OK;
}

h2_pal_result_t time_get_status(void *,
                                h2_pal_time_wall_status_t *out_status) {
  if (out_status == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(time_mutex);
  out_status->valid = wall_valid ? 1u : 0u;
  out_status->source = wall_source;
  return H2_PAL_OK;
}

h2_pal_result_t time_sleep(void *, uint32_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  return H2_PAL_OK;
}

h2_pal_time_vtable_t time_vtable = {};
h2_pal_time_api_t time_api = {};

void initialize_core_apis() {
  allocator_vtable.alloc = desktop_alloc;
  allocator_vtable.realloc = desktop_realloc;
  allocator_vtable.free = desktop_free;
  for (h2_pal_mem_api_t &allocator : allocators) {
    allocator.user = nullptr;
    allocator.vtable = &allocator_vtable;
  }

  log_vtable.write = desktop_log_write;
  log_api.user = nullptr;
  log_api.vtable = &log_vtable;

  queue_vtable.create = queue_create;
  queue_vtable.destroy = queue_destroy;
  queue_vtable.send = queue_send;
  queue_vtable.send_latest = queue_send_latest;
  queue_vtable.recv = queue_recv;
  queue_vtable.reset = queue_reset;
  queue_vtable.close = queue_close;
  queue_api.user = nullptr;
  queue_api.vtable = &queue_vtable;

  sync_vtable.create_mutex = mutex_create;
  sync_vtable.destroy_mutex = mutex_destroy;
  sync_vtable.lock_mutex = mutex_lock;
  sync_vtable.try_lock_mutex = mutex_try_lock;
  sync_vtable.unlock_mutex = mutex_unlock;
  sync_vtable.create_semaphore = semaphore_create;
  sync_vtable.destroy_semaphore = semaphore_destroy;
  sync_vtable.take_semaphore = semaphore_take;
  sync_vtable.give_semaphore = semaphore_give;
  sync_vtable.create_cond = condition_create;
  sync_vtable.destroy_cond = condition_destroy;
  sync_vtable.wait_cond = condition_wait;
  sync_vtable.signal_cond = condition_signal;
  sync_vtable.broadcast_cond = condition_broadcast;
  sync_api.user = nullptr;
  sync_api.vtable = &sync_vtable;

  task_vtable.start = task_start;
  task_vtable.join = task_join;
  task_api.user = nullptr;
  task_api.vtable = &task_vtable;

  time_vtable.get_monotonic_ms = time_get_monotonic;
  time_vtable.get_monotonic_us = time_get_monotonic_us;
  time_vtable.get_wall_ms = time_get_wall;
  time_vtable.set_wall_ms = time_set_wall;
  time_vtable.get_wall_status = time_get_status;
  time_vtable.sleep_ms = time_sleep;
  time_api.user = nullptr;
  time_api.vtable = &time_vtable;
}

std::once_flag core_once;

void ensure_core_apis() {
  std::call_once(core_once, initialize_core_apis);
}

} // namespace

extern "C" {

h2_pal_mem_api_t *h2_desktop_platform_default_allocator(void) {
  ensure_core_apis();
  return &allocators[0];
}

h2_pal_mem_api_t *h2_desktop_platform_psram_allocator(void) {
  ensure_core_apis();
  return &allocators[1];
}

h2_pal_mem_api_t *h2_desktop_platform_internal_allocator(void) {
  ensure_core_apis();
  return &allocators[2];
}

h2_pal_mem_api_t *h2_desktop_platform_dma_allocator(void) {
  ensure_core_apis();
  return &allocators[3];
}

const h2_pal_log_api_t *h2_desktop_platform_log_api(void) {
  ensure_core_apis();
  return &log_api;
}

const h2_pal_queue_api_t *h2_desktop_platform_queue_api(void) {
  ensure_core_apis();
  return &queue_api;
}

const h2_pal_sync_api_t *h2_desktop_platform_sync_api(void) {
  ensure_core_apis();
  return &sync_api;
}

const h2_pal_task_api_t *h2_desktop_platform_task_api(void) {
  ensure_core_apis();
  return &task_api;
}

const h2_pal_time_api_t *h2_desktop_platform_time_api(void) {
  ensure_core_apis();
  return &time_api;
}

} // extern "C"
