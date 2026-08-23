#include "h2_bk3633_sdk_runtime_internal.h"
#include "h2_bk3633_uart_printf_abi.h"

#include "ke_task.h"
#include "gapm_task.h"
#include "rf.h"
#include "rwip.h"
#include "rwble.h"
#include <string.h>

static h2_bk3633_sdk_runtime_config_t s_environment_config;

static void sdk_rwip_reset(void)
{
    rwip_reset();
}

#if BLE_HOST_PRESENT
/* Safe defaults for a project that does not link SDK-native profiles. */
static ke_task_id_t sdk_prf_get_id_from_task(ke_msg_id_t task)
{
    (void)task;
    return KE_TASK_INVALID;
}

static ke_task_id_t sdk_prf_get_task_from_id(ke_msg_id_t id)
{
    (void)id;
    return KE_TASK_INVALID;
}

static void sdk_prf_init(uint8_t reset)
{
    (void)reset;
}

static void sdk_prf_create(uint8_t conidx)
{
    (void)conidx;
}

static void sdk_prf_cleanup(uint8_t conidx, uint8_t reason)
{
    (void)conidx;
    (void)reason;
}

static uint8_t sdk_prf_add_profile(
    struct gapm_profile_task_add_cmd *params,
    ke_task_id_t *prf_task)
{
    (void)params;
    if (prf_task != NULL)
        *prf_task = KE_TASK_INVALID;
    return KE_TASK_FAIL;
}
#endif

static void sdk_platform_reset(uint32_t error)
{
    s_environment_config.fatal_reset(
        s_environment_config.user, error);
    for (;;) {
    }
}

static void sdk_set_event(uint32_t event)
{
    h2_bk3633_sdk_runtime_set_event(event);
}

static void sdk_clear_event(uint32_t event)
{
    h2_bk3633_sdk_runtime_clear_event(event);
}

h2_pal_result_t h2_bk3633_sdk_runtime_configure_rom_environment(
    const h2_bk3633_sdk_runtime_config_t *config)
{
    if (config == NULL || config->fatal_reset == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    s_environment_config = *config;
    memset(&rom_env, 0, sizeof(rom_env));
#if BLE_HOST_PRESENT
    rom_env.prf_get_id_from_task = sdk_prf_get_id_from_task;
    rom_env.prf_get_task_from_id = sdk_prf_get_task_from_id;
    rom_env.prf_init = sdk_prf_init;
    rom_env.prf_create = sdk_prf_create;
    rom_env.prf_cleanup = sdk_prf_cleanup;
    rom_env.prf_add_profile = sdk_prf_add_profile;
#endif
    rom_env.rwip_reset = sdk_rwip_reset;
    rom_env.platform_reset = sdk_platform_reset;
    rom_env.rwble_sleep_wakeup_end = rwble_sleep_wakeup_end;
    rom_env.stack_printf = uart_printf;
    rom_env.ana_xvr_reg_get = ana_xvr_reg_get;
    rom_env.ana_xvr_reg_set = ana_xvr_reg_set;
    rom_env.setEvent = sdk_set_event;
    rom_env.clearEvent = sdk_clear_event;
    if (config->configure_rom_environment != NULL) {
        h2_pal_result_t result =
            config->configure_rom_environment(config->user);
        if (result != H2_PAL_OK) {
            return result;
        }
    }
    /* Image profile hooks cannot opt deep-stack logging back in. */
    rom_env.stack_printf = uart_printf;
    return H2_PAL_OK;
}
