/* Desktop stdio adapter over the shared GizOS Host Core. One process,
 * one serial connection, one SDK caller. The input thread never touches serial.
 * Device bytes are hex-framed and cannot inject control messages. */
#include "h2_h2loader_native.h"
#include "h2_h2loader_host_lifecycle.h"
#include "h2_h2loader_host_package.h"
#include "h2_bleikcp.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static CRITICAL_SECTION input_lock;
#define LOCK() EnterCriticalSection(&input_lock)
#define UNLOCK() LeaveCriticalSection(&input_lock)
#else
#include <pthread.h>
static pthread_mutex_t input_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&input_lock)
#define UNLOCK() pthread_mutex_unlock(&input_lock)
#endif

typedef struct request {
  unsigned long long id;
  char command[24];
  char path[4096];
} request_t;
static request_t queued;
static atomic_int pending, quitting;
static atomic_ullong cancelled_id;
static unsigned long long current_id;
static const h2_pal_time_api_t* clock_api;
static const h2_pal_fs_api_t* fs;
static h2_h2loader_host_serial_connection_t* connection;
static h2_h2loader_host_status_t status;
static char identity[H2_H2LOADER_HOST_DEVICE_UID_MAX_LEN];
static const char* endpoint;

static void json_string(const char* s) {
  putchar('"');
  for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
    if (*p == '"' || *p == '\\') {
      putchar('\\');
      putchar(*p);
    } else if (*p < 32u)
      printf("\\u%04x", (unsigned)*p);
    else
      putchar(*p);
  }
  putchar('"');
}
static const char* role_name(h2_h2loader_host_active_role_t role) {
  return role == H2_H2LOADER_HOST_ACTIVE_ROLE_APP      ? "app"
         : role == H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER ? "loader"
                                                       : "unknown";
}
static void metadata(const h2_h2loader_host_metadata_t* m) {
  printf(
      "{\"valid\":%s,\"packageSize\":%llu,\"imageSize\":%llu,\"packageChecksum\":",
      m->valid ? "true" : "false", (unsigned long long)m->package_size,
      (unsigned long long)m->image_size);
  json_string(m->package_checksum);
  printf(",\"imageChecksum\":");
  json_string(m->image_checksum);
  printf(",\"version\":");
  json_string(m->version);
  printf(",\"board\":");
  json_string(m->board);
  printf(",\"target\":");
  json_string(m->target);
  printf(",\"role\":");
  json_string(role_name(m->role));
  putchar('}');
}
static void publish_status(void) {
  printf("{\"event\":\"status\",\"id\":%llu,\"value\":{\"board\":", current_id);
  json_string(status.board);
  printf(",\"target\":");
  json_string(status.target);
  printf(",\"chip\":");
  json_string(status.chip);
  printf(",\"deviceUid\":");
  json_string(status.device_uid);
  printf(
      ",\"capabilities\":%u,\"commandAvailability\":%u,\"active\":{\"role\":",
      (unsigned)status.capabilities, (unsigned)status.command_availability);
  json_string(role_name(status.active_role));
  printf(",\"version\":");
  json_string(status.active_version);
  printf(",\"imageChecksum\":");
  json_string(status.active_checksum);
  printf(
      ",\"imageSize\":%llu},\"runningPartition\":%u,\"nextPartition\":%u,\"bootIntent\":",
      (unsigned long long)status.active_image_size,
      (unsigned)status.running_partition, (unsigned)status.next_partition);
  json_string(status.boot_intent == H2_H2LOADER_HOST_BOOT_INTENT_LOADER
                  ? "loader"
                  : "auto");
  printf(",\"stage\":");
  metadata(&status.stage);
  printf(",\"partition1\":");
  metadata(&status.partition_1);
  printf(",\"partition2\":");
  metadata(&status.partition_2);
  printf(",\"lastResult\":%d,\"mfg\":{\"mode\":%u,\"steps\":[",
         (int)status.last, (unsigned)status.mfg_mode);
  for (uint32_t i = 0; i < status.mfg_step_total; ++i)
    printf("%s%u", i ? "," : "", (unsigned)status.mfg_steps[i]);
  printf("]}}}\n");
  fflush(stdout);
}
static h2_pal_result_t output(void* user, const uint8_t* data, size_t len) {
  const char* source = user;
  while (len) {
    size_t count = len > 512u ? 512u : len;
    printf("{\"event\":\"data\",\"source\":\"%s\",\"hex\":\"", source);
    for (size_t i = 0; i < count; ++i) printf("%02x", (unsigned)data[i]);
    printf("\"}\n");
    data += count;
    len -= count;
  }
  fflush(stdout);
  return H2_PAL_OK;
}
static void progress(void* user, uint64_t acknowledged, uint64_t total) {
  (void)user;
  printf("{\"event\":\"progress\",\"id\":%llu,\"acked\":%llu,\"total\":%llu}\n",
         current_id, (unsigned long long)acknowledged,
         (unsigned long long)total);
  fflush(stdout);
}
static int cancelled(void* user) {
  (void)user;
  return atomic_load(&quitting) ||
         (current_id && atomic_load(&cancelled_id) == current_id);
}
static int idle_interrupt(void* user) {
  (void)user;
  return atomic_load(&quitting) || atomic_load(&pending);
}
static void disconnect_session(void) {
  (void)h2_h2loader_host_serial_disconnect(&connection);
  printf("{\"event\":\"disconnected\"}\n");
  fflush(stdout);
}
static h2_pal_result_t connect_session(void) {
  if (connection) return H2_PAL_OK;
  h2_h2loader_host_serial_connection_config_t config = {
      .serial = h2_h2loader_native_serial(),
      .time = clock_api,
      .allocator = h2_h2loader_native_mem(),
      .port_id = endpoint,
      .baud_rate = H2_H2LOADER_HOST_RELIABLE_SERIAL_BAUD,
      .handshake_timeout_ms = 60000u,
      .command_timeout_ms = 120000u,
      .on_log = output,
      .log_user = "device",
      .preserve_control_lines = 1,
  };
  h2_pal_result_t rc = h2_h2loader_host_serial_connect(&config, &connection);
  if (rc == H2_PAL_OK)
    rc = h2_h2loader_host_serial_read_status(connection, &status);
  if (rc == H2_PAL_OK && identity[0] && strcmp(identity, status.device_uid))
    rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK) {
    if (!identity[0])
      (void)snprintf(identity, sizeof(identity), "%s", status.device_uid);
    publish_status();
  } else
    disconnect_session();
  return rc;
}
static h2_pal_result_t file_read(void* user, uint64_t offset, uint8_t* out,
                                 size_t size, size_t* read) {
  h2_pal_result_t rc = h2_pal_fs_seek(fs, user, offset);
  return rc == H2_PAL_OK ? h2_pal_fs_read(fs, user, out, size, read) : rc;
}
static int inspect_package(const char* path) {
  char resolved[4096];
  if (!h2_h2loader_native_resolve_path(path, resolved, sizeof(resolved)))
    return 2;
  h2_pal_fs_stat_t info;
  h2_pal_result_t rc = h2_pal_fs_stat(fs, resolved, &info);
  if (rc != H2_PAL_OK || info.is_dir || !info.size ||
      info.size > 256u * 1024u * 1024u)
    return 2;
  h2_pal_fs_file_t* file = NULL;
  rc = h2_pal_fs_open(fs, resolved, H2_PAL_FS_OPEN_READ, &file);
  if (rc != H2_PAL_OK) return 2;
  h2_h2loader_host_catalog_entry_t asset = {0};
  h2_h2loader_host_package_inspect_config_t config = {
      .allocator = h2_h2loader_native_mem(),
      .read_payload = file_read,
      .payload_user = file,
      .payload_bytes = info.size,
  };
  rc = h2_h2loader_host_package_inspect(&config, &asset);
  (void)h2_pal_fs_close(fs, file);
  if (rc != H2_PAL_OK) {
    fprintf(stderr, "Package inspection failed: %d\n", (int)rc);
    return 2;
  }
  printf("{\"role\":");
  json_string(asset.role == H2_H2LOADER_HOST_ASSET_ROLE_APP ? "app"
                                                            : "h2loader");
  printf(",\"board\":");
  json_string(asset.board);
  printf(",\"target\":");
  json_string(asset.target);
  printf(",\"version\":");
  json_string(asset.version);
  printf("}\n");
  return 0;
}
static h2_pal_result_t stage(const char* path) {
  char resolved[4096];
  if (!h2_h2loader_native_resolve_path(path, resolved, sizeof(resolved)))
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_fs_stat_t stat_value;
  h2_pal_result_t rc = h2_pal_fs_stat(fs, resolved, &stat_value);
  if (rc != H2_PAL_OK || stat_value.is_dir ||
      stat_value.size > 256u * 1024u * 1024u)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_fs_file_t* file = NULL;
  rc = h2_pal_fs_open(fs, resolved, H2_PAL_FS_OPEN_READ, &file);
  if (rc != H2_PAL_OK) return rc;
  h2_h2loader_host_catalog_entry_t asset = {0};
  h2_h2loader_host_package_inspect_config_t inspect = {
      .allocator = h2_h2loader_native_mem(),
      .read_payload = file_read,
      .payload_user = file,
      .payload_bytes = stat_value.size,
  };
  rc = h2_h2loader_host_package_inspect(&inspect, &asset);
  if (rc == H2_PAL_OK && (strcmp(status.board, asset.board) ||
                          strcmp(status.target, asset.target) ||
                          !(status.command_availability &
                            H2_H2LOADER_HOST_COMMAND_AVAILABLE_STAGE_PAYLOAD)))
    rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK && cancelled(NULL)) rc = H2_PAL_EXIT;
  if (rc == H2_PAL_OK)
    rc = h2_h2loader_host_serial_stage(connection, &asset, file_read, file,
                                       cancelled, NULL, progress, NULL);
  (void)h2_pal_fs_close(fs, file);
  if (rc != H2_PAL_OK) return rc;
  // Verify durable stage on the SAME connection. Reconnect only after a real
  // transport failure; never replay a transfer whose outcome is uncertain.
  rc = h2_h2loader_host_serial_read_status(connection, &status);
  if (rc != H2_PAL_OK && !cancelled(NULL)) {
    disconnect_session();
    rc = connect_session();
  }
  if (rc == H2_PAL_OK &&
      (strcmp(status.board, asset.board) ||
       strcmp(status.target, asset.target) || !status.stage.valid ||
       status.stage.package_size != asset.bytes ||
       strcmp(status.stage.package_checksum, asset.sha256)))
    rc = H2_PAL_ERR_INVALID_STATE;
  return rc;
}
static h2_pal_result_t reboot(h2_h2loader_host_command_t command) {
  h2_h2loader_host_status_t before = status;
  h2_h2loader_host_command_request_t request = {
      .command = command,
      .status = &before,
      .is_cancelled = cancelled,
      .on_output = output,
      .output_user = "stdout",
  };
  h2_h2loader_host_command_result_t result = {0};
  h2_pal_result_t rc =
      h2_h2loader_host_serial_execute_command(connection, &request, &result);
  if (rc != H2_PAL_OK) return rc;
  if (result.terminal != H2_H2LOADER_HOST_COMMAND_TERMINAL_OK)
    return H2_PAL_ERR_INVALID_STATE;
  disconnect_session();
  for (unsigned i = 0; i < 120u && !cancelled(NULL); ++i) {
    (void)h2_pal_time_sleep_ms(clock_api, 500u);
    rc = connect_session();
    if (rc == H2_PAL_ERR_INVALID_STATE)
      return rc;  // identity changed: fail closed
    if (rc == H2_PAL_OK) {
      rc = h2_h2loader_host_verify_reboot_status(command, &before, &status);
      if (rc == H2_PAL_OK) return rc;
    }
    disconnect_session();
  }
  return cancelled(NULL) ? H2_PAL_EXIT : rc;
}
static int nibble(char c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                : -1;
}
static int parse_request(char* line, request_t* out) {
  char* end = NULL;
  errno = 0;
  memset(out, 0, sizeof(*out));
  out->id = strtoull(line, &end, 10);
  if (errno || !out->id || out->id > 9007199254740991ULL || *end != ' ')
    return 0;
  char* command = end + 1;
  end = strchr(command, ' ');
  char* newline = strchr(command, '\n');
  if (!newline) return 0;
  if (!end) end = newline;
  size_t count = (size_t)(end - command);
  if (!count || count >= sizeof(out->command)) return 0;
  memcpy(out->command, command, count);
  if (!strcmp(out->command, "stage")) {
    if (*end != ' ') return 0;
    const char* hex = end + 1;
    size_t length = (size_t)(newline - hex);
    if (!length || (length & 1u) || length / 2u >= sizeof(out->path)) return 0;
    for (size_t i = 0; i < length / 2u; ++i) {
      int a = nibble(hex[i * 2u]), b = nibble(hex[i * 2u + 1u]);
      if (a < 0 || b < 0 || !(a || b)) return 0;
      out->path[i] = (char)((a << 4) | b);
    }
    return 1;
  }
  if (end != newline) return 0;
  return !strcmp(out->command, "status") ||
         !strcmp(out->command, "rebootApp") ||
         !strcmp(out->command, "rebootLoader") ||
         !strcmp(out->command, "rebootUpgrade") ||
         !strcmp(out->command, "cancel");
}
#ifdef _WIN32
static DWORD WINAPI input_thread(LPVOID unused) {
#else
static void* input_thread(void* unused) {
#endif
  (void)unused;
  char line[8300];
  while (fgets(line, sizeof(line), stdin)) {
    request_t next;
    if (!parse_request(line, &next)) break;
    if (!strcmp(next.command, "cancel")) {
      atomic_store(&cancelled_id, next.id);
      continue;
    }
    LOCK();
    if (atomic_load(&pending)) {
      UNLOCK();
      break;
    }
    queued = next;
    atomic_store(&pending, 1);
    UNLOCK();
  }
  atomic_store(&quitting, 1);
#ifdef _WIN32
  return 0;
#else
  return NULL;
#endif
}
static h2_pal_result_t execute_request(const request_t* request) {
  current_id = request->id;
  h2_pal_result_t rc = cancelled(NULL) ? H2_PAL_EXIT : connect_session();
  if (rc == H2_PAL_OK)
    rc = h2_h2loader_host_serial_read_status(connection, &status);
  if (rc == H2_PAL_OK && !strcmp(request->command, "stage"))
    rc = stage(request->path);
  else if (rc == H2_PAL_OK && !strcmp(request->command, "rebootApp"))
    rc = reboot(H2_H2LOADER_HOST_COMMAND_REBOOT_APP);
  else if (rc == H2_PAL_OK && !strcmp(request->command, "rebootLoader"))
    rc = reboot(H2_H2LOADER_HOST_COMMAND_REBOOT_LOADER);
  else if (rc == H2_PAL_OK && !strcmp(request->command, "rebootUpgrade"))
    rc = reboot(H2_H2LOADER_HOST_COMMAND_REBOOT_UPGRADE);
  if (cancelled(NULL)) rc = H2_PAL_EXIT;
  if (rc == H2_PAL_OK)
    publish_status();
  else
    disconnect_session();
  return rc;
}
static int session_main(int argc, char** argv) {
  if (argc == 2 && !strcmp(argv[1], "--help")) {
    puts(
        "h2loader-desktop-session protocol=1; --port ENDPOINT; requests: ID status|stage HEX_PATH|rebootApp|rebootLoader|rebootUpgrade|cancel");
    return 0;
  }
  if (argc == 3 && !strcmp(argv[1], "--inspect")) {
    if (h2_h2loader_native_start() != H2_PAL_OK) return 3;
    fs = h2_h2loader_native_fs();
    int result = inspect_package(argv[2]);
    h2_h2loader_native_stop();
    return result;
  }
  if (argc != 3 || strcmp(argv[1], "--port") || !argv[2][0]) return 2;
  endpoint = argv[2];
  if (h2_h2loader_native_start() != H2_PAL_OK) return 3;
  if (h2_bleikcp_global_init() != H2_PAL_OK) {
    h2_h2loader_native_stop();
    return 3;
  }
  clock_api = h2_h2loader_native_time();
  fs = h2_h2loader_native_fs();
#ifdef _WIN32
  InitializeCriticalSection(&input_lock);
  HANDLE reader = CreateThread(NULL, 0, input_thread, NULL, 0, NULL);
  if (reader == NULL) return 3;
#else
  pthread_t reader;
  if (pthread_create(&reader, NULL, input_thread, NULL)) return 3;
#endif
  while (!atomic_load(&quitting)) {
    request_t request = {0};
    LOCK();
    if (atomic_load(&pending)) {
      request = queued;
      atomic_store(&pending, 0);
    }
    UNLOCK();
    if (!request.id) {
      current_id = 0;
      if (connection) {
        h2_pal_result_t rc = h2_h2loader_host_serial_monitor_logs(
            connection, idle_interrupt, NULL);
        if (rc != H2_PAL_EXIT && rc != H2_PAL_OK) disconnect_session();
      } else if (identity[0]) {
        for (unsigned i = 0; i < 30u && !idle_interrupt(NULL); ++i)
          (void)h2_pal_time_sleep_ms(clock_api, 100u);
        if (!idle_interrupt(NULL)) (void)connect_session();
      } else
        (void)h2_pal_time_sleep_ms(clock_api, 20u);
      continue;
    }
    h2_pal_result_t rc = execute_request(&request);
    printf("{\"event\":\"done\",\"id\":%llu,\"code\":%d}\n", request.id,
           (int)rc);
    fflush(stdout);
    current_id = 0;
  }
  disconnect_session();
#ifdef _WIN32
  WaitForSingleObject(reader, INFINITE);
  CloseHandle(reader);
  DeleteCriticalSection(&input_lock);
#else
  (void)pthread_join(reader, NULL);
#endif
  (void)h2_bleikcp_global_shutdown();
  h2_h2loader_native_stop();
  return 0;
}
#ifndef _WIN32
int main(int argc, char** argv) { return session_main(argc, argv); }
#else
int wmain(int argc, wchar_t** wide) {
  char** argv = calloc((size_t)argc + 1u, sizeof(*argv));
  if (!argv) return 3;
  int result = 3;
  for (int i = 0; i < argc; ++i) {
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1,
                                   NULL, 0, NULL, NULL);
    if (size <= 0 || !(argv[i] = malloc((size_t)size)) ||
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, argv[i],
                            size, NULL, NULL) != size)
      goto done;
  }
  result = session_main(argc, argv);
done:
  for (int i = 0; i < argc; ++i) free(argv[i]);
  free(argv);
  return result;
}
#endif
