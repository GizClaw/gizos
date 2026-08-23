#ifndef H2_H2LOADER_HOST_FACTORY_H
#define H2_H2LOADER_HOST_FACTORY_H

#include "h2_h2loader_host.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_H2LOADER_HOST_FACTORY_FILE_CAPACITY 16u
#define H2_H2LOADER_HOST_FACTORY_NAME_MAX_LEN 128u
#define H2_H2LOADER_HOST_FACTORY_HEADER_SIZE 3092u

typedef enum h2_h2loader_host_factory_driver {
    H2_H2LOADER_HOST_FACTORY_DRIVER_ESP_ROM = 1,
    H2_H2LOADER_HOST_FACTORY_DRIVER_BK7258_ROM = 2,
} h2_h2loader_host_factory_driver_t;

typedef struct h2_h2loader_host_factory_file {
    uint32_t flash_offset;
    uint64_t data_offset;
    uint64_t bytes;
    char sha256[H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u];
    char name[H2_H2LOADER_HOST_FACTORY_NAME_MAX_LEN];
} h2_h2loader_host_factory_file_t;

typedef struct h2_h2loader_host_factory_manifest {
    h2_h2loader_host_factory_driver_t driver;
    uint32_t flags;
    uint32_t baud;
    char board[H2_H2LOADER_HOST_IDENTITY_MAX_LEN];
    char target[H2_H2LOADER_HOST_IDENTITY_MAX_LEN];
    size_t file_count;
    h2_h2loader_host_factory_file_t
        files[H2_H2LOADER_HOST_FACTORY_FILE_CAPACITY];
} h2_h2loader_host_factory_manifest_t;

/**
 * @brief Parse and validate a version-1 H2 factory bundle.
 *
 * The outer catalog asset must already be size/SHA-256 validated. This call
 * additionally validates the embedded board/target/driver/file manifest,
 * every member range, and each member SHA-256 before raw recovery is exposed.
 */
h2_pal_result_t h2_h2loader_host_factory_open(
    const h2_h2loader_host_catalog_entry_t *asset,
    h2_h2loader_host_payload_read_fn read_payload,
    void *payload_user,
    h2_h2loader_host_factory_manifest_t *out_manifest);

/** Read bytes from one already validated member. */
h2_pal_result_t h2_h2loader_host_factory_read_member(
    const h2_h2loader_host_factory_manifest_t *manifest,
    size_t file_index,
    h2_h2loader_host_payload_read_fn read_payload,
    void *payload_user,
    uint64_t offset,
    uint8_t *out,
    size_t out_size,
    size_t *out_read);

#ifdef __cplusplus
}
#endif

#endif
