#include "h2_h2loader_web.h"

#include "h2_h2loader_host.h"
#include "h2_h2loader_host_package.h"
#include "h2_web_platform.h"

#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define H2_WEB_JOB_COUNT 24u
#define H2_WEB_JOB_STACK_SIZE (128u * 1024u)
#define H2_WEB_BLOB_SLICE_MAX (64u * 1024u)
#define H2_WEB_JSON_SIZE 12288u
#define H2_WEB_ERROR_DETAIL_SIZE 256u
// reboot-app and the managed install's activate make the loader install the
// staged app (decompress + flash) before acknowledging, so their command read
// needs headroom: read timeout = command_timeout_ms + 30000 = 120s here.
#define H2_WEB_INSTALL_COMMAND_TIMEOUT_MS 90000u

typedef enum h2_web_job_kind {
  H2_WEB_JOB_LIST_PORTS = 1,
  H2_WEB_JOB_INSPECT = 2,
  H2_WEB_JOB_STATUS = 3,
  H2_WEB_JOB_INSTALL = 4,
  H2_WEB_JOB_ROLLBACK = 5,
  H2_WEB_JOB_RESTART = 6,
  H2_WEB_JOB_REBOOT_LOADER = 7,
  H2_WEB_JOB_REBOOT_APP = 8,
  H2_WEB_JOB_STAGE = 9,
} h2_web_job_kind_t;

typedef struct h2_web_job h2_web_job_t;

struct h2_web_job {
  struct h2_h2loader_web_client *client;
  h2_pal_task_t *task;
  h2_h2loader_host_serial_connection_t *connection;
  uint32_t handle;
  h2_web_job_kind_t kind;
  char port_id[H2_PAL_SERIAL_HOST_PORT_ID_MAX_LEN];
  uint32_t blob_handle;
  uint32_t blob_size;
  uint32_t command_timeout_ms;
  uint32_t read_token;
  size_t read_count;
  h2_pal_result_t read_result;
  h2_pal_result_t result;
  h2_h2loader_host_command_result_t command_result;
  h2_h2loader_host_catalog_entry_t asset;
  h2_h2loader_host_status_t status;
  h2_pal_serial_host_port_info_t *ports;
  size_t port_count;
  uint64_t acknowledged;
  uint64_t total;
  h2_h2loader_host_operation_phase_t phase;
  char detail[H2_WEB_ERROR_DETAIL_SIZE];
  int read_pending;
  int started;
  int cancelled;
  int complete;
};

struct h2_h2loader_web_client {
  h2_web_platform_t *platform;
  h2_web_job_t jobs[H2_WEB_JOB_COUNT];
  uint32_t next_handle;
  char authorization_port[H2_PAL_SERIAL_HOST_PORT_ID_MAX_LEN];
  char *json;
  size_t json_capacity;
  h2_pal_result_t shutdown_result;
  int closing;
};

static h2_web_job_t *find_job(h2_h2loader_web_client_t *client,
                              uint32_t handle) {
  if (client == NULL || handle == 0u) return NULL;
  for (size_t index = 0u; index < H2_WEB_JOB_COUNT; ++index) {
    if (client->jobs[index].handle == handle) return &client->jobs[index];
  }
  return NULL;
}

EM_JS(void, h2_web_blob_read_js,
      (uintptr_t job_address, uint32_t job_handle, uint32_t token,
       uint32_t blob_handle, uint32_t offset, uintptr_t out,
       uint32_t out_size), {
        const blobs = Module['h2LoaderWebBlobs'];
        const blob = blobs && blobs.get(blob_handle);
        // The job slot is fixed storage that a later job can reuse, so both
        // outcomes must re-validate the never-reused job handle plus the read
        // token before touching WASM memory or completing a read.
        const live = () => Module['_h2_h2loader_web_blob_read_valid'](
            job_address, job_handle, token);
        const finish = (result, count) => {
          if (!live()) return;
          Module['_h2_h2loader_web_blob_read_complete'](
              job_address, job_handle, token, result, count || 0);
        };
        if (!blob) {
          finish(-8, 0);
          return;
        }
        blob.slice(offset, offset + out_size).arrayBuffer().then(buffer => {
          if (!live()) return;
          const bytes = new Uint8Array(buffer);
          HEAPU8.set(bytes, out);
          finish(0, bytes.byteLength);
        }, () => finish(-4, 0));
      });

EMSCRIPTEN_KEEPALIVE int h2_h2loader_web_blob_read_valid(
    uintptr_t job_address, uint32_t job_handle, uint32_t token) {
  h2_web_job_t *job = (h2_web_job_t *)job_address;
  return job != NULL && job->handle != 0u && job->handle == job_handle &&
         job->read_pending && job->read_token == token && !job->cancelled;
}

EMSCRIPTEN_KEEPALIVE void h2_h2loader_web_blob_read_complete(
    uintptr_t job_address, uint32_t job_handle, uint32_t token, int result,
    size_t count) {
  h2_web_job_t *job = (h2_web_job_t *)job_address;
  if (job == NULL || job->handle == 0u || job->handle != job_handle ||
      !job->read_pending || job->read_token != token) {
    return;
  }
  job->read_result = (h2_pal_result_t)result;
  job->read_count = count;
  job->read_pending = 0;
  if (job->client != NULL && job->client->platform != NULL) {
    h2_web_platform_schedule(job->client->platform);
  }
}

static h2_pal_result_t blob_read(void *user, uint64_t offset, uint8_t *out,
                                 size_t out_size, size_t *out_read) {
  h2_web_job_t *job = user;
  if (job == NULL || out_read == NULL ||
      (out == NULL && out_size != 0u) || out_size > H2_WEB_BLOB_SLICE_MAX ||
      offset > UINT32_MAX || out_size > UINT32_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_read = 0u;
  if (job->cancelled) return H2_PAL_EXIT;
  if (offset >= job->blob_size || out_size == 0u) return H2_PAL_OK;
  if (out_size > job->blob_size - (uint32_t)offset) {
    out_size = job->blob_size - (uint32_t)offset;
  }
  ++job->read_token;
  job->read_result = H2_PAL_ERR_WOULD_BLOCK;
  job->read_count = 0u;
  job->read_pending = 1;
  h2_web_blob_read_js((uintptr_t)job, job->handle, job->read_token,
                      job->blob_handle, (uint32_t)offset, (uintptr_t)out,
                      (uint32_t)out_size);
  while (job->read_pending && !job->cancelled) {
    h2_pal_result_t wait_result = h2_pal_time_sleep_ms(
        h2_web_platform_time_api(job->client->platform), 1u);
    if (wait_result != H2_PAL_OK) {
      job->read_pending = 0;
      return wait_result;
    }
  }
  if (job->cancelled) {
    ++job->read_token;
    job->read_pending = 0;
    return H2_PAL_EXIT;
  }
  if (job->read_result == H2_PAL_OK) *out_read = job->read_count;
  return job->read_result;
}

static int job_cancelled(void *user) {
  return ((h2_web_job_t *)user)->cancelled;
}

static h2_pal_result_t job_connect(void *user,
                                   h2_h2loader_host_status_t *out_status) {
  h2_web_job_t *job = user;
  h2_h2loader_host_serial_connection_config_t config = {
      .serial = h2_web_platform_serial_host_api(job->client->platform),
      .time = h2_web_platform_time_api(job->client->platform),
      .allocator = h2_web_platform_mem_api(),
      .port_id = job->port_id,
      .handshake_timeout_ms = 5000u,
      .command_timeout_ms = job->command_timeout_ms != 0u
          ? job->command_timeout_ms
          : H2_H2LOADER_HOST_DEFAULT_COMMAND_TIMEOUT_MS,
  };
  (void)h2_h2loader_host_serial_disconnect(&job->connection);
  h2_pal_result_t result =
      h2_h2loader_host_serial_connect(&config, &job->connection);
  if (result == H2_PAL_OK) {
    result = h2_h2loader_host_serial_read_status(job->connection, out_status);
  }
  if (result != H2_PAL_OK) {
    (void)h2_h2loader_host_serial_disconnect(&job->connection);
  }
  return result;
}

static h2_pal_result_t job_disconnect(void *user) {
  h2_web_job_t *job = user;
  return h2_h2loader_host_serial_disconnect(&job->connection);
}

static h2_pal_result_t job_stage(
    void *user, const h2_h2loader_host_catalog_entry_t *asset,
    h2_h2loader_host_payload_read_fn read_payload, void *payload_user,
    h2_h2loader_host_cancelled_fn is_cancelled, void *cancel_user,
    h2_h2loader_host_progress_fn on_progress, void *progress_user) {
  h2_web_job_t *job = user;
  return h2_h2loader_host_serial_stage(
      job->connection, asset, read_payload, payload_user, is_cancelled,
      cancel_user, on_progress, progress_user);
}

static h2_pal_result_t job_activate(
    void *user, const h2_h2loader_host_catalog_entry_t *asset) {
  return h2_h2loader_host_serial_activate(
      ((h2_web_job_t *)user)->connection, asset);
}

static h2_pal_result_t job_rediscover(void *user) {
  h2_web_job_t *job = user;
  const h2_pal_serial_host_api_t *serial =
      h2_web_platform_serial_host_api(job->client->platform);
  h2_pal_serial_host_snapshot_t *snapshot = NULL;
  h2_pal_result_t result = h2_pal_serial_host_scan(serial, &snapshot);
  size_t count = 0u;
  if (result == H2_PAL_OK) {
    result = h2_pal_serial_host_snapshot_count(serial, snapshot, &count);
  }
  int found = 0;
  for (size_t index = 0u; result == H2_PAL_OK && index < count; ++index) {
    h2_pal_serial_host_port_info_t info;
    result = h2_pal_serial_host_snapshot_get(serial, snapshot, index, &info);
    if (result == H2_PAL_OK && strcmp(info.port_id, job->port_id) == 0) {
      found = 1;
      break;
    }
  }
  const h2_pal_result_t cleanup =
      h2_pal_serial_host_snapshot_destroy(serial, &snapshot);
  if (result == H2_PAL_OK) result = cleanup;
  return result == H2_PAL_OK && found ? H2_PAL_OK : H2_PAL_ERR_NOT_FOUND;
}

static void job_progress(void *user, uint64_t acknowledged, uint64_t total) {
  h2_web_job_t *job = user;
  job->acknowledged = acknowledged;
  job->total = total;
}

static void job_event(void *user, h2_h2loader_host_operation_phase_t phase,
                      h2_pal_result_t result) {
  h2_web_job_t *job = user;
  (void)result;
  job->phase = phase;
}

static h2_pal_result_t inspect_blob(h2_web_job_t *job) {
  h2_h2loader_host_package_inspect_config_t config = {
      .allocator = h2_web_platform_mem_api(),
      .read_payload = blob_read,
      .payload_user = job,
      .payload_bytes = job->blob_size,
  };
  return h2_h2loader_host_package_inspect(&config, &job->asset);
}

static h2_pal_result_t list_ports(h2_web_job_t *job) {
  const h2_pal_serial_host_api_t *serial =
      h2_web_platform_serial_host_api(job->client->platform);
  h2_pal_serial_host_snapshot_t *snapshot = NULL;
  h2_pal_result_t result = h2_pal_serial_host_scan(serial, &snapshot);
  size_t count = 0u;
  if (result == H2_PAL_OK) {
    result = h2_pal_serial_host_snapshot_count(serial, snapshot, &count);
  }
  if (result == H2_PAL_OK && count > 0u) {
    if (count > SIZE_MAX / sizeof(*job->ports)) {
      result = H2_PAL_ERR_NO_SPACE;
    } else {
      job->ports = calloc(count, sizeof(*job->ports));
      if (job->ports == NULL) result = H2_PAL_ERR_NO_MEMORY;
    }
  }
  for (size_t index = 0u; result == H2_PAL_OK && index < count; ++index) {
    result = h2_pal_serial_host_snapshot_get(serial, snapshot, index,
                                             &job->ports[index]);
  }
  if (result == H2_PAL_OK) job->port_count = count;
  const h2_pal_result_t cleanup =
      h2_pal_serial_host_snapshot_destroy(serial, &snapshot);
  return result == H2_PAL_OK ? cleanup : result;
}

static h2_pal_result_t read_status(h2_web_job_t *job) {
  h2_pal_result_t result = job_connect(job, &job->status);
  const h2_pal_result_t cleanup = job_disconnect(job);
  return result == H2_PAL_OK ? cleanup : result;
}

static h2_pal_result_t run_command(h2_web_job_t *job,
                                   h2_h2loader_host_command_t command) {
  h2_h2loader_host_status_t initial = {0};
  h2_pal_result_t result = job_connect(job, &initial);
  if (result == H2_PAL_OK) {
    h2_h2loader_host_command_request_t request = {
        .command = command,
        .status = &initial,
        .is_cancelled = job_cancelled,
        .cancel_user = job,
    };
    result = h2_h2loader_host_serial_execute_command(
        job->connection, &request, &job->command_result);
  }
  (void)job_disconnect(job);
  if (result == H2_PAL_OK &&
      job->command_result.terminal != H2_H2LOADER_HOST_COMMAND_TERMINAL_OK) {
    result = H2_PAL_ERR_IO;
  }
  if (result == H2_PAL_OK) {
    (void)snprintf(job->detail, sizeof(job->detail),
                   "accepted-unverified");
  }
  return result;
}

static h2_pal_result_t restart_device(h2_web_job_t *job) {
  h2_h2loader_host_status_t initial = {0};
  h2_pal_result_t result = job_connect(job, &initial);
  h2_h2loader_host_command_t command = H2_H2LOADER_HOST_COMMAND_HELP;
  if (result == H2_PAL_OK &&
      initial.active_role == H2_H2LOADER_HOST_ACTIVE_ROLE_APP) {
    command = H2_H2LOADER_HOST_COMMAND_APP_RESTART;
  } else if (result == H2_PAL_OK &&
             initial.active_role == H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER) {
    command = H2_H2LOADER_HOST_COMMAND_LOADER_REBOOT_LOADER;
  } else if (result == H2_PAL_OK) {
    result = H2_PAL_ERR_INVALID_STATE;
  }
  if (result == H2_PAL_OK) {
    h2_h2loader_host_command_request_t request = {
        .command = command,
        .status = &initial,
        .is_cancelled = job_cancelled,
        .cancel_user = job,
    };
    result = h2_h2loader_host_serial_execute_command(
        job->connection, &request, &job->command_result);
  }
  (void)job_disconnect(job);
  if (result == H2_PAL_OK &&
      job->command_result.terminal != H2_H2LOADER_HOST_COMMAND_TERMINAL_OK) {
    result = H2_PAL_ERR_IO;
  }
  if (result == H2_PAL_OK) {
    (void)snprintf(job->detail, sizeof(job->detail),
                   "accepted-unverified");
  }
  return result;
}

static h2_pal_result_t stage_blob(h2_web_job_t *job) {
  static const h2_h2loader_host_managed_transport_vtable_t transport_vtable = {
      .connect = job_connect,
      .stage = job_stage,
      .activate = job_activate,
      .disconnect = job_disconnect,
      .rediscover = job_rediscover,
  };
  h2_pal_result_t result = inspect_blob(job);
  if (result != H2_PAL_OK) return result;
  h2_h2loader_host_managed_operation_config_t config = {
      .time = h2_web_platform_time_api(job->client->platform),
      .transport = {.user = job, .vtable = &transport_vtable},
      .asset = &job->asset,
      .read_payload = blob_read,
      .payload_user = job,
      .is_cancelled = job_cancelled,
      .cancel_user = job,
      .on_progress = job_progress,
      .progress_user = job,
      .on_event = job_event,
      .event_user = job,
      .reconnect_delay_ms = 250u,
      .reconnect_attempts = 20u,
  };
  result = h2_h2loader_host_stage_operation_run(&config, &job->status);
  if (result == H2_PAL_OK) {
    (void)snprintf(job->detail, sizeof(job->detail), "staged");
  }
  return result;
}

static h2_pal_result_t install_blob(h2_web_job_t *job) {
  static const h2_h2loader_host_managed_transport_vtable_t transport_vtable = {
      .connect = job_connect,
      .stage = job_stage,
      .activate = job_activate,
      .disconnect = job_disconnect,
      .rediscover = job_rediscover,
  };
  h2_pal_result_t result = inspect_blob(job);
  h2_h2loader_host_status_t initial = {0};
  if (result == H2_PAL_OK) result = job_connect(job, &initial);
  if (result == H2_PAL_OK &&
      h2_h2loader_host_status_verify_asset(&initial, &job->asset) ==
          H2_PAL_OK) {
    job->status = initial;
    (void)snprintf(job->detail, sizeof(job->detail), "already-target");
    return job_disconnect(job);
  }
  if (result == H2_PAL_OK &&
      (strcmp(initial.board, job->asset.board) != 0 ||
       strcmp(initial.target, job->asset.target) != 0)) {
    result = H2_PAL_ERR_INVALID_STATE;
  }
  if (job->connection != NULL) {
    const h2_pal_result_t cleanup = job_disconnect(job);
    if (result == H2_PAL_OK) result = cleanup;
  }
  if (result != H2_PAL_OK) return result;
  h2_h2loader_host_managed_operation_config_t config = {
      .time = h2_web_platform_time_api(job->client->platform),
      .transport = {.user = job, .vtable = &transport_vtable},
      .asset = &job->asset,
      .read_payload = blob_read,
      .payload_user = job,
      .is_cancelled = job_cancelled,
      .cancel_user = job,
      .on_progress = job_progress,
      .progress_user = job,
      .on_event = job_event,
      .event_user = job,
      .reconnect_delay_ms = 250u,
      .reconnect_attempts = 20u,
  };
  result = h2_h2loader_host_managed_operation_run(&config, &job->status);
  if (result == H2_PAL_OK) {
    (void)snprintf(job->detail, sizeof(job->detail), "verified");
  }
  return result;
}

static void job_entry(void *user) {
  h2_web_job_t *job = user;
  job->started = 1;
  switch (job->kind) {
    case H2_WEB_JOB_LIST_PORTS:
      job->result = list_ports(job);
      break;
    case H2_WEB_JOB_INSPECT:
      job->result = inspect_blob(job);
      break;
    case H2_WEB_JOB_STATUS:
      job->result = read_status(job);
      break;
    case H2_WEB_JOB_INSTALL:
      job->command_timeout_ms = H2_WEB_INSTALL_COMMAND_TIMEOUT_MS;
      job->result = install_blob(job);
      break;
    case H2_WEB_JOB_STAGE:
      job->result = stage_blob(job);
      break;
    case H2_WEB_JOB_ROLLBACK:
      job->result = run_command(
          job, H2_H2LOADER_HOST_COMMAND_APP_ROLLBACK);
      break;
    case H2_WEB_JOB_RESTART:
      job->result = restart_device(job);
      break;
    case H2_WEB_JOB_REBOOT_LOADER:
      job->result = run_command(
          job, H2_H2LOADER_HOST_COMMAND_LOADER_REBOOT_LOADER);
      break;
    case H2_WEB_JOB_REBOOT_APP:
      job->command_timeout_ms = H2_WEB_INSTALL_COMMAND_TIMEOUT_MS;
      job->result = run_command(
          job, H2_H2LOADER_HOST_COMMAND_LOADER_REBOOT_APP);
      break;
    default:
      job->result = H2_PAL_ERR_INVALID_ARG;
      break;
  }
  (void)h2_h2loader_host_serial_disconnect(&job->connection);
  if (job->result != H2_PAL_OK && job->detail[0] == '\0') {
    (void)snprintf(job->detail, sizeof(job->detail), "rc=%d", job->result);
  }
  job->complete = 1;
}

h2_h2loader_web_client_t *h2_h2loader_web_create(void) {
  h2_h2loader_web_client_t *client = calloc(1u, sizeof(*client));
  if (client == NULL) return NULL;
  client->json = calloc(H2_WEB_JSON_SIZE, 1u);
  if (client->json == NULL) {
    free(client);
    return NULL;
  }
  client->json_capacity = H2_WEB_JSON_SIZE;
  const h2_web_platform_config_t config = {1, 1};
  client->platform = h2_web_platform_create(&config);
  if (client->platform == NULL) {
    free(client->json);
    free(client);
    return NULL;
  }
  client->next_handle = 1u;
  return client;
}

int h2_h2loader_web_request_port(h2_h2loader_web_client_t *client) {
  if (client == NULL || client->closing) return H2_PAL_ERR_INVALID_STATE;
  client->authorization_port[0] = '\0';
  return h2_web_platform_serial_request_port(client->platform);
}

int h2_h2loader_web_authorization_result(
    h2_h2loader_web_client_t *client) {
  if (client == NULL || client->closing) return H2_PAL_ERR_INVALID_STATE;
  return h2_web_platform_serial_authorization(
      client->platform, client->authorization_port,
      sizeof(client->authorization_port));
}

const char *h2_h2loader_web_authorization_port(
    h2_h2loader_web_client_t *client) {
  return client == NULL ? NULL : client->authorization_port;
}

int h2_h2loader_web_forget_port(h2_h2loader_web_client_t *client,
                                const char *port_id) {
  if (client == NULL || client->closing) return H2_PAL_ERR_INVALID_STATE;
  return h2_web_platform_serial_forget_port(client->platform, port_id);
}

int h2_h2loader_web_forget_result(h2_h2loader_web_client_t *client) {
  if (client == NULL || client->closing) return H2_PAL_ERR_INVALID_STATE;
  return h2_web_platform_serial_forget_result(client->platform);
}

static uint32_t start_job(h2_h2loader_web_client_t *client,
                          h2_web_job_kind_t kind, const char *port_id,
                          uint32_t blob_handle, uint64_t blob_size) {
  if (client == NULL || client->closing || blob_size > UINT32_MAX ||
      (port_id != NULL &&
       strlen(port_id) >= H2_PAL_SERIAL_HOST_PORT_ID_MAX_LEN)) {
    return 0u;
  }
  h2_web_job_t *job = NULL;
  for (size_t index = 0u; index < H2_WEB_JOB_COUNT; ++index) {
    if (client->jobs[index].handle == 0u) {
      job = &client->jobs[index];
      break;
    }
  }
  if (job == NULL) return 0u;
  memset(job, 0, sizeof(*job));
  job->client = client;
  job->kind = kind;
  job->blob_handle = blob_handle;
  job->blob_size = (uint32_t)blob_size;
  job->phase = H2_H2LOADER_HOST_OPERATION_CONNECT;
  if (port_id != NULL) strcpy(job->port_id, port_id);
  uint32_t handle;
  do {
    handle = client->next_handle++;
  } while (handle == 0u || find_job(client, handle) != NULL);
  job->handle = handle;
  const h2_pal_task_options_t options = {
      .name = "h2loader-web-job",
      .min_stack_size = H2_WEB_JOB_STACK_SIZE,
  };
  h2_pal_result_t result = h2_pal_task_start(
      h2_web_platform_task_api(client->platform), &options, job_entry, job,
      &job->task);
  if (result != H2_PAL_OK) {
    memset(job, 0, sizeof(*job));
    return 0u;
  }
  h2_web_platform_schedule(client->platform);
  return job->handle;
}

uint32_t h2_h2loader_web_list_ports(h2_h2loader_web_client_t *client) {
  return start_job(client, H2_WEB_JOB_LIST_PORTS, NULL, 0u, 0u);
}

uint32_t h2_h2loader_web_inspect_package(
    h2_h2loader_web_client_t *client, uint32_t blob_handle,
    uint32_t blob_size) {
  if (blob_handle == 0u || blob_size == 0u) return 0u;
  return start_job(client, H2_WEB_JOB_INSPECT, NULL, blob_handle, blob_size);
}

uint32_t h2_h2loader_web_status(h2_h2loader_web_client_t *client,
                                const char *port_id) {
  return start_job(client, H2_WEB_JOB_STATUS, port_id, 0u, 0u);
}

uint32_t h2_h2loader_web_install(h2_h2loader_web_client_t *client,
                                 const char *port_id, uint32_t blob_handle,
                                 uint32_t blob_size) {
  if (blob_handle == 0u || blob_size == 0u) return 0u;
  return start_job(client, H2_WEB_JOB_INSTALL, port_id, blob_handle, blob_size);
}

uint32_t h2_h2loader_web_stage(h2_h2loader_web_client_t *client,
                               const char *port_id, uint32_t blob_handle,
                               uint32_t blob_size) {
  if (blob_handle == 0u || blob_size == 0u) return 0u;
  return start_job(client, H2_WEB_JOB_STAGE, port_id, blob_handle, blob_size);
}

uint32_t h2_h2loader_web_rollback(h2_h2loader_web_client_t *client,
                                  const char *port_id) {
  return start_job(client, H2_WEB_JOB_ROLLBACK, port_id, 0u, 0u);
}

uint32_t h2_h2loader_web_restart(h2_h2loader_web_client_t *client,
                                 const char *port_id) {
  return start_job(client, H2_WEB_JOB_RESTART, port_id, 0u, 0u);
}

uint32_t h2_h2loader_web_reboot_loader(h2_h2loader_web_client_t *client,
                                       const char *port_id) {
  return start_job(client, H2_WEB_JOB_REBOOT_LOADER, port_id, 0u, 0u);
}

uint32_t h2_h2loader_web_reboot_app(h2_h2loader_web_client_t *client,
                                    const char *port_id) {
  return start_job(client, H2_WEB_JOB_REBOOT_APP, port_id, 0u, 0u);
}

int h2_h2loader_web_job_done(h2_h2loader_web_client_t *client,
                             uint32_t handle) {
  h2_web_job_t *job = find_job(client, handle);
  return job == NULL ? H2_PAL_ERR_NOT_FOUND : job->complete;
}

int h2_h2loader_web_job_result(h2_h2loader_web_client_t *client,
                               uint32_t handle) {
  h2_web_job_t *job = find_job(client, handle);
  if (job == NULL) return H2_PAL_ERR_NOT_FOUND;
  return job->complete ? job->result : H2_PAL_ERR_WOULD_BLOCK;
}

static size_t append_text(char *out, size_t capacity, size_t offset,
                          const char *text) {
  if (offset >= capacity) return capacity;
  const int count = snprintf(out + offset, capacity - offset, "%s", text);
  if (count < 0 || (size_t)count >= capacity - offset) return capacity;
  return offset + (size_t)count;
}

static size_t append_json_string(char *out, size_t capacity, size_t offset,
                                 const char *text) {
  offset = append_text(out, capacity, offset, "\"");
  for (const unsigned char *cursor = (const unsigned char *)text;
       *cursor != '\0' && offset < capacity; ++cursor) {
    char escaped[7];
    if (*cursor == '"' || *cursor == '\\') {
      escaped[0] = '\\';
      escaped[1] = (char)*cursor;
      escaped[2] = '\0';
    } else if (*cursor < 0x20u) {
      (void)snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor);
    } else {
      escaped[0] = (char)*cursor;
      escaped[1] = '\0';
    }
    offset = append_text(out, capacity, offset, escaped);
  }
  return append_text(out, capacity, offset, "\"");
}

static size_t append_status_json(char *out, size_t capacity, size_t offset,
                                 const h2_h2loader_host_status_t *status) {
  offset = append_text(out, capacity, offset, "{\"board\":");
  offset = append_json_string(out, capacity, offset, status->board);
  offset = append_text(out, capacity, offset, ",\"target\":");
  offset = append_json_string(out, capacity, offset, status->target);
  offset = append_text(out, capacity, offset, ",\"chip\":");
  offset = append_json_string(out, capacity, offset, status->chip);
  offset = append_text(out, capacity, offset, ",\"activeName\":");
  offset = append_json_string(out, capacity, offset, status->active_name);
  offset = append_text(out, capacity, offset, ",\"version\":");
  offset = append_json_string(out, capacity, offset, status->active_version);
  offset = append_text(out, capacity, offset, ",\"checksum\":");
  offset = append_json_string(out, capacity, offset,
                              status->active_checksum);
  offset = append_text(out, capacity, offset, ",\"installedChecksum\":");
  offset = append_json_string(out, capacity, offset,
                              status->installed_checksum);
  offset = append_text(out, capacity, offset, ",\"state\":");
  offset = append_json_string(out, capacity, offset, status->state);
  offset = append_text(out, capacity, offset, ",\"upgradePhase\":");
  offset = append_json_string(out, capacity, offset, status->upgrade_phase);
  char numbers[384];
  (void)snprintf(
      numbers, sizeof(numbers),
      ",\"role\":%u,\"capabilities\":%u,"
      "\"stagedBytes\":%llu,\"stagedValid\":%u,"
      "\"installedValid\":%u,\"appConfirmed\":%u}",
      (unsigned int)status->active_role,
      (unsigned int)status->capabilities,
      (unsigned long long)status->staged_bytes,
      (unsigned int)status->staged_valid,
      (unsigned int)status->installed_valid,
      (unsigned int)status->app_confirmed);
  return append_text(out, capacity, offset, numbers);
}

static h2_pal_result_t ensure_json_capacity(h2_h2loader_web_client_t *client,
                                            size_t capacity) {
  if (client == NULL || capacity == 0u) return H2_PAL_ERR_INVALID_ARG;
  if (capacity <= client->json_capacity) return H2_PAL_OK;
  char *json = realloc(client->json, capacity);
  if (json == NULL) return H2_PAL_ERR_NO_MEMORY;
  client->json = json;
  client->json_capacity = capacity;
  return H2_PAL_OK;
}

const char *h2_h2loader_web_job_json(h2_h2loader_web_client_t *client,
                                     uint32_t handle) {
  h2_web_job_t *job = find_job(client, handle);
  if (client == NULL || job == NULL || !job->complete) return NULL;
  if (job->kind == H2_WEB_JOB_LIST_PORTS && job->port_count > 0u) {
    const size_t port_json_capacity =
        6u * (sizeof(job->ports[0].port_id) +
              sizeof(job->ports[0].display_name)) +
        128u;
    if (job->port_count > (SIZE_MAX - 32u) / port_json_capacity ||
        ensure_json_capacity(
            client, 32u + job->port_count * port_json_capacity) != H2_PAL_OK) {
      return NULL;
    }
  }
  size_t offset = 0u;
  client->json[0] = '\0';
  if (job->kind == H2_WEB_JOB_LIST_PORTS) {
    offset = append_text(client->json, client->json_capacity, offset,
                         "{\"ports\":[");
    for (size_t index = 0u; index < job->port_count; ++index) {
      if (index != 0u) {
        offset = append_text(client->json, client->json_capacity, offset, ",");
      }
      offset = append_text(client->json, client->json_capacity, offset,
                           "{\"id\":");
      offset = append_json_string(client->json, client->json_capacity, offset,
                                  job->ports[index].port_id);
      offset = append_text(client->json, client->json_capacity, offset,
                           ",\"label\":");
      offset = append_json_string(client->json, client->json_capacity, offset,
                                  job->ports[index].display_name);
      char info[192];
      (void)snprintf(info, sizeof(info),
                     ",\"usbVendorId\":%u,\"usbProductId\":%u}",
                     (unsigned int)job->ports[index].usb_vid,
                     (unsigned int)job->ports[index].usb_pid);
      offset = append_text(client->json, client->json_capacity, offset, info);
    }
    (void)append_text(client->json, client->json_capacity, offset, "]}");
    return client->json;
  }
  if (job->kind == H2_WEB_JOB_INSPECT) {
    offset = append_text(client->json, client->json_capacity, offset,
                         "{\"role\":");
    offset = append_json_string(
        client->json, client->json_capacity, offset,
        job->asset.role == H2_H2LOADER_HOST_ASSET_ROLE_APP ? "app" :
                                                             "h2loader");
    offset = append_text(client->json, client->json_capacity, offset,
                         ",\"board\":");
    offset = append_json_string(client->json, client->json_capacity, offset,
                                job->asset.board);
    offset = append_text(client->json, client->json_capacity, offset,
                         ",\"target\":");
    offset = append_json_string(client->json, client->json_capacity, offset,
                                job->asset.target);
    offset = append_text(client->json, client->json_capacity, offset,
                         ",\"version\":");
    offset = append_json_string(client->json, client->json_capacity, offset,
                                job->asset.version);
    offset = append_text(client->json, client->json_capacity, offset,
                         ",\"sha256\":");
    offset = append_json_string(client->json, client->json_capacity, offset,
                                job->asset.sha256);
    char tail[160];
    (void)snprintf(tail, sizeof(tail), ",\"bytes\":%llu}",
                   (unsigned long long)job->asset.bytes);
    (void)append_text(client->json, client->json_capacity, offset, tail);
    return client->json;
  }
  offset = append_text(client->json, client->json_capacity, offset,
                       "{\"detail\":");
  offset = append_json_string(client->json, client->json_capacity, offset,
                              job->detail);
  if (job->kind == H2_WEB_JOB_STATUS || job->kind == H2_WEB_JOB_INSTALL ||
      job->kind == H2_WEB_JOB_STAGE) {
    offset = append_text(client->json, client->json_capacity, offset,
                         ",\"status\":");
    offset = append_status_json(client->json, client->json_capacity, offset,
                                &job->status);
  }
  (void)append_text(client->json, client->json_capacity, offset, "}");
  return client->json;
}

const char *h2_h2loader_web_job_progress_json(
    h2_h2loader_web_client_t *client, uint32_t handle) {
  h2_web_job_t *job = find_job(client, handle);
  if (client == NULL || job == NULL) return NULL;
  (void)snprintf(client->json, client->json_capacity,
                 "{\"phase\":%d,\"acknowledgedBytes\":%llu,"
                 "\"totalBytes\":%llu}",
                 (int)job->phase, (unsigned long long)job->acknowledged,
                 (unsigned long long)job->total);
  return client->json;
}

int h2_h2loader_web_job_cancel(h2_h2loader_web_client_t *client,
                               uint32_t handle) {
  h2_web_job_t *job = find_job(client, handle);
  if (job == NULL) return H2_PAL_ERR_NOT_FOUND;
  job->cancelled = 1;
  if (job->started && job->task != NULL && !job->complete) {
    (void)h2_web_platform_task_cancel(client->platform, job->task);
  }
  return H2_PAL_OK;
}

int h2_h2loader_web_job_release(h2_h2loader_web_client_t *client,
                                uint32_t handle) {
  h2_web_job_t *job = find_job(client, handle);
  if (job == NULL) return H2_PAL_ERR_NOT_FOUND;
  if (!job->complete || job->task == NULL) return H2_PAL_ERR_BUSY;
  h2_pal_result_t result = h2_pal_task_join(
      h2_web_platform_task_api(client->platform), job->task);
  if (result != H2_PAL_OK && result != H2_PAL_EXIT) return result;
  free(job->ports);
  memset(job, 0, sizeof(*job));
  return H2_PAL_OK;
}

int h2_h2loader_web_close_begin(h2_h2loader_web_client_t *client) {
  if (client == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (client->closing) return client->shutdown_result;
  client->closing = 1;
  client->shutdown_result =
      h2_web_platform_serial_shutdown(client->platform);
  for (size_t index = 0u; index < H2_WEB_JOB_COUNT; ++index) {
    h2_web_job_t *job = &client->jobs[index];
    if (job->handle == 0u) continue;
    job->cancelled = 1;
    if (job->started && job->task != NULL && !job->complete) {
      (void)h2_web_platform_task_cancel(client->platform, job->task);
    }
  }
  return client->shutdown_result;
}

int h2_h2loader_web_close_step(h2_h2loader_web_client_t *client) {
  if (client == NULL || !client->closing) return H2_PAL_ERR_INVALID_STATE;
  int pending = 0;
  for (size_t index = 0u; index < H2_WEB_JOB_COUNT; ++index) {
    h2_web_job_t *job = &client->jobs[index];
    if (job->handle == 0u) continue;
    if (!job->complete) {
      pending = 1;
      continue;
    }
    if (job->task != NULL) {
      h2_pal_result_t result = h2_pal_task_join(
          h2_web_platform_task_api(client->platform), job->task);
      if (result != H2_PAL_OK && result != H2_PAL_EXIT) return result;
    }
    free(job->ports);
    memset(job, 0, sizeof(*job));
  }
  if (pending) {
    (void)h2_web_platform_pump(client->platform, 32u, NULL);
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  h2_pal_result_t result = client->shutdown_result;
  h2_web_platform_destroy(client->platform);
  client->platform = NULL;
  free(client->json);
  free(client);
  return result;
}
