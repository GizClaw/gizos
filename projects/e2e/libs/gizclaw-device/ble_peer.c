#define _POSIX_C_SOURCE 200809L
#include "ble_peer_transfer.h"
#include "ble_peer_receipt.h"

#include "h2_darwin_platform.h"
#include "h2_gizclaw_ble_binding.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct peer_scan {
  pthread_mutex_t mutex;
  const char *window;
  bool found;
  h2_pal_ble_addr_t address;
} peer_scan_t;

static void *allocate(void *user, size_t size) { (void)user; return malloc(size); }
static void *resize(void *user, void *ptr, size_t size) { (void)user; return realloc(ptr, size); }
static void release(void *user, void *ptr) { (void)user; free(ptr); }
static int quiet_log(void *user, h2_pal_log_level_t level,
                      const char *scope, const char *message) {
  (void)user; (void)level; (void)scope; (void)message;
  return H2_PAL_OK;
}

static void pause_ms(unsigned ms) {
  struct timespec delay = {.tv_sec = (time_t)(ms / 1000u),
      .tv_nsec = (long)(ms % 1000u) * 1000000L};
  while (nanosleep(&delay, &delay) && errno == EINTR) {}
}

static bool found(void *user, const h2_pal_ble_scan_result_t *result) {
  peer_scan_t *scan = user;
  if (!result->connectable || !result->local_name ||
      result->local_name_len != strlen(scan->window) ||
      memcmp(result->local_name, scan->window, result->local_name_len))
    return false;
  bool service = false;
  for (size_t i = 0u; i < result->service_uuid_count; ++i)
    if (result->service_uuids[i].len == h2_gizclaw_ble_binding_service_uuid.len &&
        !memcmp(result->service_uuids[i].data,
                  h2_gizclaw_ble_binding_service_uuid.data,
                  h2_gizclaw_ble_binding_service_uuid.len)) service = true;
  if (!service) return false;
  (void)pthread_mutex_lock(&scan->mutex);
  scan->address = result->addr; scan->found = true;
  (void)pthread_mutex_unlock(&scan->mutex);
  return true;
}

static int scan_window(const h2_pal_ble_host_api_t *ble, peer_scan_t *scan) {
  (void)pthread_mutex_lock(&scan->mutex); scan->found = false;
  (void)pthread_mutex_unlock(&scan->mutex);
  const h2_pal_ble_scan_params_t params = {.mode = H2_PAL_BLE_SCAN_MODE_ACTIVE,
      .interval_ms = 100u, .window_ms = 100u, .timeout_ms = 30000u};
  int rc = h2_pal_ble_start_scan(ble, &params, found, scan);
  if (rc != H2_PAL_OK) return rc;
  bool ready = false;
  for (unsigned i = 0u; i < 1500u; ++i) {
    (void)pthread_mutex_lock(&scan->mutex); ready = scan->found;
    (void)pthread_mutex_unlock(&scan->mutex);
    if (ready) break;
    pause_ms(20u);
  }
  rc = h2_pal_ble_stop_scan(ble);
  return rc != H2_PAL_OK ? rc : ready ? H2_PAL_OK : H2_PAL_ERR_TIMEOUT;
}

static bool hexadecimal(const char *text, size_t length) {
  if (strlen(text) != length) return false;
  for (size_t i = 0u; i < length; ++i)
    if (!((text[i] >= '0' && text[i] <= '9') ||
          (text[i] >= 'a' && text[i] <= 'f'))) return false;
  return true;
}

static bool number(const char *text, uint64_t *value) {
  if (!text[0]) return false;
  for (size_t i = 0u; text[i]; ++i)
    if (text[i] < '0' || text[i] > '9') return false;
  errno = 0; char *end = NULL;
  const unsigned long long parsed = strtoull(text, &end, 10);
  if (errno || !end || *end || parsed == 0u) return false;
  *value = (uint64_t)parsed; return true;
}

static bool version_valid(const char *version) {
  const size_t size = strlen(version);
  if (!size || size > 127u) return false;
  for (size_t i = 0u; i < size; ++i)
    if (!((version[i] >= '0' && version[i] <= '9') ||
          (version[i] >= 'a' && version[i] <= 'z') ||
          (version[i] >= 'A' && version[i] <= 'Z') ||
          version[i] == '-' || version[i] == '.' || version[i] == '_'))
      return false;
  return true;
}

static void utc(char out[40]) {
  struct timespec now; struct tm broken; char seconds[24];
  if (clock_gettime(CLOCK_REALTIME, &now) || !gmtime_r(&now.tv_sec, &broken) ||
      !strftime(seconds, sizeof(seconds), "%Y-%m-%dT%H:%M:%S", &broken)) {
    out[0] = '\0'; return;
  }
  (void)snprintf(out, 40u, "%s.%09ldZ", seconds, now.tv_nsec);
}

static uint64_t monotonic_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now)) return UINT64_MAX;
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

int main(int argc, char **argv) {
  const char *window = NULL, *digest = NULL, *output = NULL;
  const char *version = NULL, *execution = NULL;
  uint64_t revision = 0u, size = 0u;
  const char *usage = "ble_peer --version <firmware-version> --execution <32-hex> "
                      "--window h2e2e-<16-hex> --revision N --size N "
                      "--sha256 <64-hex> --output <new-receipt.json>\n";
  if (argc == 2 && !strcmp(argv[1], "--help")) { fputs(usage, stdout); return 0; }
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--window") && !window) window = argv[i + 1];
    else if (!strcmp(argv[i], "--sha256") && !digest) digest = argv[i + 1];
    else if (!strcmp(argv[i], "--output") && !output) output = argv[i + 1];
    else if (!strcmp(argv[i], "--version") && !version) version = argv[i + 1];
    else if (!strcmp(argv[i], "--execution") && !execution) execution = argv[i + 1];
    else if (!strcmp(argv[i], "--revision") && !revision && number(argv[i + 1], &revision)) {}
    else if (!strcmp(argv[i], "--size") && !size && number(argv[i + 1], &size)) {}
    else { fputs(usage, stderr); return 2; }
  }
  if (argc != 15 || !version || !version_valid(version) ||
      !execution || !hexadecimal(execution, 32u) ||
      !strcmp(execution, "00000000000000000000000000000000") ||
      !window || strlen(window) != 22u ||
      strncmp(window, "h2e2e-", 6u) || !hexadecimal(window + 6u, 16u) ||
      !digest || !hexadecimal(digest, 64u) || !revision || !size ||
      size > H2_GIZCLAW_BLE_BINDING_URL_MAX || !output) {
    fputs(usage, stderr); return 2;
  }
  const int descriptor = open(output, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (descriptor < 0) { fputs("cannot create a fresh private receipt\n", stderr); return 2; }
  FILE *receipt = fdopen(descriptor, "w");
  if (!receipt) { (void)close(descriptor); return 2; }
  /* A hard process deadline also covers stalled PAL calls and teardown. A
   * timeout terminates nonzero; a partial/self-reported receipt cannot admit it. */
  (void)alarm(H2_GIZCLAW_E2E_BLE_PEER_TIMEOUT_MS / 1000u);
  const uint64_t began_ms = monotonic_ms();
  char started[40], finished[40]; utc(started);
  static const h2_pal_mem_vtable_t memory_vtable = {
      .alloc = allocate, .realloc = resize, .free = release};
  static const h2_pal_mem_api_t memory = {.vtable = &memory_vtable};
  static const h2_pal_log_vtable_t log_vtable = {.write = quiet_log};
  static const h2_pal_log_api_t log = {.vtable = &log_vtable};
  const h2_pal_ble_host_api_t *ble = h2_darwin_corebluetooth_ble(&memory, &log);
  int rc = ble ? h2_pal_ble_start(ble) : H2_PAL_ERR_UNSUPPORTED;
  const bool host_started = rc == H2_PAL_OK;
  peer_scan_t scan = {.mutex = PTHREAD_MUTEX_INITIALIZER, .window = window};
  const h2_gizclaw_e2e_ble_peer_expected_t expected = {
      .revision = revision, .url_bytes = (size_t)size, .url_sha256 = digest};
  unsigned completed = 0u; size_t chunks[2] = {0};
  for (unsigned round = 0u; rc == H2_PAL_OK && round < 2u; ++round) {
    rc = scan_window(ble, &scan);
    if (rc == H2_PAL_OK) pause_ms(200u);
    uint16_t connection = H2_PAL_BLE_INVALID_CONN_HANDLE;
    const h2_pal_ble_connect_params_t params = {.timeout_ms = 8000u};
    if (rc == H2_PAL_OK)
      rc = h2_pal_ble_connect(ble, &scan.address, &params, &connection);
    if (rc == H2_PAL_OK) rc = h2_gizclaw_e2e_ble_peer_transfer(
        ble, connection, &expected, &chunks[round]);
    if (connection != H2_PAL_BLE_INVALID_CONN_HANDLE) {
      /* Keep the adopted link visible to the DUT's bounded polling loop. */
      if (rc == H2_PAL_OK) pause_ms(200u);
      const int disconnected = h2_pal_ble_disconnect(ble, connection);
      if (rc == H2_PAL_OK) rc = disconnected;
    }
    if (rc == H2_PAL_OK) ++completed;
  }
  if (host_started) {
    const int stopped = h2_pal_ble_stop(ble);
    if (rc == H2_PAL_OK) rc = stopped;
  }
  (void)pthread_mutex_destroy(&scan.mutex); utc(finished);
  const uint64_t ended_ms = monotonic_ms();
  const uint64_t elapsed = began_ms != UINT64_MAX && ended_ms >= began_ms
                               ? ended_ms - began_ms : UINT64_MAX;
  const bool qualified = rc == H2_PAL_OK && completed == 2u && started[0] &&
      finished[0] && elapsed < H2_GIZCLAW_E2E_BLE_PEER_TIMEOUT_MS;
  const h2_gizclaw_e2e_ble_peer_receipt_t value = {.version = version,
      .execution = execution, .window = window, .url_sha256 = digest,
      .started_utc = started, .finished_utc = finished, .revision = revision,
      .url_bytes = size, .elapsed_ms = elapsed, .completed_connections = completed,
      .chunks = {chunks[0], chunks[1]}, .rc = rc, .qualified = qualified};
  const int written = h2_gizclaw_e2e_ble_peer_write_receipt(receipt, &value);
  const int closed = fclose(receipt);
  fprintf(stderr, "BLE binding peer connections=%u rc=%d\n", completed, rc);
  (void)alarm(0u);
  return written > 0 && closed == 0 && qualified ? 0 : 1;
}
