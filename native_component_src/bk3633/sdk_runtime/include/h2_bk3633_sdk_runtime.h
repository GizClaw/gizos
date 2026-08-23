#ifndef H2_BK3633_SDK_RUNTIME_H
#define H2_BK3633_SDK_RUNTIME_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2_libco.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef h2_pal_result_t (*h2_bk3633_sdk_platform_prepare_fn)(void *user);
typedef void (*h2_bk3633_sdk_fatal_reset_fn)(void *user, uint32_t error);
typedef h2_pal_result_t (*h2_bk3633_sdk_record_wake_fn)(
    void *user, uintptr_t wait_key);
typedef h2_pal_result_t (*h2_bk3633_sdk_wait_completion_fn)(
    void *user, uintptr_t wait_key, uint32_t timeout_ms);
typedef h2_pal_result_t (*h2_bk3633_sdk_configure_rom_environment_fn)(
    void *user);
typedef h2_pal_result_t (*h2_bk3633_sdk_application_init_fn)(void *user);
typedef h2_pal_result_t (*h2_bk3633_sdk_dispatch_one_fn)(
    void *user, bool *out_more_work);
typedef h2_pal_result_t (*h2_bk3633_sdk_started_fn)(void *user);
typedef bool (*h2_bk3633_sdk_nvds_oversize_is_missing_fn)(
    void *user, uint8_t tag, size_t stored_size, size_t requested_capacity);

typedef bool (*h2_bk3633_sdk_ble_standby_poll_wake_fn)(
    void *user, uint32_t *out_reason);

/**
 * Target-owned semantic wake poll for one RWIP-retaining low-voltage standby.
 *
 * The callback executes on the BLE Stack task, which owns execution without
 * yielding to other cooperative tasks until it selects a wake. The wake reason
 * is opaque to SDK Runtime.
 */
typedef struct h2_bk3633_sdk_ble_standby_config {
    void *user;
    h2_bk3633_sdk_ble_standby_poll_wake_fn poll_wake;
} h2_bk3633_sdk_ble_standby_config_t;

typedef struct h2_bk3633_sdk_runtime_config {
    /** Opaque context forwarded to every configured callback. */
    void *user;
    /** Borrowed executor that owns the BLE Stack task. */
    h2_libco_t *executor;
    /** Required borrowed image allocator used by the extended NVDS ABI. */
    const h2_pal_mem_api_t *mem;
    /** Nonzero private key used only by the BLE Stack task. */
    uintptr_t rwip_wait_key;
    /** Prepares board-selected clocks, watchdog, UART and GPIO resources. */
    h2_bk3633_sdk_platform_prepare_fn platform_prepare;
    /** Performs the target reset requested by an unrecoverable SDK failure. */
    h2_bk3633_sdk_fatal_reset_fn fatal_reset;
    /** Record a root-dispatched wake without switching from SDK context. */
    h2_bk3633_sdk_record_wake_fn record_wake;
    /** Suspend the BLE Stack task until its root-dispatched wake is delivered. */
    h2_bk3633_sdk_wait_completion_fn wait_completion;
    /** Applies project profile hooks after the default ROM environment. */
    h2_bk3633_sdk_configure_rom_environment_fn configure_rom_environment;
    /**
     * Installs the image-owned TASK_APP dispatcher and queues its GAPM
     * bootstrap. rwip_init() invokes this synchronously through appm_init().
     */
    h2_bk3633_sdk_application_init_fn application_init;
    /** Applies target clock/RF policy after rwip_init() returns. */
    h2_bk3633_sdk_started_fn started;
    /**
     * Optional image migration policy for an NVDS value larger than the
     * caller-provided buffer. Returning true exposes that value as missing so
     * its application owner can recreate it in the current schema.
     */
    h2_bk3633_sdk_nvds_oversize_is_missing_fn nvds_oversize_is_missing;
    /** Dispatches one copied event and reports whether another turn is due. */
    h2_bk3633_sdk_dispatch_one_fn dispatch_one;
} h2_bk3633_sdk_runtime_config_t;

/*
 * Prepare board-selected SDK resources, flash and NVDS. The configuration is
 * copied and retained by the singleton BK3633 RWIP runtime. This one-way
 * lifecycle has no shutdown operation and rejects repeated initialization.
 */
h2_pal_result_t h2_bk3633_sdk_runtime_platform_init(
    const h2_bk3633_sdk_runtime_config_t *config);

/** Compact obsolete or malformed NVDS tail records before App allocation. */
h2_pal_result_t h2_bk3633_sdk_runtime_nvds_maintain(void);

/** Native libco entry that exclusively owns rwip_init() and rwip_schedule(). */
int h2_bk3633_sdk_runtime_task(void *user);

/** Root-idle predicate; true means the BLE Stack task must not sleep. */
bool h2_bk3633_sdk_runtime_has_pending_work(void);

/**
 * Retain RWIP while all non-BLE cooperative tasks remain suspended.
 *
 * This synchronous operation must be called from a non-BLE task owned by the
 * configured executor. It returns after poll_wake() selects a semantic wake;
 * the caller still owns target and Board restore after this operation.
 */
h2_pal_result_t h2_bk3633_sdk_runtime_ble_standby(
    const h2_bk3633_sdk_ble_standby_config_t *config,
    uint32_t *out_wake_reason);

#ifdef __cplusplus
}
#endif

#endif
