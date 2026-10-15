#ifndef H2_BK_DHCP_TRACE_H
#define H2_BK_DHCP_TRACE_H

#include <stdint.h>
#include "h2_bk_dhcp_ring.h"
#include <lwip/def.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>

/* Read-only diagnostics: bounded chain reads; never log packet payload or keys. */
static uint16_t h2_bk_dhcp_u16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8u) | p[1]);
}
static uint32_t h2_bk_dhcp_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24u) | ((uint32_t)p[1] << 16u) |
           ((uint32_t)p[2] << 8u) | p[3];
}
static int h2_bk_dhcp_read(const struct pbuf *p, uint32_t offset,
                          void *out, uint16_t length) {
    if (offset > p->tot_len || length > p->tot_len - offset || offset > UINT16_MAX)
        return 0;
    return pbuf_copy_partial(p, out, length, (uint16_t)offset) == length;
}
/* Return metadata only for a complete bounded DHCP/BOOTP frame. This helper
 * is also used by the host-to-controller sender to preserve its send result. */
static int h2_bk_dhcp_capture(uint32_t direction, const struct pbuf *p,
                              const struct netif *netif, int vif_id,
                              int vif_type, h2_bk_dhcp_entry_t *out) {
    uint8_t ether[18], ip[20], udp[8], bootp[8], cookie[4];
    uint8_t yiaddr[4], chaddr[6], bootp_flags[2];
    uint32_t ethernet_size = 14u;
    if (p == NULL || out == NULL || !h2_bk_dhcp_read(p, 0u, ether, 14u)) return 0;
    uint16_t type = h2_bk_dhcp_u16(ether + 12u);
    if (type == 0x8100u) {
        if (!h2_bk_dhcp_read(p, 0u, ether, 18u)) return 0;
        type = h2_bk_dhcp_u16(ether + 16u);
        ethernet_size = 18u;
    }
    if (type != 0x0800u || !h2_bk_dhcp_read(p, ethernet_size, ip, 20u)) return 0;
    uint32_t ihl = (uint32_t)(ip[0] & 15u) * 4u;
    uint32_t ip_length = h2_bk_dhcp_u16(ip + 2u);
    if ((ip[0] >> 4u) != 4u || ihl < 20u || ihl > 60u || ip[9] != 17u ||
        (h2_bk_dhcp_u16(ip + 6u) & 0x3fffu) != 0u ||
        ip_length < ihl + 8u || ethernet_size + ip_length > p->tot_len) return 0;
    uint32_t udp_offset = ethernet_size + ihl;
    if (!h2_bk_dhcp_read(p, udp_offset, udp, 8u)) return 0;
    uint16_t source_port = h2_bk_dhcp_u16(udp);
    uint16_t destination_port = h2_bk_dhcp_u16(udp + 2u);
    if (!((source_port == 67u && destination_port == 68u) ||
          (source_port == 68u && destination_port == 67u))) return 0;
    uint32_t udp_length = h2_bk_dhcp_u16(udp + 4u);
    if (udp_length < 248u || udp_length > ip_length - ihl) return 0;
    uint32_t start = udp_offset + 8u, end = udp_offset + udp_length;
    if (!h2_bk_dhcp_read(p, start, bootp, 8u) ||
        !h2_bk_dhcp_read(p, start + 10u, bootp_flags, 2u) ||
        !h2_bk_dhcp_read(p, start + 16u, yiaddr, 4u) ||
        !h2_bk_dhcp_read(p, start + 28u, chaddr, 6u) ||
        !h2_bk_dhcp_read(p, start + 236u, cookie, 4u) ||
        h2_bk_dhcp_u32(cookie) != 0x63825363u) return 0;
    unsigned message_type = 0u;
    uint32_t server_id = 0u, cursor = start + 240u;
    for (unsigned options = 0u; options < 64u && cursor < end; ++options) {
        uint8_t tag, length, value, server[4];
        if (!h2_bk_dhcp_read(p, cursor++, &tag, 1u)) break;
        if (tag == 255u) break;
        if (tag == 0u) continue;
        if (cursor >= end || !h2_bk_dhcp_read(p, cursor++, &length, 1u) ||
            length > end - cursor) break;
        if (tag == 53u && length == 1u &&
            h2_bk_dhcp_read(p, cursor, &value, 1u)) message_type = value;
        if (tag == 54u && length == 4u &&
            h2_bk_dhcp_read(p, cursor, server, 4u)) server_id = h2_bk_dhcp_u32(server);
        cursor += length;
    }
    *out = (h2_bk_dhcp_entry_t){
        .dir = direction,
        .type = message_type,
        .src = source_port,
        .dst = destination_port,
        .xid = h2_bk_dhcp_u32(bootp + 4u),
        .vif = (uint32_t)vif_id,
        .role = (uint32_t)vif_type,
        .netif = netif ? (uint32_t)netif_get_index(netif) : 0u,
        .flags = netif ? (uint32_t)netif->flags : 0u,
        .ip4 = netif ? lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(netif))) : 0u,
        .is_default = (uint32_t)(netif != NULL && netif == netif_default),
        .bytes = p->tot_len,
        .ip_src = h2_bk_dhcp_u32(ip + 12u),
        .ip_dst = h2_bk_dhcp_u32(ip + 16u),
        .yiaddr = h2_bk_dhcp_u32(yiaddr),
        .server_id = server_id,
        .udp_checksum = h2_bk_dhcp_u16(udp + 6u),
        .bootp_flags = h2_bk_dhcp_u16(bootp_flags),
        .eth_src_hi = h2_bk_dhcp_u16(ether + 6u),
        .eth_src_lo = h2_bk_dhcp_u32(ether + 8u),
        .eth_dst_hi = h2_bk_dhcp_u16(ether),
        .eth_dst_lo = h2_bk_dhcp_u32(ether + 2u),
        .chaddr_hi = h2_bk_dhcp_u16(chaddr),
        .chaddr_lo = h2_bk_dhcp_u32(chaddr + 2u),
    };
    return 1;
}

static void h2_bk_dhcp_trace(const char *direction, const struct pbuf *p,
                             const struct netif *netif, int vif_id, int vif_type) {
    h2_bk_dhcp_entry_t entry;
    if (h2_bk_dhcp_capture(direction[0] == 'T' ? 1u : 2u, p, netif,
                           vif_id, vif_type, &entry))
        h2_bk_dhcp_record(&entry);
}

#endif
