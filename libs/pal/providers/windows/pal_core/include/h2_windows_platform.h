#ifndef H2_WINDOWS_PLATFORM_H
#define H2_WINDOWS_PLATFORM_H

#include "h2_pal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_windows_platform h2_windows_platform_t;

typedef struct h2_windows_platform_config {
    const char *const *fs_sources;
    const char *const *fs_targets;
    size_t fs_mount_count;
} h2_windows_platform_config_t;

/** Create a Windows PAL context and copy all configuration strings. */
h2_pal_result_t h2_windows_platform_create(
    const h2_windows_platform_config_t *config,
    h2_windows_platform_t **out_platform);

/**
 * Create a host context that mounts each accessible local DOS drive at the
 * lower-case portable path matching its drive letter, for example C:\ at /c.
 */
h2_pal_result_t h2_windows_platform_create_with_logical_drives(
    h2_windows_platform_t **out_platform);

/**
 * Destroy a Windows PAL context after all borrowed capabilities are idle.
 * A NULL caller pointer is a successful no-op. On failure the context remains
 * live and the caller retains ownership.
 */
h2_pal_result_t h2_windows_platform_destroy(
    h2_windows_platform_t **platform);

const h2_pal_mem_api_t *h2_windows_mem_api(h2_windows_platform_t *platform);
const h2_pal_log_api_t *h2_windows_log_api(h2_windows_platform_t *platform);
const h2_pal_time_api_t *h2_windows_time_api(h2_windows_platform_t *platform);
const h2_pal_timer_api_t *h2_windows_timer_api(h2_windows_platform_t *platform);
const h2_pal_task_api_t *h2_windows_task_api(h2_windows_platform_t *platform);
const h2_pal_queue_api_t *h2_windows_queue_api(h2_windows_platform_t *platform);
const h2_pal_sync_api_t *h2_windows_sync_api(h2_windows_platform_t *platform);
const h2_pal_fs_api_t *h2_windows_fs_api(h2_windows_platform_t *platform);
const h2_pal_net_api_t *h2_windows_net_api(h2_windows_platform_t *platform);
const h2_pal_netif_api_t *h2_windows_netif_api(h2_windows_platform_t *platform);
const h2_pal_system_event_api_t *h2_windows_system_event_api(
    h2_windows_platform_t *platform);

/** Fill an entropy buffer with the Windows system-preferred RNG. */
int h2_windows_entropy(void *user, uint8_t *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif
