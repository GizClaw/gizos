#ifndef H2_H2LOADER_HOST_LIFECYCLE_H
#define H2_H2LOADER_HOST_LIFECYCLE_H
#include "h2_h2loader_host.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Verify role, boot intent and retained package metadata after a reboot.
 * The caller must already have verified the same authoritative device UID.
 * before and after are borrowed snapshots. NULL snapshots return INVALID_ARG.
 */
h2_pal_result_t h2_h2loader_host_verify_reboot_status(
    h2_h2loader_host_command_t kind, const h2_h2loader_host_status_t* before,
    const h2_h2loader_host_status_t* after);
/** Return nonzero while the exact staged App is running but still awaits its
 * own confirmation. Both snapshots are borrowed; NULL returns zero. */
int h2_h2loader_host_app_awaits_confirmation(
    const h2_h2loader_host_status_t* before,
    const h2_h2loader_host_status_t* after);
#ifdef __cplusplus
}
#endif
#endif
