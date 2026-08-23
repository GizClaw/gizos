#ifndef H2_BUNDLE_INSTALLER_H
#define H2_BUNDLE_INSTALLER_H

#include "h2_bundle_manifest.h"
#include "h2_bundle_types.h"
#include "h2/pal/os/h2_pal_fs.h"
#include "h2/pal/os/h2_pal_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_bundle_installer {
    const h2_pal_fs_api_t *fs;
    const h2_pal_mem_api_t *allocator;
    h2_bundle_progress_fn progress;
    void *progress_user;
    h2_bundle_install_stats_t stats;
} h2_bundle_installer_t;

typedef struct h2_bundle_app_writer {
    void *user;
    int (*begin)(void *user, const h2_bundle_entry_t *entry);
    int (*write)(void *user, const h2_bundle_entry_t *entry, const void *data, size_t len);
    int (*end)(void *user, const h2_bundle_entry_t *entry);
    void (*abort)(void *user);
} h2_bundle_app_writer_t;

typedef int (*h2_bundle_clear_data_fn)(void *user, const char *data_root);

typedef struct h2_bundle_install_options {
    const char *archive_path;
    const char *dst_root;
    const h2_bundle_manifest_t *manifest;
    const char *installed_version_path;
    int ota_layout;
    const char *installed_checksum_path;
    const h2_bundle_app_writer_t *app_writer;
    h2_bundle_clear_data_fn clear_data;
    void *clear_data_user;
    /** Validate app entries without invoking app_writer. */
    int skip_app_install;
    /** Validate data entries without changing data_root or its checksum. */
    int skip_data_install;
} h2_bundle_install_options_t;

typedef struct h2_bundle_ota_options {
    const char *archive_path;
    const char *data_root;
    const char *installed_checksum_path;
    const h2_bundle_app_writer_t *app_writer;
    h2_bundle_clear_data_fn clear_data;
    void *clear_data_user;
    /** Validate app entries without invoking app_writer. */
    int skip_app_install;
    /** Validate data entries without changing data_root or its checksum. */
    int skip_data_install;
} h2_bundle_ota_options_t;

int h2_bundle_installer_init(h2_bundle_installer_t *installer, const h2_pal_fs_api_t *fs, const h2_pal_mem_api_t *allocator);
void h2_bundle_installer_set_progress(h2_bundle_installer_t *installer, h2_bundle_progress_fn progress, void *user);
int h2_bundle_install(h2_bundle_installer_t *installer, const h2_bundle_install_options_t *options);
int h2_bundle_install_archive(h2_bundle_installer_t *installer, const char *archive_path, const char *dst_root);
int h2_bundle_install_ota(h2_bundle_installer_t *installer, const h2_bundle_ota_options_t *options);
int h2_bundle_installed_version_matches(const h2_pal_fs_api_t *fs, const char *installed_version_path, const char *version, int *out_matches);

#ifdef __cplusplus
}
#endif

#endif
