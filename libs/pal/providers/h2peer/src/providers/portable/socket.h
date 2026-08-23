#ifndef SOCKET_H_
#define SOCKET_H_

#include "address.h"
#include "h2/pal/net/h2_pal_net.h"

typedef struct UdpSocket {
  h2_pal_net_socket_t fd;
  h2_pal_net_addr_t bind_addr;
  const h2_pal_net_api_t* net;
} UdpSocket;

typedef struct TcpSocket {
  h2_pal_net_socket_t fd;
  const h2_pal_net_api_t* net;
} TcpSocket;

int udp_socket_open(UdpSocket* udp_socket, const h2_pal_net_api_t* net,
                    h2_pal_net_family_t family, uint16_t port);
void udp_socket_close(UdpSocket* udp_socket);
int udp_socket_sendto(UdpSocket* udp_socket, const h2_pal_net_addr_t* addr,
                      const uint8_t* buf, int len);
int udp_socket_recvfrom(UdpSocket* udp_socket, h2_pal_net_addr_t* addr,
                        uint8_t* buf, int len, uint32_t timeout_ms);

int tcp_socket_connect(
    TcpSocket* tcp_socket, const h2_pal_net_api_t* net,
    const h2_pal_net_addr_t* source_addr,
    const h2_pal_net_addr_t* remote_addr, uint32_t timeout_ms);
int tcp_socket_send_timeout(
    TcpSocket* tcp_socket, const uint8_t* buf, int len,
    uint32_t timeout_ms);
int tcp_socket_recv(
    TcpSocket* tcp_socket, uint8_t* buf, int len, uint32_t timeout_ms);
void tcp_socket_close(TcpSocket* tcp_socket);

#endif  // SOCKET_H_
