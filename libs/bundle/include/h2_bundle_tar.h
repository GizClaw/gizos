#ifndef H2_BUNDLE_TAR_H
#define H2_BUNDLE_TAR_H

#include "h2_bundle_types.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_bundle_tar_header_result {
    H2_BUNDLE_TAR_HEADER_ENTRY = 1,
    H2_BUNDLE_TAR_HEADER_ZERO = 2,
} h2_bundle_tar_header_result_t;

int h2_bundle_tar_parse_header(const uint8_t block[512], h2_bundle_entry_t *out_entry, int *out_header_result);
uint64_t h2_bundle_tar_padding(uint64_t size);

#ifdef __cplusplus
}
#endif

#endif
