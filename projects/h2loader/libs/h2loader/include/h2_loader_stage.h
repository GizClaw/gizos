#ifndef H2_LOADER_STAGE_H
#define H2_LOADER_STAGE_H

#include "h2_loader_metadata.h"
#include "h2_loader_package.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Persist an invalid Stage record before replacing any candidate bytes. */
int h2_loader_stage_begin(const h2_pal_pref_api_t *pref);

/**
 * Validate the published package and commit its complete Stage metadata. The
 * valid record is written only after file size, package identity, manifest,
 * and raw image identity have all been validated.
 */
int h2_loader_stage_publish(
    h2_loader_package_t *package,
    const h2_pal_pref_api_t *pref,
    uint64_t package_size,
    const char *package_checksum,
    h2_loader_metadata_t *out_stage);

/** Remove the published package and its Stage metadata. */
int h2_loader_stage_abort(
    const h2_pal_fs_api_t *fs,
    const h2_pal_pref_api_t *pref,
    const char *package_path);

#ifdef __cplusplus
}
#endif

#endif
