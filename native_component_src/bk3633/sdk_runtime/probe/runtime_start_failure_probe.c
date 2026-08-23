#include "h2_bk3633_sdk_runtime.h"
#include "runtime_state_probe_stubs.h"

#include <stdint.h>

static void fatal_reset(void *user, uint32_t error)
{
    (void)user;
    (void)error;
}

static h2_pal_result_t image_init(void *user)
{
    (void)user;
    return H2_PAL_OK;
}

int main(void)
{
    h2_libco_t *executor = h2_bk3633_probe_executor_create();
    h2_libco_task_t *task = NULL;
    size_t resumed = 0u;
    int task_result = 0;
    const h2_bk3633_sdk_runtime_config_t config = {
        .user = executor,
        .executor = executor,
        .mem = h2_bk3633_probe_mem_api(),
        .rwip_wait_key = 0x3633u,
        .fatal_reset = fatal_reset,
        .record_wake = h2_bk3633_probe_record_wake,
        .wait_completion = h2_bk3633_probe_wait_completion,
        .application_init = image_init,
    };

    if (executor == NULL ||
        h2_bk3633_sdk_runtime_platform_init(&config) != H2_PAL_OK) {
        return 1;
    }
    h2_bk3633_probe_set_rom_environment_result(H2_PAL_ERR_IO);
    if (h2_libco_task_start(executor, NULL, h2_bk3633_sdk_runtime_task,
                            NULL, &task) != H2_LIBCO_OK ||
        h2_libco_schedule(executor, 1u, &resumed) != H2_LIBCO_OK ||
        h2_libco_task_join(executor, task, &task_result) != H2_LIBCO_OK ||
        task_result != H2_PAL_ERR_IO) {
        return 2;
    }
    h2_bk3633_probe_executor_destroy(&executor);
    return executor == NULL ? 0 : 3;
}
