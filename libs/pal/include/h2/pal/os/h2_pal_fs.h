#ifndef H2_PAL_FS_H
#define H2_PAL_FS_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef h2_pal_result_t h2_pal_fs_result_t;

typedef struct h2_pal_fs_file h2_pal_fs_file_t;

typedef enum h2_pal_fs_open_mode {
    H2_PAL_FS_OPEN_WRITE_TRUNCATE = 1,
    H2_PAL_FS_OPEN_READ = 2,
} h2_pal_fs_open_mode_t;

typedef struct h2_pal_fs_stat {
    uint64_t size;
    int is_dir;
} h2_pal_fs_stat_t;

typedef struct h2_pal_fs_vtable {
    int (*mkdir)(void *user, const char *path);
    int (*open)(void *user, const char *path, h2_pal_fs_open_mode_t mode, h2_pal_fs_file_t **out_file);
    int (*read)(void *user, h2_pal_fs_file_t *file, void *data, size_t len, size_t *out_read);
    int (*seek)(void *user, h2_pal_fs_file_t *file, uint64_t position);
    int (*write)(void *user, h2_pal_fs_file_t *file, const void *data, size_t len, size_t *out_written);
    int (*sync)(void *user, h2_pal_fs_file_t *file);
    int (*close)(void *user, h2_pal_fs_file_t *file);
    int (*stat)(void *user, const char *path, h2_pal_fs_stat_t *out_stat);
    int (*clear)(void *user, const char *path);
    int (*remove)(void *user, const char *path);
    int (*rename)(void *user, const char *old_path, const char *new_path);
} h2_pal_fs_vtable_t;

typedef struct h2_pal_fs_api {
    void *user;
    const h2_pal_fs_vtable_t *vtable;
} h2_pal_fs_api_t;

static inline int h2_pal_fs_mkdir(const h2_pal_fs_api_t *api, const char *path) {
    if (api == NULL || api->vtable == NULL || api->vtable->mkdir == NULL || path == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    return api->vtable->mkdir(api->user, path);
}

static inline int h2_pal_fs_open(
    const h2_pal_fs_api_t *api,
    const char *path,
    h2_pal_fs_open_mode_t mode,
    h2_pal_fs_file_t **out_file) {
    if (api == NULL || api->vtable == NULL || api->vtable->open == NULL ||
        path == NULL || out_file == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    return api->vtable->open(api->user, path, mode, out_file);
}

static inline int h2_pal_fs_read(
    const h2_pal_fs_api_t *api,
    h2_pal_fs_file_t *file,
    void *data,
    size_t len,
    size_t *out_read) {
    if (api == NULL || api->vtable == NULL || api->vtable->read == NULL ||
        file == NULL || (data == NULL && len != 0u) || out_read == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    return api->vtable->read(api->user, file, data, len, out_read);
}

static inline int h2_pal_fs_write(
    const h2_pal_fs_api_t *api,
    h2_pal_fs_file_t *file,
    const void *data,
    size_t len,
    size_t *out_written) {
    if (api == NULL || api->vtable == NULL || api->vtable->write == NULL ||
        file == NULL || (data == NULL && len != 0u) || out_written == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    return api->vtable->write(api->user, file, data, len, out_written);
}

static inline int h2_pal_fs_seek(
    const h2_pal_fs_api_t *api,
    h2_pal_fs_file_t *file,
    uint64_t position) {
    if (api == NULL || file == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    if (api->vtable == NULL || api->vtable->seek == NULL) {
        return H2_PAL_FS_ERR_UNSUPPORTED;
    }
    return api->vtable->seek(api->user, file, position);
}

static inline int h2_pal_fs_sync(const h2_pal_fs_api_t *api, h2_pal_fs_file_t *file) {
    if (api == NULL || file == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    if (api->vtable == NULL || api->vtable->sync == NULL) {
        return H2_PAL_FS_OK;
    }
    return api->vtable->sync(api->user, file);
}

static inline int h2_pal_fs_close(const h2_pal_fs_api_t *api, h2_pal_fs_file_t *file) {
    if (api == NULL || api->vtable == NULL || api->vtable->close == NULL || file == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    return api->vtable->close(api->user, file);
}

static inline int h2_pal_fs_stat(const h2_pal_fs_api_t *api, const char *path, h2_pal_fs_stat_t *out_stat) {
    if (api == NULL || api->vtable == NULL || api->vtable->stat == NULL ||
        path == NULL || out_stat == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    return api->vtable->stat(api->user, path, out_stat);
}

static inline int h2_pal_fs_clear(const h2_pal_fs_api_t *api, const char *path) {
    if (api == NULL || api->vtable == NULL || api->vtable->clear == NULL || path == NULL) {
        return H2_PAL_FS_ERR_UNSUPPORTED;
    }
    return api->vtable->clear(api->user, path);
}

static inline int h2_pal_fs_remove(const h2_pal_fs_api_t *api, const char *path) {
    if (api == NULL || api->vtable == NULL || api->vtable->remove == NULL || path == NULL) {
        return H2_PAL_FS_ERR_UNSUPPORTED;
    }
    return api->vtable->remove(api->user, path);
}

static inline int h2_pal_fs_rename(const h2_pal_fs_api_t *api, const char *old_path, const char *new_path) {
    if (api == NULL || api->vtable == NULL || api->vtable->rename == NULL ||
        old_path == NULL || new_path == NULL) {
        return H2_PAL_FS_ERR_UNSUPPORTED;
    }
    return api->vtable->rename(api->user, old_path, new_path);
}

#ifdef __cplusplus
}
#endif

#endif
