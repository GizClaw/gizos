#ifndef H2_BUNDLE_SEGMENTED_H
#define H2_BUNDLE_SEGMENTED_H

#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_fs.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_BUNDLE_SEGMENTED_TEXT_SIZE 96u
#define H2_BUNDLE_SEGMENTED_SHA_SIZE 65u
#define H2_BUNDLE_SEGMENTED_MANIFEST_MAX 1024u

/** Borrowed synchronous SHA-256 implementation; one digest runs at a time. */
typedef struct h2_bundle_digest_api {
    void *user;
    int (*start)(void *user);
    int (*update)(void *user, const uint8_t *data, size_t len);
    int (*finish)(void *user, uint8_t digest[32]);
    void (*abort)(void *user);
} h2_bundle_digest_api_t;

/** Read bounded bytes at an absolute offset. Short nonempty reads are allowed. */
typedef int (*h2_bundle_segmented_read_fn)(
    void *user, uint64_t offset, uint8_t *data, size_t capacity,
    size_t *out_read);

/** Consume borrowed decompressed bytes synchronously; errors stop inflation. */
typedef int (*h2_bundle_segmented_write_fn)(
    void *user, const uint8_t *data, size_t len);

typedef struct h2_bundle_segment {
    uint64_t offset;
    uint64_t compressed_size;
    uint64_t size;
    char compressed_sha256[H2_BUNDLE_SEGMENTED_SHA_SIZE];
} h2_bundle_segment_t;

/** Format-2 identity and independent compressed member locations. */
typedef struct h2_bundle_segmented_manifest {
    char role[H2_BUNDLE_SEGMENTED_TEXT_SIZE];
    char board[H2_BUNDLE_SEGMENTED_TEXT_SIZE];
    char target[H2_BUNDLE_SEGMENTED_TEXT_SIZE];
    char version[H2_BUNDLE_SEGMENTED_TEXT_SIZE];
    char image_sha256[H2_BUNDLE_SEGMENTED_SHA_SIZE];
    char data_sha256[H2_BUNDLE_SEGMENTED_SHA_SIZE];
    uint64_t data_bytes;
    uint64_t pixa_bytes;
    h2_bundle_segment_t app;
    h2_bundle_segment_t data;
} h2_bundle_segmented_manifest_t;

/** Borrowed file adapter. Initialize position to the file's actual position. */
typedef struct h2_bundle_segmented_file {
    const h2_pal_fs_api_t *fs;
    h2_pal_fs_file_t *file;
    uint64_t position;
} h2_bundle_segmented_file_t;

int h2_bundle_segmented_file_read(
    void *user, uint64_t offset, uint8_t *data, size_t capacity, size_t *out_read);

/** Parse the canonical plaintext format-2 manifest; clear output on failure. */
int h2_bundle_segmented_manifest_parse(
    const uint8_t *data, size_t len,
    h2_bundle_segmented_manifest_t *out_manifest);

/**
 * Inspect exactly manifest, data.tar.zlib, app.bin.zlib followed by zero tar
 * termination/padding. Verify compressed member SHA-256 without inflating.
 * Reader and digest are borrowed for this synchronous call. Output is cleared
 * on failure. The caller independently verifies the complete package SHA-256.
 */
int h2_bundle_segmented_inspect(
    h2_bundle_segmented_read_fn read, void *read_user, uint64_t package_size,
    const h2_bundle_digest_api_t *digest,
    h2_bundle_segmented_manifest_t *out_manifest);

/**
 * Inflate exactly one independent zlib member through bounded buffers.
 * Reject truncation, trailing compressed bytes and a decompressed length that
 * differs from segment.size. No buffers are retained. Partial output can be
 * delivered before an error; the caller owns writer abort and raw digest checks.
 */
int h2_bundle_segmented_inflate(
    h2_bundle_segmented_read_fn read, void *read_user,
    const h2_bundle_segment_t *segment, const h2_pal_mem_api_t *allocator,
    h2_bundle_segmented_write_fn write, void *write_user);

int h2_bundle_digest_valid(const h2_bundle_digest_api_t *digest);
void h2_bundle_digest_hex(const uint8_t digest[32], char out_hex[65]);

#ifdef __cplusplus
}
#endif
#endif
