#include "h2_esp_board_private.h"

#include "h2_esp_board_config.h"
#include "h2_esp_platform_core.h"

#include "esp_partition.h"

int h2_esp_board_h2loader_fs_init(h2_pal_fs_api_t *fs);
int h2_esp_board_h2loader_fs_deinit(void);

int h2_esp_board_fs_init(h2_pal_fs_api_t *fs) {
    uint8_t partition_subtype;
    const h2_esp_platform_spiffs_config_t config = {
        .base_path = H2_ESP_BOARD_FS_BASE_PATH,
        .partition_label = H2_ESP_BOARD_FS_PARTITION_LABEL,
        .max_files = 16u,
        .format_if_mount_failed = true,
    };

    h2_pal_result_t rc = h2_esp_platform_data_partition_subtype(
        H2_ESP_BOARD_FS_PARTITION_LABEL, &partition_subtype);
    if (rc == H2_PAL_OK) {
        return h2_esp_platform_spiffs_fs_init(fs, &config);
    }
    if (rc != H2_PAL_ERR_NOT_FOUND) {
        return rc;
    }
    return h2_esp_board_h2loader_fs_init(fs);
}

int h2_esp_board_fs_deinit(void) {
    int legacy_rc = h2_esp_platform_spiffs_fs_deinit(H2_ESP_BOARD_FS_PARTITION_LABEL);
    int h2loader_rc = h2_esp_board_h2loader_fs_deinit();

    if (legacy_rc == H2_PAL_FS_OK || h2loader_rc == H2_PAL_FS_OK) {
        return H2_PAL_FS_OK;
    }
    return h2loader_rc;
}

h2_pal_mem_api_t *h2_esp_board_default_allocator(void) {
    return h2_esp_platform_default_allocator();
}

h2_pal_mem_api_t *h2_esp_board_psram_allocator(void) {
    return h2_esp_platform_psram_allocator();
}

h2_pal_mem_api_t *h2_esp_board_internal_allocator(void) {
    return h2_esp_platform_internal_allocator();
}

h2_pal_mem_api_t *h2_esp_board_dma_allocator(void) {
    return h2_esp_platform_dma_allocator();
}

const h2_pal_log_api_t *h2_esp_board_log_api(void) {
    return h2_esp_platform_log_api();
}

const h2_pal_sync_api_t *h2_esp_board_sync_api(void) {
    return h2_esp_platform_sync_api();
}

const h2_pal_task_api_t *h2_esp_board_task_api(void) {
    return h2_esp_platform_task_api();
}

const h2_pal_queue_api_t *h2_esp_board_queue_api(void) {
    return h2_esp_platform_queue_api();
}

const h2_pal_time_api_t *h2_esp_board_time_api(void) {
    return h2_esp_platform_time_api();
}

const h2_pal_system_event_api_t *h2_esp_board_system_event_api(void) {
    return h2_esp_platform_system_event_api();
}

const h2_pal_crypto_api_t *h2_esp_board_crypto_api(void) {
    return h2_esp_platform_crypto_api();
}

const h2_pal_power_api_t *h2_esp_board_power_api(void) {
    return h2_esp_platform_power_api();
}

const h2_pal_disk_api_t *h2_esp_board_disk_api(void) {
    return h2_esp_platform_disk_api();
}

const h2_pal_pref_api_t *h2_esp_board_pref_api(void) {
    return h2_esp_platform_pref_api();
}

const h2_pal_webrtc_api_t *h2_esp_board_webrtc_api(void) {
    return h2_esp_platform_webrtc_api();
}

h2_pal_ble_t *h2_esp_board_ble(void) {
    return h2_esp_platform_ble();
}

h2_pal_wifi_sta_t *h2_esp_board_wifi_sta(void) {
    return h2_esp_platform_wifi_sta();
}

h2_pal_wifi_ap_t *h2_esp_board_wifi_ap(void) {
    return h2_esp_platform_wifi_ap();
}

h2_pal_wifi_settings_t *h2_esp_board_wifi_settings(void) {
    return h2_esp_platform_wifi_settings();
}

h2_pal_modem_t *h2_esp_board_modem(void) {
    return h2_esp_platform_modem_unsupported();
}
