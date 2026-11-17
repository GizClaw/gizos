/* Both board tests compile their actual launcher into this fixture. Public
 * dependencies model persistent App selection, console failure and cold time;
 * longjmp terminates only the launcher's permanent task/reboot loops. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_printf(const char* format, ...);
#define printf test_printf
#define main native_launcher_main
#define H2_LAUNCHER_STRINGIFY_IMPL(value) #value
#define H2_LAUNCHER_STRINGIFY(value) H2_LAUNCHER_STRINGIFY_IMPL(value)
#include H2_LAUNCHER_STRINGIFY(H2_GIZCLAW_LAUNCHER_SOURCE)
#undef main
#undef printf

const unsigned char h2_gizclaw_e2e_pcm[] = {0u, 0u};
const size_t h2_gizclaw_e2e_pcm_size = sizeof(h2_gizclaw_e2e_pcm);
const char h2_gizclaw_e2e_launcher_task_name[] =
    H2_GIZCLAW_E2E_LAUNCHER_TASK_NAME_VALUE;

static jmp_buf terminal;
static struct {
  h2_runtime_t runtime;
  int ntp_rc, wall_rc, resolve_rc, select_rc, reboot_rc;
  unsigned boards, deinit, prepares, serial_starts, admits;
  unsigned creates, runs, confirms, publishes, replays;
  unsigned resolves, syncs, wall_reads, wall_sets, selects, reboots, waits;
  unsigned locked, unlocks, failure_count;
  uint32_t next_partition;
  int failure_rc;
  char failure_stage[64];
  bool calibrated, remain_uncalibrated, confirmed, provider_live;
  bool setup_failure, publish_failure, pre_runtime_failure;
  bool selection_retry, prepare_retry, reboot_returns;
} f;

static int test_printf(const char* format, ...) {
  char line[256];
  va_list args;
  va_start(args, format);
  const int size = vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  assert(size > 0 && (size_t)size < sizeof(line));
  if (strstr(line, "H2_GIZCLAW_SETUP_FAIL ") == line) {
    ++f.failure_count;
    assert(sscanf(line, "H2_GIZCLAW_SETUP_FAIL stage=%63s rc=%d",
                  f.failure_stage, &f.failure_rc) == 2);
  }
  return size;
}

static h2_pal_result_t wall_status(void* user,
                                   h2_pal_time_wall_status_t* status) {
  assert(user == &f);
  if (f.wall_rc != H2_PAL_OK) return f.wall_rc;
  *status = (h2_pal_time_wall_status_t){
      .valid = f.calibrated,
      .source = f.calibrated ? H2_PAL_TIME_WALL_SOURCE_RTC
                             : H2_PAL_TIME_WALL_SOURCE_BOOT_DEFAULT};
  return H2_PAL_OK;
}
static h2_pal_result_t wall_read(void* user, uint64_t* wall_ms) {
  assert(user == &f && f.calibrated);
  ++f.wall_reads;
  *wall_ms = UINT64_C(1794398400000);
  return H2_PAL_OK;
}
static h2_pal_result_t wall_set(void* user, uint64_t wall_ms) {
  assert(user == &f && wall_ms == UINT64_C(1794398400000));
  ++f.wall_sets;
  f.calibrated = !f.remain_uncalibrated;
  return H2_PAL_OK;
}
static const h2_pal_time_vtable_t time_vtable = {
    .get_wall_ms = wall_read,
    .set_wall_ms = wall_set,
    .get_wall_status = wall_status};
static const h2_pal_time_api_t time_api = {.user = &f, .vtable = &time_vtable};
static int resolve(void* user, const char* host, h2_pal_net_addr_t* out) {
  assert(user == &f && !strcmp(host, "time.fixture.invalid"));
  assert(!f.creates && !f.calibrated);
  ++f.resolves;
  if (f.resolve_rc != H2_PAL_OK) return f.resolve_rc;
  *out = (h2_pal_net_addr_t){.family = H2_PAL_NET_FAMILY_IPV4,
                             .ip = {192u, 0u, 2u, 1u}};
  return H2_PAL_OK;
}
static const h2_pal_net_vtable_t net_vtable = {.resolve_addr = resolve};
static const h2_pal_net_api_t net_api = {.user = &f, .vtable = &net_vtable};
int h2_ntp_sync(const h2_ntp_client_config_t* config,
                h2_ntp_sync_result_t* result) {
  assert(config->net == &net_api && config->time == &time_api);
  assert(config->server.family == H2_PAL_NET_FAMILY_IPV4 &&
         config->server.port == H2_NTP_PORT && config->server.ip[0] == 192u);
  assert(!config->timeout_ms && !config->retries && config->set_wall_clock);
  assert(!config->bind && !f.creates && !f.calibrated && f.resolves == 1u);
  ++f.syncs;
  if (f.ntp_rc != H2_NTP_OK) return f.ntp_rc;
  assert(h2_pal_time_set_wall_ms(config->time, UINT64_C(1794398400000)) ==
         H2_PAL_OK);
  result->wall_clock_set = 1u;
  return H2_NTP_OK;
}
static int wifi_status(void* user, h2_pal_wifi_sta_status_t* status) {
  assert(user == &f && h2_management.serial_started);
  *status = (h2_pal_wifi_sta_status_t){.state = H2_PAL_WIFI_STA_STATE_GOT_IP,
                                       .ip = {.ip4 = 0xc0000202u},
                                       .ip_valid = 1u};
  return H2_PAL_OK;
}
static const h2_pal_wifi_sta_vtable_t wifi_vtable = {.get_status = wifi_status};
static h2_pal_wifi_sta_api_t wifi_api = {.user = &f, .vtable = &wifi_vtable};
static void* allocate(void* user, size_t size) {
  assert(user == &f);
  return malloc(size);
}
static void release(void* user, void* allocation) {
  assert(user == &f);
  free(allocation);
}
static const h2_pal_mem_vtable_t mem_vtable = {.alloc = allocate,
                                               .free = release};
static h2_pal_mem_api_t mem_api = {.user = &f, .vtable = &mem_vtable};
static const h2_pal_log_api_t console_api = {0};
static const h2_pal_http_api_t http_api = {0};

static int select_loader(void* user, uint32_t partition_id) {
  assert(user == &f && f.locked && partition_id == 1u && f.admits);
  assert(f.provider_live || f.pre_runtime_failure);
  ++f.selects;
  if (f.selection_retry && f.selects == 1u) return H2_PAL_ERR_IO;
  if (f.select_rc != H2_PAL_OK) return f.select_rc;
  f.next_partition = partition_id;
  return H2_PAL_OK;
}
static int reboot_loader(void* user, uint32_t reason) {
  assert(user == &f && f.locked && reason == 0u);
  /* The model starts with next=confirmed App. Every reboot must first replace
   * that durable selection, including after a successful new confirmation. */
  assert(f.next_partition == 1u && f.selects);
  ++f.reboots;
  if (!f.reboot_returns) longjmp(terminal, 2);
  return f.reboot_rc;
}
static const h2_pal_power_vtable_t power_vtable = {
    .set_next_boot_partition = select_loader, .reboot = reboot_loader};
static const h2_pal_power_api_t power_api = {.user = &f,
                                             .vtable = &power_vtable};
const h2_pal_power_api_t* h2_device_app_commands_power(
    const h2_device_app_commands_t* state) {
  assert(state == &h2_management && state->prepared && f.locked);
  return &power_api;
}
int h2_device_app_operation_lock(h2_device_app_commands_t* state) {
  assert(state == &h2_management && state->prepared && !f.locked);
  f.locked = 1u;
  return H2_PAL_OK;
}
int h2_device_app_operation_unlock(h2_device_app_commands_t* state) {
  assert(state == &h2_management && f.locked);
  f.locked = 0u;
  ++f.unlocks;
  return H2_PAL_OK;
}
void h2_device_app_commands_admit_sessions(h2_device_app_commands_t* state) {
  assert(state == &h2_management && state->serial_started);
  ++f.admits;
}
int h2_device_app_commands_prepare(
    h2_device_app_commands_t* state, const h2_runtime_config_t* config,
    const h2_device_app_commands_config_t* commands) {
  assert(state == &h2_management && !runtime && f.pre_runtime_failure);
  assert(config->power == &power_api && config->time == &time_api);
  assert(commands->h2loader_partition_id == 1u);
  assert(commands->boot_policy_flags & H2_DEVICE_BOOT_PREPARE_SELECTION);
  ++f.prepares;
  if (f.prepare_retry && f.prepares == 1u) return H2_PAL_ERR_IO;
  state->prepared = 1;
  return H2_PAL_OK;
}
int h2_device_app_commands_prepare_runtime(
    h2_device_app_commands_t* state, h2_runtime_t* value,
    const h2_device_app_commands_config_t* config) {
  assert(state == &h2_management && value == &f.runtime && config);
  state->prepared = 1;
  return H2_PAL_OK;
}
int h2_device_app_commands_start_serial(h2_device_app_commands_t* state,
                                        const h2_iostreamikcp_io_t* io) {
  assert(state == &h2_management && state->prepared && !io);
  ++f.serial_starts;
  state->serial_started = 1;
  return H2_PAL_OK;
}
int h2_device_app_commands_start_serial_named(h2_device_app_commands_t* state,
                                              h2_runtime_t* value,
                                              const char* name,
                                              uint32_t capabilities) {
  assert(state == &h2_management && value == &f.runtime);
  assert(!strcmp(name, "gizclaw-e2e") && f.provider_live);
  assert(capabilities ==
         (H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI));
  state->prepared = 1;
  return h2_device_app_commands_start_serial(state, NULL);
}
int h2_device_app_confirm(h2_device_app_commands_t* state,
                          h2_runtime_t* value) {
  assert(state == &h2_management && value == &f.runtime && f.runs == 1u);
  ++f.confirms;
  f.confirmed = true;
  return H2_PAL_OK;
}

int h2_gizclaw_e2e_execution_create(
    h2_runtime_t* value, const h2_gizclaw_e2e_config_t* app,
    const h2_gizclaw_e2e_execution_config_t* evidence,
    h2_gizclaw_e2e_execution_t** out) {
  assert(value == &f.runtime && evidence->output == &console_api);
  assert(f.calibrated && f.wall_reads && !f.runs && f.provider_live);
  assert(app->server_endpoint.len == strlen("https://fixture.invalid"));
  assert(!memcmp(app->server_endpoint.data, "https://fixture.invalid",
                 app->server_endpoint.len));
  ++f.creates;
  if (f.setup_failure) return H2_PAL_ERR_NO_MEMORY;
  *out = (h2_gizclaw_e2e_execution_t*)&f.creates;
  return H2_PAL_OK;
}
h2_gizclaw_e2e_exit_t h2_gizclaw_e2e_execution_run(
    h2_gizclaw_e2e_execution_t* value) {
  assert(value == execution && f.creates == 1u && f.calibrated);
  ++f.runs;
  return H2_GIZCLAW_E2E_EXIT_PASS;
}
bool h2_gizclaw_e2e_execution_qualified(
    const h2_gizclaw_e2e_execution_t* value) {
  assert(value == execution && f.runs == 1u);
  return true;
}
int h2_gizclaw_e2e_execution_publish(h2_gizclaw_e2e_execution_t* value,
                                     int confirm_rc) {
  assert(value == execution && confirm_rc == H2_PAL_OK && f.confirmed);
  ++f.publishes;
  return f.publish_failure ? H2_PAL_ERR_BUSY : H2_PAL_OK;
}
int h2_gizclaw_e2e_execution_replay(h2_gizclaw_e2e_execution_t* value) {
  assert(value == execution && f.publishes == 1u);
  ++f.replays;
  return H2_PAL_OK;
}
const h2_pal_log_api_t* h2_bk_platform_console_log_api(void) {
  return &console_api;
}
h2_pal_result_t h2_corehttp_create(const h2_corehttp_config_t* config,
                                   h2_corehttp_t** out,
                                   h2_pal_http_api_t* api) {
  assert(config->net == &net_api && config->time == &time_api);
  assert(config->tls_verify == H2_PAL_NET_TLS_VERIFY_REQUIRED);
  assert(config->root_ca_pem_len == 2u &&
         !memcmp(config->root_ca_pem, "AB", 2u));
  if (f.pre_runtime_failure) return H2_PAL_ERR_IO;
  f.provider_live = true;
  *out = (h2_corehttp_t*)&f.provider_live;
  *api = http_api;
  return H2_PAL_OK;
}
void h2_corehttp_destroy(h2_corehttp_t* http) {
  assert(http == fixture_http);
  f.provider_live = false;
}
static h2_pal_result_t start_task(void* user,
                                  const h2_pal_task_options_t* options,
                                  h2_pal_task_entry_t callback, void* context,
                                  h2_pal_task_t** out) {
  assert(user == &f && options && out && !context);
  assert(options->name == h2_gizclaw_e2e_launcher_task_name);
  callback(context);
  return H2_PAL_OK;
}
static const h2_pal_task_vtable_t task_vtable = {.start = start_task};
static const h2_pal_task_api_t task_api = {.user = &f, .vtable = &task_vtable};
h2_pal_result_t h2_bk7258_board_runtime_config(h2_runtime_config_t* config) {
  ++f.boards;
  *config = (h2_runtime_config_t){.mem = &mem_api,
                                  .time = &time_api,
                                  .net = &net_api,
                                  .wifi_sta = &wifi_api,
                                  .power = &power_api,
                                  .task = &task_api};
  return H2_PAL_OK;
}
void h2_bk7258_board_runtime_deinit(void) { ++f.deinit; }
h2_pal_mem_api_t* h2_bk7258_board_psram_allocator(void) { return &mem_api; }
h2_pal_result_t h2_runtime_init(const h2_runtime_config_t* config,
                                h2_runtime_t** out) {
  assert(config->http == &fixture_http_api && f.provider_live);
  f.runtime = (h2_runtime_t){.mem = config->mem,
                             .time = config->time,
                             .net = config->net,
                             .wifi_sta = config->wifi_sta,
                             .power = config->power,
                             .task = config->task};
  *out = &f.runtime;
  return H2_PAL_OK;
}
void rtos_delay_milliseconds(uint32_t ms) {
  if (ms == 10000u) longjmp(terminal, 1);
  if (ms == 3000u) {
    assert(!f.reboots || f.reboot_returns);
    assert(!f.locked && f.failure_count == 1u);
    ++f.waits;
    if ((f.selection_retry || f.prepare_retry) && f.waits == 1u) return;
    longjmp(terminal, 3);
  }
  assert(ms == 250u && f.next_partition == 1u && f.locked);
}
int h2_bk_target_task_policy_install(void) { return H2_PAL_OK; }
void bk_init(void) {}
h2_pal_result_t h2_bk7258_board_start_entry_task(const char* name,
                                                 void (*callback)(void*),
                                                 void* user) {
  assert(!strcmp(name, "bk/gizclaw-e2e") && !user);
  callback(user);
  return H2_PAL_OK;
}
const char* h2_gizclaw_e2e_fixture_key(void) { return "fixture"; }
const char* h2_gizclaw_e2e_fixture_profile(void) { return "fixture"; }
const char* h2_gizclaw_e2e_fixture_workflow_tag(void) { return "fixture"; }
const char* h2_gizclaw_e2e_fixture_value(void) { return "fixture"; }
const char* h2_gizclaw_e2e_fixture_endpoint(void) {
  return "https://fixture.invalid";
}
const char* h2_gizclaw_e2e_fixture_token(void) { return "fixture"; }
const char* h2_gizclaw_e2e_fixture_time_server(void) {
  return "time.fixture.invalid";
}
int h2_gizclaw_e2e_fixture_physical_audio(void) { return 0; }
const char* h2_gizclaw_e2e_fixture_device_api_url(void) {
  return "https://fixture.invalid/api";
}
const char* h2_gizclaw_e2e_fixture_audio_url(void) {
  return "https://fixture.invalid/audio";
}
const char* h2_gizclaw_e2e_fixture_root_ca_hex(void) { return "4142"; }

static void reset(void) {
  const h2_device_app_commands_config_t config = h2_management.config;
  memset(&f, 0, sizeof(f));
  f.calibrated = true;
  f.next_partition = 2u;
  h2_management = (h2_device_app_commands_t){.config = config};
  runtime = NULL;
  execution = NULL;
  fixture_http = NULL;
}
static int launch(void) {
  const int reason = setjmp(terminal);
  if (reason) return reason;
  native_launcher_main();
  assert(0);
  return 0;
}
static void expect_time_failure(int rc) {
  assert(launch() == 2);
  assert(!strcmp(f.failure_stage, "time") && f.failure_rc == rc);
  assert(!f.creates && !f.runs && !f.confirms && f.next_partition == 1u);
}

int main(void) {
  reset();
  assert(launch() == 1);
  assert(f.creates == 1u && f.confirms == 1u && !f.resolves && !f.syncs);
  assert(f.wall_reads == 1u && !f.wall_sets && !f.selects);

  reset();
  f.calibrated = false;
  assert(launch() == 1);
  assert(f.syncs == 1u && f.wall_sets == 1u && f.wall_reads == 1u);
  assert(f.creates == 1u && f.confirms == 1u && !f.selects);

  reset();
  f.wall_rc = H2_PAL_ERR_IO;
  expect_time_failure(H2_PAL_ERR_IO);
  assert(!f.resolves && !f.syncs);
  const int ntp_errors[] = {H2_NTP_ERR_TIMEOUT, H2_NTP_ERR_UNSUPPORTED,
                            H2_NTP_OK_TIME_SET_UNSUPPORTED,
                            H2_NTP_ERR_MALFORMED};
  const int pal_errors[] = {H2_PAL_ERR_TIMEOUT, H2_PAL_ERR_UNSUPPORTED,
                            H2_PAL_ERR_UNSUPPORTED, H2_PAL_ERR_IO};
  for (size_t i = 0u; i < sizeof(ntp_errors) / sizeof(ntp_errors[0]); ++i) {
    reset();
    f.calibrated = false;
    f.ntp_rc = ntp_errors[i];
    expect_time_failure(pal_errors[i]);
    assert(f.syncs == 1u && !f.wall_sets);
  }
  reset();
  f.calibrated = false;
  f.resolve_rc = H2_PAL_ERR_TIMEOUT;
  expect_time_failure(H2_PAL_ERR_TIMEOUT);
  assert(!f.syncs);
  reset();
  f.calibrated = false;
  f.remain_uncalibrated = true;
  expect_time_failure(H2_PAL_TIME_ERR_UNCALIBRATED);
  assert(f.wall_sets == 1u && !f.wall_reads);

  /* A persisted, already confirmed image must select Loader after setup
   * failure. */
  reset();
  f.confirmed = true;
  f.setup_failure = true;
  assert(launch() == 2);
  assert(f.confirmed && !f.runs && !f.confirms && f.selects == 1u);
  assert(!strcmp(f.failure_stage, "evidence"));

  /* A console failure after successful confirmation must also select Loader. */
  reset();
  f.publish_failure = true;
  assert(launch() == 2);
  assert(f.confirmed && f.confirms == 1u && f.selects == 1u);
  assert(!strcmp(f.failure_stage, "evidence_publish") &&
         f.failure_rc == H2_PAL_ERR_BUSY);

  reset();
  f.publish_failure = true;
  f.select_rc = H2_PAL_ERR_IO;
  assert(launch() == 3);
  assert(f.confirmed && !f.reboots && f.next_partition == 2u &&
         f.unlocks == 1u);
  assert(h2_management.serial_started && f.admits && f.provider_live &&
         execution);
  assert(f.deinit == 0u);

  reset();
  f.publish_failure = true;
  f.selection_retry = true;
  assert(launch() == 2);
  assert(f.selects == 2u && f.waits == 1u && f.unlocks == 1u &&
         f.reboots == 1u);
  assert(f.confirms == 1u && f.creates == 1u && f.provider_live);

  reset();
  f.pre_runtime_failure = true;
  f.prepare_retry = true;
  assert(launch() == 2);
  assert(!runtime && f.boards == 3u && f.prepares == 2u && f.waits == 1u);
  assert(f.deinit == 1u && f.serial_starts == 1u && f.selects == 1u);

  reset();
  f.publish_failure = true;
  f.reboot_returns = true;
  f.reboot_rc = H2_PAL_ERR_IO;
  assert(launch() == 3);
  assert(f.unlocks == 1u && f.reboots == 1u && f.next_partition == 1u);
  assert(h2_management.serial_started && f.provider_live && execution);
  return 0;
}
