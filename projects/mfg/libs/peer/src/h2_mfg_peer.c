#include "h2_mfg_peer.h"
#include "h2_mfg_peer_scan.h"
#include "h2_h2loader_host.h"
#include "h2_bleikcp.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct peer {
  h2_runtime_t* rt;
  h2_pal_mutex_t* mutex;
  h2_mfg_peer_scan_t scan_state;
  h2_pal_system_event_subscription_t* disconnect_subscription;
  int disconnected;
  char reply[1024];
  size_t used;
  h2_h2loader_host_ble_connection_t* connection;
} peer_t;

static int disconnected(void* user, const h2_pal_system_event_t* event) {
  peer_t* p = user;
  if (event->type != H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED ||
      event->payload == NULL ||
      event->payload_size != sizeof(h2_pal_ble_disconnected_info_t))
    return 0;
  const h2_pal_ble_disconnected_info_t* info = event->payload;
  if (memcmp(info->peer_addr.value, p->scan_state.address, 6) != 0) return 0;
  int rc = h2_pal_mutex_lock(p->rt->sync, p->mutex);
  if (rc != 0) return rc;
  p->disconnected = 1;
  return h2_pal_mutex_unlock(p->rt->sync, p->mutex);
}

static void cleanup_log(peer_t* p, const char* phase, int result) {
  char line[128];
  (void)snprintf(line, sizeof(line), "H2_MFG_PEER_CLEANUP phase=%s code=%d",
                 phase, result);
  (void)h2_pal_log_write(p->rt->log, H2_PAL_LOG_INFO, "mfg-peer", line);
}

static bool scan(void* user, const h2_pal_ble_scan_result_t* result) {
  peer_t* p = user;
  if (h2_pal_mutex_lock(p->rt->sync, p->mutex) != 0) return false;
  bool found = h2_mfg_peer_scan_update(&p->scan_state, result);
  (void)h2_pal_mutex_unlock(p->rt->sync, p->mutex);
  return found;
}

static uint64_t clock_ms(peer_t* p) {
  uint64_t now = 0;
  (void)h2_pal_time_get_monotonic_ms(p->rt->time, &now);
  return now;
}

static void drain_events(peer_t* p) {
  uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
  h2_runtime_event_t event = {.payload = payload,
                              .payload_capacity = sizeof(payload)};
  for (unsigned i = 0; i < 128; ++i)
    if (h2_runtime_poll_event(p->rt, &event) != 0) break;
}

static h2_pal_result_t output(void* user, const uint8_t* data, size_t len) {
  peer_t* p = user;
  if (len >= sizeof(p->reply) - p->used) return H2_PAL_ERR_TRUNCATED;
  memcpy(p->reply + p->used, data, len);
  p->used += len;
  p->reply[p->used] = '\0';
  return 0;
}

static h2_pal_result_t connect_log(void* user, const uint8_t* data,
                                   size_t len) {
  peer_t* p = user;
  char line[160];
  if (len >= sizeof(line)) return H2_PAL_ERR_FORMAT;
  memcpy(line, data, len);
  while (len != 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) --len;
  line[len] = '\0';
  return h2_pal_log_write(p->rt->log, H2_PAL_LOG_INFO, "mfg-peer", line);
}

static int command(peer_t* p, const h2_h2loader_host_status_t* status,
                   const char* action, const char* ticket) {
  drain_events(p);
  p->used = 0;
  p->reply[0] = '\0';
  h2_h2loader_host_command_request_t request = {
      .command = H2_H2LOADER_HOST_COMMAND_MFG,
      .status = status,
      .mfg_action = action,
      .mfg_step = ticket == NULL ? NULL : "ble",
      .mfg_ticket = ticket,
      .on_output = output,
      .output_user = p};
  h2_h2loader_host_command_result_t result;
  int rc =
      h2_h2loader_host_ble_execute_command(p->connection, &request, &result);
  if (rc == 0 && result.terminal != H2_H2LOADER_HOST_COMMAND_TERMINAL_OK)
    rc = H2_PAL_ERR_INVALID_STATE;
  return rc;
}

static int exchange(peer_t* p, const char* uid) {
  const h2_h2loader_host_ble_connection_config_t config = {
      .ble = p->rt->ble_host,
      .task = p->rt->task,
      .time = p->rt->time,
      .sync = p->rt->sync,
      .system_event = p->rt->system_event,
      .allocator = p->rt->mem,
      .address = p->scan_state.observed,
      .connect_timeout_ms = 8000,
      .command_timeout_ms = 5000,
      .on_log = connect_log,
      .log_user = p};
  h2_h2loader_host_status_t status;
  int rc = h2_h2loader_host_ble_connect(&config, &p->connection, &status);
  if (rc != 0) return rc;
  drain_events(p);
  char identity[192];
  (void)snprintf(identity, sizeof(identity),
                 "H2_MFG_PEER_IDENTITY uid=%s board=%s expected_uid=%s",
                 status.device_uid, status.board, uid);
  (void)h2_pal_log_write(p->rt->log, H2_PAL_LOG_INFO, "mfg-peer", identity);
  if (strcmp(status.device_uid, uid) != 0 ||
      status.active_role != H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER)
    return H2_PAL_ERR_INVALID_STATE;
  rc = command(p, &status, "status", NULL);
  if (rc != 0) return rc;
  if (strstr(p->reply, " step=ble ") == NULL) return H2_PAL_ERR_WOULD_BLOCK;
  const char* at = strstr(p->reply, " ticket=");
  if (at == NULL || strlen(at + 8) < 9 ||
      strspn(at + 8, "0123456789abcdefABCDEF") != 8 || at[16] != ' ')
    return H2_PAL_ERR_FORMAT;
  char ticket[9];
  memcpy(ticket, at + 8, 8);
  ticket[8] = '\0';
  const char* index_at = strstr(p->reply, " index=");
  if (index_at == NULL || index_at[7] < '0' || index_at[7] > '9')
    return H2_PAL_ERR_FORMAT;
  char* index_end = NULL;
  errno = 0;
  unsigned long ble_index = strtoul(index_at + 7, &index_end, 10);
  if (errno == ERANGE || *index_end != ' ' ||
      ble_index >= status.mfg_step_total)
    return H2_PAL_ERR_FORMAT;
  rc = command(p, &status, "probe", ticket);
  if (rc != 0) return rc;
  /* Confirm the durable result through another actual BLE response. */
  uint64_t started = clock_ms(p);
  do {
    rc = h2_h2loader_host_ble_read_status(p->connection, &status);
    if (rc != 0) return rc;
    drain_events(p);
    if (strcmp(status.device_uid, uid) != 0) return H2_PAL_ERR_INVALID_STATE;
    if (status.mfg_step_total > ble_index &&
        status.mfg_steps[ble_index] == 1u) {
      char line[192];
      (void)snprintf(line, sizeof(line),
                     "H2_MFG_PEER_PASS uid=%s board=%s ticket=%s "
                     "step_index=%lu durable=1",
                     uid, status.board, ticket, ble_index);
      (void)h2_pal_log_write(p->rt->log, H2_PAL_LOG_INFO, "mfg-peer", line);
      return 0;
    }
    (void)h2_pal_time_sleep_ms(p->rt->time, 100);
  } while (clock_ms(p) - started < 3000);
  return H2_PAL_ERR_TIMEOUT;
}

int h2_mfg_peer_run(h2_runtime_t* rt, const char* uid, uint32_t timeout_ms) {
  return h2_mfg_peer_run_with_address(rt, uid, uid, timeout_ms);
}

int h2_mfg_peer_run_with_address(h2_runtime_t* rt, const char* uid,
                                 const char* ble_address, uint32_t timeout_ms) {
  if (rt == NULL || uid == NULL || strlen(uid) != 12 ||
      strspn(uid, "0123456789abcdef") != 12 || timeout_ms == 0)
    return H2_PAL_ERR_INVALID_ARG;
  /* Retained on a failed PAL stop/join; callbacks never borrow stack state. */
  peer_t* p = h2_pal_mem_alloc(rt->mem, sizeof(*p));
  if (p == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(p, 0, sizeof(*p));
  p->rt = rt;
  int rc = h2_mfg_peer_scan_set_uid(&p->scan_state, ble_address);
  if (rc != 0) {
    h2_pal_mem_free(rt->mem, p);
    return rc;
  }
  h2_pal_mutex_config_t mc = {.name = "mfg/peer", .allocator = rt->mem};
  rc = h2_pal_mutex_create(rt->sync, &mc, &p->mutex);
  if (rc == 0)
    rc = h2_pal_system_event_subscribe(
        rt->system_event, H2_PAL_SYSTEM_EVENT_TYPE_BLE_DISCONNECTED,
        disconnected, p, &p->disconnect_subscription);
  int global_started = 0, host_started = 0;
  if (rc == 0) {
    rc = h2_bleikcp_global_init();
    global_started = rc == 0;
  }
  if (rc == 0) {
    rc = h2_pal_ble_start(rt->ble_host);
    host_started = rc == 0;
  }
  uint64_t started = clock_ms(p);
  int passed = 0;
  while (rc == 0 && !passed && clock_ms(p) - started < timeout_ms) {
    drain_events(p);
    (void)h2_pal_mutex_lock(rt->sync, p->mutex);
    h2_mfg_peer_scan_begin(&p->scan_state);
    (void)h2_pal_mutex_unlock(rt->sync, p->mutex);
    h2_pal_ble_scan_params_t params = {.mode = H2_PAL_BLE_SCAN_MODE_ACTIVE,
                                       .interval_ms = 80,
                                       .window_ms = 60,
                                       .timeout_ms = 2000};
    rc = h2_pal_ble_start_scan(rt->ble_host, &params, scan, p);
    if (rc != 0) break;
    uint64_t scan_start = clock_ms(p);
    int found = 0;
    while (clock_ms(p) - scan_start < 2100) {
      drain_events(p);
      (void)h2_pal_mutex_lock(rt->sync, p->mutex);
      found = p->scan_state.found;
      (void)h2_pal_mutex_unlock(rt->sync, p->mutex);
      if (found) break;
      (void)h2_pal_time_sleep_ms(rt->time, 20);
    }
    int stop = h2_pal_ble_stop_scan(rt->ble_host);
    if (stop != 0 && stop != H2_PAL_ERR_INVALID_STATE) return stop;
    char scan_line[128];
    (void)snprintf(scan_line, sizeof(scan_line),
                   "H2_MFG_PEER_SCAN reports=%u found=%d",
                   p->scan_state.reports, found);
    (void)h2_pal_log_write(rt->log, H2_PAL_LOG_INFO, "mfg-peer", scan_line);
    if (found) {
      const uint8_t* address = p->scan_state.observed.value;
      char found_line[128];
      (void)snprintf(
          found_line, sizeof(found_line),
          "H2_MFG_PEER_FOUND address=%02x%02x%02x%02x%02x%02x expected_uid=%s",
          address[5], address[4], address[3], address[2], address[1],
          address[0], uid);
      (void)h2_pal_log_write(rt->log, H2_PAL_LOG_INFO, "mfg-peer", found_line);
      (void)h2_pal_mutex_lock(rt->sync, p->mutex);
      p->disconnected = 0;
      (void)h2_pal_mutex_unlock(rt->sync, p->mutex);
      int exchange_result = exchange(p, uid);
      int had_connection = p->connection != NULL;
      int disconnect = h2_h2loader_host_ble_disconnect(&p->connection);
      cleanup_log(p, "disconnect-request-and-join", disconnect);
      if (disconnect != 0) return disconnect;
      uint64_t disconnect_start = clock_ms(p);
      int ended = 0;
      while (had_connection && clock_ms(p) - disconnect_start < 3000) {
        drain_events(p);
        (void)h2_pal_mutex_lock(rt->sync, p->mutex);
        ended = p->disconnected;
        (void)h2_pal_mutex_unlock(rt->sync, p->mutex);
        if (ended) break;
        (void)h2_pal_time_sleep_ms(rt->time, 20);
      }
      cleanup_log(p, "disconnected-event",
                  !had_connection || ended ? 0 : H2_PAL_ERR_TIMEOUT);
      if (had_connection && !ended) return H2_PAL_ERR_TIMEOUT;
      passed = exchange_result == 0;
      if (!passed && exchange_result != H2_PAL_ERR_WOULD_BLOCK) {
        /* A P4's radio address belongs to its C6, while its Loader UID is
         * the P4 eFuse MAC. Wildcard discovery must try other service
         * candidates instead of repeatedly choosing the first nearby board.
         * exchange() still verifies the connected UID before any probe. */
        (void)h2_pal_mutex_lock(rt->sync, p->mutex);
        h2_mfg_peer_scan_reject_current(&p->scan_state);
        (void)h2_pal_mutex_unlock(rt->sync, p->mutex);
        char line[96];
        (void)snprintf(line, sizeof(line), "H2_MFG_PEER_RETRY code=%d",
                       exchange_result);
        (void)h2_pal_log_write(rt->log, H2_PAL_LOG_INFO, "mfg-peer", line);
      }
    } else {
      /* Once all visible candidates have been tried, allow a fresh cycle:
       * transient failures must not permanently exclude the actual DUT. */
      (void)h2_pal_mutex_lock(rt->sync, p->mutex);
      h2_mfg_peer_scan_retry_all(&p->scan_state);
      (void)h2_pal_mutex_unlock(rt->sync, p->mutex);
    }
    if (!passed) (void)h2_pal_time_sleep_ms(rt->time, 1000);
  }
  int stop = host_started ? h2_pal_ble_stop(rt->ble_host) : 0;
  cleanup_log(p, "host-stop", stop);
  if (stop != 0) return stop;
  if (p->disconnect_subscription != NULL) {
    h2_pal_system_event_unsubscribe(rt->system_event,
                                    p->disconnect_subscription);
    p->disconnect_subscription = NULL;
  }
  int shutdown = global_started ? h2_bleikcp_global_shutdown() : 0;
  cleanup_log(p, "global-shutdown", shutdown);
  if (shutdown != 0) return shutdown;
  if (p->mutex != NULL) (void)h2_pal_mutex_destroy(rt->sync, p->mutex);
  h2_pal_mem_free(rt->mem, p);
  return rc != 0 ? rc : passed ? 0 : H2_PAL_ERR_TIMEOUT;
}
