#include "h2_desktop_application_host.h"
#include "h2_ffmpeg.h"
#if defined(H2_ADEC_AUDIOCONVERTER_PROBE)
#include "h2_ios_platform.h"
#endif
#include "h2_pal_audio_decoder_e2e.h"
#include <inttypes.h>
#include <stdio.h>

static void report(void *user, const h2_pal_adec_case_result_t *item) {
    (void)user;
    const char *names[] = {"NOT_RUN", "PASS", "FAIL", "BLOCKED"};
    printf("H2_ADEC_CASE {\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d,\"line\":%u}\n",
           item->id, names[item->status], item->detail, item->line);
}

static h2_pal_adec_result_t result;
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
    (void)user;
    const h2_pal_adec_config_t test = {
        .decoder = runtime->audio_decoder, .mem = runtime->mem,
        .time = runtime->time, .sync = runtime->sync, .report = report};
    int rc = h2_pal_audio_decoder_e2e_run(&test, &result);
    if (result.retained) h2_application_host_retain(host, NULL, H2_PAL_ERR_BUSY);
    return rc;
}
#if defined(H2_ADEC_AUDIOCONVERTER_PROBE)
static h2_pal_result_t configure(void *user, h2_runtime_config_t *config) {
    (void)user;
    config->audio_decoder = h2_ios_platform_audio_decoder_api();
    return H2_PAL_OK;
}
#endif
int main(void) {
    const h2_desktop_application_host_config_t config = {0};
    const h2_application_host_config_t app = {
        .entry = run,
#if defined(H2_ADEC_AUDIOCONVERTER_PROBE)
        .configure = configure,
#endif
    };
    h2_application_host_result_t lifecycle = {0};
    int rc = h2_desktop_application_host_run(&config, &app, &lifecycle);
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
    return rc || lifecycle.retained || !result.qualified ? 1 : 0;
}
