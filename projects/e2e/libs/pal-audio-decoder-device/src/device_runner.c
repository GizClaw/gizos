#include "device_runner.h"
#include "h2_pal_audio_decoder_e2e.h"
#include <inttypes.h>
#include <stdio.h>

static h2_pal_adec_result_t result;
static const char *image_version;
static int run_rc;
static int complete;
int h2_adec_device_run(h2_runtime_t *runtime, const char *version) {
    if (image_version || !runtime || !version) return H2_PAL_ERR_INVALID_STATE;
    image_version = version;
    printf("H2_ADEC_BOOT version=%s\n", version);
    const h2_pal_adec_config_t config = {
        .decoder = runtime->audio_decoder, .mem = runtime->mem,
        .time = runtime->time, .sync = runtime->sync};
    run_rc = h2_pal_audio_decoder_e2e_run(&config, &result);
    complete = 1;
    h2_adec_device_replay(runtime);
    return run_rc;
}
void h2_adec_device_replay(h2_runtime_t *runtime) {
    if (!complete) return;
    const char *names[] = {"NOT_RUN", "PASS", "FAIL", "BLOCKED"};
    for (size_t i = 0u; i < H2_PAL_ADEC_CASE_COUNT; ++i) {
        const h2_pal_adec_case_result_t *c = &result.cases[i];
        printf("H2_ADEC_CASE {\"version\":\"%s\",\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d,\"line\":%u}\n",
               image_version, c->id, names[c->status], c->detail, c->line);
        h2_pal_time_sleep_ms(runtime->time, 90u);
    }
    printf("H2_ADEC_REPORT {\"version\":\"%s\",\"contract\":1,\"operations\":8,\"passed\":%zu,\"failed\":%zu,\"blocked\":%zu,\"retained\":%zu,\"qualified\":%d,\"rc\":%d,\"frames\":%" PRIu64 ",\"pcm_bytes\":%" PRIu64 "}\n",
           image_version, result.passed, result.failed, result.blocked, result.retained,
           result.qualified, run_rc, result.frames, result.pcm_bytes);
}
