#include <stdbool.h>
#include <stdint.h>

#include "att.h"
#include "attm.h"

/* The pinned SDK declares this legacy helper but omits its implementation.
 * Keep the compatibility symbol weak so an SDK that supplies it takes
 * precedence, and preserve every service-permission bit except visibility. */
__attribute__((weak)) uint8_t attmdb_svc_visibility_set(
    uint16_t handle, bool hide)
{
    uint8_t permission = 0u;
    uint8_t status = attm_svc_get_permission(handle, &permission);
    if (status != ATT_ERR_NO_ERROR) {
        return status;
    }

    if (hide) {
        permission = (uint8_t)(permission | PERM(SVC_DIS, ENABLE));
    } else {
        permission = (uint8_t)(permission & ~PERM(SVC_DIS, ENABLE));
    }
    return attm_svc_set_permission(handle, permission);
}
