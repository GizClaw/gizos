#ifndef H2_BK_DHCP_RING_H
#define H2_BK_DHCP_RING_H

#include <stdint.h>

#define H2_BK_DHCP_RING_CAPACITY 32u
#define H2_BK_DHCP_SNAPSHOT_VERSION 1u

typedef struct h2_bk_dhcp_entry {
    uint32_t ticket, dir, type, src, dst, xid, vif, role;
    uint32_t netif, flags, ip4, is_default, bytes;
} h2_bk_dhcp_entry_t;

typedef struct h2_bk_dhcp_snapshot_data {
    uint32_t version, count, last_ticket, dropped;
    h2_bk_dhcp_entry_t entries[H2_BK_DHCP_RING_CAPACITY];
} h2_bk_dhcp_snapshot_t;

/* Packet path: one CAS attempt, no lock wait, logging, clock, IPC or allocation. */
void h2_bk_dhcp_record(const h2_bk_dhcp_entry_t *entry);
/* Ordinary query task: up to three attempts per slot, each record is stable. */
void h2_bk_dhcp_snapshot(h2_bk_dhcp_snapshot_t *out);

/* AP ordinary task: query the paired CP via the SDK confirmed-payload RPC. */
int h2_bk_dhcp_query(h2_bk_dhcp_snapshot_t *out);

#endif
