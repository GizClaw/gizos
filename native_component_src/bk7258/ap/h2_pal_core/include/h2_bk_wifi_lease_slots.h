#ifndef H2_BK_WIFI_LEASE_SLOTS_H
#define H2_BK_WIFI_LEASE_SLOTS_H

#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2_bk_wifi_lease.h"
#include <stdint.h>
#include <string.h>

typedef struct h2_bk_wifi_accepted_lease {
    h2_pal_wifi_ap_client_t client;
    h2_pal_wifi_ap_client_t join_client;
    uint32_t sequence;
    uint32_t xid;
    uint8_t used, joined, join_pending, granted, pending, left_pending, release_pending;
} h2_bk_wifi_accepted_lease_t;

typedef int (*h2_bk_wifi_lease_join_post_t)(
    const h2_pal_wifi_ap_client_t *client, void *context);

/* STARTING admits real L2 joins, but delivery waits until AP_STARTED has
 * entered Runtime. STOPPED admits neither new nor delayed joins. */
static inline int h2_bk_wifi_lease_join_admitted(int active, int starting) {
    return active || starting;
}

/* L2 JOIN is real even when Runtime's bounded queue cannot take the event.
 * Keep its new payload separate while an old association's LEFT/RELEASED is
 * waiting; the old events still need the previous client and lease. */
static inline void h2_bk_wifi_lease_note_join(
    h2_bk_wifi_accepted_lease_t *lease,
    const h2_pal_wifi_ap_client_t *client) {
    lease->join_client = *client;
    lease->join_client.lease_valid = 0u;
    memset(&lease->join_client.lease, 0, sizeof(lease->join_client.lease));
    lease->join_pending = 1u;
}

static inline void h2_bk_wifi_lease_cancel_join(
    h2_bk_wifi_accepted_lease_t *lease) {
    lease->join_pending = 0u;
    lease->pending = 0u;
}

/* The ordinary worker retries this after draining old LEFT/RELEASED events.
 * A transient post failure never marks the association joined or consumes its
 * pending ACK; a later pass can then emit JOIN before GRANTED. */
static inline int h2_bk_wifi_lease_drain_join(
    h2_bk_wifi_accepted_lease_t *lease,
    int ap_started,
    h2_bk_wifi_lease_join_post_t post, void *context) {
    if (!lease->join_pending) return 1;
    if (!ap_started || lease->left_pending || lease->release_pending ||
        post(&lease->join_client, context) != H2_PAL_OK) return 0;
    uint32_t previous_ip4 = lease->client.lease.ip4;
    lease->client = lease->join_client;
    lease->client.lease.ip4 = previous_ip4;
    lease->client.lease_valid = 0u;
    lease->joined = 1u;
    lease->join_pending = 0u;
    return 1;
}

static inline int h2_bk_wifi_lease_ready_for_grant(
    const h2_bk_wifi_accepted_lease_t *lease) {
    return lease != NULL && lease->joined && !lease->join_pending &&
           lease->pending && lease->sequence && lease->client.lease.ip4;
}

/* A departed client retains its last ACK sequence until that MAC returns, so
 * a delayed snapshot cannot grant its old lease after a new JOIN. A different
 * MAC may reuse the entry only after every event has drained. CP still owns
 * the generation-wide sequence and forgets the old MAC on its L2 departure. */
static inline int h2_bk_wifi_lease_slot_index(
    const h2_bk_wifi_accepted_lease_t slots[H2_BK_WIFI_LEASE_CAPACITY],
    const uint8_t mac[6], int create) {
    int unused = -1, drained = -1;
    for (unsigned i = 0u; i < H2_BK_WIFI_LEASE_CAPACITY; ++i) {
        const h2_bk_wifi_accepted_lease_t *slot = &slots[i];
        if (slot->used && memcmp(slot->client.mac, mac, 6u) == 0)
            return (int)i;
        if (!create) continue;
        if (!slot->used && unused < 0)
            unused = (int)i;
        else if (slot->used && !slot->joined && !slot->join_pending &&
                 !slot->granted &&
                 !slot->pending && !slot->left_pending &&
                 !slot->release_pending && drained < 0)
            drained = (int)i;
    }
    return unused >= 0 ? unused : drained;
}

#endif
