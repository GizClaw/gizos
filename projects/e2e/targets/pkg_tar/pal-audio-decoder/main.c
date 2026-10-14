#include "h2_web_application_host.h"
#include "h2_pal_audio_decoder_e2e.h"
#include <emscripten/threading.h>
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
int main(void) {
    const h2_web_application_host_config_t config = {.name = "pal-audio-decoder", .display_width = 1, .display_height = 1};
    const h2_application_host_config_t app = {
        .entry = run
    };
    h2_application_host_result_t lifecycle = {0};
    int rc = h2_web_application_host_run_app(&config, &app, &lifecycle);
    int teardown = lifecycle.cleanup;
    printf("H2_ADEC_REPORT {\"platform\":\"wasm-chromium\",\"contract\":1,"
           "\"worker\":1,\"operations\":8,\"cases\":%u,\"passed\":%zu,"
           "\"failed\":%zu,\"blocked\":%zu,\"retained\":%zu,"
           "\"frames\":%" PRIu64 ",\"pcm_bytes\":%" PRIu64 ","
           "\"qualified\":%d,\"rc\":%d,\"teardown\":%d}\n",
           (unsigned)H2_PAL_ADEC_CASE_COUNT, result.passed, result.failed, result.blocked,
           result.retained, result.frames, result.pcm_bytes, result.qualified, rc, teardown);
    return rc || teardown || lifecycle.retained || !result.qualified ? 1 : 0;
}
