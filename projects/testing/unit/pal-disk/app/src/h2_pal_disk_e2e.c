#include "h2_pal_disk_e2e.h"

#include <string.h>

#include "h2_atomic.h"

enum {
  CHUNK = 256,
  DEFAULT_ROUNDS = 512,
  DEFAULT_DEADLINE_MS = 120000,
  DEFAULT_READY_MS = 5000
};
enum { GO, STOP, WORKER_BASE };
enum { READY, DONE, COUNT, STAGE, CORE, WORKER_VALUES };
enum { VALUE_COUNT = WORKER_BASE + 2 * WORKER_VALUES };

typedef struct worker {
  h2_pal_disk_e2e_t* app;
  unsigned index;
  h2_pal_task_t* task;
  /* Published by DONE; only read after acquiring DONE. */
  int rc;
} worker_t;

struct h2_pal_disk_e2e {
  h2_pal_disk_e2e_config_t config;
  h2_pal_disk_partition_t partition;
  h2_atomic_uint_t values[VALUE_COUNT];
  unsigned initialized;
  worker_t workers[2];
  uint8_t write_buffer[CHUNK];
  uint8_t verify_buffer[CHUNK];
  uint8_t read_buffer[CHUNK];
  int ran;
  int dirty;
  h2_pal_disk_e2e_result_t result;
};

const char* h2_pal_disk_e2e_case_name(h2_pal_disk_e2e_case_t index) {
  static const char* const names[] = {
#define H2_PAL_DISK_CASE(symbol, id) id,
#include "h2_pal_disk_cases.inc"
#undef H2_PAL_DISK_CASE
  };
  return (unsigned)index < H2_PAL_DISK_E2E_CASE_COUNT ? names[index] : NULL;
}

static unsigned key(unsigned worker, unsigned field) {
  return WORKER_BASE + worker * WORKER_VALUES + field;
}

static unsigned load(const h2_pal_disk_e2e_t* app, unsigned field) {
  return h2_atomic_uint_load(&app->values[field], H2_ATOMIC_ACQUIRE);
}

static void store(h2_pal_disk_e2e_t* app, unsigned field, unsigned value) {
  h2_atomic_uint_store(&app->values[field], value, H2_ATOMIC_RELEASE);
}

static void snapshot(h2_pal_disk_e2e_t* app) {
  app->result.workers_joined = 1;
  for (unsigned i = 0; i < 2; ++i) {
    app->result.rounds[i] = load(app, key(i, COUNT));
    app->result.stage[i] = load(app, key(i, STAGE));
    app->result.core[i] = load(app, key(i, CORE));
    if (app->workers[i].task) app->result.workers_joined = 0;
  }
}

static int emit(h2_pal_disk_e2e_t* app, const char* phase) {
  snapshot(app);
  if (!app->result.output_rc)
    app->result.output_rc =
        app->config.emit(app->config.emit_user, phase, &app->result);
  return app->result.output_rc;
}

static int record(h2_pal_disk_e2e_t* app, h2_pal_disk_e2e_case_t index, int rc,
                  int blocked) {
  h2_pal_disk_e2e_status_t status =
      blocked || rc == H2_PAL_ERR_UNSUPPORTED
          ? H2_PAL_DISK_E2E_BLOCKED
          : (rc ? H2_PAL_DISK_E2E_FAIL : H2_PAL_DISK_E2E_PASS);
  app->result.cases[index].status = status;
  app->result.cases[index].rc = rc;
  app->result.current_case = index;
  if (status == H2_PAL_DISK_E2E_PASS) ++app->result.passed;
  if (status == H2_PAL_DISK_E2E_FAIL) ++app->result.failed;
  if (status == H2_PAL_DISK_E2E_BLOCKED) ++app->result.blocked;
  const int output = emit(app, "case");
  return rc ? rc : output;
}

typedef struct partition_scan {
  const h2_pal_disk_partition_t* expected;
  unsigned found;
} partition_scan_t;

static int listed_partition(void* user, const h2_pal_disk_partition_t* value) {
  partition_scan_t* scan = user;
  if (!value) return H2_PAL_ERR_INVALID_STATE;
  const h2_pal_disk_partition_t* expected = scan->expected;
  if (value->id != expected->id) return 0;
  if (++scan->found != 1 || value->flags != expected->flags ||
      value->size != expected->size ||
      value->erase_block_size != expected->erase_block_size ||
      value->write_alignment != expected->write_alignment ||
      strncmp(value->name, expected->name, sizeof(value->name)))
    return H2_PAL_ERR_INVALID_STATE;
  return 0;
}

static int check_partitions(h2_pal_disk_e2e_t* app) {
  partition_scan_t scan = {.expected = &app->partition};
  int rc =
      h2_pal_disk_list_partitions(app->config.disk, listed_partition, &scan);
  if (!rc && scan.found != 1) rc = H2_PAL_ERR_NOT_FOUND;
  return rc;
}

static uint8_t pattern(uint32_t round, uint32_t offset) {
  return (uint8_t)((round * 73u) ^ (offset * 29u) ^ (offset >> 8u) ^ 0xa5u);
}

static int read_pattern(h2_pal_disk_e2e_t* app, uint64_t base, uint32_t round,
                        uint8_t* buffer) {
  for (uint32_t offset = 0; offset < app->partition.erase_block_size;
       offset += CHUNK) {
    int rc = h2_pal_disk_read(app->config.disk, app->config.partition_id,
                              base + offset, buffer, CHUNK);
    if (rc) return rc;
    for (unsigned i = 0; i < CHUNK; ++i)
      if (buffer[i] != pattern(round, offset + i))
        return H2_PAL_DISK_E2E_ERR_DATA;
  }
  return 0;
}

static int write_pattern(h2_pal_disk_e2e_t* app, uint64_t base,
                         uint32_t round) {
  int rc = h2_pal_disk_erase(app->config.disk, app->config.partition_id, base,
                             app->partition.erase_block_size);
  if (rc) return rc;
  for (uint32_t offset = 0; offset < app->partition.erase_block_size;
       offset += CHUNK) {
    for (unsigned i = 0; i < CHUNK; ++i)
      app->write_buffer[i] = pattern(round, offset + i);
    rc = h2_pal_disk_write(app->config.disk, app->config.partition_id,
                           base + offset, app->write_buffer, CHUNK);
    if (rc) return rc;
  }
  return h2_pal_disk_flush(app->config.disk, app->config.partition_id);
}

static int worker_error(h2_pal_disk_e2e_t* app) {
  for (unsigned i = 0; i < 2; ++i)
    if (load(app, key(i, DONE)) && app->workers[i].rc)
      return app->workers[i].rc;
  return 0;
}

static void worker_run(void* context) {
  worker_t* worker = context;
  h2_pal_disk_e2e_t* app = worker->app;
  const unsigned i = worker->index;
  int rc = 0;
  if (app->config.current_core)
    store(app, key(i, CORE), app->config.current_core(app->config.core_user));
  store(app, key(i, READY), 1);
  while (!load(app, GO) && !rc) rc = h2_pal_time_sleep_ms(app->config.time, 1);
  /* Ensure the reader participated even on a host with a very fast Disk. */
  while (!i && !load(app, key(1, COUNT)) && !load(app, STOP) && !rc)
    rc = h2_pal_time_sleep_ms(app->config.time, 1);
  uint32_t count = 0;
  while (!rc && !load(app, STOP)) {
    if (!i) {
      if (count == app->config.rounds) break;
      store(app, key(i, STAGE), H2_PAL_DISK_E2E_STAGE_WRITE);
      rc = write_pattern(app, 0, count + 1);
      if (!rc) {
        store(app, key(i, STAGE), H2_PAL_DISK_E2E_STAGE_VERIFY);
        rc = read_pattern(app, 0, count + 1, app->verify_buffer);
      }
    } else {
      if (load(app, key(0, DONE))) break;
      store(app, key(i, STAGE), H2_PAL_DISK_E2E_STAGE_READ);
      rc = read_pattern(app, app->partition.erase_block_size, 0,
                        app->read_buffer);
    }
    if (!rc) {
      if (count != UINT32_MAX) ++count;
      store(app, key(i, COUNT), count);
    }
  }
  worker->rc = rc;
  if (rc) store(app, STOP, 1);
  store(app, key(i, STAGE), H2_PAL_DISK_E2E_STAGE_DONE);
  store(app, key(i, DONE), 1);
}

static int wait_workers(h2_pal_disk_e2e_t* app, unsigned field,
                        uint32_t timeout_ms) {
  uint64_t started = 0, last_report = 0;
  int rc = h2_pal_time_get_monotonic_ms(app->config.time, &started);
  last_report = started;
  while (!rc && (!load(app, key(0, field)) || !load(app, key(1, field)))) {
    uint64_t now = 0;
    rc = h2_pal_time_get_monotonic_ms(app->config.time, &now);
    if (rc) break;
    if (h2_pal_time_elapsed_ms(started, now) >= timeout_ms)
      return H2_PAL_ERR_TIMEOUT;
    if (h2_pal_time_elapsed_ms(last_report, now) >= 1000u) {
      rc = emit(app, "progress");
      last_report = now;
    }
    if (!rc) rc = h2_pal_time_sleep_ms(app->config.time, 1);
  }
  return rc;
}

static int cleanup(h2_pal_disk_e2e_t* app) {
  /* Never erase while a worker can still be executing a Disk callback. */
  for (unsigned i = 0; i < 2; ++i) {
    worker_t* worker = &app->workers[i];
    if (!worker->task) continue;
    if (!load(app, key(i, DONE))) return H2_PAL_ERR_BUSY;
    int rc = h2_pal_task_join(app->config.task, worker->task);
    if (rc) return rc;
    worker->task = NULL;
  }
  if (!app->dirty) return 0;
  const uint64_t block = app->partition.erase_block_size;
  for (unsigned i = 0; i < 2; ++i) {
    int rc = h2_pal_disk_erase(app->config.disk, app->config.partition_id,
                               i * block, block);
    if (rc) return rc;
  }
  int rc = h2_pal_disk_flush(app->config.disk, app->config.partition_id);
  if (rc) return rc;
  for (uint64_t offset = 0; offset < 2 * block; offset += CHUNK) {
    rc = h2_pal_disk_read(app->config.disk, app->config.partition_id, offset,
                          app->verify_buffer, CHUNK);
    if (rc) return rc;
    for (unsigned i = 0; i < CHUNK; ++i)
      if (app->verify_buffer[i] != 0xff) return H2_PAL_DISK_E2E_ERR_ERASE;
  }
  app->dirty = 0;
  return 0;
}

static void free_app(h2_pal_disk_e2e_t* app) {
  for (unsigned i = 0; i < app->initialized; ++i)
    h2_atomic_uint_destroy(&app->values[i]);
  h2_pal_mem_free(app->config.mem, app);
}

int h2_pal_disk_e2e_create(const h2_pal_disk_e2e_config_t* config,
                           h2_pal_disk_e2e_t** out) {
  if (!out) return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  if (!config || !config->emit || !config->mem || !config->mem->vtable ||
      !config->mem->vtable->alloc || !config->mem->vtable->free ||
      !config->disk || !config->disk->vtable ||
      !config->disk->vtable->get_partition || !config->disk->vtable->erase ||
      !config->disk->vtable->write || !config->disk->vtable->read ||
      !config->disk->vtable->flush || !config->task || !config->task->vtable ||
      !config->task->vtable->start || !config->task->vtable->join ||
      !config->time || !config->time->vtable ||
      !config->time->vtable->get_monotonic_ms ||
      !config->time->vtable->sleep_ms ||
      (config->current_core &&
       config->expected_core[0] == config->expected_core[1]))
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_disk_partition_t partition = {0};
  int rc =
      h2_pal_disk_get_partition(config->disk, config->partition_id, &partition);
  if (rc) return rc;
  const uint32_t flags = H2_PAL_DISK_PARTITION_FLAG_READABLE |
                         H2_PAL_DISK_PARTITION_FLAG_WRITABLE |
                         H2_PAL_DISK_PARTITION_FLAG_ERASABLE;
  if (partition.id != config->partition_id ||
      strncmp(partition.name, "pal_disk", sizeof(partition.name)) ||
      (partition.flags & flags) != flags ||
      (partition.flags & H2_PAL_DISK_PARTITION_FLAG_BOOTABLE) ||
      !partition.erase_block_size || partition.erase_block_size % CHUNK ||
      !partition.write_alignment || CHUNK % partition.write_alignment ||
      partition.size < 2 * (uint64_t)partition.erase_block_size)
    return H2_PAL_DISK_E2E_ERR_REGION;
  h2_pal_disk_e2e_t* app = h2_pal_mem_alloc(config->mem, sizeof(*app));
  if (!app) return H2_PAL_ERR_NO_MEMORY;
  memset(app, 0, sizeof(*app));
  app->config = *config;
  app->partition = partition;
  if (!app->config.rounds) app->config.rounds = DEFAULT_ROUNDS;
  if (!app->config.deadline_ms) app->config.deadline_ms = DEFAULT_DEADLINE_MS;
  if (!app->config.ready_ms) app->config.ready_ms = DEFAULT_READY_MS;
  for (; app->initialized < VALUE_COUNT; ++app->initialized) {
    h2_atomic_result_t ar =
        h2_atomic_uint_init(&app->values[app->initialized], 0);
    if (ar != H2_ATOMIC_OK) {
      free_app(app);
      return ar == H2_ATOMIC_NO_MEMORY ? H2_PAL_ERR_NO_MEMORY
                                       : H2_PAL_ERR_UNSUPPORTED;
    }
  }
  for (unsigned i = 0; i < 2; ++i) {
    app->workers[i].app = app;
    app->workers[i].index = i;
  }
  *out = app;
  return 0;
}

static int sequential(h2_pal_disk_e2e_t* app) {
  app->dirty = 1;
  int rc = 0;
  for (unsigned i = 0; i < 2; ++i) {
    const uint64_t base = (uint64_t)i * app->partition.erase_block_size;
    rc = write_pattern(app, base, 0);
    if (!rc) rc = read_pattern(app, base, 0, app->verify_buffer);
    if (rc) return rc;
  }
  return rc;
}

static int concurrent(h2_pal_disk_e2e_t* app) {
  int rc = 0;
  static const char* const names[] = {"pal-disk/e2e/writer",
                                      "pal-disk/e2e/reader"};
  for (unsigned i = 0; i < 2; ++i) {
    const h2_pal_task_options_t options = {.name = names[i],
                                           .min_stack_size = 4096};
    rc = h2_pal_task_start(app->config.task, &options, worker_run,
                           &app->workers[i], &app->workers[i].task);
    if (rc) return rc;
  }
  rc = wait_workers(app, READY, app->config.ready_ms);
  if (rc) return rc;
  if (app->config.current_core) {
    for (unsigned i = 0; i < 2; ++i)
      if (load(app, key(i, CORE)) != app->config.expected_core[i])
        return H2_PAL_DISK_E2E_ERR_CORE;
    app->result.cores_verified = 1;
  }
  rc = emit(app, "concurrent_begin");
  if (rc) return rc;
  store(app, GO, 1);
  rc = wait_workers(app, DONE, app->config.deadline_ms);
  if (rc) return rc;
  rc = worker_error(app);
  if (!rc && (load(app, key(0, COUNT)) != app->config.rounds ||
              !load(app, key(1, COUNT))))
    rc = H2_PAL_DISK_E2E_ERR_INCOMPLETE;
  return rc;
}

int h2_pal_disk_e2e_run(h2_pal_disk_e2e_t* app,
                        h2_pal_disk_e2e_result_t* result) {
  if (!result) return H2_PAL_ERR_INVALID_ARG;
  memset(result, 0, sizeof(*result));
  result->rc = H2_PAL_ERR_INVALID_STATE;
  if (!app || app->ran) return result->rc;
  app->ran = 1;
  int rc =
      record(app, H2_PAL_DISK_E2E_CASE_PARTITIONS, check_partitions(app), 0);
  if (!rc) rc = emit(app, "region");
  const int ran_sequential = rc == 0;
  int sequential_rc = rc ? H2_PAL_ERR_UNAVAILABLE : sequential(app);
  int next =
      record(app, H2_PAL_DISK_E2E_CASE_SEQUENTIAL, sequential_rc, rc != 0);
  if (!rc) rc = next;
  if (!rc) rc = emit(app, "sequential");
  int concurrent_rc = rc ? H2_PAL_ERR_UNAVAILABLE : concurrent(app);
  next = record(app, H2_PAL_DISK_E2E_CASE_CONCURRENT, concurrent_rc, rc != 0);
  if (!rc) rc = next;
  app->result.rc = rc;
  store(app, STOP, 1);
  store(app, GO, 1);
  app->result.cleanup_rc = cleanup(app);
  (void)record(app, H2_PAL_DISK_E2E_CASE_CLEANUP,
               ran_sequential ? app->result.cleanup_rc : H2_PAL_ERR_UNAVAILABLE,
               !ran_sequential);
  if (!app->result.rc) app->result.rc = app->result.cleanup_rc;
  (void)emit(app, "result");
  if (!app->result.rc) app->result.rc = app->result.output_rc;
  snapshot(app);
  *result = app->result;
  return result->rc;
}

int h2_pal_disk_e2e_destroy(h2_pal_disk_e2e_t** app) {
  if (!app) return H2_PAL_ERR_INVALID_ARG;
  if (!*app) return 0;
  store(*app, STOP, 1);
  store(*app, GO, 1);
  int rc = cleanup(*app);
  if (rc) return rc;
  free_app(*app);
  *app = NULL;
  return 0;
}
