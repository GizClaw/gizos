#include "h2_bk3633_sdk_runtime.h"
#include "h2_bk3633_sdk_runtime_internal.h"
#include "runtime_state_probe_stubs.h"

#include <stdint.h>

static int s_image_init_count;
static int s_dispatch_count;
static int s_standby_poll_count;

static void fatal_reset(void *user, uint32_t error)
{
    (void)user;
    (void)error;
}

static h2_pal_result_t image_init(void *user)
{
    (void)user;
    ++s_image_init_count;
    return H2_PAL_OK;
}

static h2_pal_result_t dispatch_one(void *user, bool *out_more_work)
{
    (void)user;
    ++s_dispatch_count;
    *out_more_work = s_dispatch_count == 1;
    return H2_PAL_OK;
}

static bool standby_poll(void *user, uint32_t *out_reason)
{
    (void)user;
    ++s_standby_poll_count;
    *out_reason = 7u;
    return true;
}

static int standby_request(void *user)
{
    (void)user;
    const h2_bk3633_sdk_ble_standby_config_t config = {
        .poll_wake = standby_poll,
    };
    uint32_t wake_reason = 0u;
    h2_pal_result_t result =
        h2_bk3633_sdk_runtime_ble_standby(&config, &wake_reason);
    return result == H2_PAL_OK && wake_reason == 7u
               ? (int)H2_PAL_OK
               : (int)H2_PAL_ERR_INVALID_STATE;
}

int main(void)
{
    h2_libco_t *executor = h2_bk3633_probe_executor_create();
    h2_libco_task_t *task = NULL;
    size_t resumed = 0u;
    int task_result = 0;
    if (executor == NULL) {
        return 1;
    }
    h2_bk3633_sdk_runtime_config_t config = {
        .user = executor,
        .executor = executor,
        .mem = h2_bk3633_probe_mem_api(),
        .rwip_wait_key = 0x3633u,
        .fatal_reset = fatal_reset,
        .record_wake = h2_bk3633_probe_record_wake,
        .wait_completion = h2_bk3633_probe_wait_completion,
        .application_init = image_init,
        .dispatch_one = dispatch_one,
    };

    if (h2_bk3633_sdk_runtime_platform_init(&config) != H2_PAL_OK ||
        h2_bk3633_sdk_runtime_platform_init(&config) !=
            H2_PAL_ERR_INVALID_STATE) {
        return 2;
    }
    if (h2_libco_task_start(executor, NULL, h2_bk3633_sdk_runtime_task,
                            NULL, &task) != H2_LIBCO_OK ||
        h2_libco_schedule(executor, 1u, &resumed) != H2_LIBCO_OK ||
        resumed != 1u || s_image_init_count != 1 ||
        h2_bk3633_probe_application_init_count() != 1) {
        return 3;
    }
    h2_bk3633_sdk_runtime_set_event(1u);
    if (h2_libco_schedule(executor, 1u, &resumed) != H2_LIBCO_OK ||
        resumed != 1u || h2_bk3633_probe_schedule_count() != 1 ||
        s_dispatch_count != 1) {
        return 4;
    }
    if (h2_libco_schedule(executor, 1u, &resumed) != H2_LIBCO_OK ||
        resumed != 1u || h2_bk3633_probe_schedule_count() != 1 ||
        s_dispatch_count != 2) {
        return 5;
    }
    h2_libco_task_t *standby_task = NULL;
    if (h2_libco_task_start(executor, NULL, standby_request, NULL,
                            &standby_task) != H2_LIBCO_OK) {
        return 60;
    }
    h2_libco_result_t standby_join = H2_LIBCO_ERR_BUSY;
    for (size_t turn = 0u;
         turn < 4u && standby_join == H2_LIBCO_ERR_BUSY; ++turn) {
        if (h2_libco_schedule(executor, 4u, &resumed) != H2_LIBCO_OK) {
            return 61;
        }
        standby_join =
            h2_libco_task_join(executor, standby_task, &task_result);
    }
    if (standby_join != H2_LIBCO_OK) {
        return 62;
    }
    if (task_result != H2_PAL_OK) {
        return 63;
    }
    if (s_standby_poll_count != 1) {
        return 64;
    }
    if (h2_bk3633_probe_schedule_count() != 2) {
        return 65;
    }
    if (h2_libco_task_cancel(executor, task) != H2_LIBCO_OK ||
        h2_libco_schedule(executor, 1u, &resumed) != H2_LIBCO_OK ||
        h2_libco_task_join(executor, task, &task_result) != H2_LIBCO_OK ||
        task_result != H2_PAL_EXIT) {
        return 7;
    }
    h2_bk3633_probe_executor_destroy(&executor);
    return executor == NULL ? 0 : 8;
}
