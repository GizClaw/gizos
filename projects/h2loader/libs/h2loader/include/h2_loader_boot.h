#ifndef H2_LOADER_BOOT_H
#define H2_LOADER_BOOT_H

#include "h2_loader_package.h"
#include "h2/pal/hal/h2_pal_power.h"
#include "h2/pal/os/h2_pal_pref.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_loader_capability {
    H2_LOADER_CAP_STATUS = UINT32_C(1) << 0,
    H2_LOADER_CAP_STAGE = UINT32_C(1) << 1,
    H2_LOADER_CAP_UPGRADE = UINT32_C(1) << 2,
    H2_LOADER_CAP_REBOOT = UINT32_C(1) << 3,
    H2_LOADER_CAP_RESTART = UINT32_C(1) << 4,
    H2_LOADER_CAP_ROLLBACK = UINT32_C(1) << 5,
    H2_LOADER_CAP_HOLD = UINT32_C(1) << 6,
    H2_LOADER_CAP_COREDUMP = UINT32_C(1) << 7,
} h2_loader_capability_t;

typedef enum h2_loader_command_availability {
    H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP = UINT32_C(1) << 0,
    H2_LOADER_COMMAND_AVAILABLE_REBOOT_LOADER = UINT32_C(1) << 1,
} h2_loader_command_availability_t;

#define H2_LOADER_COMMAND_AVAILABILITY_ALL              \
    (H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP |           \
     H2_LOADER_COMMAND_AVAILABLE_REBOOT_LOADER)

#define H2_LOADER_CAPABILITIES_ALL                                        \
    (H2_LOADER_CAP_STATUS | H2_LOADER_CAP_STAGE | H2_LOADER_CAP_UPGRADE | \
     H2_LOADER_CAP_REBOOT | H2_LOADER_CAP_RESTART |                       \
     H2_LOADER_CAP_ROLLBACK | H2_LOADER_CAP_HOLD |                        \
     H2_LOADER_CAP_COREDUMP)

#define H2_LOADER_CAPABILITIES_LOADER                                      \
    (H2_LOADER_CAP_STATUS | H2_LOADER_CAP_STAGE | H2_LOADER_CAP_UPGRADE | \
     H2_LOADER_CAP_REBOOT | H2_LOADER_CAP_HOLD | H2_LOADER_CAP_COREDUMP)
#define H2_LOADER_CAPABILITIES_APP                                      \
    (H2_LOADER_CAP_STATUS | H2_LOADER_CAP_RESTART |                    \
     H2_LOADER_CAP_ROLLBACK | H2_LOADER_CAP_COREDUMP)

typedef enum h2_loader_boot_intent {
    H2_LOADER_BOOT_INTENT_UNKNOWN = 0,
    H2_LOADER_BOOT_INTENT_H2LOADER = 1,
    H2_LOADER_BOOT_INTENT_APP = 2,
} h2_loader_boot_intent_t;

typedef enum h2_loader_install_state {
    H2_LOADER_INSTALL_STATE_IDLE = 0,
    H2_LOADER_INSTALL_STATE_STAGED = 1,
    H2_LOADER_INSTALL_STATE_INSTALL_REQUESTED = 2,
    H2_LOADER_INSTALL_STATE_INSTALLING = 3,
    H2_LOADER_INSTALL_STATE_INSTALLED_PENDING_CONFIRM = 4,
    H2_LOADER_INSTALL_STATE_CONFIRMED = 5,
    H2_LOADER_INSTALL_STATE_INSTALL_FAILED = 6,
    H2_LOADER_INSTALL_STATE_RETURN_REQUESTED = 7,
    H2_LOADER_INSTALL_STATE_MAIN_FAILED = 8,
} h2_loader_install_state_t;

typedef enum h2_loader_startup_action {
    H2_LOADER_STARTUP_ACTION_COMMAND_MODE = 0,
    H2_LOADER_STARTUP_ACTION_REBOOTING_APP = 1,
    H2_LOADER_STARTUP_ACTION_REBOOTING_H2LOADER = 2,
} h2_loader_startup_action_t;

typedef enum h2_loader_startup_event {
    H2_LOADER_STARTUP_EVENT_INSTALL_BEGIN = 1,
    H2_LOADER_STARTUP_EVENT_INSTALL_SKIP_SAME_IDENTITY = 2,
    H2_LOADER_STARTUP_EVENT_BOOT_APP = 3,
    H2_LOADER_STARTUP_EVENT_MAIN_FAILED = 4,
    H2_LOADER_STARTUP_EVENT_INSTALL_FAILED = 5,
} h2_loader_startup_event_t;

typedef enum h2_loader_disruptive_action {
    H2_LOADER_DISRUPTIVE_BOOT_H2LOADER = 1,
    H2_LOADER_DISRUPTIVE_UPGRADE_H2LOADER = 2,
    H2_LOADER_DISRUPTIVE_BOOT_APP = 3,
} h2_loader_disruptive_action_t;

typedef enum h2_loader_upgrade_phase {
    H2_LOADER_UPGRADE_PHASE_IDLE = 0,
    H2_LOADER_UPGRADE_PHASE_TRIAL_PENDING = 1,
    H2_LOADER_UPGRADE_PHASE_TRIAL_RUNNING = 2,
    H2_LOADER_UPGRADE_PHASE_CANONICAL_PENDING = 3,
    H2_LOADER_UPGRADE_PHASE_FAILED = 4,
    H2_LOADER_UPGRADE_PHASE_CORRUPT = 5,
} h2_loader_upgrade_phase_t;

#define H2_LOADER_MFG_STEP_TOTAL 22u

typedef enum h2_loader_mfg_step_status {
    H2_LOADER_MFG_STEP_UNTESTED = 0,
    H2_LOADER_MFG_STEP_PASSED = 1,
    H2_LOADER_MFG_STEP_SKIPPED = 2,
    H2_LOADER_MFG_STEP_FAILED = 3,
} h2_loader_mfg_step_status_t;

typedef struct h2_loader_mfg_summary {
    /** Zero only when MFG is disabled; otherwise exactly 22. */
    uint32_t total;
    uint8_t step_status[H2_LOADER_MFG_STEP_TOTAL];
} h2_loader_mfg_summary_t;

#if defined(_MSC_VER)
typedef volatile long h2_loader_atomic_flag_t;
#else
typedef volatile int h2_loader_atomic_flag_t;
#endif

typedef struct h2_loader_upgrade_record {
    uint32_t format;
    h2_loader_upgrade_phase_t phase;
    char package_sha256[H2_LOADER_SHA256_HEX_SIZE];
    h2_loader_image_identity_t candidate;
    uint32_t canonical_partition;
    uint32_t trial_partition;
    int32_t last_result;
} h2_loader_upgrade_record_t;

/** Called synchronously after the trial partition is selected, before reboot. */
typedef int (*h2_loader_upgrade_transition_fn)(void *user);
/**
 * Called synchronously after a Loader reboot request is committed and before
 * disruptive teardown and reboot. The user pointer is borrowed for the call.
 */
typedef int (*h2_loader_reboot_transition_fn)(void *user);
/**
 * Called synchronously after a staged App request is atomically committed and
 * before disruptive teardown. The user pointer is borrowed for the call.
 */
typedef int (*h2_loader_install_transition_fn)(void *user);

typedef struct h2_loader_status {
    h2_loader_boot_intent_t boot_intent;
    h2_loader_install_state_t install_state;
    /** Persistent safety gate set only after the running App confirms. */
    int app_confirmed;
    int manual_hold;
    int last_result;
    char board[H2_LOADER_IDENTITY_TEXT_MAX];
    char target[H2_LOADER_IDENTITY_TEXT_MAX];
    char chip[H2_LOADER_IDENTITY_TEXT_MAX];
    char active_role[H2_LOADER_IDENTITY_TEXT_MAX];
    char active_name[H2_LOADER_IDENTITY_TEXT_MAX];
    char active_version[H2_LOADER_IDENTITY_TEXT_MAX];
    char active_checksum[H2_LOADER_IDENTITY_TEXT_MAX];
    uint32_t capabilities;
    uint32_t command_availability;
    h2_loader_identity_t installed;
    h2_loader_identity_t staged;
    uint32_t running_partition_id;
    uint32_t next_partition_id;
    h2_loader_upgrade_record_t loader_upgrade;
    char loader_upgrade_step[H2_LOADER_IDENTITY_TEXT_MAX];
    h2_loader_mfg_summary_t mfg;
} h2_loader_status_t;

/** Returns nonzero when an install result must enter App confirmation. */
int h2_loader_install_requires_app_confirmation(
    int app_written,
    int app_confirmed);

typedef struct h2_loader_config {
    h2_loader_package_config_t package;
    const h2_pal_pref_api_t *pref;
    const h2_pal_power_api_t *power;
    const char *board;
    /** Optional previous board identity accepted during a controlled migration. */
    const char *accepted_board_alias;
    const char *target;
    const char *chip;
    uint32_t h2loader_partition_id;
    uint32_t app_partition_id;
    uint32_t mfg_required_total;
    uint32_t capabilities;
    h2_loader_image_identity_t active_identity;
    void *confirm_user;
    int (*confirm_active_image)(void *user);
    void *mount_user;
    int (*mount_file_point)(void *user, const char *path);
    void *event_user;
    void (*on_event)(void *user, h2_loader_startup_event_t event, int code);
    void *disruptive_user;
    int (*before_disruptive)(void *user, h2_loader_disruptive_action_t action);
} h2_loader_config_t;

typedef struct h2_loader {
    h2_loader_config_t config;
    h2_loader_package_t package;
    h2_loader_status_t status;
    int force_command_mode;
    /** Volatile current-boot bypass; never serialized into MFG status. */
    h2_loader_atomic_flag_t mfg_gate_bypass;
    /** Volatile product-owned capability gates; never persisted. */
    h2_loader_atomic_flag_t capability_availability;
    /** Volatile product-owned command gates; never persisted. */
    h2_loader_atomic_flag_t command_availability;
} h2_loader_t;

/** Enables or revokes the volatile MFG App gate bypass for this boot. */
int h2_loader_set_mfg_gate_bypass(h2_loader_t *loader, int enabled);
/**
 * Atomically makes one or more implemented capabilities available or
 * unavailable for this boot.
 *
 * This mask can only restrict config.capabilities. All defined capabilities
 * start available for backward compatibility.
 */
int h2_loader_set_capability_availability(
    h2_loader_t *loader,
    uint32_t capabilities,
    bool available);
/** Returns the currently available subset of config.capabilities. */
uint32_t h2_loader_get_available_capabilities(const h2_loader_t *loader);
/**
 * Atomically sets or clears one or more product-owned command gates.
 *
 * The flags are an additional restriction and never bypass Loader validation.
 * All currently defined flags start set for backward compatibility.
 */
int h2_loader_set_command_availability(
    h2_loader_t *loader,
    uint32_t flags,
    bool available);

const char *h2_loader_boot_intent_name(h2_loader_boot_intent_t intent);
const char *h2_loader_install_state_name(h2_loader_install_state_t state);
const char *h2_loader_upgrade_phase_name(h2_loader_upgrade_phase_t phase);
int h2_loader_upgrade_record_encode(
    const h2_loader_upgrade_record_t *record,
    void *data,
    size_t capacity,
    size_t *out_len);
int h2_loader_upgrade_record_decode(
    const void *data,
    size_t len,
    h2_loader_upgrade_record_t *out_record);
/** Reads the canonical MFG blob; absence returns OK with out_present set to 0. */
int h2_loader_mfg_read(
    const h2_pal_pref_api_t *pref,
    const h2_pal_mem_api_t *allocator,
    h2_loader_mfg_summary_t *out_summary,
    int *out_present);
/** Atomically writes and commits one validated canonical MFG snapshot. */
int h2_loader_mfg_write(
    const h2_pal_pref_api_t *pref,
    const h2_loader_mfg_summary_t *summary);
/** Replaces the MFG snapshot with RUNNING 0/total. */
int h2_loader_mfg_reset(
    const h2_pal_pref_api_t *pref,
    uint32_t total);
/**
 * Resets the MFG snapshot when the product acceptance contract changes.
 *
 * The revision marker is committed only after RUNNING 0/total is durable, so
 * power loss cannot preserve an obsolete PASSED snapshot with a new revision.
 */
int h2_loader_mfg_ensure_acceptance_revision(
    const h2_pal_pref_api_t *pref,
    uint32_t total,
    uint32_t required_revision);
int h2_loader_init(h2_loader_t *loader, const h2_loader_config_t *config);
int h2_loader_startup(h2_loader_t *loader, h2_loader_startup_action_t *out_action);
int h2_loader_mark_return_requested(const h2_pal_pref_api_t *pref);
int h2_loader_mark_app_confirmed(const h2_pal_pref_api_t *pref);
/**
 * Invalidates the current staged candidate before receiving its replacement.
 *
 * temporary_path and previous_path are borrowed for this call. Missing files
 * are ignored. App install state, confirmation, hold, result, and Loader
 * upgrade state are preserved. Legacy STAGED lifecycle records are normalized
 * to the installed App state because a staged candidate is not an install
 * transition.
 */
int h2_loader_begin_stage_replacement(
    h2_loader_t *loader,
    const char *temporary_path,
    const char *previous_path);
int h2_loader_prepare_stage_publish(h2_loader_t *loader);
int h2_loader_publish_stage(h2_loader_t *loader, uint32_t bytes, const char *sha256);
int h2_loader_abort_stage(h2_loader_t *loader);
/** Requests staged App installation without running startup on this task. */
int h2_loader_request_install_staged(h2_loader_t *loader);
/**
 * Atomically commits manual_hold=0, install state, and App boot intent before
 * reporting the accepted transition, then runs disruptive preparation.
 *
 * transition and transition_user are borrowed for this call. A non-null
 * transition is called at most once. Callback or later teardown failure does
 * not roll back the committed request.
 */
int h2_loader_request_install_staged_with_transition(
    h2_loader_t *loader,
    h2_loader_install_transition_fn transition,
    void *transition_user);
int h2_loader_install_staged(h2_loader_t *loader);
/**
 * Atomically commits a staged App request, reports the accepted transition,
 * then runs startup, including disruptive preparation, on the current task.
 *
 * transition and transition_user are borrowed for this call. A non-null
 * transition is called at most once. Callback or startup failure does not roll
 * back the committed request.
 */
int h2_loader_install_staged_with_transition(
    h2_loader_t *loader,
    h2_loader_install_transition_fn transition,
    void *transition_user);
/** Persists result and updates the loader's in-memory status on success. */
int h2_loader_set_last_result(h2_loader_t *loader, int result);
int h2_loader_set_hold(h2_loader_t *loader, int enabled);
int h2_loader_boot_app(h2_loader_t *loader);
int h2_loader_boot_h2loader(h2_loader_t *loader);
/** Selects the canonical Loader partition and reboots, even when already on it. */
int h2_loader_reboot_h2loader(h2_loader_t *loader);
/**
 * Selects the canonical Loader partition when needed, commits Loader boot
 * intent, reports the accepted transition, then runs disruptive teardown and
 * reboots.
 *
 * transition and transition_user are borrowed for this call. A non-null
 * transition is called at most once. Callback, teardown, or reboot failure does
 * not roll back the committed selection or intent.
 */
int h2_loader_reboot_h2loader_with_transition(
    h2_loader_t *loader,
    h2_loader_reboot_transition_fn transition,
    void *transition_user);
/** Installs a staged Loader to the trial partition and starts copy-back. */
int h2_loader_upgrade_start(h2_loader_t *loader);
/**
 * Starts Loader copy-back and reports the accepted transition before reboot.
 *
 * transition and transition_user are borrowed for this call. transition is
 * called only after the upgrade record is committed and the trial partition
 * has been selected successfully. A callback error cancels the reboot and is
 * persisted as an upgrade failure.
 */
int h2_loader_upgrade_start_with_transition(
    h2_loader_t *loader,
    h2_loader_upgrade_transition_fn transition,
    void *transition_user);
int h2_loader_read_pref_status(
    const h2_pal_pref_api_t *pref,
    const h2_pal_mem_api_t *allocator,
    h2_loader_status_t *out_status);
int h2_loader_read_status(h2_loader_t *loader, h2_loader_status_t *out_status);
#ifdef __cplusplus
}
#endif

#endif
