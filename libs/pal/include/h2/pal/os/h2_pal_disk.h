#ifndef H2_PAL_DISK_H
#define H2_PAL_DISK_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PAL_DISK_PARTITION_NAME_MAX 32u

typedef enum h2_pal_disk_partition_flag {
    H2_PAL_DISK_PARTITION_FLAG_NONE = 0,
    H2_PAL_DISK_PARTITION_FLAG_READABLE = 1u << 0,
    H2_PAL_DISK_PARTITION_FLAG_WRITABLE = 1u << 1,
    H2_PAL_DISK_PARTITION_FLAG_ERASABLE = 1u << 2,
    H2_PAL_DISK_PARTITION_FLAG_BOOTABLE = 1u << 3,
} h2_pal_disk_partition_flag_t;

typedef struct h2_pal_disk_partition {
    uint32_t id;
    uint32_t flags;
    uint64_t size;
    uint32_t erase_block_size;
    uint32_t write_alignment;
    char name[H2_PAL_DISK_PARTITION_NAME_MAX];
} h2_pal_disk_partition_t;

typedef h2_pal_result_t (*h2_pal_disk_partition_cb_t)(
    void *user,
    const h2_pal_disk_partition_t *partition);

typedef struct h2_pal_disk_vtable {
    h2_pal_result_t (*list_partitions)(
        void *user,
        h2_pal_disk_partition_cb_t cb,
        void *cb_user);
    h2_pal_result_t (*get_partition)(
        void *user,
        uint32_t partition_id,
        h2_pal_disk_partition_t *out_partition);
    h2_pal_result_t (*read)(
        void *user,
        uint32_t partition_id,
        uint64_t offset,
        void *data,
        size_t len);
    h2_pal_result_t (*erase)(
        void *user,
        uint32_t partition_id,
        uint64_t offset,
        uint64_t len);
    h2_pal_result_t (*write)(
        void *user,
        uint32_t partition_id,
        uint64_t offset,
        const void *data,
        size_t len);
    h2_pal_result_t (*flush)(void *user, uint32_t partition_id);
} h2_pal_disk_vtable_t;

typedef struct h2_pal_disk_api {
    void *user;
    const h2_pal_disk_vtable_t *vtable;
} h2_pal_disk_api_t;

static inline h2_pal_result_t h2_pal_disk_list_partitions(
    const h2_pal_disk_api_t *api,
    h2_pal_disk_partition_cb_t cb,
    void *cb_user) {
    if (cb == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->list_partitions == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->list_partitions(api->user, cb, cb_user);
}

static inline h2_pal_result_t h2_pal_disk_get_partition(
    const h2_pal_disk_api_t *api,
    uint32_t partition_id,
    h2_pal_disk_partition_t *out_partition) {
    if (out_partition == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->get_partition == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->get_partition(api->user, partition_id, out_partition);
}

static inline h2_pal_result_t h2_pal_disk_read(
    const h2_pal_disk_api_t *api,
    uint32_t partition_id,
    uint64_t offset,
    void *data,
    size_t len) {
    if (data == NULL && len != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->read == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->read(api->user, partition_id, offset, data, len);
}

static inline h2_pal_result_t h2_pal_disk_erase(
    const h2_pal_disk_api_t *api,
    uint32_t partition_id,
    uint64_t offset,
    uint64_t len) {
    if (api == NULL || api->vtable == NULL || api->vtable->erase == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->erase(api->user, partition_id, offset, len);
}

static inline h2_pal_result_t h2_pal_disk_write(
    const h2_pal_disk_api_t *api,
    uint32_t partition_id,
    uint64_t offset,
    const void *data,
    size_t len) {
    if (data == NULL && len != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->write == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->write(api->user, partition_id, offset, data, len);
}

static inline h2_pal_result_t h2_pal_disk_flush(
    const h2_pal_disk_api_t *api,
    uint32_t partition_id) {
    if (api == NULL || api->vtable == NULL || api->vtable->flush == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->flush(api->user, partition_id);
}

#ifdef __cplusplus
}
#endif

#endif
