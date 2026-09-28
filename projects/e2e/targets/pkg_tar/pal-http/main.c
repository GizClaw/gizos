#include "h2_pal_http_e2e.h"
#include "h2_web_platform.h"
#include "h2_web_main_thread.h"
#include <emscripten/threading.h>
#include <stdio.h>
#include <string.h>

EM_JS(int, calling_js_is_worker, (), {
    return ENVIRONMENT_IS_PTHREAD && typeof window === 'undefined' ? 1 : 0;
});

/* Read the same module closure through the same bridge as Web HTTP. The
 * calling C pthread emits the result; the page's global Module is not evidence. */
EM_JS(void, observe_http_registry,
    (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
    h2WebMain(context, result, completion, ["pointer", "i32"], null, (output, fault) => {
        if (fault === 1) delete Module['h2WebHttp'];
        const registry = Module['h2WebHttp'];
        if (fault === 2 && registry instanceof Map) {
            const invalid = new Map();
            Object.defineProperty(invalid, 'size', {value: NaN});
            registry.set('negative-observation', invalid);
        }
        if (fault === 3 && registry instanceof Map)
            registry.set('negative-observation', new Map([['retained', {}]]));
        const exists = registry instanceof Map;
        let valid = exists;
        let pending = 0;
        if (exists) {
            for (const requests of registry.values()) {
                if (!(requests instanceof Map) || !Number.isSafeInteger(requests.size) || requests.size < 0) {
                    valid = false;
                    break;
                }
                pending += requests.size;
            }
            if (!Number.isSafeInteger(pending)) valid = false;
        }
        HEAP32[output >> 2] = exists ? 1 : 0;
        HEAP32[(output + 4) >> 2] = valid ? 1 : 0;
        HEAP32[(output + 8) >> 2] = valid ? pending : -1;
        HEAP32[(output + 12) >> 2] = ENVIRONMENT_IS_PTHREAD ? 1 : 0;
    });
});

static void report(void *user, const h2_pal_http_e2e_case_result_t *result) {
    (void)user;
    printf("H2_PAL_HTTP_CASE {\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d,\"line\":%u,\"elapsed_ms\":%llu,\"request_result\":%d}\n",
        result->id, result->passed ? "PASS" : result->blocked ? "BLOCKED" : "FAIL",
        result->detail, result->line, (unsigned long long)result->elapsed_ms, result->request_result);
}

int main(int argc, char **argv) {
    if ((argc != 4 && argc != 5) || emscripten_is_main_runtime_thread() || !calling_js_is_worker()) return 2;
    const h2_web_platform_config_t platform_config = {.display_width = 1, .display_height = 1};
    h2_web_platform_t *platform = h2_web_platform_create(&platform_config);
    if (platform == NULL) return 2;
    h2_runtime_t runtime = {0};
    runtime.http = h2_web_platform_http_api(platform);
    runtime.mem = h2_web_platform_mem_api();
    runtime.time = h2_web_platform_time_api(platform);
    h2_pal_http_e2e_config_t config = {
        .runtime = &runtime, .http_base = argv[1], .https_base = argv[2],
        .untrusted_https_base = argv[3], .untrusted_tls_error = H2_PAL_ERR_IO, .report = report,
    };
    h2_pal_http_e2e_result_t result;
    int rc = h2_pal_http_e2e_run(&config, &result);
    int fault = 0;
    if (argc == 5) {
        if (strcmp(argv[4], "missing") == 0) fault = 1;
        else if (strcmp(argv[4], "invalid-count") == 0) fault = 2;
        else if (strcmp(argv[4], "retained") == 0) fault = 3;
    }
    int cleanup = h2_web_platform_destroy(platform);
    int observation[4] = {-1, -1, -1, -1};
    (void)h2_web_main_call(observe_http_registry, (const void *[]){&(int *){observation}, &fault});
    printf("H2_PAL_HTTP_WORKER_STATE {\"schema\":1,\"caller_is_pthread\":%d,\"caller_is_main_runtime\":%d,\"registry_present\":%d,\"registry_valid\":%d,\"pending_requests\":%d,\"registry_owner_is_pthread\":%d}\n",
        calling_js_is_worker(), emscripten_is_main_runtime_thread(), observation[0], observation[1], observation[2], observation[3]);
    int observed_clean = observation[0] == 1 && observation[1] == 1 && observation[2] == 0;
    printf("H2_PAL_HTTP_SUMMARY {\"passed\":%u,\"failed\":%u,\"blocked\":%u,\"retained_allocations\":%zu,\"cleanup\":%d}\n",
        result.passed, result.failed, result.blocked, result.retained_allocations, cleanup);
    return rc == H2_PAL_OK && cleanup == H2_PAL_OK && observed_clean ? 0 : 1;
}
