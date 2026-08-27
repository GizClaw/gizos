#include "h2_bk7258_board.h"
#include "h2_bk_audio_decoder.h"
#include "h2_bk_h2loader.h"
#include "h2_smoke_mp4_player.h"
#include "h2_tinyh264.h"
#include "h2_bk_target_task_policy.h"

#include "bk_private/bk_init.h"
#include "os/os.h"

#include <stdarg.h>
#include <stdio.h>

static void h2_bk_serial_log_string(int port, const char *string) {
    (void)port;
    os_printf("%s", string);
}

#define emergency_uart_write_string h2_bk_serial_log_string

static void emit_marker(const char *fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    emergency_uart_write_string(0, line);
    emergency_uart_write_string(0, "\r\n");
    os_printf("%s\r\n", line);
}

static h2_pal_result_t confirm_ready(void *user) {
    h2_runtime_t *runtime = user;
    const h2_pal_result_t result =
        (h2_pal_result_t)h2_bk_h2loader_confirm_current_app(runtime->pref);
    if (result == H2_PAL_OK) {
        emit_marker("H2_BK_MP4_PLAYER_READY rc=0");
    }
    return result;
}

static void app_entry(void *user) {
    (void)user;
    h2_runtime_config_t config;
    h2_runtime_t *runtime = NULL;

    emit_marker("H2_BK_AP_BOOT image=mp4-player");
    h2_pal_result_t result = h2_bk7258_board_runtime_config(&config);
    if (result != H2_PAL_OK) {
        emit_marker("H2_BK_MP4_PLAYER_FAIL stage=runtime_config rc=%d", result);
        return;
    }
    config.mem = h2_bk7258_board_psram_allocator();
    config.video_decoder = h2_tinyh264_video_decoder_api();
    config.audio_decoder = h2_bk_audio_decoder_api();

    result = h2_runtime_init(&config, &runtime);
    if (result != H2_PAL_OK) {
        emit_marker("H2_BK_MP4_PLAYER_FAIL stage=runtime_init rc=%d", result);
        (void)h2_bk7258_board_runtime_deinit();
        return;
    }
    if (result == H2_PAL_OK) {
        result = (h2_pal_result_t)
            h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
                runtime, "mp4-player", H2_LOADER_CAPABILITY_UART);
    }
    if (result != H2_PAL_OK) {
        emit_marker("H2_BK_MP4_PLAYER_FAIL stage=app_cli rc=%d", result);
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
    emit_marker("H2_BK_MP4_PLAYER_FAIL stage=run rc=%d", result);
    for (;;) {
        rtos_delay_milliseconds(1000);
    }
}

int main(void) {
    if (h2_bk_target_task_policy_install() != H2_PAL_OK) {
        return -1;
    }
    emergency_uart_write_string(
        0, "H2_BK_AP_MAIN image=mp4-player stage=before_bk_init\r\n");
    bk_init();
    const h2_pal_result_t result = h2_bk7258_board_start_entry_task(
        "bk/mp4-player", app_entry, NULL);
    if (result != H2_PAL_OK) {
        emit_marker("H2_BK_BOARD_ENTRY_FAIL image=mp4-player rc=%d", result);
    }
    return 0;
}
