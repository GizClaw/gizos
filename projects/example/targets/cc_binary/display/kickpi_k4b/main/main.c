#define _POSIX_C_SOURCE 200809L

#include "h2_kickpi_k4b_board.h"
#include "h2/pal/h2_pal_unsupported.h"
#include "h2_smoke_display.h"

#include <signal.h>
#include <stdio.h>

static volatile sig_atomic_t s_stop_requested;

static void request_stop(int signal_number) {
    (void)signal_number;
    s_stop_requested = 1;
}

int main(void) {
    struct sigaction action = {0};
    action.sa_handler = request_stop;
    (void)sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) != 0 || sigaction(SIGINT, &action, NULL) != 0) {
        fprintf(stderr, "H2_SMOKE_DISPLAY_FAIL stage=signal rc=%d\n", H2_PAL_ERR_IO);
        return 1;
    }

    sigset_t stop_signals;
    sigset_t previous_mask;
    sigset_t suspend_mask;
    if (sigemptyset(&stop_signals) != 0 || sigaddset(&stop_signals, SIGTERM) != 0 ||
        sigaddset(&stop_signals, SIGINT) != 0 ||
        sigprocmask(SIG_BLOCK, &stop_signals, &previous_mask) != 0) {
        fprintf(stderr, "H2_SMOKE_DISPLAY_FAIL stage=signal-mask rc=%d\n", H2_PAL_ERR_IO);
        return 1;
    }
    suspend_mask = previous_mask;
    (void)sigdelset(&suspend_mask, SIGTERM);
    (void)sigdelset(&suspend_mask, SIGINT);

    h2_runtime_config_t board_config = {0};
    h2_runtime_t *runtime = NULL;
    const h2_kickpi_k4b_board_providers_t providers = {
        .audio = h2_pal_unsupported_audio_api(),
        .audio_decoder = h2_pal_unsupported_audio_decoder_api(),
        .video_decoder = h2_pal_unsupported_video_decoder_api(),
    };
    h2_pal_result_t result = h2_kickpi_k4b_board_runtime_config(
        &board_config, &providers);
    if (result == H2_PAL_OK) result = h2_runtime_init(&board_config, &runtime);
    if (result == H2_PAL_OK) result = h2_smoke_display_run(runtime);
    if (result != H2_PAL_OK) {
        fprintf(stderr, "H2_SMOKE_DISPLAY_FAIL stage=run rc=%d\n", result);
        if (runtime != NULL) {
            (void)h2_pal_display_close(runtime->display);
            h2_runtime_deinit(runtime);
        }
        (void)sigprocmask(SIG_SETMASK, &previous_mask, NULL);
        return 1;
    }

    fprintf(stderr, "H2_SMOKE_DISPLAY_READY board=kickpi_k4b width=1024 height=600\n");
    while (!s_stop_requested) (void)sigsuspend(&suspend_mask);
    (void)sigprocmask(SIG_SETMASK, &previous_mask, NULL);
    (void)h2_pal_display_close(runtime->display);
    h2_runtime_deinit(runtime);
    fprintf(stderr, "H2_SMOKE_DISPLAY_STOPPED board=kickpi_k4b\n");
    return 0;
}
