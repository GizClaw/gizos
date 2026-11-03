/* Test the production session dispatch against a fake SDK boundary. No serial
 * ports or firmware are touched. The real parser/ownership/command gates run.
 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "h2_h2loader_host.h"
#include "h2_h2loader_host_package.h"
#include "h2/pal/os/h2_pal_fs.h"
#include <assert.h>
#include "h2_h2loader_cli_target.h"
static h2_pal_result_t fake_connect(
    const h2_h2loader_host_serial_connection_config_t*,
    h2_h2loader_host_serial_connection_t**);
static h2_pal_result_t fake_disconnect(h2_h2loader_host_serial_connection_t**);
static h2_pal_result_t fake_status(h2_h2loader_host_serial_connection_t*,
                                   h2_h2loader_host_status_t*);
static h2_pal_result_t fake_execute(h2_h2loader_host_serial_connection_t*,
                                    const h2_h2loader_host_command_request_t*,
                                    h2_h2loader_host_command_result_t*);
static h2_pal_result_t fake_monitor(h2_h2loader_host_serial_connection_t*,
                                    h2_h2loader_host_cancelled_fn, void*);
static int fake_path(const char*, char*, size_t);
static h2_pal_result_t fake_stat(const h2_pal_fs_api_t*, const char*,
                                 h2_pal_fs_stat_t*);
static h2_pal_result_t fake_open(const h2_pal_fs_api_t*, const char*,
                                 h2_pal_fs_open_mode_t, h2_pal_fs_file_t**);
static h2_pal_result_t fake_close(const h2_pal_fs_api_t*, h2_pal_fs_file_t*);
static h2_pal_result_t fake_inspect(
    const h2_h2loader_host_package_inspect_config_t*,
    h2_h2loader_host_catalog_entry_t*);
static h2_pal_result_t fake_stage(h2_h2loader_host_serial_connection_t*,
                                  const h2_h2loader_host_catalog_entry_t*,
                                  h2_h2loader_host_payload_read_fn, void*,
                                  h2_h2loader_host_cancelled_fn, void*,
                                  h2_h2loader_host_progress_fn, void*);
#define h2_h2loader_cli_target_resolve_path fake_path
#define h2_pal_fs_stat fake_stat
#define h2_pal_fs_open fake_open
#define h2_pal_fs_close fake_close
#define h2_h2loader_host_package_inspect fake_inspect
#define h2_h2loader_host_serial_stage fake_stage
#define h2_h2loader_host_serial_connect fake_connect
#define h2_h2loader_host_serial_disconnect fake_disconnect
#define h2_h2loader_host_serial_read_status fake_status
#define h2_h2loader_host_serial_execute_command fake_execute
#define h2_h2loader_host_serial_monitor_logs fake_monitor
#define main session_program_main
#define wmain session_program_wmain
#include "session.c"
#undef main
#undef wmain

static unsigned opens, closes, writes;
static int staged;
static const char* fake_uid = "001122334455";
enum {
  MATCHING_APP,
  WRONG_STAGE,
  WRONG_APP,
  WRONG_LOADER,
  WRONG_BOARD,
  WRONG_TARGET,
  WRONG_ROLE,
  WRONG_UID
};
typedef struct reboot_probe {
  h2_h2loader_host_status_t before;
  unsigned confirm_at, cancel_at, status_fail_at, status_fail_from;
  unsigned mismatch_from;
  int mismatch;
  unsigned polls, monitors, logs, reboots, sleeps;
  uint64_t now_ms, last_poll_ms;
  h2_pal_result_t monitor_result;
} reboot_probe_t;
static reboot_probe_t* reboot_probe;
static h2_h2loader_host_transport_log_fn fake_log;
static void* fake_log_user;
static h2_pal_result_t fake_connect(
    const h2_h2loader_host_serial_connection_config_t* config,
    h2_h2loader_host_serial_connection_t** out) {
  assert(config->preserve_control_lines == 1);
  assert(config->handshake_timeout_ms == 60000u);
  assert(config->command_timeout_ms == 120000u);
  assert(config->on_log != NULL);
  fake_log = config->on_log;
  fake_log_user = config->log_user;
  assert(*out == NULL);
  ++opens;
  *out = (h2_h2loader_host_serial_connection_t*)(uintptr_t)1;
  return H2_PAL_OK;
}
static h2_pal_result_t fake_disconnect(
    h2_h2loader_host_serial_connection_t** out) {
  if (*out) ++closes;
  *out = NULL;
  return H2_PAL_OK;
}
static h2_pal_result_t fake_status(
    h2_h2loader_host_serial_connection_t* session,
    h2_h2loader_host_status_t* out) {
  assert(session != NULL);
  if (reboot_probe != NULL) {
    reboot_probe_t* probe = reboot_probe;
    *out = probe->before;
    if (!probe->reboots) return H2_PAL_OK;
    ++probe->polls;
    assert(probe->polls <= 800u);  // catch unbounded waits on mismatched Apps
    assert(probe->now_ms - probe->last_poll_ms >= 500u);
    probe->last_poll_ms = probe->now_ms;
    if (probe->polls == probe->status_fail_at ||
        (probe->status_fail_from && probe->polls >= probe->status_fail_from))
      return H2_PAL_ERR_IO;
    out->active_role = H2_H2LOADER_HOST_ACTIVE_ROLE_APP;
    out->running_partition = out->next_partition = 2u;
    out->partition_2 = probe->before.stage;
    if (probe->polls >= probe->confirm_at) out->stage.valid = 0;
    if (probe->mismatch_from && probe->polls >= probe->mismatch_from) {
      switch (probe->mismatch) {
        case WRONG_STAGE:
          out->stage.package_checksum[0] = 'b';
          break;
        case WRONG_APP:
          out->partition_2.image_checksum[0] = 'b';
          break;
        case WRONG_LOADER:
          out->partition_1.version[0] = 'x';
          break;
        case WRONG_BOARD:
          out->board[0] = 'x';
          break;
        case WRONG_TARGET:
          out->target[0] = 'x';
          break;
        case WRONG_ROLE:
          out->active_role = H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER;
          break;
        case WRONG_UID:
          out->device_uid[0] = 'f';
          break;
      }
    }
    return H2_PAL_OK;
  }
  memset(out, 0, sizeof(*out));
  (void)snprintf(out->device_uid, sizeof(out->device_uid), "%s", fake_uid);
  (void)snprintf(out->board, sizeof(out->board), "fixture");
  (void)snprintf(out->target, sizeof(out->target), "host");
  out->active_role = H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER;
  out->command_availability = H2_H2LOADER_HOST_COMMAND_AVAILABLE_STATUS |
                              H2_H2LOADER_HOST_COMMAND_AVAILABLE_STAGE_PAYLOAD;
  if (staged) {
    out->stage.valid = 1;
    out->stage.package_size = 16;
    memset(out->stage.package_checksum, 'a', 64);
  }
  return H2_PAL_OK;
}
static h2_pal_result_t fake_execute(
    h2_h2loader_host_serial_connection_t* session,
    const h2_h2loader_host_command_request_t* request,
    h2_h2loader_host_command_result_t* result) {
  assert(session == connection && reboot_probe != NULL);
  assert(request->command == H2_H2LOADER_HOST_COMMAND_REBOOT_UPGRADE);
  assert(request->status->stage.valid);
  assert(request->is_cancelled != NULL);
  assert(!request->is_cancelled(request->cancel_user));
  ++reboot_probe->reboots;
  result->terminal = H2_H2LOADER_HOST_COMMAND_TERMINAL_OK;
  return H2_PAL_OK;
}
static h2_pal_result_t fake_monitor(
    h2_h2loader_host_serial_connection_t* session,
    h2_h2loader_host_cancelled_fn done, void* user) {
  assert(session == connection && reboot_probe != NULL);
  reboot_probe_t* probe = reboot_probe;
  ++probe->monitors;
  if (probe->monitor_result != H2_PAL_OK) return probe->monitor_result;
  assert(!done(user));
  static const uint8_t log[] = "App awaiting confirmation\n";
  assert(fake_log(fake_log_user, log, sizeof(log) - 1u) == H2_PAL_OK);
  ++probe->logs;
  probe->now_ms += 250u;
  if (probe->cancel_at && probe->polls >= probe->cancel_at)
    atomic_store(&cancelled_id, current_id);
  if (done(user)) return H2_PAL_EXIT;
  probe->now_ms += 250u;
  assert(done(user));
  return H2_PAL_EXIT;
}
static h2_pal_result_t fake_clock(void* user, uint64_t* out) {
  *out = ((reboot_probe_t*)user)->now_ms;
  return H2_PAL_OK;
}
static h2_pal_result_t fake_sleep(void* user, uint32_t duration) {
  reboot_probe_t* probe = user;
  assert(duration == 500u);
  ++probe->sleeps;
  probe->now_ms += duration;
  return H2_PAL_OK;
}
static const h2_pal_time_vtable_t fake_time_vtable = {
    .get_monotonic_ms = fake_clock, .sleep_ms = fake_sleep};
static int fake_path(const char* path, char* out, size_t size) {
  return snprintf(out, size, "%s", path) > 0;
}
static h2_pal_result_t fake_stat(const h2_pal_fs_api_t* api, const char* path,
                                 h2_pal_fs_stat_t* out) {
  (void)api;
  (void)path;
  memset(out, 0, sizeof(*out));
  out->size = 16;
  return H2_PAL_OK;
}
static h2_pal_result_t fake_open(const h2_pal_fs_api_t* api, const char* path,
                                 h2_pal_fs_open_mode_t mode,
                                 h2_pal_fs_file_t** out) {
  (void)api;
  (void)path;
  (void)mode;
  *out = (h2_pal_fs_file_t*)(uintptr_t)1;
  return H2_PAL_OK;
}
static h2_pal_result_t fake_close(const h2_pal_fs_api_t* api,
                                  h2_pal_fs_file_t* file) {
  (void)api;
  (void)file;
  return H2_PAL_OK;
}
static h2_pal_result_t fake_inspect(
    const h2_h2loader_host_package_inspect_config_t* config,
    h2_h2loader_host_catalog_entry_t* out) {
  assert(config->payload_bytes == 16);
  memset(out, 0, sizeof(*out));
  (void)snprintf(out->board, sizeof(out->board), "fixture");
  (void)snprintf(out->target, sizeof(out->target), "host");
  out->bytes = 16;
  memset(out->sha256, 'a', 64);
  return H2_PAL_OK;
}
static h2_pal_result_t fake_stage(h2_h2loader_host_serial_connection_t* session,
                                  const h2_h2loader_host_catalog_entry_t* asset,
                                  h2_h2loader_host_payload_read_fn read,
                                  void* payload,
                                  h2_h2loader_host_cancelled_fn cancel,
                                  void* cancel_user,
                                  h2_h2loader_host_progress_fn notify,
                                  void* progress_user) {
  (void)read;
  (void)payload;
  assert(session == connection);
  assert(asset->bytes == 16);
  assert(!cancel(cancel_user));
  ++writes;
  staged = 1;
  notify(progress_user, 16, 16);
  return output("device", (const uint8_t*)"log during stage\n", 17);
}
static void run_reboot_cases(void) {
  static const struct {
    const char* name;
    reboot_probe_t probe;
    h2_pal_result_t result;
    unsigned polls, opens, monitors;
  } cases[] = {
      {"already-confirmed", {.confirm_at = 1u}, H2_PAL_OK, 1u, 2u, 0u},
      {"pending150-then-confirmed",
       {.confirm_at = 151u},
       H2_PAL_OK,
       151u,
       2u,
       150u},
      {"pending650-then-confirmed",
       {.confirm_at = 651u},
       H2_PAL_OK,
       651u,
       2u,
       650u},
      {"pending-cancel",
       {.confirm_at = 999u, .cancel_at = 200u},
       H2_PAL_EXIT,
       200u,
       2u,
       200u},
      {"pending-one-status-error-then-confirmed",
       {.confirm_at = 171u, .status_fail_at = 151u},
       H2_PAL_OK,
       171u,
       3u,
       169u},
      {"pending-then120-status-errors",
       {.confirm_at = 999u, .status_fail_from = 151u},
       H2_PAL_ERR_IO,
       270u,
       121u,
       150u},
      {"wrong-stage-bounded120",
       {.confirm_at = 999u, .mismatch_from = 1u, .mismatch = WRONG_STAGE},
       H2_PAL_ERR_INVALID_STATE,
       120u,
       121u,
       0u},
      {"wrong-app-bounded120",
       {.confirm_at = 999u, .mismatch_from = 1u, .mismatch = WRONG_APP},
       H2_PAL_ERR_INVALID_STATE,
       120u,
       121u,
       0u},
      {"wrong-loader-bounded120",
       {.confirm_at = 999u, .mismatch_from = 1u, .mismatch = WRONG_LOADER},
       H2_PAL_ERR_INVALID_STATE,
       120u,
       121u,
       0u},
      {"wrong-board-bounded120",
       {.confirm_at = 999u, .mismatch_from = 1u, .mismatch = WRONG_BOARD},
       H2_PAL_ERR_INVALID_STATE,
       120u,
       121u,
       0u},
      {"wrong-target-bounded120",
       {.confirm_at = 999u, .mismatch_from = 1u, .mismatch = WRONG_TARGET},
       H2_PAL_ERR_INVALID_STATE,
       120u,
       121u,
       0u},
      {"wrong-role-bounded120",
       {.confirm_at = 999u, .mismatch_from = 1u, .mismatch = WRONG_ROLE},
       H2_PAL_ERR_INVALID_STATE,
       120u,
       121u,
       0u},
      {"pending-then-wrong-app-bounded120",
       {.confirm_at = 999u, .mismatch_from = 151u, .mismatch = WRONG_APP},
       H2_PAL_ERR_INVALID_STATE,
       270u,
       121u,
       150u},
      {"pending-UID-change-fails-closed",
       {.confirm_at = 999u, .mismatch_from = 151u, .mismatch = WRONG_UID},
       H2_PAL_ERR_INVALID_STATE,
       151u,
       2u,
       150u},
      {"confirmed-UID-change-fails-closed",
       {.confirm_at = 151u, .mismatch_from = 151u, .mismatch = WRONG_UID},
       H2_PAL_ERR_INVALID_STATE,
       151u,
       2u,
       150u},
      {"pending-monitor-IO-bounded120",
       {.confirm_at = 999u, .monitor_result = H2_PAL_ERR_IO},
       H2_PAL_ERR_IO,
       120u,
       121u,
       120u},
      {"pending-monitor-EXIT-bounded120",
       {.confirm_at = 999u, .monitor_result = H2_PAL_EXIT},
       H2_PAL_ERR_INVALID_STATE,
       120u,
       121u,
       120u},
  };
  for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    reboot_probe_t probe = cases[i].probe;
    probe.before = (h2_h2loader_host_status_t){
        .board = "fixture",
        .target = "host",
        .device_uid = "001122334455",
        .active_role = H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER,
        .running_partition = 1u,
        .next_partition = 1u,
        .boot_intent = H2_H2LOADER_HOST_BOOT_INTENT_AUTO,
        .last = H2_PAL_OK,
        .partition_1 = {.valid = 1,
                        .role = H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER,
                        .version = "loader"},
        .stage = {.valid = 1,
                  .role = H2_H2LOADER_HOST_ACTIVE_ROLE_APP,
                  .package_size = 16u,
                  .image_size = 32u,
                  .version = "app",
                  .board = "fixture",
                  .target = "host"},
    };
    memset(probe.before.stage.package_checksum, 'a', 64);
    memset(probe.before.stage.image_checksum, 'a', 64);
    reboot_probe = &probe;
    h2_pal_time_api_t time = {.user = &probe, .vtable = &fake_time_vtable};
    clock_api = &time;
    identity[0] = '\0';
    atomic_store(&cancelled_id, 0);
    opens = closes = 0u;
    request_t request = {.id = 6u, .command = "rebootUpgrade"};
    h2_pal_result_t rc = execute_request(&request);
    if (rc != cases[i].result || probe.polls != cases[i].polls ||
        opens != cases[i].opens || probe.monitors != cases[i].monitors)
      fprintf(
          stderr,
          "reboot case=%s code=%d polls=%u opens=%u closes=%u monitors=%u\n",
          cases[i].name, (int)rc, probe.polls, opens, closes, probe.monitors);
    assert(rc == cases[i].result && probe.polls == cases[i].polls);
    assert(opens == cases[i].opens && probe.monitors == cases[i].monitors);
    assert(probe.reboots == 1u && probe.sleeps == opens - 1u);
    if (rc == H2_PAL_OK) {
      assert(connection != NULL && !status.stage.valid);
      assert(closes == opens - 1u);
      disconnect_session();
    } else
      assert(connection == NULL);
    assert(closes == opens);
    if (probe.monitor_result == H2_PAL_OK)
      assert(probe.logs == probe.monitors);
    else
      assert(probe.logs == 0u);
    if (probe.cancel_at) assert(atomic_load(&cancelled_id) == request.id);
  }
  reboot_probe = NULL;
  clock_api = NULL;
}
static void run_dispatch(void) {
  request_t request;
  char first[] = "1 status\n", second[] = "2 status\n";
  assert(parse_request(first, &request));
  assert(execute_request(&request) == H2_PAL_OK);
  assert(parse_request(second, &request));
  assert(execute_request(&request) == H2_PAL_OK);
  assert(opens == 1 && closes == 0);  // no close/reopen between commands
  char encoded[] = "3 stage 2f746d702fe4b8ad\n";
  assert(parse_request(encoded, &request));
  assert(strcmp(request.path, "/tmp/中") == 0);
  assert(execute_request(&request) == H2_PAL_OK);
  assert(writes == 1);
  assert(opens == 1 && closes == 0);
  char invalid[] = "4 arbitrary\n", nul[] = "4 stage 00\n",
       oversized[] = "9007199254740992 status\n", extra[] = "4 status extra\n";
  assert(!parse_request(invalid, &request));
  assert(!parse_request(nul, &request));
  assert(!parse_request(oversized, &request));
  assert(!parse_request(extra, &request));
  char cancel[] = "5 status\n";
  assert(parse_request(cancel, &request));
  atomic_store(&cancelled_id, 5);
  assert(execute_request(&request) == H2_PAL_EXIT);
  assert(opens == 1 && closes == 1);
  fake_uid = "aabbccddeeff";
  current_id = 0;
  assert(connect_session() == H2_PAL_ERR_INVALID_STATE);
  assert(connection == NULL);
  assert(opens == 2 && closes == 2);  // never silently switch physical devices
}
int main(void) {
  run_dispatch();
  run_reboot_cases();
  puts(
      "session dispatch: persistent handle, framing, cancellation, identity and reboot confirmation checks passed");
  return 0;
}
