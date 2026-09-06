#include "h2_gizclaw_e2e_amoled_ota.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_gizclaw_api_key.h"
#include "h2_gizclaw_e2e_amoled_config.h"
#include "h2_gizclaw_firmware.h"
#include "h2_gizclaw_registration.h"
#include "h2_gizclaw_telemetry.h"
#include "h2_loader_stage.h"
#include "h2_yyjson_json.h"
#include "nvs.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* This lane is selected explicitly at build time. Its isolated registration
 * binds an AMOLED package, never the shared one-byte deploy-default fixture. */
#ifndef H2_GIZCLAW_E2E_OTA_TOKEN
#define H2_GIZCLAW_E2E_OTA_TOKEN ""
#endif
#define OTA_TOKEN H2_GIZCLAW_E2E_OTA_TOKEN
#define OTA_PREF "amoled_ota_e2e"
#define OTA_PATH H2_LOADER_DEFAULT_PACKAGE_PATH

typedef struct ota_lane {
  h2_runtime_t *runtime;
  h2_gizclaw_service_t *service;
  h2_gizclaw_config_t config;
  h2_loader_t loader;
  h2_loader_image_identity_t active;
  h2_pal_fs_file_t *file;
  uint64_t expected_size;
  uint64_t written;
  char expected_sha[65];
  char private_key[65];
  char public_key[65];
  char update_id[97];
  h2_gizclaw_api_key_t api_key;
  atomic_bool activate;
  nvs_handle_t pref;
  bool pref_open;
  char serial[H2_LOADER_DEVICE_UID_MAX];
} ota_lane_t;
static ota_lane_t lane;

static h2_gizclaw_str_t str(const char *text) {
  return (h2_gizclaw_str_t){text, strlen(text)};
}
static void evidence(const char *stage, int rc) {
  printf("H2_AMOLED_OTA stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
}
static int save_text(const char *key, const char *value) {
  esp_err_t rc = nvs_set_str(lane.pref, key, value);
  if (rc == ESP_OK)
    rc = nvs_commit(lane.pref);
  return rc == ESP_OK ? H2_PAL_OK : H2_PAL_ERR_IO;
}
static int load_text(const char *key, char *out, size_t capacity) {
  esp_err_t rc = nvs_get_str(lane.pref, key, out, &capacity);
  return rc == ESP_OK ? H2_PAL_OK : H2_PAL_ERR_NOT_FOUND;
}
static int key_to_base58(const uint8_t *bytes, size_t len, char *out,
                         size_t out_cap) {
  static const char alphabet[] =
      "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
  uint8_t digits[64] = {0};
  size_t zeros = 0, count = 0;
  while (zeros < len && bytes[zeros] == 0)
    ++zeros;
  for (size_t i = zeros; i < len; ++i) {
    unsigned carry = bytes[i];
    for (size_t j = 0; j < count; ++j) {
      unsigned value = digits[j] * 256u + carry;
      digits[j] = value % 58u;
      carry = value / 58u;
    }
    while (carry) {
      if (count == sizeof(digits))
        return H2_PAL_ERR_NO_SPACE;
      digits[count++] = carry % 58u;
      carry /= 58u;
    }
  }
  if (zeros + count >= out_cap)
    return H2_PAL_ERR_NO_SPACE;
  size_t at = 0;
  while (at < zeros)
    out[at++] = '1';
  while (count)
    out[at++] = alphabet[digits[--count]];
  out[at] = 0;
  return H2_PAL_OK;
}
static int identity(void) {
  if (load_text("private", lane.private_key, sizeof(lane.private_key)) == 0 &&
      load_text("public", lane.public_key, sizeof(lane.public_key)) == 0)
    return H2_PAL_OK;
  h2_pal_x25519_keypair_t pair = {0};
  int rc = h2_pal_crypto_x25519_keypair_generate(lane.runtime->crypto, &pair);
  if (!rc)
    rc = key_to_base58(pair.private_key.bytes, 32, lane.private_key,
                       sizeof(lane.private_key));
  if (!rc)
    rc = key_to_base58(pair.public_key.bytes, 32, lane.public_key,
                       sizeof(lane.public_key));
  memset(&pair, 0, sizeof(pair));
  if (!rc)
    rc = save_text("private", lane.private_key);
  if (!rc)
    rc = save_text("public", lane.public_key);
  return rc;
}
static h2_pal_result_t get_facts(void *user, h2_gizclaw_device_facts_t *out) {
  ota_lane_t *state = user;
  *out = (h2_gizclaw_device_facts_t){.has_firmware_sha256 = true};
  memcpy(out->firmware_sha256, state->active.image_sha256, 65);
  return H2_PAL_OK;
}
static h2_pal_result_t begin(void *user, const h2_gizclaw_firmware_t *firmware,
                             const char *update_id) {
  ota_lane_t *state = user;
  if (firmware->size <= 0 || strlen(update_id) >= sizeof(state->update_id))
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_loader_stage_begin(state->runtime->pref);
  if (!rc)
    rc = save_text("update_id", update_id);
  if (!rc)
    rc = h2_pal_fs_open(state->runtime->fs, OTA_PATH,
                        H2_PAL_FS_OPEN_WRITE_TRUNCATE, &state->file);
  if (!rc) {
    state->expected_size = firmware->size;
    state->written = 0;
    memcpy(state->expected_sha, firmware->sha256, 65);
    strcpy(state->update_id, update_id);
  }
  printf("H2_AMOLED_OTA stage=begin rc=%d update_id=%s bytes=%lld\n", rc,
         update_id, (long long)firmware->size);
  return rc;
}
static h2_pal_result_t write_bytes(void *user, const uint8_t *data,
                                   size_t length) {
  ota_lane_t *state = user;
  size_t written = 0;
  if (length > state->expected_size - state->written)
    return H2_PAL_ERR_FORMAT;
  int rc =
      h2_pal_fs_write(state->runtime->fs, state->file, data, length, &written);
  if (!rc && written != length)
    rc = H2_PAL_ERR_IO;
  if (!rc)
    state->written += written;
  return rc;
}
static h2_pal_result_t finish(void *user) {
  ota_lane_t *state = user;
  int rc = h2_pal_fs_sync(state->runtime->fs, state->file);
  int close_rc = h2_pal_fs_close(state->runtime->fs, state->file);
  state->file = NULL;
  if (!rc)
    rc = close_rc;
  if (!rc && state->written != state->expected_size)
    rc = H2_PAL_ERR_FORMAT;
  if (!rc)
    rc = h2_loader_package_verify_path(&state->loader.package, OTA_PATH,
                                       state->expected_size,
                                       state->expected_sha);
  h2_loader_package_inspection_t inspection = {0};
  if (!rc)
    rc = h2_loader_package_inspect_path(&state->loader.package, OTA_PATH,
                                        &inspection);
  if (!rc && (inspection.legacy ||
              inspection.manifest.role != H2_LOADER_IMAGE_ROLE_APP ||
              strcmp(inspection.manifest.board, "amoled") ||
              strcmp(inspection.manifest.target, "esp32s3")))
    rc = H2_PAL_ERR_FORMAT;
  if (!rc)
    rc = save_text("target_sha", inspection.manifest.image_sha256);
  if (!rc)
    rc = save_text("target_version", inspection.manifest.version);
  if (!rc)
    rc = h2_loader_stage_commit_inspection(
        state->runtime->pref, state->expected_size, state->expected_sha,
        &inspection, NULL);
  printf("H2_AMOLED_OTA stage=verified rc=%d bytes=%llu target=%s sha256=%s\n",
         rc, (unsigned long long)state->written, inspection.manifest.version,
         inspection.manifest.image_sha256);
  return rc;
}
static void abort_stage(void *user) {
  ota_lane_t *state = user;
  if (state->file) {
    (void)h2_pal_fs_close(state->runtime->fs, state->file);
    state->file = NULL;
  }
  evidence("abort", h2_loader_stage_abort(state->runtime->fs,
                                          state->runtime->pref, OTA_PATH));
}
static h2_pal_result_t activate(void *user) {
  ota_lane_t *state = user;
  atomic_store_explicit(&state->activate, true, memory_order_release);
  return H2_PAL_OK;
}
static const h2_gizclaw_vtable_t vtable = {
    .get_facts = get_facts,
    .ota_begin = begin,
    .ota_write = write_bytes,
    .ota_finish = finish,
    .ota_abort = abort_stage,
    .ota_activate = activate,
};
static int http_call(bool update, bool *persisted) {
  if (persisted)
    *persisted = false;
  const char *url = update
                        ? "https://ap.e2e.gizclaw.com/gizclaw/v1/device/"
                          "actions/firmware-update"
                        : "https://ap.e2e.gizclaw.com/gizclaw/v1/device/status";
  const char *body = "{\"channel\":\"develop\"}";
  char authorization[112];
  snprintf(authorization, sizeof(authorization), "Bearer %s",
           lane.api_key.secret);
  const h2_pal_http_header_t headers[] = {
      {{"Authorization", 13}, {authorization, strlen(authorization)}},
      {{"Content-Type", 12}, {"application/json", 16}},
  };
  uint8_t buffer[8192];
  h2_pal_http_request_t request = {
      .method = update ? H2_PAL_HTTP_POST : H2_PAL_HTTP_GET,
      .url = {url, strlen(url)},
      .headers = headers,
      .header_count = 2,
      .timeout_ms = 30000,
      .body = update ? (const uint8_t *)body : NULL,
      .body_len = update ? strlen(body) : 0,
      .response_buf = buffer,
      .response_buf_cap = sizeof(buffer),
      .allocator = lane.runtime->mem,
  };
  h2_pal_http_response_t response = {0};
  int rc = h2_pal_http_request(lane.runtime->http, &request, &response);
  printf("H2_AMOLED_OTA stage=%s http=%d rc=%d\n",
         update ? "client.firmware.update" : "status_read",
         response.status_code, rc);
  if (!rc && response.status_code != (update ? 204 : 200))
    rc = H2_PAL_ERR_INVALID_STATE;
  if (!rc && !update) {
    h2_yyjson_json_t *provider = NULL;
    h2_pal_json_document_t *document = NULL;
    rc = h2_yyjson_json_create(lane.runtime->mem, &provider);
    const h2_pal_json_api_t *json =
        provider ? h2_yyjson_json_api(provider) : NULL;
    if (!rc)
      rc = h2_pal_json_document_parse(json, response.body, response.body_len,
                                      NULL, &document);
    h2_pal_json_value_t *root = NULL, *ota = NULL, *value = NULL;
    h2_pal_json_string_view_t state = {0}, id = {0}, version = {0};
    double percent = 0;
    if (!rc)
      rc = h2_pal_json_document_root(json, document, &root);
    if (!rc &&
        h2_pal_json_object_get(json, root, "ota", 3, &ota) == H2_PAL_OK &&
        ota) {
      if (h2_pal_json_object_get(json, ota, "state", 5, &value) == H2_PAL_OK)
        (void)h2_pal_json_value_get_string(json, value, &state);
      if (h2_pal_json_object_get(json, ota, "update_id", 9, &value) ==
          H2_PAL_OK)
        (void)h2_pal_json_value_get_string(json, value, &id);
      if (h2_pal_json_object_get(json, ota, "target_version", 14, &value) ==
          H2_PAL_OK)
        (void)h2_pal_json_value_get_string(json, value, &version);
      if (h2_pal_json_object_get(json, ota, "download_percent", 16, &value) ==
          H2_PAL_OK)
        (void)h2_pal_json_value_get_number(json, value, &percent);
      if (state.len <= 32 && id.len <= 128 && version.len <= 96) {
        printf("H2_AMOLED_OTA stage=persisted state=%.*s update_id=%.*s "
               "target=%.*s percent=%.2f\n",
               (int)state.len, state.data ? state.data : "", (int)id.len,
               id.data ? id.data : "", (int)version.len,
               version.data ? version.data : "", percent);
        if (persisted && state.len == 9 &&
            !memcmp(state.data, "succeeded", 9) &&
            id.len == strlen(lane.update_id) &&
            !memcmp(id.data, lane.update_id, id.len) &&
            version.len == strlen(lane.active.version) &&
            !memcmp(version.data, lane.active.version, version.len) &&
            percent == 100)
          *persisted = true;
      }
      if (h2_pal_json_object_get(json, ota, "error_code", 10, &value) ==
          H2_PAL_OK) {
        h2_pal_json_string_view_t error = {0};
        if (h2_pal_json_value_get_string(json, value, &error) == H2_PAL_OK &&
            error.len <= 32)
          printf("H2_AMOLED_OTA stage=persisted_error code=%.*s\n",
                 (int)error.len, error.data);
      }
    }
    if (document)
      (void)h2_pal_json_document_destroy(json, &document);
    if (provider)
      h2_yyjson_json_destroy(&provider);
  }
  h2_pal_http_response_free(lane.runtime->http, &response);
  memset(authorization, 0, sizeof(authorization));
  return rc;
}
static int report_success(void) {
  char target[96], sha[65];
  if (load_text("update_id", lane.update_id, sizeof(lane.update_id)) ||
      load_text("target_sha", sha, sizeof(sha)) ||
      load_text("target_version", target, sizeof(target)))
    return H2_PAL_ERR_NOT_FOUND;
  if (strcmp(sha, lane.active.image_sha256) ||
      strcmp(target, lane.active.version))
    return H2_PAL_ERR_INVALID_STATE;
  h2_loader_status_t status = {0};
  int rc = h2_loader_read_status(&lane.loader, &status);
  if (!rc && (status.stage.valid || !status.partition_2.valid))
    rc = H2_PAL_ERR_INVALID_STATE;
  uint64_t wall = 0;
  if (!rc)
    rc = h2_pal_time_get_wall_ms(lane.runtime->time, &wall);
  const h2_gizclaw_telemetry_observation_t observation = {
      .kind = H2_GIZCLAW_TELEMETRY_OTA,
      .value.ota = {.state = H2_GIZCLAW_OTA_STATE_SUCCEEDED,
                    .update_id = str(lane.update_id),
                    .has_target_version = true,
                    .target_version = str(target),
                    .has_download_percent = true,
                    .download_percent = 100},
  };
  const h2_gizclaw_telemetry_frame_t frame = {
      .sequence = 1,
      .observed_at_unix_ms = (int64_t)wall,
      .observations = &observation,
      .observation_count = 1,
  };
  if (!rc)
    rc = h2_gizclaw_rpc_telemetry_send(lane.service, &frame, 10000);
  printf("H2_AMOLED_OTA stage=postboot_identity rc=%d version=%s sha256=%s "
         "update_id=%s stage_valid=%d\n",
         rc, lane.active.version, lane.active.image_sha256, lane.update_id,
         status.stage.valid);
  return rc;
}
static int run(void) {
  h2_runtime_t *runtime = lane.runtime;
  if (!OTA_TOKEN[0])
    return H2_PAL_ERR_INVALID_ARG;
  if (nvs_open(OTA_PREF, NVS_READWRITE, &lane.pref) != ESP_OK)
    return H2_PAL_ERR_IO;
  lane.pref_open = true;
  int rc = identity();
  if (rc)
    return rc;
  h2_pal_firmware_info_t info;
  rc = h2_pal_firmware_info_get_current(runtime->firmware_info, &info);
  if (!rc)
    rc = h2_esp_h2loader_current_image_identity(H2_LOADER_IMAGE_ROLE_APP,
                                                "amoled", "esp32s3",
                                                info.version, &lane.active);
  if (rc)
    return rc;
  const h2_loader_config_t loader_config = {
      .package = {.fs = runtime->fs,
                  .disk = runtime->disk,
                  .allocator = runtime->mem,
                  .digest = h2_esp_h2loader_digest_api()},
      .pref = runtime->pref,
      .power = runtime->power,
      .board = "amoled",
      .target = "esp32s3",
      .chip = "esp32s3",
      .hardware_capabilities = 7,
      .active_identity = lane.active,
      .h2loader_partition_id = 1,
      .app_partition_id = 2,
  };
  rc = h2_loader_init(&lane.loader, &loader_config);
  if (rc)
    return rc;
  h2_loader_status_t device_status = {0};
  rc = h2_loader_read_status(&lane.loader, &device_status);
  if (rc)
    return rc;
  memcpy(lane.serial, device_status.device_uid, sizeof(lane.serial));
  lane.config = (h2_gizclaw_config_t){
      .server_endpoint = h2_gizclaw_e2e_amoled_config()->server_endpoint,
      .private_key = str(lane.private_key),
      .cipher_mode = H2_GIZCLAW_CIPHER_CHACHA20_POLY1305,
      .connect_timeout_ms = 180000,
      .write_timeout_ms = 30000,
      .allocator = runtime->mem,
      .http = runtime->http,
      .webrtc = runtime->webrtc,
      .crypto = runtime->crypto,
      .time = runtime->time,
      .log = runtime->log,
      .vtable = &vtable,
      .user = &lane,
      .model = "amoled-ota-e2e",
      .serial = lane.serial,
      .firmware_channel = H2_GIZCLAW_FIRMWARE_CHANNEL_DEVELOP,
  };
  const h2_gizclaw_service_config_t service_config = {
      .client_config = &lane.config,
      .task = runtime->task,
      .queue = runtime->queue,
      .sync = runtime->sync,
      .net_task_options = {.min_stack_size = 32768},
      .operation_capacity = 8,
      .client_poll_timeout_ms = 1,
  };
  rc = h2_gizclaw_service_init(&service_config, &lane.service);
  if (!rc)
    rc = h2_gizclaw_service_start(lane.service);
  h2_gizclaw_registration_result_t registration;
  if (!rc)
    rc = h2_gizclaw_rpc_register(lane.service, OTA_TOKEN, 30000, &registration);
  printf("H2_AMOLED_OTA stage=registered rc=%d peer=%s version=%s\n", rc,
         lane.public_key, info.version);
  if (rc)
    return rc;
  char previous_key[27] = {0};
  if (load_text("api_name", previous_key, sizeof(previous_key)) == H2_PAL_OK &&
      previous_key[0]) {
    int revoke_rc =
        h2_gizclaw_rpc_api_key_revoke(lane.service, str(previous_key), 15000);
    evidence("previous_api_key_revoked", revoke_rc);
    if (revoke_rc && revoke_rc != H2_PAL_ERR_NOT_FOUND)
      return revoke_rc;
  }
  rc = h2_gizclaw_rpc_api_key_create(lane.service, str("amoled-ota-e2e"), false,
                                     15000, &lane.api_key);
  if (!rc)
    rc = save_text("api_name", lane.api_key.name);
  if (rc)
    return rc;
  if (strstr(info.version, "source")) {
    if (!rc)
      rc = http_call(false, NULL);
    if (!rc)
      rc = http_call(true, NULL);
    if (rc)
      return rc;
  } else {
    rc = report_success();
    if (rc)
      return rc;
    bool persisted = false;
    for (unsigned attempt = 0; attempt < 30 && !persisted; ++attempt) {
      (void)h2_pal_time_sleep_ms(runtime->time, 1000);
      rc = http_call(false, &persisted);
      if (rc)
        return rc;
    }
    if (!persisted)
      return H2_PAL_ERR_TIMEOUT;
    evidence("FULL_CHAIN_PASS", H2_PAL_OK);
    rc = h2_gizclaw_rpc_api_key_revoke(lane.service, str(lane.api_key.name),
                                       15000);
    evidence("api_key_revoked", rc);
    if (!rc)
      rc = save_text("api_name", "");
    memset(&lane.api_key, 0, sizeof(lane.api_key));
    if (rc)
      return rc;
  }
  for (unsigned seconds = 0;; ++seconds) {
    size_t dispatched = 0;
    rc = h2_gizclaw_service_poll(lane.service, 8, &dispatched);
    if (rc)
      return rc;
    if (atomic_load_explicit(&lane.activate, memory_order_acquire)) {
      /* The RPC owner has already sent its reply. Stop and join the Service
       * before H2Loader changes the boot selection and reboots. */
      rc = h2_gizclaw_service_stop(lane.service);
      evidence("service_stopped", rc);
      if (!rc)
        rc = h2_loader_reboot_upgrade_with_transition(&lane.loader, NULL, NULL);
      return rc;
    }
    if (seconds % 10 == 0)
      evidence("online", H2_PAL_OK);
    if (strstr(info.version, "source") && seconds % 5 == 0) {
      int status_rc = http_call(false, NULL);
      if (status_rc)
        evidence("status_retry", status_rc);
    }
    rc = h2_pal_time_sleep_ms(runtime->time, 1000);
    if (rc)
      return rc;
  }
}
void h2_gizclaw_e2e_amoled_ota_run(h2_runtime_t *runtime) {
  lane.runtime = runtime;
  atomic_init(&lane.activate, false);
  int rc = run();
  /* A failed acceptance must not keep its network worker or API key alive.
   * Retain NVS until stop succeeds because Stage callbacks borrow it. */
  if (lane.service) {
    if (lane.api_key.name[0]) {
      int revoke_rc = h2_gizclaw_rpc_api_key_revoke(
          lane.service, str(lane.api_key.name), 15000);
      evidence("failure_api_key_revoked", revoke_rc);
      if (!revoke_rc && lane.pref_open)
        (void)save_text("api_name", "");
    }
    int stop_rc;
    do {
      stop_rc = h2_gizclaw_service_stop(lane.service);
      if (stop_rc) {
        evidence("cleanup_retry", stop_rc);
        (void)h2_pal_time_sleep_ms(runtime->time, 1000);
      }
    } while (stop_rc);
    size_t dispatched = 0;
    do {
      dispatched = 0;
      int poll_rc = h2_gizclaw_service_poll(lane.service, 8, &dispatched);
      if (poll_rc) {
        evidence("cleanup_poll", poll_rc);
        break;
      }
    } while (dispatched);
    evidence("service_deinit", h2_gizclaw_service_deinit(lane.service));
  }
  if (lane.file)
    abort_stage(&lane);
  if (lane.pref_open)
    nvs_close(lane.pref);
  memset(&lane.api_key, 0, sizeof(lane.api_key));
  for (;;) {
    evidence("terminal", rc);
    (void)h2_pal_time_sleep_ms(runtime->time, 10000);
  }
}
