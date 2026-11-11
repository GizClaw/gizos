#include "h2_g99_wifi.h"

#include "hal/h2_pal_wifi_settings.h"
#include "os/h2_pal_mem.h"
#include "os/h2_pal_sync.h"
#include "os/h2_pal_task.h"
#include "h2_g99_task_names.h"

#include <string.h>

typedef enum command {
  COMMAND_NONE = 0,
  COMMAND_SCAN,
  COMMAND_CONNECT,
} command_t;

struct h2_g99_wifi {
  h2_runtime_t* runtime;
  h2_pal_task_t* task;
  h2_pal_mutex_t* mutex;
  h2_pal_semaphore_t* wake;
  /* Guarded by mutex. */
  bool stop;
  h2_g99_wifi_snapshot_t snapshot;
  command_t pending;
  h2_pal_wifi_sta_config_t pending_config;
};

uint8_t h2_g99_wifi_bars(int rssi) {
  if (rssi >= -55) return 4u;
  if (rssi >= -65) return 3u;
  if (rssi >= -75) return 2u;
  if (rssi >= -85) return 1u;
  return 0u;
}

void h2_g99_wifi_merge(h2_g99_wifi_snapshot_t* snapshot,
                       const h2_pal_wifi_scan_entry_t* entry) {
  if (snapshot == NULL || entry == NULL || entry->ssid_len == 0u ||
      entry->ssid_len > H2_PAL_WIFI_SSID_MAX || entry->ssid[0] == '\0')
    return;
  uint8_t index = snapshot->network_count;
  for (uint8_t i = 0u; i < snapshot->network_count; ++i) {
    if (strncmp(snapshot->networks[i].ssid, entry->ssid,
                H2_PAL_WIFI_SSID_MAX) == 0) {
      if (snapshot->networks[i].rssi >= entry->rssi) return;
      index = i;
      break;
    }
  }
  if (index == snapshot->network_count) {
    if (index < H2_G99_WIFI_MAX_NETWORKS) {
      snapshot->network_count++;
    } else if (snapshot->networks[index - 1u].rssi >= entry->rssi) {
      return;
    } else {
      index = (uint8_t)(index - 1u);
    }
  }
  h2_g99_wifi_network_t network = {0};
  memcpy(network.ssid, entry->ssid, entry->ssid_len);
  network.rssi = entry->rssi;
  network.security = entry->security;
  /* Bubble the updated slot towards the front to keep RSSI order. */
  while (index > 0u && snapshot->networks[index - 1u].rssi < network.rssi) {
    snapshot->networks[index] = snapshot->networks[index - 1u];
    --index;
  }
  snapshot->networks[index] = network;
}

static void lock(h2_g99_wifi_t* wifi) {
  (void)h2_pal_mutex_lock(wifi->runtime->sync, wifi->mutex);
}

static void unlock(h2_g99_wifi_t* wifi) {
  (void)h2_pal_mutex_unlock(wifi->runtime->sync, wifi->mutex);
}

static void publish_status(h2_g99_wifi_t* wifi) {
  h2_pal_wifi_sta_status_t status = {0};
  const int rc = h2_pal_wifi_sta_get_status(wifi->runtime->wifi_sta, &status);
  lock(wifi);
  const bool connected =
      rc == H2_PAL_OK && (status.state == H2_PAL_WIFI_STA_STATE_CONNECTED ||
                          status.state == H2_PAL_WIFI_STA_STATE_GOT_IP);
  if (connected != wifi->snapshot.connected ||
      (connected && strncmp(wifi->snapshot.connected_ssid, status.ssid,
                            H2_PAL_WIFI_SSID_MAX) != 0)) {
    wifi->snapshot.connected = connected;
    memset(wifi->snapshot.connected_ssid, 0,
           sizeof(wifi->snapshot.connected_ssid));
    if (connected && status.ssid_len <= H2_PAL_WIFI_SSID_MAX)
      memcpy(wifi->snapshot.connected_ssid, status.ssid, status.ssid_len);
    wifi->snapshot.version++;
  }
  unlock(wifi);
}

static void set_phase(h2_g99_wifi_t* wifi, h2_g99_wifi_phase_t phase) {
  lock(wifi);
  wifi->snapshot.phase = phase;
  wifi->snapshot.version++;
  unlock(wifi);
}

static bool collect(void* user, const h2_pal_wifi_scan_entry_t* entry) {
  h2_g99_wifi_merge((h2_g99_wifi_snapshot_t*)user, entry);
  return true;
}

static void run_scan(h2_g99_wifi_t* wifi) {
  set_phase(wifi, H2_G99_WIFI_SCANNING);
  h2_g99_wifi_snapshot_t found = {0};
  const h2_pal_wifi_scan_request_t request = {0};
  const int rc = h2_pal_wifi_sta_scan(wifi->runtime->wifi_sta, &request,
                                      collect, &found, 10000u);
  lock(wifi);
  if (rc == H2_PAL_OK) {
    memcpy(wifi->snapshot.networks, found.networks, sizeof(found.networks));
    wifi->snapshot.network_count = found.network_count;
  }
  wifi->snapshot.last_result = (h2_pal_result_t)rc;
  wifi->snapshot.last_was_connect = false;
  wifi->snapshot.phase = H2_G99_WIFI_IDLE;
  wifi->snapshot.version++;
  unlock(wifi);
}

static void run_connect(h2_g99_wifi_t* wifi,
                        const h2_pal_wifi_sta_config_t* config) {
  set_phase(wifi, H2_G99_WIFI_CONNECTING);
  const h2_pal_result_t rc = h2_runtime_wifi_connect_and_save(
      wifi->runtime, config, H2_G99_WIFI_CONNECT_TIMEOUT_MS);
  publish_status(wifi);
  lock(wifi);
  wifi->snapshot.last_result = rc;
  wifi->snapshot.last_was_connect = true;
  wifi->snapshot.connects_done++;
  wifi->snapshot.phase = H2_G99_WIFI_IDLE;
  wifi->snapshot.version++;
  unlock(wifi);
}

static bool stopping(h2_g99_wifi_t* wifi) {
  lock(wifi);
  const bool stop = wifi->stop;
  unlock(wifi);
  return stop;
}

static void worker(void* context) {
  h2_g99_wifi_t* wifi = (h2_g99_wifi_t*)context;
  publish_status(wifi);
  bool connected = false;
  lock(wifi);
  connected = wifi->snapshot.connected;
  unlock(wifi);
  if (!connected && !stopping(wifi)) {
    /* Reconnect in the background; Main never waits for the network. */
    set_phase(wifi, H2_G99_WIFI_CONNECTING);
    (void)h2_runtime_wifi_connect_best_saved(wifi->runtime, 0u);
    publish_status(wifi);
    set_phase(wifi, H2_G99_WIFI_IDLE);
  }
  while (!stopping(wifi)) {
    const h2_pal_result_t wait =
        h2_pal_semaphore_take(wifi->runtime->sync, wifi->wake, 2000u);
    if (stopping(wifi)) break;
    command_t command = COMMAND_NONE;
    h2_pal_wifi_sta_config_t config = {0};
    lock(wifi);
    command = wifi->pending;
    config = wifi->pending_config;
    wifi->pending = COMMAND_NONE;
    memset(&wifi->pending_config, 0, sizeof(wifi->pending_config));
    unlock(wifi);
    if (command == COMMAND_SCAN) {
      run_scan(wifi);
    } else if (command == COMMAND_CONNECT) {
      run_connect(wifi, &config);
    } else if (wait != H2_PAL_OK) {
      publish_status(wifi);
    }
    memset(&config, 0, sizeof(config));
  }
}

h2_pal_result_t h2_g99_wifi_create(h2_runtime_t* runtime,
                                   h2_g99_wifi_t** out_wifi) {
  if (runtime == NULL || out_wifi == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_wifi = NULL;
  h2_g99_wifi_t* wifi = h2_pal_mem_alloc(runtime->mem, sizeof(*wifi));
  if (wifi == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(wifi, 0, sizeof(*wifi));
  wifi->runtime = runtime;
  wifi->snapshot.last_result = H2_PAL_OK;
  h2_pal_wifi_sta_status_t probe = {0};
  if (runtime->wifi_sta == NULL ||
      h2_pal_wifi_sta_get_status(runtime->wifi_sta, &probe) ==
          H2_PAL_ERR_UNSUPPORTED) {
    wifi->snapshot.phase = H2_G99_WIFI_UNAVAILABLE;
    *out_wifi = wifi;
    return H2_PAL_OK;
  }
  wifi->snapshot.phase = H2_G99_WIFI_IDLE;
  const h2_pal_mutex_config_t mutex_config = {"g99-wifi", runtime->mem, 0u};
  const h2_pal_semaphore_config_t wake_config = {"g99-wifi-wake", runtime->mem,
                                                 0u, 1u};
  h2_pal_result_t rc =
      h2_pal_mutex_create(runtime->sync, &mutex_config, &wifi->mutex);
  if (rc == H2_PAL_OK)
    rc = h2_pal_semaphore_create(runtime->sync, &wake_config, &wifi->wake);
  if (rc == H2_PAL_OK) {
    const h2_pal_task_options_t options = {h2_g99_wifi_task_name, 8192u};
    rc = h2_pal_task_start(runtime->task, &options, worker, wifi, &wifi->task);
  }
  if (rc != H2_PAL_OK) {
    if (wifi->wake != NULL)
      (void)h2_pal_semaphore_destroy(runtime->sync, wifi->wake);
    if (wifi->mutex != NULL)
      (void)h2_pal_mutex_destroy(runtime->sync, wifi->mutex);
    h2_pal_mem_free(runtime->mem, wifi);
    return rc;
  }
  *out_wifi = wifi;
  return H2_PAL_OK;
}

h2_pal_result_t h2_g99_wifi_destroy(h2_g99_wifi_t* wifi) {
  if (wifi == NULL) return H2_PAL_OK;
  if (wifi->task != NULL) {
    lock(wifi);
    wifi->stop = true;
    unlock(wifi);
    (void)h2_pal_semaphore_give(wifi->runtime->sync, wifi->wake);
    const h2_pal_result_t rc =
        h2_pal_task_join(wifi->runtime->task, wifi->task);
    if (rc != H2_PAL_OK) return rc;
    wifi->task = NULL;
  }
  if (wifi->wake != NULL)
    (void)h2_pal_semaphore_destroy(wifi->runtime->sync, wifi->wake);
  if (wifi->mutex != NULL)
    (void)h2_pal_mutex_destroy(wifi->runtime->sync, wifi->mutex);
  h2_pal_mem_free(wifi->runtime->mem, wifi);
  return H2_PAL_OK;
}

static h2_pal_result_t post(h2_g99_wifi_t* wifi, command_t command,
                            const h2_pal_wifi_sta_config_t* config) {
  if (wifi == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (wifi->task == NULL) return H2_PAL_ERR_UNSUPPORTED;
  lock(wifi);
  const bool busy = wifi->snapshot.phase == H2_G99_WIFI_CONNECTING ||
                    wifi->pending == COMMAND_CONNECT;
  if (!busy) {
    wifi->pending = command;
    if (config != NULL) wifi->pending_config = *config;
  }
  unlock(wifi);
  if (busy) return H2_PAL_ERR_BUSY;
  (void)h2_pal_semaphore_give(wifi->runtime->sync, wifi->wake);
  return H2_PAL_OK;
}

h2_pal_result_t h2_g99_wifi_request_scan(h2_g99_wifi_t* wifi) {
  return post(wifi, COMMAND_SCAN, NULL);
}

h2_pal_result_t h2_g99_wifi_request_connect(h2_g99_wifi_t* wifi,
                                            const char* ssid,
                                            const char* password) {
  if (ssid == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_pal_wifi_sta_config_t config = {0};
  const size_t ssid_len = strlen(ssid);
  const size_t password_len = password == NULL ? 0u : strlen(password);
  if (ssid_len == 0u || ssid_len > H2_PAL_WIFI_SSID_MAX ||
      password_len > H2_PAL_WIFI_PASSWORD_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  memcpy(config.ssid, ssid, ssid_len);
  config.ssid_len = ssid_len;
  if (password_len != 0u) memcpy(config.password, password, password_len);
  config.password_len = password_len;
  const h2_pal_result_t rc = post(wifi, COMMAND_CONNECT, &config);
  memset(&config, 0, sizeof(config));
  return rc;
}

void h2_g99_wifi_get(h2_g99_wifi_t* wifi, h2_g99_wifi_snapshot_t* out) {
  if (out == NULL) return;
  if (wifi == NULL) {
    *out = (h2_g99_wifi_snapshot_t){0};
    return;
  }
  if (wifi->mutex == NULL) {
    *out = wifi->snapshot;
    return;
  }
  lock(wifi);
  *out = wifi->snapshot;
  unlock(wifi);
}
