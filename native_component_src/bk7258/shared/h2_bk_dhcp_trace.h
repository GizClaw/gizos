#ifndef H2_BK_DHCP_TRACE_H
#define H2_BK_DHCP_TRACE_H

#include <stdint.h>
#include <common/bk_include.h>
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
static void h2_bk_dhcp_trace(const char *direction, const struct pbuf *p,
                             const struct netif *netif, int vif_id, int vif_type) {
    uint8_t ether[18], ip[20], udp[8], bootp[8], cookie[4];
    uint32_t ethernet_size = 14u;
    if (p == NULL || !h2_bk_dhcp_read(p, 0u, ether, 14u)) return;
    uint16_t type = h2_bk_dhcp_u16(ether + 12u);
    if (type == 0x8100u) {
        if (!h2_bk_dhcp_read(p, 0u, ether, 18u)) return;
        type = h2_bk_dhcp_u16(ether + 16u);
        ethernet_size = 18u;
    }
    if (type != 0x0800u || !h2_bk_dhcp_read(p, ethernet_size, ip, 20u)) return;
    uint32_t ihl = (uint32_t)(ip[0] & 15u) * 4u;
    uint32_t ip_length = h2_bk_dhcp_u16(ip + 2u);
    if ((ip[0] >> 4u) != 4u || ihl < 20u || ihl > 60u || ip[9] != 17u ||
        (h2_bk_dhcp_u16(ip + 6u) & 0x3fffu) != 0u ||
        ip_length < ihl + 8u || ethernet_size + ip_length > p->tot_len) return;
    uint32_t udp_offset = ethernet_size + ihl;
    if (!h2_bk_dhcp_read(p, udp_offset, udp, 8u)) return;
    uint16_t source_port = h2_bk_dhcp_u16(udp), destination_port = h2_bk_dhcp_u16(udp + 2u);
    if (!((source_port == 67u && destination_port == 68u) ||
          (source_port == 68u && destination_port == 67u))) return;
    uint32_t udp_length = h2_bk_dhcp_u16(udp + 4u);
    if (udp_length < 248u || udp_length > ip_length - ihl) return;
    uint32_t start = udp_offset + 8u, end = udp_offset + udp_length;
    if (!h2_bk_dhcp_read(p, start, bootp, 8u) ||
        !h2_bk_dhcp_read(p, start + 236u, cookie, 4u) ||
        h2_bk_dhcp_u32(cookie) != 0x63825363u) return;
    unsigned message_type = 0u;
    uint32_t cursor = start + 240u;
    for (unsigned options = 0u; options < 64u && cursor < end; ++options) {
        uint8_t tag, length, value;
        if (!h2_bk_dhcp_read(p, cursor++, &tag, 1u)) break;
        if (tag == 255u) break;
        if (tag == 0u) continue;
        if (cursor >= end || !h2_bk_dhcp_read(p, cursor++, &length, 1u) || length > end - cursor) break;
        if (tag == 53u && length == 1u && h2_bk_dhcp_read(p, cursor, &value, 1u)) {
            message_type = value;
            break;
        }
        cursor += length;
    }
    BK_LOGI("h2_dhcp", "H2_BK_DHCP dir=%s type=%u src=%u dst=%u xid=%lu "
            "vif=%d role=%d netif=%u flags=%u ip4=%lu default=%u bytes=%u\r\n",
            direction, message_type, source_port, destination_port,
            (unsigned long)h2_bk_dhcp_u32(bootp + 4u), vif_id, vif_type,
            netif ? (unsigned)netif_get_index(netif) : 0u,
            netif ? (unsigned)netif->flags : 0u,
            netif ? (unsigned long)lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(netif))) : 0ul,
            (unsigned)(netif != NULL && netif == netif_default), (unsigned)p->tot_len);
}

#endif
