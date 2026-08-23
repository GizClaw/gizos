#ifndef H2_BUNDLE_PATH_H
#define H2_BUNDLE_PATH_H

#include "h2_bundle_types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int h2_bundle_path_is_safe_relative(const char *path);
int h2_bundle_path_join(char *out, size_t out_len, const char *root, const char *relative_path);
int h2_bundle_path_parent_dir(char *out, size_t out_len, const char *path);
int h2_bundle_path_has_suffix(const char *path, const char *suffix);

#ifdef __cplusplus
}
#endif

#endif
