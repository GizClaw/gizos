#ifndef FAKE_NVDS_H
#define FAKE_NVDS_H

#include "h2_bk3633_pal_storage_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct fake_nvds_tag {
    uint8_t data[H2_BK3633_NVDS_VALUE_SIZE_MAX];
    uint8_t len;
    bool present;
} fake_nvds_tag_t;

typedef struct fake_nvds {
    fake_nvds_tag_t tags[256];
    h2_bk3633_nvds_status_t next_get_status;
    h2_bk3633_nvds_status_t next_put_status;
    h2_bk3633_nvds_status_t next_del_status;
} fake_nvds_t;

void fake_nvds_init(fake_nvds_t *fake);
const h2_bk3633_nvds_driver_t *fake_nvds_driver(fake_nvds_t *fake);
void fake_nvds_set_raw(
    fake_nvds_t *fake,
    uint8_t tag,
    const uint8_t *data,
    size_t len);

#endif
