#include "h2_windows_internal.h"

#include <bcrypt.h>

int h2_windows_entropy(void *user, uint8_t *out, size_t len) {
    (void)user;
    if ((out == NULL && len != 0u) || len > (size_t)ULONG_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (len == 0u) {
        return H2_PAL_OK;
    }
    NTSTATUS status = BCryptGenRandom(NULL, out, (ULONG)len,
                                     BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status >= 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}
