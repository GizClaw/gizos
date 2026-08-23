#ifndef ICE_TRANSPORT_H_
#define ICE_TRANSPORT_H_

#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "socket.h"

typedef enum IceTransportProtocol {
  ICE_TRANSPORT_UDP = 0,
  ICE_TRANSPORT_TCP,
} IceTransportProtocol;

typedef struct IceTransport {
  IceTransportProtocol protocol;
  UdpSocket* udp_socket;
  TcpSocket tcp_socket;
  int tcp_connected;
  h2_pal_net_addr_t source_addr;
  h2_pal_net_addr_t remote_addr;
  uint8_t pending_frame[CONFIG_MTU + 2u];
  size_t pending_len;
  size_t pending_offset;
  uint8_t receive_prefix[2];
  size_t receive_prefix_len;
  uint8_t receive_packet[CONFIG_MTU];
  size_t receive_len;
  size_t receive_expected;
} IceTransport;

void ice_transport_init_udp(
    IceTransport* transport, UdpSocket* socket,
    const h2_pal_net_addr_t* remote_addr);
int ice_transport_init_tcp(
    IceTransport* transport, const h2_pal_net_api_t* net,
    const h2_pal_net_addr_t* source_addr,
    const h2_pal_net_addr_t* remote_addr, uint32_t timeout_ms);
int ice_transport_progress_tcp_connect(
    IceTransport* transport, uint32_t timeout_ms);
int ice_transport_send_packet(
    IceTransport* transport, const uint8_t* packet, size_t packet_len,
    uint32_t timeout_ms);
int ice_transport_flush(IceTransport* transport, uint32_t timeout_ms);
int ice_transport_receive_packet(
    IceTransport* transport, h2_pal_net_addr_t* out_addr,
    uint8_t* out_packet, size_t out_cap, uint32_t timeout_ms);
void ice_transport_close(IceTransport* transport);

#endif  // ICE_TRANSPORT_H_
