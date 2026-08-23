#ifndef H2_BUNDLE_MANIFEST_H
#define H2_BUNDLE_MANIFEST_H

#include "h2_bundle_types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_bundle_manifest_entry {
    char path[H2_BUNDLE_PATH_MAX];
    char checksum_hex[65];
    uint64_t size;
} h2_bundle_manifest_entry_t;

typedef struct h2_bundle_manifest {
    const char *version;
    const h2_bundle_manifest_entry_t *entries;
    size_t entry_count;
} h2_bundle_manifest_t;

int h2_bundle_manifest_parse_line(const char *line, h2_bundle_manifest_entry_t *out_entry);
int h2_bundle_manifest_is_hex_digest(const char *text, size_t len);
const h2_bundle_manifest_entry_t *h2_bundle_manifest_find(const h2_bundle_manifest_t *manifest, const char *path);
int h2_bundle_manifest_verify_crc32(const h2_bundle_manifest_entry_t *entry, uint64_t size, uint32_t crc32_value);

#ifdef __cplusplus
}
#endif

#endif
