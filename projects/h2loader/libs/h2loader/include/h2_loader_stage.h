#ifndef H2_LOADER_STAGE_H
#define H2_LOADER_STAGE_H

#include "h2_loader_metadata.h"
#include "h2_loader_package.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Streaming App-package transaction. Zero-initialize before first use. The
 * caller holds the shared Loader operation mutex from begin through commit or
 * abort; all calls run on that owning task. Package and Pref are borrowed for
 * the transaction lifetime. No image/partition or /data writes occur here.
 * Treat fields as private; inspect returns a borrowed immutable manifest.
 */
typedef struct h2_loader_stage_writer {
    h2_loader_package_t *package;
    const h2_pal_pref_api_t *pref;
    h2_pal_fs_file_t *file;
    uint64_t expected;
    uint64_t written;
    char checksum[65];
    const char *board;
    const char *target;
    h2_loader_package_inspection_t inspection;
    unsigned percent;
    int started;
    int verified;
} h2_loader_stage_writer_t;

/** Begin replacing Stage. On any failure call abort before releasing the lock. */
int h2_loader_stage_writer_begin(
    h2_loader_stage_writer_t *writer, h2_loader_package_t *package,
    const h2_pal_pref_api_t *pref, uint64_t size, const char *checksum,
    const char *board, const char *target);
/** Write complete chunks; short writes fail. out_percent counts accepted bytes. */
int h2_loader_stage_writer_write(
    h2_loader_stage_writer_t *writer, const uint8_t *data, size_t size,
    unsigned *out_percent);
/** Flush, close and validate size, digest, App manifest and device identity. */
int h2_loader_stage_writer_inspect(
    h2_loader_stage_writer_t *writer,
    const h2_loader_package_inspection_t **out_inspection);
/** Publish verified Stage after the caller's product policy/journal succeeds. */
int h2_loader_stage_writer_commit(h2_loader_stage_writer_t *writer);
/** Close partial writes and invalidate owned Stage; safe before begin succeeds. */
int h2_loader_stage_writer_abort(h2_loader_stage_writer_t *writer);

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

/**
 * Commit Stage metadata for package bytes that were already checksum-verified
 * and inspected before being published at the canonical package path.
 *
 * The caller must publish exactly the inspected package before calling this
 * function. This is the final Pref transaction of a Stage replacement and is
 * the only transaction that writes valid=true.
 */
int h2_loader_stage_commit_inspection(
    const h2_pal_pref_api_t *pref,
    uint64_t package_size,
    const char *package_checksum,
    const h2_loader_package_inspection_t *inspection,
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
