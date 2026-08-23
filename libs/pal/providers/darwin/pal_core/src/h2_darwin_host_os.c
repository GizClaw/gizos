#include "h2_darwin_platform.h"

#include "h2_posix_pal_core.h"

#include <stdlib.h>

struct h2_darwin_host_fs {
    h2_posix_host_fs_t *impl;
};

int h2_darwin_host_fs_create(const char *const *sources,
                             const char *const *targets, size_t mount_count,
                             h2_darwin_host_fs_t **out_fs) {
    if (out_fs == NULL) {
        return H2_PAL_FS_ERR_INVALID_ARG;
    }
    *out_fs = NULL;
    h2_darwin_host_fs_t *fs = calloc(1u, sizeof(*fs));
    if (fs == NULL) {
        return H2_PAL_FS_ERR_NO_MEMORY;
    }
    int result = h2_posix_host_fs_create(sources, targets, mount_count,
                                         &fs->impl);
    if (result != H2_PAL_FS_OK) {
        free(fs);
        return result;
    }
    *out_fs = fs;
    return H2_PAL_FS_OK;
}

void h2_darwin_host_fs_destroy(h2_darwin_host_fs_t *fs) {
    if (fs != NULL) {
        h2_posix_host_fs_destroy(fs->impl);
        free(fs);
    }
}

const h2_pal_fs_api_t *h2_darwin_host_fs_api(h2_darwin_host_fs_t *fs) {
    return fs == NULL ? NULL : h2_posix_host_fs_api(fs->impl);
}

const h2_pal_net_api_t *h2_darwin_net_api(void) {
    return h2_posix_net_api();
}

int h2_darwin_entropy(void *user, uint8_t *out, size_t len) {
    return h2_posix_entropy(user, out, len);
}
