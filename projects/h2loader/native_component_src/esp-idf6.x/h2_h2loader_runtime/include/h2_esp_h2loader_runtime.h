#ifndef H2_ESP_H2LOADER_RUNTIME_H
#define H2_ESP_H2LOADER_RUNTIME_H

#include "h2_loader_boot.h"
#include "h2_loader_memory.h"
#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*h2_esp_h2loader_mount_fn)(const char *path);

/** Board-owned callbacks and MFG policy borrowed for the Loader lifetime. */
typedef struct h2_esp_h2loader_config {
    const char *board;
    h2_esp_h2loader_mount_fn mount_file_point;
    uint32_t mfg_required_total;
    void *user;
    int (*run_mfg)(void *user, h2_loader_t *loader,
                   const h2_pal_sync_api_t *operation_sync,
                   h2_pal_mutex_t *operation_mutex,
                   const h2_pal_sync_api_t *wifi_operation_sync,
                   h2_pal_mutex_t *wifi_operation_mutex);
    void (*rearm_mfg)(void *user);
    int (*before_disruptive)(void *user, h2_loader_disruptive_action_t action);
    void (*install_progress)(void *user, h2_loader_install_phase_t phase,
                             uint64_t completed, uint64_t total,
                             const char *detail);
    void (*show_installing)(void *user);
    void (*show_install_failed)(void *user, int code);
    void (*show_launching)(void *user);
} h2_esp_h2loader_config_t;

/** Runs the blocking ESP H2Loader with an optional board MFG gate and UI. */
void h2_esp_h2loader_run_with_config(
    h2_runtime_t *runtime,
    const h2_esp_h2loader_config_t *config);
/** Runs the blocking ESP H2Loader without a board-specific MFG gate. */
void h2_esp_h2loader_run(
    h2_runtime_t *runtime,
    const char *board,
    h2_esp_h2loader_mount_fn mount_file_point);

/** Confirms the running ESP App image and persists the H2Loader App state. */
int h2_esp_h2loader_app_confirm(h2_runtime_t *runtime);

/** Reads the ESP heap regions exposed by the H2Loader memory command. */
h2_pal_result_t h2_esp_h2loader_memory_stats_read(
    void *user,
    h2_loader_memory_stats_t *out_stats);

/** Reads the running ESP image length and verified SHA-256 identity. */
int h2_esp_h2loader_current_image_identity(
    h2_loader_image_role_t role,
    const char *board,
    const char *target,
    const char *version,
    h2_loader_image_identity_t *out_identity);

/** Shared streaming SHA-256 adapter used by APP and Loader commands. */
h2_loader_digest_api_t h2_esp_h2loader_digest_api(void);

#ifdef __cplusplus
}
#endif

#endif
