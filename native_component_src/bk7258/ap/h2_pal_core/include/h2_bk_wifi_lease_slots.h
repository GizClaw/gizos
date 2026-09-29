#ifndef H2_BK_WIFI_LEASE_SLOTS_H
#define H2_BK_WIFI_LEASE_SLOTS_H

#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2_bk_wifi_lease.h"
#include <stdint.h>
#include <string.h>

typedef struct h2_bk_wifi_accepted_lease {
    h2_pal_wifi_ap_client_t client;
    uint32_t sequence;
    uint32_t xid;
    uint8_t used, joined, granted, pending, left_pending, release_pending;
} h2_bk_wifi_accepted_lease_t;

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
        else if (slot->used && !slot->joined && !slot->granted &&
                 !slot->pending && !slot->left_pending &&
                 !slot->release_pending && drained < 0)
            drained = (int)i;
    }
    return unused >= 0 ? unused : drained;
}

#endif
