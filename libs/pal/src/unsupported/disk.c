#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_disk_list_partitions(void *p0, h2_pal_disk_partition_cb_t p1, void *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_disk_get_partition(void *p0, uint32_t p1, h2_pal_disk_partition_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_disk_read(void *p0, uint32_t p1, uint64_t p2, void *p3, size_t p4) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_disk_erase(void *p0, uint32_t p1, uint64_t p2, uint64_t p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_disk_write(void *p0, uint32_t p1, uint64_t p2, const void *p3, size_t p4) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_disk_flush(void *p0, uint32_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_disk_vtable_t unsupported_disk_vtable = {
    .list_partitions = unsupported_disk_list_partitions,
    .get_partition = unsupported_disk_get_partition,
    .read = unsupported_disk_read,
    .erase = unsupported_disk_erase,
    .write = unsupported_disk_write,
    .flush = unsupported_disk_flush,
};
static const h2_pal_disk_api_t unsupported_disk_api = { .user = NULL, .vtable = &unsupported_disk_vtable };
const h2_pal_disk_api_t *h2_pal_unsupported_disk_api(void) { return &unsupported_disk_api; }
