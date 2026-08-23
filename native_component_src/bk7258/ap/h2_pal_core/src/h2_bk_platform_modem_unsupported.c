#include "h2_bk_platform_core.h"

static h2_pal_result_t h2_bk_modem_get_capabilities(
    void *user,
    uint32_t *out_capabilities) {
    (void)user;
    if (out_capabilities == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_capabilities = 0u;
    return H2_PAL_OK;
}

h2_pal_modem_t *h2_bk_platform_modem_unsupported(void) {
    static const h2_pal_modem_vtable_t vtable = {
        .get_capabilities = h2_bk_modem_get_capabilities,
    };
    static h2_pal_modem_t modem = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &modem;
}
