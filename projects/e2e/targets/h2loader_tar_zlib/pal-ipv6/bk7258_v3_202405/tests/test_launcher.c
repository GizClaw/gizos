/* Drive the actual native entry and runner. PAL vtables model saved-network
 * startup and address arrival; longjmp terminates hold/report loops. */
#include "h2_bk7258_board.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int test_printf(const char* format, ...);
#define printf test_printf
#define main native_launcher_main
#include "ap_main_for_test.inc"
#undef main
#undef printf

const char h2_pal_ipv6_device_task_name[] = "pal-ipv6/e2e/runner";
typedef enum scenario {
  SUCCESS,
  POLICY_ERROR,
  ENTRY_ERROR,
  BOARD_ERROR,
  RUNTIME_ERROR,
  COMMAND_ERROR,
  IMAGE_INFO_ERROR,
  RUNNER_ERROR,
  WIFI_ERROR,
  NO_SAVED_WIFI,
  CLOCK_ERROR,
  CLOCK_POLL_ERROR,
  ADDRESS_ERROR,
  ADDRESS_TIMEOUT,
  ADDRESS_TIMEOUT_WRAP,
  ADDRESS_DELAYED,
  NON_FIXTURE_ADDRESS,
  RUN_ERROR,
  INCOMPLETE_RESULT,
  FAILED_RESULT,
  BLOCKED_RESULT,
  EXCESS_RESULT,
  CONFIRM_ERROR,
} scenario_t;
static struct {
  scenario_t scenario;
  jmp_buf terminal;
  h2_runtime_t runtime;
  void (*entry)(void*);
  h2_pal_task_entry_t runner;
  unsigned policy, init, entries, boards, runtimes, commands, images, tasks;
  unsigned wifi, clocks, addresses, polls, psram_requests, runs, confirms;
  unsigned reports, ready, boots, links, failures, reboots, initial_waits;
  uint64_t now;
  int failure_rc, terminal_reason, ready_rc, ready_confirm;
  char failure_stage[32];
} f;
static const h2_pal_mem_api_t control_memory;
static h2_pal_mem_api_t psram_memory;
static const h2_pal_dtls_api_t dtls_api;
static const h2_pal_time_api_t time_api;
static const h2_pal_net_api_t net_api;
static const h2_pal_task_api_t task_api;
static const h2_pal_firmware_info_api_t firmware_api;
static const h2_pal_pref_api_t preferences;
static const h2_pal_wifi_sta_api_t wifi_api;

static int test_printf(const char* format, ...) {
  char line[1024];
  va_list args;
  va_start(args, format);
  int length = vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  assert(length > 0 && (size_t)length < sizeof(line));
  if (strstr(line, "H2_PAL_IPV6_SETUP_FAIL ") == line) {
    assert(sscanf(line, "H2_PAL_IPV6_SETUP_FAIL stage=%31s rc=%d",
                  f.failure_stage, &f.failure_rc) == 2);
    ++f.failures;
  }
  if (strstr(line, "H2_PAL_IPV6_READY ") == line) {
    const char* rc = strstr(line, " rc=");
    assert(rc &&
           sscanf(rc, " rc=%d confirm=%d", &f.ready_rc, &f.ready_confirm) == 2);
    ++f.ready;
  }
  if (strstr(line, "H2_PAL_IPV6_BOOT ") == line) {
    assert(strstr(line, "board=test-bk version=host-probe"));
    ++f.boots;
  }
  if (strstr(line, "H2_PAL_IPV6_LINK ") == line) ++f.links;
  return length;
}
static h2_pal_result_t clock_ms(void* user, uint64_t* out) {
  assert(user == &f && f.wifi == 1);
  ++f.clocks;
  if (f.scenario == CLOCK_ERROR ||
      (f.scenario == CLOCK_POLL_ERROR && f.clocks == 2))
    return H2_PAL_ERR_IO;
  *out = f.now;
  return H2_PAL_OK;
}
static const h2_pal_time_vtable_t time_vtable = {.get_monotonic_ms = clock_ms};
static const h2_pal_time_api_t time_api = {.user = &f, .vtable = &time_vtable};
static int get_address(void* user, const char* iface,
                       h2_pal_net_family_t family, h2_pal_net_addr_t* out) {
  assert(user == &f && !iface && family == H2_PAL_NET_FAMILY_IPV6 && out);
  assert(f.wifi == 1 && f.clocks);
  ++f.addresses;
  if (f.scenario == ADDRESS_ERROR) return H2_PAL_ERR_IO;
  if (f.scenario == ADDRESS_TIMEOUT || f.scenario == ADDRESS_TIMEOUT_WRAP ||
      f.scenario == CLOCK_POLL_ERROR)
    return H2_PAL_ERR_NOT_FOUND;
  if (f.scenario == ADDRESS_DELAYED && f.addresses <= 2)
    return f.addresses == 1 ? H2_PAL_ERR_NOT_FOUND : H2_PAL_ERR_UNAVAILABLE;
  memset(out, 0, sizeof(*out));
  out->family = H2_PAL_NET_FAMILY_IPV6;
  out->ip[0] = 0xfd;
  out->ip[1] = 0x53;
  if (f.scenario == NON_FIXTURE_ADDRESS ||
      (f.scenario == ADDRESS_DELAYED && f.addresses == 3)) {
    out->ip[0] = 0xfe;
    out->ip[1] = 0x80;
  }
  out->ip[15] = 1;
  return H2_PAL_OK;
}
static const h2_pal_net_vtable_t net_vtable = {.get_host_addr_family =
                                                   get_address};
static const h2_pal_net_api_t net_api = {.user = &f, .vtable = &net_vtable};
static h2_pal_result_t image_info(void* user, h2_pal_firmware_info_t* out) {
  assert(user == &f && f.commands == 1 && commands_ready && out);
  ++f.images;
  if (f.scenario == IMAGE_INFO_ERROR) return H2_PAL_ERR_IO;
  strcpy(out->version, "host-probe");
  return H2_PAL_OK;
}
static const h2_pal_firmware_info_vtable_t firmware_vtable = {.get_current =
                                                                  image_info};
static const h2_pal_firmware_info_api_t firmware_api = {
    .user = &f, .vtable = &firmware_vtable};
static h2_pal_result_t start_task(void* user,
                                  const h2_pal_task_options_t* options,
                                  h2_pal_task_entry_t callback, void* context,
                                  h2_pal_task_t** out) {
  assert(user == &f && options &&
         options->name == h2_pal_ipv6_device_task_name);
  assert(f.boots == 1 && f.images == 1 && commands_ready);
  assert(!context && callback && out && !f.wifi);
  ++f.tasks;
  if (f.scenario == RUNNER_ERROR) return H2_PAL_ERR_TASK;
  f.runner = callback;
  *out = (h2_pal_task_t*)&f.tasks;
  return H2_PAL_OK;
}
static const h2_pal_task_vtable_t task_vtable = {.start = start_task};
static const h2_pal_task_api_t task_api = {.user = &f, .vtable = &task_vtable};
h2_pal_result_t h2_bk7258_board_runtime_config(h2_runtime_config_t* config) {
  ++f.boards;
  if (f.scenario == BOARD_ERROR) return H2_PAL_ERR_IO;
  *config = (h2_runtime_config_t){.board = "test-bk",
                                  .mem = &control_memory,
                                  .pref = &preferences,
                                  .wifi_sta = &wifi_api,
                                  .time = &time_api,
                                  .net = &net_api,
                                  .task = &task_api,
                                  .firmware_info = &firmware_api};
  return H2_PAL_OK;
}
h2_pal_result_t h2_runtime_init(const h2_runtime_config_t* config,
                                h2_runtime_t** out) {
  assert(config->mem == &control_memory && config->pref == &preferences);
  ++f.runtimes;
  *out = NULL;
  if (f.scenario == RUNTIME_ERROR) return H2_PAL_ERR_IO;
  f.runtime = (h2_runtime_t){.board = config->board,
                             .mem = config->mem,
                             .pref = config->pref,
                             .wifi_sta = config->wifi_sta,
                             .time = config->time,
                             .net = config->net,
                             .task = config->task,
                             .firmware_info = config->firmware_info};
  *out = &f.runtime;
  return H2_PAL_OK;
}
int h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
    h2_runtime_t* value, const char* name, uint32_t capabilities) {
  assert(value == &f.runtime && value->mem == &control_memory && value->pref);
  assert(!strcmp(name, "pal-ipv6"));
  assert(capabilities ==
         (H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI));
  assert(!f.psram_requests && !f.wifi);
  ++f.commands;
  return f.scenario == COMMAND_ERROR ? H2_PAL_ERR_IO : H2_PAL_OK;
}
h2_pal_result_t h2_runtime_wifi_connect_saved(h2_runtime_t* value,
                                              uint32_t timeout_ms) {
  assert(value == &f.runtime && value->pref == &preferences &&
         value->wifi_sta == &wifi_api);
  assert(commands_ready && f.initial_waits == 1 && timeout_ms == 20000u);
  ++f.wifi;
  if (f.scenario == NO_SAVED_WIFI) return H2_PAL_ERR_NOT_FOUND;
  return f.scenario == WIFI_ERROR ? H2_PAL_ERR_IO : H2_PAL_OK;
}
h2_pal_mem_api_t* h2_bk_platform_psram_allocator(void) {
  assert(commands_ready && f.links == 1);
  ++f.psram_requests;
  return &psram_memory;
}
const h2_pal_dtls_api_t* h2_bk_platform_dtls_api(void) { return &dtls_api; }
int h2_pal_ipv6_device_board_fixture(void) {
  return f.scenario != NON_FIXTURE_ADDRESS;
}
const char* h2_pal_ipv6_device_host(void) { return "fd53::2"; }
int h2_pal_ipv6_device_run(h2_runtime_t* value, const h2_pal_dtls_api_t* dtls,
                           h2_pal_ipv6_result_t* out) {
  assert(value && value != &f.runtime && out == &result && dtls == &dtls_api);
  assert(value->mem == &psram_memory && f.runtime.mem == &control_memory);
  assert(value->board == f.runtime.board && value->net == f.runtime.net);
  assert(value->time == f.runtime.time && value->task == f.runtime.task);
  assert(value->pref == f.runtime.pref &&
         value->wifi_sta == f.runtime.wifi_sta);
  assert(value->firmware_info == f.runtime.firmware_info && !f.confirms);
  assert(f.psram_requests == 1 && !out->passed && !out->failed &&
         !out->blocked);
  ++f.runs;
  out->passed = H2_PAL_IPV6_CASES;
  if (f.scenario == INCOMPLETE_RESULT) --out->passed;
  if (f.scenario == FAILED_RESULT) {
    --out->passed;
    out->failed = 1;
  }
  if (f.scenario == BLOCKED_RESULT) {
    --out->passed;
    out->blocked = 1;
  }
  if (f.scenario == EXCESS_RESULT) ++out->passed;
  return f.scenario == RUN_ERROR ? H2_PAL_ERR_IO : H2_PAL_OK;
}
int h2_bk_h2loader_confirm_current_app(h2_runtime_t* value) {
  assert(value == &f.runtime && value->mem == &control_memory &&
         commands_ready);
  assert(f.runs == 1 && result.passed == H2_PAL_IPV6_CASES);
  assert(!result.failed && !result.blocked && f.scenario != RUN_ERROR);
  ++f.confirms;
  return f.scenario == CONFIRM_ERROR ? H2_PAL_ERR_IO : H2_PAL_OK;
}
void h2_pal_ipv6_device_report(const h2_runtime_t* value,
                               const h2_pal_ipv6_result_t* out) {
  assert(value == &f.runtime && value->mem == &control_memory &&
         out == &result);
  assert(f.runs == 1 && !f.failures);
  ++f.reports;
}
static h2_pal_result_t reboot(void* user, uint32_t reason) {
  assert(user == &f && reason == 0 && !commands_ready && f.failures == 1);
  ++f.reboots;
  return H2_PAL_ERR_IO; /* Returning reboot cannot resume failed startup. */
}
static const h2_pal_power_vtable_t power_vtable = {.reboot = reboot};
static const h2_pal_power_api_t power_api = {.user = &f,
                                             .vtable = &power_vtable};
const h2_pal_power_api_t* h2_bk_h2loader_power_api(void) { return &power_api; }
void rtos_delay_milliseconds(uint32_t ms) {
  if (ms == 1000u || (ms == 5000u && f.reports == 2)) {
    f.terminal_reason = ms == 1000u ? 1 : 2;
    longjmp(f.terminal, 1);
  }
  if (ms == 5000u) {
    if (!f.reports) ++f.initial_waits;
  } else if (ms == 100u) {
    assert(++f.polls <= 200);
  } else {
    assert(ms == 200u && !commands_ready && f.failures);
  }
  f.now += ms;
}
int h2_bk_target_task_policy_install(void) {
  ++f.policy;
  return f.scenario == POLICY_ERROR ? H2_PAL_ERR_IO : H2_PAL_OK;
}
void bk_init(void) {
  assert(f.policy == 1);
  ++f.init;
}
h2_pal_result_t h2_bk7258_board_start_entry_task(const char* name,
                                                 void (*callback)(void*),
                                                 void* user) {
  assert(f.init == 1 && !strcmp(name, "bk/pal-ipv6") && callback && !user);
  ++f.entries;
  if (f.scenario == ENTRY_ERROR) return H2_PAL_ERR_TASK;
  f.entry = callback;
  return H2_PAL_OK;
}
static void reset(scenario_t scenario) {
  memset(&f, 0, sizeof(f));
  f.scenario = scenario;
  if (scenario == ADDRESS_TIMEOUT_WRAP) f.now = UINT64_MAX - 10000u;
  memset(&result, 0, sizeof(result));
  runtime = NULL;
  commands_ready = 0;
}
static void drive(void (*callback)(void*), void* user) {
  f.terminal_reason = 0;
  if (!setjmp(f.terminal)) {
    callback(user);
    assert(0 && "launcher returned before a terminal boundary");
  }
  assert(f.terminal_reason);
}
static void boot(void* user) {
  (void)user;
  (void)native_launcher_main();
}
static void test_startup(void) {
  reset(POLICY_ERROR);
  assert(native_launcher_main() == -1 && f.policy == 1 && !f.init &&
         !f.entries);
  reset(ENTRY_ERROR);
  drive(boot, NULL);
  assert(f.reboots == 1 && !f.runner && !strcmp(f.failure_stage, "entry"));
}
static void test_launcher(scenario_t scenario, const char* failure, int error) {
  reset(scenario);
  assert(native_launcher_main() == 0 && f.entry && f.entries == 1);
  drive(f.entry, NULL);
  if (f.runner) {
    assert(f.boots == 1 && commands_ready && !f.wifi);
    drive(f.runner, NULL);
  }
  if (failure) {
    assert(f.failures == 1 && !strcmp(f.failure_stage, failure));
    assert(f.failure_rc == error && f.terminal_reason == 1);
    assert(!f.runs && !f.confirms && !f.reports && !f.ready &&
           !f.psram_requests);
    assert(f.reboots == (scenario <= COMMAND_ERROR ? 1u : 0u));
    if (scenario == ADDRESS_TIMEOUT || scenario == ADDRESS_TIMEOUT_WRAP)
      assert(f.polls == 200 && f.addresses == 201);
    return;
  }
  assert(!f.failures && !f.reboots && f.terminal_reason == 2);
  assert(f.runs == 1 && f.reports == 2 && f.ready == 2 && f.links == 1);
  assert(f.runtime.mem == &control_memory && f.wifi == 1);
  unsigned qualifies = scenario == SUCCESS || scenario == ADDRESS_DELAYED ||
                       scenario == NON_FIXTURE_ADDRESS ||
                       scenario == CONFIRM_ERROR;
  assert(f.confirms == qualifies);
  assert(f.ready_rc == (scenario == RUN_ERROR ? H2_PAL_ERR_IO : H2_PAL_OK));
  assert(f.ready_confirm == (!qualifies ? H2_PAL_ERR_INVALID_STATE
                             : scenario == CONFIRM_ERROR ? H2_PAL_ERR_IO
                                                         : H2_PAL_OK));
  if (scenario == ADDRESS_DELAYED) assert(f.polls == 3 && f.addresses == 4);
}
int main(void) {
  test_startup();
  test_launcher(BOARD_ERROR, "board", H2_PAL_ERR_IO);
  test_launcher(RUNTIME_ERROR, "runtime", H2_PAL_ERR_IO);
  test_launcher(COMMAND_ERROR, "commands", H2_PAL_ERR_IO);
  test_launcher(IMAGE_INFO_ERROR, "firmware_info", H2_PAL_ERR_IO);
  test_launcher(RUNNER_ERROR, "runner", H2_PAL_ERR_TASK);
  test_launcher(WIFI_ERROR, "wifi", H2_PAL_ERR_IO);
  test_launcher(NO_SAVED_WIFI, "wifi", H2_PAL_ERR_NOT_FOUND);
  test_launcher(CLOCK_ERROR, "clock", H2_PAL_ERR_IO);
  test_launcher(CLOCK_POLL_ERROR, "clock", H2_PAL_ERR_IO);
  test_launcher(ADDRESS_ERROR, "ipv6_address", H2_PAL_ERR_IO);
  test_launcher(ADDRESS_TIMEOUT, "ipv6_address", H2_PAL_ERR_UNAVAILABLE);
  test_launcher(ADDRESS_TIMEOUT_WRAP, "ipv6_address", H2_PAL_ERR_UNAVAILABLE);
  const scenario_t results[] = {
      SUCCESS,        ADDRESS_DELAYED,   NON_FIXTURE_ADDRESS,
      RUN_ERROR,      INCOMPLETE_RESULT, FAILED_RESULT,
      BLOCKED_RESULT, EXCESS_RESULT,     CONFIRM_ERROR};
  for (unsigned i = 0; i < sizeof(results) / sizeof(results[0]); ++i)
    test_launcher(results[i], NULL, H2_PAL_OK);
  puts("BK IPv6 launcher regression PASS");
  return 0;
}
