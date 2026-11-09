#ifndef H2_BUNDLE_ARCHIVE_H
#define H2_BUNDLE_ARCHIVE_H

#include "h2_bundle_installer.h"
#include "h2_bundle_segmented.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Stream a format-1 archive with workspace owned by installer->allocator.
 * I/O blocks and parser state stay off the caller's stack. Workspace allocation
 * failure returns NO_MEMORY before opening files or invoking writer callbacks.
 */
int h2_bundle_archive_install_zlib_tar(
    h2_bundle_installer_t* installer,
    const h2_bundle_install_options_t* options);

/**
 * Install a format-2 independent data tar stream. Only data/ entries are
 * accepted, in canonical path order. Verify their canonical SHA-256 before
 * committing installed_checksum_path. Failure can leave partial data; its
 * checksum is invalidated before replacement. Dependencies are borrowed.
 */
int h2_bundle_archive_install_data_zlib(
    h2_bundle_installer_t* installer, const h2_bundle_ota_options_t* options,
    h2_bundle_segmented_read_fn read, void* read_user,
    const h2_bundle_segmented_manifest_t* manifest,
    const h2_bundle_digest_api_t* digest);

#ifdef __cplusplus
}
#endif

#endif
