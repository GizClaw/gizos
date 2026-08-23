#ifndef H2_LVGL_FS_INTERNAL_H
#define H2_LVGL_FS_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

static inline bool h2_lvgl_fs_relative_target(uint64_t base,
                                               uint32_t encoded_offset,
                                               uint32_t *out_target) {
    int64_t offset = encoded_offset <= (uint32_t)INT32_MAX
                         ? (int64_t)encoded_offset
                         : (int64_t)encoded_offset - ((int64_t)UINT32_MAX + 1);
    if (out_target == NULL || base > UINT32_MAX) {
        return false;
    }
    int64_t target = (int64_t)base + offset;
    if (target < 0 || target > (int64_t)UINT32_MAX) {
        return false;
    }
    *out_target = (uint32_t)target;
    return true;
}

#endif
