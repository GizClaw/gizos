#include "h2_desktop_platform.h"
#include "h2_ffmpeg.h"
#if defined(H2_ADEC_AUDIOCONVERTER_PROBE)
#include "h2_ios_platform.h"
#endif
#include "h2_pal_audio_decoder_e2e.h"
#include "h2_smoke_host_runtime.h"
#include <inttypes.h>
#include <stdio.h>

static void report(void *user, const h2_pal_adec_case_result_t *item) {
    (void)user;
    const char *names[] = {"NOT_RUN", "PASS", "FAIL", "BLOCKED"};
    printf("H2_ADEC_CASE {\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d,\"line\":%u}\n",
           item->id, names[item->status], item->detail, item->line);
}

int main(void) {
    h2_runtime_config_t config = h2_smoke_host_runtime_config(
        "pal-audio-decoder", "desktop", "host", h2_desktop_platform_default_allocator(),
        h2_desktop_platform_time_api(), h2_desktop_platform_queue_api(),
        h2_pal_unsupported_display_api());
#if defined(H2_ADEC_AUDIOCONVERTER_PROBE)
    config.audio_decoder = h2_ios_platform_audio_decoder_api();
#else
    config.audio_decoder = h2_ffmpeg_audio_decoder_api();
#endif
    config.sync = h2_desktop_platform_sync_api();
    h2_runtime_t *runtime = NULL;
    int rc = h2_runtime_init(&config, &runtime);
    h2_pal_adec_result_t result = {0};
    if (!rc) {
        const h2_pal_adec_config_t test = {
            .decoder = runtime->audio_decoder, .mem = runtime->mem,
            .time = runtime->time, .sync = runtime->sync, .report = report};
        rc = h2_pal_audio_decoder_e2e_run(&test, &result);
    }
    const char *platform =
#if defined(H2_ADEC_AUDIOCONVERTER_PROBE)
        "darwin-audioconverter-probe";
#elif defined(__APPLE__)
        "macos";
#else
        "linux";
#endif
    printf("H2_ADEC_REPORT {\"platform\":\"%s\",\"operations\":8,\"cases\":%u,"
           "\"passed\":%zu,\"failed\":%zu,\"blocked\":%zu,\"retained\":%zu,"
           "\"frames\":%" PRIu64 ",\"pcm_bytes\":%" PRIu64 ",\"qualified\":%d,\"rc\":%d}\n",
           platform, (unsigned)H2_PAL_ADEC_CASE_COUNT, result.passed, result.failed, result.blocked,
           result.retained, result.frames, result.pcm_bytes, result.qualified, rc);
    if (runtime && result.retained == 0u) h2_runtime_deinit(runtime);
    return rc || !result.qualified ? 1 : 0;
}
