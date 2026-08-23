#include "h2_esp_platform_pref_migration.h"

#include "h2_esp_platform_littlefs_io.h"
#include "h2_esp_platform_pref_nvs_legacy.h"
#include "h2_esp_platform_safe_call.h"

#include "esp_attr.h"
#include "esp_ota_ops.h"

#include <string.h>

#define H2_ESP_PREF_MIGRATION_MARKER "migration"
#define H2_ESP_PREF_MIGRATION_SAFE_STACK_DEPTH 4096u

typedef struct h2_esp_pref_migration_app_state_call {
    int allows_cleanup;
} h2_esp_pref_migration_app_state_call_t;

static h2_esp_pref_store_t *s_migration_store;

static void IRAM_ATTR running_app_allows_cleanup_safe(void *context) {
    h2_esp_pref_migration_app_state_call_t *call =
        (h2_esp_pref_migration_app_state_call_t *)context;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    esp_err_t error;
    call->allows_cleanup = 0;
    if (running == NULL || strcmp(running->label, "h2loader") == 0) return;
    error = esp_ota_get_state_partition(running, &state);
    if (error == ESP_ERR_NOT_FOUND) {
        call->allows_cleanup = 1;
        return;
    }
    if (error == ESP_OK) {
        call->allows_cleanup = state != ESP_OTA_IMG_PENDING_VERIFY &&
                               state != ESP_OTA_IMG_NEW;
    }
}

static int running_app_allows_cleanup(void) {
    h2_esp_pref_migration_app_state_call_t call = {0};
    int rc = h2_esp_platform_safe_call(
        running_app_allows_cleanup_safe, &call, sizeof(call),
        H2_ESP_PREF_MIGRATION_SAFE_STACK_DEPTH);
    return rc == H2_PAL_OK && call.allows_cleanup;
}

int h2_esp_pref_migration_finalize(void) {
    char phase[16];
    int rc;
    if (s_migration_store == NULL) return H2_PAL_ERR_INVALID_STATE;
    rc = h2_esp_pref_io_read_marker(s_migration_store,
                                    H2_ESP_PREF_MIGRATION_MARKER,
                                    phase, sizeof(phase));
    if (rc != H2_PAL_OK) return rc;
    if (strcmp(phase, "complete") == 0) return H2_PAL_OK;
    if (strcmp(phase, "copied") != 0) return H2_PAL_ERR_IO;
    rc = h2_esp_pref_legacy_cleanup();
    if (rc != H2_PAL_OK) return rc;
    return h2_esp_pref_io_write_marker(s_migration_store,
                                       H2_ESP_PREF_MIGRATION_MARKER,
                                       "complete");
}

int h2_esp_pref_migration_prepare(h2_esp_pref_store_t *store) {
    char phase[16];
    int rc;
    if (store == NULL) return H2_PAL_ERR_INVALID_ARG;
    s_migration_store = store;
    rc = h2_esp_pref_io_read_marker(store, H2_ESP_PREF_MIGRATION_MARKER,
                                    phase, sizeof(phase));
    if (rc == H2_PAL_OK && strcmp(phase, "complete") == 0) return H2_PAL_OK;
    if (rc == H2_PAL_OK && strcmp(phase, "copied") != 0) return H2_PAL_ERR_IO;
    if (rc == H2_PAL_ERR_NOT_FOUND) {
        rc = h2_esp_pref_legacy_copy(store);
        if (rc != H2_PAL_OK) return rc;
        rc = h2_esp_pref_io_write_marker(store,
                                         H2_ESP_PREF_MIGRATION_MARKER,
                                         "copied");
        if (rc != H2_PAL_OK) return rc;
    } else if (rc != H2_PAL_OK) {
        return rc;
    }
    return running_app_allows_cleanup()
        ? h2_esp_pref_migration_finalize()
        : H2_PAL_OK;
}
