/* Test the production session dispatch against a fake SDK boundary. No serial
 * ports or firmware are touched. The real parser/ownership/command gates run.
 */
#include "h2_h2loader_host.h"
#include "h2_h2loader_host_package.h"
#include "h2/pal/os/h2_pal_fs.h"
#include <assert.h>
#include "h2_h2loader_native.h"
static h2_pal_result_t fake_connect(
    const h2_h2loader_host_serial_connection_config_t*,
    h2_h2loader_host_serial_connection_t**);
static h2_pal_result_t fake_disconnect(h2_h2loader_host_serial_connection_t**);
static h2_pal_result_t fake_status(h2_h2loader_host_serial_connection_t*,
                                   h2_h2loader_host_status_t*);
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
#define h2_h2loader_native_resolve_path fake_path
#define h2_pal_fs_stat fake_stat
#define h2_pal_fs_open fake_open
#define h2_pal_fs_close fake_close
#define h2_h2loader_host_package_inspect fake_inspect
#define h2_h2loader_host_serial_stage fake_stage
#define h2_h2loader_host_serial_connect fake_connect
#define h2_h2loader_host_serial_disconnect fake_disconnect
#define h2_h2loader_host_serial_read_status fake_status
#define main session_program_main
#define wmain session_program_wmain
#include "session.c"
#undef main
#undef wmain

static unsigned opens, closes, writes;
static int staged;
static const char* fake_uid = "001122334455";
static h2_pal_result_t fake_connect(
    const h2_h2loader_host_serial_connection_config_t* config,
    h2_h2loader_host_serial_connection_t** out) {
  assert(config->preserve_control_lines == 1);
  assert(config->handshake_timeout_ms == 60000u);
  assert(config->command_timeout_ms == 120000u);
  assert(config->on_log != NULL);
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
int main(void) {
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
  puts(
      "session dispatch: persistent handle, framing, cancellation and identity checks passed");
  return 0;
}
