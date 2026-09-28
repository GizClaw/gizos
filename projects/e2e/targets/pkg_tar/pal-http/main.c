#include "h2_pal_http_e2e.h"
#include "h2_web_platform.h"
#include <emscripten/threading.h>
#include <stdio.h>

static void report(void *user, const h2_pal_http_e2e_case_result_t *result) {
    (void)user;
    printf("H2_PAL_HTTP_CASE {\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d,\"line\":%u,\"elapsed_ms\":%llu}\n",
        result->id, result->passed ? "PASS" : result->blocked ? "BLOCKED" : "FAIL",
        result->detail, result->line, (unsigned long long)result->elapsed_ms);
}

int main(int argc, char **argv) {
    if (argc != 4 || emscripten_is_main_runtime_thread()) return 2;
    const h2_web_platform_config_t platform_config = {.display_width = 1, .display_height = 1};
    h2_web_platform_t *platform = h2_web_platform_create(&platform_config);
    if (platform == NULL) return 2;
    h2_runtime_t runtime = {0};
    runtime.http = h2_web_platform_http_api(platform);
    runtime.mem = h2_web_platform_mem_api();
    runtime.time = h2_web_platform_time_api(platform);
    h2_pal_http_e2e_config_t config = {
        .runtime = &runtime, .http_base = argv[1], .https_base = argv[2],
        .untrusted_https_base = argv[3], .report = report,
    };
    h2_pal_http_e2e_result_t result;
    int rc = h2_pal_http_e2e_run(&config, &result);
    int cleanup = h2_web_platform_destroy(platform);
    printf("H2_PAL_HTTP_SUMMARY {\"passed\":%u,\"failed\":%u,\"blocked\":%u,\"retained_allocations\":%zu,\"cleanup\":%d}\n",
        result.passed, result.failed, result.blocked, result.retained_allocations, cleanup);
    return rc == H2_PAL_OK && cleanup == H2_PAL_OK ? 0 : 1;
}
