#ifndef H2_BK_DHCP_RING_H
#define H2_BK_DHCP_RING_H

#include <stdint.h>

#define H2_BK_DHCP_RING_CAPACITY 32u
#define H2_BK_DHCP_SNAPSHOT_VERSION 2u

typedef struct h2_bk_dhcp_entry {
    uint32_t ticket, dir, type, src, dst, xid, vif, role;
    uint32_t netif, flags, ip4, is_default, bytes;
    uint32_t ip_src, ip_dst, yiaddr, server_id, udp_checksum, bootp_flags;
    uint32_t eth_src_hi, eth_src_lo, eth_dst_hi, eth_dst_lo;
    uint32_t chaddr_hi, chaddr_lo, send_rc;
} h2_bk_dhcp_entry_t;

typedef struct h2_bk_dhcp_snapshot_data {
    uint32_t version, count, last_ticket, dropped;
    h2_bk_dhcp_entry_t entries[H2_BK_DHCP_RING_CAPACITY];
} h2_bk_dhcp_snapshot_t;

/* dir: 1=CP lwIP TX, 2=CP radio RX, 3=AP-host IPC TX attempt,
 * 4=AP-host IPC TX sender returned. send_rc is valid for dir 4 only.
 * Packet path: one CAS attempt, no lock wait, logging, clock, IPC or allocation. */
void h2_bk_dhcp_record(const h2_bk_dhcp_entry_t *entry);
/* Ordinary query task: up to three attempts per slot, each record is stable. */
void h2_bk_dhcp_snapshot(h2_bk_dhcp_snapshot_t *out);

/* AP ordinary task: query the paired CP via the SDK confirmed-payload RPC. */
int h2_bk_dhcp_query(h2_bk_dhcp_snapshot_t *out);

#endif
