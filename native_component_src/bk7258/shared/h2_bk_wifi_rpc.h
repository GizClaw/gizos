#ifndef H2_BK_WIFI_RPC_H
#define H2_BK_WIFI_RPC_H

/* Private AP/CP extension, unused in the pinned SDK's 0x300..0x5ff API
 * command range. Managed packages always carry the paired AP and CP code. */
#define H2_BK_WIFI_RPC_STA_DISASSOCIATE 0x5f0u
#define H2_BK_WIFI_RPC_STA_ASSOCIATE 0x5f1u
#define H2_BK_WIFI_RPC_DHCP_SNAPSHOT 0x5f2u
#define H2_BK_WIFI_RPC_LEASE_SNAPSHOT 0x5f3u
#define H2_BK_WIFI_RPC_IPV6_SNAPSHOT 0x5f4u

#endif
