#include "asm/includes.h"

#include "jieli_h2loader_app_support.h"

#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_ac791n_devkit_partitions.h"
#include "h2_jieli_wl82_platform_core.h"
#include "h2_loader_boot.h"
#include "h2loader_sha256.h"
#include "h2/pal/h2_pal_unsupported.h"
#include "os/os_api.h"
#include "system/timer.h"

#include <string.h>

extern int snprintf(char *buffer, size_t size, const char *format, ...);

typedef struct h2_jieli_app_digest {
  h2_jieli_sha256_t sha;
  int active;
} h2_jieli_app_digest_t;

static h2_jieli_app_digest_t digest;
static h2_pal_mutex_t *operation_mutex;

static int digest_start(void *user) {
  h2_jieli_app_digest_t *self = user;
  h2_jieli_sha256_init(&self->sha);
  self->active = 1;
  return H2_PAL_OK;
}

static int digest_update(void *user, const uint8_t *data, size_t len) {
  h2_jieli_app_digest_t *self = user;
  if (!self->active || (data == NULL && len != 0u)) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  h2_jieli_sha256_update(&self->sha, data, len);
  return H2_PAL_OK;
}

static int digest_finish(void *user, uint8_t out[32]) {
  h2_jieli_app_digest_t *self = user;
  if (!self->active || out == NULL) return H2_PAL_ERR_INVALID_STATE;
  h2_jieli_sha256_finish(&self->sha, out);
  self->active = 0;
  return H2_PAL_OK;
}

static void digest_abort(void *user) {
  memset(user, 0, sizeof(h2_jieli_app_digest_t));
}

static uint64_t now_ms(void *user) {
  (void)user;
  return timer_get_ms();
}

static void sleep_ms(void *user, uint32_t delay_ms) {
  (void)user;
  os_time_dly((delay_ms + 9u) / 10u);
}

static int active_identity(h2_loader_image_identity_t *out_identity) {
  h2_loader_status_t status;
  int rc;
  if (out_identity == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_identity, 0, sizeof(*out_identity));
  rc = h2_loader_read_pref_status(
      h2_jieli_ac791n_devkit_pref_api(),
      h2_jieli_wl82_platform_mem_api(), &status);
  if (rc != H2_PAL_OK) return rc;
  if (!status.partition_2.valid ||
      status.partition_2.role != H2_LOADER_IMAGE_ROLE_APP) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  out_identity->format = 1u;
  out_identity->role = status.partition_2.role;
  out_identity->image_size = status.partition_2.image_size;
  (void)snprintf(out_identity->board, sizeof(out_identity->board), "%s",
                 status.partition_2.board);
  (void)snprintf(out_identity->target, sizeof(out_identity->target), "%s",
                 status.partition_2.target);
  (void)snprintf(out_identity->version, sizeof(out_identity->version), "%s",
                 status.partition_2.version);
  (void)snprintf(out_identity->image_sha256,
                 sizeof(out_identity->image_sha256), "%s",
                 status.partition_2.image_checksum);
  return H2_PAL_OK;
}

int h2_jieli_app_loader_config_init(
    h2_loader_app_client_config_t *out_config,
    h2_pal_fs_api_t *fs,
    const h2_pal_power_api_t *power,
    h2_loader_memory_stats_api_t memory_stats,
    uint32_t hardware_capabilities) {
  if (out_config == NULL || fs == NULL || power == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_loader_image_identity_t identity;
  int rc = active_identity(&identity);
  if (rc != H2_PAL_OK) return rc;
  if (operation_mutex == NULL) {
    const h2_pal_mutex_config_t mutex_config = {
        .name = "h2loader-app-operation",
        .allocator = h2_jieli_wl82_platform_mem_api(),
        .flags = H2_PAL_MUTEX_FLAG_RECURSIVE,
    };
    rc = h2_pal_mutex_create(h2_jieli_wl82_platform_sync_api(),
                             &mutex_config, &operation_mutex);
    if (rc != H2_PAL_OK) return rc;
  }
  memset(&digest, 0, sizeof(digest));
  *out_config = (h2_loader_app_client_config_t){
      .pref = h2_jieli_ac791n_devkit_pref_api(),
      .operation_sync = h2_jieli_wl82_platform_sync_api(),
      .operation_mutex = operation_mutex,
      .power = power,
      .allocator = h2_jieli_wl82_platform_mem_api(),
      .disk = h2_jieli_ac791n_devkit_disk_api(),
      .fs = fs,
      .http = h2_pal_unsupported_http_api(),
      .wifi = h2_jieli_ac791n_devkit_wifi_sta_api(),
      .wifi_settings = h2_jieli_ac791n_devkit_wifi_settings_api(),
      .digest = {
          .user = &digest,
          .start = digest_start,
          .update = digest_update,
          .finish = digest_finish,
          .abort = digest_abort,
      },
      .board = "jieli_ac791n_devkit",
      .target = "wl82",
      .chip = "ac791n",
      .device_uid = h2_jieli_ac791n_devkit_device_uid(),
      .app_entry_path = H2_JIELI_APP_ENTRY_PATH,
      .active_identity = identity,
      .hardware_capabilities = hardware_capabilities,
      .h2loader_partition_id = H2_JIELI_PARTITION_LOADER,
      .app_partition_id = H2_JIELI_PARTITION_APP,
      .coredump_partition_id = H2_JIELI_PARTITION_COREDUMP,
      .now_ms = now_ms,
      .sleep_ms = sleep_ms,
      .memory_stats = memory_stats,
  };
  return H2_PAL_OK;
}

int h2_jieli_app_loader_confirm(
    const h2_loader_app_client_config_t *config) {
  if (config == NULL) return H2_PAL_ERR_INVALID_ARG;
  return h2_loader_finalize_active_app_with_confirmation(
      config->pref, config->allocator, config->fs,
      H2_LOADER_DEFAULT_PACKAGE_PATH, &config->active_identity,
      H2_JIELI_PARTITION_APP, H2_JIELI_PARTITION_APP, NULL, NULL);
}
