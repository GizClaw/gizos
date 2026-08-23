#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__linux__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif

#include "h2_h2loader_cli_host_path.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int join(const char *dir, const char *name, char *out, size_t out_size) {
    size_t dir_len = strlen(dir);
    size_t name_len = strlen(name);
    int need_sep = name_len != 0u && dir_len != 0u && dir[dir_len - 1u] != '/';
    if (dir_len + (need_sep ? 1u : 0u) + name_len + 1u > out_size) return 0;
    memcpy(out, dir, dir_len);
    if (need_sep) out[dir_len++] = '/';
    memcpy(out + dir_len, name, name_len + 1u);
    return 1;
}

/* realpath() that tolerates a missing final component. */
static int resolve_host(const char *absolute, char *out, size_t out_size) {
    char buffer[PATH_MAX];
    if (realpath(absolute, buffer) != NULL) {
        return strlen(buffer) + 1u <= out_size ? (strcpy(out, buffer), 1) : 0;
    }
    if (errno != ENOENT) return 0;
    const char *slash = strrchr(absolute, '/');
    if (slash == NULL || slash[1] == '\0') return 0;
    char parent[PATH_MAX];
    size_t parent_len = slash == absolute ? 1u : (size_t)(slash - absolute);
    if (parent_len + 1u > sizeof(parent)) return 0;
    memcpy(parent, absolute, parent_len);
    parent[parent_len] = '\0';
    if (realpath(parent, buffer) == NULL) return 0;
    return join(buffer, slash + 1, out, out_size);
}

int h2_h2loader_cli_host_path_resolve(
    const char *base_dir,
    const char *const *sources,
    const char *const *targets,
    size_t mount_count,
    const char *path,
    char *out,
    size_t out_size) {
    char absolute[PATH_MAX];
    char resolved[PATH_MAX];
    if (path == NULL || path[0] == '\0' || out == NULL || out_size == 0u) return 0;
    if (path[0] == '/') {
        if (strlen(path) + 1u > sizeof(absolute)) return 0;
        strcpy(absolute, path);
    } else {
        if (base_dir == NULL || base_dir[0] != '/' ||
            !join(base_dir, path, absolute, sizeof(absolute))) {
            return 0;
        }
    }
    if (!resolve_host(absolute, resolved, sizeof(resolved))) return 0;
    for (size_t i = 0u; i < mount_count; ++i) {
        size_t source_len = strlen(sources[i]);
        if (strncmp(resolved, sources[i], source_len) != 0) continue;
        if (resolved[source_len] != '\0' && resolved[source_len] != '/') continue;
        return join(targets[i], resolved + source_len +
            (resolved[source_len] == '/' ? 1u : 0u), out, out_size);
    }
    return 0;
}
