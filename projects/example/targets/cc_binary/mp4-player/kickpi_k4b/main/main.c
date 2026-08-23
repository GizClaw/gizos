#define _POSIX_C_SOURCE 200809L

#include "h2_allwinner_cedarx_video_decoder.h"
#include "h2_kickpi_k4b_board.h"
#include "h2_linux_alsa_audio.h"
#include "h2_linux_fdk_aac_decoder.h"
#include "h2_smoke_mp4_player.h"

#include <signal.h>
#include <stdio.h>

#define H2_KICKPI_K4B_MP4_PLAYER_DEFAULT_MEDIA \
    "/opt/h2/share/mp4-player/test_1024x600_h264_aac.mp4"

static volatile sig_atomic_t s_stop_requested;

static void request_stop(int signal_number) {
    (void)signal_number;
    s_stop_requested = 1;
}

static int should_stop(void *user) {
    (void)user;
    return s_stop_requested != 0;
}

static h2_pal_result_t file_read_at(
    void *user,
    uint64_t offset,
    void *buffer,
    size_t capacity,
    size_t *out_read) {
    FILE *file = user;
    if (offset > (uint64_t)INT64_MAX ||
        fseeko(file, (off_t)offset, SEEK_SET) != 0) {
        return H2_PAL_ERR_IO;
    }
    *out_read = fread(buffer, 1u, capacity, file);
    return *out_read == capacity || feof(file) ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static h2_pal_result_t player_ready(void *user) {
    (void)user;
    fprintf(
        stderr,
        "H2_SMOKE_MP4_PLAYER_READY board=kickpi_k4b width=1024 height=600\n");
    return H2_PAL_OK;
}

static h2_pal_result_t run_media(h2_runtime_t *runtime, const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return H2_PAL_ERR_IO;
    h2_pal_result_t result = H2_PAL_ERR_IO;
    if (fseeko(file, 0, SEEK_END) == 0) {
        const off_t size = ftello(file);
        if (size > 0) {
            const h2_smoke_mp4_player_config_t player_config = {
                .source = {
                    .user = file,
                    .size = (uint64_t)size,
                    .read_at = file_read_at,
                },
                .acquire_timeout_ms = 1000u,
                .max_frames = 0u,
                .looping = 1,
                .display_mode = H2_SMOKE_MP4_PLAYER_DISPLAY_EXACT,
                .require_audio = 1,
                .should_stop = should_stop,
                .on_ready = player_ready,
            };
            result = h2_smoke_mp4_player_run(runtime, &player_config);
        }
    }
    (void)fclose(file);
    return result;
}

int main(int argc, char **argv) {
    struct sigaction action = {0};
    action.sa_handler = request_stop;
    (void)sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) != 0 ||
        sigaction(SIGINT, &action, NULL) != 0) {
        fprintf(
            stderr,
            "H2_SMOKE_MP4_PLAYER_FAIL stage=signal rc=%d\n",
            H2_PAL_ERR_IO);
        return 1;
    }

    h2_runtime_config_t board_config = {0};
    h2_runtime_t *runtime = NULL;
    const h2_linux_alsa_audio_config_t audio_config = {
        .device = "default",
        .playback_format = {
            .sample_rate_hz = 16000u,
            .frame_samples_per_channel = 1024u,
            .channels = 1u,
            .sample_format = H2_AUDIO_SAMPLE_S16LE,
        },
    };
    h2_pal_result_t result = h2_linux_alsa_audio_configure(&audio_config);
    const h2_kickpi_k4b_board_providers_t providers = {
        .audio = h2_linux_alsa_audio_api(),
        .audio_decoder = h2_linux_fdk_aac_decoder_api(),
        .video_decoder = h2_allwinner_cedarx_video_decoder_api(),
    };
    if (result == H2_PAL_OK) {
        result = h2_kickpi_k4b_board_runtime_config(
            &board_config, &providers);
    }
    if (result == H2_PAL_OK) result = h2_runtime_init(&board_config, &runtime);
    if (result != H2_PAL_OK) {
        fprintf(
            stderr,
            "H2_SMOKE_MP4_PLAYER_FAIL stage=runtime rc=%d\n",
            result);
        return 1;
    }

    const char *media_path = argc > 1
        ? argv[1]
        : H2_KICKPI_K4B_MP4_PLAYER_DEFAULT_MEDIA;
    result = run_media(runtime, media_path);
    h2_runtime_deinit(runtime);
    if (result != H2_PAL_OK) {
        fprintf(
            stderr,
            "H2_SMOKE_MP4_PLAYER_FAIL stage=run rc=%d media=%s\n",
            result,
            media_path);
        return 1;
    }
    fprintf(stderr, "H2_SMOKE_MP4_PLAYER_STOPPED board=kickpi_k4b\n");
    return 0;
}
