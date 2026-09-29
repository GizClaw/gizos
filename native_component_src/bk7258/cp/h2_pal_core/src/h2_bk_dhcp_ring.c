#include "h2_bk_dhcp_ring.h"
#include <stddef.h>

_Static_assert(sizeof(h2_bk_dhcp_entry_t) == 26u * sizeof(uint32_t), "wire entry layout");
_Static_assert(__atomic_always_lock_free(sizeof(uint32_t), 0), "32-bit atomics required");

typedef struct h2_bk_dhcp_slot {
    uint32_t guard;
    h2_bk_dhcp_entry_t entry;
} h2_bk_dhcp_slot_t;

static h2_bk_dhcp_slot_t slots[H2_BK_DHCP_RING_CAPACITY];
static uint32_t next_ticket, dropped;

/* Sequential consistency keeps the guard and every field ordered, and atomic
 * fields avoid C data races even when the reader later rejects a busy slot. */
#define STORE(field, value) __atomic_store_n(&(field), (value), __ATOMIC_SEQ_CST)
#define LOAD(field) __atomic_load_n(&(field), __ATOMIC_SEQ_CST)
#define EACH_FIELD(OP) \
    OP(ticket) OP(dir) OP(type) OP(src) OP(dst) OP(xid) OP(vif) OP(role) \
    OP(netif) OP(flags) OP(ip4) OP(is_default) OP(bytes) \
    OP(ip_src) OP(ip_dst) OP(yiaddr) OP(server_id) OP(udp_checksum) \
    OP(bootp_flags) OP(eth_src_hi) OP(eth_src_lo) OP(eth_dst_hi) \
    OP(eth_dst_lo) OP(chaddr_hi) OP(chaddr_lo) OP(send_rc)

void h2_bk_dhcp_record(const h2_bk_dhcp_entry_t *entry) {
    if (entry == NULL) return;
    uint32_t ticket = __atomic_add_fetch(&next_ticket, 1u, __ATOMIC_SEQ_CST);
    if (ticket == 0u) ticket = __atomic_add_fetch(&next_ticket, 1u, __ATOMIC_SEQ_CST);
    h2_bk_dhcp_slot_t *slot = &slots[(ticket - 1u) % H2_BK_DHCP_RING_CAPACITY];
    uint32_t version = LOAD(slot->guard);
    if ((version & 1u) != 0u ||
        !__atomic_compare_exchange_n(&slot->guard, &version, version + 1u, 1,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        (void)__atomic_add_fetch(&dropped, 1u, __ATOMIC_SEQ_CST);
        return;
    }
    h2_bk_dhcp_entry_t value = *entry;
    value.ticket = ticket;
#define WRITE_FIELD(name) STORE(slot->entry.name, value.name);
    EACH_FIELD(WRITE_FIELD)
#undef WRITE_FIELD
    STORE(slot->guard, version + 2u);
}

void h2_bk_dhcp_snapshot(h2_bk_dhcp_snapshot_t *out) {
    if (out == NULL) return;
    *out = (h2_bk_dhcp_snapshot_t){0};
    out->version = H2_BK_DHCP_SNAPSHOT_VERSION;
    out->count = 0u;
    for (unsigned index = 0u; index < H2_BK_DHCP_RING_CAPACITY; ++index) {
        h2_bk_dhcp_slot_t *slot = &slots[index];
        for (unsigned attempt = 0u; attempt < 3u; ++attempt) {
            uint32_t before = LOAD(slot->guard);
            if ((before & 1u) != 0u) continue;
            h2_bk_dhcp_entry_t value;
#define READ_FIELD(name) value.name = LOAD(slot->entry.name);
            EACH_FIELD(READ_FIELD)
#undef READ_FIELD
            uint32_t after = LOAD(slot->guard);
            if (before == after && (after & 1u) == 0u) {
                if (value.ticket != 0u) out->entries[out->count++] = value;
                break;
            }
        }
    }
    out->last_ticket = LOAD(next_ticket);
    out->dropped = LOAD(dropped);
    /* Consumers should remember the last observed ticket per physical slot,
     * (ticket-1)%32, rather than advancing a global cursor to last_ticket:
     * another producer may still be completing an older reserved ticket. */
}

#undef EACH_FIELD
#undef STORE
#undef LOAD
