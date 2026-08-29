#ifndef H2_LOADER_PACKAGE_H
#define H2_LOADER_PACKAGE_H

#include "h2_bundle_installer.h"
#include "h2/pal/os/h2_pal_disk.h"
#include "h2/pal/os/h2_pal_fs.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_pref.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_LOADER_DEFAULT_PACKAGE_PATH "/dl/update.tar.zlib"
#define H2_LOADER_DEFAULT_DATA_ROOT "/data"
#define H2_LOADER_DEFAULT_CHECKSUM_PATH "/data/.checksum"
#define H2_LOADER_DEFAULT_APP_ENTRY_PATH "app/esp/app.bin"
#define H2_LOADER_IDENTITY_TEXT_MAX 96u
#define H2_LOADER_SHA256_HEX_SIZE 65u
#define H2_LOADER_PREF_NAMESPACE "h2loader"

typedef enum h2_loader_image_role {
    H2_LOADER_IMAGE_ROLE_UNKNOWN = 0,
    H2_LOADER_IMAGE_ROLE_APP = 1,
    H2_LOADER_IMAGE_ROLE_H2LOADER = 2,
} h2_loader_image_role_t;

typedef struct h2_loader_package_manifest {
    uint32_t format;
    h2_loader_image_role_t role;
    char board[H2_LOADER_IDENTITY_TEXT_MAX];
    char target[H2_LOADER_IDENTITY_TEXT_MAX];
    char version[H2_LOADER_IDENTITY_TEXT_MAX];
    uint64_t image_size;
    char image_sha256[H2_LOADER_SHA256_HEX_SIZE];
} h2_loader_package_manifest_t;

typedef h2_loader_package_manifest_t h2_loader_image_identity_t;

typedef enum h2_loader_install_phase {
    H2_LOADER_INSTALL_PHASE_VALIDATE = 1,
    H2_LOADER_INSTALL_PHASE_IMAGE = 2,
    H2_LOADER_INSTALL_PHASE_DATA = 3,
    H2_LOADER_INSTALL_PHASE_PIXA = 4,
    H2_LOADER_INSTALL_PHASE_VERIFY = 5,
    H2_LOADER_INSTALL_PHASE_DONE = 6,
} h2_loader_install_phase_t;

/**
 * Reports synchronous install progress from the task performing the install.
 * detail and user are borrowed and must not be retained by the callback.
 */
typedef void (*h2_loader_install_progress_fn)(
    void *user,
    h2_loader_install_phase_t phase,
    uint64_t completed,
    uint64_t total,
    const char *detail);

typedef struct h2_loader_identity {
    int valid;
    char version[H2_LOADER_IDENTITY_TEXT_MAX];
    char checksum[H2_LOADER_IDENTITY_TEXT_MAX];
    uint64_t size;
} h2_loader_identity_t;

typedef struct h2_loader_digest_api {
    void *user;
    int (*start)(void *user);
    int (*update)(void *user, const uint8_t *data, size_t len);
    int (*finish)(void *user, uint8_t out_digest[32]);
    void (*abort)(void *user);
} h2_loader_digest_api_t;

typedef struct h2_loader_image_reader_vtable {
    int (*get_capacity)(void *user, uint32_t partition_id, uint64_t *out_capacity);
    int (*read)(void *user, uint32_t partition_id, uint64_t offset, void *data, size_t len);
} h2_loader_image_reader_vtable_t;

typedef struct h2_loader_image_reader_api {
    /** Borrowed target capability; called synchronously by H2Loader Common. */
    void *user;
    const h2_loader_image_reader_vtable_t *vtable;
} h2_loader_image_reader_api_t;

typedef struct h2_loader_image_writer_vtable {
    int (*get_capacity)(void *user, uint32_t partition_id, uint64_t *out_capacity);
    int (*begin)(
        void *user,
        uint32_t partition_id,
        const h2_loader_image_identity_t *identity);
    int (*write)(void *user, const void *data, size_t len);
    int (*finish)(void *user, const h2_loader_image_identity_t *identity);
    void (*abort)(void *user);
} h2_loader_image_writer_vtable_t;

typedef struct h2_loader_image_writer_api {
    /** Borrowed target capability; called synchronously by H2Loader Common. */
    void *user;
    const h2_loader_image_writer_vtable_t *vtable;
} h2_loader_image_writer_api_t;

typedef struct h2_loader_package_inspection {
    int legacy;
    h2_loader_identity_t staged;
    h2_loader_package_manifest_t manifest;
    char image_path[H2_BUNDLE_PATH_MAX];
    /** Exact bytes stored in the package checksum entry. */
    uint8_t data_checksum[H2_LOADER_IDENTITY_TEXT_MAX];
    /** Number of valid bytes in data_checksum. */
    size_t data_checksum_len;
    uint64_t data_bytes;
    uint64_t pixa_bytes;
} h2_loader_package_inspection_t;

/** Per-component decisions computed before installing a format-1 package. */
typedef struct h2_loader_package_install_plan {
    /** Nonzero when the App partition must be written. */
    int update_app;
    /** Nonzero when the data namespace must be replaced. */
    int update_data;
} h2_loader_package_install_plan_t;

/** Records which component writes completed successfully. */
typedef struct h2_loader_package_install_result {
    /** Nonzero when the App writer completed. */
    int app_written;
    /** Nonzero when the data replacement completed. */
    int data_written;
} h2_loader_package_install_result_t;

typedef struct h2_loader_package_config {
    const h2_pal_fs_api_t *fs;
    const h2_pal_disk_api_t *disk;
    const h2_pal_mem_api_t *allocator;
    h2_loader_digest_api_t digest;
    const char *package_path;
    const char *data_root;
    const char *installed_checksum_path;
    const char *app_entry_path;
    const h2_bundle_app_writer_t *app_writer;
    const h2_loader_image_reader_api_t *image_reader;
    const h2_loader_image_writer_api_t *image_writer;
    h2_bundle_clear_data_fn clear_data;
    void *clear_data_user;
    uint32_t app_partition_id;
    void *progress_user;
    h2_loader_install_progress_fn progress;
} h2_loader_package_config_t;

typedef struct h2_loader_package {
    h2_loader_package_config_t config;
    h2_bundle_installer_t installer;
    uint64_t bundle_progress_bytes;
    uint64_t data_progress_bytes;
    uint64_t data_total_bytes;
    uint64_t pixa_progress_bytes;
    uint64_t pixa_total_bytes;
} h2_loader_package_t;

int h2_loader_package_init(h2_loader_package_t *package, const h2_loader_package_config_t *config);
/**
 * Removes interrupted replacement files without restoring an old candidate.
 *
 * A committed package remains available. An unpublished package, temporary
 * file, or previous-candidate file is discarded together with staged metadata.
 */
int h2_loader_package_recover_publish(
    const h2_pal_fs_api_t *fs,
    const h2_pal_pref_api_t *pref,
    const char *package_path,
    const char *previous_path);
int h2_loader_package_read_staged_identity(
    h2_loader_package_t *package,
    const h2_pal_pref_api_t *pref,
    h2_loader_identity_t *out_identity);
int h2_loader_package_validate_path(h2_loader_package_t *package, const char *archive_path);
int h2_loader_package_verify_path(
    h2_loader_package_t *package,
    const char *archive_path,
    uint64_t expected_size,
    const char *expected_sha256);
int h2_loader_package_inspect_path(
    h2_loader_package_t *package,
    const char *archive_path,
    h2_loader_package_inspection_t *out_inspection);
int h2_loader_package_manifest_parse(
    const void *data,
    size_t len,
    h2_loader_package_manifest_t *out_manifest);
int h2_loader_package_inspect(
    h2_loader_package_t *package,
    const h2_pal_pref_api_t *pref,
    h2_loader_package_inspection_t *out_inspection);
/**
 * Builds a read-only plan by comparing the package identities with installed
 * App partition bytes and /data/.checksum. No destination content is changed.
 */
int h2_loader_package_plan_install(
    h2_loader_package_t *package,
    const h2_loader_package_inspection_t *inspection,
    uint32_t destination_partition_id,
    h2_loader_package_install_plan_t *out_plan);
int h2_loader_package_install_staged(
    h2_loader_package_t *package,
    const h2_loader_identity_t *identity);
int h2_loader_package_install_to(
    h2_loader_package_t *package,
    const h2_loader_package_inspection_t *inspection,
    uint32_t destination_partition_id,
    const h2_loader_package_install_plan_t *plan,
    h2_loader_package_install_result_t *out_result);
int h2_loader_image_copy_to(
    h2_loader_package_t *package,
    const h2_loader_image_identity_t *identity,
    uint32_t source_partition_id,
    uint32_t destination_partition_id);
int h2_loader_image_verify(
    h2_loader_package_t *package,
    const h2_loader_image_identity_t *identity,
    uint32_t partition_id);
int h2_loader_package_identity_equal(
    const h2_loader_identity_t *a,
    const h2_loader_identity_t *b);

#ifdef __cplusplus
}
#endif

#endif
