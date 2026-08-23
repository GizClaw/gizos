#ifndef H2_H2LOADER_CLI_HOST_PATH_H
#define H2_H2LOADER_CLI_HOST_PATH_H

#include <stddef.h>

/* Rewrites a host path typed by the user into the PAL filesystem namespace.
 *
 * - Relative paths are joined to base_dir (the invoking shell's directory;
 *   `bazel run` changes cwd to runfiles and exports BUILD_WORKING_DIRECTORY).
 * - Symlinks are resolved (realpath); for a not-yet-existing final component
 *   (output files) the parent directory is resolved instead.
 * - The resolved host path is mapped back from a mount source prefix to its
 *   PAL target prefix (macOS: /private/tmp -> /tmp).
 *
 * Returns 1 and fills out when the path was rewritten into a mount, 0 when it
 * should be passed through unchanged (PAL then reports its own error). */
int h2_h2loader_cli_host_path_resolve(
    const char *base_dir,
    const char *const *sources,
    const char *const *targets,
    size_t mount_count,
    const char *path,
    char *out,
    size_t out_size);

#endif
