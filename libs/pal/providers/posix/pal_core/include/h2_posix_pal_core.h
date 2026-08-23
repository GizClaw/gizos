#ifndef H2_POSIX_PAL_CORE_H
#define H2_POSIX_PAL_CORE_H

#include "h2/pal/net/h2_pal_net.h"
#include "h2/pal/os/h2_pal_fs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_posix_host_fs h2_posix_host_fs_t;

int h2_posix_host_fs_create(const char *const *sources,
                            const char *const *targets, size_t mount_count,
                            h2_posix_host_fs_t **out_fs);
void h2_posix_host_fs_destroy(h2_posix_host_fs_t *fs);
const h2_pal_fs_api_t *h2_posix_host_fs_api(h2_posix_host_fs_t *fs);

const h2_pal_net_api_t *h2_posix_net_api(void);
int h2_posix_entropy(void *user, uint8_t *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif
