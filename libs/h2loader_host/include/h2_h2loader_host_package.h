#ifndef H2_H2LOADER_HOST_PACKAGE_H
#define H2_H2LOADER_HOST_PACKAGE_H

#include "h2_h2loader_host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_h2loader_host_package_inspect_config {
    const h2_pal_mem_api_t *allocator;
    h2_h2loader_host_payload_read_fn read_payload;
    void *payload_user;
    uint64_t payload_bytes;
} h2_h2loader_host_package_inspect_config_t;

typedef h2_pal_result_t (*h2_h2loader_host_package_write_fn)(
    void *user,
    const uint8_t *data,
    size_t len);

typedef struct h2_h2loader_host_package_source {
    const char *name;
    uint64_t size;
    h2_h2loader_host_payload_read_fn read;
    void *user;
} h2_h2loader_host_package_source_t;

typedef struct h2_h2loader_host_package_writer_config {
    const h2_pal_mem_api_t *allocator;
    const char *role;
    const char *board;
    const char *target;
    const char *version;
    h2_h2loader_host_package_source_t app;
    const h2_h2loader_host_package_source_t *data_entries;
    size_t data_entry_count;
    h2_h2loader_host_package_write_fn write;
    void *write_user;
} h2_h2loader_host_package_writer_config_t;

typedef struct h2_h2loader_host_package_writer_result {
    uint64_t package_bytes;
    char image_sha256[H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u];
    char data_sha256[H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u];
} h2_h2loader_host_package_writer_result_t;

/** Write one canonical format-1 USTAR+zlib package through callbacks. */
h2_pal_result_t h2_h2loader_host_package_write(
    const h2_h2loader_host_package_writer_config_t *config,
    h2_h2loader_host_package_writer_result_t *out_result);

/**
 * @brief Inspect one standalone format-1 update package.
 *
 * The inspector reads bounded chunks through read_payload, validates the
 * zlib/USTAR layout and manifest, and returns an immutable managed asset.
 * Format 1 does not carry an App image name, so the returned entry has an
 * empty image and PACKAGE_MANIFEST identity source. No package bytes are
 * retained after this call.
 */
h2_pal_result_t h2_h2loader_host_package_inspect(
    const h2_h2loader_host_package_inspect_config_t *config,
    h2_h2loader_host_catalog_entry_t *out_asset);

#ifdef __cplusplus
}
#endif

#endif
