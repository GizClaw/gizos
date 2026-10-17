#include "h2_libco_smoke_test_sdk.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int observed_printf(const char *format, ...);
static int observed_fflush(FILE *stream);
static int observed_smoke_run(h2_runtime_t *runtime,
                               const h2_libco_smoke_config_t *config);

/* Exercise the real entry and portable suite, replacing only SDK observations
 * and terminal hold. The shared console still performs real framing/gating. */
#define printf observed_printf
#define fflush observed_fflush
#define h2_libco_smoke_run observed_smoke_run
#include "projects/e2e/targets/h2loader_tar_zlib/libco-smoke/devkit/main/main.c"
#undef h2_libco_smoke_run
#undef fflush
#undef printf

static jmp_buf hold_boundary;
static h2_runtime_t test_runtime;
static struct {
  unsigned initialized, prepared, waited, samples, runs, confirms, restarts;
  unsigned writes, drains, live_allocations, delegated, visible;
  unsigned fail_write, short_write, fail_drain, fail_checkpoint;
  unsigned router_owned, operation_owned, operation_locks, operation_unlocks;
  unsigned bad_count, ignore_run_error, missing_console, wrong_core;
  int confirm_result, root_write_error, root_flush_error;
  int confirm_write_error, ready_write_error;
  int router_cookie, operation_cookie;
  uint64_t now;
  size_t bytes, drained_bytes;
  char output[16384], last_record[1024];
} state;

static void append(const char *data, size_t length) {
  assert(state.bytes + length < sizeof(state.output));
  memcpy(state.output + state.bytes, data, length);
  state.bytes += length;
  state.output[state.bytes] = '\0';
}

static int observed_printf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  int length = vsnprintf(state.last_record, sizeof(state.last_record), format, args);
  va_end(args);
  assert(length >= 0 && (size_t)length < sizeof(state.last_record));
  if (strstr(state.last_record, "H2_ESP_LIBCO_ROOT ") == state.last_record) {
    assert(state.wrong_core || (state.waited && state.samples == 2u));
    if (state.root_write_error) return -1;
  } else {
    assert(state.wrong_core || state.waited);
  }
  if (state.confirm_write_error && strstr(state.last_record, "H2_ESP_LIBCO_CONFIRM ")) return -1;
  if (state.ready_write_error && strstr(state.last_record, "H2_ESP_LIBCO_SMOKE_READY ")) return -1;
  append(state.last_record, (size_t)length);
  return length;
}

static int observed_fflush(FILE *stream) {
  assert(stream == stdout);
  return state.root_flush_error && strstr(state.last_record, "H2_ESP_LIBCO_ROOT ") ? EOF : 0;
}

BaseType_t xTaskGetCoreID(void *task) { assert(task == NULL); return 0; }
BaseType_t xPortGetCoreID(void) { return state.wrong_core ? 1 : 0; }
StackType_t *xTaskGetStackStart(void *task) {
  assert(task == NULL);
  return (StackType_t *)(uintptr_t)1000u;
}
uintptr_t esp_cpu_get_sp(void) {
  return 1000u + (state.samples == 0u ? 7000u : state.samples == 1u ? 6500u : 6000u);
}
bool heap_caps_check_integrity_all(bool print_errors) {
  assert(print_errors);
  ++state.samples;
  if (state.samples == 1u) assert(!state.initialized);
  if (state.samples == 2u) assert(state.initialized && !state.waited);
  return state.samples != state.fail_checkpoint;
}
UBaseType_t uxTaskGetStackHighWaterMark(void *task) {
  assert(task == NULL);
  return state.samples == 1u ? 6800u : state.samples == 2u ? 6300u : 5800u;
}
void vTaskDelay(uint32_t ms) {
  if (ms == 5000u) {
    assert(state.initialized && state.prepared && state.samples == 2u);
    assert(state.bytes == 0u && state.waited == 0u);
    state.waited = 1u;
    return;
  }
  assert(ms == 1000u);
  longjmp(hold_boundary, 1);
}
void esp_restart(void) {
  ++state.restarts;
  longjmp(hold_boundary, 2);
}
int h2_esp_target_task_policy_install(void) { return H2_PAL_OK; }

static void *alloc(void *user, size_t size) {
  (void)user;
  void *value = malloc(size);
  if (value != NULL) ++state.live_allocations;
  return value;
}
static void free_memory(void *user, void *memory) {
  (void)user;
  assert(memory != NULL && state.live_allocations != 0u);
  --state.live_allocations;
  free(memory);
}
static int now(void *user, uint64_t *out) { (void)user; *out = state.now; return 0; }
static int sleep_ms(void *user, uint32_t ms) { (void)user; state.now += ms; return 0; }
static int original_log(void *user, h2_pal_log_level_t level,
                        const char *scope, const char *message) {
  (void)user;
  assert(scope != NULL && message != NULL);
  ++state.delegated;
  if (level >= H2_PAL_LOG_WARN) ++state.visible;
  return H2_PAL_OK;
}
static int create_mutex(void *user, const h2_pal_mutex_config_t *config,
                         h2_pal_mutex_t **out) {
  (void)user;
  assert(strcmp(config->name, "e2e/result") == 0);
  *out = (h2_pal_mutex_t *)&state.router_cookie;
  return 0;
}
static int destroy_mutex(void *user, h2_pal_mutex_t *mutex) {
  (void)user;
  assert(mutex == (h2_pal_mutex_t *)&state.router_cookie && !state.router_owned);
  return 0;
}
static int lock_mutex(void *user, h2_pal_mutex_t *mutex) {
  (void)user;
  if (mutex == (h2_pal_mutex_t *)&state.router_cookie) {
    assert(!state.router_owned && !state.operation_owned);
    state.router_owned = 1u;
  } else {
    assert(mutex == (h2_pal_mutex_t *)&state.operation_cookie && !state.operation_owned && !state.router_owned);
    state.operation_owned = 1u;
    ++state.operation_locks;
  }
  return 0;
}
static int unlock_mutex(void *user, h2_pal_mutex_t *mutex) {
  (void)user;
  if (mutex == (h2_pal_mutex_t *)&state.router_cookie) {
    assert(state.router_owned);
    state.router_owned = 0u;
  } else {
    assert(mutex == (h2_pal_mutex_t *)&state.operation_cookie && state.operation_owned);
    state.operation_owned = 0u;
    ++state.operation_unlocks;
  }
  return 0;
}
static const h2_pal_mem_vtable_t mem_vtable = {.alloc = alloc, .free = free_memory};
static const h2_pal_time_vtable_t time_vtable = {.get_monotonic_ms = now, .sleep_ms = sleep_ms};
static const h2_pal_log_vtable_t log_vtable = {.write = original_log};
static const h2_pal_sync_vtable_t sync_vtable = {
    .create_mutex = create_mutex, .destroy_mutex = destroy_mutex,
    .lock_mutex = lock_mutex, .unlock_mutex = unlock_mutex};
static const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};
static const h2_pal_time_api_t clock_api = {.vtable = &time_vtable};
static const h2_pal_log_api_t original = {.vtable = &log_vtable};
static const h2_pal_sync_api_t sync_api = {.vtable = &sync_vtable};

int h2_esp_board_runtime_config(h2_runtime_config_t *config) {
  assert(state.samples == 1u && !state.initialized);
  *config = (h2_runtime_config_t){.mem = &mem, .time = &clock_api, .log = &original, .sync = &sync_api};
  return 0;
}
int h2_esp_h2loader_app_commands_prepare_serial_with_config(
    const h2_runtime_config_t *runtime_config,
    const h2_esp_h2loader_app_commands_config_t *config) {
  assert(runtime_config->system_event == h2_pal_unsupported_system_event_api());
  assert(strcmp(config->active_name, "libco-smoke") == 0 && config->hardware_capabilities == H2_LOADER_CAPABILITY_UART);
  assert(config->h2loader_partition_id == 1u && config->coredump_partition_id == 3u);
  state.prepared = 1u;
  return 0;
}
int h2_runtime_init(const h2_runtime_config_t *config, h2_runtime_t **out) {
  assert(state.prepared && !state.initialized);
  test_runtime = (h2_runtime_t){.mem = config->mem, .time = config->time, .log = config->log, .sync = config->sync};
  *out = &test_runtime;
  state.initialized = 1u;
  return 0;
}

static int write_uart(void *user, const void *data, size_t length,
                       size_t *written, uint32_t timeout_ms) {
  (void)user;
  assert(state.waited && state.router_owned && timeout_ms == 5000u);
  ++state.writes;
  *written = 0u;
  if (state.writes == state.fail_write) return H2_PAL_ERR_TIMEOUT;
  if (state.writes == state.short_write) length = 3u;
  append(data, length);
  *written = length;
  return 0;
}
static int drain_uart(void *user) {
  (void)user;
  assert(state.router_owned);
  ++state.drains;
  if (state.drains == state.fail_drain) return H2_PAL_ERR_TIMEOUT;
  state.drained_bytes = state.bytes;
  return 0;
}
static const h2_pal_uart_io_stream_vtable_t uart_vtable = {.write = write_uart, .flush = drain_uart};
static const h2_pal_uart_io_stream_api_t uart = {.vtable = &uart_vtable};
int h2_esp_e2e_result_log_prepare(h2_esp_e2e_result_log_t *router,
    h2_runtime_t *runtime, const char *scope, const char *second_scope) {
  assert(state.waited && runtime == &test_runtime && runtime->log == &original);
  assert(strcmp(scope, "libco-smoke") == 0 && second_scope == NULL);
  int rc = h2_esp_e2e_result_log_init(router, runtime->log, runtime->sync,
      state.missing_console ? NULL : &uart, NULL, scope, second_scope);
  if (rc == 0) runtime->log = &router->log;
  return rc;
}

static int observed_smoke_run(h2_runtime_t *runtime,
                               const h2_libco_smoke_config_t *config) {
  assert(state.samples == 3u && state.waited && config == NULL);
  ++state.runs;
  assert(h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "provider", "hidden") == 0);
  assert(h2_pal_log_write(runtime->log, H2_PAL_LOG_WARN, "provider", "visible") == 0);
  int rc = h2_libco_smoke_run(runtime, config);
  if (state.bad_count) {
    assert(result_log.records == 7u);
    if (state.bad_count == 1u) --result_log.records;
    else ++result_log.records;
  }
  return state.ignore_run_error ? H2_PAL_OK : rc;
}
int h2_esp_h2loader_app_commands_get_config(h2_loader_app_client_config_t *config) {
  assert(state.writes == 7u && state.drains == 7u && state.runs == 1u && !state.live_allocations);
  assert(!state.router_owned && result_log.records == 7u && h2_esp_e2e_result_log_error(&result_log) == 0);
  *config = (h2_loader_app_client_config_t){.operation_sync = &sync_api, .operation_mutex = (h2_pal_mutex_t *)&state.operation_cookie};
  return 0;
}
int h2_esp_h2loader_app_confirm(h2_runtime_t *runtime) {
  assert(runtime == &test_runtime && state.operation_owned && !state.router_owned);
  assert(state.samples == 5u && state.drained_bytes != 0u);
  ++state.confirms;
  return state.confirm_result;
}

static size_t occurrences(const char *needle) {
  size_t count = 0u;
  for (const char *at = state.output; (at = strstr(at, needle)) != NULL; at += strlen(needle)) ++count;
  return count;
}
static void reset(void) {
  if (result_log.error.storage != NULL) assert(h2_esp_e2e_result_log_deinit(&result_log) == 0);
  memset(&state, 0, sizeof(state));
}
static void run_entry(void) {
  if (setjmp(hold_boundary) == 0) app_main();
  assert(!state.live_allocations && !state.router_owned && !state.operation_owned);
  assert(state.operation_locks == state.operation_unlocks);
}

int main(void) {
  reset();
  run_entry();
  assert(state.confirms == 1u && state.samples == 6u && !state.restarts);
  assert(state.writes == 7u && state.drains == 7u && state.delegated == 2u && state.visible == 1u);
  assert(occurrences("H2_LIBCO_SMOKE_STAGE stage=") == 6u && occurrences("H2_LIBCO_SMOKE_PASS rc=0") == 1u);
  assert(occurrences("H2_ESP_LIBCO_ROOT ") == 1u && occurrences("H2_ESP_LIBCO_CHECKPOINT ") == 6u);
  assert(strstr(state.output, "stage=boot heap_valid=1 main_stack_free=7000 main_stack_min_free=6800 rc=0"));
  assert(strstr(state.output, "stage=runtime heap_valid=1 main_stack_free=6500 main_stack_min_free=6300 rc=0"));
  assert(occurrences("H2_ESP_LIBCO_CONFIRM rc=0") == 1u && occurrences("H2_ESP_LIBCO_SMOKE_READY rc=0") == 1u);
  assert(!strstr(state.output, "FAIL"));
  static const char *const ledger[] = {
      "H2_ESP_LIBCO_ROOT ", "CHECKPOINT stage=boot ",
      "CHECKPOINT stage=runtime ", "CHECKPOINT stage=before_libco ",
      "STAGE stage=fifo-yield\r\n", "STAGE stage=wait-wake\r\n",
      "STAGE stage=timeout\r\n", "STAGE stage=cancel\r\n",
      "STAGE stage=join\r\n", "STAGE stage=switch-stress\r\n",
      "H2_LIBCO_SMOKE_PASS rc=0\r\n", "CHECKPOINT stage=after_libco ",
      "CHECKPOINT stage=before_confirm ", "H2_ESP_LIBCO_CONFIRM rc=0\n",
      "CHECKPOINT stage=after_confirm ", "H2_ESP_LIBCO_SMOKE_READY rc=0\n",
  };
  const char *at = state.output;
  for (size_t i = 0u; i < sizeof(ledger) / sizeof(ledger[0]); ++i) {
    at = strstr(at, ledger[i]);
    assert(at != NULL);
    at += strlen(ledger[i]);
  }
  for (unsigned boundary = 1u; boundary <= 7u; ++boundary) {
    for (unsigned kind = 0u; kind < 3u; ++kind) {
      reset();
      if (kind == 0u) state.fail_write = boundary;
      if (kind == 1u) state.short_write = boundary;
      if (kind == 2u) state.fail_drain = boundary;
      run_entry();
      assert(!state.confirms && !state.operation_locks);
      assert(strstr(state.output, "H2_ESP_LIBCO_SMOKE_FAIL stage=portable "));
      assert(!strstr(state.output, "H2_ESP_LIBCO_SMOKE_READY"));
    }
  }
  for (unsigned count = 1u; count <= 2u; ++count) {
    reset(); state.bad_count = count; run_entry();
    assert(!state.confirms && strstr(state.output, "FAIL stage=result_output "));
  }
  reset(); state.fail_drain = 7u; state.ignore_run_error = 1u; run_entry();
  assert(!state.confirms && strstr(state.output, "FAIL stage=result_output "));
  reset(); state.missing_console = 1u; run_entry();
  assert(!state.runs && !state.confirms && strstr(state.output, "FAIL stage=result_console "));
  for (unsigned point = 1u; point <= 6u; ++point) {
    reset(); state.fail_checkpoint = point; run_entry();
    assert(state.confirms == (point == 6u ? 1u : 0u));
    assert(!strstr(state.output, "H2_ESP_LIBCO_SMOKE_READY"));
  }
  reset(); state.root_write_error = 1; run_entry();
  assert(!state.runs && !state.confirms && strstr(state.output, "FAIL stage=root_output "));
  reset(); state.root_flush_error = 1; run_entry();
  assert(!state.runs && !state.confirms && strstr(state.output, "FAIL stage=root_output "));
  reset(); state.confirm_result = H2_PAL_ERR_IO; run_entry();
  assert(state.confirms == 1u && !strstr(state.output, "H2_ESP_LIBCO_SMOKE_READY"));
  reset(); state.confirm_write_error = 1; run_entry();
  assert(state.confirms == 1u && strstr(state.output, "FAIL stage=confirm_output "));
  reset(); state.ready_write_error = 1; run_entry();
  assert(state.confirms == 1u && strstr(state.output, "FAIL stage=ready_output "));
  reset(); state.wrong_core = 1u; run_entry();
  assert(state.restarts == 1u && !state.initialized && !state.waited && !state.runs && !state.confirms);
  reset();
  puts("Libco actual entry startup, portable output/drain and confirmation gates passed");
  return 0;
}
