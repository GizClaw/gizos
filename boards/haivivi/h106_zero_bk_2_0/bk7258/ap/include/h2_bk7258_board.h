#ifndef H2_BK7258_BOARD_H
#define H2_BK7258_BOARD_H

#include "h2_runtime.h"
#include "os/h2_pal_mem.h"
#include "h2_zero_bk_display_capture.h"

/* The shared Display launcher observes completed wire transfers through the
 * Board's diagnostic interface instead of reading an RGB scanout buffer. */
#define H2_BK7258_BOARD_DISPLAY_TRANSFER_CAPTURE \
  h2_zero_bk_display_set_transfer_capture
#define H2_BK7258_BOARD_DISPLAY_BACKLIGHT_PERCENT \
  h2_zero_bk_display_backlight_percent

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*h2_bk7258_board_entry_task_fn)(void* user);

h2_pal_result_t h2_bk7258_board_runtime_config(h2_runtime_config_t* out_config);

/** Configure a minimal recovery Runtime with native image/boot capabilities,
 * console transport and the board QSPI filesystem, without media startup.
 * Call during serialized image startup; output APIs live until board teardown.
 * Full configuration may reuse this filesystem. Call board_runtime_deinit()
 * after Runtime teardown, including a failed Runtime initialization. */
h2_pal_result_t h2_bk7258_board_recovery_runtime_config(
    h2_runtime_config_t* out_config);

/** Stable h106-zero-XXXXXX advertising name, or NULL if the MAC is unavailable.
 * Borrowed for the image lifetime. Call from the serialized board startup
 * before constructing either the product or minimal Loader Runtime. */
const char* h2_bk7258_board_ble_local_name(void);
/** Optional board hook for launchers also compiled with public board headers.
 */
#define H2_BK7258_BOARD_BLE_LOCAL_NAME h2_bk7258_board_ble_local_name

/**
 * Tears down board-owned Runtime providers after h2_runtime_deinit().
 *
 * All HTTP requests must have returned before this call. The teardown destroys
 * the HTTP provider before releasing its filesystem dependency. A caller whose
 * h2_runtime_init() fails after runtime_config() succeeds must also call this
 * function. Repeated calls after teardown return H2_PAL_OK.
 */
h2_pal_result_t h2_bk7258_board_runtime_deinit(void);

/**
 * Mounts the QSPI0 LittleFS volume and returns the `/dl` and `/data`
 * filesystem.
 *
 * Images that do not use the full board Runtime, such as the Loader, call this
 * directly. h2_bk7258_board_fs_deinit() unmounts the volume.
 */
int h2_bk7258_board_fs_init(h2_pal_fs_api_t* fs);
int h2_bk7258_board_fs_deinit(void);

/** Borrowed image-lifetime allocator for Display bitmap working sets.
 * Release allocations through this API; it uses this Board's PSRAM arena. */
const h2_pal_mem_api_t* h2_bk7258_board_display_allocator(void);

/** Board PSRAM allocator for large target-specific working sets. */
h2_pal_mem_api_t* h2_bk7258_board_psram_allocator(void);

/**
 * Starts the board image entry point on a detached board-owned worker task.
 *
 * The caller must return from the SDK user entry after this function succeeds.
 * The supplied user pointer must remain valid until the entry function returns.
 * The entry function must release Runtime before returning; the task wrapper
 * then calls h2_bk7258_board_runtime_deinit() before deleting itself.
 */
h2_pal_result_t h2_bk7258_board_start_entry_task(
    const char* name, h2_bk7258_board_entry_task_fn entry, void* user);

#ifdef __cplusplus
}
#endif

#endif
