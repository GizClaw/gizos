#ifndef H2_DARWIN_NETIF_INTERNAL_H
#define H2_DARWIN_NETIF_INTERNAL_H

#include "h2/pal/net/h2_pal_netif.h"

#include <pthread.h>

typedef struct h2_darwin_netif_os_monitor {
    int route_fd;
    int wake_read_fd;
    int wake_write_fd;
} h2_darwin_netif_os_monitor_t;

h2_pal_result_t h2_darwin_netif_os_default_name(
    char out_name[H2_PAL_NETIF_NAME_MAX]);
h2_pal_result_t h2_darwin_netif_os_monitor_open(
    h2_darwin_netif_os_monitor_t *monitor);
h2_pal_result_t h2_darwin_netif_os_monitor_wait(
    h2_darwin_netif_os_monitor_t *monitor);
void h2_darwin_netif_os_monitor_wake(
    h2_darwin_netif_os_monitor_t *monitor);
void h2_darwin_netif_os_monitor_close(
    h2_darwin_netif_os_monitor_t *monitor);

h2_pal_result_t h2_darwin_netif_monitor_start(void);
void h2_darwin_netif_monitor_stop(void);
#if defined(H2_DARWIN_NETIF_TESTING)
void h2_darwin_netif_test_set_snapshot(
    const h2_pal_netif_status_t *entries,
    size_t count,
    const h2_pal_netif_dns_server_t *dns,
    size_t dns_count);
void h2_darwin_netif_test_set_default(
    const h2_pal_netif_ref_t *ref,
    int valid);
h2_pal_result_t h2_darwin_netif_test_reconcile_default(
    const h2_pal_netif_ref_t *ref,
    int valid);
#endif

#endif
