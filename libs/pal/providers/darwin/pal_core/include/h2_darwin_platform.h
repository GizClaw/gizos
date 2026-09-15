#ifndef H2_DARWIN_PLATFORM_H
#define H2_DARWIN_PLATFORM_H

#include "h2/pal/hal/h2_pal_ble.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_log.h"
#include "h2/pal/net/h2_pal_net.h"
#include "h2/pal/net/h2_pal_netif.h"
#include "h2/pal/os/h2_pal_fs.h"
#include "h2/pal/os/h2_pal_serial_host.h"
#include "h2/pal/os/h2_pal_system_event.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_darwin_host_fs h2_darwin_host_fs_t;

int h2_darwin_host_fs_create(const char *const *sources,
                             const char *const *targets, size_t mount_count,
                             h2_darwin_host_fs_t **out_fs);
void h2_darwin_host_fs_destroy(h2_darwin_host_fs_t *fs);
const h2_pal_fs_api_t *h2_darwin_host_fs_api(h2_darwin_host_fs_t *fs);
const h2_pal_net_api_t *h2_darwin_net_api(void);
int h2_darwin_entropy(void *user, uint8_t *out, size_t len);

/** Return the Darwin network-interface provider. */
const h2_pal_netif_api_t *h2_darwin_netif_api(void);

/** Return the Darwin system-event provider. */
const h2_pal_system_event_api_t *h2_darwin_system_event_api(void);

/** Return the Darwin Host Serial provider. */
const h2_pal_serial_host_api_t *h2_darwin_serial_host_api(void);

/**
 * @brief Bind borrowed Memory/Log services and return the CoreBluetooth provider.
 *
 * The first successful call binds both services for the provider lifetime.
 * Repeated calls with the same API pointers return the same provider. NULL,
 * incomplete, or different services return NULL. The caller keeps both API
 * objects and their user state alive; asynchronous diagnostics run on the
 * backend queue, so the Log service must support calls from that queue.
 */
h2_pal_ble_t *h2_darwin_corebluetooth_ble(
    const h2_pal_mem_api_t *allocator, const h2_pal_log_api_t *log);

#ifdef __cplusplus
}
#endif

#endif
