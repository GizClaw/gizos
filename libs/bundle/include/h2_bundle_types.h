#ifndef H2_BUNDLE_TYPES_H
#define H2_BUNDLE_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_BUNDLE_PATH_MAX 384u

typedef enum h2_bundle_result {
    H2_BUNDLE_OK = 0,
    H2_BUNDLE_ERR_INVALID_ARG = -1,
    H2_BUNDLE_ERR_NO_MEMORY = -2,
    H2_BUNDLE_ERR_NO_SPACE = -3,
    H2_BUNDLE_ERR_IO = -4,
    H2_BUNDLE_ERR_FS = -5,
    H2_BUNDLE_ERR_ZLIB = -6,
    H2_BUNDLE_ERR_TAR = -7,
    H2_BUNDLE_ERR_UNSAFE_PATH = -8,
    H2_BUNDLE_ERR_UNSUPPORTED_ENTRY = -9,
    H2_BUNDLE_ERR_PIXA = -10,
    H2_BUNDLE_ERR_MANIFEST = -11,
    H2_BUNDLE_ERR_LAYOUT = -12,
} h2_bundle_result_t;

typedef enum h2_bundle_entry_kind {
    H2_BUNDLE_ENTRY_FILE = 1,
    H2_BUNDLE_ENTRY_DIR = 2,
} h2_bundle_entry_kind_t;

typedef struct h2_bundle_entry {
    char path[H2_BUNDLE_PATH_MAX];
    uint64_t size;
    h2_bundle_entry_kind_t kind;
} h2_bundle_entry_t;

typedef struct h2_bundle_install_stats {
    size_t entry_count;
    size_t file_count;
    size_t dir_count;
    size_t pixa_count;
    uint64_t payload_bytes;
} h2_bundle_install_stats_t;

typedef void (*h2_bundle_progress_fn)(void *user, const h2_bundle_entry_t *entry, const h2_bundle_install_stats_t *stats);

const char *h2_bundle_result_name(int result);

#ifdef __cplusplus
}
#endif

#endif
