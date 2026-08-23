#include "h2_posix_pal_core.h"

#include <errno.h>
#include <sys/random.h>

int h2_posix_entropy(void *user, uint8_t *out, size_t len) {
    (void)user;
    if (out == NULL && len != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    while (len > 0u) {
        size_t chunk = len > 256u ? 256u : len;
        int rc;
        do {
            rc = getentropy(out, chunk);
        } while (rc != 0 && errno == EINTR);
        if (rc != 0) {
            return H2_PAL_ERR_IO;
        }
        out += chunk;
        len -= chunk;
    }
    return H2_PAL_OK;
}
