#define _POSIX_C_SOURCE 200809L
#include "h2_flash_stress.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { BLOCK = 4096, CAPACITY = 3 * BLOCK, IO_ERROR = -4321 };
typedef enum fault {
  NORMAL,
  CORRUPT_READ,
  WRITE_ERROR,
  ERASE_ERROR,
  FLUSH_ERROR,
  BLOCK_WRITER
} fault_t;

typedef struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  uint8_t bytes[CAPACITY];
  h2_pal_disk_partition_t partition;
  h2_pal_disk_api_t disk;
  h2_pal_task_api_t task;
  h2_pal_time_api_t time;
  h2_pal_mem_api_t mem;
  h2_flash_stress_config_t config;
  fault_t fault;
  unsigned starts, joins, allocations, frees, erases, writes;
  int blocked, released, fail_start, fail_join, fail_alloc, wrong_core;
  const char* fail_output;
  unsigned results;
} fixture_t;

struct h2_pal_task {
  pthread_t thread;
  h2_pal_task_entry_t entry;
  void* context;
  unsigned role;
};
static _Thread_local int role = -1;

static int get_partition(void* user, uint32_t id,
                         h2_pal_disk_partition_t* out) {
  fixture_t* f = user;
  assert(id == 42);
  *out = f->partition;
  return 0;
}

static void check_range(uint32_t id, uint64_t offset, uint64_t len) {
  assert(id == 42);
  assert(offset <= 2 * BLOCK && len <= 2 * BLOCK - offset);
}

static int read_disk(void* user, uint32_t id, uint64_t offset, void* data,
                     size_t len) {
  fixture_t* f = user;
  check_range(id, offset, len);
  assert(pthread_mutex_lock(&f->mutex) == 0);
  memcpy(data, f->bytes + offset, len);
  if (role == 1 && f->fault == CORRUPT_READ && len) ((uint8_t*)data)[0] ^= 1;
  assert(pthread_mutex_unlock(&f->mutex) == 0);
  return 0;
}

static int erase_disk(void* user, uint32_t id, uint64_t offset, uint64_t len) {
  fixture_t* f = user;
  check_range(id, offset, len);
  assert(offset % BLOCK == 0 && len == BLOCK);
  assert(pthread_mutex_lock(&f->mutex) == 0);
  if (role == 0 && f->fault == BLOCK_WRITER) {
    f->blocked = 1;
    while (!f->released)
      assert(pthread_cond_wait(&f->condition, &f->mutex) == 0);
  }
  int rc = role == 0 && f->fault == ERASE_ERROR ? IO_ERROR : 0;
  if (!rc) {
    memset(f->bytes + offset, 0xff, (size_t)len);
    ++f->erases;
  }
  assert(pthread_mutex_unlock(&f->mutex) == 0);
  return rc;
}

static int write_disk(void* user, uint32_t id, uint64_t offset,
                      const void* data, size_t len) {
  fixture_t* f = user;
  check_range(id, offset, len);
  assert(pthread_mutex_lock(&f->mutex) == 0);
  int rc = role == 0 && f->fault == WRITE_ERROR ? IO_ERROR : 0;
  if (!rc) {
    for (size_t i = 0; i < len; ++i) {
      const uint8_t byte = ((const uint8_t*)data)[i];
      assert((f->bytes[offset + i] & byte) == byte);
      f->bytes[offset + i] = byte;
    }
    ++f->writes;
  }
  assert(pthread_mutex_unlock(&f->mutex) == 0);
  return rc;
}

static int flush_disk(void* user, uint32_t id) {
  fixture_t* f = user;
  assert(id == 42);
  return role == 0 && f->fault == FLUSH_ERROR ? IO_ERROR : 0;
}

static void* trampoline(void* context) {
  h2_pal_task_t* task = context;
  role = (int)task->role;
  task->entry(task->context);
  return NULL;
}

static int start(void* user, const h2_pal_task_options_t* options,
                 h2_pal_task_entry_t entry, void* context,
                 h2_pal_task_t** out) {
  fixture_t* f = user;
  *out = NULL;
  ++f->starts;
  if (f->fail_start && f->starts == 2) return H2_PAL_ERR_TASK;
  assert(options->min_stack_size >= 4096);
  h2_pal_task_t* task = calloc(1, sizeof(*task));
  assert(task);
  task->role = f->starts - 1;
  assert(strcmp(options->name, task->role ? "flash-stress/reader"
                                          : "flash-stress/writer") == 0);
  task->entry = entry;
  task->context = context;
  assert(pthread_create(&task->thread, NULL, trampoline, task) == 0);
  *out = task;
  return 0;
}

static int join(void* user, h2_pal_task_t* task) {
  fixture_t* f = user;
  if (f->fail_join) {
    f->fail_join = 0;
    return H2_PAL_ERR_TASK;
  }
  assert(pthread_join(task->thread, NULL) == 0);
  ++f->joins;
  free(task);
  return 0;
}

static int now(void* user, uint64_t* out) {
  (void)user;
  struct timespec ts;
  assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
  *out = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
  return 0;
}

static int sleep_ms(void* user, uint32_t ms) {
  (void)user;
  struct timespec ts = {.tv_sec = ms / 1000,
                        .tv_nsec = (long)(ms % 1000) * 1000000};
  while (nanosleep(&ts, &ts) != 0) assert(errno == EINTR);
  return 0;
}

static void* allocate(void* user, size_t size) {
  fixture_t* f = user;
  if (f->fail_alloc) return NULL;
  void* ptr = malloc(size);
  if (ptr) ++f->allocations;
  return ptr;
}

static void deallocate(void* user, void* ptr) {
  fixture_t* f = user;
  ++f->frees;
  free(ptr);
}

static uint32_t core(void* user) {
  fixture_t* f = user;
  return f->wrong_core ? 7u : (uint32_t)role + 1;
}

static int output(void* user, const char* phase,
                  const h2_flash_stress_result_t* result) {
  fixture_t* f = user;
  assert(result);
  if (strcmp(phase, "result") == 0) ++f->results;
  return f->fail_output && strcmp(f->fail_output, phase) == 0 ? H2_PAL_ERR_WRITE
                                                              : 0;
}

static void init(fixture_t* f) {
  memset(f, 0, sizeof(*f));
  memset(f->bytes, 0x5a, sizeof(f->bytes));
  assert(pthread_mutex_init(&f->mutex, NULL) == 0);
  assert(pthread_cond_init(&f->condition, NULL) == 0);
  f->partition =
      (h2_pal_disk_partition_t){.id = 42,
                                .name = "flash_stress",
                                .size = CAPACITY,
                                .erase_block_size = BLOCK,
                                .write_alignment = 4,
                                .flags = H2_PAL_DISK_PARTITION_FLAG_READABLE |
                                         H2_PAL_DISK_PARTITION_FLAG_WRITABLE |
                                         H2_PAL_DISK_PARTITION_FLAG_ERASABLE};
  static const h2_pal_disk_vtable_t disk = {.get_partition = get_partition,
                                            .read = read_disk,
                                            .write = write_disk,
                                            .erase = erase_disk,
                                            .flush = flush_disk};
  static const h2_pal_task_vtable_t task = {.start = start, .join = join};
  static const h2_pal_time_vtable_t time = {.get_monotonic_ms = now,
                                            .sleep_ms = sleep_ms};
  static const h2_pal_mem_vtable_t mem = {.alloc = allocate,
                                          .free = deallocate};
  f->disk = (h2_pal_disk_api_t){f, &disk};
  f->task = (h2_pal_task_api_t){f, &task};
  f->time = (h2_pal_time_api_t){f, &time};
  f->mem = (h2_pal_mem_api_t){f, &mem};
  f->config = (h2_flash_stress_config_t){.disk = &f->disk,
                                         .task = &f->task,
                                         .time = &f->time,
                                         .mem = &f->mem,
                                         .partition_id = 42,
                                         .rounds = 8,
                                         .deadline_ms = 2000,
                                         .ready_ms = 2000,
                                         .current_core = core,
                                         .core_user = f,
                                         .expected_core = {1, 2},
                                         .emit = output,
                                         .emit_user = f};
}

static void finish(fixture_t* f, h2_flash_stress_t** app) {
  for (unsigned i = 0; i < 2000; ++i) {
    int rc = h2_flash_stress_destroy(app);
    if (!rc) break;
    assert(rc == H2_PAL_ERR_BUSY);
    sleep_ms(NULL, 1);
  }
  assert(!*app);
  assert(f->allocations == f->frees);
  assert(f->joins == f->starts - (f->fail_start ? 1u : 0u));
  for (unsigned i = 2 * BLOCK; i < CAPACITY; ++i) assert(f->bytes[i] == 0x5a);
  assert(pthread_cond_destroy(&f->condition) == 0);
  assert(pthread_mutex_destroy(&f->mutex) == 0);
}

static void assert_clean(fixture_t* f) {
  for (unsigned i = 0; i < 2 * BLOCK; ++i) assert(f->bytes[i] == 0xff);
}

static void test_normal(int observe_cores) {
  fixture_t f;
  init(&f);
  if (!observe_cores) f.config.current_core = NULL;
  h2_flash_stress_t* app = NULL;
  assert(h2_flash_stress_create(&f.config, &app) == 0);
  h2_flash_stress_result_t result;
  assert(h2_flash_stress_run(app, &result) == 0);
  assert(result.rc == 0 && result.cleanup_rc == 0 && result.output_rc == 0);
  assert(result.rounds[0] == 8 && result.rounds[1] > 0);
  assert(result.cores_verified == observe_cores);
  assert(result.workers_joined && f.results == 1);
  assert_clean(&f);
  assert(h2_flash_stress_run(app, &result) == H2_PAL_ERR_INVALID_STATE);
  finish(&f, &app);
  assert(h2_flash_stress_destroy(&app) == 0);
}

static void test_fault(fault_t fault) {
  fixture_t f;
  init(&f);
  f.fault = fault;
  h2_flash_stress_t* app = NULL;
  assert(h2_flash_stress_create(&f.config, &app) == 0);
  h2_flash_stress_result_t result;
  int expected = fault == CORRUPT_READ ? H2_FLASH_STRESS_ERR_DATA : IO_ERROR;
  assert(h2_flash_stress_run(app, &result) == expected);
  assert(result.rc == expected && result.cleanup_rc == 0);
  assert_clean(&f);
  finish(&f, &app);
}

static void test_reject_region(void) {
  for (unsigned i = 0; i < 8; ++i) {
    fixture_t f;
    init(&f);
    switch (i) {
      case 0:
        f.partition.flags |= H2_PAL_DISK_PARTITION_FLAG_BOOTABLE;
        break;
      case 1:
        f.partition.size = BLOCK;
        break;
      case 2:
        f.partition.erase_block_size = 0;
        break;
      case 3:
        f.partition.write_alignment = 3;
        break;
      case 4:
        strcpy(f.partition.name, "application");
        break;
      case 5:
        memset(f.partition.name, 'x', sizeof(f.partition.name));
        break;
      case 6:
        f.partition.flags = H2_PAL_DISK_PARTITION_FLAG_READABLE;
        break;
      case 7:
        f.partition.id = 41;
        break;
    }
    h2_flash_stress_t* app = NULL;
    assert(h2_flash_stress_create(&f.config, &app) ==
           H2_FLASH_STRESS_ERR_REGION);
    assert(!app && !f.erases && !f.writes && !f.allocations);
    finish(&f, &app);
  }
}

static void test_lifecycle_failure(unsigned which) {
  fixture_t f;
  init(&f);
  if (which == 0) f.wrong_core = 1;
  if (which == 1) f.fail_start = 1;
  if (which == 2) f.fail_join = 1;
  h2_flash_stress_t* app = NULL;
  assert(h2_flash_stress_create(&f.config, &app) == 0);
  h2_flash_stress_result_t result;
  int expected = which == 0 ? H2_FLASH_STRESS_ERR_CORE : H2_PAL_ERR_TASK;
  assert(h2_flash_stress_run(app, &result) == expected);
  assert(app && f.frees == 0);
  finish(&f, &app);
  assert_clean(&f);
}

static void test_output_failure(const char* phase) {
  fixture_t f;
  init(&f);
  f.fail_output = phase;
  h2_flash_stress_t* app = NULL;
  assert(h2_flash_stress_create(&f.config, &app) == 0);
  h2_flash_stress_result_t result;
  assert(h2_flash_stress_run(app, &result) == H2_PAL_ERR_WRITE);
  assert(result.rc == H2_PAL_ERR_WRITE && result.output_rc == H2_PAL_ERR_WRITE);
  finish(&f, &app);
}

static void test_blocked_worker(void) {
  fixture_t f;
  init(&f);
  f.fault = BLOCK_WRITER;
  f.config.deadline_ms = 50;
  h2_flash_stress_t* app = NULL;
  assert(h2_flash_stress_create(&f.config, &app) == 0);
  h2_flash_stress_result_t result;
  assert(h2_flash_stress_run(app, &result) == H2_PAL_ERR_TIMEOUT);
  assert(result.cleanup_rc == H2_PAL_ERR_BUSY && !result.workers_joined);
  assert(pthread_mutex_lock(&f.mutex) == 0);
  assert(f.blocked && f.erases == 2 && f.frees == 0);
  assert(pthread_mutex_unlock(&f.mutex) == 0);
  assert(h2_flash_stress_destroy(&app) == H2_PAL_ERR_BUSY);
  assert(app && f.frees == 0);
  assert(pthread_mutex_lock(&f.mutex) == 0);
  f.released = 1;
  assert(pthread_cond_broadcast(&f.condition) == 0);
  assert(pthread_mutex_unlock(&f.mutex) == 0);
  finish(&f, &app);
  assert_clean(&f);
}

static void test_create_failure(void) {
  fixture_t f;
  init(&f);
  h2_flash_stress_t* app = NULL;
  f.fail_alloc = 1;
  assert(h2_flash_stress_create(&f.config, &app) == H2_PAL_ERR_NO_MEMORY);
  assert(!app);
  f.config.emit = NULL;
  assert(h2_flash_stress_create(&f.config, &app) == H2_PAL_ERR_INVALID_ARG);
  assert(!app && !f.erases && !f.writes);
  finish(&f, &app);
}

int main(void) {
  test_normal(0);
  test_normal(1);
  test_reject_region();
  test_create_failure();
  for (unsigned i = 0; i < 3; ++i) test_lifecycle_failure(i);
  for (fault_t fault = CORRUPT_READ; fault <= FLUSH_ERROR; ++fault)
    test_fault(fault);
  test_output_failure("region");
  test_output_failure("sequential");
  test_output_failure("concurrent_begin");
  test_output_failure("result");
  test_blocked_worker();
  return 0;
}
