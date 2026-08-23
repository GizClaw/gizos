#ifndef H2_BK3633_PAL_STORAGE_INTERNAL_H
#define H2_BK3633_PAL_STORAGE_INTERNAL_H

#include "h2_bk3633_platform_core.h"

#include <stddef.h>
#include <stdint.h>

typedef enum h2_bk3633_nvds_status {
    H2_BK3633_NVDS_STATUS_OK = 0,
    H2_BK3633_NVDS_STATUS_FAIL = 1,
    H2_BK3633_NVDS_STATUS_NOT_FOUND = 2,
    H2_BK3633_NVDS_STATUS_NO_SPACE = 3,
    H2_BK3633_NVDS_STATUS_LENGTH = 4,
    H2_BK3633_NVDS_STATUS_LOCKED = 5,
    H2_BK3633_NVDS_STATUS_CORRUPT = 6,
} h2_bk3633_nvds_status_t;

typedef struct h2_bk3633_nvds_driver {
    h2_bk3633_nvds_status_t (*get)(
        void *user,
        uint8_t tag,
        uint8_t *in_out_len,
        uint8_t *data);
    h2_bk3633_nvds_status_t (*put)(
        void *user,
        uint8_t tag,
        uint8_t len,
        const uint8_t *data);
    h2_bk3633_nvds_status_t (*del)(void *user, uint8_t tag);
    void *user;
} h2_bk3633_nvds_driver_t;

typedef struct h2_bk3633_flash_driver {
    h2_pal_result_t (*read)(
        void *user,
        uint32_t address,
        void *data,
        size_t len);
    h2_pal_result_t (*erase)(
        void *user,
        uint32_t address,
        size_t len);
    h2_pal_result_t (*write)(
        void *user,
        uint32_t address,
        const void *data,
        size_t len);
    void *user;
} h2_bk3633_flash_driver_t;

h2_pal_result_t h2_bk3633_nvds_pref_init_with_driver(
    const h2_bk3633_nvds_pref_entry_t *entries,
    size_t entry_count,
    const h2_bk3633_nvds_driver_t *driver);

h2_pal_result_t h2_bk3633_flash_disk_init_with_driver(
    const h2_bk3633_flash_partition_config_t *partitions,
    size_t partition_count,
    const h2_bk3633_flash_driver_t *driver);

#endif
