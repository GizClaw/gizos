/* Execute the complete native launcher with controlled SDK/PAL outcomes.
 * The portable network tests separately exercise the real TCP/UDP matrix. */
#include "test_sdk.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int captured_printf(const char* format, ...);
static int captured_puts(const char* line);
static int captured_fflush(FILE* stream);
#define printf captured_printf
#define puts captured_puts
#define fflush captured_fflush
#define main native_launcher_main
#include "ap_main_for_test.inc"
#undef main
#undef printf
#undef puts
#undef fflush

const char h2_iperf_client_app_task_name[] = "iperf-client/run";
struct h2_pal_task {
  unsigned live;
};
typedef enum scenario {
  SUCCESS,
  CONFIRM_LOG_ERROR,
  MALFORMED_RECORD,
  STOP_ERROR,
  CONFIG_ERROR,
  WRITE_ERROR,
  PARTIAL_WRITE,
  DRAIN_TIMEOUT,
  RESTART_ERROR,
  BENCH_ERROR,
  CONFIRM_ERROR,
  CONFIRMED_LOG_ERROR,
  RUNTIME_ERROR,
  BOARD_ERROR,
} scenario_t;
static struct {
  scenario_t scenario;
  jmp_buf park;
  h2_runtime_t runtime;
  h2_pal_task_entry_t runner;
  void* runner_user;
  uint32_t now;
  unsigned starts, stops, raw_configures, management_configures, deinits;
  unsigned bench_calls, confirms, confirmed_records, boot_records, writes;
  unsigned board_deinits, delays, parked, failure_seen, image_accepted;
  int failure_rc, board_live, runtime_live, manager_live, uart_live;
  int fault_sent, management_config;
} f;
static struct h2_pal_task runner_task;
static h2_pal_mem_api_t normal_mem, psram_mem;
static h2_pal_pref_api_t prefs;
static const h2_pal_uart_io_stream_api_t uart;

static void capture(const char* line) {
  if (strstr(line, "H2_IPERF_CLIENT_BOOT ") == line) ++f.boot_records;
  if (strstr(line, "H2_IPERF_CLIENT_FAIL ") == line) {
    const char* rc = strstr(line, " rc=");
    assert(rc && sscanf(rc, " rc=%d", &f.failure_rc) == 1);
    ++f.failure_seen;
  }
  if (strstr(line, "H2_IPERF_CLIENT_CONFIRMED ") == line) {
    int result;
    assert(sscanf(strstr(line, " rc="), " rc=%d", &result) == 1);
    f.confirmed_records += result == H2_PAL_OK;
  }
}
static int captured_printf(const char* format, ...) {
  char line[1025];
  va_list args;
  va_start(args, format);
  int n = vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  assert(n > 0 && (size_t)n < sizeof(line));
  capture(line);
  return n;
}
static int captured_puts(const char* line) {
  capture(line);
  return 0;
}
static int captured_fflush(FILE* stream) {
  assert(stream == stdout);
  return 0;
}
static int board_write(void* user, h2_pal_log_level_t level, const char* scope,
                       const char* message) {
  (void)user;
  (void)level;
  assert(scope && message);
  return H2_PAL_OK;
}
static const h2_pal_log_vtable_t board_vtable = {.write = board_write};
static const h2_pal_log_api_t board_api = {.vtable = &board_vtable};
static int configure(void* user, const h2_pal_uart_io_stream_config_t* config) {
  (void)user;
  assert(config && config->baud_rate == CONFIG_UART_PRINT_BAUD_RATE);
  if (f.management_config) {
    ++f.management_configures;
  } else {
    assert(!f.manager_live && !f.uart_live);
    assert(config->data_bits == 8 && config->stop_bits == 1);
    assert(config->rx_buffer_size == 8192 && config->tx_buffer_size == 2048);
    ++f.raw_configures;
    if (f.scenario == CONFIG_ERROR) return H2_PAL_ERR_IO;
  }
  f.uart_live = 1;
  return H2_PAL_OK;
}
static int write_uart(void* user, const void* bytes, size_t size,
                      size_t* written, uint32_t timeout) {
  (void)user;
  assert(f.uart_live && bytes && written && size >= 2 && size <= 1024);
  assert(timeout == 5000 &&
         h2_atomic_bool_load(&console_writable, H2_ATOMIC_ACQUIRE));
  const char* data = bytes;
  assert(data[size - 2] == '\r' && data[size - 1] == '\n');
  char line[1025];
  memcpy(line, bytes, size - 2);
  line[size - 2] = '\0';
  assert(!strchr(line, '\r') && !strchr(line, '\n'));
  ++f.writes;
  int fault = !f.fault_sent &&
              ((f.scenario == WRITE_ERROR || f.scenario == PARTIAL_WRITE) ||
               (f.scenario == CONFIRM_LOG_ERROR &&
                strstr(line, "phase=confirm-begin")) ||
               (f.scenario == CONFIRMED_LOG_ERROR &&
                strstr(line, "H2_IPERF_CLIENT_CONFIRMED ") == line));
  if (fault) {
    f.fault_sent = 1;
    *written = f.scenario == PARTIAL_WRITE ? size - 1 : 0;
    return f.scenario == PARTIAL_WRITE ? H2_PAL_OK : H2_PAL_ERR_IO;
  }
  *written = size;
  capture(line);
  return H2_PAL_OK;
}
static const h2_pal_uart_io_stream_vtable_t uart_vtable = {
    .configure = configure, .write = write_uart};
static const h2_pal_uart_io_stream_api_t uart = {.vtable = &uart_vtable};
static int start_task(void* user, const h2_pal_task_options_t* options,
                      h2_pal_task_entry_t entry_fn, void* context,
                      h2_pal_task_t** out) {
  (void)user;
  assert(options->min_stack_size == 32768 &&
         !strcmp(options->name, "iperf-client/run"));
  assert(f.runtime_live && f.manager_live);
  f.runner = entry_fn;
  f.runner_user = context;
  *out = &runner_task;
  return H2_PAL_OK;
}
static const h2_pal_task_vtable_t task_vtable = {.start = start_task};
static const h2_pal_task_api_t task_api = {.vtable = &task_vtable};
static int image_info(void* user, h2_pal_firmware_info_t* out) {
  (void)user;
  strcpy(out->version, "host-launcher-probe");
  return H2_PAL_OK;
}
static const h2_pal_firmware_info_vtable_t image_vtable = {.get_current =
                                                               image_info};
static const h2_pal_firmware_info_api_t image_api = {.vtable = &image_vtable};
h2_pal_result_t h2_bk7258_board_runtime_config(h2_runtime_config_t* config) {
  if (f.scenario == BOARD_ERROR) return H2_PAL_ERR_IO;
  assert(!f.board_live);
  f.board_live = 1;
  *config = (h2_runtime_config_t){.mem = &normal_mem,
                                  .log = &board_api,
                                  .task = &task_api,
                                  .firmware_info = &image_api,
                                  .pref = &prefs};
  return H2_PAL_OK;
}
h2_pal_result_t h2_bk7258_board_runtime_deinit(void) {
  assert(f.board_live && !f.runtime_live);
  ++f.board_deinits;
  f.board_live = 0;
  return H2_PAL_OK;
}
h2_pal_result_t h2_runtime_init(const h2_runtime_config_t* config,
                                h2_runtime_t** out) {
  assert(config->mem == &psram_mem && config->log == &ledger_api);
  *out = NULL;
  if (f.scenario == RUNTIME_ERROR) return H2_PAL_ERR_IO;
  f.runtime = (h2_runtime_t){.mem = config->mem,
                             .log = config->log,
                             .task = config->task,
                             .firmware_info = config->firmware_info,
                             .pref = config->pref};
  *out = &f.runtime;
  f.runtime_live = 1;
  return H2_PAL_OK;
}
h2_pal_mem_api_t* h2_bk_platform_psram_allocator(void) { return &psram_mem; }
const h2_pal_uart_io_stream_api_t* h2_bk_platform_uart_io_stream_api(void) {
  return &uart;
}
void h2_bk_platform_uart_io_stream_deinit(void) {
  assert(!f.manager_live &&
         !h2_atomic_bool_load(&console_writable, H2_ATOMIC_ACQUIRE));
  ++f.deinits;
  f.uart_live = 0;
}
int h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
    h2_runtime_t* rt, const char* name, uint32_t capabilities) {
  assert(rt == &f.runtime && rt->pref && !strcmp(name, "iperf-client"));
  assert(capabilities ==
         (H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI));
  assert(!f.manager_live);
  ++f.starts;
  if (f.starts == 2) {
    assert(!f.uart_live && f.deinits == 1);
    assert(!h2_atomic_bool_load(&console_writable, H2_ATOMIC_ACQUIRE));
    if (f.scenario == RESTART_ERROR) return H2_PAL_ERR_IO;
  }
  /* Model the inspected management contract: restart synchronously configures
   * the same static UART API before it can report success. */
  const h2_pal_uart_io_stream_config_t config = {
      .baud_rate = CONFIG_UART_PRINT_BAUD_RATE};
  f.management_config = 1;
  int rc = h2_pal_uart_io_stream_configure(&uart, &config);
  f.management_config = 0;
  assert(rc == H2_PAL_OK);
  f.manager_live = 1;
  return H2_PAL_OK;
}
int h2_bk_h2loader_stop_app_iostreamikcp(void) {
  assert(f.manager_live && f.uart_live);
  ++f.stops;
  if (f.scenario == STOP_ERROR) return H2_PAL_ERR_TIMEOUT;
  f.manager_live = f.uart_live = 0;
  return H2_PAL_OK;
}
int h2_bk_h2loader_confirm_current_app(h2_runtime_t* rt) {
  assert(rt == &f.runtime && f.manager_live && f.uart_live);
  assert(f.bench_calls == 1 && f.management_configures == 2);
  ++f.confirms;
  if (f.scenario == CONFIRM_ERROR) return H2_PAL_ERR_IO;
  f.image_accepted = 1;
  return H2_PAL_OK;
}
h2_pal_result_t h2_iperf_client_app_bench(h2_runtime_t* rt, const char* target,
                                          h2_iperf_e2e_checkpoint_fn checkpoint,
                                          void* user) {
  assert(rt == &f.runtime && !strcmp(target, "bk7258") && checkpoint);
  assert(!f.manager_live && f.raw_configures == 1 && f.uart_live);
  ++f.bench_calls;
  const char* message = f.scenario == MALFORMED_RECORD
                            ? "bad\nledger"
                            : "H2_IPERF_CLIENT_COMPLETE board=bk7258 rc=0";
  (void)h2_pal_log_write(rt->log, H2_PAL_LOG_INFO, "iperf-client", message);
  checkpoint(user, "probe");
  return f.scenario == BENCH_ERROR ? H2_PAL_ERR_IO : H2_PAL_OK;
}
void rtos_delay_milliseconds(uint32_t milliseconds) {
  if (milliseconds == 1000) {
    f.parked = 1;
    longjmp(f.park, 1);
  }
  if (milliseconds == 1) assert(++f.delays <= 5000);
  f.now += milliseconds;
}
uint32_t rtos_get_time(void) { return f.now; }
int bk_uart_is_tx_over(uint32_t port) {
  assert(port == CONFIG_UART_PRINT_PORT);
  return f.scenario != DRAIN_TIMEOUT;
}
size_t rtos_get_free_heap_size(void) { return 10000; }
size_t rtos_get_minimum_free_heap_size(void) { return 9000; }
size_t rtos_get_psram_free_heap_size(void) { return 100000; }
h2_pal_result_t h2_bk_platform_get_resource_stats(
    h2_bk_platform_resource_stats_t* out) {
  memset(out, 0, sizeof(*out));
  return H2_PAL_OK;
}
void bk_init(void) {}
int h2_bk_target_task_policy_install(void) { return H2_PAL_OK; }
h2_pal_result_t h2_bk7258_board_start_entry_task(const char* name,
                                                 void (*callback)(void*),
                                                 void* user) {
  assert(!strcmp(name, "bk/iperf-client") && callback && !user);
  return H2_PAL_OK;
}
static void drive(h2_pal_task_entry_t callback, void* user) {
  f.parked = 0;
  if (!setjmp(f.park)) callback(user);
  assert(f.parked);
}
static void reset(scenario_t scenario) {
  memset(&f, 0, sizeof(f));
  f.scenario = scenario;
  f.now = scenario == DRAIN_TIMEOUT ? UINT32_MAX - 6000u : 0;
  runtime = NULL;
  board_log = NULL;
  console = NULL;
  console_error = H2_PAL_OK;
  h2_atomic_bool_store(&console_writable, false, H2_ATOMIC_RELEASE);
}
static void test(scenario_t scenario) {
  reset(scenario);
  drive(entry, NULL);
  if (scenario == RUNTIME_ERROR || scenario == BOARD_ERROR) {
    assert(!f.runner && !runtime && !f.confirms && !f.starts && f.failure_seen);
    assert(!f.board_live &&
           f.board_deinits == (scenario == RUNTIME_ERROR ? 1u : 0u));
    return;
  }
  assert(f.runner && f.manager_live && f.runtime_live && f.boot_records == 1);
  drive(f.runner, f.runner_user);
  const unsigned attempts = scenario == SUCCESS || scenario == CONFIRM_ERROR ||
                            scenario == CONFIRMED_LOG_ERROR;
  assert(f.confirms == attempts);
  assert(f.image_accepted ==
         (scenario == SUCCESS || scenario == CONFIRMED_LOG_ERROR));
  assert(f.confirmed_records == (scenario == SUCCESS));
  assert(f.failure_seen == (scenario != SUCCESS));
  if (scenario != SUCCESS) {
    const int expected = scenario == STOP_ERROR || scenario == DRAIN_TIMEOUT
                             ? H2_PAL_ERR_TIMEOUT
                         : scenario == MALFORMED_RECORD ? H2_PAL_ERR_INVALID_ARG
                                                        : H2_PAL_ERR_IO;
    assert(f.failure_rc == expected);
  }
  if (scenario == STOP_ERROR) {
    assert(f.manager_live && f.uart_live && f.starts == 1 &&
           !f.raw_configures && !f.deinits && !f.bench_calls);
  } else {
    assert(f.starts == 2 && f.stops == 1 && f.raw_configures == 1 &&
           f.deinits == 1);
    assert(f.bench_calls == (scenario != CONFIG_ERROR));
    assert(f.manager_live == (scenario != RESTART_ERROR));
  }
  if (scenario == DRAIN_TIMEOUT) assert(f.delays == 5000);
}
static void test_records(void) {
  reset(SUCCESS);
  assert(record("closed") == H2_PAL_ERR_INVALID_STATE);
  assert(console_error == H2_PAL_ERR_INVALID_STATE && !f.writes);
  const char* bad[] = {NULL, "", "bad\rrow", "bad\nrow"};
  for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    reset(SUCCESS);
    console = &uart;
    f.uart_live = 1;
    h2_atomic_bool_store(&console_writable, true, H2_ATOMIC_RELEASE);
    assert(record(bad[i]) == H2_PAL_ERR_INVALID_ARG);
    assert(console_error == H2_PAL_ERR_INVALID_ARG && !f.writes);
  }
  char line[1024];
  memset(line, 'x', sizeof(line));
  line[1023] = '\0';
  reset(SUCCESS);
  console = &uart;
  f.uart_live = 1;
  h2_atomic_bool_store(&console_writable, true, H2_ATOMIC_RELEASE);
  assert(record(line) == H2_PAL_ERR_INVALID_ARG &&
         console_error == H2_PAL_ERR_INVALID_ARG);
  line[1022] = '\0';
  assert(record(line) == H2_PAL_OK && f.writes == 1);
  assert(console_error == H2_PAL_ERR_INVALID_ARG);
}
int main(int argc, char** argv) {
  const struct {
    const char* name;
    scenario_t scenario;
  } cases[] = {{"success", SUCCESS},
               {"confirm-log", CONFIRM_LOG_ERROR},
               {"malformed", MALFORMED_RECORD},
               {"stop", STOP_ERROR},
               {"configure", CONFIG_ERROR},
               {"write", WRITE_ERROR},
               {"partial", PARTIAL_WRITE},
               {"drain", DRAIN_TIMEOUT},
               {"restart", RESTART_ERROR},
               {"bench", BENCH_ERROR},
               {"confirm", CONFIRM_ERROR},
               {"confirmed-log", CONFIRMED_LOG_ERROR},
               {"runtime-init", RUNTIME_ERROR},
               {"board", BOARD_ERROR}};
  unsigned executed = 0;
  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    if (argc == 1 || !strcmp(argv[1], cases[i].name)) {
      test(cases[i].scenario);
      ++executed;
    }
  assert(executed);
  if (argc == 1) test_records();
  puts("BK launcher UART/confirmation lifecycle PASS");
  return 0;
}
