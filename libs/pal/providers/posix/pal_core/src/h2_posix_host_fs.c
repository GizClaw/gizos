#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__linux__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif

#include "h2_posix_pal_core.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct h2_posix_host_fs_mount_state {
    char *source;
    char *target;
} h2_posix_host_fs_mount_state_t;

struct h2_posix_host_fs {
    h2_pal_fs_api_t api;
    h2_posix_host_fs_mount_state_t *mounts;
    size_t mount_count;
};

typedef struct h2_posix_host_fs_file {
    FILE *stream;
} h2_posix_host_fs_file_t;

static int map_errno(int error_number) {
    switch (error_number) {
        case ENOENT:
            return H2_PAL_FS_ERR_NOT_FOUND;
        case ENOMEM:
            return H2_PAL_FS_ERR_NO_MEMORY;
        case ENOSPC:
#ifdef EDQUOT
        case EDQUOT:
#endif
            return H2_PAL_FS_ERR_NO_SPACE;
        case EINVAL:
        case ELOOP:
        case ENAMETOOLONG:
            return H2_PAL_FS_ERR_INVALID_ARG;
        default:
            return H2_PAL_FS_ERR_IO;
    }
}

static int portable_path_valid(const char *path) {
    if (path == NULL || path[0] != '/' || path[1] == '\0' || path[strlen(path) - 1u] == '/') {
        return 0;
    }
    const char *component = path + 1;
    while (*component != '\0') {
        const char *slash = strchr(component, '/');
        size_t len = slash == NULL ? strlen(component) : (size_t)(slash - component);
        if (len == 0u || (len == 1u && component[0] == '.') ||
            (len == 2u && component[0] == '.' && component[1] == '.')) {
            return 0;
        }
        if (slash == NULL) {
            break;
        }
        component = slash + 1;
    }
    return 1;
}

static int mount_contains(const char *target, const char *path, const char **out_relative) {
    size_t target_len = strlen(target);
    if (strcmp(target, path) == 0) {
        *out_relative = path + target_len;
        return 1;
    }
    if (strncmp(target, path, target_len) == 0 && path[target_len] == '/') {
        *out_relative = path + target_len + 1u;
        return 1;
    }
    return 0;
}

static int resolve_path(
    h2_posix_host_fs_t *fs,
    const char *path,
    h2_posix_host_fs_mount_state_t **out_mount,
    char **out_host_path) {
    if (fs == NULL || !portable_path_valid(path) || out_mount == NULL || out_host_path == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    *out_mount = NULL;
    *out_host_path = NULL;

    for (size_t i = 0u; i < fs->mount_count; ++i) {
        const char *relative = NULL;
        if (!mount_contains(fs->mounts[i].target, path, &relative)) {
            continue;
        }
        size_t source_len = strlen(fs->mounts[i].source);
        size_t relative_len = strlen(relative);
        size_t separator_len = relative_len == 0u ? 0u : 1u;
        if (source_len > SIZE_MAX - separator_len ||
            source_len + separator_len > SIZE_MAX - relative_len ||
            source_len + separator_len + relative_len == SIZE_MAX) {
            return H2_PAL_FS_ERR_NO_MEMORY;
        }
        size_t total = source_len + separator_len + relative_len + 1u;
        char *host_path = (char *)malloc(total);
        if (host_path == NULL) {
            return H2_PAL_FS_ERR_NO_MEMORY;
        }
        memcpy(host_path, fs->mounts[i].source, source_len);
        if (relative_len != 0u) {
            host_path[source_len] = '/';
            memcpy(host_path + source_len + 1u, relative, relative_len);
        }
        host_path[total - 1u] = '\0';
        *out_mount = &fs->mounts[i];
        *out_host_path = host_path;
        return H2_PAL_FS_OK;
    }
    return H2_PAL_FS_ERR_NOT_FOUND;
}

static int reject_symlink_components(const char *root, const char *host_path) {
    size_t root_len = strlen(root);
    if (strncmp(root, host_path, root_len) != 0 ||
        (host_path[root_len] != '\0' && host_path[root_len] != '/')) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }

    char *probe = strdup(host_path);
    if (probe == NULL) {
        return H2_PAL_FS_ERR_NO_MEMORY;
    }
    int rc = H2_PAL_FS_OK;
    for (char *cursor = probe + root_len; *cursor != '\0'; ++cursor) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = '\0';
        struct stat st;
        if (lstat(probe, &st) != 0) {
            rc = errno == ENOENT ? H2_PAL_FS_OK : map_errno(errno);
            *cursor = '/';
            break;
        }
        if (S_ISLNK(st.st_mode)) {
            rc = H2_PAL_FS_ERR_INVALID_ARG;
            *cursor = '/';
            break;
        }
        *cursor = '/';
    }
    if (rc == H2_PAL_FS_OK) {
        struct stat st;
        if (lstat(probe, &st) == 0) {
            if (S_ISLNK(st.st_mode)) {
                rc = H2_PAL_FS_ERR_INVALID_ARG;
            }
        } else if (errno != ENOENT) {
            rc = map_errno(errno);
        }
    }
    free(probe);
    return rc;
}

static int resolve_checked(
    h2_posix_host_fs_t *fs,
    const char *path,
    h2_posix_host_fs_mount_state_t **out_mount,
    char **out_host_path) {
    int rc = resolve_path(fs, path, out_mount, out_host_path);
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    errno = 0;
    rc = reject_symlink_components((*out_mount)->source, *out_host_path);
    if (rc != H2_PAL_FS_OK) {
        free(*out_host_path);
        *out_host_path = NULL;
        *out_mount = NULL;
    }
    return rc;
}

static int posix_host_fs_mkdir(void *user, const char *path) {
    h2_posix_host_fs_mount_state_t *mount = NULL;
    char *host_path = NULL;
    int rc = resolve_checked((h2_posix_host_fs_t *)user, path, &mount, &host_path);
    (void)mount;
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    rc = mkdir(host_path, 0777) == 0 || errno == EEXIST ?
        H2_PAL_FS_OK : map_errno(errno);
    free(host_path);
    return rc;
}

static int posix_host_fs_open(
    void *user,
    const char *path,
    h2_pal_fs_open_mode_t mode,
    h2_pal_fs_file_t **out_file) {
    if (out_file == NULL ||
        (mode != H2_PAL_FS_OPEN_READ && mode != H2_PAL_FS_OPEN_WRITE_TRUNCATE)) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    *out_file = NULL;
    h2_posix_host_fs_mount_state_t *mount = NULL;
    char *host_path = NULL;
    int rc = resolve_checked((h2_posix_host_fs_t *)user, path, &mount, &host_path);
    (void)mount;
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }

    FILE *stream = fopen(host_path, mode == H2_PAL_FS_OPEN_READ ? "rb" : "wb");
    free(host_path);
    if (stream == NULL) {
        return map_errno(errno);
    }
    h2_posix_host_fs_file_t *file = (h2_posix_host_fs_file_t *)calloc(1u, sizeof(*file));
    if (file == NULL) {
        fclose(stream);
        return H2_PAL_FS_ERR_NO_MEMORY;
    }
    file->stream = stream;
    *out_file = (h2_pal_fs_file_t *)file;
    return H2_PAL_FS_OK;
}

static int posix_host_fs_read(void *user, h2_pal_fs_file_t *raw_file, void *data, size_t len, size_t *out_read) {
    (void)user;
    h2_posix_host_fs_file_t *file = (h2_posix_host_fs_file_t *)raw_file;
    if (file == NULL || file->stream == NULL || out_read == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    *out_read = fread(data, 1u, len, file->stream);
    return ferror(file->stream) ? H2_PAL_FS_ERR_IO : H2_PAL_FS_OK;
}

static int posix_host_fs_write(void *user, h2_pal_fs_file_t *raw_file, const void *data, size_t len, size_t *out_written) {
    (void)user;
    h2_posix_host_fs_file_t *file = (h2_posix_host_fs_file_t *)raw_file;
    if (file == NULL || file->stream == NULL || out_written == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    errno = 0;
    *out_written = fwrite(data, 1u, len, file->stream);
    if (*out_written != len && ferror(file->stream)) {
        return map_errno(errno);
    }
    return H2_PAL_FS_OK;
}

static int posix_host_fs_seek(
    void *user,
    h2_pal_fs_file_t *raw_file,
    uint64_t offset) {
    (void)user;
    h2_posix_host_fs_file_t *file = (h2_posix_host_fs_file_t *)raw_file;
    off_t native_offset = (off_t)offset;
    if (file == NULL || file->stream == NULL || native_offset < 0 ||
        (uint64_t)native_offset != offset) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    errno = 0;
    return fseeko(file->stream, native_offset, SEEK_SET) == 0
        ? H2_PAL_FS_OK
        : map_errno(errno);
}

static int posix_host_fs_sync(void *user, h2_pal_fs_file_t *raw_file) {
    (void)user;
    h2_posix_host_fs_file_t *file = (h2_posix_host_fs_file_t *)raw_file;
    if (file == NULL || file->stream == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    if (fflush(file->stream) != 0 || fsync(fileno(file->stream)) != 0) {
        return map_errno(errno);
    }
    return H2_PAL_FS_OK;
}

static int posix_host_fs_close(void *user, h2_pal_fs_file_t *raw_file) {
    (void)user;
    h2_posix_host_fs_file_t *file = (h2_posix_host_fs_file_t *)raw_file;
    if (file == NULL || file->stream == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    int rc = fclose(file->stream) == 0 ? H2_PAL_FS_OK : map_errno(errno);
    file->stream = NULL;
    free(file);
    return rc;
}

static int posix_host_fs_stat(void *user, const char *path, h2_pal_fs_stat_t *out_stat) {
    if (out_stat == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    memset(out_stat, 0, sizeof(*out_stat));
    h2_posix_host_fs_mount_state_t *mount = NULL;
    char *host_path = NULL;
    int rc = resolve_checked((h2_posix_host_fs_t *)user, path, &mount, &host_path);
    (void)mount;
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    struct stat st;
    if (stat(host_path, &st) != 0) {
        rc = map_errno(errno);
    } else {
        out_stat->size = (uint64_t)st.st_size;
        out_stat->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
        rc = H2_PAL_FS_OK;
    }
    free(host_path);
    return rc;
}

static int clear_directory(const char *path) {
    DIR *dir = opendir(path);
    if (dir == NULL) {
        return map_errno(errno);
    }
    int rc = H2_PAL_FS_OK;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            errno = 0;
            continue;
        }
        size_t path_len = strlen(path);
        size_t name_len = strlen(entry->d_name);
        if (path_len > SIZE_MAX - name_len - 2u) {
            rc = H2_PAL_FS_ERR_NO_MEMORY;
            break;
        }
        char *child = (char *)malloc(path_len + 1u + name_len + 1u);
        if (child == NULL) {
            rc = H2_PAL_FS_ERR_NO_MEMORY;
            break;
        }
        memcpy(child, path, path_len);
        child[path_len] = '/';
        memcpy(child + path_len + 1u, entry->d_name, name_len + 1u);
        struct stat st;
        if (lstat(child, &st) != 0) {
            rc = map_errno(errno);
        } else if (S_ISLNK(st.st_mode)) {
            rc = H2_PAL_FS_ERR_INVALID_ARG;
        } else if (S_ISDIR(st.st_mode)) {
            rc = clear_directory(child);
            if (rc == H2_PAL_FS_OK && rmdir(child) != 0) {
                rc = map_errno(errno);
            }
        } else if (unlink(child) != 0) {
            rc = map_errno(errno);
        }
        free(child);
        if (rc != H2_PAL_FS_OK) {
            break;
        }
        errno = 0;
    }
    if (rc == H2_PAL_FS_OK && errno != 0) {
        rc = map_errno(errno);
    }
    closedir(dir);
    return rc;
}

static int posix_host_fs_clear(void *user, const char *path) {
    h2_posix_host_fs_mount_state_t *mount = NULL;
    char *host_path = NULL;
    int rc = resolve_checked((h2_posix_host_fs_t *)user, path, &mount, &host_path);
    (void)mount;
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    struct stat st;
    if (stat(host_path, &st) != 0) {
        rc = map_errno(errno);
    } else if (!S_ISDIR(st.st_mode)) {
        rc = H2_PAL_FS_ERR_INVALID_ARG;
    } else {
        rc = clear_directory(host_path);
    }
    free(host_path);
    return rc;
}

static int posix_host_fs_remove(void *user, const char *path) {
    h2_posix_host_fs_mount_state_t *mount = NULL;
    char *host_path = NULL;
    int rc = resolve_checked((h2_posix_host_fs_t *)user, path, &mount, &host_path);
    (void)mount;
    if (rc != H2_PAL_FS_OK) {
        return rc;
    }
    struct stat st;
    if (lstat(host_path, &st) != 0) {
        rc = map_errno(errno);
    } else if (S_ISLNK(st.st_mode)) {
        rc = H2_PAL_FS_ERR_INVALID_ARG;
    } else if (S_ISDIR(st.st_mode)) {
        rc = rmdir(host_path) == 0 ? H2_PAL_FS_OK : map_errno(errno);
    } else {
        rc = unlink(host_path) == 0 ? H2_PAL_FS_OK : map_errno(errno);
    }
    free(host_path);
    return rc;
}

static int posix_host_fs_rename(void *user, const char *old_path, const char *new_path) {
    h2_posix_host_fs_t *fs = (h2_posix_host_fs_t *)user;
    h2_posix_host_fs_mount_state_t *old_mount = NULL;
    h2_posix_host_fs_mount_state_t *new_mount = NULL;
    char *old_host = NULL;
    char *new_host = NULL;
    int rc = resolve_checked(fs, old_path, &old_mount, &old_host);
    if (rc == H2_PAL_FS_OK) {
        rc = resolve_checked(fs, new_path, &new_mount, &new_host);
    }
    if (rc == H2_PAL_FS_OK && old_mount != new_mount) {
        rc = H2_PAL_FS_ERR_UNSUPPORTED;
    }
    if (rc == H2_PAL_FS_OK && rename(old_host, new_host) != 0) {
        rc = map_errno(errno);
    }
    free(old_host);
    free(new_host);
    return rc;
}

static const h2_pal_fs_vtable_t posix_host_fs_vtable = {
    .mkdir = posix_host_fs_mkdir,
    .open = posix_host_fs_open,
    .read = posix_host_fs_read,
    .write = posix_host_fs_write,
    .seek = posix_host_fs_seek,
    .sync = posix_host_fs_sync,
    .close = posix_host_fs_close,
    .stat = posix_host_fs_stat,
    .clear = posix_host_fs_clear,
    .remove = posix_host_fs_remove,
    .rename = posix_host_fs_rename,
};

static int targets_overlap(const char *left, const char *right) {
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    return strcmp(left, right) == 0 ||
        (left_len < right_len && strncmp(left, right, left_len) == 0 && right[left_len] == '/') ||
        (right_len < left_len && strncmp(left, right, right_len) == 0 && left[right_len] == '/');
}

static char *resolve_packaged_source(const char *source) {
    const char *build_root = getenv("H2_DESKTOP_SOURCE_ROOT");
    const char *resource_root = getenv("H2_DESKTOP_RESOURCE_ROOT");
    if (build_root == NULL || build_root[0] != '/' || resource_root == NULL ||
        resource_root[0] != '/') {
        return strdup(source);
    }
    size_t build_root_len = strlen(build_root);
    if (build_root_len == 1u || build_root[build_root_len - 1u] == '/') {
        return strdup(source);
    }
    if (strncmp(source, build_root, build_root_len) != 0 ||
        (source[build_root_len] != '\0' && source[build_root_len] != '/')) {
        return strdup(source);
    }
    const char *relative = source + build_root_len;
    size_t resource_root_len = strlen(resource_root);
    size_t relative_len = strlen(relative);
    if (resource_root_len > SIZE_MAX - relative_len - 1u) {
        return NULL;
    }
    size_t result_len = resource_root_len + relative_len + 1u;
    char *result = (char *)malloc(result_len);
    if (result == NULL) {
        return NULL;
    }
    int written = snprintf(result, result_len, "%s%s", resource_root, relative);
    if (written < 0 || (size_t)written >= result_len) {
        free(result);
        return NULL;
    }
    return result;
}

int h2_posix_host_fs_create(const char *const *sources,
                            const char *const *targets, size_t mount_count,
                            h2_posix_host_fs_t **out_fs) {
    if (out_fs == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    *out_fs = NULL;
    if ((sources == NULL || targets == NULL) && mount_count != 0u) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    h2_posix_host_fs_t *fs = (h2_posix_host_fs_t *)calloc(1u, sizeof(*fs));
    if (fs == NULL) {
        return H2_PAL_FS_ERR_NO_MEMORY;
    }
    if (mount_count != 0u) {
        fs->mounts = (h2_posix_host_fs_mount_state_t *)calloc(mount_count, sizeof(*fs->mounts));
        if (fs->mounts == NULL) {
            free(fs);
            return H2_PAL_FS_ERR_NO_MEMORY;
        }
    }
    fs->mount_count = mount_count;
    fs->api.user = fs;
    fs->api.vtable = &posix_host_fs_vtable;

    int rc = H2_PAL_FS_OK;
    for (size_t i = 0u; i < mount_count; ++i) {
        struct stat st;
        if (sources[i] == NULL || sources[i][0] != '/' ||
            !portable_path_valid(targets[i])) {
            rc = H2_PAL_FS_ERR_INVALID_ARG;
            break;
        }
        char *resolved_source = resolve_packaged_source(sources[i]);
        if (resolved_source == NULL) {
            rc = H2_PAL_FS_ERR_NO_MEMORY;
            break;
        }
        if (lstat(resolved_source, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
            free(resolved_source);
            rc = H2_PAL_FS_ERR_INVALID_ARG;
            break;
        }
        char *canonical_source = realpath(resolved_source, NULL);
        free(resolved_source);
        if (canonical_source == NULL) {
            rc = errno == ENOMEM ? H2_PAL_FS_ERR_NO_MEMORY : H2_PAL_FS_ERR_INVALID_ARG;
            break;
        }
        for (size_t j = 0u; j < i; ++j) {
            if (targets_overlap(targets[j], targets[i])) {
                rc = H2_PAL_FS_ERR_INVALID_ARG;
                break;
            }
        }
        if (rc != H2_PAL_FS_OK) {
            free(canonical_source);
            break;
        }
        fs->mounts[i].source = canonical_source;
        fs->mounts[i].target = strdup(targets[i]);
        if (fs->mounts[i].target == NULL) {
            rc = H2_PAL_FS_ERR_NO_MEMORY;
            break;
        }
    }
    if (rc != H2_PAL_FS_OK) {
        h2_posix_host_fs_destroy(fs);
        return rc;
    }
    *out_fs = fs;
    return H2_PAL_FS_OK;
}

void h2_posix_host_fs_destroy(h2_posix_host_fs_t *fs) {
    if (fs == NULL) {
        return;
    }
    for (size_t i = 0u; i < fs->mount_count; ++i) {
        free(fs->mounts[i].source);
        free(fs->mounts[i].target);
    }
    free(fs->mounts);
    free(fs);
}

const h2_pal_fs_api_t *h2_posix_host_fs_api(h2_posix_host_fs_t *fs) {
    return fs == NULL ? NULL : &fs->api;
}
