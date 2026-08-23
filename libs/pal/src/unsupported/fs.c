#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static int unsupported_fs_mkdir(void *p0, const char *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_fs_open(void *p0, const char *p1, h2_pal_fs_open_mode_t p2, h2_pal_fs_file_t **p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_fs_read(void *p0, h2_pal_fs_file_t *p1, void *p2, size_t p3, size_t *p4) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_fs_write(void *p0, h2_pal_fs_file_t *p1, const void *p2, size_t p3, size_t *p4) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_fs_seek(void *p0, h2_pal_fs_file_t *p1, uint64_t p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_fs_sync(void *p0, h2_pal_fs_file_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_fs_close(void *p0, h2_pal_fs_file_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_fs_stat(void *p0, const char *p1, h2_pal_fs_stat_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_fs_clear(void *p0, const char *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_fs_remove(void *p0, const char *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int unsupported_fs_rename(void *p0, const char *p1, const char *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_fs_vtable_t unsupported_fs_vtable = {
    .mkdir = unsupported_fs_mkdir,
    .open = unsupported_fs_open,
    .read = unsupported_fs_read,
    .seek = unsupported_fs_seek,
    .write = unsupported_fs_write,
    .sync = unsupported_fs_sync,
    .close = unsupported_fs_close,
    .stat = unsupported_fs_stat,
    .clear = unsupported_fs_clear,
    .remove = unsupported_fs_remove,
    .rename = unsupported_fs_rename,
};
static const h2_pal_fs_api_t unsupported_fs_api = { .user = NULL, .vtable = &unsupported_fs_vtable };
const h2_pal_fs_api_t *h2_pal_unsupported_fs_api(void) { return &unsupported_fs_api; }
