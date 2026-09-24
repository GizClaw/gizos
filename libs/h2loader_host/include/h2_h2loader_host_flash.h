#ifndef H2_H2LOADER_HOST_FLASH_H
#define H2_H2LOADER_HOST_FLASH_H

#include "h2_h2loader_host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_h2loader_host_recovery_reason {
    H2_H2LOADER_HOST_RECOVERY_NONE = 0,
    H2_H2LOADER_HOST_RECOVERY_PROBE_FAILED = 1,
    H2_H2LOADER_HOST_RECOVERY_BLANK_FIXTURE = 2,
} h2_h2loader_host_recovery_reason_t;

typedef struct h2_h2loader_host_recovery_authorization {
    h2_h2loader_host_transport_t transport;
    h2_h2loader_host_recovery_reason_t reason;
    h2_pal_result_t probe_result;
    uint64_t probe_completed_ms;
    uint64_t expires_ms;
    uint8_t probe_attempts;
    uint8_t identity_confirmed;
    uint8_t destructive_confirmed;
} h2_h2loader_host_recovery_authorization_t;

typedef struct h2_h2loader_host_flash_driver_vtable {
    h2_pal_result_t (*prepare)(
        void *user,
        const h2_h2loader_host_catalog_entry_t *asset,
        h2_h2loader_host_payload_read_fn read_payload,
        void *payload_user);
    h2_pal_result_t (*erase)(
        void *user,
        h2_h2loader_host_cancelled_fn is_cancelled,
        void *cancel_user);
    h2_pal_result_t (*write)(
        void *user,
        const h2_h2loader_host_catalog_entry_t *asset,
        h2_h2loader_host_payload_read_fn read_payload,
        void *payload_user,
        h2_h2loader_host_cancelled_fn is_cancelled,
        void *cancel_user,
        h2_h2loader_host_progress_fn on_progress,
        void *progress_user);
    h2_pal_result_t (*verify)(
        void *user,
        h2_h2loader_host_cancelled_fn is_cancelled,
        void *cancel_user);
    h2_pal_result_t (*reset_to_loader)(void *user);
    h2_pal_result_t (*close)(void *user);
} h2_h2loader_host_flash_driver_vtable_t;

typedef struct h2_h2loader_host_flash_driver {
    void *user;
    const h2_h2loader_host_flash_driver_vtable_t *vtable;
} h2_h2loader_host_flash_driver_t;

typedef enum h2_h2loader_host_esp_boot_policy {
    /** Operator/fixture has already placed the target in ROM download mode. */
    H2_H2LOADER_HOST_ESP_BOOT_MANUAL = 1,
    /** Explicitly use supported DTR/RTS lines for ESP download/reset. */
    H2_H2LOADER_HOST_ESP_BOOT_DTR_RTS = 2,
} h2_h2loader_host_esp_boot_policy_t;

typedef struct h2_h2loader_host_esp_flash_config {
    const h2_pal_serial_host_api_t *serial;
    const h2_pal_time_api_t *time;
    const h2_pal_mem_api_t *allocator;
    const char *port_id;
    const char *expected_target;
    h2_h2loader_host_esp_boot_policy_t boot_policy;
} h2_h2loader_host_esp_flash_config_t;

/**
 * @brief Open the official Espressif C ROM/stub flasher over Host Serial PAL.
 *
 * The returned driver is process-global because esp-serial-flasher's port
 * callbacks are global. A second simultaneous ESP raw driver returns
 * H2_PAL_ERR_UNAVAILABLE. Normal managed serial sessions are unaffected.
 */
h2_pal_result_t h2_h2loader_host_esp_flash_open(
    const h2_h2loader_host_esp_flash_config_t *config,
    h2_h2loader_host_flash_driver_t *out_driver);

typedef struct h2_h2loader_host_bk7258_flash_config {
    const h2_pal_serial_host_api_t *serial;
    const h2_pal_time_api_t *time;
    const h2_pal_mem_api_t *allocator;
    const char *port_id;
    const char *expected_target;
    uint8_t connect_attempts;
} h2_h2loader_host_bk7258_flash_config_t;

/**
 * @brief Open the BK7258 ROM loader over Host Serial PAL.
 *
 * The operator or fixture must place the target in BK download mode. This
 * driver never toggles DTR/RTS and never invokes an SDK, Python runtime, or
 * subprocess. It validates a BK7258 factory bundle before connecting, erases
 * the current 8 MiB BK7258 board flash in 64 KiB blocks, writes 4 KiB
 * sectors, and verifies member SHA-256 through ROM readback.
 */
h2_pal_result_t h2_h2loader_host_bk7258_flash_open(
    const h2_h2loader_host_bk7258_flash_config_t *config,
    h2_h2loader_host_flash_driver_t *out_driver);

/**
 * @brief Validate the fail-closed authorization for destructive recovery.
 *
 * Recovery is serial-only, requires a validated recovery asset, two explicit
 * confirmations, a non-expired authorization, and either a recorded bounded
 * two independent H2Loader probe timeouts or an explicit documented
 * blank-fixture path with a recorded H2Loader probe timeout.
 * Permission, busy, argument, allocation and generic I/O errors do not prove
 * that H2Loader is absent and therefore cannot authorize recovery.
 */
h2_pal_result_t h2_h2loader_host_recovery_validate(
    const h2_h2loader_host_recovery_authorization_t *authorization,
    const h2_h2loader_host_catalog_entry_t *asset,
    uint64_t now_ms);

/**
 * @brief Execute prepare, erase, write, verify and reset through a driver.
 *
 * prepare() must validate the complete factory manifest and every member
 * before erase() is called. Driver success is only the raw recovery
 * checkpoint. The caller must
 * rediscover and authoritatively verify the new Loader before reporting final
 * success or installing an App through the managed path. Once a structurally
 * valid driver is passed, this function owns and closes it on every return
 * path, including authorization failure and cancellation.
 */
h2_pal_result_t h2_h2loader_host_recovery_run(
    const h2_h2loader_host_recovery_authorization_t *authorization,
    const h2_h2loader_host_catalog_entry_t *asset,
    uint64_t now_ms,
    const h2_h2loader_host_flash_driver_t *driver,
    h2_h2loader_host_payload_read_fn read_payload,
    void *payload_user,
    h2_h2loader_host_cancelled_fn is_cancelled,
    void *cancel_user,
    h2_h2loader_host_progress_fn on_progress,
    void *progress_user);

#ifdef __cplusplus
}
#endif

#endif
