#include "h2_bk_wifi_lease_slots.h"
#include <assert.h>
#include <string.h>

static h2_bk_wifi_accepted_lease_t slots[H2_BK_WIFI_LEASE_CAPACITY];

static void mac_for(unsigned number, uint8_t mac[6]) {
    memset(mac, 0, 6u);
    mac[0] = 0x02;
    mac[5] = (uint8_t)number;
}

int main(void) {
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
