#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"
#include "h2_loader_boot.h"
#include "h2_smoke_ble_wifi_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>

static void image_entry(void *user) {
    (void)user;
    h2_runtime_config_t runtime_config = { 0 };
    h2_runtime_t *runtime = NULL;
    int rc = h2_esp_board_runtime_config(&runtime_config);
    if (rc == H2_PAL_OK) {
        rc = h2_esp_h2loader_app_commands_prepare_serial(
            &runtime_config, "ble-wifi-config", 1u, 3u);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_runtime_init(&runtime_config, &runtime);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_esp_h2loader_app_commands_start(
            runtime, "ble-wifi-config", 1u, 3u);
    }
    /*
     * Confirm before opening the window, not after: provisioning waits for a
     * phone that may never arrive, and an unconfirmed image would roll back
     * while the operator is still looking for the device.
     */
    if (rc == H2_PAL_OK) {
        printf("H2_ESP_SMOKE_BLE_WIFI_CONFIG_CONFIRM stage=begin\n");
        fflush(stdout);
        rc = h2_esp_h2loader_app_confirm(runtime);
        printf("H2_ESP_SMOKE_BLE_WIFI_CONFIG_CONFIRM stage=done rc=%d\n", rc);
        fflush(stdout);
    }
    /*
     * The BLE Host advertises one legacy payload at a time and the H2Loader
     * App command service owns it, so the provisioning window borrows it.
     *
     * Joining that advertisement instead does not fit: legacy advertising data
     * caps at 31 bytes and two 128-bit service UUIDs alone are 34. Pausing is
     * also the honest reading of the window — while it is open, provisioning
     * is what the device is for, and the phone filters on the provisioning
     * service UUID. Serial keeps reaching the loader throughout; only its BLE
     * command transport is unavailable, and it comes back afterwards.
     */
    if (rc == H2_PAL_OK) {
        int pause_rc = h2_esp_h2loader_app_commands_pause_ble_advertising();
        printf("H2_ESP_SMOKE_BLE_WIFI_CONFIG_LOADER_ADV stage=paused rc=%d\n",
               pause_rc);
        fflush(stdout);
        if (pause_rc != H2_PAL_OK) {
            rc = pause_rc;
        }
    }
    if (rc == H2_PAL_OK) {
        rc = h2_smoke_ble_wifi_config_run(runtime);
        int resume_rc = h2_esp_h2loader_app_commands_resume_ble_advertising();
        printf("H2_ESP_SMOKE_BLE_WIFI_CONFIG_LOADER_ADV stage=resumed rc=%d\n",
               resume_rc);
        fflush(stdout);
        /*
         * A failed resume leaves the loader's BLE command transport down, so
         * the image must not report success. The provisioning result wins when
         * it already failed: that is the more specific outcome.
         */
        if (rc == H2_PAL_OK && resume_rc != H2_PAL_OK) {
            rc = resume_rc;
        }
    }
    printf("H2_ESP_SMOKE_BLE_WIFI_CONFIG_DONE rc=%d\n", rc);
    fflush(stdout);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000u));
    }
}

void app_main(void) {
    if (h2_esp_target_task_policy_install() != H2_PAL_OK) {
        return;
    }
    h2_pal_result_t rc = h2_esp_board_start_entry_task(
        "devkit/ble-wificfg", image_entry, NULL);
    if (rc != H2_PAL_OK) {
        printf(
            "H2_BOARD_ENTRY_FAIL board=devkit image=ble-wifi-config code=%d\n",
            rc);
    }
}
