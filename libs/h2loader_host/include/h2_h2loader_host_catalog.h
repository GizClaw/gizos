#ifndef H2_H2LOADER_HOST_CATALOG_H
#define H2_H2LOADER_HOST_CATALOG_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_H2LOADER_HOST_IDENTITY_MAX_LEN 96u
#define H2_H2LOADER_HOST_RESOURCE_NAME_MAX_LEN 256u
#define H2_H2LOADER_HOST_SHA256_HEX_LEN 64u

typedef enum h2_h2loader_host_asset_role {
    H2_H2LOADER_HOST_ASSET_ROLE_APP = 1,
    H2_H2LOADER_HOST_ASSET_ROLE_LOADER = 2,
} h2_h2loader_host_asset_role_t;

typedef enum h2_h2loader_host_asset_operation {
    H2_H2LOADER_HOST_ASSET_OPERATION_MANAGED_INSTALL = 1,
    H2_H2LOADER_HOST_ASSET_OPERATION_RECOVERY = 2,
    H2_H2LOADER_HOST_ASSET_OPERATION_DIAGNOSTIC = 3,
} h2_h2loader_host_asset_operation_t;

typedef enum h2_h2loader_host_asset_identity_source {
    /** Asset identity includes the release-catalog image name. */
    H2_H2LOADER_HOST_ASSET_IDENTITY_RELEASE_CATALOG = 0,
    /** Standalone format-1 manifest; the package carries no image name. */
    H2_H2LOADER_HOST_ASSET_IDENTITY_PACKAGE_MANIFEST = 1,
} h2_h2loader_host_asset_identity_source_t;

/**
 * @brief One immutable validated release asset.
 *
 * The catalog owns all strings. resource_name is a safe relative path beneath
 * the caller's packaged resource root. An entry is returned only after its
 * schema, identity, byte length, path and SHA-256 have been validated.
 */
typedef struct h2_h2loader_host_catalog_entry {
    char board[H2_H2LOADER_HOST_IDENTITY_MAX_LEN];
    char target[H2_H2LOADER_HOST_IDENTITY_MAX_LEN];
    char image[H2_H2LOADER_HOST_IDENTITY_MAX_LEN];
    char version[H2_H2LOADER_HOST_IDENTITY_MAX_LEN];
    char resource_name[H2_H2LOADER_HOST_RESOURCE_NAME_MAX_LEN];
    /** SHA-256 of the packaged resource bytes. */
    char sha256[H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u];
    /** Device-reported image checksum from the package manifest. */
    char image_sha256[H2_H2LOADER_HOST_SHA256_HEX_LEN + 1u];
    uint64_t bytes;
    h2_h2loader_host_asset_role_t role;
    h2_h2loader_host_asset_operation_t operation;
    h2_h2loader_host_asset_identity_source_t identity_source;
} h2_h2loader_host_catalog_entry_t;

typedef struct h2_h2loader_host_catalog h2_h2loader_host_catalog_t;

/**
 * @brief Read bytes from one packaged resource.
 *
 * The callback must return H2_PAL_OK and set out_read to zero at EOF. It must
 * reject paths outside the immutable resource root. A successful read cannot
 * return more than out_size bytes.
 */
typedef h2_pal_result_t (*h2_h2loader_host_resource_read_fn)(
    void *user,
    const char *resource_name,
    uint64_t offset,
    uint8_t *out,
    size_t out_size,
    size_t *out_read);

typedef struct h2_h2loader_host_catalog_config {
    const h2_pal_mem_api_t *allocator;
    const uint8_t *index_json;
    size_t index_json_len;
    h2_h2loader_host_resource_read_fn read_resource;
    void *resource_user;
} h2_h2loader_host_catalog_config_t;

/**
 * @brief Parse and verify a version-1 firmware-index.json.
 *
 * The call is blocking and hashes every referenced resource through
 * read_resource. Call it from a worker before exposing any firmware choice.
 * index_json is borrowed only for this call. The returned catalog is owned by
 * the caller and remains immutable until close().
 */
h2_pal_result_t h2_h2loader_host_catalog_open(
    const h2_h2loader_host_catalog_config_t *config,
    h2_h2loader_host_catalog_t **out_catalog);

/** Close a catalog. Repeating the call with a NULL handle is a no-op. */
h2_pal_result_t h2_h2loader_host_catalog_close(
    h2_h2loader_host_catalog_t **inout_catalog);

h2_pal_result_t h2_h2loader_host_catalog_count(
    const h2_h2loader_host_catalog_t *catalog,
    size_t *out_count);

h2_pal_result_t h2_h2loader_host_catalog_get(
    const h2_h2loader_host_catalog_t *catalog,
    size_t index,
    h2_h2loader_host_catalog_entry_t *out_entry);

/**
 * @brief Find managed assets compatible with an authoritative live identity.
 *
 * The caller provides output storage. out_count is always the total number of
 * matches, even when out_indices is smaller; H2_PAL_ERR_NO_SPACE indicates
 * truncation. role and operation must be valid enum values.
 */
h2_pal_result_t h2_h2loader_host_catalog_find(
    const h2_h2loader_host_catalog_t *catalog,
    const char *board,
    const char *target,
    h2_h2loader_host_asset_role_t role,
    h2_h2loader_host_asset_operation_t operation,
    size_t *out_indices,
    size_t out_capacity,
    size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif
