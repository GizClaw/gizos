#include "h2_bk_wifi_lease_slots.h"
#include <assert.h>
#include <string.h>

static h2_bk_wifi_accepted_lease_t slots[H2_BK_WIFI_LEASE_CAPACITY];

static void mac_for(unsigned number, uint8_t mac[6]) {
    memset(mac, 0, 6u);
    mac[0] = 0x02;
    mac[5] = (uint8_t)number;
}

typedef struct fake_event_queue {
    unsigned available;
    unsigned count;
    char events[8];
    uint8_t last_mac[6];
} fake_event_queue_t;

static int post_join(const h2_pal_wifi_ap_client_t *client, void *context) {
    fake_event_queue_t *queue = context;
    if (!queue->available) return H2_PAL_ERR_TIMEOUT;
    --queue->available;
    queue->events[queue->count++] = 'J';
    memcpy(queue->last_mac, client->mac, sizeof(queue->last_mac));
    return H2_PAL_OK;
}

static void grant_if_ready(h2_bk_wifi_accepted_lease_t *lease,
                           fake_event_queue_t *queue) {
    if (!h2_bk_wifi_lease_ready_for_grant(lease)) return;
    assert(queue->available);
    --queue->available;
    queue->events[queue->count++] = 'G';
    lease->pending = 0u;
}

static void test_join_backpressure(void) {
    h2_bk_wifi_accepted_lease_t lease = {0};
    h2_pal_wifi_ap_client_t client = {0};
    fake_event_queue_t queue = {0};
    mac_for(42u, client.mac);
    lease.used = 1u;
    memcpy(lease.client.mac, client.mac, 6u);
    h2_bk_wifi_lease_note_join(&lease, &client);
    lease.sequence = 12u;
    lease.client.lease.ip4 = 0xc0a8bc02u;
    lease.pending = 1u; /* real ACK arrived while JOIN was backed up */

    assert(!h2_bk_wifi_lease_drain_join(&lease, 1, post_join, &queue));
    assert(!lease.joined && lease.join_pending && lease.pending);
    grant_if_ready(&lease, &queue);
    assert(queue.count == 0u); /* no GRANTED before JOIN */

    queue.available = 2u;
    assert(h2_bk_wifi_lease_drain_join(&lease, 1, post_join, &queue));
    grant_if_ready(&lease, &queue);
    assert(lease.joined && !lease.join_pending && !lease.pending);
    assert(queue.count == 2u && queue.events[0] == 'J' && queue.events[1] == 'G');
    assert(h2_bk_wifi_lease_drain_join(&lease, 1, post_join, &queue));
    assert(queue.count == 2u); /* duplicate event/reconcile cannot duplicate JOIN */

    /* A pending JOIN cannot consume a drained slot needed by another MAC. */
    lease.joined = 0u;
    h2_bk_wifi_lease_note_join(&lease, &client);
    h2_bk_wifi_accepted_lease_t full[H2_BK_WIFI_LEASE_CAPACITY] = {0};
    full[0] = lease;
    for (unsigned i = 1u; i < H2_BK_WIFI_LEASE_CAPACITY; ++i) {
        full[i].used = 1u;
        full[i].joined = 1u;
        mac_for(i, full[i].client.mac);
    }
    uint8_t other[6];
    mac_for(99u, other);
    assert(h2_bk_wifi_lease_slot_index(full, other, 1) == -1);
    h2_bk_wifi_lease_cancel_join(&full[0]);
    assert(h2_bk_wifi_lease_slot_index(full, other, 1) == 0);
}

static void test_leave_rejoin_and_stop_while_join_pending(void) {
    h2_bk_wifi_accepted_lease_t lease = {0};
    h2_pal_wifi_ap_client_t client = {0};
    fake_event_queue_t queue = {0};
    mac_for(51u, client.mac);
    lease.used = 1u;
    memcpy(lease.client.mac, client.mac, 6u);
    h2_bk_wifi_lease_note_join(&lease, &client);
    lease.sequence = 7u;
    lease.pending = 1u;
    assert(!h2_bk_wifi_lease_drain_join(&lease, 1, post_join, &queue));

    /* L2 LEFT before a visible JOIN cancels both events and stale ACK. */
    h2_bk_wifi_lease_cancel_join(&lease);
    assert(!lease.join_pending && !lease.pending && !lease.joined);
    queue.available = 1u;
    assert(h2_bk_wifi_lease_drain_join(&lease, 1, post_join, &queue));
    assert(queue.count == 0u);

    /* A new association of the same MAC has one fresh JOIN, never old GRANT. */
    client.rssi = -42;
    h2_bk_wifi_lease_note_join(&lease, &client);
    assert(h2_bk_wifi_lease_drain_join(&lease, 1, post_join, &queue));
    grant_if_ready(&lease, &queue);
    assert(queue.count == 1u && queue.events[0] == 'J' && !lease.pending);
    assert(lease.client.rssi == -42);

    /* Old LEFT/RELEASED must drain before a later association's JOIN. */
    lease.joined = 0u;
    lease.left_pending = 1u;
    lease.release_pending = 1u;
    h2_bk_wifi_lease_note_join(&lease, &client);
    queue.available = 1u;
    assert(!h2_bk_wifi_lease_drain_join(&lease, 1, post_join, &queue));
    assert(queue.count == 1u);
    lease.left_pending = 0u;
    assert(!h2_bk_wifi_lease_drain_join(&lease, 1, post_join, &queue));
    lease.release_pending = 0u;
    assert(h2_bk_wifi_lease_drain_join(&lease, 1, post_join, &queue));
    assert(queue.count == 2u && queue.events[1] == 'J');

    /* AP STOP clears an unreported JOIN before the next generation. */
    lease.joined = 0u;
    h2_bk_wifi_lease_note_join(&lease, &client);
    h2_bk_wifi_lease_cancel_join(&lease);
    memset(&lease, 0, sizeof(lease));
    assert(h2_bk_wifi_lease_drain_join(&lease, 1, post_join, &queue));
    assert(queue.count == 2u);
}

static void test_join_during_ap_start(void) {
    h2_bk_wifi_accepted_lease_t lease = {0};
    h2_pal_wifi_ap_client_t client = {0};
    fake_event_queue_t queue = {.available = 3u};
    mac_for(63u, client.mac);
    lease.used = 1u;
    memcpy(lease.client.mac, client.mac, 6u);

    /* bk_wifi_ap_start may deliver a real JOIN before 5F3 returns. The
     * STARTING state admits it, but cannot post it before AP_STARTED. */
    assert(h2_bk_wifi_lease_join_admitted(0, 1));
    h2_bk_wifi_lease_note_join(&lease, &client);
    lease.sequence = 1u;
    lease.client.lease.ip4 = 0xc0a8bc02u;
    lease.pending = 1u;
    assert(!h2_bk_wifi_lease_drain_join(&lease, 0, post_join, &queue));
    assert(queue.count == 0u && lease.join_pending && !lease.joined);

    queue.events[queue.count++] = 'S'; /* AP_STARTED post accepted */
    --queue.available;
    assert(h2_bk_wifi_lease_join_admitted(1, 0));
    assert(h2_bk_wifi_lease_drain_join(&lease, 1, post_join, &queue));
    grant_if_ready(&lease, &queue);
    assert(queue.count == 3u && queue.events[0] == 'S' &&
           queue.events[1] == 'J' && queue.events[2] == 'G');
    assert(h2_bk_wifi_lease_drain_join(&lease, 1, post_join, &queue));
    assert(queue.count == 3u);

    /* A failed AP_STARTED/stop cancels the unseen JOIN. After STOPPED, a
     * delayed SDK callback cannot create a slot in the next generation. */
    memset(&lease, 0, sizeof(lease));
    h2_bk_wifi_lease_note_join(&lease, &client);
    assert(!h2_bk_wifi_lease_drain_join(&lease, 0, post_join, &queue));
    h2_bk_wifi_lease_cancel_join(&lease);
    memset(&lease, 0, sizeof(lease));
    assert(!h2_bk_wifi_lease_join_admitted(0, 0));
    assert(h2_bk_wifi_lease_drain_join(&lease, 1, post_join, &queue));
    assert(queue.count == 3u);
}

int main(void) {
    test_join_backpressure();
    test_leave_rejoin_and_stop_while_join_pending();
    test_join_during_ap_start();
    uint8_t mac[6];
    for (unsigned i = 0u; i < H2_BK_WIFI_LEASE_CAPACITY; ++i) {
        mac_for(i + 1u, mac);
        int slot = h2_bk_wifi_lease_slot_index(slots, mac, 1);
        assert(slot == (int)i);
        slots[slot].used = 1u;
        slots[slot].joined = 1u;
        slots[slot].sequence = i + 1u;
        memcpy(slots[slot].client.mac, mac, 6u);
    }
    mac_for(9u, mac);
    assert(h2_bk_wifi_lease_slot_index(slots, mac, 1) == -1);

    /* Once the first peer departs but its release has not reached Runtime,
     * the slot remains reserved. A full table must fail closed. */
    slots[0].joined = 0u;
    slots[0].granted = 0u;
    slots[0].release_pending = 1u;
    assert(h2_bk_wifi_lease_slot_index(slots, mac, 1) == -1);
    slots[0].release_pending = 0u;
    slots[0].left_pending = 1u;
    assert(h2_bk_wifi_lease_slot_index(slots, mac, 1) == -1);
    slots[0].left_pending = 0u;
    slots[0].pending = 1u;
    assert(h2_bk_wifi_lease_slot_index(slots, mac, 1) == -1);
    slots[0].pending = 0u;
    assert(h2_bk_wifi_lease_slot_index(slots, mac, 1) == 0);

    /* The same MAC reuses its own record without losing the last ACK floor,
     * which rejects a delayed replay after the next JOIN. */
    mac_for(1u, mac);
    assert(h2_bk_wifi_lease_slot_index(slots, mac, 1) == 0);
    assert(slots[0].sequence == 1u);

    /* More than eight distinct sequential clients remain possible. A newly
     * allocated client has no old sequence; CP's generation-wide sequence is
     * still authoritative for its next ACK. */
    for (unsigned i = 9u; i <= 24u; ++i) {
        mac_for(i, mac);
        int slot = h2_bk_wifi_lease_slot_index(slots, mac, 1);
        assert(slot == 0);
        memset(&slots[slot], 0, sizeof(slots[slot]));
        slots[slot].used = 1u;
        slots[slot].joined = 1u;
        slots[slot].sequence = i;
        memcpy(slots[slot].client.mac, mac, 6u);
        slots[slot].joined = 0u;
    }
    return 0;
}
