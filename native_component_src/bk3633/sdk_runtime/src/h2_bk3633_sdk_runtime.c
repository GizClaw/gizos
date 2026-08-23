#include "h2_bk3633_sdk_runtime.h"
#include "h2_bk3633_sdk_runtime_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(BK3633)
#include "arch.h"
#include "icu.h"
#else
#include <stdatomic.h>
#endif

#include "flash.h"
#include "gapm_task.h"
#include "nvds.h"
#include "co_utils.h"

#if defined(BK3633_SDK_HAS_RWIP_GLUE) && BK3633_SDK_HAS_RWIP_GLUE
#include "rwip.h"
#endif

typedef enum h2_bk3633_sdk_runtime_state {
    H2_BK3633_SDK_RUNTIME_STATE_UNINITIALIZED = 0,
    H2_BK3633_SDK_RUNTIME_STATE_PLATFORM_READY = 1,
    H2_BK3633_SDK_RUNTIME_STATE_STARTING = 2,
    H2_BK3633_SDK_RUNTIME_STATE_STARTED = 3,
    H2_BK3633_SDK_RUNTIME_STATE_FAILED = 4,
} h2_bk3633_sdk_runtime_state_t;

typedef enum h2_bk3633_sdk_standby_state {
    H2_BK3633_SDK_STANDBY_IDLE = 0,
    H2_BK3633_SDK_STANDBY_REQUESTED,
    H2_BK3633_SDK_STANDBY_RUNNING,
    H2_BK3633_SDK_STANDBY_COMPLETED,
} h2_bk3633_sdk_standby_state_t;

static h2_bk3633_sdk_runtime_config_t s_runtime_config;
static h2_bk3633_sdk_runtime_state_t s_runtime_state;
static h2_pal_result_t s_application_init_result;
static h2_bk3633_sdk_ble_standby_config_t s_standby_config;
static h2_bk3633_sdk_standby_state_t s_standby_state;
static uint32_t s_standby_wake_reason;
static uint8_t s_standby_wait_key;
#if defined(BK3633)
static volatile uint32_t s_pending_events;
static volatile int s_wake_fault;
#else
static atomic_uint_fast32_t s_pending_events;
static atomic_int s_wake_fault;
#endif

static uint32_t sdk_runtime_critical_enter(void)
{
#if defined(BK3633)
    uint32_t state = __disable_fiq() != 0 ? 1u : 0u;
    if (__disable_irq() != 0) {
        state |= 2u;
    }
    return state;
#else
    return 0u;
#endif
}

static void sdk_runtime_critical_exit(uint32_t state)
{
#if defined(BK3633)
    if ((state & 1u) == 0u) {
        __enable_fiq();
    }
    if ((state & 2u) == 0u) {
        __enable_irq();
    }
#else
    (void)state;
#endif
}

static void sdk_runtime_pending_reset(void)
{
#if defined(BK3633)
    uint32_t state = sdk_runtime_critical_enter();
    s_pending_events = 0u;
    s_wake_fault = 0;
    sdk_runtime_critical_exit(state);
#else
    atomic_store_explicit(&s_pending_events, 0u, memory_order_relaxed);
    atomic_store_explicit(&s_wake_fault, 0, memory_order_relaxed);
#endif
}

static uint32_t sdk_runtime_pending_take(void)
{
#if defined(BK3633)
    uint32_t state = sdk_runtime_critical_enter();
    uint32_t pending = s_pending_events;
    s_pending_events = 0u;
    sdk_runtime_critical_exit(state);
    return pending;
#else
    return (uint32_t)atomic_exchange_explicit(
        &s_pending_events, 0u, memory_order_acq_rel);
#endif
}

static bool sdk_runtime_wake_failed(void)
{
#if defined(BK3633)
    return s_wake_fault != 0;
#else
    return atomic_load_explicit(&s_wake_fault, memory_order_acquire) != 0;
#endif
}

static bool sdk_runtime_standby_requested(void)
{
    uint32_t state = sdk_runtime_critical_enter();
    bool requested = s_standby_state == H2_BK3633_SDK_STANDBY_REQUESTED;
    sdk_runtime_critical_exit(state);
    return requested;
}

static void sdk_runtime_standby_complete(uint32_t wake_reason)
{
    uint32_t state = sdk_runtime_critical_enter();
    s_standby_wake_reason = wake_reason;
    s_standby_state = H2_BK3633_SDK_STANDBY_COMPLETED;
    sdk_runtime_critical_exit(state);
    if (s_runtime_config.record_wake(
            s_runtime_config.user, (uintptr_t)&s_standby_wait_key) !=
        H2_PAL_OK) {
#if defined(BK3633)
        s_wake_fault = 1;
#else
        atomic_store_explicit(&s_wake_fault, 1, memory_order_release);
#endif
    }
}

static void sdk_runtime_standby_sleep_once(void)
{
#if defined(BK3633)
    GLOBAL_INT_DISABLE();
    uint8_t sleep_state = rwip_sleep();
    if (sleep_state == RWIP_DEEP_SLEEP) {
        cpu_reduce_voltage_sleep();
        cpu_wakeup();
    } else if (sleep_state == RWIP_CPU_SLEEP) {
        cpu_idle_sleep();
    }
    GLOBAL_INT_RESTORE();
#endif
}

static void sdk_runtime_run_standby(void)
{
    uint32_t state = sdk_runtime_critical_enter();
    if (s_standby_state != H2_BK3633_SDK_STANDBY_REQUESTED) {
        sdk_runtime_critical_exit(state);
        return;
    }
    s_standby_state = H2_BK3633_SDK_STANDBY_RUNNING;
    sdk_runtime_critical_exit(state);

    uint32_t wake_reason = 0u;
#if defined(BK3633)
    icu_set_sleep_mode(MCU_REDUCE_VO_SLEEP);
#endif
    for (;;) {
        (void)sdk_runtime_pending_take();
        rwip_schedule();
        if (s_standby_config.poll_wake(
                s_standby_config.user, &wake_reason)) {
            break;
        }
        sdk_runtime_standby_sleep_once();
    }
#if defined(BK3633)
    icu_set_sleep_mode(MCU_IDLE_SLEEP);
#endif
    sdk_runtime_standby_complete(wake_reason);
}

/*
 * The prebuilt allroles stack exports the kernel/task symbols, but not the
 * SDK's rwip_init()/rwip_schedule() host glue. The target build enables these
 * calls when it also selects the matching RWIP sources from sdk_build.
 */
static bool sdk_runtime_config_valid(
    const h2_bk3633_sdk_runtime_config_t *config)
{
    if (config == NULL || config->fatal_reset == NULL ||
        config->application_init == NULL || config->executor == NULL ||
        config->mem == NULL ||
        config->rwip_wait_key == 0u || config->record_wake == NULL ||
        config->wait_completion == NULL) {
        return false;
    }
    return true;
}

static void sdk_runtime_enter_failed_state(void)
{
    memset(&s_runtime_config, 0, sizeof(s_runtime_config));
    s_runtime_state = H2_BK3633_SDK_RUNTIME_STATE_FAILED;
    s_application_init_result = H2_PAL_ERR_INVALID_STATE;
}

bool h2_bk3633_sdk_runtime_nvds_oversize_is_missing(
    uint8_t tag, size_t stored_size, size_t requested_capacity)
{
    if (s_runtime_config.nvds_oversize_is_missing == NULL) {
        return false;
    }
    return s_runtime_config.nvds_oversize_is_missing(
        s_runtime_config.user, tag, stored_size, requested_capacity);
}

const h2_pal_mem_api_t *h2_bk3633_sdk_runtime_nvds_mem_api(void)
{
    return s_runtime_config.mem;
}

h2_pal_result_t h2_bk3633_sdk_runtime_platform_init(
    const h2_bk3633_sdk_runtime_config_t *config)
{
    if (!sdk_runtime_config_valid(config)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (s_runtime_state != H2_BK3633_SDK_RUNTIME_STATE_UNINITIALIZED) {
        return H2_PAL_ERR_INVALID_STATE;
    }

    s_runtime_config = *config;
    s_application_init_result = H2_PAL_ERR_INVALID_STATE;
    sdk_runtime_pending_reset();
    if (s_runtime_config.platform_prepare != NULL) {
        h2_pal_result_t result =
            s_runtime_config.platform_prepare(s_runtime_config.user);
        if (result != H2_PAL_OK) {
            sdk_runtime_enter_failed_state();
            return result;
        }
    }

    flash_init();

    /* Match app_gatt_all_roles/main.c: load the factory address before the
     * RWIP controller seeds its random/LL state. */
    {
        struct bd_addr bdaddr;
        memset(&bdaddr, 0xff, sizeof(bdaddr));
        flash_read_data(&bdaddr.addr[0], flash_env.bdaddr_def_addr_abs, 6u);
        if (bdaddr.addr[0] != 0xffu || bdaddr.addr[1] != 0xffu ||
            bdaddr.addr[2] != 0xffu || bdaddr.addr[3] != 0xffu ||
            bdaddr.addr[4] != 0xffu || bdaddr.addr[5] != 0xffu) {
            memcpy(&co_default_bdaddr, &bdaddr, sizeof(bdaddr));
        }
        srand((unsigned int)(co_default_bdaddr.addr[0] +
                             co_default_bdaddr.addr[5]));
    }
    /* rwip_init() installs nvds_get/nvds_put into rwip_param.  The NVDS
     * environment must therefore be ready before lld_init() first queries
     * controller parameters. */
    if (nvds_init() != NVDS_OK) {
        sdk_runtime_enter_failed_state();
        return H2_PAL_ERR_UNAVAILABLE;
    }

    s_runtime_state = H2_BK3633_SDK_RUNTIME_STATE_PLATFORM_READY;
    return H2_PAL_OK;
}

void h2_bk3633_sdk_runtime_set_event(uint32_t event)
{
    uint32_t critical_state = sdk_runtime_critical_enter();
#if defined(BK3633)
    uint32_t previous = s_pending_events;
    s_pending_events |= event == 0u ? 1u : event;
#else
    uint_fast32_t previous = atomic_fetch_or_explicit(
        &s_pending_events, event == 0u ? 1u : event, memory_order_release);
#endif
    sdk_runtime_critical_exit(critical_state);
    if (previous == 0u &&
        s_runtime_config.record_wake(
            s_runtime_config.user,
            s_runtime_config.rwip_wait_key) != H2_PAL_OK) {
#if defined(BK3633)
        s_wake_fault = 1;
#else
        atomic_store_explicit(&s_wake_fault, 1, memory_order_release);
#endif
    }
}

void h2_bk3633_sdk_runtime_clear_event(uint32_t event)
{
#if defined(BK3633)
    uint32_t critical_state = sdk_runtime_critical_enter();
    if (event == 0u) {
        s_pending_events = 0u;
    } else {
        s_pending_events &= ~event;
    }
    sdk_runtime_critical_exit(critical_state);
#else
    if (event == 0u) {
        atomic_store_explicit(&s_pending_events, 0u, memory_order_release);
        return;
    }
    (void)atomic_fetch_and_explicit(
        &s_pending_events, (uint_fast32_t)~event, memory_order_release);
#endif
}

bool h2_bk3633_sdk_runtime_has_pending_work(void)
{
#if defined(BK3633)
    return s_pending_events != 0u || s_wake_fault != 0 ||
           s_standby_state == H2_BK3633_SDK_STANDBY_REQUESTED;
#else
    return atomic_load_explicit(
               &s_pending_events, memory_order_acquire) != 0u ||
           atomic_load_explicit(&s_wake_fault, memory_order_acquire) != 0 ||
           s_standby_state == H2_BK3633_SDK_STANDBY_REQUESTED;
#endif
}

h2_pal_result_t h2_bk3633_sdk_runtime_ble_standby(
    const h2_bk3633_sdk_ble_standby_config_t *config,
    uint32_t *out_wake_reason)
{
    if (config == NULL || config->poll_wake == NULL ||
        out_wake_reason == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_wake_reason = 0u;
    uint32_t state = sdk_runtime_critical_enter();
    if (s_runtime_state != H2_BK3633_SDK_RUNTIME_STATE_STARTED ||
        s_standby_state != H2_BK3633_SDK_STANDBY_IDLE) {
        sdk_runtime_critical_exit(state);
        return H2_PAL_ERR_INVALID_STATE;
    }
    s_standby_config = *config;
    s_standby_wake_reason = 0u;
    s_standby_state = H2_BK3633_SDK_STANDBY_REQUESTED;
    sdk_runtime_critical_exit(state);

    h2_pal_result_t result = s_runtime_config.record_wake(
        s_runtime_config.user, s_runtime_config.rwip_wait_key);
    if (result != H2_PAL_OK) {
        state = sdk_runtime_critical_enter();
        memset(&s_standby_config, 0, sizeof(s_standby_config));
        s_standby_state = H2_BK3633_SDK_STANDBY_IDLE;
        sdk_runtime_critical_exit(state);
        return result;
    }
    for (;;) {
        state = sdk_runtime_critical_enter();
        if (s_standby_state == H2_BK3633_SDK_STANDBY_COMPLETED) {
            *out_wake_reason = s_standby_wake_reason;
            memset(&s_standby_config, 0, sizeof(s_standby_config));
            s_standby_state = H2_BK3633_SDK_STANDBY_IDLE;
            sdk_runtime_critical_exit(state);
            return H2_PAL_OK;
        }
        sdk_runtime_critical_exit(state);
        result = s_runtime_config.wait_completion(
            s_runtime_config.user, (uintptr_t)&s_standby_wait_key,
            H2_LIBCO_WAIT_FOREVER);
        if (result != H2_PAL_OK) {
            return result;
        }
    }
}

int h2_bk3633_sdk_runtime_task(void *user)
{
    (void)user;
#if defined(BK3633_SDK_HAS_RWIP_GLUE) && BK3633_SDK_HAS_RWIP_GLUE
    if (s_runtime_state != H2_BK3633_SDK_RUNTIME_STATE_PLATFORM_READY) {
        return (int)H2_PAL_ERR_INVALID_STATE;
    }
    h2_pal_result_t result =
        h2_bk3633_sdk_runtime_configure_rom_environment(&s_runtime_config);
    if (result != H2_PAL_OK) {
        sdk_runtime_enter_failed_state();
        return (int)result;
    }
    s_runtime_state = H2_BK3633_SDK_RUNTIME_STATE_STARTING;
    s_application_init_result = H2_PAL_ERR_INVALID_STATE;
    rwip_init(0u);
    if (s_application_init_result != H2_PAL_OK) {
        s_runtime_state = H2_BK3633_SDK_RUNTIME_STATE_FAILED;
        return (int)s_application_init_result;
    }
    if (s_runtime_config.started != NULL) {
        result = s_runtime_config.started(s_runtime_config.user);
        if (result != H2_PAL_OK) {
            s_runtime_state = H2_BK3633_SDK_RUNTIME_STATE_FAILED;
            return (int)result;
        }
    }
    s_runtime_state = H2_BK3633_SDK_RUNTIME_STATE_STARTED;
    bool dispatch_pending = false;
    for (;;) {
        if (sdk_runtime_wake_failed()) {
            return (int)H2_PAL_ERR_FULL;
        }
        if (sdk_runtime_standby_requested()) {
            sdk_runtime_run_standby();
            h2_libco_result_t yield_result =
                h2_libco_yield(s_runtime_config.executor);
            if (yield_result == H2_LIBCO_ERR_CANCELLED) {
                return (int)H2_PAL_EXIT;
            }
            if (yield_result != H2_LIBCO_OK) {
                return (int)H2_PAL_ERR_INVALID_STATE;
            }
            continue;
        }
        uint32_t pending = sdk_runtime_pending_take();
        if (pending == 0u && !dispatch_pending) {
            h2_pal_result_t wait_result =
                s_runtime_config.wait_completion(
                s_runtime_config.user,
                s_runtime_config.rwip_wait_key,
                H2_LIBCO_WAIT_FOREVER);
            if (wait_result != H2_PAL_OK) {
                return (int)wait_result;
            }
            continue;
        }

        if (pending != 0u) {
            rwip_schedule();
        }
        dispatch_pending = false;
        if (s_runtime_config.dispatch_one != NULL) {
            result = s_runtime_config.dispatch_one(
                s_runtime_config.user, &dispatch_pending);
            if (result != H2_PAL_OK) {
                return (int)result;
            }
        }
        if (dispatch_pending || h2_bk3633_sdk_runtime_has_pending_work()) {
            h2_libco_result_t yield_result =
                h2_libco_yield(s_runtime_config.executor);
            if (yield_result == H2_LIBCO_ERR_CANCELLED) {
                return (int)H2_PAL_EXIT;
            }
            if (yield_result != H2_LIBCO_OK) {
                return (int)H2_PAL_ERR_INVALID_STATE;
            }
        }
    }
#else
    return (int)H2_PAL_ERR_UNAVAILABLE;
#endif
}

/*
 * BK3633 RWIP calls this fixed SDK ABI hook from rwip_init(). Keep the global
 * symbol in the SDK lifecycle component and delegate image policy through the
 * retained runtime configuration.
 */
void appm_init(void)
{
    if (s_runtime_state != H2_BK3633_SDK_RUNTIME_STATE_STARTING ||
        s_runtime_config.application_init == NULL) {
        s_application_init_result = H2_PAL_ERR_INVALID_STATE;
        return;
    }
    s_application_init_result =
        s_runtime_config.application_init(s_runtime_config.user);
}
