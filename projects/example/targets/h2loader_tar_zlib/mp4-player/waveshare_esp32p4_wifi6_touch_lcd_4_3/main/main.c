#include "h2_esp_audio_decoder.h"
#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_platform_core.h"
#include "h2_loader_boot.h"
#include "h2_smoke_mp4_player.h"
#include "h2_tinyh264.h"
#include "h2_esp_layout_task_policy.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>

static h2_pal_result_t confirm_ready(void *user) {
    h2_runtime_t *runtime = user;
    h2_pal_result_t result = h2_esp_platform_confirm_running_app();
    if (result == H2_PAL_OK) {
        result = (h2_pal_result_t)h2_loader_mark_app_confirmed(runtime->pref);
    }
    if (result == H2_PAL_OK) {
        printf("H2_MP4_PLAYER_IMAGE_READY board=waveshare_esp32p4_wifi6_touch_lcd_4_3\n");
    }
    return result;
}

static void image_entry(void *user) {
    (void)user;
    h2_runtime_config_t config = {0};
    h2_runtime_t *runtime = NULL;
    h2_pal_result_t result = h2_esp_board_runtime_config(&config);
    if (result != H2_PAL_OK) {
        printf("H2_MP4_PLAYER_IMAGE_FAIL stage=runtime_config rc=%d\n", result);
        return;
    }
    config.mem = h2_esp_board_psram_allocator();
    config.video_decoder = h2_tinyh264_video_decoder_api();
    config.audio_decoder = h2_esp_audio_decoder_api();

    result = h2_esp_h2loader_app_commands_prepare_serial(
        &config, "mp4-player", 1u, 3u);
    if (result != H2_PAL_OK) {
        printf("H2_MP4_PLAYER_IMAGE_FAIL stage=serial_recovery rc=%d\n", result);
        return;
    }
    result = h2_runtime_init(&config, &runtime);
    if (result != H2_PAL_OK) {
        printf("H2_MP4_PLAYER_IMAGE_FAIL stage=runtime_init rc=%d\n", result);
        (void)h2_esp_board_runtime_deinit();
        return;
    }
    result = h2_esp_h2loader_app_commands_start(
        runtime, "mp4-player", 1u, 3u);
    if (result != H2_PAL_OK) {
        printf("H2_MP4_PLAYER_IMAGE_FAIL stage=command_services rc=%d\n", result);
        return;
    }
    const h2_smoke_mp4_player_config_t player_config = {
        .media_path = "/data/media/showcase.mp4",
        .acquire_timeout_ms = 2000u,
        .looping = 1,
        .require_audio = 1,
        .on_ready = confirm_ready,
        .ready_user = runtime,
    };
    result = h2_smoke_mp4_player_run(runtime, &player_config);
    printf("H2_MP4_PLAYER_IMAGE_FAIL stage=run rc=%d\n", result);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void) {
    if (h2_esp_layout_task_policy_install() != H2_PAL_OK) {
        return;
    }
    const h2_pal_result_t result = h2_esp_board_start_entry_task(
        "waveshare_esp32p4_wifi6_touch_lcd_4_3/mp4-player",
        image_entry,
        NULL);
    if (result != H2_PAL_OK) {
        printf(
            "H2_BOARD_ENTRY_FAIL board=waveshare_esp32p4_wifi6_touch_lcd_4_3 image=mp4-player code=%d\n",
            result);
    }
}
