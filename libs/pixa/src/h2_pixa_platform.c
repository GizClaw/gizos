#include "h2_pixa_platform.h"

#include <stddef.h>

static int h2_pixa_map_pal_result(int result) {
    switch (result) {
    case H2_PAL_OK:
        return PIXA_OSAL_OK;
    case H2_PAL_ERR_INVALID_ARG:
        return PIXA_OSAL_ERR_INVALID_ARG;
    case H2_PAL_ERR_IO:
        return PIXA_OSAL_ERR_IO;
    case H2_PAL_ERR_NO_MEMORY:
        return PIXA_OSAL_ERR_NO_MEMORY;
    case H2_PAL_ERR_NO_SPACE:
        return PIXA_OSAL_ERR_NO_SPACE;
    case H2_PAL_ERR_UNSUPPORTED:
        return PIXA_OSAL_ERR_UNSUPPORTED;
    default:
        return PIXA_OSAL_ERR_IO;
    }
}

static h2_pal_fs_file_t *h2_pixa_pal_file(pixa_osal_file_t *file) {
    return (h2_pal_fs_file_t *)file;
}

static pixa_osal_file_t *h2_pixa_osal_file(h2_pal_fs_file_t *file) {
    return (pixa_osal_file_t *)file;
}

static int h2_pixa_mkdir(void *user, const char *path) {
    h2_pixa_platform_t *platform = (h2_pixa_platform_t *)user;
    return h2_pixa_map_pal_result(h2_pal_fs_mkdir(platform->fs, path));
}

static int h2_pixa_open(
    void *user,
    const char *path,
    pixa_osal_open_mode_t mode,
    pixa_osal_file_t **out_file) {
    h2_pixa_platform_t *platform = (h2_pixa_platform_t *)user;
    h2_pal_fs_open_mode_t pal_mode;
    h2_pal_fs_file_t *pal_file = NULL;
    int result;

    *out_file = NULL;
    switch (mode) {
    case PIXA_OSAL_OPEN_WRITE_TRUNCATE:
        pal_mode = H2_PAL_FS_OPEN_WRITE_TRUNCATE;
        break;
    case PIXA_OSAL_OPEN_READ:
        pal_mode = H2_PAL_FS_OPEN_READ;
        break;
    default:
        return PIXA_OSAL_ERR_INVALID_ARG;
    }

    result = h2_pal_fs_open(platform->fs, path, pal_mode, &pal_file);
    if (result == H2_PAL_OK) {
        *out_file = h2_pixa_osal_file(pal_file);
    }
    return h2_pixa_map_pal_result(result);
}

static int h2_pixa_read(
    void *user,
    pixa_osal_file_t *file,
    void *data,
    size_t len,
    size_t *out_read) {
    h2_pixa_platform_t *platform = (h2_pixa_platform_t *)user;
    *out_read = 0u;
    return h2_pixa_map_pal_result(
        h2_pal_fs_read(platform->fs, h2_pixa_pal_file(file), data, len, out_read));
}

static int h2_pixa_seek(
    void *user,
    pixa_osal_file_t *file,
    uint64_t position) {
    h2_pixa_platform_t *platform = (h2_pixa_platform_t *)user;
    return h2_pixa_map_pal_result(
        h2_pal_fs_seek(platform->fs, h2_pixa_pal_file(file), position));
}

static int h2_pixa_write(
    void *user,
    pixa_osal_file_t *file,
    const void *data,
    size_t len,
    size_t *out_written) {
    h2_pixa_platform_t *platform = (h2_pixa_platform_t *)user;
    *out_written = 0u;
    return h2_pixa_map_pal_result(
        h2_pal_fs_write(platform->fs, h2_pixa_pal_file(file), data, len, out_written));
}

static int h2_pixa_sync(void *user, pixa_osal_file_t *file) {
    h2_pixa_platform_t *platform = (h2_pixa_platform_t *)user;
    return h2_pixa_map_pal_result(
        h2_pal_fs_sync(platform->fs, h2_pixa_pal_file(file)));
}

static int h2_pixa_close(void *user, pixa_osal_file_t *file) {
    h2_pixa_platform_t *platform = (h2_pixa_platform_t *)user;
    return h2_pixa_map_pal_result(
        h2_pal_fs_close(platform->fs, h2_pixa_pal_file(file)));
}

static int h2_pixa_stat(
    void *user,
    const char *path,
    pixa_osal_stat_t *out_stat) {
    h2_pixa_platform_t *platform = (h2_pixa_platform_t *)user;
    h2_pal_fs_stat_t pal_stat = {0};
    int result;

    out_stat->size = 0u;
    out_stat->is_dir = 0;
    result = h2_pal_fs_stat(platform->fs, path, &pal_stat);
    if (result == H2_PAL_OK) {
        out_stat->size = pal_stat.size;
        out_stat->is_dir = pal_stat.is_dir;
    }
    return h2_pixa_map_pal_result(result);
}

static int h2_pixa_clear(void *user, const char *path) {
    h2_pixa_platform_t *platform = (h2_pixa_platform_t *)user;
    return h2_pixa_map_pal_result(h2_pal_fs_clear(platform->fs, path));
}

static int h2_pixa_remove(void *user, const char *path) {
    h2_pixa_platform_t *platform = (h2_pixa_platform_t *)user;
    return h2_pixa_map_pal_result(h2_pal_fs_remove(platform->fs, path));
}

static int h2_pixa_rename(
    void *user,
    const char *old_path,
    const char *new_path) {
    h2_pixa_platform_t *platform = (h2_pixa_platform_t *)user;
    return h2_pixa_map_pal_result(
        h2_pal_fs_rename(platform->fs, old_path, new_path));
}

static void *h2_pixa_alloc(void *user, size_t len) {
    h2_pixa_platform_t *platform = (h2_pixa_platform_t *)user;
    return h2_pal_mem_alloc(platform->mem, len);
}

static void h2_pixa_free(void *user, void *ptr) {
    h2_pixa_platform_t *platform = (h2_pixa_platform_t *)user;
    h2_pal_mem_free(platform->mem, ptr);
}

static const pixa_osal_vtable_t h2_pixa_osal_vtable = {
    .mkdir = h2_pixa_mkdir,
    .open = h2_pixa_open,
    .read = h2_pixa_read,
    .seek = h2_pixa_seek,
    .write = h2_pixa_write,
    .sync = h2_pixa_sync,
    .close = h2_pixa_close,
    .stat = h2_pixa_stat,
    .clear = h2_pixa_clear,
    .remove = h2_pixa_remove,
    .rename = h2_pixa_rename,
};

h2_pal_result_t h2_pixa_platform_init(
    h2_pixa_platform_t *platform,
    const h2_pixa_platform_config_t *config) {
    h2_pixa_platform_t initialized = {0};

    if (platform == NULL || config == NULL || config->fs == NULL ||
        config->fs->vtable == NULL || config->mem == NULL ||
        config->mem->vtable == NULL || config->mem->vtable->alloc == NULL ||
        config->mem->vtable->free == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    initialized.fs = config->fs;
    initialized.mem = config->mem;
    initialized.osal.user = platform;
    initialized.osal.vtable = &h2_pixa_osal_vtable;
    initialized.allocator.user = platform;
    initialized.allocator.alloc = h2_pixa_alloc;
    initialized.allocator.free = h2_pixa_free;
    initialized.initialized = 1;
    *platform = initialized;
    return H2_PAL_OK;
}

void h2_pixa_platform_deinit(h2_pixa_platform_t *platform) {
    if (platform != NULL) {
        *platform = (h2_pixa_platform_t){0};
    }
}

const pixa_osal_api_t *h2_pixa_platform_osal(
    const h2_pixa_platform_t *platform) {
    if (platform == NULL || !platform->initialized) {
        return NULL;
    }
    return &platform->osal;
}

const pixa_alloc_t *h2_pixa_platform_allocator(
    const h2_pixa_platform_t *platform) {
    if (platform == NULL || !platform->initialized) {
        return NULL;
    }
    return &platform->allocator;
}
