#include "h2_bk_platform_core.h"

#include "h2_bk_dhcp_ring.h"
#include "h2_bk_wifi_ipv6_internal.h"
#include "h2_bk_wifi_ipv6_sync.h"
#include "h2_bk_wifi_lease.h"
#include "h2_bk_wifi_lease_slots.h"
#include "h2_bk_wifi_rpc.h"
#include "lwip/def.h"
#include "lwip/nd6.h"
#include "lwip/netif.h"
#include "lwip/priv/tcpip_priv.h"
#include "lwip/tcpip.h"
#include "net.h"
#include "wdrv_cntrl.h"
#include "wifi_api_ipc.h"
#include <common/bk_err.h>
#include <components/event.h>
#include <components/netif.h>
#include <modules/wifi.h>
#include <os/mem.h>
#include <os/os.h>

/* Exported by the pinned AP SDK, although omitted from its public header. */
extern bool wifi_sta_is_started(void);

#include <string.h>
#include "h2_atomic_static.h"

#include "h2_wifi_sta.h"

#include "FreeRTOS.h"
#include "semphr.h"

static beken_semaphore_t s_h2_bk_wifi_scan_sem;
static int s_h2_bk_wifi_events_registered;
static StaticSemaphore_t s_h2_bk_wifi_ipv6_mutex_control;
static beken_mutex_t s_h2_bk_wifi_ipv6_mutex;
#if LWIP_IPV6
static uint32_t s_h2_bk_wifi_cp_generation;
static int s_h2_bk_wifi_ipv6_watch_started;
#endif
static int s_h2_bk_wifi_ap_active;
static int s_h2_bk_wifi_ap_starting;
static h2_pal_wifi_ap_status_t s_h2_bk_wifi_ap_status;
static h2_pal_wifi_ap_config_t s_h2_bk_wifi_ap_config;
static h2_bk_wifi_accepted_lease_t s_h2_bk_wifi_leases[H2_BK_WIFI_LEASE_CAPACITY];
static uint32_t s_h2_bk_wifi_lease_generation;
static uint32_t s_h2_bk_wifi_lease_wake_hint;
static int s_h2_bk_wifi_lease_error;
static uint32_t s_h2_bk_wifi_lease_watch_stop;
static uint32_t s_h2_bk_wifi_lease_watch_running;
static int s_h2_bk_wifi_connect_pending;
static h2_pal_wifi_power_save_t s_h2_bk_wifi_power_save;
static int s_h2_bk_wifi_power_save_set;
static int s_h2_bk_wifi_power_save_error;
static uint32_t s_h2_bk_wifi_connect_generation;
static h2_pal_wifi_sta_status_t s_h2_bk_wifi_connect_status;
static int s_h2_bk_wifi_sta_status_valid;
static h2_pal_wifi_sta_status_t s_h2_bk_wifi_sta_status;
static uint32_t s_h2_bk_wifi_had_ip;
static int s_h2_bk_wifi_last_config_valid;
static h2_pal_wifi_sta_config_t s_h2_bk_wifi_last_config;
static StaticSemaphore_t s_h2_bk_wifi_request_mutex_control;
static beken_mutex_t s_h2_bk_wifi_request_mutex;
static StaticSemaphore_t s_h2_bk_wifi_status_mutex_control;
static beken_mutex_t s_h2_bk_wifi_status_mutex;
static StaticSemaphore_t s_h2_bk_wifi_event_mutex_control;
static beken_mutex_t s_h2_bk_wifi_event_mutex;

static int h2_bk_wifi_ap_active(void) {
    return __atomic_load_n(&s_h2_bk_wifi_ap_active, __ATOMIC_ACQUIRE);
}

static int h2_bk_wifi_ap_starting(void) {
    return __atomic_load_n(&s_h2_bk_wifi_ap_starting, __ATOMIC_ACQUIRE);
}

static int h2_bk_wifi_event_lock(void) {
    return s_h2_bk_wifi_event_mutex != NULL &&
        rtos_lock_mutex(&s_h2_bk_wifi_event_mutex) == kNoErr
        ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}

static void h2_bk_wifi_event_unlock(void) {
    if (s_h2_bk_wifi_event_mutex != NULL)
        (void)rtos_unlock_mutex(&s_h2_bk_wifi_event_mutex);
}

static int h2_bk_wifi_request_lock(void) {
    return s_h2_bk_wifi_request_mutex != NULL &&
        rtos_lock_mutex(&s_h2_bk_wifi_request_mutex) == kNoErr
        ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}

static void h2_bk_wifi_request_unlock(void) {
    if (s_h2_bk_wifi_request_mutex != NULL) {
        (void)rtos_unlock_mutex(&s_h2_bk_wifi_request_mutex);
    }
}

static int h2_bk_wifi_status_lock(void) {
    return s_h2_bk_wifi_status_mutex != NULL &&
        rtos_lock_mutex(&s_h2_bk_wifi_status_mutex) == kNoErr
        ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}

static void h2_bk_wifi_status_unlock(void) {
    if (s_h2_bk_wifi_status_mutex != NULL) {
        (void)rtos_unlock_mutex(&s_h2_bk_wifi_status_mutex);
    }
}

static int h2_bk_wifi_ipv6_refresh(void);
#if LWIP_IPV6
static void h2_bk_wifi_ipv6_watch(void *user);
#endif

static int h2_bk_wifi_sta_get_status(
    h2_pal_wifi_sta_t *sta,
    h2_pal_wifi_sta_status_t *out_status);

static size_t h2_bk_wifi_strnlen(const char *value, size_t max_len) {
    size_t len = 0u;
    while (len < max_len && value[len] != '\0') {
        len++;
    }
    return len;
}

static int h2_bk_wifi_parse_ip4(const char *value, uint32_t *out_ip) {
    if (value == NULL || out_ip == NULL) {
        return 0;
    }

    uint32_t octets[4] = {0};
    const char *cursor = value;
    for (size_t i = 0u; i < 4u; ++i) {
        if (*cursor < '0' || *cursor > '9') {
            return 0;
        }

        uint32_t octet = 0u;
        while (*cursor >= '0' && *cursor <= '9') {
            octet = (octet * 10u) + (uint32_t)(*cursor - '0');
            if (octet > 255u) {
                return 0;
            }
            cursor++;
        }
        octets[i] = octet;

        if (i < 3u) {
            if (*cursor != '.') {
                return 0;
            }
            cursor++;
        }
    }

    if (*cursor != '\0') {
        return 0;
    }

    *out_ip = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    return 1;
}

static int h2_bk_wifi_map_error(bk_err_t err) {
    switch (err) {
    case BK_OK:
        return H2_PAL_OK;
    case BK_ERR_PARAM:
    case BK_ERR_NULL_PARAM:
    case BK_ERR_WIFI_CHAN_RANGE:
    case BK_ERR_WIFI_CHAN_NUMBER:
    case BK_ERR_WIFI_RESERVED_FIELD:
        return H2_PAL_ERR_INVALID_ARG;
    case BK_ERR_WIFI_NOT_INIT:
        return H2_PAL_ERR_UNAVAILABLE;
    case BK_ERR_NOT_SUPPORT:
        return H2_PAL_ERR_UNSUPPORTED;
    case BK_ERR_NO_MEM:
        return H2_PAL_ERR_NO_MEMORY;
    case BK_ERR_TIMEOUT:
        return H2_PAL_ERR_TIMEOUT;
    case BK_ERR_BUSY:
    case BK_ERR_IN_PROGRESS:
    case BK_ERR_STATE:
    case BK_ERR_WIFI_MONITOR_IP:
        return H2_PAL_ERR_INVALID_STATE;
    case BK_ERR_NOT_FOUND:
    case BK_ERR_WIFI_STA_NOT_CONFIG:
    case BK_ERR_WIFI_STA_NOT_STARTED:
    case BK_ERR_WIFI_AP_NOT_CONFIG:
    case BK_ERR_WIFI_AP_NOT_STARTED:
        return H2_PAL_ERR_NOT_FOUND;
    default:
        BK_LOGW("h2_wifi", "H2_WIFI_SDK_ERROR code=%d\r\n", (int)err);
        return H2_PAL_ERR_IO;
    }
}

int h2_bk_dhcp_query(h2_bk_dhcp_snapshot_t *out) {
    if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    bk_err_t err = wifi_send_com_api_cmd(H2_BK_WIFI_RPC_DHCP_SNAPSHOT, 1,
                                        (uint32_t)(uintptr_t)out);
    if (err != BK_OK) return h2_bk_wifi_map_error(err);
    if (out->version != H2_BK_DHCP_SNAPSHOT_VERSION ||
        out->count > H2_BK_DHCP_RING_CAPACITY) return H2_PAL_ERR_IO;
    return H2_PAL_OK;
}

static int h2_bk_wifi_apply_power_save(h2_pal_wifi_power_save_t mode) {
    if (mode == H2_PAL_WIFI_POWER_SAVE_NONE)
        return h2_bk_wifi_map_error(bk_wifi_sta_pm_disable());
    /* Select the SDK's minimum recommended listen interval for MIN, including
     * the next DTIM. MAX spans ten beacon intervals. No wake/power measurement
     * is implied by the firmware configuration readback. */
    uint8_t interval = mode == H2_PAL_WIFI_POWER_SAVE_MAX_MODEM ? 10u : 1u;
    uint8_t before = 0xffu;
    bk_err_t err = bk_wifi_get_listen_interval(&before);
    if (err == BK_OK) err = bk_wifi_send_listen_interval_req(interval);
    uint8_t actual = 0xffu;
    if (err == BK_OK) err = bk_wifi_get_listen_interval(&actual);
    if (err != BK_OK) return h2_bk_wifi_map_error(err);
    BK_LOGI("h2_wifi", "H2_WIFI_POWER_SAVE_READBACK mode=%u before=%u requested=%u actual=%u\r\n",
            (unsigned)mode, (unsigned)before, (unsigned)interval, (unsigned)actual);
    if (actual != interval) return H2_PAL_ERR_UNSUPPORTED;
    err = bk_wifi_sta_pm_enable();
    if (err != BK_OK) return h2_bk_wifi_map_error(err);
    BK_LOGI("h2_wifi", "H2_WIFI_POWER_SAVE mode=%u listen_interval=%u verified=1\r\n",
            (unsigned)mode, (unsigned)actual);
    return H2_PAL_OK;
}

static int h2_bk_wifi_post_system_event_payload(
    h2_pal_system_event_type_t type,
    const void *payload,
    size_t payload_size) {
    const h2_pal_system_event_api_t *api = h2_bk_platform_system_event_api();
    if (api == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }

    h2_pal_system_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    if (payload != NULL && payload_size > 0u) {
        event.payload = payload;
        event.payload_size = payload_size;
    }
    return h2_pal_system_event_post(api, &event, 0u);
}

static void h2_bk_wifi_post_sta_system_event(
    h2_pal_system_event_type_t type,
    const h2_pal_wifi_sta_status_t *status) {
    h2_bk_wifi_post_system_event_payload(type, status, status != NULL ? sizeof(*status) : 0u);
}

static void h2_bk_wifi_store_sta_status(
    const h2_pal_wifi_sta_status_t *status) {
    if (status == NULL || h2_bk_wifi_status_lock() != H2_PAL_OK) {
        return;
    }
    s_h2_bk_wifi_sta_status = *status;
    s_h2_bk_wifi_sta_status_valid = 1;
    h2_bk_wifi_status_unlock();
}

static int h2_bk_wifi_load_sta_status(
    h2_pal_wifi_sta_status_t *out_status) {
    if (out_status == NULL || h2_bk_wifi_status_lock() != H2_PAL_OK) {
        return 0;
    }
    if (s_h2_bk_wifi_sta_status_valid == 0) {
        h2_bk_wifi_status_unlock();
        return 0;
    }
    *out_status = s_h2_bk_wifi_sta_status;
    h2_bk_wifi_status_unlock();
    return 1;
}

static void h2_bk_wifi_publish_disconnected(int reason) {
    if (h2_bk_wifi_event_lock() != H2_PAL_OK) return;
    h2_pal_wifi_sta_status_t status;
    memset(&status, 0, sizeof(status));
    (void)h2_bk_wifi_load_sta_status(&status);
    if (__atomic_exchange_n(&s_h2_bk_wifi_had_ip, 0u, __ATOMIC_ACQ_REL)) {
        status.state = H2_PAL_WIFI_STA_STATE_DISCONNECTED;
        status.ip_valid = 0u;
        memset(&status.ip, 0, sizeof(status.ip));
        status.disconnect_reason = reason;
        h2_bk_wifi_post_sta_system_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_LOST_IP, &status);
    }
    memset(&status, 0, sizeof(status));
    status.state = H2_PAL_WIFI_STA_STATE_DISCONNECTED;
    status.disconnect_reason = reason;
    h2_bk_wifi_store_sta_status(&status);
    h2_bk_wifi_post_sta_system_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_DISCONNECTED, &status);
    h2_bk_wifi_event_unlock();
}

static int h2_bk_wifi_post_ap_system_event(
    h2_pal_system_event_type_t type,
    const h2_pal_wifi_ap_status_t *status) {
    h2_pal_wifi_ap_event_t event;
    memset(&event, 0, sizeof(event));
    if (status != NULL) {
        event.status = *status;
    }
    return h2_bk_wifi_post_system_event_payload(type, &event, sizeof(event));
}

static int h2_bk_wifi_post_ap_client_system_event(
    h2_pal_system_event_type_t type,
    const h2_pal_wifi_ap_client_t *client) {
    h2_pal_wifi_ap_client_event_t event;
    memset(&event, 0, sizeof(event));
    if (client != NULL) {
        event.client = *client;
    }
    return h2_bk_wifi_post_system_event_payload(type, &event, sizeof(event));
}

static int h2_bk_wifi_post_join_event(
    const h2_pal_wifi_ap_client_t *client, void *context) {
    (void)context;
    return h2_bk_wifi_post_ap_client_system_event(
        H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_CLIENT_JOINED, client);
}

static void h2_bk_wifi_lease_mac(uint32_t hi, uint32_t lo, uint8_t out[6]) {
    out[0] = (uint8_t)(hi >> 8u);
    out[1] = (uint8_t)hi;
    out[2] = (uint8_t)(lo >> 24u);
    out[3] = (uint8_t)(lo >> 16u);
    out[4] = (uint8_t)(lo >> 8u);
    out[5] = (uint8_t)lo;
}

static h2_bk_wifi_accepted_lease_t *h2_bk_wifi_lease_find_locked(
    const uint8_t mac[6], int create) {
    int index = h2_bk_wifi_lease_slot_index(s_h2_bk_wifi_leases, mac, create);
    if (index < 0) {
        if (create) s_h2_bk_wifi_lease_error = H2_PAL_ERR_NO_MEMORY;
        return NULL;
    }
    h2_bk_wifi_accepted_lease_t *lease = &s_h2_bk_wifi_leases[index];
    if (!lease->used || memcmp(lease->client.mac, mac, 6u) != 0) {
        memset(lease, 0, sizeof(*lease));
        lease->used = 1u;
        memcpy(lease->client.mac, mac, 6u);
    }
    return lease;
}

static void h2_bk_wifi_lease_grant_locked(h2_bk_wifi_accepted_lease_t *lease) {
    if (!h2_bk_wifi_lease_ready_for_grant(lease)) return;
    h2_pal_wifi_ap_client_t event_client = lease->client;
    event_client.lease_valid = 1u;
    if (h2_bk_wifi_post_ap_client_system_event(
            H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_LEASE_GRANTED, &event_client) == H2_PAL_OK) {
        lease->client.lease_valid = 1u;
        lease->granted = 1u;
        lease->pending = 0u;
    }
}

static void h2_bk_wifi_lease_accept_locked(uint32_t generation, uint32_t sequence,
                                            uint32_t ip4, uint32_t mac_hi,
                                            uint32_t mac_lo, uint32_t xid) {
    if (!h2_bk_wifi_ap_active() || generation != s_h2_bk_wifi_lease_generation ||
        !sequence || !ip4 || mac_hi > 0xffffu) return;
    uint8_t mac[6];
    h2_bk_wifi_lease_mac(mac_hi, mac_lo, mac);
    if ((mac[0] & 1u) || (mac_hi == 0u && mac_lo == 0u)) return;
    /* An ACK may reach CP before WDRV delivers L2 JOIN. Leave it in CP's
     * authoritative snapshot until JOIN allocates the local slot; never let a
     * stale departed CP record consume a slot or grant a future association. */
    h2_bk_wifi_accepted_lease_t *lease = h2_bk_wifi_lease_find_locked(mac, 0);
    if (lease == NULL) return;
    /* Keep the old sequence after LEFT. A delayed ACK from that association
     * must not become a pending grant for its next JOIN. The next accepted
     * DHCP XID is read again after the new JOIN. */
    if (!lease->joined && lease->sequence != 0u) return;
    if (sequence <= lease->sequence) {
        if (sequence == lease->sequence) h2_bk_wifi_lease_grant_locked(lease);
        return;
    }
    /* An old grant may change addresses after DHCP renewal. Preserve its
     * RELEASED event if the bounded Runtime queue cannot accept it yet. */
    if (lease->granted && lease->client.lease.ip4 != ip4) {
        if (h2_bk_wifi_post_ap_client_system_event(
                H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_LEASE_RELEASED,
                &lease->client) != H2_PAL_OK) return;
        lease->granted = 0u;
        lease->client.lease_valid = 0u;
    }
    lease->sequence = sequence;
    lease->xid = xid;
    lease->client.lease.ip4 = ip4;
    lease->pending = 1u;
    h2_bk_wifi_lease_grant_locked(lease);
}

/* WDRV invokes this on its receive path. It cannot synchronously ask CP to
 * authenticate a wake: that would wait on the same IPC worker. Keep only a
 * pending hint. The ordinary AP methods fetch the authoritative snapshot and
 * post GRANTED only while its matching ACK record still exists. */
static void h2_bk_wifi_lease_wake_callback(void *data, uint16_t len) {
    if (data == NULL || len != sizeof(h2_bk_wifi_lease_wake_t)) return;
    h2_bk_wifi_lease_wake_t wake;
    memcpy(&wake, data, sizeof(wake));
    if (wake.magic != H2_BK_WIFI_LEASE_WAKE_MAGIC ||
        wake.version != H2_BK_WIFI_LEASE_VERSION ||
        h2_bk_wifi_event_lock() != H2_PAL_OK) return;
    if (h2_bk_wifi_ap_active() && wake.generation == s_h2_bk_wifi_lease_generation &&
        wake.sequence > s_h2_bk_wifi_lease_wake_hint)
        s_h2_bk_wifi_lease_wake_hint = wake.sequence;
    h2_bk_wifi_event_unlock();
}

static int h2_bk_wifi_lease_query(h2_bk_wifi_lease_snapshot_t *out) {
    memset(out, 0, sizeof(*out));
    bk_err_t err = wifi_send_com_api_cmd(H2_BK_WIFI_RPC_LEASE_SNAPSHOT, 1,
                                         (uint32_t)(uintptr_t)out);
    if (err != BK_OK) return h2_bk_wifi_map_error(err);
    if (out->version != H2_BK_WIFI_LEASE_VERSION ||
        out->count > H2_BK_WIFI_LEASE_CAPACITY || !out->generation)
        return H2_PAL_ERR_IO;
    return H2_PAL_OK;
}

static int h2_bk_wifi_lease_flush_left_locked(h2_bk_wifi_accepted_lease_t *lease) {
    if (lease->left_pending) {
        if (h2_bk_wifi_post_ap_client_system_event(
                H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_CLIENT_LEFT, &lease->client) != H2_PAL_OK)
            return 0;
        lease->left_pending = 0u;
    }
    if (lease->release_pending) {
        if (h2_bk_wifi_post_ap_client_system_event(
                H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_LEASE_RELEASED, &lease->client) != H2_PAL_OK)
            return 0;
        lease->release_pending = 0u;
        lease->granted = 0u;
        lease->client.lease_valid = 0u;
    }
    return 1;
}

static int h2_bk_wifi_lease_reconcile(void) {
    if (h2_bk_wifi_ap_starting()) return H2_PAL_ERR_INVALID_STATE;
    h2_bk_wifi_lease_snapshot_t snapshot;
    int rc = h2_bk_wifi_lease_query(&snapshot);
    if (rc != H2_PAL_OK) return rc;
    if (h2_bk_wifi_event_lock() != H2_PAL_OK) return H2_PAL_ERR_INVALID_STATE;
    if (h2_bk_wifi_ap_starting() ||
        snapshot.generation != s_h2_bk_wifi_lease_generation ||
        snapshot.last_sequence < s_h2_bk_wifi_lease_wake_hint)
        rc = H2_PAL_ERR_INVALID_STATE;
    else {
        for (unsigned i = 0u; i < H2_BK_WIFI_LEASE_CAPACITY; ++i) {
            h2_bk_wifi_accepted_lease_t *lease = &s_h2_bk_wifi_leases[i];
            if (!lease->used) continue;
            if (!h2_bk_wifi_lease_flush_left_locked(lease) ||
                !h2_bk_wifi_lease_drain_join(
                    lease, 1, h2_bk_wifi_post_join_event, NULL))
                rc = H2_PAL_ERR_TIMEOUT;
        }
        for (uint32_t i = 0u; i < snapshot.count; ++i) {
            const h2_bk_wifi_lease_record_t *record = &snapshot.records[i];
            h2_bk_wifi_lease_accept_locked(snapshot.generation, record->sequence,
                                           record->ip4, record->mac_hi,
                                           record->mac_lo, record->xid);
        }
        if (s_h2_bk_wifi_lease_error != H2_PAL_OK) rc = s_h2_bk_wifi_lease_error;
        s_h2_bk_wifi_lease_wake_hint = 0u;
    }
    h2_bk_wifi_event_unlock();
    return rc;
}

/* The hint callback runs on WDRV's IPC path and cannot query CP itself.
 * Reconcile on an ordinary bounded worker even if no caller invokes any AP
 * method, so a Runtime-only event consumer still observes accepted leases. */
static void h2_bk_wifi_lease_watch_worker(void *user) {
    (void)user;
    while (!__atomic_load_n(&s_h2_bk_wifi_lease_watch_stop, __ATOMIC_ACQUIRE)) {
        if (h2_bk_wifi_ap_active())
            (void)h2_bk_wifi_lease_reconcile();
        rtos_delay_milliseconds(200u);
    }
    __atomic_store_n(&s_h2_bk_wifi_lease_watch_running, 0u, __ATOMIC_RELEASE);
    rtos_delete_thread(NULL);
}

static int h2_bk_wifi_lease_watch_start(void) {
    if (__atomic_load_n(&s_h2_bk_wifi_lease_watch_running, __ATOMIC_ACQUIRE))
        return H2_PAL_ERR_INVALID_STATE;
    __atomic_store_n(&s_h2_bk_wifi_lease_watch_stop, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_h2_bk_wifi_lease_watch_running, 1u, __ATOMIC_RELEASE);
    if (rtos_create_psram_thread(NULL, BEKEN_APPLICATION_PRIORITY,
                                 "h2_wifi_lease", h2_bk_wifi_lease_watch_worker,
                                 4096u, NULL) != kNoErr) {
        __atomic_store_n(&s_h2_bk_wifi_lease_watch_running, 0u, __ATOMIC_RELEASE);
        return H2_PAL_ERR_NO_MEMORY;
    }
    return H2_PAL_OK;
}

static int h2_bk_wifi_lease_watch_stop(void) {
    __atomic_store_n(&s_h2_bk_wifi_lease_watch_stop, 1u, __ATOMIC_RELEASE);
    for (unsigned i = 0u; i < 50u; ++i) {
        if (!__atomic_load_n(&s_h2_bk_wifi_lease_watch_running, __ATOMIC_ACQUIRE))
            return H2_PAL_OK;
        rtos_delay_milliseconds(100u);
    }
    return H2_PAL_ERR_TIMEOUT;
}

static void h2_bk_wifi_lease_left_locked(const uint8_t mac[6]) {
    h2_bk_wifi_accepted_lease_t *lease = h2_bk_wifi_lease_find_locked(mac, 0);
    if (lease == NULL) return;
    /* A JOIN never seen by Runtime has no LEFT/RELEASED pair to emit. Keep
     * any older association's queued LEFT/RELEASED untouched. */
    if (lease->join_pending) h2_bk_wifi_lease_cancel_join(lease);
    if (!lease->joined) return;
    lease->joined = 0u;
    lease->pending = 0u;
    lease->left_pending = 1u;
    lease->release_pending = lease->granted;
    (void)h2_bk_wifi_lease_flush_left_locked(lease);
}

static int h2_bk_wifi_lease_stop(void) {
    if (h2_bk_wifi_event_lock() != H2_PAL_OK) return H2_PAL_ERR_INVALID_STATE;
    __atomic_store_n(&s_h2_bk_wifi_ap_active, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_h2_bk_wifi_ap_starting, 0, __ATOMIC_RELEASE);
    for (unsigned i = 0u; i < H2_BK_WIFI_LEASE_CAPACITY; ++i)
        if (s_h2_bk_wifi_leases[i].used && s_h2_bk_wifi_leases[i].join_pending)
            h2_bk_wifi_lease_cancel_join(&s_h2_bk_wifi_leases[i]);
    for (unsigned i = 0u; i < H2_BK_WIFI_LEASE_CAPACITY; ++i)
        if (s_h2_bk_wifi_leases[i].used && s_h2_bk_wifi_leases[i].joined)
            h2_bk_wifi_lease_left_locked(s_h2_bk_wifi_leases[i].client.mac);
    for (unsigned attempt = 0u; attempt < 10u; ++attempt) {
        int pending = 0;
        for (unsigned i = 0u; i < H2_BK_WIFI_LEASE_CAPACITY; ++i)
            if (s_h2_bk_wifi_leases[i].used &&
                !h2_bk_wifi_lease_flush_left_locked(&s_h2_bk_wifi_leases[i]))
                pending = 1;
        if (!pending) {
            memset(s_h2_bk_wifi_leases, 0, sizeof(s_h2_bk_wifi_leases));
            s_h2_bk_wifi_lease_generation = 0u;
            s_h2_bk_wifi_lease_wake_hint = 0u;
            s_h2_bk_wifi_lease_error = H2_PAL_OK;
            h2_bk_wifi_event_unlock();
            return H2_PAL_OK;
        }
        h2_bk_wifi_event_unlock();
        rtos_delay_milliseconds(100u);
        if (h2_bk_wifi_event_lock() != H2_PAL_OK) return H2_PAL_ERR_INVALID_STATE;
    }
    h2_bk_wifi_event_unlock();
    return H2_PAL_ERR_TIMEOUT;
}

static h2_pal_wifi_security_t h2_bk_wifi_security(wifi_security_t security) {
    switch (security) {
    case WIFI_SECURITY_NONE:
        return H2_PAL_WIFI_SECURITY_OPEN;
    case WIFI_SECURITY_WEP:
        return H2_PAL_WIFI_SECURITY_WEP;
    case WIFI_SECURITY_WPA_TKIP:
    case WIFI_SECURITY_WPA_AES:
    case WIFI_SECURITY_WPA_MIXED:
        return H2_PAL_WIFI_SECURITY_WPA;
    case WIFI_SECURITY_WPA2_TKIP:
    case WIFI_SECURITY_WPA2_AES:
    case WIFI_SECURITY_WPA2_MIXED:
        return H2_PAL_WIFI_SECURITY_WPA2;
    case WIFI_SECURITY_WPA3_SAE:
        return H2_PAL_WIFI_SECURITY_WPA3;
    case WIFI_SECURITY_WPA3_WPA2_MIXED:
        return H2_PAL_WIFI_SECURITY_WPA2_WPA3;
    case WIFI_SECURITY_EAP:
        return H2_PAL_WIFI_SECURITY_ENTERPRISE;
    default:
        return H2_PAL_WIFI_SECURITY_UNKNOWN;
    }
}

static wifi_security_t h2_bk_wifi_security_to_sdk(h2_pal_wifi_security_t security) {
    switch (security) {
    case H2_PAL_WIFI_SECURITY_OPEN:
        return WIFI_SECURITY_NONE;
    case H2_PAL_WIFI_SECURITY_WPA:
        return WIFI_SECURITY_WPA_AES;
    case H2_PAL_WIFI_SECURITY_WPA2:
        return WIFI_SECURITY_WPA2_AES;
    case H2_PAL_WIFI_SECURITY_WPA3:
        return WIFI_SECURITY_WPA3_SAE;
    case H2_PAL_WIFI_SECURITY_WPA2_WPA3:
        return WIFI_SECURITY_WPA3_WPA2_MIXED;
    default:
        return WIFI_SECURITY_WPA2_AES;
    }
}

static void h2_bk_wifi_copy_ap_client(
    h2_pal_wifi_ap_client_t *out_client,
    const wlan_ap_sta_t *sta) {
    memset(out_client, 0, sizeof(*out_client));
    memcpy(out_client->mac, sta->addr, sizeof(out_client->mac));
    out_client->rssi = sta->rssi;
    /* CP's DHCP server inserts sta->ipaddr in its lookup table on DISCOVER,
     * before a real client accepts an OFFER. Only the paired successful ACK
     * signal may mark a PAL lease valid; never expose this provisional IP. */
}

static int h2_bk_wifi_get_ap_client_by_mac(
    const uint8_t mac[6],
    h2_pal_wifi_ap_client_t *out_client) {
    wlan_ap_stas_t stas;
    memset(&stas, 0, sizeof(stas));
    bk_err_t err = bk_wifi_ap_get_sta_list(&stas);
    if (err != BK_OK) {
        return h2_bk_wifi_map_error(err);
    }
    int rc = H2_PAL_ERR_NOT_FOUND;
    if (stas.sta != NULL) {
        for (int i = 0; i < stas.num; ++i) {
            if (memcmp(stas.sta[i].addr, mac, 6u) == 0) {
                h2_bk_wifi_copy_ap_client(out_client, &stas.sta[i]);
                rc = H2_PAL_OK;
                break;
            }
        }
    }
    if (stas.sta != NULL) {
        os_free(stas.sta);
    }
    return rc;
}

static h2_pal_wifi_sta_state_t h2_bk_wifi_state(wifi_link_state_t state) {
    switch (state) {
    case WIFI_LINKSTATE_STA_IDLE:
        return H2_PAL_WIFI_STA_STATE_IDLE;
    case WIFI_LINKSTATE_STA_CONNECTING:
        return H2_PAL_WIFI_STA_STATE_CONNECTING;
    case WIFI_LINKSTATE_STA_CONNECTED:
        return H2_PAL_WIFI_STA_STATE_CONNECTED;
    case WIFI_LINKSTATE_STA_GOT_IP:
        return H2_PAL_WIFI_STA_STATE_GOT_IP;
    case WIFI_LINKSTATE_STA_DISCONNECTED:
        return H2_PAL_WIFI_STA_STATE_DISCONNECTED;
    case WIFI_LINKSTATE_STA_CONNECT_FAILED:
        return H2_PAL_WIFI_STA_STATE_FAILED;
    case WIFI_LINKSTATE_STA_SCAN_DONE:
        return H2_PAL_WIFI_STA_STATE_SCANNING;
    default:
        return H2_PAL_WIFI_STA_STATE_UNKNOWN;
    }
}

static void h2_bk_wifi_fill_sta_ip(h2_pal_wifi_sta_status_t *status) {
    if (status == NULL) {
        return;
    }

    h2_pal_netif_ref_t ref = {.type = H2_PAL_NETIF_REF_KIND,
                              .kind = H2_PAL_NETIF_KIND_WIFI_STA};
    h2_pal_netif_status_t network = {0};
    if (h2_pal_netif_get_status(h2_bk_platform_netif_api(), &ref, &network) ==
            H2_PAL_OK &&
        (network.flags & H2_PAL_NETIF_FLAG_HAS_IPV6) &&
        h2_pal_net_ipv6_is_non_link_local_unicast(network.ipv6.ip)) {
      memcpy(status->ip.ip6, network.ipv6.ip, sizeof(status->ip.ip6));
      status->ip.ip6_valid = 1u;
    }
    netif_ip4_config_t ip4_config;
    memset(&ip4_config, 0, sizeof(ip4_config));
    if (bk_netif_get_ip4_config(NETIF_IF_STA, &ip4_config) != BK_OK) {
        return;
    }

    uint32_t ip4 = 0u;
    if (h2_bk_wifi_parse_ip4(ip4_config.ip, &ip4) == 0 || ip4 == 0u) {
        return;
    }

    status->ip.ip4 = ip4;
    (void)h2_bk_wifi_parse_ip4(ip4_config.mask, &status->ip.netmask4);
    (void)h2_bk_wifi_parse_ip4(ip4_config.gateway, &status->ip.gateway4);
    status->ip_valid = 1u;
}

typedef struct h2_bk_wifi_sta_up_call {
    struct tcpip_api_call_data call;
    struct netif *sta;
    uint32_t generation;
} h2_bk_wifi_sta_up_call_t;

static err_t h2_bk_wifi_sta_up_api_call(struct tcpip_api_call_data *data) {
    h2_bk_wifi_sta_up_call_t *request = (h2_bk_wifi_sta_up_call_t *)data;
    if (request->sta == NULL || ip4_addr_isany_val(*netif_ip4_addr(request->sta)) ||
        __atomic_load_n(&s_h2_bk_wifi_last_config_valid, __ATOMIC_ACQUIRE) == 0 ||
        __atomic_load_n(&s_h2_bk_wifi_connect_generation, __ATOMIC_ACQUIRE) !=
            request->generation)
        return ERR_IF;
    /* CP supplied the authenticated association and DHCP address. The AP
     * SDK synchronizes the address but never restores its local link flag. */
    netif_set_link_up(request->sta);
    netif_set_up(request->sta);
    return ERR_OK;
}

static bk_err_t h2_bk_wifi_system_event_handler(
    void *arg,
    event_module_t event_module,
    int event_id,
    void *event_data) {
    (void)arg;
    if (event_module == EVENT_MOD_NETIF) {
        const netif_event_got_ip4_t *netif_event = (const netif_event_got_ip4_t *)event_data;
        if (netif_event == NULL || netif_event->netif_if != NETIF_IF_STA) {
            return BK_OK;
        }

        if (event_id == EVENT_NETIF_GOT_IP4) {
            /* CP notifications can arrive after STA_STOP. Require a current
             * authenticated association and the actual local DHCP address;
             * an event alone cannot resurrect the stopped connection. */
            if (__atomic_load_n(&s_h2_bk_wifi_last_config_valid,
                                __ATOMIC_ACQUIRE) == 0) return BK_OK;
            uint32_t generation = __atomic_load_n(
                &s_h2_bk_wifi_connect_generation, __ATOMIC_ACQUIRE);
            wifi_link_status_t link;
            memset(&link, 0, sizeof(link));
            if (bk_wifi_sta_get_link_status(&link) != BK_OK ||
                (link.state != WIFI_LINKSTATE_STA_CONNECTED &&
                 link.state != WIFI_LINKSTATE_STA_GOT_IP)) return BK_OK;
            size_t len = h2_bk_wifi_strnlen(link.ssid, H2_PAL_WIFI_SSID_MAX);
            if (len == 0u || len != s_h2_bk_wifi_last_config.ssid_len ||
                memcmp(link.ssid, s_h2_bk_wifi_last_config.ssid, len) != 0)
                return BK_OK;
            h2_pal_wifi_sta_status_t status;
            memset(&status, 0, sizeof(status));
            h2_bk_wifi_fill_sta_ip(&status);
            if (status.ip_valid == 0u) return BK_OK;
            h2_bk_wifi_sta_up_call_t up = {
                .sta = (struct netif *)net_get_sta_handle(), .generation = generation};
            if (tcpip_api_call(h2_bk_wifi_sta_up_api_call, &up.call) != ERR_OK ||
                __atomic_load_n(&s_h2_bk_wifi_connect_generation, __ATOMIC_ACQUIRE) !=
                    generation) return BK_OK;
            if (__atomic_load_n(&s_h2_bk_wifi_power_save_set, __ATOMIC_ACQUIRE)) {
                int power_rc = h2_bk_wifi_apply_power_save(__atomic_load_n(
                    &s_h2_bk_wifi_power_save, __ATOMIC_ACQUIRE));
                __atomic_store_n(&s_h2_bk_wifi_power_save_error, power_rc, __ATOMIC_RELEASE);
            }
            if (h2_bk_wifi_ap_active()) {
                netif_ip4_config_t *cp_ap = os_malloc(sizeof(*cp_ap));
                if (cp_ap != NULL) {
                    memset(cp_ap, 0, sizeof(*cp_ap));
                    bk_err_t owner_rc = wifi_send_com_api_cmd(
                        AP_GET_NETIF_IP4_CONFIG, 1, (uint32_t)cp_ap);
                    uint32_t cp_ip = 0u;
                    if (owner_rc == BK_OK)
                        (void)h2_bk_wifi_parse_ip4(cp_ap->ip, &cp_ip);
                    BK_LOGI("h2_wifi", "H2_WIFI_BK_CP_AP ip=%lu rc=%d\r\n",
                            (unsigned long)cp_ip, (int)owner_rc);
                    os_free(cp_ap);
                }
            }
            status.state = H2_PAL_WIFI_STA_STATE_GOT_IP;
            status.ssid_len = len;
            memcpy(status.ssid, link.ssid, len);
            status.ssid[len] = '\0';
            memcpy(status.bssid, link.bssid, sizeof(status.bssid));
            status.bssid_set = 1u;
            status.channel = link.channel;
            status.rssi = link.rssi;
            if (h2_bk_wifi_event_lock() != H2_PAL_OK) return BK_FAIL;
            h2_bk_wifi_store_sta_status(&status);
            __atomic_store_n(&s_h2_bk_wifi_had_ip, 1u, __ATOMIC_RELEASE);
            h2_bk_wifi_post_sta_system_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_GOT_IP, &status);
            h2_bk_wifi_event_unlock();
            (void)h2_bk_platform_netif_reconcile_default_async();
            return BK_OK;
        }

        if (event_id == EVENT_NETIF_DHCP_TIMEOUT) {
            if (h2_bk_wifi_event_lock() != H2_PAL_OK) return BK_FAIL;
            h2_pal_wifi_sta_status_t status;
            memset(&status, 0, sizeof(status));
            (void)h2_bk_wifi_load_sta_status(&status);
            memset(&status.ip, 0, sizeof(status.ip));
            status.ip_valid = 0u;
            h2_bk_wifi_fill_sta_ip(&status);
            status.state = H2_PAL_WIFI_STA_STATE_GOT_IP;
            const int ready = h2_pal_wifi_sta_status_has_ip(&status);
            if (!ready)
              status.state = H2_PAL_WIFI_STA_STATE_CONNECTED;
            int had_ip = __atomic_exchange_n(&s_h2_bk_wifi_had_ip, ready,
                                             __ATOMIC_ACQ_REL);
            h2_bk_wifi_store_sta_status(&status);
            if (had_ip && !ready)
              h2_bk_wifi_post_sta_system_event(
                  H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_LOST_IP, &status);
            h2_bk_wifi_event_unlock();
            (void)h2_bk_platform_netif_reconcile_default_async();
            return BK_OK;
        }

        return BK_OK;
    }

    if (event_module != EVENT_MOD_WIFI) {
        return BK_OK;
    }

    if (event_id == EVENT_WIFI_STA_CONNECTED) {
        h2_pal_wifi_sta_status_t status;
        memset(&status, 0, sizeof(status));
        status.state = H2_PAL_WIFI_STA_STATE_CONNECTED;
        const wifi_event_sta_connected_t *connected = (const wifi_event_sta_connected_t *)event_data;
        if (connected != NULL) {
            status.ssid_len = h2_bk_wifi_strnlen(connected->ssid, H2_PAL_WIFI_SSID_MAX);
            memcpy(status.ssid, connected->ssid, status.ssid_len);
            status.ssid[status.ssid_len] = '\0';
        }
        h2_bk_wifi_store_sta_status(&status);
        h2_bk_wifi_post_sta_system_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTED, &status);
        return BK_OK;
    }

    if (event_id == EVENT_WIFI_STA_DISCONNECTED) {
        h2_bk_wifi_publish_disconnected(0);
        (void)h2_bk_platform_netif_reconcile_default_async();
        return BK_OK;
    }

    if (event_id == EVENT_WIFI_AP_CONNECTED) {
        const wifi_event_ap_connected_t *connected = (const wifi_event_ap_connected_t *)event_data;
        if (connected == NULL) return BK_FAIL;
        h2_pal_wifi_ap_client_t client;
        memset(&client, 0, sizeof(client));
        memcpy(client.mac, connected->mac, sizeof(client.mac));
        (void)h2_bk_wifi_get_ap_client_by_mac(connected->mac, &client);
        if (h2_bk_wifi_event_lock() != H2_PAL_OK) return BK_FAIL;
        if (!h2_bk_wifi_lease_join_admitted(
                h2_bk_wifi_ap_active(), h2_bk_wifi_ap_starting())) {
            h2_bk_wifi_event_unlock();
            return BK_OK;
        }
        h2_bk_wifi_accepted_lease_t *lease = h2_bk_wifi_lease_find_locked(client.mac, 1);
        if (lease != NULL && !lease->joined) {
            h2_bk_wifi_lease_note_join(lease, &client);
            if (!h2_bk_wifi_ap_starting() &&
                h2_bk_wifi_lease_flush_left_locked(lease) &&
                h2_bk_wifi_lease_drain_join(
                    lease, 1, h2_bk_wifi_post_join_event, NULL))
                h2_bk_wifi_lease_grant_locked(lease);
        }
        h2_bk_wifi_event_unlock();
        return BK_OK;
    }

    if (event_id == EVENT_WIFI_AP_DISCONNECTED) {
        const wifi_event_ap_disconnected_t *disconnected = (const wifi_event_ap_disconnected_t *)event_data;
        if (disconnected == NULL || h2_bk_wifi_event_lock() != H2_PAL_OK) return BK_FAIL;
        h2_bk_wifi_lease_left_locked(disconnected->mac);
        h2_bk_wifi_event_unlock();
        return BK_OK;
    }

    return BK_OK;
}

static int h2_bk_wifi_ensure_events_registered(void) {
    if (s_h2_bk_wifi_events_registered != 0) {
        return H2_PAL_OK;
    }

    if (s_h2_bk_wifi_event_mutex == NULL)
        s_h2_bk_wifi_event_mutex = (beken_mutex_t)xSemaphoreCreateMutexStatic(
            &s_h2_bk_wifi_event_mutex_control);
    if (s_h2_bk_wifi_event_mutex == NULL) return H2_PAL_ERR_NO_MEMORY;

    bk_err_t err = bk_event_register_cb(
        EVENT_MOD_WIFI,
        EVENT_WIFI_STA_CONNECTED,
        h2_bk_wifi_system_event_handler,
        NULL);
    if (err != BK_OK && err != BK_ERR_EVENT_CB_EXIST) {
        return h2_bk_wifi_map_error(err);
    }

    err = bk_event_register_cb(
        EVENT_MOD_WIFI,
        EVENT_WIFI_STA_DISCONNECTED,
        h2_bk_wifi_system_event_handler,
        NULL);
    if (err != BK_OK && err != BK_ERR_EVENT_CB_EXIST) {
        return h2_bk_wifi_map_error(err);
    }

    err = bk_event_register_cb(
        EVENT_MOD_WIFI,
        EVENT_WIFI_AP_CONNECTED,
        h2_bk_wifi_system_event_handler,
        NULL);
    if (err != BK_OK && err != BK_ERR_EVENT_CB_EXIST) {
        return h2_bk_wifi_map_error(err);
    }

    err = bk_event_register_cb(
        EVENT_MOD_WIFI,
        EVENT_WIFI_AP_DISCONNECTED,
        h2_bk_wifi_system_event_handler,
        NULL);
    if (err != BK_OK && err != BK_ERR_EVENT_CB_EXIST) {
        return h2_bk_wifi_map_error(err);
    }

    err = bk_event_register_cb(
        EVENT_MOD_NETIF,
        EVENT_NETIF_GOT_IP4,
        h2_bk_wifi_system_event_handler,
        NULL);
    if (err != BK_OK && err != BK_ERR_EVENT_CB_EXIST) {
        return h2_bk_wifi_map_error(err);
    }

    err = bk_event_register_cb(
        EVENT_MOD_NETIF,
        EVENT_NETIF_DHCP_TIMEOUT,
        h2_bk_wifi_system_event_handler,
        NULL);
    if (err != BK_OK && err != BK_ERR_EVENT_CB_EXIST) {
        return h2_bk_wifi_map_error(err);
    }

    if (s_h2_bk_wifi_ipv6_mutex == NULL)
      s_h2_bk_wifi_ipv6_mutex = (beken_mutex_t)xSemaphoreCreateMutexStatic(
          &s_h2_bk_wifi_ipv6_mutex_control);
    if (s_h2_bk_wifi_ipv6_mutex == NULL)
      return H2_PAL_ERR_NO_MEMORY;
#if LWIP_IPV6
    if (!s_h2_bk_wifi_ipv6_watch_started) {
      if (rtos_create_thread(NULL, BEKEN_APPLICATION_PRIORITY, "h2_wifi_ip6",
                             h2_bk_wifi_ipv6_watch, 4096u, NULL) != kNoErr)
        return H2_PAL_ERR_NO_MEMORY;
      s_h2_bk_wifi_ipv6_watch_started = 1;
    }
#endif
    bk_customer_event_register_callback(h2_bk_wifi_lease_wake_callback);
    s_h2_bk_wifi_events_registered = 1;
    return H2_PAL_OK;
}

static bk_err_t h2_bk_wifi_scan_done_handler(
    void *arg,
    event_module_t event_module,
    int event_id,
    void *event_data) {
    (void)arg;
    (void)event_module;
    (void)event_id;
    (void)event_data;

    if (s_h2_bk_wifi_scan_sem != NULL) {
        (void)rtos_set_semaphore(&s_h2_bk_wifi_scan_sem);
    }
    return BK_OK;
}

static void h2_bk_wifi_copy_scan_entry(
    h2_pal_wifi_scan_entry_t *out_entry,
    const wifi_scan_ap_info_t *ap) {
    memset(out_entry, 0, sizeof(*out_entry));
    out_entry->ssid_len = h2_bk_wifi_strnlen(ap->ssid, H2_PAL_WIFI_SSID_MAX);
    memcpy(out_entry->ssid, ap->ssid, out_entry->ssid_len);
    out_entry->ssid[out_entry->ssid_len] = '\0';
    memcpy(out_entry->bssid, ap->bssid, sizeof(out_entry->bssid));
    out_entry->channel = ap->channel;
    out_entry->rssi = ap->rssi;
    out_entry->security = h2_bk_wifi_security(ap->security);
}

static int h2_bk_wifi_sta_get_status(
    h2_pal_wifi_sta_t *sta,
    h2_pal_wifi_sta_status_t *out_status) {
    (void)sta;
    if (out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_status, 0, sizeof(*out_status));
    (void)h2_bk_wifi_ipv6_refresh();
    int power_rc = __atomic_load_n(&s_h2_bk_wifi_power_save_error, __ATOMIC_ACQUIRE);
    if (power_rc != H2_PAL_OK) return power_rc;
    if (__atomic_load_n(
            &s_h2_bk_wifi_connect_pending,
            __ATOMIC_ACQUIRE) != 0) {
        *out_status = s_h2_bk_wifi_connect_status;
        return H2_PAL_OK;
    }
    if (h2_bk_wifi_load_sta_status(out_status) != 0) {
      if (h2_pal_wifi_sta_status_has_ip(out_status) ||
          (out_status->state == H2_PAL_WIFI_STA_STATE_DISCONNECTED &&
           __atomic_load_n(&s_h2_bk_wifi_last_config_valid, __ATOMIC_ACQUIRE) ==
               0)) {
        return H2_PAL_OK;
      }
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->state = H2_PAL_WIFI_STA_STATE_IDLE;

    wifi_link_status_t link_status;
    memset(&link_status, 0, sizeof(link_status));
    bk_err_t err = bk_wifi_sta_get_link_status(&link_status);
    if (err == BK_ERR_WIFI_STA_NOT_STARTED || err == BK_ERR_WIFI_STA_NOT_CONFIG) {
        return H2_PAL_OK;
    }
    if (err != BK_OK) {
        return h2_bk_wifi_map_error(err);
    }

    out_status->state = h2_bk_wifi_state(link_status.state);
    out_status->ssid_len = h2_bk_wifi_strnlen(link_status.ssid, H2_PAL_WIFI_SSID_MAX);
    memcpy(out_status->ssid, link_status.ssid, out_status->ssid_len);
    out_status->ssid[out_status->ssid_len] = '\0';
    memcpy(out_status->bssid, link_status.bssid, sizeof(out_status->bssid));
    out_status->bssid_set = out_status->ssid_len > 0u ? 1u : 0u;
    out_status->channel = link_status.channel;
    out_status->rssi = link_status.rssi;
    h2_bk_wifi_fill_sta_ip(out_status);
    out_status->state = H2_PAL_WIFI_STA_STATE_GOT_IP;
    if (!h2_pal_wifi_sta_status_has_ip(out_status))
      out_status->state = h2_bk_wifi_state(link_status.state);
    h2_bk_wifi_store_sta_status(out_status);
    return H2_PAL_OK;
}

static int h2_bk_wifi_sta_scan(
    h2_pal_wifi_sta_t *sta,
    const h2_pal_wifi_scan_request_t *request,
    h2_pal_wifi_scan_result_fn on_result,
    void *user,
    uint32_t timeout_ms) {
    (void)sta;
    if (on_result == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    int register_rc = h2_bk_wifi_ensure_events_registered();
    if (register_rc != H2_PAL_OK) {
        return register_rc;
    }

    wifi_scan_config_t scan_config;
    memset(&scan_config, 0, sizeof(scan_config));
    if (request != NULL && request->ssid_len > 0u) {
        memcpy(scan_config.ssid, request->ssid, request->ssid_len);
        scan_config.ssid[request->ssid_len] = '\0';
    }
    if (request != NULL && request->channel != 0u) {
        scan_config.chan_cnt = 1u;
        scan_config.chan_nb[0] = request->channel;
    }

    bk_err_t err = rtos_init_semaphore(&s_h2_bk_wifi_scan_sem, 1);
    if (err != BK_OK) {
        return h2_bk_wifi_map_error(err);
    }

    err = bk_event_register_cb(EVENT_MOD_WIFI, EVENT_WIFI_SCAN_DONE, h2_bk_wifi_scan_done_handler, NULL);
    if (err != BK_OK && err != BK_ERR_EVENT_CB_EXIST) {
        rtos_deinit_semaphore(&s_h2_bk_wifi_scan_sem);
        return h2_bk_wifi_map_error(err);
    }

    err = bk_wifi_scan_start(&scan_config);
    if (err == BK_OK) {
        uint32_t wait_ms = timeout_ms != 0u ? timeout_ms : 10000u;
        err = rtos_get_semaphore(&s_h2_bk_wifi_scan_sem, wait_ms);
        if (err == BK_OK) {
            wifi_scan_result_t scan_result;
            memset(&scan_result, 0, sizeof(scan_result));
            err = bk_wifi_scan_get_result(&scan_result);
            if (err == BK_OK) {
                size_t copy_count = (size_t)scan_result.ap_num;
                for (size_t i = 0u; i < copy_count; ++i) {
                    h2_pal_wifi_scan_entry_t entry;
                    h2_bk_wifi_copy_scan_entry(&entry, &scan_result.aps[i]);
                    if (!on_result(user, &entry)) {
                        break;
                    }
                }
                bk_wifi_scan_free_result(&scan_result);
            }
        }
    }

    (void)bk_event_unregister_cb(EVENT_MOD_WIFI, EVENT_WIFI_SCAN_DONE, h2_bk_wifi_scan_done_handler);
    rtos_deinit_semaphore(&s_h2_bk_wifi_scan_sem);
    s_h2_bk_wifi_scan_sem = NULL;
    return h2_bk_wifi_map_error(err);
}

static void h2_bk_wifi_copy_sta_config(
    wifi_sta_config_t *out_config,
    const h2_pal_wifi_sta_config_t *config) {
    memset(out_config, 0, sizeof(*out_config));
    memcpy(out_config->ssid, config->ssid, config->ssid_len);
    out_config->ssid[config->ssid_len] = '\0';
    memcpy(out_config->password, config->password, config->password_len);
    out_config->password[config->password_len] = '\0';
    if (config->bssid_set != 0u) {
        memcpy(out_config->bssid, config->bssid, sizeof(out_config->bssid));
    }
    out_config->channel = config->channel;
    out_config->security = WIFI_SECURITY_AUTO;
    /* Saved credentials belong to wifi_settings; SDK fast-connect caching
     * must not persist a temporary connection behind that API. */
    out_config->is_not_support_auto_fci = 1u;
    out_config->auto_reconnect_count = 0;
    out_config->auto_reconnect_timeout = 0;
}

static int h2_bk_wifi_sta_is_connected(
    const wifi_link_status_t *link_status,
    const h2_pal_wifi_sta_config_t *config) {
    if (link_status->state != WIFI_LINKSTATE_STA_CONNECTED &&
        link_status->state != WIFI_LINKSTATE_STA_GOT_IP) {
        return 0;
    }
    size_t ssid_len =
        h2_bk_wifi_strnlen(link_status->ssid, H2_PAL_WIFI_SSID_MAX);
    return ssid_len == config->ssid_len &&
        memcmp(link_status->ssid, config->ssid, ssid_len) == 0;
}

static int
h2_bk_wifi_sta_cached_connected(const h2_pal_wifi_sta_config_t *config) {
  h2_pal_wifi_sta_status_t status = {0};
  return h2_bk_wifi_load_sta_status(&status) &&
         h2_pal_wifi_sta_status_has_ip(&status) &&
         status.ssid_len == config->ssid_len &&
         memcmp(status.ssid, config->ssid, config->ssid_len) == 0 &&
         (!config->bssid_set ||
          (status.bssid_set && memcmp(status.bssid, config->bssid, 6u) == 0));
}

typedef struct h2_bk_wifi_connect_request {
    h2_pal_wifi_sta_config_t requested_config;
    wifi_sta_config_t sdk_config;
    uint32_t generation;
} h2_bk_wifi_connect_request_t;

static void h2_bk_wifi_connect_worker(void *arg) {
    h2_bk_wifi_connect_request_t *request = arg;
    int has_request = request != NULL;
    uint32_t generation = has_request ? request->generation : 0u;
    if (has_request) {
        /* Starting BK Wi-Fi briefly stalls the AP/CP command transport. Give
         * the caller time to deliver the accepted response first. */
        rtos_delay_milliseconds(500u);
        for (uint32_t attempt = 0u; attempt < 3u; ++attempt) {
            if (__atomic_load_n(
                    &s_h2_bk_wifi_connect_generation,
                    __ATOMIC_ACQUIRE) != generation) {
                break;
            }
            if (h2_bk_wifi_sta_cached_connected(&request->requested_config))
              goto done;
            wifi_link_status_t current_status;
            memset(&current_status, 0, sizeof(current_status));
            bk_err_t err = bk_wifi_sta_get_link_status(&current_status);
            if (err == BK_OK && h2_bk_wifi_sta_is_connected(
                    &current_status, &request->requested_config)) {
                break;
            }
            int current_generation = 0;
            int started = 0;
            bk_err_t start_err = BK_FAIL;
            if (h2_bk_wifi_request_lock() == H2_PAL_OK) {
                current_generation = __atomic_load_n(
                    &s_h2_bk_wifi_connect_generation,
                    __ATOMIC_ACQUIRE) == generation;
                if (current_generation) {
                    int was_started = wifi_sta_is_started();
                    start_err = bk_wifi_sta_set_config(&request->sdk_config);
                    if (start_err == BK_OK) {
                        start_err = bk_wifi_sta_start();
                        started = start_err == BK_OK;
                    }
                    if (start_err == BK_OK) {
                        /* Keep authentication/DHCP awake; the generation-bound
                         * GOT_IP handler restores the requested steady policy. */
                        start_err = bk_wifi_sta_pm_disable();
                    }
                    /* Fresh CP STA_START already calls connect. Repeating it
                     * starts a second disconnect/authentication/DHCP sequence. */
                    if (start_err == BK_OK && was_started) {
                        start_err = wifi_send_com_api_cmd(H2_BK_WIFI_RPC_STA_ASSOCIATE, 0);
                        if (start_err == BK_OK) start_err = bk_wifi_sta_connect();
                    }
                }
                h2_bk_wifi_request_unlock();
            }
            if (!current_generation) {
                goto done;
            }
            if (start_err == BK_OK) {
                for (uint32_t elapsed = 0u; elapsed < 15000u; elapsed += 100u) {
                    if (__atomic_load_n(
                            &s_h2_bk_wifi_connect_generation,
                            __ATOMIC_ACQUIRE) != generation) {
                        goto done;
                    }
                    if (h2_bk_wifi_sta_cached_connected(
                            &request->requested_config))
                      goto done;
                    memset(&current_status, 0, sizeof(current_status));
                    if (bk_wifi_sta_get_link_status(&current_status) == BK_OK &&
                        h2_bk_wifi_sta_is_connected(
                            &current_status,
                            &request->requested_config)) {
                        goto done;
                    }
                    rtos_delay_milliseconds(100u);
                }
            }
            if (started) {
                if (h2_bk_wifi_request_lock() != H2_PAL_OK)
                    goto done;
                current_generation = __atomic_load_n(
                    &s_h2_bk_wifi_connect_generation,
                    __ATOMIC_ACQUIRE) == generation;
                if (current_generation)
                    (void)bk_wifi_sta_stop();
                h2_bk_wifi_request_unlock();
                if (!current_generation)
                    goto done;
            }
            rtos_delay_milliseconds(500u);
        }
done:
        os_free(request);
    }
    if (has_request && h2_bk_wifi_request_lock() == H2_PAL_OK) {
        if (__atomic_load_n(
                &s_h2_bk_wifi_connect_generation,
                __ATOMIC_ACQUIRE) == generation) {
            __atomic_store_n(
                &s_h2_bk_wifi_connect_pending,
                0,
                __ATOMIC_RELEASE);
        }
        h2_bk_wifi_request_unlock();
    }
    rtos_delete_thread(NULL);
}

static int h2_bk_wifi_sta_connect(
    h2_pal_wifi_sta_t *sta,
    const h2_pal_wifi_sta_config_t *config,
    uint32_t timeout_ms) {
    (void)sta;
    int rc = h2_pal_wifi_settings_validate_sta_config(config);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    rc = h2_bk_wifi_ensure_events_registered();
    if (rc != H2_PAL_OK) {
        return rc;
    }

    rc = h2_bk_wifi_request_lock();
    if (rc != H2_PAL_OK) {
        return rc;
    }

    if (__atomic_load_n(
            &s_h2_bk_wifi_connect_pending,
            __ATOMIC_ACQUIRE) != 0) {
        rc = timeout_ms == 0u &&
            s_h2_bk_wifi_connect_status.ssid_len == config->ssid_len &&
            memcmp(
                s_h2_bk_wifi_connect_status.ssid,
                config->ssid,
                config->ssid_len) == 0
            ? H2_PAL_OK
            : H2_PAL_ERR_BUSY;
        h2_bk_wifi_request_unlock();
        return rc;
    }

    h2_pal_wifi_sta_status_t cached_status;
    memset(&cached_status, 0, sizeof(cached_status));
    if (__atomic_load_n(
            &s_h2_bk_wifi_last_config_valid,
            __ATOMIC_ACQUIRE) != 0 &&
        s_h2_bk_wifi_last_config.ssid_len == config->ssid_len &&
        memcmp(
            s_h2_bk_wifi_last_config.ssid,
            config->ssid,
            config->ssid_len) == 0) {
      if (h2_bk_wifi_sta_get_status(NULL, &cached_status) == H2_PAL_OK &&
          cached_status.state == H2_PAL_WIFI_STA_STATE_GOT_IP &&
          h2_pal_wifi_sta_status_has_ip(&cached_status) &&
          cached_status.ssid_len == config->ssid_len &&
          memcmp(cached_status.ssid, config->ssid, config->ssid_len) == 0) {
        h2_bk_wifi_request_unlock();
        return H2_PAL_OK;
      }
    }
    if (h2_bk_wifi_load_sta_status(&cached_status) != 0 &&
        cached_status.state == H2_PAL_WIFI_STA_STATE_GOT_IP &&
        h2_pal_wifi_sta_status_has_ip(&cached_status) &&
        cached_status.ssid_len == config->ssid_len &&
        memcmp(cached_status.ssid, config->ssid, config->ssid_len) == 0) {
      h2_bk_wifi_request_unlock();
      return H2_PAL_OK;
    }

    (void)__atomic_add_fetch(&s_h2_bk_wifi_connect_generation, 1u,
                             __ATOMIC_ACQ_REL);
    wifi_sta_config_t bk_config;
    h2_bk_wifi_copy_sta_config(&bk_config, config);
    s_h2_bk_wifi_last_config = *config;
    __atomic_store_n(
        &s_h2_bk_wifi_last_config_valid,
        1,
        __ATOMIC_RELEASE);
    bk_err_t err = BK_OK;
    if (timeout_ms == 0u) {
        memset(
            &s_h2_bk_wifi_connect_status,
            0,
            sizeof(s_h2_bk_wifi_connect_status));
        s_h2_bk_wifi_connect_status.state =
            H2_PAL_WIFI_STA_STATE_CONNECTING;
        s_h2_bk_wifi_connect_status.ssid_len = config->ssid_len;
        memcpy(
            s_h2_bk_wifi_connect_status.ssid,
            config->ssid,
            config->ssid_len);
        s_h2_bk_wifi_connect_status.ssid[config->ssid_len] = '\0';
        __atomic_store_n(
            &s_h2_bk_wifi_connect_pending,
            1,
            __ATOMIC_RELEASE);
        h2_bk_wifi_connect_request_t *request =
            os_malloc(sizeof(*request));
        if (request == NULL) {
            __atomic_store_n(
                &s_h2_bk_wifi_connect_pending,
                0,
                __ATOMIC_RELEASE);
            h2_bk_wifi_request_unlock();
            return H2_PAL_ERR_NO_MEMORY;
        }
        request->requested_config = *config;
        request->sdk_config = bk_config;
        request->generation =
            __atomic_load_n(&s_h2_bk_wifi_connect_generation, __ATOMIC_ACQUIRE);
        if (rtos_create_thread(
                NULL,
                BEKEN_APPLICATION_PRIORITY,
                "h2_wifi_conn",
                h2_bk_wifi_connect_worker,
                4096u,
                request) != kNoErr) {
            os_free(request);
            __atomic_store_n(
                &s_h2_bk_wifi_connect_pending,
                0,
                __ATOMIC_RELEASE);
            h2_bk_wifi_request_unlock();
            return H2_PAL_ERR_NO_MEMORY;
        }
        h2_bk_wifi_request_unlock();
    } else {
        h2_bk_wifi_request_unlock();
        wifi_link_status_t current_status;
        memset(&current_status, 0, sizeof(current_status));
        err = bk_wifi_sta_get_link_status(&current_status);
        if (err == BK_OK &&
            h2_bk_wifi_sta_is_connected(&current_status, config)) {
            return H2_PAL_OK;
        }
        if (err != BK_OK &&
            err != BK_ERR_WIFI_STA_NOT_STARTED &&
            err != BK_ERR_WIFI_STA_NOT_CONFIG &&
            !((err == BK_FAIL || err == BK_ERR_WIFI_DRIVER) &&
              cached_status.state == H2_PAL_WIFI_STA_STATE_DISCONNECTED)) {
            return h2_bk_wifi_map_error(err);
        }
        if (h2_bk_wifi_ap_active()) {
            netif_ip4_config_t *cp_ap = os_malloc(sizeof(*cp_ap));
            if (cp_ap != NULL) {
                memset(cp_ap, 0, sizeof(*cp_ap));
                bk_err_t owner_rc = wifi_send_com_api_cmd(AP_GET_NETIF_IP4_CONFIG, 1, (uint32_t)cp_ap);
                uint32_t ip = 0u;
                if (owner_rc == BK_OK) (void)h2_bk_wifi_parse_ip4(cp_ap->ip, &ip);
                BK_LOGI("h2_wifi", "H2_WIFI_BK_CP_AP_BEFORE ip=%lu rc=%d\r\n",
                        (unsigned long)ip, (int)owner_rc);
                os_free(cp_ap);
            }
        }
        int was_started = wifi_sta_is_started();
        err = bk_wifi_sta_set_config(&bk_config);
        if (err != BK_OK) {
            return h2_bk_wifi_map_error(err);
        }

        err = bk_wifi_sta_start();
        if (err != BK_OK) {
            return h2_bk_wifi_map_error(err);
        }
        /* STA_STOP can retain CP power-save state across VIF creation. DHCP
         * must complete awake before reapplying the current/future policy. */
        err = bk_wifi_sta_pm_disable();
        if (err != BK_OK) return h2_bk_wifi_map_error(err);
        /* Fresh SDK STA_START auto-connects on CP. Explicit CONNECT is only
         * needed when START was an already-started no-op. */
        if (was_started) {
            /* AP's public CONNECT only changes a local flag. The paired CP
             * request actually starts fresh authentication and DHCP. */
            err = wifi_send_com_api_cmd(H2_BK_WIFI_RPC_STA_ASSOCIATE, 0);
            if (err == BK_OK) err = bk_wifi_sta_connect();
        }
        if (err != BK_OK) {
            return h2_bk_wifi_map_error(err);
        }
    }
    h2_pal_wifi_sta_status_t connecting_status;
    memset(&connecting_status, 0, sizeof(connecting_status));
    connecting_status.state = H2_PAL_WIFI_STA_STATE_CONNECTING;
    connecting_status.ssid_len = config->ssid_len;
    memcpy(connecting_status.ssid, config->ssid, config->ssid_len);
    connecting_status.ssid[connecting_status.ssid_len] = '\0';
    h2_bk_wifi_post_sta_system_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTING, &connecting_status);
    if (timeout_ms == 0u) {
        return H2_PAL_OK;
    }

    uint32_t elapsed = 0u;
    while (elapsed < timeout_ms) {
      if (h2_bk_wifi_sta_cached_connected(config))
        return H2_PAL_OK;
      wifi_link_status_t link_status;
      memset(&link_status, 0, sizeof(link_status));
      err = bk_wifi_sta_get_link_status(&link_status);
      if (err == BK_OK && h2_bk_wifi_sta_is_connected(&link_status, config)) {
        return H2_PAL_OK;
      }
        if (err != BK_OK &&
            err != BK_ERR_WIFI_STA_NOT_STARTED &&
            err != BK_ERR_WIFI_STA_NOT_CONFIG && err != BK_FAIL && err != BK_ERR_WIFI_DRIVER) {
            return h2_bk_wifi_map_error(err);
        }
        rtos_delay_milliseconds(100u);
        elapsed += 100u;
    }
    return H2_PAL_ERR_TIMEOUT;
}

typedef struct h2_bk_wifi_sta_down_call {
    struct tcpip_api_call_data call;
    struct netif *sta;
} h2_bk_wifi_sta_down_call_t;

static err_t h2_bk_wifi_sta_down_api_call(struct tcpip_api_call_data *data) {
    h2_bk_wifi_sta_down_call_t *request = (h2_bk_wifi_sta_down_call_t *)data;
    if (request->sta == NULL) return ERR_IF;
#if LWIP_IPV6
    h2_bk_wifi_ipv6_clear(request->sta);
#endif
    ip4_addr_t zero;
    ip4_addr_set_zero(&zero);
    netif_set_link_down(request->sta);
    netif_set_down(request->sta);
    netif_set_addr(request->sta, &zero, &zero, &zero);
    if (netif_default == request->sta) netif_set_default(NULL);
    return ERR_OK;
}

#if LWIP_IPV6
typedef struct h2_bk_wifi_ipv6_sync_call {
  struct tcpip_api_call_data call;
  const h2_bk_wifi_ipv6_snapshot_t *snapshot;
  uint32_t generation;
  h2_pal_wifi_sta_status_t *status;
} h2_bk_wifi_ipv6_sync_call_t;

static err_t h2_bk_wifi_ipv6_sync(struct tcpip_api_call_data *raw) {
  h2_bk_wifi_ipv6_sync_call_t *request = (h2_bk_wifi_ipv6_sync_call_t *)raw;
  if (!__atomic_load_n(&s_h2_bk_wifi_last_config_valid, __ATOMIC_ACQUIRE) ||
      request->generation !=
          __atomic_load_n(&s_h2_bk_wifi_connect_generation, __ATOMIC_ACQUIRE))
    return ERR_IF;
  struct netif *sta = (struct netif *)net_get_sta_handle();
  if (sta == NULL)
    return ERR_IF;
  return h2_bk_wifi_ipv6_install(sta, request->snapshot, request->status,
                                 &s_h2_bk_wifi_cp_generation);
}
#endif

static int h2_bk_wifi_ipv6_refresh(void) {
#if LWIP_IPV6
  if (s_h2_bk_wifi_ipv6_mutex == NULL ||
      rtos_lock_mutex(&s_h2_bk_wifi_ipv6_mutex) != kNoErr)
    return H2_PAL_ERR_INVALID_STATE;
  h2_pal_wifi_sta_config_t config = {0};
  uint32_t generation = 0u;
  int rc = h2_bk_wifi_request_lock();
  if (rc != H2_PAL_OK)
    goto done;
  if (!__atomic_load_n(&s_h2_bk_wifi_last_config_valid, __ATOMIC_ACQUIRE)) {
    h2_bk_wifi_request_unlock();
    goto done;
  }
  config = s_h2_bk_wifi_last_config;
  generation =
      __atomic_load_n(&s_h2_bk_wifi_connect_generation, __ATOMIC_ACQUIRE);
  h2_bk_wifi_request_unlock();
  h2_bk_wifi_ipv6_snapshot_t *shared = os_malloc(sizeof(*shared));
  if (shared == NULL) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  memset(shared, 0, sizeof(*shared));
  shared->size = sizeof(*shared);
  bk_err_t sdk = wifi_send_com_api_cmd(H2_BK_WIFI_RPC_IPV6_SNAPSHOT, 1,
                                       (uint32_t)(uintptr_t)shared);
  h2_bk_wifi_ipv6_snapshot_t snapshot = *shared;
  os_free(shared);
  if (sdk != BK_OK) {
    rc = h2_bk_wifi_map_error(sdk);
    goto done;
  }
  if (!h2_bk_wifi_ipv6_snapshot_matches(&snapshot, &config)) {
    rc = H2_PAL_ERR_INVALID_STATE;
    goto done;
  }
  h2_pal_wifi_sta_status_t status = {0};
  status.state = snapshot.connected ? H2_PAL_WIFI_STA_STATE_CONNECTED
                                    : H2_PAL_WIFI_STA_STATE_DISCONNECTED;
  status.ssid_len = snapshot.ssid_len;
  memcpy(status.ssid, snapshot.ssid, status.ssid_len);
  memcpy(status.bssid, snapshot.bssid, sizeof(status.bssid));
  status.bssid_set = snapshot.connected;
  rc = h2_bk_wifi_request_lock();
  if (rc != H2_PAL_OK)
    goto done;
  h2_bk_wifi_ipv6_sync_call_t sync = {
      .snapshot = &snapshot, .generation = generation, .status = &status};
  const err_t applied = tcpip_api_call(h2_bk_wifi_ipv6_sync, &sync.call);
  h2_bk_wifi_request_unlock();
  if (applied != ERR_OK) {
    rc = H2_PAL_ERR_UNAVAILABLE;
    goto done;
  }
  h2_pal_wifi_sta_status_t previous = {0};
  if (h2_bk_wifi_event_lock() != H2_PAL_OK) {
    rc = H2_PAL_ERR_INVALID_STATE;
    goto done;
  }
  if (generation !=
      __atomic_load_n(&s_h2_bk_wifi_connect_generation, __ATOMIC_ACQUIRE)) {
    h2_bk_wifi_event_unlock();
    rc = H2_PAL_ERR_UNAVAILABLE;
    goto done;
  }
  (void)h2_bk_wifi_load_sta_status(&previous);
  status.channel = (uint8_t)snapshot.channel;
  status.rssi = snapshot.rssi;
  status.state = snapshot.connected ? H2_PAL_WIFI_STA_STATE_GOT_IP
                                    : H2_PAL_WIFI_STA_STATE_DISCONNECTED;
  const int ready = h2_pal_wifi_sta_status_has_ip(&status);
  if (!ready && snapshot.connected)
    status.state = H2_PAL_WIFI_STA_STATE_CONNECTED;
  const int changed = status.ip_valid != previous.ip_valid ||
                      memcmp(&status.ip, &previous.ip, sizeof(status.ip)) != 0;
  int had_ip =
      __atomic_exchange_n(&s_h2_bk_wifi_had_ip, ready, __ATOMIC_ACQ_REL);
  h2_bk_wifi_store_sta_status(&status);
  if (changed && ready)
    h2_bk_wifi_post_sta_system_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_GOT_IP,
                                     &status);
  else if (had_ip && !ready)
    h2_bk_wifi_post_sta_system_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_LOST_IP,
                                     &status);
  h2_bk_wifi_event_unlock();
  (void)h2_bk_platform_netif_reconcile_default_async();
  rc = H2_PAL_OK;
done:
  memset(&config, 0, sizeof(config));
  (void)rtos_unlock_mutex(&s_h2_bk_wifi_ipv6_mutex);
  return rc;
#else
  return H2_PAL_ERR_UNSUPPORTED;
#endif
}

#if LWIP_IPV6
static void h2_bk_wifi_ipv6_watch(void *user) {
  (void)user;
  /* Provider/event registrations are image-lifetime singletons. This worker
   * is shared by all Runtime consumers and never owns saved credentials. */
  for (;;) {
    (void)h2_bk_wifi_ipv6_refresh();
    rtos_delay_milliseconds(500u);
  }
}

#endif

static int h2_bk_wifi_sta_disconnect(h2_pal_wifi_sta_t *sta) {
    (void)sta;
    int lock_rc = h2_bk_wifi_request_lock();
    if (lock_rc != H2_PAL_OK) {
        return lock_rc;
    }
    (void)__atomic_add_fetch(
        &s_h2_bk_wifi_connect_generation,
        1u,
        __ATOMIC_ACQ_REL);
    __atomic_store_n(
        &s_h2_bk_wifi_connect_pending,
        0,
        __ATOMIC_RELEASE);
    /* A fresh authenticated attempt cannot reuse last SSID/IP evidence. */
    __atomic_store_n(&s_h2_bk_wifi_last_config_valid, 0, __ATOMIC_RELEASE);
    h2_bk_wifi_request_unlock();
    /* AP SDK public disconnect only clears its local adapter. The paired
     * private RPC invokes the actual CP disassociate without deleting its
     * service/VIF. An unpaired/old CP must fail closed, never fake a disconnect. */
    bk_err_t err = BK_OK;
    if (wifi_sta_is_started()) {
        err = wifi_send_com_api_cmd(H2_BK_WIFI_RPC_STA_DISASSOCIATE, 0);
        if (err == BK_OK) err = bk_wifi_sta_disconnect();
    } else err = bk_wifi_sta_stop();
    int rc = err == BK_ERR_WIFI_STA_NOT_STARTED || err == BK_ERR_WIFI_STA_NOT_CONFIG
        ? H2_PAL_OK : h2_bk_wifi_map_error(err);
    if (rc == H2_PAL_OK) {
        /* Recover an adapter removed by SDK teardown/retry before returning
         * the actual down/addressless state. Normal disassociate retains it. */
        h2_pal_netif_ref_t ref;
        const h2_pal_netif_filter_t filter = {.kind = H2_PAL_NETIF_KIND_WIFI_STA};
        int netif_rc = h2_pal_netif_find(h2_bk_platform_netif_api(), &filter, &ref);
        if (netif_rc == H2_PAL_ERR_NOT_FOUND) {
            uint8_t mac[6];
            bk_err_t mac_rc = bk_wifi_sta_get_mac(mac);
            if (mac_rc != BK_OK) return h2_bk_wifi_map_error(mac_rc);
            if (host_wlan_add_netif(mac) != 0) return H2_PAL_ERR_IO;
        } else if (netif_rc != H2_PAL_OK) return netif_rc;
        /* SDK low_level_init marks a newly added adapter LINK_UP by default.
         * Clear real lwIP link/address/route state on its owning TCP/IP core. */
        h2_bk_wifi_sta_down_call_t down = {
            .sta = (struct netif *)net_get_sta_handle()};
        if (tcpip_api_call(h2_bk_wifi_sta_down_api_call, &down.call) != ERR_OK)
            return H2_PAL_ERR_IO;
        (void)h2_bk_platform_netif_reconcile_default();
        h2_bk_wifi_publish_disconnected(0);
    }
    return rc;
}

static int h2_bk_wifi_sta_get_mac(h2_pal_wifi_sta_t *sta, uint8_t out_mac[6]) {
    (void)sta;
    if (out_mac == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return h2_bk_wifi_map_error(bk_wifi_sta_get_mac(out_mac));
}

static int h2_bk_wifi_sta_set_power_save(
    h2_pal_wifi_sta_t *sta,
    h2_pal_wifi_power_save_t mode) {
    (void)sta;
    if (mode != H2_PAL_WIFI_POWER_SAVE_NONE &&
        mode != H2_PAL_WIFI_POWER_SAVE_MIN_MODEM &&
        mode != H2_PAL_WIFI_POWER_SAVE_MAX_MODEM) return H2_PAL_ERR_INVALID_ARG;
    wifi_link_status_t link;
    memset(&link, 0, sizeof(link));
    bk_err_t err = bk_wifi_sta_get_link_status(&link);
    if (err == BK_OK && (link.state == WIFI_LINKSTATE_STA_CONNECTED ||
                        link.state == WIFI_LINKSTATE_STA_GOT_IP)) {
        int rc = h2_bk_wifi_apply_power_save(mode);
        if (rc != H2_PAL_OK) return rc;
    } else if (err != BK_OK && err != BK_FAIL && err != BK_ERR_WIFI_DRIVER &&
               err != BK_ERR_WIFI_STA_NOT_STARTED && err != BK_ERR_WIFI_STA_NOT_CONFIG)
        return h2_bk_wifi_map_error(err);
    /* An absent STA has no firmware VIF yet. Apply the saved requested policy
     * on its next authenticated DHCP association and propagate apply errors. */
    __atomic_store_n(&s_h2_bk_wifi_power_save, mode, __ATOMIC_RELEASE);
    __atomic_store_n(&s_h2_bk_wifi_power_save_set, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&s_h2_bk_wifi_power_save_error, H2_PAL_OK, __ATOMIC_RELEASE);
    return H2_PAL_OK;
}

static void h2_bk_wifi_copy_ap_config(
    wifi_ap_config_t *out_config,
    const h2_pal_wifi_ap_config_t *config) {
    memset(out_config, 0, sizeof(*out_config));
    memcpy(out_config->ssid, config->ssid, config->ssid_len);
    memcpy(out_config->password, config->password, config->password_len);
    out_config->channel = config->channel != 0u ? config->channel : 1u;
    out_config->security = h2_bk_wifi_security_to_sdk(config->security);
    out_config->hidden = config->hidden != 0u ? 1u : 0u;
    out_config->max_con = config->max_clients != 0u ? config->max_clients : H2_PAL_WIFI_AP_MAX_CLIENTS;
    if (config->security == H2_PAL_WIFI_SECURITY_OPEN) {
        out_config->security = WIFI_SECURITY_NONE;
        out_config->password[0] = '\0';
    }
}

static void h2_bk_wifi_set_ap_status_from_config(const h2_pal_wifi_ap_config_t *config) {
    memset(&s_h2_bk_wifi_ap_status, 0, sizeof(s_h2_bk_wifi_ap_status));
    s_h2_bk_wifi_ap_status.state = H2_PAL_WIFI_AP_STATE_STARTED;
    s_h2_bk_wifi_ap_status.ssid_len = config->ssid_len;
    memcpy(s_h2_bk_wifi_ap_status.ssid, config->ssid, config->ssid_len);
    s_h2_bk_wifi_ap_status.ssid[s_h2_bk_wifi_ap_status.ssid_len] = '\0';
    s_h2_bk_wifi_ap_status.channel = config->channel != 0u ? config->channel : 1u;
    s_h2_bk_wifi_ap_status.max_clients = config->max_clients != 0u ? config->max_clients : H2_PAL_WIFI_AP_MAX_CLIENTS;
    s_h2_bk_wifi_ap_status.security = config->security;
    s_h2_bk_wifi_ap_status.hidden = config->hidden;
}

static int h2_bk_wifi_ap_config_same(const h2_pal_wifi_ap_config_t *config) {
    return h2_bk_wifi_ap_active() != 0 &&
        s_h2_bk_wifi_ap_config.ssid_len == config->ssid_len &&
        s_h2_bk_wifi_ap_config.password_len == config->password_len &&
        s_h2_bk_wifi_ap_config.channel == config->channel &&
        s_h2_bk_wifi_ap_config.max_clients == config->max_clients &&
        s_h2_bk_wifi_ap_config.security == config->security &&
        s_h2_bk_wifi_ap_config.hidden == config->hidden &&
        memcmp(s_h2_bk_wifi_ap_config.ssid, config->ssid, config->ssid_len) == 0 &&
        memcmp(s_h2_bk_wifi_ap_config.password, config->password, config->password_len) == 0;
}

static int h2_bk_wifi_ap_start(
    h2_pal_wifi_ap_t *ap,
    const h2_pal_wifi_ap_config_t *config,
    uint32_t timeout_ms) {
    (void)ap;
    (void)timeout_ms;
    int rc = h2_pal_wifi_ap_config_validate(config);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (h2_bk_wifi_ap_starting()) return H2_PAL_ERR_INVALID_STATE;
    if (h2_bk_wifi_ap_config_same(config)) {
        return H2_PAL_OK;
    }

    rc = h2_bk_wifi_ensure_events_registered();
    if (rc != H2_PAL_OK) {
        return rc;
    }

    if (h2_bk_wifi_ap_active() != 0) {
        rc = h2_bk_wifi_lease_watch_stop();
        if (rc != H2_PAL_OK) return rc;
        bk_err_t stop_err = bk_wifi_ap_stop();
        if (stop_err != BK_OK && stop_err != BK_ERR_WIFI_AP_NOT_STARTED && stop_err != BK_ERR_WIFI_AP_NOT_CONFIG) {
            (void)h2_bk_wifi_lease_watch_start();
            return h2_bk_wifi_map_error(stop_err);
        }
        rc = h2_bk_wifi_lease_stop();
        if (rc != H2_PAL_OK) return rc;
        s_h2_bk_wifi_ap_status.state = H2_PAL_WIFI_AP_STATE_STOPPED;
        s_h2_bk_wifi_ap_status.client_count = 0u;
        rc = h2_bk_wifi_post_ap_system_event(
            H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_STOPPED, &s_h2_bk_wifi_ap_status);
        if (rc != H2_PAL_OK) return rc;
    }

    wifi_ap_config_t bk_config;
    h2_bk_wifi_copy_ap_config(&bk_config, config);
    /* AP's local lwIP and CP's real DHCP server otherwise start from different
     * SDK defaults (192.168.188.1 versus 192.168.4.1). Configure both owners
     * through the SDK IPC-aware Netif API before starting the AP. Keep the
     * AP subnet separate from the upstream STA subnet used by our fixture. */
    const netif_ip4_config_t ip4 = {
        .ip = "192.168.188.1", .mask = "255.255.255.0",
        .gateway = "192.168.188.1", .dns = "192.168.188.1"};
    bk_err_t err = bk_netif_set_ip4_config(NETIF_IF_AP, &ip4);
    if (err != BK_OK) return h2_bk_wifi_map_error(err);
    err = bk_wifi_ap_set_config(&bk_config);
    if (err != BK_OK) {
        return h2_bk_wifi_map_error(err);
    }
    /* SDK can report L2 JOIN inside bk_wifi_ap_start(), before we obtain the
     * new CP generation or publish AP_STARTED. Admit it as pending only. */
    __atomic_store_n(&s_h2_bk_wifi_ap_starting, 1, __ATOMIC_RELEASE);
    err = bk_wifi_ap_start();
    if (err != BK_OK) {
        int release_rc = h2_bk_wifi_lease_stop();
        return release_rc == H2_PAL_OK ? h2_bk_wifi_map_error(err) : release_rc;
    }

    h2_bk_wifi_lease_snapshot_t lease_snapshot;
    rc = h2_bk_wifi_lease_query(&lease_snapshot);
    if (rc != H2_PAL_OK) {
        bk_err_t stop_err = bk_wifi_ap_stop();
        if (stop_err == BK_OK) {
            int release_rc = h2_bk_wifi_lease_stop();
            if (release_rc != H2_PAL_OK) return release_rc;
        }
        return stop_err == BK_OK ? rc : h2_bk_wifi_map_error(stop_err);
    }
    if (h2_bk_wifi_event_lock() != H2_PAL_OK) {
        bk_err_t stop_err = bk_wifi_ap_stop();
        if (stop_err == BK_OK) (void)h2_bk_wifi_lease_stop();
        return stop_err == BK_OK ? H2_PAL_ERR_INVALID_STATE
                                 : h2_bk_wifi_map_error(stop_err);
    }
    s_h2_bk_wifi_lease_generation = lease_snapshot.generation;
    s_h2_bk_wifi_lease_wake_hint = 0u;
    s_h2_bk_wifi_lease_error = H2_PAL_OK;
    __atomic_store_n(&s_h2_bk_wifi_ap_active, 1, __ATOMIC_RELEASE);
    h2_bk_wifi_event_unlock();

    s_h2_bk_wifi_ap_config = *config;
    h2_bk_wifi_set_ap_status_from_config(config);
    rc = h2_bk_wifi_post_ap_system_event(
        H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_STARTED, &s_h2_bk_wifi_ap_status);
    if (rc != H2_PAL_OK) {
        bk_err_t stop_err = bk_wifi_ap_stop();
        if (stop_err == BK_OK) (void)h2_bk_wifi_lease_stop();
        return stop_err == BK_OK ? rc : h2_bk_wifi_map_error(stop_err);
    }
    /* AP_STARTED is now in Runtime's queue. Only from here may the event
     * callback or watch worker deliver an early pending JOIN and its ACK. */
    __atomic_store_n(&s_h2_bk_wifi_ap_starting, 0, __ATOMIC_RELEASE);
    rc = h2_bk_wifi_lease_watch_start();
    if (rc != H2_PAL_OK) {
        bk_err_t stop_err = bk_wifi_ap_stop();
        if (stop_err == BK_OK) {
            int release_rc = h2_bk_wifi_lease_stop();
            if (release_rc != H2_PAL_OK) return release_rc;
            s_h2_bk_wifi_ap_status.state = H2_PAL_WIFI_AP_STATE_STOPPED;
            s_h2_bk_wifi_ap_status.client_count = 0u;
            (void)h2_bk_wifi_post_ap_system_event(
                H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_STOPPED, &s_h2_bk_wifi_ap_status);
        }
        return stop_err == BK_OK ? rc : h2_bk_wifi_map_error(stop_err);
    }
    return H2_PAL_OK;
}

static int h2_bk_wifi_ap_stop(h2_pal_wifi_ap_t *ap, uint32_t timeout_ms) {
    (void)ap;
    (void)timeout_ms;
    int was_active = h2_bk_wifi_ap_active();
    int watch_rc = h2_bk_wifi_lease_watch_stop();
    if (watch_rc != H2_PAL_OK) return watch_rc;
    bk_err_t err = bk_wifi_ap_stop();
    if (err == BK_ERR_WIFI_AP_NOT_STARTED || err == BK_ERR_WIFI_AP_NOT_CONFIG) {
        err = BK_OK;
    }
    int rc = h2_bk_wifi_map_error(err);
    if (rc != H2_PAL_OK) {
        if (was_active) (void)h2_bk_wifi_lease_watch_start();
        return rc;
    }
    if (rc == H2_PAL_OK) {
        int release_rc = h2_bk_wifi_lease_stop();
        if (release_rc != H2_PAL_OK) return release_rc;
        s_h2_bk_wifi_ap_status.state = H2_PAL_WIFI_AP_STATE_STOPPED;
        s_h2_bk_wifi_ap_status.client_count = 0u;
        if (was_active != 0) {
            rc = h2_bk_wifi_post_ap_system_event(
                H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_STOPPED, &s_h2_bk_wifi_ap_status);
        }
    }
    return rc;
}

static int h2_bk_wifi_ap_get_status(
    h2_pal_wifi_ap_t *ap,
    h2_pal_wifi_ap_status_t *out_status) {
    (void)ap;
    if (out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_status, 0, sizeof(*out_status));
    if (h2_bk_wifi_ap_active() == 0) {
        out_status->state = H2_PAL_WIFI_AP_STATE_STOPPED;
        return H2_PAL_OK;
    }
    int lease_rc = h2_bk_wifi_lease_reconcile();
    if (lease_rc != H2_PAL_OK) return lease_rc;
    *out_status = s_h2_bk_wifi_ap_status;
    wlan_ap_stas_t stas;
    memset(&stas, 0, sizeof(stas));
    bk_err_t err = bk_wifi_ap_get_sta_list(&stas);
    if (err == BK_OK) {
        out_status->client_count = stas.num > 0 ? (size_t)stas.num : 0u;
        if (stas.sta != NULL) {
            os_free(stas.sta);
        }
    }
    return H2_PAL_OK;
}

static int h2_bk_wifi_ap_get_clients(
    h2_pal_wifi_ap_t *ap,
    h2_pal_wifi_ap_client_t *out_clients,
    size_t max_clients,
    size_t *out_count) {
    (void)ap;
    if (out_count == NULL || (max_clients > 0u && out_clients == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_count = 0u;
    if (h2_bk_wifi_ap_active() == 0) {
        return H2_PAL_OK;
    }
    int lease_rc = h2_bk_wifi_lease_reconcile();
    if (lease_rc != H2_PAL_OK) return lease_rc;

    wlan_ap_stas_t stas;
    memset(&stas, 0, sizeof(stas));
    bk_err_t err = bk_wifi_ap_get_sta_list(&stas);
    if (err != BK_OK) {
        return h2_bk_wifi_map_error(err);
    }
    size_t copy_count = stas.num > 0 ? (size_t)stas.num : 0u;
    if (copy_count > max_clients) {
        copy_count = max_clients;
    }
    if (stas.sta != NULL) {
        if (h2_bk_wifi_event_lock() != H2_PAL_OK) {
            os_free(stas.sta);
            return H2_PAL_ERR_INVALID_STATE;
        }
        for (size_t i = 0u; i < copy_count; ++i) {
            h2_bk_wifi_copy_ap_client(&out_clients[i], &stas.sta[i]);
            h2_bk_wifi_accepted_lease_t *lease = h2_bk_wifi_lease_find_locked(
                out_clients[i].mac, 0);
            if (lease != NULL && lease->joined && lease->granted) {
                out_clients[i].lease = lease->client.lease;
                out_clients[i].lease_valid = 1u;
            }
        }
        h2_bk_wifi_event_unlock();
    } else {
        copy_count = 0u;
    }
    if (stas.sta != NULL) {
        os_free(stas.sta);
    }
    *out_count = copy_count;
    return H2_PAL_OK;
}

static int h2_bk_wifi_ap_get_mac(h2_pal_wifi_ap_t *ap, uint8_t out_mac[6]) {
    (void)ap;
    if (out_mac == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return h2_bk_wifi_map_error(bk_wifi_ap_get_mac(out_mac));
}

/* One admission gate covers the entire authentication/IP/save transaction. */
H2_ATOMIC_DEFINE_STATIC(flag, s_h2_bk_wifi_connect_busy, 0u);

static int h2_bk_wifi_connect(void *user,
                             const h2_pal_wifi_sta_config_t *config,
                             uint32_t timeout_ms) {
    if (h2_atomic_flag_test_and_set(&s_h2_bk_wifi_connect_busy, H2_ATOMIC_SEQ_CST))
        return H2_PAL_ERR_BUSY;
    int rc = h2_bk_wifi_sta_connect(user, config, timeout_ms);
    h2_atomic_flag_clear(&s_h2_bk_wifi_connect_busy, H2_ATOMIC_SEQ_CST);
    return rc;
}

static int h2_bk_wifi_disconnect(void *user) {
    if (h2_atomic_flag_test_and_set(&s_h2_bk_wifi_connect_busy, H2_ATOMIC_SEQ_CST))
        return H2_PAL_ERR_BUSY;
    int rc = h2_bk_wifi_sta_disconnect(user);
    h2_atomic_flag_clear(&s_h2_bk_wifi_connect_busy, H2_ATOMIC_SEQ_CST);
    return rc;
}

static int h2_bk_wifi_connect_and_save(void *user,
                                      const h2_pal_wifi_sta_config_t *config,
                                      uint32_t timeout_ms) {
    if (h2_atomic_flag_test_and_set(&s_h2_bk_wifi_connect_busy, H2_ATOMIC_SEQ_CST))
        return H2_PAL_ERR_BUSY;
    static const h2_pal_wifi_sta_vtable_t raw_vtable = {
        .get_status = (h2_pal_wifi_sta_get_status_fn)h2_bk_wifi_sta_get_status,
        .connect = (h2_pal_wifi_sta_connect_fn)h2_bk_wifi_sta_connect,
        .disconnect = (h2_pal_wifi_sta_disconnect_fn)h2_bk_wifi_sta_disconnect,
    };
    const h2_pal_wifi_sta_api_t raw = {user, &raw_vtable};
    const h2_wifi_sta_dependencies_t deps = {
        .sta = &raw,
        .settings = h2_bk_platform_wifi_settings(),
        .time = h2_bk_platform_time_api(),
    };
    int rc = h2_wifi_sta_connect_and_save(&deps, config, timeout_ms);
    h2_atomic_flag_clear(&s_h2_bk_wifi_connect_busy, H2_ATOMIC_SEQ_CST);
    return rc;
}

static const h2_pal_wifi_sta_vtable_t s_h2_bk_wifi_sta_vtable = {
    .get_status = (h2_pal_wifi_sta_get_status_fn)h2_bk_wifi_sta_get_status,
    .scan = (h2_pal_wifi_sta_scan_fn)h2_bk_wifi_sta_scan,
    .connect = h2_bk_wifi_connect,
    .connect_and_save = h2_bk_wifi_connect_and_save,
    .disconnect = h2_bk_wifi_disconnect,
    .get_mac = (h2_pal_wifi_sta_get_mac_fn)h2_bk_wifi_sta_get_mac,
    .set_power_save =
        (h2_pal_wifi_sta_set_power_save_fn)h2_bk_wifi_sta_set_power_save,
};

static h2_pal_wifi_sta_t s_h2_bk_wifi_sta = {
    .user = &s_h2_bk_wifi_sta,
    .vtable = &s_h2_bk_wifi_sta_vtable,
};

static const h2_pal_wifi_ap_vtable_t s_h2_bk_wifi_ap_vtable = {
    .start = (h2_pal_wifi_ap_start_fn)h2_bk_wifi_ap_start,
    .stop = (h2_pal_wifi_ap_stop_fn)h2_bk_wifi_ap_stop,
    .get_status = (h2_pal_wifi_ap_get_status_fn)h2_bk_wifi_ap_get_status,
    .get_clients = (h2_pal_wifi_ap_get_clients_fn)h2_bk_wifi_ap_get_clients,
    .get_mac = (h2_pal_wifi_ap_get_mac_fn)h2_bk_wifi_ap_get_mac,
};

static h2_pal_wifi_ap_t s_h2_bk_wifi_ap = {
    .user = &s_h2_bk_wifi_ap,
    .vtable = &s_h2_bk_wifi_ap_vtable,
};

h2_pal_wifi_sta_t *h2_bk_platform_wifi_sta(void) {
    if (s_h2_bk_wifi_request_mutex == NULL) {
        s_h2_bk_wifi_request_mutex = (beken_mutex_t)xSemaphoreCreateMutexStatic(
            &s_h2_bk_wifi_request_mutex_control);
    }
    if (s_h2_bk_wifi_status_mutex == NULL) {
        s_h2_bk_wifi_status_mutex = (beken_mutex_t)xSemaphoreCreateMutexStatic(
            &s_h2_bk_wifi_status_mutex_control);
    }
    return &s_h2_bk_wifi_sta;
}

h2_pal_wifi_ap_t *h2_bk_platform_wifi_ap(void) {
    return &s_h2_bk_wifi_ap;
}
