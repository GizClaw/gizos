#include "h2_g99_storage_worker.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <pthread.h>
#include <thread>

struct h2_pal_mutex {
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
};

struct h2_pal_semaphore {
  std::mutex mutex;
  std::condition_variable ready;
  uint32_t count;
};

struct h2_pal_task {
  std::thread thread;
};

struct Environment {
  std::atomic<unsigned> allocations{0};
  std::atomic<unsigned> handles{0};
  bool fail_start = false;
  bool fail_join = false;
  bool fail_destroy = false;
};

static void* allocate(void* user, size_t bytes) {
  auto* environment = static_cast<Environment*>(user);
  void* pointer = std::malloc(bytes);
  if (pointer != nullptr) ++environment->allocations;
  return pointer;
}

static void release(void* user, void* pointer) {
  --static_cast<Environment*>(user)->allocations;
  std::free(pointer);
}

static h2_pal_result_t mutex_create(void* user, const h2_pal_mutex_config_t*,
                                    h2_pal_mutex_t** out) {
  *out = new h2_pal_mutex;
  ++static_cast<Environment*>(user)->handles;
  return H2_PAL_OK;
}

static h2_pal_result_t mutex_destroy(void* user, h2_pal_mutex_t* mutex) {
  auto* environment = static_cast<Environment*>(user);
  if (environment->fail_destroy) return H2_PAL_ERR_IO;
  assert(pthread_mutex_destroy(&mutex->mutex) == 0);
  delete mutex;
  --environment->handles;
  return H2_PAL_OK;
}

static h2_pal_result_t mutex_lock(void*, h2_pal_mutex_t* mutex) {
  assert(pthread_mutex_lock(&mutex->mutex) == 0);
  return H2_PAL_OK;
}

static h2_pal_result_t mutex_unlock(void*, h2_pal_mutex_t* mutex) {
  assert(pthread_mutex_unlock(&mutex->mutex) == 0);
  return H2_PAL_OK;
}

static h2_pal_result_t semaphore_create(void* user,
                                        const h2_pal_semaphore_config_t* config,
                                        h2_pal_semaphore_t** out) {
  *out = new h2_pal_semaphore;
  (*out)->count = config->initial_count;
  ++static_cast<Environment*>(user)->handles;
  return H2_PAL_OK;
}

static h2_pal_result_t semaphore_destroy(void* user,
                                         h2_pal_semaphore_t* semaphore) {
  delete semaphore;
  --static_cast<Environment*>(user)->handles;
  return H2_PAL_OK;
}

static h2_pal_result_t semaphore_take(void*, h2_pal_semaphore_t* semaphore,
                                      uint32_t timeout_ms) {
  assert(timeout_ms == UINT32_MAX);
  std::unique_lock<std::mutex> lock(semaphore->mutex);
  semaphore->ready.wait(lock, [semaphore] { return semaphore->count > 0u; });
  --semaphore->count;
  return H2_PAL_OK;
}

static h2_pal_result_t semaphore_give(void*, h2_pal_semaphore_t* semaphore) {
  std::lock_guard<std::mutex> lock(semaphore->mutex);
  if (semaphore->count != 0u) return H2_PAL_ERR_FULL;
  ++semaphore->count;
  semaphore->ready.notify_one();
  return H2_PAL_OK;
}

static int task_start(void* user, const h2_pal_task_options_t*,
                      h2_pal_task_entry_t entry, void* context,
                      h2_pal_task_t** out) {
  auto* environment = static_cast<Environment*>(user);
  if (environment->fail_start) return H2_PAL_ERR_NO_MEMORY;
  *out = new h2_pal_task{std::thread(entry, context)};
  ++environment->handles;
  return H2_PAL_OK;
}

static int task_join(void* user, h2_pal_task_t* task) {
  auto* environment = static_cast<Environment*>(user);
  if (environment->fail_join) return H2_PAL_ERR_BUSY;
  task->thread.join();
  delete task;
  --environment->handles;
  return H2_PAL_OK;
}

struct Query {
  std::mutex mutex;
  std::condition_variable ready;
  bool entered = false;
  bool resume = false;
  bool invalid = false;
  h2_pal_result_t result = H2_PAL_OK;

  static h2_pal_result_t run(void* user, h2_g99_storage_volume_t* internal,
                             h2_g99_storage_volume_t* external) {
    auto* query = static_cast<Query*>(user);
    std::unique_lock<std::mutex> lock(query->mutex);
    query->entered = true;
    query->ready.notify_all();
    query->ready.wait(lock, [query] { return query->resume; });
    internal->result = H2_PAL_OK;
    internal->used_bytes = query->invalid ? 200u : 25u;
    internal->total_bytes = 100u;
    external->result = H2_PAL_ERR_UNSUPPORTED;
    external->used_bytes = 1000u;
    external->total_bytes = 2000u;
    return query->result;
  }

  void wait() {
    std::unique_lock<std::mutex> lock(mutex);
    assert(ready.wait_for(lock, std::chrono::seconds(3),
                          [this] { return entered; }));
  }

  void finish() {
    std::lock_guard<std::mutex> lock(mutex);
    resume = true;
    ready.notify_all();
  }
};

static h2_g99_storage_snapshot_t completed(h2_g99_storage_t* storage) {
  h2_g99_storage_snapshot_t snapshot{};
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  do {
    assert(h2_g99_storage_get(storage, &snapshot) == H2_PAL_OK);
    if (!snapshot.pending) return snapshot;
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  assert(false);
  return snapshot;
}

int main() {
  Environment environment;
  const h2_pal_mem_vtable_t memory_vtable = {allocate, nullptr, release,
                                             nullptr};
  const h2_pal_mem_api_t memory = {&environment, &memory_vtable};
  h2_pal_sync_vtable_t sync_vtable{};
  sync_vtable.create_mutex = mutex_create;
  sync_vtable.destroy_mutex = mutex_destroy;
  sync_vtable.lock_mutex = mutex_lock;
  sync_vtable.unlock_mutex = mutex_unlock;
  sync_vtable.create_semaphore = semaphore_create;
  sync_vtable.destroy_semaphore = semaphore_destroy;
  sync_vtable.take_semaphore = semaphore_take;
  sync_vtable.give_semaphore = semaphore_give;
  const h2_pal_sync_api_t sync = {&environment, &sync_vtable};
  const h2_pal_task_vtable_t task_vtable = {task_start, task_join};
  const h2_pal_task_api_t task = {&environment, &task_vtable};
  h2_runtime_t runtime{};
  runtime.mem = &memory;
  runtime.sync = &sync;
  runtime.task = &task;
  h2_g99_storage_t* storage = nullptr;
  assert(h2_g99_storage_create(&runtime, nullptr, &storage) == H2_PAL_OK);
  assert(h2_g99_storage_request(storage) == H2_PAL_ERR_UNSUPPORTED);
  assert(completed(storage).internal.result == H2_PAL_ERR_UNSUPPORTED);
  assert(h2_g99_storage_destroy(&storage) == H2_PAL_OK);
  assert(storage == nullptr && environment.allocations == 0u);

  Query query;
  const h2_g99_storage_api_t api = {&query, H2_G99_STORAGE_SPI_FLASH, false,
                                    Query::run};
  assert(h2_g99_storage_create(&runtime, &api, &storage) == H2_PAL_OK);
  assert(h2_g99_storage_request(storage) == H2_PAL_OK);
  query.wait();
  assert(h2_g99_storage_request(storage) == H2_PAL_ERR_BUSY);
  query.finish();
  auto snapshot = completed(storage);
  assert(snapshot.internal.used_bytes == 25u);
  assert(snapshot.external.kind == H2_G99_STORAGE_SPI_FLASH);
  assert(snapshot.external.result == H2_PAL_ERR_UNSUPPORTED);
  assert(snapshot.external.used_bytes == 0u);

  query.invalid = true;
  assert(h2_g99_storage_request(storage) == H2_PAL_OK);
  snapshot = completed(storage);
  assert(snapshot.internal.result == H2_PAL_ERR_INVALID_STATE);
  assert(snapshot.internal.total_bytes == 0u);
  query.result = H2_PAL_ERR_IO;
  assert(h2_g99_storage_request(storage) == H2_PAL_OK);
  snapshot = completed(storage);
  assert(snapshot.internal.result == H2_PAL_ERR_IO);
  assert(snapshot.external.result == H2_PAL_ERR_IO);
  environment.fail_join = true;
  assert(h2_g99_storage_destroy(&storage) == H2_PAL_ERR_BUSY);
  assert(storage != nullptr && environment.allocations == 1u);
  environment.fail_join = false;
  environment.fail_destroy = true;
  assert(h2_g99_storage_destroy(&storage) == H2_PAL_ERR_IO);
  environment.fail_destroy = false;
  assert(h2_g99_storage_destroy(&storage) == H2_PAL_OK);
  assert(storage == nullptr && environment.handles == 0u);
  assert(environment.allocations == 0u);

  environment.fail_start = true;
  assert(h2_g99_storage_create(&runtime, &api, &storage) ==
         H2_PAL_ERR_NO_MEMORY);
  assert(storage == nullptr && environment.handles == 0u);
  environment.fail_destroy = true;
  assert(h2_g99_storage_create(&runtime, &api, &storage) == H2_PAL_ERR_IO);
  assert(storage != nullptr && environment.allocations == 1u);
  environment.fail_destroy = false;
  assert(h2_g99_storage_destroy(&storage) == H2_PAL_OK);
  assert(environment.handles == 0u && environment.allocations == 0u);
  return 0;
}
