#ifndef H2_LVGL_FS_H
#define H2_LVGL_FS_H

#include "h2/pal/os/h2_pal_fs.h"
#include "h2/pal/os/h2_pal_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_lvgl_fs h2_lvgl_fs_t;

typedef struct h2_lvgl_fs_config {
    const h2_pal_fs_api_t *fs;
    const h2_pal_mem_api_t *allocator;
    const char *root;
    char drive_letter;
    uint32_t cache_size;
} h2_lvgl_fs_config_t;

/**
 * @brief Register one LVGL drive backed by a borrowed PAL filesystem.
 *
 * LVGL must already be initialized. The returned adapter owns only its driver
 * state and open-file wrappers; the PAL APIs and root string remain borrowed.
 */
h2_pal_result_t h2_lvgl_fs_register(const h2_lvgl_fs_config_t *config,
                                    h2_lvgl_fs_t **out_fs);

/**
 * @brief Release adapter state after LVGL has been deinitialized.
 *
 * All LVGL users of the drive, including file-backed fonts, must be destroyed
 * before `lv_deinit()` is called. The caller must then call `lv_deinit()` so
 * LVGL releases its filesystem driver-list nodes before calling this function.
 *
 * This function does not call LVGL and is safe to call with NULL.
 */
void h2_lvgl_fs_release(h2_lvgl_fs_t *fs);

#ifdef __cplusplus
}
#endif

#endif
