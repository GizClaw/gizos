#include "h2_bundle_path.h"

#include <string.h>

static int is_separator(char ch) {
    return ch == '/' || ch == '\\';
}

int h2_bundle_path_is_safe_relative(const char *path) {
    size_t len;
    size_t component_start = 0u;

    if (path == NULL || path[0] == '\0' || is_separator(path[0])) {
        return 0;
    }
    len = strlen(path);
    if (path[len - 1u] == '/' && len == 1u) {
        return 0;
    }

    for (size_t i = 0u; i <= len; ++i) {
        char ch = path[i];
        if (ch == ':' || ch == '\\') {
            return 0;
        }
        if (ch != '/' && ch != '\0') {
            continue;
        }

        size_t component_len = i - component_start;
        if (component_len == 0u) {
            return 0;
        }
        if ((component_len == 1u && path[component_start] == '.') ||
            (component_len == 2u && path[component_start] == '.' && path[component_start + 1u] == '.')) {
            return 0;
        }
        component_start = i + 1u;
    }

    return 1;
}

int h2_bundle_path_join(char *out, size_t out_len, const char *root, const char *relative_path) {
    size_t root_len;
    size_t rel_len;
    int needs_slash;

    if (out == NULL || out_len == 0u || root == NULL || relative_path == NULL) {
        return H2_BUNDLE_ERR_INVALID_ARG;
    }
    if (!h2_bundle_path_is_safe_relative(relative_path)) {
        return H2_BUNDLE_ERR_UNSAFE_PATH;
    }

    root_len = strlen(root);
    rel_len = strlen(relative_path);
    needs_slash = root_len > 0u && root[root_len - 1u] != '/';
    if (root_len + (needs_slash ? 1u : 0u) + rel_len + 1u > out_len) {
        return H2_BUNDLE_ERR_NO_SPACE;
    }

    memcpy(out, root, root_len);
    if (needs_slash) {
        out[root_len++] = '/';
    }
    memcpy(out + root_len, relative_path, rel_len);
    out[root_len + rel_len] = '\0';
    return H2_BUNDLE_OK;
}

int h2_bundle_path_parent_dir(char *out, size_t out_len, const char *path) {
    size_t len;
    size_t last_slash = (size_t)-1;

    if (out == NULL || out_len == 0u || path == NULL || path[0] == '\0') {
        return H2_BUNDLE_ERR_INVALID_ARG;
    }
    len = strlen(path);
    for (size_t i = 0u; i < len; ++i) {
        if (path[i] == '/') {
            last_slash = i;
        }
    }
    if (last_slash == (size_t)-1) {
        out[0] = '\0';
        return H2_BUNDLE_OK;
    }
    if (last_slash + 1u > out_len) {
        return H2_BUNDLE_ERR_NO_SPACE;
    }
    memcpy(out, path, last_slash);
    out[last_slash] = '\0';
    return H2_BUNDLE_OK;
}

int h2_bundle_path_has_suffix(const char *path, const char *suffix) {
    size_t path_len;
    size_t suffix_len;

    if (path == NULL || suffix == NULL) {
        return 0;
    }
    path_len = strlen(path);
    suffix_len = strlen(suffix);
    if (path_len < suffix_len) {
        return 0;
    }
    return memcmp(path + path_len - suffix_len, suffix, suffix_len) == 0;
}
