#ifndef H2_BK_WIFI_LEASE_H
#define H2_BK_WIFI_LEASE_H

#include <stdint.h>

/* Private paired AP/CP protocol. Every address is a 32-bit host-order IPv4
 * value. MAC is split into network-order high 16 and low 32 bits. */
#define H2_BK_WIFI_LEASE_CAPACITY 8u
#define H2_BK_WIFI_LEASE_VERSION 1u
#define H2_BK_WIFI_LEASE_WAKE_MAGIC 0x48324c31u

typedef struct h2_bk_wifi_lease_record {
    uint32_t sequence;
    uint32_t ip4;
    uint32_t mac_hi;
    uint32_t mac_lo;
    uint32_t xid;
} h2_bk_wifi_lease_record_t;

typedef struct h2_bk_wifi_lease_snapshot {
    uint32_t version;
    uint32_t generation;
    uint32_t count;
    uint32_t last_sequence;
    uint32_t wake_failures;
    h2_bk_wifi_lease_record_t records[H2_BK_WIFI_LEASE_CAPACITY];
} h2_bk_wifi_lease_snapshot_t;

/* Hint only: the snapshot above remains authoritative if IPC loses a hint. */
typedef struct h2_bk_wifi_lease_wake {
    uint32_t magic;
    uint32_t version;
    uint32_t generation;
    uint32_t sequence;
    uint32_t ip4;
    uint32_t mac_hi;
    uint32_t mac_lo;
    uint32_t xid;
} h2_bk_wifi_lease_wake_t;

/* CP ordinary DHCP service and paired Wi-Fi RPC only. A successful ACK send
 * records one grant per MAC/XID/generation. Duplicate sends of that ACK keep
 * the same sequence; a new XID advances it even with the same MAC/IP. No
 * OFFER may call accept. */
void h2_bk_wifi_lease_reset(void);
/* CP L2 disconnect invalidates that association's ACK. A new XID must
 * receive a fresh ACK before a reconnect can be reported as leased. */
void h2_bk_wifi_lease_forget(const uint8_t mac[6]);
uint32_t h2_bk_wifi_lease_accept(const uint8_t mac[6], uint32_t ip4, uint32_t xid);
void h2_bk_wifi_lease_snapshot(h2_bk_wifi_lease_snapshot_t *out);

#endif
