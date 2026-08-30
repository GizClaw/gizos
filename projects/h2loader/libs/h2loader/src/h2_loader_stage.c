#include "h2_loader_stage.h"

#include <stdio.h>
#include <string.h>

static void copy_text(char *out, size_t capacity, const char *value) {
    if (out == NULL || capacity == 0u) return;
    (void)snprintf(out, capacity, "%s", value != NULL ? value : "");
}

int h2_loader_stage_begin(const h2_pal_pref_api_t *pref) {
    const h2_loader_metadata_t invalid = {0};
    return h2_loader_metadata_write(
        pref, H2_LOADER_METADATA_SLOT_STAGE, &invalid);
}

int h2_loader_stage_publish(
    h2_loader_package_t *package,
    const h2_pal_pref_api_t *pref,
    uint64_t package_size,
    const char *package_checksum,
    h2_loader_metadata_t *out_stage) {
    h2_loader_package_inspection_t inspection;
    h2_pal_fs_stat_t stat;
    int rc;
    if (package == NULL || pref == NULL || package_checksum == NULL ||
        package_size == 0u || package->config.fs == NULL ||
        package->config.package_path == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2_pal_fs_stat(
        package->config.fs, package->config.package_path, &stat);
    if (rc != H2_PAL_FS_OK) return rc;
    if (stat.is_dir || stat.size != package_size) return H2_PAL_ERR_FORMAT;
    rc = h2_loader_package_verify_path(
        package,
        package->config.package_path,
        package_size,
        package_checksum);
    if (rc != H2_PAL_OK) return rc;
    rc = h2_loader_package_inspect_path(
        package, package->config.package_path, &inspection);
    if (rc != H2_PAL_OK) return rc;
    if (inspection.legacy || inspection.manifest.format == 0u) {
        return H2_PAL_ERR_FORMAT;
    }
    return h2_loader_stage_commit_inspection(
        pref, package_size, package_checksum, &inspection, out_stage);
}

int h2_loader_stage_commit_inspection(
    const h2_pal_pref_api_t *pref,
    uint64_t package_size,
    const char *package_checksum,
    const h2_loader_package_inspection_t *inspection,
    h2_loader_metadata_t *out_stage) {
    h2_loader_metadata_t stage = {0};
    int rc;

    if (pref == NULL || package_checksum == NULL || inspection == NULL ||
        package_size == 0u || inspection->legacy ||
        inspection->manifest.format == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    stage.package_size = package_size;
    stage.image_size = inspection->manifest.image_size;
    stage.role = inspection->manifest.role;
    copy_text(stage.package_checksum, sizeof(stage.package_checksum),
        package_checksum);
    copy_text(stage.image_checksum, sizeof(stage.image_checksum),
        inspection->manifest.image_sha256);
    copy_text(stage.version, sizeof(stage.version),
        inspection->manifest.version);
    copy_text(stage.board, sizeof(stage.board), inspection->manifest.board);
    copy_text(stage.target, sizeof(stage.target), inspection->manifest.target);
    stage.valid = 1;
    rc = h2_loader_metadata_write(
        pref, H2_LOADER_METADATA_SLOT_STAGE, &stage);
    if (rc != H2_PAL_OK) return rc;
    if (out_stage != NULL) *out_stage = stage;
    return H2_PAL_OK;
}

int h2_loader_stage_abort(
    const h2_pal_fs_api_t *fs,
    const h2_pal_pref_api_t *pref,
    const char *package_path) {
    h2_loader_metadata_t invalid = {0};
    int rc;
    int remove_rc;
    if (fs == NULL || pref == NULL || package_path == NULL ||
        package_path[0] == '\0') {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2_loader_metadata_write(
        pref, H2_LOADER_METADATA_SLOT_STAGE, &invalid);
    if (rc != H2_PAL_OK) return rc;
    remove_rc = h2_pal_fs_remove(fs, package_path);
    if (remove_rc != H2_PAL_OK && remove_rc != H2_PAL_FS_ERR_NOT_FOUND) {
        return remove_rc;
    }
    return h2_loader_metadata_clear(pref, H2_LOADER_METADATA_SLOT_STAGE);
}
