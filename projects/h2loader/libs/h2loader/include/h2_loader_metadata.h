#ifndef H2_LOADER_METADATA_H
#define H2_LOADER_METADATA_H

#include "h2_loader_package.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_pref.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_loader_metadata_slot {
    H2_LOADER_METADATA_SLOT_STAGE = 1,
    H2_LOADER_METADATA_SLOT_PARTITION_1 = 2,
    H2_LOADER_METADATA_SLOT_PARTITION_2 = 3,
} h2_loader_metadata_slot_t;

typedef struct h2_loader_metadata {
    int valid;
    char package_checksum[H2_LOADER_SHA256_HEX_SIZE];
    uint64_t package_size;
    char image_checksum[H2_LOADER_SHA256_HEX_SIZE];
    uint64_t image_size;
    h2_loader_image_role_t role;
    char version[H2_LOADER_IDENTITY_TEXT_MAX];
    char board[H2_LOADER_IDENTITY_TEXT_MAX];
    char target[H2_LOADER_IDENTITY_TEXT_MAX];
} h2_loader_metadata_t;

const char *h2_loader_metadata_slot_key(h2_loader_metadata_slot_t slot);

/** Validate one complete record according to its slot ownership. */
int h2_loader_metadata_validate(
    h2_loader_metadata_slot_t slot,
    const h2_loader_metadata_t *metadata);

/** Encode one record into the stable versioned Preference blob format. */
int h2_loader_metadata_encode(
    h2_loader_metadata_slot_t slot,
    const h2_loader_metadata_t *metadata,
    void *data,
    size_t capacity,
    size_t *out_len);

/** Decode and validate one stable Preference blob. */
int h2_loader_metadata_decode(
    h2_loader_metadata_slot_t slot,
    const void *data,
    size_t len,
    h2_loader_metadata_t *out_metadata);

/**
 * Read one slot. A missing key returns OK with out_present set to zero and an
 * all-zero invalid record.
 */
int h2_loader_metadata_read(
    const h2_pal_pref_api_t *pref,
    const h2_pal_mem_api_t *allocator,
    h2_loader_metadata_slot_t slot,
    h2_loader_metadata_t *out_metadata,
    int *out_present);

/** Atomically replace one complete slot record. */
int h2_loader_metadata_write(
    const h2_pal_pref_api_t *pref,
    h2_loader_metadata_slot_t slot,
    const h2_loader_metadata_t *metadata);

/** Remove one slot record atomically. */
int h2_loader_metadata_clear(
    const h2_pal_pref_api_t *pref,
    h2_loader_metadata_slot_t slot);

/** Copy the package and image identity from Stage into a Partition record. */
int h2_loader_metadata_from_stage(
    const h2_loader_metadata_t *stage,
    h2_loader_metadata_t *out_partition);

/** Compare the complete raw-image identity, excluding source package fields. */
int h2_loader_metadata_image_equal(
    const h2_loader_metadata_t *a,
    const h2_loader_metadata_t *b);

#ifdef __cplusplus
}
#endif

#endif
