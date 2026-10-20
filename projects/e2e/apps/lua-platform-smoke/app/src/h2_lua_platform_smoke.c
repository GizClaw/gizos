#include "h2_lua_platform_smoke.h"
#include "h2_lua.h"
#include "h2_lua_job.h"
#include <string.h>

int h2_lua_platform_smoke_run(h2_runtime_t *runtime) {
    h2_lua_host_t *host = NULL;
    const h2_lua_host_config_t config = {
        .runtime = runtime,
        .max_jobs = 1,
        .worker_count = 1,
        .worker_stack_size = 16384,
        .event_delivery_capacity = 2,
        .callback_capacity_per_job = 4,
        .audio_track_capacity_per_job = 1,
        .max_coroutines_per_vm = 4,
        .ready_queue_capacity = 8,
        .waiter_capacity = 4,
        .pending_capability_capacity = 2,
        .capability_capacity = 2,
        .vm_memory_limit_bytes = 64u * 1024u,
        .execution_timeout_ms = 1000,
    };
    h2_pal_result_t rc = h2_lua_host_create(&config, &host);
    if (rc != H2_PAL_OK)
        return rc;
    rc = h2_lua_host_start(host);
    h2_lua_job_id_t job = H2_LUA_JOB_ID_NONE;
    static const char script[] = "local r=require('runtime');r.sleep(1);return 'C3_LUA_OK'";
    if (rc == H2_PAL_OK)
        rc = h2_lua_job_submit_text(host, NULL, "@platform-smoke.lua", (const uint8_t *)script,
                                    sizeof(script) - 1, NULL, 0, &job);
    int passed = 0;
    for (unsigned i = 0; rc == H2_PAL_OK && i < 2000; ++i) {
        h2_lua_job_status_t status;
        rc = h2_lua_job_get_status(host, job, &status);
        if (rc != H2_PAL_OK)
            break;
        if (status.state == H2_LUA_JOB_SUCCEEDED) {
            char output[32];
            size_t size = 0;
            int present = 0;
            rc = h2_lua_job_get_result(host, job, output, sizeof(output), &size, &present);
            passed = rc == H2_PAL_OK && present && size == 9 && memcmp(output, "C3_LUA_OK", 9) == 0;
            break;
        }
        if (status.state == H2_LUA_JOB_FAILED || status.state == H2_LUA_JOB_CANCELLED)
            break;
        rc = h2_pal_time_sleep_ms(runtime->time, 1);
    }
    h2_pal_result_t cleanup = h2_lua_host_destroy_checked(host);
    while (cleanup != H2_PAL_OK) {
        /* A failed join cannot authorize dropping the borrowed Runtime. */
        (void)h2_pal_time_sleep_ms(runtime->time, 1);
        cleanup = h2_lua_host_destroy_checked(host);
    }
    return passed ? 0 : H2_PAL_ERR_IO;
}
