#include "h2_pal_audio_decoder_e2e.h"
#include "h2_smoke_host_runtime.h"
#include "h2_web_platform.h"
#include <emscripten/threading.h>
#include <inttypes.h>
#include <stdio.h>

static void pump(void *user) { (void)h2_web_platform_pump(user, 16u, NULL); }
static void report(void *user, const h2_pal_adec_case_result_t *item) {
    (void)user;
    const char *names[] = {"NOT_RUN", "PASS", "FAIL", "BLOCKED"};
    printf("H2_ADEC_CASE {\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d,\"line\":%u}\n",
           item->id, names[item->status], item->detail, item->line);
}

int main(void) {
    if (emscripten_is_main_runtime_thread()) return 2;
    const h2_web_platform_config_t settings = {.display_width = 1, .display_height = 1};
    h2_web_platform_t *platform = h2_web_platform_create(&settings);
    if (!platform) return 3;
    h2_runtime_config_t config = h2_smoke_host_runtime_config(
        "pal-audio-decoder", "web", "wasm32", h2_web_platform_mem_api(),
        h2_web_platform_time_api(platform), h2_web_platform_queue_api(platform),
        h2_pal_unsupported_display_api());
    config.audio_decoder = h2_web_platform_audio_decoder_api(platform);
    config.sync = h2_web_platform_sync_api(platform);
    h2_runtime_t *runtime = NULL;
    int rc = h2_runtime_init(&config, &runtime);
    h2_pal_adec_result_t result = {0};
    if (!rc) {
        const h2_pal_adec_config_t test = {
            .decoder = runtime->audio_decoder, .mem = runtime->mem,
            .time = runtime->time, .sync = runtime->sync,
            .pump = pump, .pump_user = platform, .report = report};
        rc = h2_pal_audio_decoder_e2e_run(&test, &result);
    }
    int teardown = H2_PAL_ERR_INVALID_STATE;
    if (result.retained == 0u) {
        if (runtime) h2_runtime_deinit(runtime);
        teardown = h2_web_platform_destroy(platform);
    }
    printf("H2_ADEC_REPORT {\"platform\":\"wasm-chromium\",\"contract\":1,"
           "\"worker\":1,\"operations\":8,\"cases\":%u,\"passed\":%zu,"
           "\"failed\":%zu,\"blocked\":%zu,\"retained\":%zu,"
           "\"frames\":%" PRIu64 ",\"pcm_bytes\":%" PRIu64 ","
           "\"qualified\":%d,\"rc\":%d,\"teardown\":%d}\n",
           (unsigned)H2_PAL_ADEC_CASE_COUNT, result.passed, result.failed, result.blocked,
           result.retained, result.frames, result.pcm_bytes, result.qualified, rc, teardown);
    return rc || teardown || !result.qualified ? 1 : 0;
}
