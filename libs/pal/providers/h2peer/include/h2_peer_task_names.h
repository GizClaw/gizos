#ifndef H2_PEER_TASK_NAMES_H
#define H2_PEER_TASK_NAMES_H

#define H2_PEER_NETWORK_TASK_NAME_VALUE "$h2peer/net"
#define H2_PEER_UDP_TASK_NAME_VALUE "$h2peer/udp"

#ifdef __cplusplus
extern "C" {
#endif

extern const char
    h2_peer_network_task_name[sizeof(H2_PEER_NETWORK_TASK_NAME_VALUE)];
extern const char h2_peer_udp_task_name[sizeof(H2_PEER_UDP_TASK_NAME_VALUE)];

#ifdef __cplusplus
}
#endif

#endif
