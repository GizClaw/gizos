#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_smoke_audio_system.h"

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
    char line[128];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    emergency_uart_write_string(0, line);
    emergency_uart_write_string(0, "\r\n");
    os_printf("%s\r\n", line);
}

static void app_entry(void *user) {
    h2_runtime_config_t runtime_config;
    h2_runtime_t *runtime = NULL;
    int rc;

    (void)user;

    emergency_uart_write_string(0, "H2_BK_AP_BOOT image=audio-system\r\n");
    os_printf("H2_BK_AP_BOOT image=audio-system\r\n");
    rtos_delay_milliseconds(500);
    emit_marker("H2_BK_SMOKE_AUDIO_STAGE stage=host_cli_owner_cp");
    rc = h2_bk7258_board_runtime_config(&runtime_config);
    if (rc != H2_PAL_OK) {
        emit_marker("H2_BK_SMOKE_AUDIO_FAIL stage=runtime_config rc=%d", rc);
        (void)h2_bk_h2loader_reboot_to_loader();
        for (;;) {
            rtos_delay_milliseconds(1000);
        }
    }
    rc = h2_runtime_init(&runtime_config, &runtime);
    if (rc != H2_PAL_OK) {
        emit_marker("H2_BK_SMOKE_AUDIO_FAIL stage=runtime_init rc=%d", rc);
        (void)h2_bk_h2loader_reboot_to_loader();
        for (;;) {
            rtos_delay_milliseconds(1000);
        }
    }
    rc = h2_bk_h2loader_configure_app_task_policy();
    if (rc == H2_PAL_OK) {
        rc = h2_bk_h2loader_start_app_iostreamikcp(
            runtime,
            "audio-system");
    }
    if (rc != H2_PAL_OK) {
        emit_marker("H2_BK_SMOKE_AUDIO_FAIL stage=app_cli rc=%d", rc);
        (void)h2_bk_h2loader_reboot_to_loader();
        for (;;) {
            rtos_delay_milliseconds(1000);
        }
    }
    rc = h2_bk_h2loader_start_app_ble(runtime, "audio-system");
    if (rc != H2_PAL_OK) {
        emit_marker("H2_BK_SMOKE_AUDIO_FAIL stage=app_ble rc=%d", rc);
        (void)h2_bk_h2loader_reboot_to_loader();
        for (;;) {
            rtos_delay_milliseconds(1000);
        }
    }

    emit_marker("H2_BK_SMOKE_AUDIO_STAGE stage=sd_mount_begin");
    if (runtime->fs == NULL) {
        emit_marker("H2_BK_SMOKE_AUDIO_FAIL stage=sd_mount rc=%d", H2_PAL_ERR_UNAVAILABLE);
        for (;;) {
            rtos_delay_milliseconds(1000);
        }
    }
    emit_marker("H2_BK_SMOKE_AUDIO_STAGE stage=sd_mount_done");

    const h2_smoke_audio_system_config_t config = {
        .music_path = "/data/audio/music_loop.ogg",
    };
    emit_marker("H2_BK_SMOKE_AUDIO_STAGE stage=run_begin");
    rc = h2_smoke_audio_system_run(runtime, &config);
    if (rc != H2_AUDIO_OK) {
        emit_marker("H2_BK_SMOKE_AUDIO_FAIL stage=run rc=%d", rc);
    } else {
        emit_marker("H2_BK_SMOKE_AUDIO_STAGE stage=run_done");
        rtos_delay_milliseconds(500);
        emit_marker("H2_BK_SMOKE_AUDIO_STAGE stage=confirm_begin");
        rc = h2_bk_h2loader_confirm_current_app(runtime->pref);
        if (rc != H2_PAL_OK) {
            emit_marker("H2_BK_SMOKE_AUDIO_FAIL stage=confirm rc=%d", rc);
        } else {
            emit_marker("H2_BK_SMOKE_AUDIO_READY rc=0");
        }
    }
    for (;;) {
        rtos_delay_milliseconds(1000);
    }
}

int main(void) {
    emergency_uart_write_string(0, "H2_BK_AP_MAIN image=audio-system stage=before_bk_init\r\n");
    bk_init();
    emergency_uart_write_string(0, "H2_BK_AP_MAIN image=audio-system stage=after_bk_init\r\n");
    h2_pal_result_t rc = h2_bk7258_board_start_entry_task(
        "bk/audio-system", app_entry, NULL);
    if (rc != H2_PAL_OK) {
        emit_marker("H2_BK_BOARD_ENTRY_FAIL image=audio-system rc=%d", rc);
    }
    return 0;
}
