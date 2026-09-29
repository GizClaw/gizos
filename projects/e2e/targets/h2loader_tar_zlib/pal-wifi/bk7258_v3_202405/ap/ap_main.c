#include "bk_private/bk_init.h"
#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_target_task_policy.h"
#include "h2_pal_wifi_device.h"
#include "h2_bk_dhcp_ring.h"
#include <os/mem.h>
#include <os/os.h>
#include <stdio.h>
static h2_runtime_t *runtime;
/* The packet path only writes fixed metadata. RPC and printing happen here,
 * outside lwIP and radio callbacks, so diagnostics cannot re-enter those paths. */
static uint32_t diag_stop_requested, diag_running, diag_error, diag_observed;
static h2_pal_task_t *diag_task;
static void dhcp_diag(void *user) {
    h2_bk_dhcp_snapshot_t *snapshot = user;
    uint32_t seen[H2_BK_DHCP_RING_CAPACITY] = {0};
    uint32_t last_dropped = 0, last_ticket = 0;
    while (!__atomic_load_n(&diag_stop_requested, __ATOMIC_ACQUIRE)) {
        int rc = h2_bk_dhcp_query(snapshot);
        if (rc != H2_PAL_OK) {
            if (__atomic_exchange_n(&diag_error, (uint32_t)rc, __ATOMIC_ACQ_REL) == 0u)
                printf("H2_BK_DHCP_QUERY_FAIL rc=%d\n", rc);
        } else {
            if (snapshot->last_ticket != last_ticket) {
                last_ticket = snapshot->last_ticket;
                printf("H2_BK_DHCP_SNAPSHOT last_ticket=%lu count=%u dropped=%lu\n",
                       (unsigned long)last_ticket, (unsigned)snapshot->count,
                       (unsigned long)snapshot->dropped);
            }
            for (uint32_t i = 0; i < snapshot->count; ++i) {
                const h2_bk_dhcp_entry_t *e = &snapshot->entries[i];
                if (e->ticket == 0u) continue;
                uint32_t slot = (e->ticket - 1u) % H2_BK_DHCP_RING_CAPACITY;
                if (seen[slot] == e->ticket) continue;
                seen[slot] = e->ticket;
                if ((e->dir == 1u || e->dir == 3u) && e->role == 0u &&
                    e->src == 68u && e->dst == 67u &&
                    (e->type == 1u || e->type == 3u))
                    (void)__atomic_fetch_or(&diag_observed, 1u, __ATOMIC_RELAXED);
                if (e->dir == 2u && e->role == 0u &&
                    e->src == 67u && e->dst == 68u &&
                    (e->type == 2u || e->type == 5u))
                    (void)__atomic_fetch_or(&diag_observed, 2u, __ATOMIC_RELAXED);
                const char *direction =
                    e->dir == 1u ? "CP_TX" : e->dir == 2u ? "CP_RX" :
                    e->dir == 3u ? "HOST_TX" : "HOST_TX_RESULT";
                printf("H2_BK_DHCP dir=%s type=%u src=%u dst=%u xid=%lu "
                       "vif=%ld role=%ld netif=%u flags=%u ip4=%lu default=%u "
                       "bytes=%u ticket=%lu ip_src=%lu ip_dst=%lu yiaddr=%lu "
                       "server_id=%lu udp_checksum=%u bootp_flags=%u "
                       "eth_src=%04lx%08lx eth_dst=%04lx%08lx "
                       "chaddr=%04lx%08lx send_rc=%ld\n",
                       direction, (unsigned)e->type,
                       (unsigned)e->src, (unsigned)e->dst, (unsigned long)e->xid,
                       (long)(int32_t)e->vif, (long)(int32_t)e->role,
                       (unsigned)e->netif, (unsigned)e->flags,
                       (unsigned long)e->ip4, (unsigned)e->is_default,
                       (unsigned)e->bytes, (unsigned long)e->ticket,
                       (unsigned long)e->ip_src, (unsigned long)e->ip_dst,
                       (unsigned long)e->yiaddr, (unsigned long)e->server_id,
                       (unsigned)e->udp_checksum, (unsigned)e->bootp_flags,
                       (unsigned long)e->eth_src_hi, (unsigned long)e->eth_src_lo,
                       (unsigned long)e->eth_dst_hi, (unsigned long)e->eth_dst_lo,
                       (unsigned long)e->chaddr_hi, (unsigned long)e->chaddr_lo,
                       (long)(int32_t)e->send_rc);
            }
            if (snapshot->dropped != last_dropped) {
                last_dropped = snapshot->dropped;
                printf("H2_BK_DHCP_DROPPED count=%lu\n", (unsigned long)last_dropped);
            }
        }
        rtos_delay_milliseconds(1000u);
    }
    os_free(snapshot);
    __atomic_store_n(&diag_running, 0u, __ATOMIC_RELEASE);
}
static int start_diag(void) {
    h2_bk_dhcp_snapshot_t *snapshot = os_malloc(sizeof(*snapshot));
    if (snapshot == NULL) return H2_PAL_ERR_NO_MEMORY;
    __atomic_store_n(&diag_running, 1u, __ATOMIC_RELEASE);
    const h2_pal_task_options_t options = {.name = "pal-wifi/e2e/dhcpdiag"};
    int rc = h2_pal_task_start(runtime->task, &options, dhcp_diag, snapshot, &diag_task);
    if (rc != H2_PAL_OK) {
        __atomic_store_n(&diag_running, 0u, __ATOMIC_RELEASE);
        os_free(snapshot);
    }
    return rc;
}
static int stop_diag(void) {
    __atomic_store_n(&diag_stop_requested, 1u, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < 50u && __atomic_load_n(&diag_running, __ATOMIC_ACQUIRE); ++i)
        rtos_delay_milliseconds(100u);
    if (__atomic_load_n(&diag_running, __ATOMIC_ACQUIRE)) return H2_PAL_ERR_TIMEOUT;
    int rc = h2_pal_task_join(runtime->task, diag_task);
    if (rc != H2_PAL_OK) return rc;
    diag_task = NULL;
    rc = (int32_t)__atomic_load_n(&diag_error, __ATOMIC_ACQUIRE);
    if (rc != H2_PAL_OK) return rc;
    return __atomic_load_n(&diag_observed, __ATOMIC_RELAXED) == 3u
               ? H2_PAL_OK : H2_PAL_ERR_IO;
}
static void hold(void) {
    for (;;)
        rtos_delay_milliseconds(1000u);
}
static void fail(const char *stage, int rc) {
    printf("H2_WIFI_SETUP_FAIL stage=%s rc=%d\n", stage, rc);
    fflush(stdout);
    hold();
}
static void run(void *user) {
    (void)user;
    rtos_delay_milliseconds(3000u);
    h2_pal_firmware_info_t info = {0};
    int rc = h2_pal_firmware_info_get_current(runtime->firmware_info, &info);
    if (rc)
        fail("firmware-info", rc);
    rc = h2_wifi_device_run(runtime, info.version, "bk7258");
    int diag_rc = stop_diag();
    printf("H2_BK_DHCP_DIAG_DONE rc=%d observed=%u\n", diag_rc,
           (unsigned)__atomic_load_n(&diag_observed, __ATOMIC_RELAXED));
    if (rc == H2_PAL_OK && diag_rc != H2_PAL_OK) rc = diag_rc;
    int confirm =
        rc == H2_PAL_OK ? h2_bk_h2loader_confirm_current_app(runtime) : H2_PAL_ERR_INVALID_STATE;
    printf("H2_WIFI_READY board=bk7258 rc=%d confirm=%d\n", rc, confirm);
    fflush(stdout);
    hold();
}
static void entry(void *user) {
    (void)user;
    h2_runtime_config_t config = {0};
    int rc = h2_bk7258_board_runtime_config(&config);
    if (rc)
        fail("board", rc);
    config.event_queue_capacity = 64;
    rc = h2_runtime_init(&config, &runtime);
    if (rc)
        fail("runtime", rc);
    rc = h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
        runtime, "pal-wifi", H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI);
    if (rc)
        fail("commands", rc);
    rc = start_diag();
    if (rc)
        fail("dhcpdiag", rc);
    const h2_pal_task_options_t options = {.name = h2_wifi_device_runner_task_name};
    h2_pal_task_t *runner = NULL;
    rc = h2_pal_task_start(runtime->task, &options, run, NULL, &runner);
    if (rc)
        fail("runner", rc);
    hold();
}
int main(void) {
    int rc = h2_bk_target_task_policy_install();
    if (rc)
        return -1;
    bk_init();
    rc = h2_bk7258_board_start_entry_task("bk/pal-wifi", entry, NULL);
    if (rc)
        fail("entry", rc);
    return 0;
}
