#include "mobile_runner.h"
#include "h2_pal_audio_decoder_e2e.h"
#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>

int h2_adec_mobile_run(h2_runtime_config_t config,
    const h2_pal_audio_decoder_api_t *decoder, const char *platform,
    const char *version, const char *path, int (*shutdown)(void)) {
    if (!decoder || !platform || !version || !path || !shutdown) return H2_PAL_ERR_INVALID_ARG;
    config.audio_decoder = decoder;
    h2_runtime_t *runtime = NULL;
    h2_pal_adec_result_t result = {0};
    int rc = h2_runtime_init(&config, &runtime);
    if (!rc) {
        const h2_pal_adec_config_t test = {.decoder = runtime->audio_decoder,
            .mem = runtime->mem, .time = runtime->time, .sync = runtime->sync};
        rc = h2_pal_audio_decoder_e2e_run(&test, &result);
    }
    int teardown = H2_PAL_ERR_INVALID_STATE;
    if (!result.retained) {
        if (runtime) h2_runtime_deinit(runtime);
        teardown = shutdown();
    }
    FILE *output = fopen(path, "w");
    if (!output) return H2_PAL_ERR_IO;
    fprintf(output,"{\"platform\":\"%s\",\"version\":\"%s\",\"pid\":%ld,"
        "\"contract\":1,\"operations\":8,\"passed\":%zu,\"failed\":%zu,"
        "\"blocked\":%zu,\"retained\":%zu,\"qualified\":%d,\"rc\":%d,"
        "\"teardown\":%d,\"frames\":%" PRIu64 ",\"pcm_bytes\":%" PRIu64 ",\"cases\":[",
        platform,version,(long)getpid(),result.passed,result.failed,result.blocked,
        result.retained,result.qualified,rc,teardown,result.frames,result.pcm_bytes);
    const char *names[] = {"NOT_RUN","PASS","FAIL","BLOCKED"};
    for (size_t i=0u;i<H2_PAL_ADEC_CASE_COUNT;++i) {
        const h2_pal_adec_case_result_t *c=&result.cases[i];
        fprintf(output,"%s{\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d,\"line\":%u}",
            i ? "," : "",c->id ? c->id : "",names[c->status],c->detail,c->line);
    }
    fputs("]}\n",output);
    if (fclose(output)) return H2_PAL_ERR_IO;
    return rc || teardown || !result.qualified ? H2_PAL_ERR_INVALID_STATE : H2_PAL_OK;
}
