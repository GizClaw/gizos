#ifndef H2_LOADER_BOOT_H
#define H2_LOADER_BOOT_H

#include "h2_loader_metadata.h"
#include "h2_loader_package.h"
#include "h2/pal/hal/h2_pal_power.h"
#include "h2/pal/os/h2_pal_pref.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_loader_capability {
    H2_LOADER_CAPABILITY_UART = UINT32_C(1) << 0,
    H2_LOADER_CAPABILITY_WIFI = UINT32_C(1) << 1,
    H2_LOADER_CAPABILITY_BLE = UINT32_C(1) << 2,
} h2_loader_capability_t;

#define H2_LOADER_CAPABILITIES_ALL \
    (H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI | \
     H2_LOADER_CAPABILITY_BLE)

typedef enum h2_loader_command_availability {
    H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP = UINT32_C(1) << 0,
    H2_LOADER_COMMAND_AVAILABLE_REBOOT_LOADER = UINT32_C(1) << 1,
    H2_LOADER_COMMAND_AVAILABLE_HELP = UINT32_C(1) << 2,
    H2_LOADER_COMMAND_AVAILABLE_STATUS = UINT32_C(1) << 3,
    H2_LOADER_COMMAND_AVAILABLE_STATS = UINT32_C(1) << 4,
    H2_LOADER_COMMAND_AVAILABLE_MEMORY = UINT32_C(1) << 5,
    H2_LOADER_COMMAND_AVAILABLE_COREDUMP_STATUS = UINT32_C(1) << 8,
    H2_LOADER_COMMAND_AVAILABLE_COREDUMP_DUMP = UINT32_C(1) << 9,
    H2_LOADER_COMMAND_AVAILABLE_COREDUMP_ERASE = UINT32_C(1) << 10,
    H2_LOADER_COMMAND_AVAILABLE_STAGE_PAYLOAD = UINT32_C(1) << 11,
    H2_LOADER_COMMAND_AVAILABLE_STAGE_ABORT = UINT32_C(1) << 12,
    H2_LOADER_COMMAND_AVAILABLE_STAGE_URL = UINT32_C(1) << 13,
    H2_LOADER_COMMAND_AVAILABLE_WIFI_SCAN = UINT32_C(1) << 16,
    H2_LOADER_COMMAND_AVAILABLE_WIFI_CONNECT = UINT32_C(1) << 17,
    H2_LOADER_COMMAND_AVAILABLE_WIFI_DISCONNECT = UINT32_C(1) << 18,
    H2_LOADER_COMMAND_AVAILABLE_REBOOT_UPGRADE = UINT32_C(1) << 19,
} h2_loader_command_availability_t;

#define H2_LOADER_COMMAND_AVAILABILITY_ALL \
    (H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP | \
     H2_LOADER_COMMAND_AVAILABLE_REBOOT_LOADER | \
     H2_LOADER_COMMAND_AVAILABLE_REBOOT_UPGRADE | \
     H2_LOADER_COMMAND_AVAILABLE_HELP | \
     H2_LOADER_COMMAND_AVAILABLE_STATUS | \
     H2_LOADER_COMMAND_AVAILABLE_STATS | \
     H2_LOADER_COMMAND_AVAILABLE_MEMORY | \
     H2_LOADER_COMMAND_AVAILABLE_COREDUMP_STATUS | \
     H2_LOADER_COMMAND_AVAILABLE_COREDUMP_DUMP | \
     H2_LOADER_COMMAND_AVAILABLE_COREDUMP_ERASE | \
     H2_LOADER_COMMAND_AVAILABLE_STAGE_PAYLOAD | \
     H2_LOADER_COMMAND_AVAILABLE_STAGE_ABORT | \
     H2_LOADER_COMMAND_AVAILABLE_STAGE_URL | \
     H2_LOADER_COMMAND_AVAILABLE_WIFI_SCAN | \
     H2_LOADER_COMMAND_AVAILABLE_WIFI_CONNECT | \
     H2_LOADER_COMMAND_AVAILABLE_WIFI_DISCONNECT)

typedef enum h2_loader_active_role {
    H2_LOADER_ACTIVE_ROLE_UNKNOWN = 0,
    H2_LOADER_ACTIVE_ROLE_H2LOADER = 1,
    H2_LOADER_ACTIVE_ROLE_APP = 2,
} h2_loader_active_role_t;

typedef enum h2_loader_mfg_mode {
    H2_LOADER_MFG_MODE_UNKNOWN = 0,
    H2_LOADER_MFG_MODE_DISABLED = 1,
    H2_LOADER_MFG_MODE_ENABLED = 2,
} h2_loader_mfg_mode_t;

typedef enum h2_loader_boot_intent {
    H2_LOADER_BOOT_INTENT_LOADER = 1,
    H2_LOADER_BOOT_INTENT_AUTO = 2,
} h2_loader_boot_intent_t;

typedef enum h2_loader_startup_action {
    H2_LOADER_STARTUP_ACTION_COMMAND_MODE = 0,
    H2_LOADER_STARTUP_ACTION_REBOOTING_APP = 1,
    H2_LOADER_STARTUP_ACTION_REBOOTING_H2LOADER = 2,
} h2_loader_startup_action_t;

typedef enum h2_loader_startup_event {
    H2_LOADER_STARTUP_EVENT_WRITE_PARTITION_2 = 1,
    H2_LOADER_STARTUP_EVENT_COPY_PARTITION_1 = 2,
    H2_LOADER_STARTUP_EVENT_BOOT_PARTITION_2 = 3,
    H2_LOADER_STARTUP_EVENT_STAGE_FINISHED = 4,
    H2_LOADER_STARTUP_EVENT_RECOVERY_FAILED = 5,
} h2_loader_startup_event_t;

typedef enum h2_loader_disruptive_action {
    H2_LOADER_DISRUPTIVE_REBOOT_LOADER = 1,
    H2_LOADER_DISRUPTIVE_REBOOT_UPGRADE = 2,
    H2_LOADER_DISRUPTIVE_REBOOT_APP = 3,
} h2_loader_disruptive_action_t;

#define H2_LOADER_MFG_STEP_TOTAL 22u

typedef enum h2_loader_mfg_step_status {
    H2_LOADER_MFG_STEP_UNTESTED = 0,
    H2_LOADER_MFG_STEP_PASSED = 1,
    H2_LOADER_MFG_STEP_SKIPPED = 2,
    H2_LOADER_MFG_STEP_FAILED = 3,
} h2_loader_mfg_step_status_t;

typedef struct h2_loader_mfg_summary {
    uint32_t total;
    uint8_t step_status[H2_LOADER_MFG_STEP_TOTAL];
} h2_loader_mfg_summary_t;

#if defined(_MSC_VER)
typedef volatile long h2_loader_atomic_flag_t;
#else
typedef volatile int h2_loader_atomic_flag_t;
#endif

typedef int (*h2_loader_reboot_transition_fn)(void *user);

typedef struct h2_loader_status {
    h2_loader_boot_intent_t boot_intent;
    h2_loader_metadata_t stage;
    h2_loader_metadata_t partition_1;
    h2_loader_metadata_t partition_2;
    int last_result;
    char board[H2_LOADER_IDENTITY_TEXT_MAX];
    char target[H2_LOADER_IDENTITY_TEXT_MAX];
    char chip[H2_LOADER_IDENTITY_TEXT_MAX];
    h2_loader_active_role_t active_role;
    char active_name[H2_LOADER_IDENTITY_TEXT_MAX];
    char active_version[H2_LOADER_IDENTITY_TEXT_MAX];
    char active_checksum[H2_LOADER_SHA256_HEX_SIZE];
    uint64_t active_image_size;
    uint32_t capabilities;
    uint32_t command_availability;
    uint32_t running_partition_id;
    uint32_t next_partition_id;
    h2_loader_mfg_summary_t mfg;
} h2_loader_status_t;

typedef struct h2_loader_config {
    h2_loader_package_config_t package;
    const h2_pal_pref_api_t *pref;
    const h2_pal_power_api_t *power;
    const char *board;
    const char *accepted_board_alias;
    const char *target;
    const char *chip;
    uint32_t h2loader_partition_id;
    uint32_t app_partition_id;
    uint32_t mfg_required_total;
    uint32_t hardware_capabilities;
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
    h2_loader_atomic_flag_t mfg_gate_bypass;
    h2_loader_atomic_flag_t implemented_commands;
    h2_loader_atomic_flag_t command_availability;
} h2_loader_t;

int h2_loader_set_mfg_gate_bypass(h2_loader_t *loader, int enabled);
int h2_loader_set_implemented_commands(h2_loader_t *loader, uint32_t commands);
int h2_loader_set_command_availability(
    h2_loader_t *loader, uint32_t flags, bool available);
uint32_t h2_loader_get_command_availability(
    const h2_loader_t *loader, const h2_loader_status_t *status);

const char *h2_loader_boot_intent_name(h2_loader_boot_intent_t intent);
int h2_loader_mfg_read(
    const h2_pal_pref_api_t *pref,
    const h2_pal_mem_api_t *allocator,
    h2_loader_mfg_summary_t *out_summary,
    int *out_present);
int h2_loader_mfg_write(
    const h2_pal_pref_api_t *pref,
    const h2_loader_mfg_summary_t *summary);
int h2_loader_mfg_reset(const h2_pal_pref_api_t *pref, uint32_t total);
int h2_loader_mfg_ensure_acceptance_revision(
    const h2_pal_pref_api_t *pref,
    uint32_t total,
    uint32_t required_revision);

int h2_loader_init(h2_loader_t *loader, const h2_loader_config_t *config);
int h2_loader_startup(h2_loader_t *loader, h2_loader_startup_action_t *out_action);
int h2_loader_begin_stage(
    h2_loader_t *loader,
    const char *temporary_path,
    const char *previous_path);
int h2_loader_commit_stage(
    h2_loader_t *loader, uint64_t bytes, const char *sha256);
int h2_loader_commit_inspected_stage(
    h2_loader_t *loader,
    uint64_t bytes,
    const char *sha256,
    const h2_loader_package_inspection_t *inspection);
int h2_loader_cancel_stage(h2_loader_t *loader);
int h2_loader_finalize_active_app(
    const h2_pal_pref_api_t *pref,
    const h2_pal_mem_api_t *allocator,
    const h2_pal_fs_api_t *fs,
    const char *package_path,
    const h2_loader_image_identity_t *active_identity,
    uint32_t running_partition_id,
    uint32_t app_partition_id);
int h2_loader_set_last_result(h2_loader_t *loader, int result);
int h2_loader_reboot_h2loader_with_transition(
    h2_loader_t *loader,
    h2_loader_reboot_transition_fn transition,
    void *transition_user);
int h2_loader_reboot_app_with_transition(
    h2_loader_t *loader,
    h2_loader_reboot_transition_fn transition,
    void *transition_user);
int h2_loader_reboot_upgrade_with_transition(
    h2_loader_t *loader,
    h2_loader_reboot_transition_fn transition,
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
